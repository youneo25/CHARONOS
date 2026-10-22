#include "VideoDecoder.h"
#include "Lang.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QDebug>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {

// Decoding forward this many frames is always cheaper than a seek + flush.
constexpr int64_t kForwardDecodeLimit = 96;

// Every open NVDEC decoder keeps its own surface pool in VRAM (about 20 surfaces of the
// frame size). Projects with many UHD sources would exhaust a consumer GPU, so only this
// many files decode on the GPU at once; the rest use the CPU.
constexpr int kMaxHardwareSessions = 8;

std::atomic<bool> g_hwEnabled{ true };
std::atomic<int> g_hwSessions{ 0 };
std::atomic<int> g_cudaState{ 0 };  // 0 = not tried, 1 = device ready, -1 = unavailable
std::mutex g_cudaMutex;
AVBufferRef* g_cudaDevice = nullptr;  // shared by every decoder for the life of the process

AVBufferRef* cudaDevice()
{
    std::lock_guard lock(g_cudaMutex);
    if (g_cudaState == 0) {
        const int r = av_hwdevice_ctx_create(&g_cudaDevice, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        g_cudaState = r >= 0 && g_cudaDevice ? 1 : -1;
        if (g_cudaState < 0) {
            g_cudaDevice = nullptr;
            qInfo().noquote() << "NVDEC: no CUDA device, decoding on the CPU";
        }
    }
    return g_cudaDevice ? av_buffer_ref(g_cudaDevice) : nullptr;
}

bool codecSupportsCuda(const AVCodec* codec)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) return false;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == AV_HWDEVICE_TYPE_CUDA)
            return true;
    }
}

bool reserveHardwareSession()
{
    int n = g_hwSessions.load();
    while (n < kMaxHardwareSessions)
        if (g_hwSessions.compare_exchange_weak(n, n + 1)) return true;
    return false;
}

// Prefer NVDEC surfaces; when libavcodec reports that the hwaccel can't handle this stream
// (unsupported profile, chroma format or size) it calls again without CUDA and we take the
// first software format.
AVPixelFormat pickFormat(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_CUDA) return *p;
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

QString avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

int swsColorspace(const AVFrame* f)
{
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG: return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    default: return f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

bool isFullRange(const AVFrame* f)
{
    if (f->color_range == AVCOL_RANGE_JPEG) return true;
    switch (f->format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P: return true;
    default: return false;
    }
}

} // namespace

void VideoDecoder::setHardwareDecodingEnabled(bool on) { g_hwEnabled = on; }
bool VideoDecoder::hardwareDecodingEnabled() { return g_hwEnabled; }
bool VideoDecoder::hardwareAvailable() { return g_cudaState > 0; }
int VideoDecoder::activeHardwareSessions() { return g_hwSessions; }

VideoDecoder::VideoDecoder()
    : m_frame(av_frame_alloc())
    , m_swFrame(av_frame_alloc())
    , m_pkt(av_packet_alloc())
{
}

VideoDecoder::~VideoDecoder()
{
    close();
    av_frame_free(&m_frame);
    av_frame_free(&m_swFrame);
    av_packet_free(&m_pkt);
}

void VideoDecoder::close()
{
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_swsYuv) {
        sws_freeContext(m_swsYuv);
        m_swsYuv = nullptr;
    }
    if (m_frame) av_frame_unref(m_frame);
    if (m_swFrame) av_frame_unref(m_swFrame);
    avcodec_free_context(&m_dec);
    avformat_close_input(&m_fmt);
    if (m_hwCounted) --g_hwSessions;
    m_hwCounted = false;
    m_hwActive = false;
    m_codec = nullptr;
    m_stream = -1;
    m_pts.clear();
    m_keyPts.clear();
    m_lastIndex = -1;
    m_frameValid = false;
    m_drained = false;
    m_transferred = false;
}

bool VideoDecoder::openCodec(bool hardware, QString* error)
{
    avcodec_free_context(&m_dec);
    m_hwActive = false;
    m_frameValid = false;
    m_lastIndex = -1;
    m_drained = false;
    m_transferred = false;
    if (!m_codec || !m_fmt || m_stream < 0) return false;

    m_dec = avcodec_alloc_context3(m_codec);
    if (!m_dec) {
        if (error) *error = L("Out of memory.", "메모리가 부족합니다.");
        return false;
    }
    AVStream* st = m_fmt->streams[m_stream];
    avcodec_parameters_to_context(m_dec, st->codecpar);
    m_dec->pkt_timebase = st->time_base;
    if (hardware) {
        AVBufferRef* device = cudaDevice();
        if (!device) {
            avcodec_free_context(&m_dec);
            return false;
        }
        m_dec->hw_device_ctx = device;  // the context owns this reference
        m_dec->get_format = pickFormat;
        m_dec->thread_count = 1;        // NVDEC is asynchronous already
    } else {
        m_dec->thread_count = std::clamp(QThread::idealThreadCount(), 1, 16);
        m_dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    const int r = avcodec_open2(m_dec, m_codec, nullptr);
    if (r < 0) {
        if (error) *error = L("Can't open decoder: %1", "디코더를 열 수 없습니다: %1").arg(avError(r));
        avcodec_free_context(&m_dec);
        return false;
    }
    m_hwActive = hardware;
    return true;
}

bool VideoDecoder::probeHardware()
{
    // libavcodec quietly continues in software when NVDEC rejects the stream, and then
    // runs single-threaded; decode the first frame to see which one we really got.
    if (!m_hwActive || m_keyPts.empty()) return false;
    seekToKey(0);
    for (int i = 0; i < 64; ++i) {
        const int64_t r = receiveNext();
        if (r == -2 || r == -1) return false;
        if (m_frameValid) return m_frame->format == AV_PIX_FMT_CUDA;
    }
    return false;
}

void VideoDecoder::fallbackToSoftware()
{
    if (m_hwCounted) --g_hwSessions;
    m_hwCounted = false;
    QString ignored;
    if (!openCodec(false, &ignored)) qWarning().noquote() << "Decoder: CPU fallback failed:" << ignored;
    if (m_dec && !m_keyPts.empty()) seekToKey(0);
    updateDecoderName();
    qInfo().noquote() << "Decoder:" << QFileInfo(m_info.path).fileName() << "->" << m_info.decoderName;
}

void VideoDecoder::updateDecoderName()
{
    if (!m_dec) m_info.decoderName = L("Unavailable", "사용 불가");
    else if (m_hwActive) m_info.decoderName = QStringLiteral("NVDEC (CUDA)");
    else m_info.decoderName = L("CPU · %1 threads", "CPU · %1 스레드").arg(std::max(1, m_dec->thread_count));
}

bool VideoDecoder::open(const QString& path, QString* error)
{
    close();
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        close();
        return false;
    };

    const QByteArray utf8 = path.toUtf8();
    int r = avformat_open_input(&m_fmt, utf8.constData(), nullptr, nullptr);
    if (r < 0) return fail(L("Can't open file: %1", "파일을 열 수 없습니다: %1").arg(avError(r)));

    r = avformat_find_stream_info(m_fmt, nullptr);
    if (r < 0) return fail(L("Can't read stream info: %1", "스트림 정보를 읽을 수 없습니다: %1").arg(avError(r)));

    const AVCodec* codec = nullptr;
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (m_stream < 0 || !codec) return fail(L("No video stream, or the codec isn't supported.", "비디오 스트림이 없거나 지원하지 않는 코덱입니다."));
    m_codec = codec;

    AVStream* st = m_fmt->streams[m_stream];
    if (st->codecpar->width <= 0 || st->codecpar->height <= 0)
        return fail(L("The video stream has no picture size.", "비디오 스트림에 화면 크기 정보가 없습니다."));
    bool hasAudio = false;
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        if (m_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) hasAudio = true;
        if (static_cast<int>(i) != m_stream) m_fmt->streams[i]->discard = AVDISCARD_ALL;
    }

    m_info = {};
    m_info.path = path;
    m_info.hasAudio = hasAudio;

    const bool tryHardware = g_hwEnabled && codecSupportsCuda(codec) && reserveHardwareSession();
    m_hwCounted = tryHardware;
    QString codecError;
    if (!openCodec(tryHardware, &codecError)) {
        if (m_hwCounted) --g_hwSessions;
        m_hwCounted = false;
        if (!tryHardware || !openCodec(false, &codecError)) return fail(codecError);
    }

    if (!buildIndex(error)) {
        close();
        return false;
    }
    if (m_hwActive && !probeHardware()) fallbackToSoftware();
    m_stats = {};

    const AVRational avg = st->avg_frame_rate;
    const AVRational base = st->r_frame_rate;
    double fps = (avg.num && avg.den) ? av_q2d(avg) : 0.0;
    if (fps <= 0.0 && base.num && base.den) fps = av_q2d(base);
    if (fps <= 0.0 || fps > 1000.0 || !std::isfinite(fps)) fps = 24.0;

    m_info.width = st->codecpar->width;
    m_info.height = st->codecpar->height;
    m_info.fps = fps;
    m_info.frameCount = static_cast<int64_t>(m_pts.size());
    m_info.duration = m_fmt->duration > 0 ? m_fmt->duration / double(AV_TIME_BASE) : m_pts.size() / fps;
    m_info.codec = QString::fromUtf8(codec->long_name ? codec->long_name : codec->name);
    const char* pix = av_get_pix_fmt_name(static_cast<AVPixelFormat>(st->codecpar->format));
    m_info.pixelFormat = QString::fromUtf8(pix ? pix : "?");
    m_info.variableFrameRate = (avg.num && base.num) && std::abs(av_q2d(avg) - av_q2d(base)) > 0.01;
    m_info.frameTimes.reserve(m_pts.size());
    for (int64_t pts : m_pts) m_info.frameTimes.push_back(pts * av_q2d(st->time_base));
    updateDecoderName();
    qInfo().noquote() << "Decoder:" << QFileInfo(path).fileName() << "->" << m_info.decoderName;
    return true;
}

bool VideoDecoder::buildIndex(QString* error)
{
    m_pts.clear();
    m_keyPts.clear();
    while (av_read_frame(m_fmt, m_pkt) >= 0) {
        if (m_pkt->stream_index == m_stream && !(m_pkt->flags & AV_PKT_FLAG_DISCARD)) {
            const int64_t ts = m_pkt->pts != AV_NOPTS_VALUE ? m_pkt->pts : m_pkt->dts;
            if (ts != AV_NOPTS_VALUE) {
                m_pts.push_back(ts);
                if (m_pkt->flags & AV_PKT_FLAG_KEY) m_keyPts.push_back(ts);
            }
        }
        av_packet_unref(m_pkt);
    }
    std::sort(m_pts.begin(), m_pts.end());
    m_pts.erase(std::unique(m_pts.begin(), m_pts.end()), m_pts.end());
    std::sort(m_keyPts.begin(), m_keyPts.end());
    m_keyPts.erase(std::unique(m_keyPts.begin(), m_keyPts.end()), m_keyPts.end());

    if (m_pts.empty()) {
        if (error) *error = L("No frames found.", "프레임을 찾을 수 없습니다.");
        return false;
    }
    if (m_keyPts.empty() || m_keyPts.front() > m_pts.front()) m_keyPts.insert(m_keyPts.begin(), m_pts.front());

    seekToKey(0);
    m_stats = {};
    return true;
}

void VideoDecoder::seekToKey(size_t keyIdx)
{
    if (!m_dec || m_keyPts.empty()) return;
    const int64_t ts = m_keyPts[std::min(keyIdx, m_keyPts.size() - 1)];
    if (av_seek_frame(m_fmt, m_stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        avformat_seek_file(m_fmt, m_stream, INT64_MIN, ts, ts, 0);
    avcodec_flush_buffers(m_dec);
    m_lastIndex = -1;
    m_frameValid = false;
    m_drained = false;
    m_transferred = false;
    ++m_stats.seeks;
}

int64_t VideoDecoder::indexForPts(int64_t pts) const
{
    auto it = std::lower_bound(m_pts.begin(), m_pts.end(), pts);
    if (it == m_pts.end()) return static_cast<int64_t>(m_pts.size()) - 1;
    if (it != m_pts.begin() && (pts - *(it - 1)) < (*it - pts)) --it;
    return it - m_pts.begin();
}

int64_t VideoDecoder::receiveNext()
{
    m_frameValid = false;
    m_transferred = false;
    if (!m_dec) return -2;
    for (;;) {
        int r = avcodec_receive_frame(m_dec, m_frame);
        if (r == 0) {
            int64_t ts = m_frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = m_frame->pts;
            m_lastIndex = (ts == AV_NOPTS_VALUE) ? m_lastIndex + 1 : indexForPts(ts);
            m_frameValid = true;
            ++m_stats.framesDecoded;
            return m_lastIndex;
        }
        if (r == AVERROR_EOF) return -1;
        if (r != AVERROR(EAGAIN)) return -2;
        if (m_drained) return -1;

        r = av_read_frame(m_fmt, m_pkt);
        if (r < 0) {
            avcodec_send_packet(m_dec, nullptr);  // flush delayed frames
            m_drained = true;
            continue;
        }
        if (m_pkt->stream_index == m_stream) avcodec_send_packet(m_dec, m_pkt);
        av_packet_unref(m_pkt);
    }
}

bool VideoDecoder::position(int64_t index, QString* error, const FrameSink* sink)
{
    if (!m_fmt || index < 0 || index >= static_cast<int64_t>(m_pts.size())) {
        if (error) *error = L("Frame out of range.", "프레임 범위를 벗어났습니다.");
        return false;
    }
    if (m_frameValid && m_lastIndex == index) return true;

    for (int pass = 0; pass < 2; ++pass) {
        if (!m_dec) break;
        const int64_t target = m_pts[index];
        const auto kit = std::upper_bound(m_keyPts.begin(), m_keyPts.end(), target);
        size_t keyIdx = (kit == m_keyPts.begin()) ? 0 : size_t(kit - m_keyPts.begin() - 1);

        // Keep decoding forward when the target is ahead and either in the same GOP or close by.
        // (After the demuxer hits EOF the decoder still holds delayed frames, so draining is not a reason to seek.)
        const bool ahead = m_lastIndex >= 0 && index > m_lastIndex;
        bool needSeek = !(ahead && (m_keyPts[keyIdx] <= m_pts[m_lastIndex] || index - m_lastIndex <= kForwardDecodeLimit));
        bool decodeError = false;

        for (int attempt = 0; attempt < 6; ++attempt) {
            if (needSeek) seekToKey(keyIdx);

            bool overshoot = false;
            while (!(m_frameValid && m_lastIndex == index)) {
                if (m_frameValid && m_lastIndex > index) {
                    overshoot = true;
                    break;
                }
                const int64_t got = receiveNext();
                if (got == -2) decodeError = true;
                if (got < 0) break;
                if (sink && m_frameValid && m_lastIndex < index && sink->wants(m_lastIndex)) {
                    VideoFrame side;
                    const int64_t at = m_lastIndex;
                    if (toFrame(side)) sink->deliver(at, std::move(side));
                }
            }
            if (m_frameValid && m_lastIndex == index) return true;

            // The demuxer landed after the target (open GOP / inexact seek) — back off one more keyframe.
            if (!overshoot) break;
            if (keyIdx == 0 && attempt > 0) break;
            if (keyIdx > 0) --keyIdx;
            needSeek = true;
        }

        // The GPU failed on this file: decode it again on the CPU before giving up.
        if (m_hwActive && (decodeError || !m_frameValid)) {
            qWarning().noquote() << "NVDEC error at frame" << index << "- switching to CPU decoding";
            fallbackToSoftware();
            continue;
        }
        // Decoder never produced this exact frame (corrupt or dropped). Show the nearest one instead of nothing.
        if (m_frameValid) return true;
        break;
    }
    if (error) *error = L("Can't decode frame %1.", "프레임 %1을(를) 디코딩할 수 없습니다.").arg(index);
    return false;
}

const AVFrame* VideoDecoder::picture()
{
    if (!m_frameValid) return nullptr;
    if (m_frame->format != AV_PIX_FMT_CUDA) return m_frame;
    if (!m_transferred) {
        av_frame_unref(m_swFrame);
        const int r = av_hwframe_transfer_data(m_swFrame, m_frame, 0);
        if (r < 0) {
            qWarning().noquote() << "NVDEC: frame download failed:" << avError(r);
            return nullptr;
        }
        av_frame_copy_props(m_swFrame, m_frame);
        m_transferred = true;
    }
    return m_swFrame;
}

bool VideoDecoder::toImage(QImage& out)
{
    const AVFrame* f = picture();
    if (!f) return false;
    const int w = f->width;
    const int h = f->height;
    m_sws = sws_getCachedContext(m_sws, w, h, static_cast<AVPixelFormat>(f->format), w, h, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) return false;

    const int* coeffs = sws_getCoefficients(swsColorspace(f));
    sws_setColorspaceDetails(m_sws, coeffs, isFullRange(f) ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                             1 << 16, 1 << 16);

    if (out.width() != w || out.height() != h || out.format() != QImage::Format_RGBA8888)
        out = QImage(w, h, QImage::Format_RGBA8888);
    if (out.isNull()) return false;

    uint8_t* dst[4] = { out.bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(out.bytesPerLine()), 0, 0, 0 };
    sws_scale(m_sws, f->data, f->linesize, 0, h, dst, dstStride);
    return true;
}

bool VideoDecoder::downloadInto(VideoFrame& out)
{
    // NVDEC surface -> cache buffer in one DMA copy: no staging frame, no repacking. The
    // viewer reads NV12 directly (luma + interleaved chroma textures).
    const int w = m_frame->width, h = m_frame->height;
    out.allocatePlanes(VideoFrame::Format::NV12, w, h);
    out.setColor(VideoFrame::matrixOf(m_frame), VideoFrame::fullRangeOf(m_frame));
    AVFrame* dst = av_frame_alloc();
    if (!dst) return false;
    dst->buf[0] = av_buffer_create(out.planeBits(0), size_t(out.sizeInBytes()), [](void*, uint8_t*) {}, nullptr, 0);
    if (!dst->buf[0]) {
        av_frame_free(&dst);
        return false;
    }
    dst->format = AV_PIX_FMT_NV12;
    dst->width = w;
    dst->height = h;
    for (int i = 0; i < 2; ++i) {
        dst->data[i] = out.planeBits(i);
        dst->linesize[i] = out.planeBytesPerLine(i);
    }
    const int r = av_hwframe_transfer_data(dst, m_frame, 0);
    av_frame_free(&dst);  // the buffer wrapper only; the pixels belong to `out`
    if (r < 0) {
        qWarning().noquote() << "NVDEC: frame download failed:" << avError(r);
        out = VideoFrame();
        return false;
    }
    // CUDA copies floor(h / 2) chroma rows; an odd height leaves the last one to fill.
    const int ch = out.planeHeight(1), bpl = out.planeBytesPerLine(1);
    if ((h & 1) && ch >= 2) std::memcpy(out.planeBits(1) + qint64(ch - 1) * bpl, out.planeBits(1) + qint64(ch - 2) * bpl, size_t(bpl));
    return true;
}

bool VideoDecoder::toFrame(VideoFrame& out)
{
    if (m_frameValid && m_frame->format == AV_PIX_FMT_CUDA && !m_transferred && m_frame->hw_frames_ctx) {
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(m_frame->hw_frames_ctx->data);
        if (frames->sw_format == AV_PIX_FMT_NV12 && downloadInto(out)) return true;
    }
    const AVFrame* f = picture();
    if (!f) return false;
    if (VideoFrame::fromAVFrame(f, out, &m_swsYuv)) return true;
    // RGB, alpha or palette sources stay RGBA.
    QImage image;
    if (!toImage(image)) return false;
    out = VideoFrame::fromImage(std::move(image));
    return !out.isNull();
}

bool VideoDecoder::decode(int64_t index, QImage& out, QString* error)
{
    if (!position(index, error, nullptr)) return false;
    if (toImage(out)) return true;
    if (error) *error = L("Can't convert frame %1.", "프레임 %1을(를) 변환할 수 없습니다.").arg(index);
    return false;
}

bool VideoDecoder::decodeFrame(int64_t index, VideoFrame& out, QString* error, const FrameSink* sink)
{
    if (!position(index, error, sink)) return false;
    if (toFrame(out)) return true;
    if (error) *error = L("Can't convert frame %1.", "프레임 %1을(를) 변환할 수 없습니다.").arg(index);
    return false;
}
