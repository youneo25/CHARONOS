#include "VideoEncoder.h"
#include "Lang.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

QString avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

AVRational rateFor(double fps)
{
    const AVRational ntsc[] = { { 24000, 1001 }, { 30000, 1001 }, { 48000, 1001 }, { 60000, 1001 } };
    for (const AVRational& r : ntsc)
        if (std::abs(fps - av_q2d(r)) < 0.005) return r;
    if (std::abs(fps - std::round(fps)) < 0.001) return { int(std::round(fps)), 1 };
    return av_d2q(fps, 1000000);
}

} // namespace

VideoEncoder::~VideoEncoder() { cleanup(); }

void VideoEncoder::cleanup()
{
    if (m_sws) sws_freeContext(m_sws);
    m_sws = nullptr;
    av_frame_free(&m_frame);
    av_frame_free(&m_aframe);
    av_packet_free(&m_pkt);
    avcodec_free_context(&m_enc);
    avcodec_free_context(&m_aenc);
    m_apending.clear();
    m_apts = 0;
    if (m_oc) {
        if (m_oc->pb) avio_closep(&m_oc->pb);
        avformat_free_context(m_oc);
        m_oc = nullptr;
    }
}

bool VideoEncoder::open(const QString& path, Codec codec, QSize size, double fps, bool withAudio, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        cleanup();
        return false;
    };

    const char* name = codec == Codec::H264        ? "libx264"
                       : codec == Codec::H264Nvenc ? "h264_nvenc"
                       : codec == Codec::HevcNvenc ? "hevc_nvenc"
                                                   : "prores_ks";
    const AVPixelFormat pix = codec == Codec::ProRes422HQ ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV420P;
    size = alignedSize(size);

    const QByteArray utf8 = path.toUtf8();
    int r = avformat_alloc_output_context2(&m_oc, nullptr, nullptr, utf8.constData());
    if (r < 0 || !m_oc) return fail(L("Can't create output format: %1", "출력 형식을 만들 수 없습니다: %1").arg(avError(r)));

    const AVCodec* enc = avcodec_find_encoder_by_name(name);
    if (!enc) return fail(L("Encoder not found: %1", "인코더를 찾을 수 없습니다: %1").arg(QLatin1String(name)));

    m_st = avformat_new_stream(m_oc, nullptr);
    m_enc = avcodec_alloc_context3(enc);
    const AVRational rate = rateFor(fps);
    m_enc->width = size.width();
    m_enc->height = size.height();
    m_enc->time_base = av_inv_q(rate);
    m_enc->framerate = rate;
    m_enc->pix_fmt = pix;
    m_enc->gop_size = std::max(1, int(std::round(fps)));  // 1 s GOP: scrubs well in review tools
    m_enc->max_b_frames = codec == Codec::H264 ? 2 : 0;
    m_enc->thread_count = std::clamp(QThread::idealThreadCount(), 1, 16);  // prores_ks defaults to one thread
    m_enc->colorspace = AVCOL_SPC_BT709;
    m_enc->color_primaries = AVCOL_PRI_BT709;
    m_enc->color_trc = AVCOL_TRC_BT709;
    m_enc->color_range = AVCOL_RANGE_MPEG;
    if (m_oc->oformat->flags & AVFMT_GLOBALHEADER) m_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* opts = nullptr;
    switch (codec) {
    case Codec::H264:
        av_dict_set(&opts, "preset", "medium", 0);
        av_dict_set(&opts, "crf", "16", 0);
        break;
    case Codec::H264Nvenc:
    case Codec::HevcNvenc:
        av_dict_set(&opts, "preset", "p5", 0);
        av_dict_set(&opts, "rc", "vbr", 0);
        av_dict_set(&opts, "cq", "18", 0);
        av_dict_set(&opts, "b", "0", 0);
        break;
    case Codec::ProRes422HQ:
        av_dict_set(&opts, "profile", "3", 0);
        av_dict_set(&opts, "vendor", "apl0", 0);
        break;
    }
    r = avcodec_open2(m_enc, enc, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        const QString hint = (codec == Codec::H264Nvenc || codec == Codec::HevcNvenc)
                                 ? L("\nCheck that your NVIDIA driver supports NVENC.", "\nNVIDIA 드라이버가 NVENC를 지원하는지 확인하세요.")
                                 : QString();
        return fail(L("Can't open encoder: %1%2", "인코더를 열 수 없습니다: %1%2").arg(avError(r), hint));
    }
    avcodec_parameters_from_context(m_st->codecpar, m_enc);
    m_st->time_base = m_enc->time_base;
    m_st->avg_frame_rate = rate;

    if (withAudio) {
        const bool pcm = codec == Codec::ProRes422HQ;  // MOV + ProRes: uncompressed audio is the norm
        const AVCodec* aenc = avcodec_find_encoder(pcm ? AV_CODEC_ID_PCM_S16LE : AV_CODEC_ID_AAC);
        if (!aenc) return fail(L("Audio encoder not found.", "오디오 인코더를 찾을 수 없습니다."));
        m_ast = avformat_new_stream(m_oc, nullptr);
        m_aenc = avcodec_alloc_context3(aenc);
        m_aenc->sample_rate = 48000;
        m_aenc->sample_fmt = pcm ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLTP;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_copy(&m_aenc->ch_layout, &stereo);
        m_aenc->time_base = { 1, 48000 };
        if (!pcm) m_aenc->bit_rate = 256000;
        if (m_oc->oformat->flags & AVFMT_GLOBALHEADER) m_aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        r = avcodec_open2(m_aenc, aenc, nullptr);
        if (r < 0) return fail(L("Can't open audio encoder: %1", "오디오 인코더를 열 수 없습니다: %1").arg(avError(r)));
        avcodec_parameters_from_context(m_ast->codecpar, m_aenc);
        m_ast->time_base = m_aenc->time_base;
        m_aframe = av_frame_alloc();
        m_aframe->format = m_aenc->sample_fmt;
        av_channel_layout_copy(&m_aframe->ch_layout, &stereo);
        m_aframe->sample_rate = 48000;
        m_aframe->nb_samples = m_aenc->frame_size > 0 ? m_aenc->frame_size : 1024;
        av_frame_get_buffer(m_aframe, 0);
    }

    r = avio_open(&m_oc->pb, utf8.constData(), AVIO_FLAG_WRITE);
    if (r < 0) return fail(L("Can't write file: %1", "파일을 쓸 수 없습니다: %1").arg(avError(r)));
    AVDictionary* fmtOpts = nullptr;
    av_dict_set(&fmtOpts, "movflags", "+faststart", 0);
    r = avformat_write_header(m_oc, &fmtOpts);
    av_dict_free(&fmtOpts);
    if (r < 0) return fail(L("Can't write header: %1", "헤더를 쓸 수 없습니다: %1").arg(avError(r)));
    m_headerWritten = true;

    m_frame = av_frame_alloc();
    m_frame->format = pix;
    m_frame->width = size.width();
    m_frame->height = size.height();
    m_frame->colorspace = AVCOL_SPC_BT709;
    m_frame->color_range = AVCOL_RANGE_MPEG;
    av_frame_get_buffer(m_frame, 0);
    m_pkt = av_packet_alloc();

    m_sws = sws_getContext(size.width(), size.height(), AV_PIX_FMT_BGRA, size.width(), size.height(), pix,
                           SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    const int* coeffs = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(m_sws, coeffs, 1, coeffs, 0, 0, 1 << 16, 1 << 16);
    return true;
}

bool VideoEncoder::write(const QImage& image, QString* error)
{
    QImage img = image;
    if (img.format() != QImage::Format_RGB32 && img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_RGB32);
    if (img.size() != QSize(m_enc->width, m_enc->height))
        img = img.scaled(m_enc->width, m_enc->height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    av_frame_make_writable(m_frame);
    const uint8_t* src[4] = { img.constBits(), nullptr, nullptr, nullptr };
    const int srcStride[4] = { int(img.bytesPerLine()), 0, 0, 0 };
    sws_scale(m_sws, src, srcStride, 0, img.height(), m_frame->data, m_frame->linesize);
    m_frame->pts = m_pts++;

    const int r = avcodec_send_frame(m_enc, m_frame);
    if (r < 0) {
        if (error) *error = L("Encoding failed: %1", "인코딩 실패: %1").arg(avError(r));
        return false;
    }
    return drain(m_enc, m_st, error);
}

bool VideoEncoder::drain(AVCodecContext* enc, AVStream* st, QString* error)
{
    for (;;) {
        const int r = avcodec_receive_packet(enc, m_pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
        if (r < 0) {
            if (error) *error = L("Encoding failed: %1", "인코딩 실패: %1").arg(avError(r));
            return false;
        }
        av_packet_rescale_ts(m_pkt, enc->time_base, st->time_base);
        m_pkt->stream_index = st->index;
        const int w = av_interleaved_write_frame(m_oc, m_pkt);
        if (w < 0) {
            if (error) *error = L("Write failed: %1", "파일 쓰기 실패: %1").arg(avError(w));
            return false;
        }
    }
}

bool VideoEncoder::writeAudio(const int16_t* data, int frames, QString* error)
{
    if (!m_aenc) return true;
    m_apending.insert(m_apending.end(), data, data + size_t(frames) * 2);
    return encodeAudio(false, error);
}

bool VideoEncoder::encodeAudio(bool flush, QString* error)
{
    const int n = m_aframe->nb_samples;
    while (m_apending.size() >= size_t(n) * 2 || (flush && !m_apending.empty())) {
        if (m_apending.size() < size_t(n) * 2) m_apending.resize(size_t(n) * 2, 0);  // pad the last frame with silence
        av_frame_make_writable(m_aframe);
        const int16_t* s = m_apending.data();
        if (m_aenc->sample_fmt == AV_SAMPLE_FMT_FLTP) {
            auto* l = reinterpret_cast<float*>(m_aframe->data[0]);
            auto* r = reinterpret_cast<float*>(m_aframe->data[1]);
            for (int i = 0; i < n; ++i) {
                l[i] = s[i * 2] / 32768.0f;
                r[i] = s[i * 2 + 1] / 32768.0f;
            }
        } else {
            std::memcpy(m_aframe->data[0], s, size_t(n) * 4);
        }
        m_aframe->pts = m_apts;
        m_apts += n;
        m_apending.erase(m_apending.begin(), m_apending.begin() + size_t(n) * 2);
        const int r = avcodec_send_frame(m_aenc, m_aframe);
        if (r < 0) {
            if (error) *error = L("Audio encoding failed: %1", "오디오 인코딩 실패: %1").arg(avError(r));
            return false;
        }
        if (!drain(m_aenc, m_ast, error)) return false;
    }
    return true;
}

bool VideoEncoder::finish(QString* error)
{
    if (!m_enc) return false;
    avcodec_send_frame(m_enc, nullptr);
    bool ok = drain(m_enc, m_st, error);
    if (m_aenc) {
        ok = encodeAudio(true, error) && ok;
        avcodec_send_frame(m_aenc, nullptr);
        ok = drain(m_aenc, m_ast, error) && ok;
    }
    if (m_headerWritten) av_write_trailer(m_oc);
    cleanup();
    return ok;
}
