#include "AudioClip.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace {
QString avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}
} // namespace

std::shared_ptr<AudioClip> AudioClip::decode(const QString& path, QString* error, const std::atomic<bool>* cancel)
{
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto cleanup = [&] {
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        av_packet_free(&pkt);
        av_frame_free(&frame);
    };

    const QByteArray utf8 = path.toUtf8();
    int r = avformat_open_input(&fmt, utf8.constData(), nullptr, nullptr);
    if (r < 0 || avformat_find_stream_info(fmt, nullptr) < 0) {
        if (error) *error = avError(r);
        cleanup();
        return {};
    }
    const AVCodec* codec = nullptr;
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) {  // no audio track: not an error
        cleanup();
        return {};
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (int(i) != stream) fmt->streams[i]->discard = AVDISCARD_ALL;

    AVStream* st = fmt->streams[stream];
    dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, st->codecpar);
    dec->pkt_timebase = st->time_base;
    if ((r = avcodec_open2(dec, codec, nullptr)) < 0) {
        if (error) *error = avError(r);
        cleanup();
        return {};
    }

    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if ((r = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kRate, &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0,
                                 nullptr)) < 0 ||
        (r = swr_init(swr)) < 0) {
        if (error) *error = avError(r);
        cleanup();
        return {};
    }

    const double seconds = st->duration > 0                 ? av_q2d(st->time_base) * st->duration
                           : fmt->duration > 0              ? fmt->duration / double(AV_TIME_BASE)
                                                            : 0.0;
    if (seconds > kMaxSeconds) {
        if (error) *error = QStringLiteral("Soundtrack longer than %1 hours is not loaded").arg(kMaxSeconds / 3600.0);
        cleanup();
        return {};
    }
    auto clip = std::make_shared<AudioClip>();
    if (dec->sample_rate > 0 && seconds > 0) clip->pcm.reserve(size_t(seconds * kRate * 2) + 4096);

    // Where the first sample sits on the container clock (shared with the video's frame times).
    // Taken from the first decoded frame, i.e. after the decoder dropped AAC/MP3 priming samples.
    bool haveStart = false;
    if (st->start_time != AV_NOPTS_VALUE) clip->startTime = st->start_time * av_q2d(st->time_base);

    auto convert = [&](const AVFrame* f) {
        if (f && !haveStart) {
            const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            if (ts != AV_NOPTS_VALUE) clip->startTime = ts * av_q2d(st->time_base);
            haveStart = true;
        }
        const int maxOut = swr_get_out_samples(swr, f ? f->nb_samples : 0);
        if (maxOut <= 0) return;
        const size_t at = clip->pcm.size();
        clip->pcm.resize(at + size_t(maxOut) * 2);
        uint8_t* out = reinterpret_cast<uint8_t*>(clip->pcm.data() + at);
        const int got = swr_convert(swr, &out, maxOut, f ? const_cast<const uint8_t**>(f->extended_data) : nullptr,
                                    f ? f->nb_samples : 0);
        clip->pcm.resize(at + size_t(std::max(0, got)) * 2);
    };

    while (av_read_frame(fmt, pkt) >= 0) {
        if (cancel && cancel->load()) {
            av_packet_unref(pkt);
            cleanup();
            return {};
        }
        if (pkt->stream_index == stream && avcodec_send_packet(dec, pkt) >= 0)
            while (avcodec_receive_frame(dec, frame) >= 0) convert(frame);
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dec, nullptr);
    while (avcodec_receive_frame(dec, frame) >= 0) convert(frame);
    convert(nullptr);  // flush the resampler
    cleanup();

    if (clip->pcm.empty()) return {};
    clip->pcm.shrink_to_fit();
    return clip;
}
