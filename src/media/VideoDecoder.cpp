#include "VideoDecoder.h"
#include "Lang.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QThread>

#include <algorithm>
#include <cmath>

namespace {

// Decoding forward this many frames is always cheaper than a seek + flush.
constexpr int64_t kForwardDecodeLimit = 96;

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

VideoDecoder::VideoDecoder()
    : m_frame(av_frame_alloc())
    , m_pkt(av_packet_alloc())
{
}

VideoDecoder::~VideoDecoder()
{
    close();
    av_frame_free(&m_frame);
    av_packet_free(&m_pkt);
}

void VideoDecoder::close()
{
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    avcodec_free_context(&m_dec);
    avformat_close_input(&m_fmt);
    m_stream = -1;
    m_pts.clear();
    m_keyPts.clear();
    m_lastIndex = -1;
    m_frameValid = false;
    m_drained = false;
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

    AVStream* st = m_fmt->streams[m_stream];
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
        if (static_cast<int>(i) != m_stream) m_fmt->streams[i]->discard = AVDISCARD_ALL;

    m_dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_dec, st->codecpar);
    m_dec->pkt_timebase = st->time_base;
    m_dec->thread_count = std::clamp(QThread::idealThreadCount(), 1, 16);
    m_dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    r = avcodec_open2(m_dec, codec, nullptr);
    if (r < 0) return fail(L("Can't open decoder: %1", "디코더를 열 수 없습니다: %1").arg(avError(r)));

    if (!buildIndex(error)) {
        close();
        return false;
    }

    const AVRational avg = st->avg_frame_rate;
    const AVRational base = st->r_frame_rate;
    double fps = (avg.num && avg.den) ? av_q2d(avg) : 0.0;
    if (fps <= 0.0 && base.num && base.den) fps = av_q2d(base);
    if (fps <= 0.0 || fps > 1000.0) fps = 24.0;

    m_info = {};
    m_info.path = path;
    m_info.width = st->codecpar->width;
    m_info.height = st->codecpar->height;
    m_info.fps = fps;
    m_info.frameCount = static_cast<int64_t>(m_pts.size());
    m_info.duration = m_fmt->duration > 0 ? m_fmt->duration / double(AV_TIME_BASE) : m_pts.size() / fps;
    m_info.codec = QString::fromUtf8(codec->long_name ? codec->long_name : codec->name);
    const char* pix = av_get_pix_fmt_name(m_dec->pix_fmt);
    m_info.pixelFormat = QString::fromUtf8(pix ? pix : "?");
    m_info.variableFrameRate = (avg.num && base.num) && std::abs(av_q2d(avg) - av_q2d(base)) > 0.01;
    m_info.frameTimes.reserve(m_pts.size());
    for (int64_t pts : m_pts) m_info.frameTimes.push_back(pts * av_q2d(st->time_base));
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
    const int64_t ts = m_keyPts[std::min(keyIdx, m_keyPts.size() - 1)];
    if (av_seek_frame(m_fmt, m_stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        avformat_seek_file(m_fmt, m_stream, INT64_MIN, ts, ts, 0);
    avcodec_flush_buffers(m_dec);
    m_lastIndex = -1;
    m_frameValid = false;
    m_drained = false;
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

bool VideoDecoder::decode(int64_t index, QImage& out, QString* error, const FrameSink* sink)
{
    if (!m_fmt || index < 0 || index >= static_cast<int64_t>(m_pts.size())) {
        if (error) *error = L("Frame out of range.", "프레임 범위를 벗어났습니다.");
        return false;
    }
    if (m_frameValid && m_lastIndex == index) return convert(out);

    const int64_t target = m_pts[index];
    const auto kit = std::upper_bound(m_keyPts.begin(), m_keyPts.end(), target);
    size_t keyIdx = (kit == m_keyPts.begin()) ? 0 : size_t(kit - m_keyPts.begin() - 1);

    // Keep decoding forward when the target is ahead and either in the same GOP or close by.
    // (After the demuxer hits EOF the decoder still holds delayed frames, so draining is not a reason to seek.)
    const bool ahead = m_lastIndex >= 0 && index > m_lastIndex;
    bool needSeek = !(ahead && (m_keyPts[keyIdx] <= m_pts[m_lastIndex] || index - m_lastIndex <= kForwardDecodeLimit));

    for (int attempt = 0; attempt < 6; ++attempt) {
        if (needSeek) seekToKey(keyIdx);

        bool overshoot = false;
        while (!(m_frameValid && m_lastIndex == index)) {
            if (m_frameValid && m_lastIndex > index) {
                overshoot = true;
                break;
            }
            if (receiveNext() < 0) break;
            if (sink && m_frameValid && m_lastIndex != index && m_lastIndex < index && sink->wants(m_lastIndex)) {
                QImage side;
                if (convert(side)) sink->deliver(m_lastIndex, std::move(side));
            }
        }
        if (m_frameValid && m_lastIndex == index) return convert(out);

        // The demuxer landed after the target (open GOP / inexact seek) — back off one more keyframe.
        if (!overshoot) break;
        if (keyIdx == 0 && attempt > 0) break;
        if (keyIdx > 0) --keyIdx;
        needSeek = true;
    }

    // Decoder never produced this exact frame (corrupt or dropped). Show the nearest one instead of nothing.
    if (m_frameValid) return convert(out);
    if (error) *error = L("Can't decode frame %1.", "프레임 %1을(를) 디코딩할 수 없습니다.").arg(index);
    return false;
}

bool VideoDecoder::convert(QImage& out)
{
    const int w = m_frame->width;
    const int h = m_frame->height;
    m_sws = sws_getCachedContext(m_sws, w, h, static_cast<AVPixelFormat>(m_frame->format), w, h, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) return false;

    const int* coeffs = sws_getCoefficients(swsColorspace(m_frame));
    sws_setColorspaceDetails(m_sws, coeffs, isFullRange(m_frame) ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                             1 << 16, 1 << 16);

    if (out.width() != w || out.height() != h || out.format() != QImage::Format_RGBA8888)
        out = QImage(w, h, QImage::Format_RGBA8888);

    uint8_t* dst[4] = { out.bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(out.bytesPerLine()), 0, 0, 0 };
    sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, h, dst, dstStride);
    return true;
}
