#pragma once

#include <QImage>

#include <cstdint>
#include <memory>

struct AVFrame;

// A decoded picture as the frame cache keeps it.
//
// Video stays in its native planar Y'CbCr layout (4:2:0 = 1.5 bytes per pixel instead of 4
// for RGBA, about 60% less cache memory) and the viewer converts it to RGB in a shader.
// Stills, canvases and anything with alpha are kept as RGBA8888. Frames are immutable once
// they are in the cache and are shared between the viewer, onion skin and compare views.
class VideoFrame {
public:
    // NV12 = 4:2:0 with Cb/Cr interleaved in one plane, as NVDEC produces it (no repacking).
    enum class Format { None, RGBA, YUV420P, YUV422P, YUV444P, NV12 };
    enum class Matrix { BT601, BT709, BT2020, SMPTE240M };

    VideoFrame() = default;
    VideoFrame(VideoFrame&&) noexcept = default;
    VideoFrame& operator=(VideoFrame&&) noexcept = default;
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;

    Format format() const { return m_format; }
    bool isNull() const { return m_format == Format::None || m_width <= 0 || m_height <= 0; }
    bool isYuv() const { return isPlanarYuv() || m_format == Format::NV12; }
    bool isPlanarYuv() const { return m_format == Format::YUV420P || m_format == Format::YUV422P || m_format == Format::YUV444P; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    QSize size() const { return { m_width, m_height }; }
    Matrix matrix() const { return m_matrix; }
    bool fullRange() const { return m_fullRange; }

    // Planes: RGBA has one (the image), planar Y'CbCr three, NV12 two. Planes are tightly
    // packed; planeWidth is in samples (an NV12 chroma sample is a Cb/Cr byte pair).
    int planeCount() const;
    int planeWidth(int plane) const;
    int planeHeight(int plane) const;
    int planeBytesPerLine(int plane) const;
    const uint8_t* planeData(int plane) const;
    uint8_t* planeBits(int plane);  // writable while the frame is being filled
    const QImage& rgba() const { return m_rgba; }

    qint64 sizeInBytes() const;

    // R'G'B' = M * (Y', Cb, Cr, 1) with full/limited range and the frame's matrix, as a
    // row-major 4x4 for the shader (values 0..1).
    void yuvToRgbMatrix(float out[16]) const;

    // RGBA8888 copy for CPU consumers (still export, AI snapshots, tests). With `fitInside` the
    // conversion also scales down in the same pass (thumbnails), keeping the aspect ratio.
    QImage toImage(QSize fitInside = QSize()) const;
    // Mean luma of an 8x8 grid, 0..255 (cheap content probe for tests and diagnostics).
    double averageLuma() const;

    static VideoFrame fromImage(QImage image);
    static std::shared_ptr<const VideoFrame> share(QImage image);

    // Copies (or converts to 8-bit planar) a software AVFrame. Returns false for frames that
    // can't be represented, after which the caller should fall back to RGBA.
    static bool fromAVFrame(const AVFrame* frame, VideoFrame& out, struct SwsContext** sws);

    // Empty planes of the given layout; colour description taken from `props` when given.
    void allocatePlanes(Format format, int width, int height);
    void setColor(Matrix matrix, bool fullRange)
    {
        m_matrix = matrix;
        m_fullRange = fullRange;
    }
    static Matrix matrixOf(const AVFrame* frame);
    static bool fullRangeOf(const AVFrame* frame);

private:

    Format m_format = Format::None;
    int m_width = 0;
    int m_height = 0;
    Matrix m_matrix = Matrix::BT709;
    bool m_fullRange = false;
    QImage m_rgba;
    std::unique_ptr<uint8_t[]> m_data;  // Y, Cb, Cr back to back
    qint64 m_offset[3] = { 0, 0, 0 };
    qint64 m_bytes = 0;
};
