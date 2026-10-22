#pragma once

#include "FrameSource.h"

#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Frame-exact video decoder built on libavformat/libavcodec.
//
// On open, every video packet is demuxed once (no decoding) to build a table of
// presentation timestamps in display order plus the keyframe positions. Frame N is
// the N-th smallest PTS, so lookups never depend on fps arithmetic and VFR files and
// B-frame reordering are handled exactly. Random access seeks to the nearest preceding
// keyframe and decodes forward; nearby forward access just keeps decoding.
//
// Decoding uses NVDEC (FFmpeg's CUDA hwaccel) when an NVIDIA GPU supports the codec and
// profile, and falls back to multi-threaded CPU decoding otherwise, including when the GPU
// fails in the middle of a file. Frames destined for the cache keep their planar Y'CbCr
// layout; RGB conversion happens on the GPU in the viewer.
class VideoDecoder final : public FrameSource {
public:
    struct Stats {
        int64_t framesDecoded = 0;
        int64_t seeks = 0;
    };

    VideoDecoder();
    ~VideoDecoder() override;

    bool open(const QString& path, QString* error);
    const MediaInfo& info() const override { return m_info; }
    bool decode(int64_t index, QImage& out, QString* error) override;
    bool decodeFrame(int64_t index, VideoFrame& out, QString* error, const FrameSink* sink = nullptr) override;
    const Stats& stats() const { return m_stats; }
    bool hardwareActive() const { return m_hwActive; }

    // Global switch read when a file is opened (Tools > Hardware Decoding). Default on.
    static void setHardwareDecodingEnabled(bool on);
    static bool hardwareDecodingEnabled();
    // True once a CUDA device could be created on this machine (false before the first try).
    static bool hardwareAvailable();
    static int activeHardwareSessions();

private:
    void close();
    bool openCodec(bool hardware, QString* error);
    bool probeHardware();
    void fallbackToSoftware();
    bool buildIndex(QString* error);
    void seekToKey(size_t keyIdx);
    int64_t receiveNext();  // index of the newly decoded frame in m_frame, -1 at EOF, -2 on error
    int64_t indexForPts(int64_t pts) const;
    // Leaves m_frame on frame `index` (or the nearest one the stream really has).
    bool position(int64_t index, QString* error, const FrameSink* sink);
    const AVFrame* picture();  // m_frame in system memory (downloads NVDEC surfaces on demand)
    bool toImage(QImage& out);
    bool toFrame(VideoFrame& out);
    bool downloadInto(VideoFrame& out);  // NVDEC surface straight into an NV12 cache frame
    void updateDecoderName();

    MediaInfo m_info;
    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_dec = nullptr;
    const AVCodec* m_codec = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_swFrame = nullptr;
    AVPacket* m_pkt = nullptr;
    SwsContext* m_sws = nullptr;     // -> RGBA
    SwsContext* m_swsYuv = nullptr;  // high bit depth -> 8-bit planar
    int m_stream = -1;

    std::vector<int64_t> m_pts;     // display-order PTS (stream time base)
    std::vector<int64_t> m_keyPts;  // keyframe PTS, sorted

    int64_t m_lastIndex = -1;  // index of the frame currently held in m_frame
    bool m_frameValid = false;
    bool m_drained = false;
    bool m_transferred = false;  // m_swFrame holds m_frame's pixels
    bool m_hwActive = false;     // decoding on NVDEC
    bool m_hwCounted = false;    // holds one of the limited hardware sessions
    Stats m_stats;
};
