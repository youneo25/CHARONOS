#pragma once

#include <QImage>
#include <QString>

#include <cstdint>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Writes RGB32 QImages to a video file through libavcodec/libavformat.
class VideoEncoder {
public:
    enum class Codec { H264, H264Nvenc, HevcNvenc, ProRes422HQ };

    ~VideoEncoder();
    // withAudio adds a 48 kHz stereo track: AAC in MP4, 16-bit PCM in MOV (ProRes).
    bool open(const QString& path, Codec codec, QSize size, double fps, bool withAudio, QString* error);
    bool writeAudio(const int16_t* interleavedStereo, int frames, QString* error);
    bool write(const QImage& rgb32, QString* error);
    bool finish(QString* error);

    // Even dimensions required by 4:2:0 encoders.
    static QSize alignedSize(QSize s) { return QSize(std::max(2, s.width() & ~1), std::max(2, s.height() & ~1)); }

private:
    bool drain(AVCodecContext* enc, AVStream* st, QString* error);
    bool encodeAudio(bool flush, QString* error);
    void cleanup();

    AVFormatContext* m_oc = nullptr;
    AVCodecContext* m_enc = nullptr;
    AVStream* m_st = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_pkt = nullptr;
    SwsContext* m_sws = nullptr;
    int64_t m_pts = 0;
    AVCodecContext* m_aenc = nullptr;
    AVStream* m_ast = nullptr;
    AVFrame* m_aframe = nullptr;
    std::vector<int16_t> m_apending;  // interleaved stereo waiting for a full codec frame
    int64_t m_apts = 0;
    bool m_headerWritten = false;
};
