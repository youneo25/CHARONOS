#pragma once

#include "MediaInfo.h"
#include "VideoFrame.h"

#include <QImage>

#include <functional>

// Receives frames a decoder produced on the way to the requested one (e.g. the rest of a
// GOP after a seek), so they can be cached instead of being decoded again later.
struct FrameSink {
    std::function<bool(int64_t index)> wants;
    std::function<void(int64_t index, VideoFrame&& frame)> deliver;
};

// A source of exactly-addressable frames. decode() is called from decoder threads only.
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual const MediaInfo& info() const = 0;

    // Decodes frame `index` (0-based, display order) into an RGBA8888 image
    // (export, tracking, stills).
    virtual bool decode(int64_t index, QImage& out, QString* error) = 0;

    // Decodes frame `index` in the form the cache keeps: native 8-bit planar Y'CbCr for video
    // (converted to RGB on the GPU), RGBA for everything else.
    virtual bool decodeFrame(int64_t index, VideoFrame& out, QString* error, const FrameSink* sink = nullptr)
    {
        Q_UNUSED(sink);
        QImage image;
        if (!decode(index, image, error)) return false;
        out = VideoFrame::fromImage(std::move(image));
        return !out.isNull();
    }

    // True when decode() may run concurrently from several threads.
    virtual bool supportsParallelDecode() const { return false; }
};
