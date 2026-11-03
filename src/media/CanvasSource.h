#pragma once
#include "FrameSource.h"

// A real frame source keeps blank animation projects on the same playback/export path.
class CanvasSource final : public FrameSource {
public:
    CanvasSource(QSize size, int64_t count, double fps) {
        m_info.width = std::clamp(size.width(), 16, 8192);
        m_info.height = std::clamp(size.height(), 16, 8192);
        m_info.frameCount = std::clamp<int64_t>(count, 1, 100000);
        m_info.fps = std::clamp(fps, 1.0, 240.0);
        m_info.duration = m_info.frameCount / m_info.fps;
        m_info.codec = "2D Canvas";
        m_info.isImageSequence = true;
        m_info.decoderName = QStringLiteral("Canvas");
        m_image = QImage(m_info.width, m_info.height, QImage::Format_RGBA8888);
        m_image.fill(QColor(32, 32, 32));
    }
    const MediaInfo& info() const override { return m_info; }
    bool supportsParallelDecode() const override { return true; }
    bool decode(int64_t index, QImage& out, QString* error) override {
        if (index < 0 || index >= m_info.frameCount) {
            if (error) *error = "Canvas frame out of range";
            return false;
        }
        out = m_image; // implicitly shared; no per-frame blank bitmap allocation
        return true;
    }
private:
    MediaInfo m_info;
    QImage m_image;
};
