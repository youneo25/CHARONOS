#include "Exporter.h"
#include "Lang.h"

#include "VideoEncoder.h"
#include "media/MediaLibrary.h"
#include "media/CanvasSource.h"
#include "model/AnnotationPainter.h"

#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>

QRectF stillFrameBounds(QSize media, const Source& source, int frame, const StillFrameOptions& o)
{
    QRectF bounds(QPointF(0, 0), QSizeF(media));
    if (!o.includeOutside) return bounds;
    if (o.notes)
        for (const auto& e : source.renderable(frame)) {
            const Annotation a = source.evaluated(*e.annotation, frame);
            // Selection boxes alone do not include thick pens or arrowheads.
            const double bleed = std::max(2.0, a.size * std::max({1.0, std::abs(a.stretchX), std::abs(a.stretchY)}) * (a.type == "arrow" ? 4 : 1));
            const QRectF box = AnnotationPainter::outline(a, media).boundingRect().adjusted(-bleed, -bleed, bleed, bleed);
            bounds = bounds.united(box);
        }
    if (o.tracks)
        for (const Track& track : source.tracks) {
            if (!track.visibleAt(frame)) continue;
            for (const auto& [f, p] : track.points) {
                if (o.trackRange > 0 && (f > frame || f < frame - o.trackRange)) continue;
                const QPointF q(p.x() * media.width(), p.y() * media.height());
                bounds = bounds.united(QRectF(q - QPointF(24, 24), QSizeF(180, 48)));
            }
        }
    return bounds.adjusted(-o.padding, -o.padding, o.padding, o.padding).toAlignedRect();
}

QImage composeStillFrame(const QImage& frame, const Source& source, int localFrame, const StillFrameOptions& o)
{
    if (frame.isNull() || !std::isfinite(o.scale) || o.scale <= 0) return {};
    const QRectF bounds = stillFrameBounds(frame.size(), source, localFrame, o);
    const double w = std::ceil(bounds.width() * o.scale), h = std::ceil(bounds.height() * o.scale);
    if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 16384 || h > 16384 || w * h > 64000000) return {};
    QImage out(QSize(int(w), int(h)), QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) return {};
    out.fill(o.background);
    QPainter p(&out);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    p.scale(o.scale, o.scale);
    p.translate(-bounds.topLeft());
    if (o.video) p.drawImage(QPointF(0, 0), frame);
    if (o.notes)
        for (const auto& e : source.renderable(localFrame))
            AnnotationPainter::paint(p, source.evaluated(*e.annotation, localFrame), frame.size(), 1, false);
    if (o.tracks) {
        TrackPainter::Style style; style.range = o.trackRange;
        TrackPainter::paint(p, source.tracks, localFrame, frame.size(), style);
    }
    if (!o.caption.isEmpty()) {
        p.resetTransform();
        QFont font("Segoe UI"); font.setPixelSize(std::max(12, out.height() / 45)); p.setFont(font);
        const QRect box = p.fontMetrics().boundingRect(o.caption).adjusted(-8, -4, 8, 4);
        const QRect target(12, out.height() - box.height() - 12, box.width(), box.height());
        p.fillRect(target, QColor(0, 0, 0, 180)); p.setPen(Qt::white); p.drawText(target, Qt::AlignCenter, o.caption);
    }
    return out;
}

QImage composeFrame(const QImage& frame, const Source& source, int localFrame, QSize outSize, bool withNotes,
                    const QString& caption, bool withTracks, int trackRange, double margin)
{
    QImage canvas(outSize, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);

    const QSizeF src(frame.size());
    const double border = 1 + 2 * std::clamp(margin, 0.0, 1.0);
    const double s = std::min(outSize.width() / (src.width() * border), outSize.height() / (src.height() * border));
    const QRectF r(QPointF((outSize.width() - src.width() * s) / 2, (outSize.height() - src.height() * s) / 2), src * s);
    p.drawImage(r, frame);

    if (withNotes) {
        p.save();
        p.translate(r.topLeft());
        p.scale(s, s);
        for (const Source::Entry& e : source.renderable(localFrame))
            AnnotationPainter::paint(p, source.evaluated(*e.annotation, localFrame), src, 1.0, false);
        p.restore();
    }

    if (withTracks && !source.tracks.isEmpty()) {
        p.save();
        p.translate(r.topLeft());
        p.scale(s, s);
        TrackPainter::Style st;
        st.range = trackRange;
        st.unit = std::max(1.0, outSize.height() / 720.0) / s;
        TrackPainter::paint(p, source.tracks, localFrame, src, st);
        p.restore();
    }

    if (!caption.isEmpty()) {
        QFont f(QStringLiteral("Segoe UI"));
        f.setPixelSize(std::max(11, outSize.height() / 45));
        p.setFont(f);
        const QFontMetrics fm(f);
        const QRect box(12, outSize.height() - fm.height() - 18, fm.horizontalAdvance(caption) + 16, fm.height() + 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(QColor(240, 240, 235));
        p.drawText(box, Qt::AlignCenter, caption);
    }
    return canvas;
}

namespace {

QPainter::CompositionMode compositionOf(BlendMode b)
{
    switch (b) {
    case BlendMode::Add: return QPainter::CompositionMode_Plus;
    case BlendMode::Screen: return QPainter::CompositionMode_Screen;
    case BlendMode::Multiply: return QPainter::CompositionMode_Multiply;
    case BlendMode::Difference: return QPainter::CompositionMode_Difference;
    default: return QPainter::CompositionMode_SourceOver;
    }
}

// The viewer's analysis filter on one decoded picture; the previous frame is fetched only when needed.
QImage filtered(const ExportJob& job, const QString& sourceId, const QImage& picture, const std::function<QImage()>& previous)
{
    if (!job.filter.enabled()) return picture;
    QImage ref;
    if (job.filter.mode == AnalysisFilter::Mode::MovingSilhouette) {
        auto it = job.plates.find(sourceId);
        if (it != job.plates.end()) ref = it->second;
    } else if (job.filter.mode == AnalysisFilter::Mode::MotionHighlight) {
        ref = previous();
    }
    return analysis::apply(picture, job.filter, job.filterThreshold, ref);
}

using DecoderMap = std::map<QString, std::unique_ptr<FrameSource>>;

FrameSource* openDecoder(DecoderMap& decoders, const Source& s, QString* error)
{
    auto it = decoders.find(s.id);
    if (it != decoders.end()) return it->second.get();
    QString err;
    std::unique_ptr<FrameSource> src;
    if (s.media.kind == MediaRef::Kind::Canvas) src = std::make_unique<CanvasSource>(QSize(s.media.width, s.media.height), s.frameCount, s.fps);
    else src = MediaLibrary::openSource(s.media.isSequence(), s.media.path, s.media.frames, s.fps, &err);
    if (!src && error) *error = QStringLiteral("%1: %2").arg(s.displayName(), err);
    return (decoders[s.id] = std::move(src)).get();
}

// 1.1: the multi-track timeline, composed frame by frame like the viewer.
bool runTimelineExport(const ExportJob& job, std::atomic<int64_t>& done, const std::atomic<bool>& cancel, QString* error)
{
    if (job.canvas.isEmpty() || int64_t(job.layers.size()) != job.frameCount()) {
        if (error) *error = L("Invalid export settings.", "내보내기 설정이 올바르지 않습니다.");
        return false;
    }
    DecoderMap decoders;
    const double border = 1 + 2 * std::clamp(job.margin, 0.0, 1.0);
    QSize outSize(qRound(job.canvas.width() * job.scale * border), qRound(job.canvas.height() * job.scale * border));
    if (!job.isSequence()) outSize = VideoEncoder::alignedSize(outSize);
    const double s = std::min(outSize.width() / (job.canvas.width() * border), outSize.height() / (job.canvas.height() * border));
    const QPointF origin((outSize.width() - job.canvas.width() * s) / 2, (outSize.height() - job.canvas.height() * s) / 2);
    const bool withAudio = job.audio && !job.isSequence() && job.mix && !job.mix->empty();
    const double tfps = std::max(1.0, job.timelineFps);

    VideoEncoder encoder;
    QString seqDir, seqBase, seqExt;
    if (job.isSequence()) {
        const QFileInfo fi(job.output);
        seqDir = fi.absolutePath();
        seqBase = fi.fileName();
        seqExt = job.format == ExportJob::Format::PngSequence ? "png" : job.format == ExportJob::Format::JpgSequence ? "jpg" : "tif";
        QDir().mkpath(seqDir);
    } else {
        const VideoEncoder::Codec codec = job.format == ExportJob::Format::Mp4Nvenc       ? VideoEncoder::Codec::H264Nvenc
                                          : job.format == ExportJob::Format::Mp4HevcNvenc ? VideoEncoder::Codec::HevcNvenc
                                          : job.format == ExportJob::Format::MovProRes    ? VideoEncoder::Codec::ProRes422HQ
                                                                                          : VideoEncoder::Codec::H264;
        if (!encoder.open(job.output, codec, outSize, job.fps, withAudio, error)) return false;
    }
    const int digits = std::max(4, int(QString::number(job.last - job.first).size()));
    int64_t samplesDone = 0;
    std::vector<float> mixBuf;
    std::vector<int16_t> abuf;
    for (int64_t g = job.first; g <= job.last; ++g) {
        if (cancel) {
            if (!job.isSequence()) encoder.finish(nullptr);
            if (error) *error = L("Cancelled", "취소됨");
            return false;
        }
        QImage out(outSize, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::black);
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(origin);
        p.scale(s, s);
        const ExportJob::Layer* primary = nullptr;
        QSizeF primarySize;
        QString caption;
        for (const ExportJob::Layer& l : job.layers[size_t(g - job.first)]) {
            const Source* src = job.project.find(l.sourceId);
            FrameSource* dec = src ? openDecoder(decoders, *src, error) : nullptr;
            if (!dec) return false;
            QImage frame;
            QString err;
            if (!dec->decode(l.local, frame, &err)) {
                if (error) *error = L("%1 frame %2: %3", "%1 프레임 %2: %3").arg(src->displayName()).arg(l.local).arg(err);
                return false;
            }
            frame = filtered(job, l.sourceId, frame, [&] {
                QImage prev;
                if (l.local > 0) dec->decode(l.local - 1, prev, nullptr);
                return prev;
            });
            p.setOpacity(std::clamp(l.opacity, 0.0, 1.0));
            p.setCompositionMode(compositionOf(l.blend));
            p.drawImage(l.rect, frame);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setOpacity(1.0);
            if (l.primary) {
                primary = &l;
                primarySize = QSizeF(frame.size());
                caption = QStringLiteral("%1   %2   #%3").arg(src->displayName()).arg(l.local).arg(g);
            }
        }
        if (primary && (job.notes || job.tracks) && !primarySize.isEmpty()) {
            const Source& src = *job.project.find(primary->sourceId);
            const auto hidden = job.hiddenNotes.find(primary->sourceId);
            p.save();
            p.translate(primary->rect.topLeft());
            p.scale(primary->rect.width() / primarySize.width(), primary->rect.height() / primarySize.height());
            if (job.notes)
                for (const Source::Entry& e : src.renderable(int(primary->local))) {
                    if (hidden != job.hiddenNotes.end() && hidden->second.contains(e.annotation->id)) continue;
                    AnnotationPainter::paint(p, src.evaluated(*e.annotation, int(primary->local)), primarySize, 1.0, false);
                }
            if (job.tracks && !src.tracks.isEmpty()) {
                TrackPainter::Style st;
                st.range = job.trackRange;
                st.unit = std::max(1.0, outSize.height() / 720.0) / (s * primary->rect.height() / primarySize.height());
                TrackPainter::paint(p, src.tracks, int(primary->local), primarySize, st);
            }
            p.restore();
        }
        if (job.caption) {
            p.resetTransform();
            if (caption.isEmpty()) caption = QStringLiteral("#%1").arg(g);
            QFont f(QStringLiteral("Segoe UI"));
            f.setPixelSize(std::max(11, outSize.height() / 45));
            p.setFont(f);
            const QFontMetrics fm(f);
            const QRect box(12, outSize.height() - fm.height() - 18, fm.horizontalAdvance(caption) + 16, fm.height() + 8);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawRoundedRect(box, 4, 4);
            p.setPen(QColor(240, 240, 235));
            p.drawText(box, Qt::AlignCenter, caption);
        }
        p.end();
        const QImage frameOut = out.convertToFormat(QImage::Format_RGB32);
        if (job.isSequence()) {
            const QString path = QStringLiteral("%1/%2_%3.%4").arg(seqDir, seqBase).arg(g - job.first, digits, 10, QChar('0')).arg(seqExt);
            QImageWriter writer(path);
            if (job.format == ExportJob::Format::JpgSequence) writer.setQuality(95);
            if (job.format == ExportJob::Format::TiffSequence) writer.setCompression(1);
            if (!writer.write(frameOut)) {
                if (error) *error = QStringLiteral("%1: %2").arg(path, writer.errorString());
                return false;
            }
        } else {
            if (!encoder.write(frameOut, error)) return false;
            if (withAudio) {
                // This output frame's share of samples: the timeline frame's sound, stretched to the output rate.
                const int64_t target = std::llround(double(g - job.first + 1) * AudioClip::kRate / job.fps);
                const int n = int(target - samplesDone);
                samplesDone = target;
                if (n > 0) {
                    mixBuf.assign(size_t(n) * 2, 0.f);
                    job.mix->render(g / tfps, (1.0 / tfps) / n, n, mixBuf.data());
                    abuf.resize(size_t(n) * 2);
                    for (size_t i = 0; i < abuf.size(); ++i) abuf[i] = int16_t(std::lround(std::clamp(double(mixBuf[i]), -1.0, 1.0) * 32767.0));
                    if (!encoder.writeAudio(abuf.data(), n, error)) return false;
                }
            }
        }
        ++done;
    }
    return job.isSequence() ? true : encoder.finish(error);
}

} // namespace

bool runExport(const ExportJob& job, std::atomic<int64_t>& done, const std::atomic<bool>& cancel, QString* error)
{
    if (job.output.trimmed().isEmpty() || job.first < 0 || job.last < job.first || job.last >= job.sequence.total || job.fps <= 0 || job.scale <= 0) {
        if (error) *error = L("Invalid export settings.", "내보내기 설정이 올바르지 않습니다.");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(job.output).absolutePath())) {
        if (error) *error = L("Cannot create output folder.", "출력 폴더를 만들 수 없습니다.");
        return false;
    }
    if (job.timeline) return runTimelineExport(job, done, cancel, error);
    std::map<QString, std::unique_ptr<FrameSource>> decoders;
    auto decoderFor = [&](const Source& s) -> FrameSource* {
        auto it = decoders.find(s.id);
        if (it != decoders.end()) return it->second.get();
        QString err;
        std::unique_ptr<FrameSource> src;
        if (s.media.kind == MediaRef::Kind::Canvas) src = std::make_unique<CanvasSource>(QSize(s.media.width, s.media.height), s.frameCount, s.fps);
        else src = MediaLibrary::openSource(s.media.isSequence(), s.media.path, s.media.frames, s.fps, &err);
        if (!src && error) *error = QStringLiteral("%1: %2").arg(s.displayName(), err);
        return (decoders[s.id] = std::move(src)).get();
    };

    // Output size follows the first clip in range; other clips are letterboxed into it.
    const Sequence::Segment* firstSeg = job.sequence.at(job.first);
    while (firstSeg && firstSeg->isGap() && firstSeg->start + firstSeg->count <= job.last) firstSeg = job.sequence.at(firstSeg->start + firstSeg->count);
    const Source* firstSrc = firstSeg ? job.project.find(firstSeg->sourceId) : nullptr;
    if (!firstSrc) {
        if (error) *error = L("Nothing to export.", "내보낼 클립이 없습니다.");
        return false;
    }
    FrameSource* firstDec = decoderFor(*firstSrc);
    if (!firstDec) return false;
    using Layout = ExportJob::Compare::Layout;
    const ExportJob::Compare& cmp = job.compare;
    const Source* bSrc = cmp.layout != Layout::Off ? job.project.find(cmp.sourceId) : nullptr;
    FrameSource* bDec = bSrc ? decoderFor(*bSrc) : nullptr;
    if (bSrc && !bDec) return false;
    // A's size; side by side / top-bottom put B in a second box of the same size next to it.
    const double border = 1 + 2 * std::clamp(job.margin, 0.0, 1.0);
    const QSize aSize(qRound(firstDec->info().width * job.scale * border), qRound(firstDec->info().height * job.scale * border));
    QSize outSize = aSize;
    if (bDec && cmp.layout == Layout::SideBySide) outSize.rwidth() *= 2;
    if (bDec && cmp.layout == Layout::TopBottom) outSize.rheight() *= 2;
    if (!job.isSequence()) outSize = VideoEncoder::alignedSize(outSize);
    const QRectF aBox = bDec && cmp.layout == Layout::SideBySide ? QRectF(0, 0, outSize.width() / 2.0, outSize.height())
                        : bDec && cmp.layout == Layout::TopBottom ? QRectF(0, 0, outSize.width(), outSize.height() / 2.0)
                                                                  : QRectF(0, 0, outSize.width(), outSize.height());
    const QRectF bBox = cmp.layout == Layout::SideBySide  ? aBox.translated(aBox.width(), 0)
                        : cmp.layout == Layout::TopBottom ? aBox.translated(0, aBox.height())
                                                          : aBox;
    auto fit = [](QSizeF media, const QRectF& box) {
        const double s = std::min(box.width() / media.width(), box.height() / media.height());
        const QSizeF size = media * s;
        return QRectF(box.center() - QPointF(size.width() / 2, size.height() / 2), size);
    };

    // Audio: is there any in the range at all? (no point writing a silent track otherwise)
    std::map<QString, std::shared_ptr<AudioClip>> audio = job.audioClips;
    auto audioFor = [&](const Source& s) -> const AudioClip* {
        if (s.media.kind == MediaRef::Kind::Canvas || s.media.isSequence()) return nullptr;
        auto it = audio.find(s.id);
        if (it == audio.end()) it = audio.emplace(s.id, AudioClip::decode(s.media.path, nullptr)).first;  // not decoded yet
        return it->second.get();
    };
    bool withAudio = false;
    if (job.audio && !job.isSequence()) {
        if (bSrc && cmp.audioFromB) withAudio = audioFor(*bSrc) != nullptr;
        else
            for (const Sequence::Segment& seg : job.sequence.segments)
                if (seg.start <= job.last && seg.start + seg.count > job.first)
                    if (const Source* s = job.project.find(seg.sourceId); s && audioFor(*s)) withAudio = true;
    }
    int64_t samplesDone = 0;
    std::vector<int16_t> abuf;

    VideoEncoder encoder;
    QString seqDir, seqBase, seqExt;
    if (job.isSequence()) {
        const QFileInfo fi(job.output);
        seqDir = fi.absolutePath();
        seqBase = fi.fileName();
        seqExt = job.format == ExportJob::Format::PngSequence   ? "png"
                 : job.format == ExportJob::Format::JpgSequence ? "jpg"
                                                                : "tif";
        QDir().mkpath(seqDir);
    } else {
        const VideoEncoder::Codec codec = job.format == ExportJob::Format::Mp4Nvenc       ? VideoEncoder::Codec::H264Nvenc
                                          : job.format == ExportJob::Format::Mp4HevcNvenc ? VideoEncoder::Codec::HevcNvenc
                                          : job.format == ExportJob::Format::MovProRes    ? VideoEncoder::Codec::ProRes422HQ
                                                                                          : VideoEncoder::Codec::H264;
        if (!encoder.open(job.output, codec, outSize, job.fps, withAudio, error)) return false;
    }

    const int digits = std::max(4, int(QString::number(job.last).size()));
    QImage frame;
    for (int64_t g = job.first; g <= job.last; ++g) {
        if (cancel) {
            if (!job.isSequence()) encoder.finish(nullptr);
            if (error) *error = L("Cancelled", "취소됨");
            return false;
        }
        const Sequence::Segment* seg = job.sequence.at(g);
        if (seg && seg->isGap()) {
            // An empty stretch of the timeline: black and silence.
            QImage black(outSize, QImage::Format_RGB32);
            black.fill(Qt::black);
            if (job.isSequence()) {
                const QString path = QStringLiteral("%1/%2_%3.%4").arg(seqDir, seqBase).arg(g - job.first, digits, 10, QChar('0')).arg(seqExt);
                if (!QImageWriter(path).write(black)) return false;
            } else {
                if (!encoder.write(black, error)) return false;
                if (withAudio) {
                    const int64_t target = std::llround(double(g - job.first + 1) * AudioClip::kRate / job.fps);
                    const int n = int(target - samplesDone);
                    samplesDone = target;
                    abuf.assign(size_t(std::max(0, n)) * 2, 0);
                    if (n > 0 && !encoder.writeAudio(abuf.data(), n, error)) return false;
                }
            }
            ++done;
            continue;
        }
        const Source* src = seg ? job.project.find(seg->sourceId) : nullptr;
        FrameSource* dec = src ? decoderFor(*src) : nullptr;
        if (!dec) return false;
        const int64_t rawLocal = g - seg->start + seg->offset;  // source frame (timeline clips may be trimmed)
        const int64_t local = std::min<int64_t>(rawLocal, dec->info().frameCount - 1);  // compare: A holds its last frame
        QString err;
        if (!dec->decode(local, frame, &err)) {
            if (error) *error = L("%1 frame %2: %3", "%1 프레임 %2: %3").arg(src->displayName()).arg(local).arg(err);
            return false;
        }
        frame = filtered(job, src->id, frame, [&] {
            QImage prev;
            if (local > 0) dec->decode(local - 1, prev, nullptr);
            return prev;
        });
        const QString caption = job.caption ? QStringLiteral("%1   %2   #%3").arg(src->displayName()).arg(local).arg(g)
                                            : QString();
        QImage out;
        int64_t bLocal = 0;
        if (bDec) {
            bLocal = cmp.bFrame.empty() ? 0 : cmp.bFrame[size_t(std::clamp<int64_t>(rawLocal, 0, int64_t(cmp.bFrame.size()) - 1))];
            QImage bFrame;
            if (!bDec->decode(bLocal, bFrame, &err)) {
                if (error) *error = L("%1 frame %2: %3", "%1 프레임 %2: %3").arg(bSrc->displayName()).arg(bLocal).arg(err);
                return false;
            }
            bFrame = filtered(job, bSrc->id, bFrame, [&] {
                QImage prev;
                if (bLocal > 0) bDec->decode(bLocal - 1, prev, nullptr);
                return prev;
            });
            const bool separate = cmp.layout == Layout::SideBySide || cmp.layout == Layout::TopBottom;
            const QString aCap = job.caption ? QStringLiteral("A  %1   %2").arg(src->displayName()).arg(local) : QString();
            const QString bCap = job.caption ? QStringLiteral("B  %1   %2").arg(bSrc->displayName()).arg(bLocal) : QString();
            out = QImage(outSize, QImage::Format_RGB32);
            out.fill(Qt::black);
            QPainter p(&out);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const QRectF ar = fit(QSizeF(frame.size()), aBox);
            p.drawImage(ar.topLeft(), composeFrame(frame, *src, int(rawLocal), ar.size().toSize(), job.notes,
                                                   separate || !job.caption ? aCap : aCap + QStringLiteral("    |    ") + bCap,
                                                   job.tracks, job.trackRange, job.margin));
            const QRectF br = fit(QSizeF(bFrame.size()), bBox);
            const QImage bImg = composeFrame(bFrame, *bSrc, int(bLocal), br.size().toSize(), job.notes, separate ? bCap : QString(),
                                             job.tracks, job.trackRange, job.margin);
            if (cmp.layout == Layout::Wipe) {
                const double x = aBox.left() + aBox.width() * cmp.wipe;
                p.save();
                p.setClipRect(QRectF(QPointF(x, 0), QPointF(outSize.width(), outSize.height())));
                p.drawImage(br.topLeft(), bImg);
                p.restore();
                p.setPen(QPen(QColor(255, 255, 255, 220), std::max(1.0, outSize.height() / 540.0)));
                p.drawLine(QPointF(x, ar.top()), QPointF(x, ar.bottom()));
            } else {
                if (cmp.layout == Layout::Blend) p.setOpacity(0.5);
                p.drawImage(br.topLeft(), bImg);
            }
        } else {
            out = composeFrame(frame, *src, int(rawLocal), outSize, job.notes, caption, job.tracks, job.trackRange, job.margin);
        }

        if (job.isSequence()) {
            const QString path = QStringLiteral("%1/%2_%3.%4").arg(seqDir, seqBase).arg(g - job.first, digits, 10, QChar('0')).arg(seqExt);
            QImageWriter writer(path);
            if (job.format == ExportJob::Format::JpgSequence) writer.setQuality(95);
            if (job.format == ExportJob::Format::TiffSequence) writer.setCompression(1);
            if (!writer.write(out)) {
                if (error) *error = QStringLiteral("%1: %2").arg(path, writer.errorString());
                return false;
            }
        } else {
            if (!encoder.write(out, error)) return false;
            if (withAudio) {
                // Exactly this output frame's share of samples, taken from the clip's own audio for the
                // matching source frame, so sound follows the picture at any output fps (varispeed).
                const int64_t target = std::llround(double(g - job.first + 1) * AudioClip::kRate / job.fps);
                const int n = int(target - samplesDone);
                samplesDone = target;
                abuf.assign(size_t(std::max(0, n)) * 2, 0);
                const bool fromB = bSrc && cmp.audioFromB && rawLocal + 1 < int64_t(cmp.bTime.size());
                if (const AudioClip* clip = fromB ? audioFor(*bSrc) : audioFor(*src); clip && n > 0) {
                    // This frame's real time span (VFR-safe) on the clip's audio; for B, the span B plays under it.
                    const MediaInfo& info = dec->info();
                    const double t0 = fromB ? cmp.bTime[size_t(rawLocal)] : info.timeOf(rawLocal);
                    const double t1 = fromB ? cmp.bTime[size_t(rawLocal + 1)] : info.timeOf(rawLocal + 1);
                    const double s0 = (t0 - clip->startTime) * clip->rate;
                    const double span = (t1 - t0) * clip->rate;
                    const int64_t frames = clip->frames();
                    for (int j = 0; j < n; ++j) {
                        const double pos = s0 + span * j / n;
                        if (pos < 0 || pos >= frames - 1) continue;
                        const int64_t i0 = int64_t(pos);
                        const double f = pos - double(i0);
                        const int16_t* s = clip->pcm.data() + i0 * 2;
                        abuf[size_t(j) * 2] = int16_t(std::lround(s[0] * (1 - f) + s[2] * f));
                        abuf[size_t(j) * 2 + 1] = int16_t(std::lround(s[1] * (1 - f) + s[3] * f));
                    }
                }
                if (n > 0 && !encoder.writeAudio(abuf.data(), n, error)) return false;
            }
        }
        ++done;
    }
    return job.isSequence() ? true : encoder.finish(error);
}
