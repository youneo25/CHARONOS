#pragma once

#include "FrameSource.h"

#include <vector>

struct AVFormatContext;
struct AVCodecContext;
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
    bool decode(int64_t index, QImage& out, QString* error, const FrameSink* sink = nullptr) override;
    const Stats& stats() const { return m_stats; }

private:
    void close();
    bool buildIndex(QString* error);
    void seekToKey(size_t keyIdx);
    int64_t receiveNext();  // index of the newly decoded frame in m_frame, -1 at EOF, -2 on error
    int64_t indexForPts(int64_t pts) const;
    bool convert(QImage& out);

    MediaInfo m_info;
    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_dec = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_pkt = nullptr;
    SwsContext* m_sws = nullptr;
    int m_stream = -1;

    std::vector<int64_t> m_pts;     // display-order PTS (stream time base)
    std::vector<int64_t> m_keyPts;  // keyframe PTS, sorted

    int64_t m_lastIndex = -1;  // index of the frame currently held in m_frame
    bool m_frameValid = false;
    bool m_drained = false;
    Stats m_stats;
};
