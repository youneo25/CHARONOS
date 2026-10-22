#include "VideoFrame.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QColor>

#include <algorithm>
#include <cstring>

namespace {

AVPixelFormat avFormat(VideoFrame::Format f)
{
    switch (f) {
    case VideoFrame::Format::YUV420P: return AV_PIX_FMT_YUV420P;
    case VideoFrame::Format::YUV422P: return AV_PIX_FMT_YUV422P;
    case VideoFrame::Format::YUV444P: return AV_PIX_FMT_YUV444P;
    case VideoFrame::Format::NV12: return AV_PIX_FMT_NV12;
    case VideoFrame::Format::RGBA: return AV_PIX_FMT_RGBA;
    default: return AV_PIX_FMT_NONE;
    }
}

int swsColorspace(VideoFrame::Matrix m)
{
    switch (m) {
    case VideoFrame::Matrix::BT601: return SWS_CS_ITU601;
    case VideoFrame::Matrix::BT2020: return SWS_CS_BT2020;
    case VideoFrame::Matrix::SMPTE240M: return SWS_CS_SMPTE240M;
    default: return SWS_CS_ITU709;
    }
}

} // namespace

VideoFrame::Matrix VideoFrame::matrixOf(const AVFrame* f)
{
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: return Matrix::BT709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return Matrix::BT2020;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_FCC: return Matrix::BT601;
    case AVCOL_SPC_SMPTE240M: return Matrix::SMPTE240M;
    default: return f->height >= 720 ? Matrix::BT709 : Matrix::BT601;
    }
}

bool VideoFrame::fullRangeOf(const AVFrame* f)
{
    if (f->color_range == AVCOL_RANGE_JPEG) return true;
    switch (f->format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P: return true;
    default: return false;
    }
}

int VideoFrame::planeCount() const
{
    switch (m_format) {
    case Format::RGBA: return 1;
    case Format::NV12: return 2;
    case Format::YUV420P:
    case Format::YUV422P:
    case Format::YUV444P: return 3;
    default: return 0;
    }
}

int VideoFrame::planeWidth(int plane) const
{
    if (plane == 0 || m_format == Format::RGBA || m_format == Format::YUV444P) return m_width;
    return (m_width + 1) / 2;  // 4:2:0 and 4:2:2 halve the chroma width
}

int VideoFrame::planeHeight(int plane) const
{
    if (plane == 0 || (m_format != Format::YUV420P && m_format != Format::NV12)) return m_height;
    return (m_height + 1) / 2;
}

int VideoFrame::planeBytesPerLine(int plane) const
{
    if (m_format == Format::RGBA) return plane == 0 ? int(m_rgba.bytesPerLine()) : 0;
    return planeWidth(plane) * (m_format == Format::NV12 && plane == 1 ? 2 : 1);
}

const uint8_t* VideoFrame::planeData(int plane) const
{
    if (m_format == Format::RGBA) return plane == 0 ? m_rgba.constBits() : nullptr;
    if (!isYuv() || plane < 0 || plane >= planeCount() || !m_data) return nullptr;
    return m_data.get() + m_offset[plane];
}

uint8_t* VideoFrame::planeBits(int plane)
{
    if (!isYuv() || plane < 0 || plane >= planeCount() || !m_data) return nullptr;
    return m_data.get() + m_offset[plane];
}

qint64 VideoFrame::sizeInBytes() const
{
    if (m_format == Format::RGBA) return m_rgba.sizeInBytes();
    return m_bytes;
}

void VideoFrame::allocatePlanes(Format format, int width, int height)
{
    m_format = format;
    m_width = width;
    m_height = height;
    m_rgba = QImage();
    const qint64 y = qint64(planeBytesPerLine(0)) * planeHeight(0);
    const qint64 c = qint64(planeBytesPerLine(1)) * planeHeight(1);
    m_offset[0] = 0;
    m_offset[1] = y;
    m_offset[2] = format == Format::NV12 ? y : y + c;
    m_bytes = format == Format::NV12 ? y + c : y + 2 * c;
    m_data = std::make_unique_for_overwrite<uint8_t[]>(size_t(m_bytes));
}

void VideoFrame::yuvToRgbMatrix(float out[16]) const
{
    double kr = 0.2126, kb = 0.0722;
    switch (m_matrix) {
    case Matrix::BT601: kr = 0.299; kb = 0.114; break;
    case Matrix::BT2020: kr = 0.2627; kb = 0.0593; break;
    case Matrix::SMPTE240M: kr = 0.212; kb = 0.087; break;
    default: break;
    }
    const double kg = 1.0 - kr - kb;
    const double ys = m_fullRange ? 1.0 : 255.0 / 219.0;
    const double yo = m_fullRange ? 0.0 : 16.0 / 255.0;
    const double cs = m_fullRange ? 1.0 : 255.0 / 224.0;
    const double co = 128.0 / 255.0;
    // Coefficients on Cb / Cr after range expansion.
    const double rv = 2.0 * (1.0 - kr);
    const double gu = -2.0 * kb * (1.0 - kb) / kg;
    const double gv = -2.0 * kr * (1.0 - kr) / kg;
    const double bu = 2.0 * (1.0 - kb);
    const double m[16] = {
        ys, 0.0,     rv * cs, -ys * yo - rv * cs * co,
        ys, gu * cs, gv * cs, -ys * yo - (gu + gv) * cs * co,
        ys, bu * cs, 0.0,     -ys * yo - bu * cs * co,
        0.0, 0.0,    0.0,     1.0,
    };
    for (int i = 0; i < 16; ++i) out[i] = float(m[i]);
}

QImage VideoFrame::toImage(QSize fitInside) const
{
    QSize outSize(m_width, m_height);
    if (fitInside.isValid() && !fitInside.isEmpty() && (m_width > fitInside.width() || m_height > fitInside.height()))
        outSize = QSize(m_width, m_height).scaled(fitInside, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    if (m_format == Format::RGBA)
        return outSize == m_rgba.size() ? m_rgba : m_rgba.scaled(outSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (!isYuv() || !m_data) return {};
    QImage out(outSize, QImage::Format_RGBA8888);
    if (out.isNull()) return {};
    const int flags = outSize == size() ? SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT : SWS_AREA;
    SwsContext* sws = sws_getContext(m_width, m_height, avFormat(m_format), outSize.width(), outSize.height(), AV_PIX_FMT_RGBA,
                                     flags, nullptr, nullptr, nullptr);
    if (!sws) return {};
    const int* coeffs = sws_getCoefficients(swsColorspace(m_matrix));
    sws_setColorspaceDetails(sws, coeffs, m_fullRange ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    const uint8_t* src[4] = { nullptr, nullptr, nullptr, nullptr };
    int srcStride[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < planeCount(); ++i) {
        src[i] = planeData(i);
        srcStride[i] = planeBytesPerLine(i);
    }
    uint8_t* dst[4] = { out.bits(), nullptr, nullptr, nullptr };
    const int dstStride[4] = { int(out.bytesPerLine()), 0, 0, 0 };
    sws_scale(sws, src, srcStride, 0, m_height, dst, dstStride);
    sws_freeContext(sws);
    return out;
}

double VideoFrame::averageLuma() const
{
    if (isNull()) return 0.0;
    double sum = 0.0;
    int n = 0;
    for (int gy = 0; gy < 8; ++gy)
        for (int gx = 0; gx < 8; ++gx) {
            const int x = std::min(m_width - 1, (2 * gx + 1) * m_width / 16);
            const int y = std::min(m_height - 1, (2 * gy + 1) * m_height / 16);
            if (isYuv()) {
                double v = planeData(0)[qint64(y) * planeBytesPerLine(0) + x];
                if (!m_fullRange) v = std::clamp((v - 16.0) * 255.0 / 219.0, 0.0, 255.0);
                sum += v;
            } else {
                const QColor c = m_rgba.pixelColor(x, y);
                sum += 0.2126 * c.red() + 0.7152 * c.green() + 0.0722 * c.blue();
            }
            ++n;
        }
    return n ? sum / n : 0.0;
}

VideoFrame VideoFrame::fromImage(QImage image)
{
    VideoFrame f;
    if (image.isNull()) return f;
    if (image.format() != QImage::Format_RGBA8888) image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) return f;
    f.m_format = Format::RGBA;
    f.m_width = image.width();
    f.m_height = image.height();
    f.m_rgba = std::move(image);
    return f;
}

std::shared_ptr<const VideoFrame> VideoFrame::share(QImage image)
{
    return std::make_shared<const VideoFrame>(fromImage(std::move(image)));
}

bool VideoFrame::fromAVFrame(const AVFrame* f, VideoFrame& out, SwsContext** sws)
{
    if (!f || f->width <= 0 || f->height <= 0) return false;
    const int w = f->width, h = f->height;
    const auto fmt = static_cast<AVPixelFormat>(f->format);

    Format target = Format::None;
    bool direct = false;
    switch (fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P: target = Format::YUV420P; direct = true; break;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P: target = Format::YUV422P; direct = true; break;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P: target = Format::YUV444P; direct = true; break;
    case AV_PIX_FMT_NV12: target = Format::NV12; direct = true; break;
    case AV_PIX_FMT_NV21: target = Format::YUV420P; break;
    default: {
        // Higher bit depths (10-bit H.265, ProRes 4:2:2 10-bit, P010 from NVDEC...) are reduced to the
        // 8-bit planar layout with the same chroma subsampling; the display is 8-bit anyway.
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
        if (!d || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL)) ||
            d->nb_components < 3)
            return false;
        if (d->log2_chroma_w == 1 && d->log2_chroma_h == 1) target = Format::YUV420P;
        else if (d->log2_chroma_w == 1 && d->log2_chroma_h == 0) target = Format::YUV422P;
        else if (d->log2_chroma_w == 0 && d->log2_chroma_h == 0) target = Format::YUV444P;
        else return false;
    }
    }

    out.allocatePlanes(target, w, h);
    out.m_matrix = matrixOf(f);
    out.m_fullRange = fullRangeOf(f);
    if (direct) {
        for (int i = 0; i < out.planeCount(); ++i)
            av_image_copy_plane(out.planeBits(i), out.planeBytesPerLine(i), f->data[i], f->linesize[i],
                                out.planeBytesPerLine(i), out.planeHeight(i));
        return true;
    }
    uint8_t* y = out.planeBits(0);
    uint8_t* u = out.planeBits(1);
    uint8_t* v = out.planeBits(2);
    const int cw = out.planeWidth(1), ch = out.planeHeight(1);
    if (fmt == AV_PIX_FMT_NV21) {
        // Cr/Cb byte pairs: de-interleave into three planes.
        av_image_copy_plane(y, w, f->data[0], f->linesize[0], w, h);
        for (int row = 0; row < ch; ++row) {
            const uint8_t* s = f->data[1] + qint64(row) * f->linesize[1];
            uint8_t* cr = v + qint64(row) * cw;
            uint8_t* cb = u + qint64(row) * cw;
            for (int x = 0; x < cw; ++x) {
                cr[x] = s[2 * x];
                cb[x] = s[2 * x + 1];
            }
        }
        return true;
    }

    *sws = sws_getCachedContext(*sws, w, h, fmt, w, h, avFormat(target), SWS_POINT, nullptr, nullptr, nullptr);
    if (!*sws) {
        out = VideoFrame();
        return false;
    }
    // Same range in and out: only the bit depth changes.
    const int* coeffs = sws_getCoefficients(swsColorspace(out.m_matrix));
    const int range = out.m_fullRange ? 1 : 0;
    sws_setColorspaceDetails(*sws, coeffs, range, coeffs, range, 0, 1 << 16, 1 << 16);
    uint8_t* dst[4] = { y, u, v, nullptr };
    const int dstStride[4] = { w, cw, cw, 0 };
    sws_scale(*sws, f->data, f->linesize, 0, h, dst, dstStride);
    return true;
}
