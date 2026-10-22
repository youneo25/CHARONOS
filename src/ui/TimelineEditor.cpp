#include "TimelineEditor.h"

#include "Icons.h"
#include "Lang.h"
#include "Palette.h"
#include "media/AudioMix.h"

#include <QDataStream>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

using Kind = TimelineClip::Kind;

constexpr int kGutter = 128;   // track headers
constexpr int kRuler = 28;
constexpr int kNoteH = 40;
constexpr int kMinNoteH = 24, kMaxNoteH = 240;
constexpr int kVideoH = 54;
constexpr int kAudioH = 46;
constexpr int kDivider = 8;
constexpr double kEdge = 6.0;  // trim handle reach, px
constexpr double kSnapPx = 9.0;
constexpr double kMinPpf = 0.01, kMaxPpf = 48.0;
constexpr int kRowNote = 2, kRowVideo = 0, kRowAudio = 1;

QFont uiFont(int px, bool bold = false)
{
    QFont f(QStringLiteral("Segoe UI"));
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

QFont monoFont(int px)
{
    QFont f(QStringLiteral("Cascadia Mono"));
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(px);
    return f;
}

QString timecodeOf(qint64 frame, double fps)
{
    const int f = std::max(1, int(std::round(fps)));
    const qint64 secs = frame / f;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(secs / 3600, 2, 10, QChar('0'))
        .arg((secs / 60) % 60, 2, 10, QChar('0'))
        .arg(secs % 60, 2, 10, QChar('0'))
        .arg(int(frame % f), 2, 10, QChar('0'));
}

QString shortTimecode(qint64 frame, double fps)
{
    const int f = std::max(1, int(std::round(fps)));
    const qint64 secs = frame / f;
    return QStringLiteral("%1:%2:%3").arg(secs / 60, 2, 10, QChar('0')).arg(secs % 60, 2, 10, QChar('0')).arg(int(frame % f), 2, 10, QChar('0'));
}

QColor videoColor(const TimelineClip& c, bool offline)
{
    if (offline) return QColor(0x5a, 0x3a, 0x38);
    if (c.color.isValid()) return c.color.darker(135);
    const int hue = 200 + int(qHash(c.sourceId) % 50u);  // blues, a little different per source
    return QColor::fromHsv(hue, 80, 122);
}

QColor audioColor(const TimelineClip& c)
{
    if (c.color.isValid()) return c.color.darker(150);
    const int hue = 135 + int(qHash(c.sourceId) % 25u);
    return QColor::fromHsv(hue, 75, 92);
}

// Gain line: -40 dB at the bottom, +12 dB at the top of an audio clip.
double gainToY(double db, const QRectF& r) { return r.bottom() - 3 - (std::clamp(db, -40.0, 12.0) + 40.0) / 52.0 * (r.height() - 6); }
double yToGain(double y, const QRectF& r) { return std::clamp((r.bottom() - 3 - y) / std::max(1.0, r.height() - 6) * 52.0 - 40.0, -40.0, 12.0); }

// Source ids from a drag that started in the project tree (QTreeWidget's standard item payload;
// role Qt::UserRole holds the id and Qt::UserRole + 1 the item kind).
QStringList sourceIdsFrom(const QMimeData* mime)
{
    QStringList ids;
    if (!mime->hasFormat(QStringLiteral("application/x-qabstractitemmodeldatalist"))) return ids;
    QByteArray encoded = mime->data(QStringLiteral("application/x-qabstractitemmodeldatalist"));
    QDataStream stream(&encoded, QIODevice::ReadOnly);
    while (!stream.atEnd()) {
        int row = 0, col = 0;
        QMap<int, QVariant> roles;
        stream >> row >> col >> roles;
        if (stream.status() != QDataStream::Ok) break;
        if (roles.value(Qt::UserRole + 1).toString() == QLatin1String("source")) {
            const QString id = roles.value(Qt::UserRole).toString();
            if (!id.isEmpty() && !ids.contains(id)) ids << id;
        }
    }
    return ids;
}

} // namespace

// ============================================================================ canvas

class TimelineCanvas final : public QWidget {
public:
    explicit TimelineCanvas(TimelineEditor* editor)
        : QWidget(editor)
        , ed(editor)
    {
        setObjectName("TimelineCanvas");
        setMouseTracking(true);
        setAcceptDrops(true);
        setFocusPolicy(Qt::ClickFocus);
        setMinimumHeight(kRuler + kNoteH + kVideoH + kDivider + kAudioH + 8);
        setAttribute(Qt::WA_OpaquePaintEvent);
        const QVariantList heights = QSettings().value("timeline/noteLaneHeights").toList();
        for (int lane = 0; lane < std::min(256, int(heights.size())); ++lane)
            noteHeights[lane] = std::clamp(heights[lane].toInt(), kMinNoteH, kMaxNoteH);
    }

    // ------------------------------------------------------------------ state
    TimelineEditor* ed;
    TimelineDoc doc;   // committed
    TimelineDoc live;  // preview while dragging
    QHash<QString, TimelineEditor::SourceInfo> sources;
    qint64 current = -1;
    bool loop = false;
    qint64 loopIn = 0, loopOut = 0;
    QBitArray cache;
    double ppf = 4.0;    // pixels per frame
    double scroll = 0;   // first visible frame
    double vscroll = 0;  // px
    bool snap = true;
    bool ripple = false;
    bool linked = true;
    bool timecode = false;
    TimelineEditor::Tool tool = TimelineEditor::Tool::Select;
    std::function<QImage(const QString&, qint64)> thumbs;
    QHash<QString, QImage> thumbCache;
    QHash<int, int> noteHeights;
    int resizeLane = -1, resizeStartHeight = kNoteH;
    int noteHeight(int lane) const { return noteHeights.value(lane, kNoteH); }
    void saveNoteHeights() const {
        QVariantList heights;
        int last = -1;
        for (auto it = noteHeights.begin(); it != noteHeights.end(); ++it) last = std::max(last, it.key());
        for (int lane = 0; lane <= last; ++lane) heights << noteHeight(lane);
        QSettings().setValue("timeline/noteLaneHeights", heights);
    }

    QSet<QString> selected;
    QString selNoteSource, selNoteKey;
    int gapKind = -1, gapTrack = -1;
    qint64 gapStart = -1, gapEnd = -1;
    QVector<TimelineClip> clipboard;

    struct Row {
        int kind;  // kRowVideo, kRowAudio, kRowNote
        int track;
        double top;  // content px (before vertical scroll)
        double h;
    };
    struct NoteView {
        QString sourceId;
        NoteClip clip;
        qint64 start = 0, count = 0;  // global
        int visFirst = 0, visLast = 0;  // source frames shown
    };

    int shiftedNoteFrame(const QString& id, int frame, qint64 delta) const
    {
        const auto& src = sources[id];
        if (!src.timing.timed()) return int(frame + delta);
        const auto& t = src.timing;
        return int(std::clamp<int64_t>(t.info->frameAt(t.info->timeOf(frame) + delta / t.fps), 0, src.frames - 1));
    }
    qint64 noteDistance(const QString& id, int from, int to) const
    {
        const auto& t = sources[id].timing;
        return t.timed() ? qint64(std::llround((t.info->timeOf(to) - t.info->timeOf(from)) * t.fps)) : to - from;
    }

    enum class Drag { None, Scrub, Pan, Move, Trim, Gain, Fade, Marquee, NoteMove, NoteTrim, NoteResize };
    Drag drag = Drag::None;
    bool dragStarted = false;
    QPointF pressPos, lastMouse;
    Qt::KeyboardModifiers pressMods;
    QString pressUid;
    int pressKind = -1, pressTrack = -1;  // the row grabbed (rows change while a preview adds tracks)
    QSet<QString> dragIds;
    int trimEdge = 0;
    bool dragRipple = false;
    bool duplicate = false;
    qint64 snapAt = -1;
    qint64 insertAt = -1;
    int fadeSide = 0;
    double gainStart = 0;
    QRectF marquee;
    int noteIndex = -1;
    qint64 noteDelta = 0;
    int noteLane = -1;
    int noteEdge = 0;
    qint64 dropFrame = -1;
    int dropRow = -1;
    qint64 bladeAt = -1;
    mutable QVector<NoteView> notesCache;
    mutable bool notesDirty = true;
    mutable QVector<Row> rowsCache;
    mutable bool rowsDirty = true;
    void invalidate()
    {
        notesDirty = true;
        rowsDirty = true;
    }

    tl::LengthFn len() const
    {
        return [this](const QString& id) -> SourceTiming {
            auto it = sources.find(id);
            if (it == sources.end()) return SourceTiming{};
            return it->timing.timed() ? it->timing : SourceTiming(it->frames);
        };
    }
    tl::OnlineFn online() const
    {
        return [this](const QString& id) {
            auto it = sources.find(id);
            return it != sources.end() && it->online;
        };
    }
    bool previewing() const { return dragStarted && (drag == Drag::Move || drag == Drag::Trim || drag == Drag::Gain || drag == Drag::Fade); }
    const TimelineDoc& shown() const { return previewing() ? live : doc; }

    // ------------------------------------------------------------------ geometry
    int noteLanes() const
    {
        int lanes = std::max<int>(1, int(doc.noteTracks.size()));
        for (const NoteView& n : notes()) lanes = std::max(lanes, n.clip.lane + 1);
        if (drag == Drag::NoteMove && noteLane >= 0) lanes = std::max(lanes, noteLane + 1);
        return lanes;
    }
    const QVector<Row>& rows() const
    {
        if (rowsDirty) {
            rowsCache = buildRows();
            rowsDirty = false;
        }
        return rowsCache;
    }
    QVector<Row> buildRows() const
    {
        QVector<Row> out;
        const TimelineDoc& d = shown();
        double y = 0;
        for (int l = noteLanes() - 1; l >= 0; --l) {
            const int height = noteHeight(l);
            out.append({ kRowNote, l, y, double(height) });
            y += height;
        }
        y += 4;
        for (int t = int(d.videoTracks.size()) - 1; t >= 0; --t) {
            out.append({ kRowVideo, t, y, double(kVideoH) });
            y += kVideoH;
        }
        y += kDivider;
        for (int t = 0; t < d.audioTracks.size(); ++t) {
            out.append({ kRowAudio, t, y, double(kAudioH) });
            y += kAudioH;
        }
        return out;
    }
    double contentHeight() const
    {
        const QVector<Row> r = rows();
        return r.isEmpty() ? 0 : r.back().top + r.back().h + 24;
    }
    double rowY(const Row& r) const { return kRuler + r.top - vscroll; }
    int resizeLaneAt(const QPointF& pos) const {
        if (pos.y() < kRuler + 4) return -1;
        for (const Row& r : rows())
            if (r.kind == kRowNote && std::abs(pos.y() - (rowY(r) + r.h)) <= 3) return r.track;
        return -1;
    }
    QRectF rowRect(const Row& r) const { return QRectF(kGutter, rowY(r), width() - kGutter, r.h); }
    int rowIndexAt(double y) const
    {
        const QVector<Row> rs = rows();
        for (int i = 0; i < rs.size(); ++i)
            if (y >= rowY(rs[i]) && y < rowY(rs[i]) + rs[i].h) return i;
        return -1;
    }
    int rowOf(int kind, int track) const
    {
        const QVector<Row> rs = rows();
        for (int i = 0; i < rs.size(); ++i)
            if (rs[i].kind == kind && rs[i].track == track) return i;
        return -1;
    }
    double xFor(double frame) const { return kGutter + (frame - scroll) * ppf; }
    double frameAtX(double x) const { return scroll + (x - kGutter) / ppf; }
    double visibleFrames() const { return std::max(1.0, (width() - kGutter) / ppf); }
    qint64 total() const { return std::max<qint64>(1, tl::duration(shown(), len())); }
    qint64 clampFrame(double f) const { return std::clamp<qint64>(qint64(std::floor(f)), 0, std::max<qint64>(0, total() - 1)); }

    QRectF clipRect(const TimelineClip& c) const
    {
        const int ri = rowOf(c.isVideo() ? kRowVideo : kRowAudio, c.track);
        if (ri < 0) return {};
        const Row r = rows()[ri];
        const tl::Span s = tl::span(c, len());
        return QRectF(xFor(s.start), rowY(r) + 2, std::max(1.0, s.count * ppf), r.h - 4);
    }

    // Annotation clips where their source is the edited picture (the primary of each frame).
    QVector<NoteView> computeNotes(const TimelineDoc& d) const
    {
        QVector<NoteView> out;
        const Sequence seq = tl::primary(d, len(), online());
        for (const Sequence::Segment& seg : seq.segments) {
            if (seg.isGap()) continue;
            auto it = sources.find(seg.sourceId);
            if (it == sources.end()) continue;
            for (const NoteClip& nc : it->noteClips) {
                const int vf = int(std::max<int64_t>(nc.first, seg.offset));
                const int vl = int(std::min<int64_t>(nc.last, seg.sourceFrame(seg.count - 1)));
                if (vf > vl) continue;
                const qint64 first = Sequence::globalIn(seg, vf), last = Sequence::globalIn(seg, vl);
                if (first >= 0 && last >= first) out.append({ seg.sourceId, nc, first, last - first + 1, vf, vl });
            }
        }
        return out;
    }
    const QVector<NoteView>& notes() const
    {
        if (notesDirty) {
            notesCache = computeNotes(shown());
            notesDirty = false;
        }
        return notesCache;
    }
    QRectF noteRect(const NoteView& n, qint64 delta = 0, int lane = -1, qint64 grow0 = 0, qint64 grow1 = 0) const
    {
        const int ri = rowOf(kRowNote, lane >= 0 ? lane : n.clip.lane);
        if (ri < 0) return {};
        const Row r = rows()[ri];
        const double x0 = xFor(n.start + delta + grow0), x1 = xFor(n.start + n.count + delta + grow1);
        return QRectF(x0, rowY(r) + 2, std::max(2.0, x1 - x0), r.h - 4);
    }

    // ------------------------------------------------------------------ hit testing
    struct Hit {
        enum What { Nothing, Ruler, Header, Clip, Note, Empty } what = Nothing;
        int row = -1;
        QString uid;
        int edge = 0;    // -1 head, +1 tail
        int handle = 0;  // 1 gain line, 2 fade in, 3 fade out
        int note = -1;
        int button = -1;
    };
    int buttonCount(int kind) const { return kind == kRowAudio ? 3 : 2; }
    QRectF buttonRect(const Row& r, int i) const
    {
        const int n = buttonCount(r.kind);
        const double size = r.kind == kRowNote ? 18 : 20;
        return QRectF(kGutter - 6 - (n - i) * (size + 2), rowY(r) + (r.h - size) / 2, size, size);
    }
    Hit hit(const QPointF& p) const
    {
        Hit h;
        if (p.y() < kRuler) {
            h.what = p.x() >= kGutter ? Hit::Ruler : Hit::Nothing;
            return h;
        }
        h.row = rowIndexAt(p.y());
        if (h.row < 0) {
            h.what = p.x() >= kGutter ? Hit::Empty : Hit::Nothing;
            return h;
        }
        const Row r = rows()[h.row];
        if (p.x() < kGutter) {
            h.what = Hit::Header;
            for (int i = 0; i < buttonCount(r.kind); ++i)
                if (buttonRect(r, i).contains(p)) h.button = i;
            return h;
        }
        h.what = Hit::Empty;
        if (r.kind == kRowNote) {
            const QVector<NoteView>& ns = notes();
            // Inside a block first (blocks of a flipbook touch), then edges just outside one.
            for (int pass = 0; pass < 2; ++pass)
                for (int i = int(ns.size()) - 1; i >= 0; --i) {
                    if (ns[i].clip.lane != r.track) continue;
                    const QRectF rc = noteRect(ns[i]);
                    const bool inside = p.x() >= rc.left() && p.x() <= rc.right();
                    if (pass == 0 && !inside) continue;
                    if (pass == 1 && (p.x() < rc.left() - kEdge || p.x() > rc.right() + kEdge)) continue;
                    const double reach = std::min(kEdge, rc.width() / 3);
                    if (pass == 1) h.edge = p.x() < rc.left() ? -1 : 1;
                    else if (p.x() - rc.left() <= reach) h.edge = -1;
                    else if (rc.right() - p.x() <= reach) h.edge = 1;
                    h.what = Hit::Note;
                    h.note = i;
                    return h;
                }
            return h;
        }
        const Kind kind = r.kind == kRowVideo ? Kind::Video : Kind::Audio;
        const TimelineDoc& d = shown();
        // The clip the pointer is in wins (two clips that touch share an edge); then edges just outside.
        for (int pass = 0; pass < 2; ++pass)
            for (const TimelineClip& c : d.clips) {
                if (c.kind != kind || c.track != r.track) continue;
                const QRectF rc = clipRect(c);
                const bool inside = p.x() >= rc.left() && p.x() <= rc.right();
                if (pass == 0 && !inside) continue;
                if (pass == 1 && (p.x() < rc.left() - kEdge || p.x() > rc.right() + kEdge)) continue;
                const double reach = std::min(kEdge, rc.width() / 3);
                int edge = 0;
                if (pass == 1) edge = p.x() < rc.left() ? -1 : 1;
                else if (p.x() - rc.left() <= reach) edge = -1;
                else if (rc.right() - p.x() <= reach) edge = 1;
                h.what = Hit::Clip;
                h.uid = c.uid;
                h.edge = edge;
                if (c.isAudio() && edge == 0) {
                    if (p.y() < rc.top() + 9 && p.x() < rc.left() + std::max(10.0, c.fadeIn * ppf) + 5) h.handle = 2;
                    else if (p.y() < rc.top() + 9 && p.x() > rc.right() - std::max(10.0, c.fadeOut * ppf) - 5) h.handle = 3;
                    else if (std::abs(p.y() - gainToY(c.gainDb, rc)) <= 4) h.handle = 1;
                }
                return h;
            }
        return h;
    }

    // ------------------------------------------------------------------ helpers
    const TimelineClip* clipOf(const QString& uid) const { return doc.clip(uid); }
    bool locked(const TimelineClip& c) const { return tl::trackLocked(doc, c.kind, c.track); }
    QSet<QString> expand(const QSet<QString>& ids) const { return linked ? tl::withLinks(doc, ids) : ids; }
    void select(const QSet<QString>& ids, bool notify = true)
    {
        selected = ids;
        selNoteKey.clear();
        selNoteSource.clear();
        gapKind = -1;
        update();
        if (notify) emit ed->selectionChanged(QStringList(selected.begin(), selected.end()));
    }
    void commit(const TimelineDoc& next, const QString& what, QStringList select = {})
    {
        if (next.sameLayout(doc)) return;
        emit ed->edited(next, what, select);
    }

    QVector<qint64> snapPoints(const QSet<QString>& exclude, const QString& excludeNote = QString()) const
    {
        QVector<qint64> pts{ 0 };
        if (current >= 0) pts << current;
        if (loop) pts << loopIn << loopOut + 1;
        for (const TimelineClip& c : doc.clips) {
            if (exclude.contains(c.uid)) continue;
            const tl::Span s = tl::span(c, len());
            pts << s.start << s.end();
        }
        for (const NoteView& n : notesCache)
            if (n.clip.key != excludeNote) pts << n.start << n.start + n.count;
        return pts;
    }
    // Adjusts `delta` so one of `edges` (+ delta) lands on a snap point within reach.
    qint64 snapped(const QVector<qint64>& edges, qint64 delta, const QVector<qint64>& points)
    {
        snapAt = -1;
        if (!snap) return delta;
        qint64 best = 0;
        double bestPx = kSnapPx;
        for (qint64 e : edges)
            for (qint64 p : points) {
                const qint64 d = p - (e + delta);
                const double px = std::abs(double(d)) * ppf;
                if (px < bestPx) {
                    bestPx = px;
                    best = d;
                    snapAt = p;
                }
            }
        return delta + best;
    }
    QVector<qint64> snapCache;

    void setZoom(double next, double anchorX)
    {
        next = std::clamp(next, kMinPpf, kMaxPpf);
        const double anchorFrame = frameAtX(anchorX);
        ppf = next;
        scroll = anchorFrame - (anchorX - kGutter) / ppf;
        clampScroll();
        ed->updateZoomUi();
        update();
    }
    void clampScroll()
    {
        const double maxScroll = std::max(0.0, double(total()) + 10 - visibleFrames() * 0.5);
        scroll = std::clamp(scroll, 0.0, maxScroll);
        vscroll = std::clamp(vscroll, 0.0, std::max(0.0, contentHeight() - (height() - kRuler)));
    }
    void ensureVisible(qint64 frame)
    {
        if (frame < 0 || drag != Drag::None) return;
        const double margin = visibleFrames() * 0.08;
        if (frame < scroll + 0.5) scroll = std::max(0.0, frame - margin);
        else if (frame > scroll + visibleFrames() - 1) scroll = frame - visibleFrames() + margin;
        clampScroll();
        ed->updateZoomUi();
    }
    qint64 rulerStep() const
    {
        static const qint64 steps[] = { 1, 2, 5, 10, 12, 24, 25, 30, 48, 50, 60, 100, 120, 240, 250, 300, 500, 600,
                                        1000, 1200, 2400, 3000, 6000, 12000, 24000, 60000 };
        const double minPx = timecode ? 84.0 : 56.0;
        const double fps = std::max(1.0, doc.fps);
        for (qint64 s : steps) {
            if (timecode && s >= 24 && s % std::max<qint64>(1, qint64(std::round(fps))) != 0 && s < 1000) continue;
            if (s * ppf >= minPx) return s;
        }
        return 120000;
    }
    QImage thumbnail(const QString& sourceId, qint64 frame)
    {
        if (!thumbs) return {};
        const QString key = sourceId + ':' + QString::number(frame);
        auto it = thumbCache.find(key);
        if (it != thumbCache.end()) return *it;
        const QImage img = thumbs(sourceId, frame);
        if (!img.isNull()) thumbCache.insert(key, img);
        return img;
    }
    QString nameOf(const TimelineClip& c) const
    {
        if (!c.name.isEmpty()) return c.name;
        auto it = sources.find(c.sourceId);
        return it == sources.end() ? c.sourceId : it->name;
    }

    // ------------------------------------------------------------------ painting
    void paintHeader(QPainter& p, const Row& r)
    {
        const QRectF box(0, rowY(r), kGutter, r.h);
        const bool lockedRow = r.kind == kRowNote ? doc.noteTracks.value(r.track).locked
                                                  : tl::trackLocked(doc, r.kind == kRowVideo ? Kind::Video : Kind::Audio, r.track);
        p.fillRect(box, r.kind == kRowNote ? QColor(0x23, 0x22, 0x26) : r.kind == kRowVideo ? QColor(0x22, 0x24, 0x28) : QColor(0x21, 0x26, 0x23));
        p.setPen(QPen(Pal::divider, 1));
        p.drawLine(QPointF(0, box.bottom() - 0.5), QPointF(kGutter, box.bottom() - 0.5));
        QString name;
        if (r.kind == kRowNote) {
            const QString custom = doc.noteTracks.value(r.track).name;
            name = custom.isEmpty() ? QStringLiteral("N%1").arg(r.track + 1) : custom;
        } else {
            name = tl::trackName(doc, r.kind == kRowVideo ? Kind::Video : Kind::Audio, r.track);
        }
        p.setFont(uiFont(r.kind == kRowNote ? 10 : 11, true));
        p.setPen(lockedRow ? Pal::textFaint : Pal::text);
        const QRectF nameBox(10, box.top(), kGutter - 10 - buttonCount(r.kind) * 22 - 6, box.height());
        p.drawText(nameBox, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(name, Qt::ElideRight, int(nameBox.width())));
        // switches
        for (int i = 0; i < buttonCount(r.kind); ++i) {
            const QRectF b = buttonRect(r, i);
            TrackState st = r.kind == kRowNote ? doc.noteTracks.value(r.track) : doc.tracks(r.kind == kRowVideo ? Kind::Video : Kind::Audio).value(r.track);
            bool on = false;
            QString icon, text;
            QColor tint = Pal::text;
            if (r.kind == kRowAudio && i == 0) {
                text = QStringLiteral("M");
                on = !st.visible;
                tint = QColor(0xe5, 0x6b, 0x5d);
            } else if (r.kind == kRowAudio && i == 1) {
                text = QStringLiteral("S");
                on = st.solo;
                tint = QColor(0xe8, 0xc3, 0x4a);
            } else if (i == 0) {
                icon = st.visible ? "eye" : "eye-off";
                on = !st.visible;
            } else {
                icon = st.locked ? "lock" : "unlock";
                on = st.locked;
            }
            p.setPen(Qt::NoPen);
            p.setBrush(on ? QColor(tint.red(), tint.green(), tint.blue(), 70) : QColor(255, 255, 255, 14));
            p.drawRoundedRect(b, 4, 4);
            if (!text.isEmpty()) {
                p.setFont(uiFont(11, true));
                p.setPen(on ? tint : Pal::textDim);
                p.drawText(b, Qt::AlignCenter, text);
            } else {
                const int px = int(b.height()) - 4;
                p.drawPixmap(QPointF(b.center().x() - px / 2.0, b.center().y() - px / 2.0),
                             Icons::pixmap(icon, on ? Pal::textStrong : Pal::textDim, px, devicePixelRatioF()));
            }
        }
        if (r.kind == kRowNote) {
            p.setPen(QPen(Pal::textDim, 1));
            p.drawLine(QPointF(10, box.bottom() - 5), QPointF(30, box.bottom() - 5));
            p.drawLine(QPointF(10, box.bottom() - 3), QPointF(30, box.bottom() - 3));
        }
    }

    void paintClip(QPainter& p, const TimelineClip& c, const TimelineDoc& d)
    {
        QRectF r = clipRect(c).adjusted(0.5, 0.5, -0.5, -0.5);
        if (r.isEmpty() || r.right() < kGutter || r.left() > width()) return;
        auto src = sources.find(c.sourceId);
        const bool offline = src == sources.end() || !src->online;
        const bool sel = selected.contains(c.uid);
        const tl::Span s = tl::span(c, len());
        QPainterPath path;
        path.addRoundedRect(r, 4, 4);
        QColor base = c.isVideo() ? videoColor(c, offline) : audioColor(c);
        if (sel) base = base.lighter(125);
        p.save();
        p.setClipPath(path);
        p.fillPath(path, base);
        if (!c.enabled || (c.isAudio() && c.muted)) {
            p.fillRect(r, QColor(20, 20, 20, 120));
            p.setPen(QPen(QColor(255, 255, 255, 25), 1));
            for (double x = r.left() - r.height(); x < r.right(); x += 8) p.drawLine(QPointF(x, r.bottom()), QPointF(x + r.height(), r.top()));
        }
        if (c.isVideo()) {
            if (r.width() > 46 && !offline) {
                const QImage thumb = thumbnail(c.sourceId, s.in);
                if (!thumb.isNull()) {
                    const double th = r.height() - 4;
                    const double tw = std::min(r.width() * 0.45, th * thumb.width() / std::max(1, thumb.height()));
                    p.setOpacity(c.enabled ? 1.0 : 0.4);
                    p.drawImage(QRectF(r.left() + 2, r.top() + 2, tw, th), thumb);
                    p.setOpacity(1.0);
                }
            }
            // marks: review keys (red), bookmarks, notes
            if (src != sources.end()) {
                auto global = [&](int f) -> qint64 {
                    if (f < s.in || f > s.last()) return -1;
                    return s.start + (s.timing.timed() ? std::max<qint64>(0, qint64(std::ceil((s.timing.info->timeOf(f) - s.timeStart) * s.timing.fps - 1e-6))) : f - s.in);
                };
                p.setPen(Qt::NoPen);
                p.setBrush(Pal::review);
                for (int k : src->keys)
                    if (const qint64 g = global(k); g >= 0) p.drawRect(QRectF(xFor(g) - 1, r.bottom() - 6, 2, 6));
                p.setBrush(Pal::textStrong);
                for (int b : src->bookmarks)
                    if (const qint64 g = global(b); g >= 0) {
                        const double x = xFor(g);
                        p.drawPolygon(QPolygonF{ QPointF(x - 3.5, r.top()), QPointF(x + 3.5, r.top()), QPointF(x, r.top() + 5) });
                    }
                p.setBrush(QColor(0xf2, 0xc9, 0x4c));
                for (int n : src->notes)
                    if (const qint64 g = global(n); g >= 0) p.drawEllipse(QPointF(xFor(g), r.bottom() - 11), 2.5, 2.5);
            }
        } else if (src != sources.end() && !src->peaks.isEmpty()) {
            // waveform, scaled by the clip's gain
            const double gain = c.muted ? 0.0 : std::min(2.0, dbToGain(c.gainDb));
            const QRectF wave = r.adjusted(1, 12, -1, -2);
            const double mid = wave.center().y();
            p.setPen(QPen(c.muted ? QColor(150, 150, 150, 120) : QColor(0xa8, 0xe0, 0xb4, 210), 1));
            const int x0 = int(std::max(wave.left(), double(kGutter))), x1 = int(std::min(wave.right(), double(width())));
            const int bins = int(src->peaks.size());
            for (int x = x0; x <= x1; ++x) {
                const double f0 = frameAtX(x) - s.start + s.in, f1 = frameAtX(x + 1) - s.start + s.in;
                auto sourceBin = [&](double f) {
                    if (!s.timing.timed()) return f * TimelineEditor::kPeakBins;
                    const double t = s.timeStart + (f - s.in) / s.timing.fps;
                    const int64_t k = s.timing.info->frameAt(t);
                    const double a = s.timing.info->timeOf(k), b = s.timing.info->timeOf(k + 1);
                    return (k + (b > a ? (t - a) / (b - a) : 0)) * TimelineEditor::kPeakBins;
                };
                const int b0 = std::clamp(int(std::floor(sourceBin(f0))), 0, bins - 1);
                const int b1 = std::clamp(int(std::ceil(sourceBin(f1))), b0 + 1, bins);
                float peak = 0;
                for (int b = b0; b < b1; ++b) peak = std::max(peak, src->peaks[b]);
                if (peak <= 0.003f) continue;
                const double h = std::min(wave.height() / 2, std::max(0.5, peak * gain * (wave.height() / 2 - 1)));
                p.drawLine(QPointF(x + 0.5, mid - h), QPointF(x + 0.5, mid + h));
            }
            // fades
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 90));
            if (c.fadeIn > 0)
                p.drawPolygon(QPolygonF{ r.topLeft(), QPointF(r.left() + c.fadeIn * ppf, r.top()), r.bottomLeft() });
            if (c.fadeOut > 0)
                p.drawPolygon(QPolygonF{ r.topRight(), QPointF(r.right() - c.fadeOut * ppf, r.top()), r.bottomRight() });
            // gain line
            const double gy = gainToY(c.gainDb, r);
            p.setPen(QPen(QColor(255, 236, 160, sel ? 230 : 150), 1.2));
            p.drawLine(QPointF(r.left() + c.fadeIn * ppf, gy), QPointF(r.right() - c.fadeOut * ppf, gy));
            if (sel) {
                p.setBrush(QColor(255, 255, 255, 200));
                p.setPen(Qt::NoPen);
                p.drawRect(QRectF(r.left() + std::max(0.0, c.fadeIn * ppf) - 3, r.top(), 6, 6));
                p.drawRect(QRectF(r.right() - std::max(0.0, c.fadeOut * ppf) - 3, r.top(), 6, 6));
            }
        }
        // label
        if (r.width() > 26) {
            const QRectF text = r.adjusted(c.isVideo() && r.width() > 46 ? std::min(r.width() * 0.45, (r.height() - 4) * 16.0 / 9.0) + 8 : 6, 2, -6, -2);
            p.setFont(uiFont(11, sel));
            p.setPen(offline ? QColor(0xff, 0xb0, 0xa8) : Pal::textStrong);
            QString title = nameOf(c);
            if (offline) title = L("Media Offline · ", "미디어 없음 · ") + title;
            const QFontMetrics fm(p.font());
            double x = text.left();
            if (!c.link.isEmpty() && text.width() > 30) {
                p.drawPixmap(QPointF(x, text.top() + 2), Icons::pixmap("link", QColor(255, 255, 255, 170), 11, devicePixelRatioF()));
                x += 14;
            }
            p.drawText(QRectF(x, text.top(), text.right() - x, 14), Qt::AlignLeft | Qt::AlignTop, fm.elidedText(title, Qt::ElideMiddle, int(text.right() - x)));
            QStringList badges;
            if (c.isVideo()) {
                badges << QStringLiteral("%1–%2").arg(s.in).arg(s.last());
                if (c.opacity < 1.0) badges << QStringLiteral("%1%").arg(qRound(c.opacity * 100));
                if (c.blend != BlendMode::Normal) {
                    static const char* names[] = { "", "Add", "Screen", "Multiply", "Difference" };
                    badges << QLatin1String(names[int(c.blend)]);
                }
                if (c.hasTransform()) badges << QStringLiteral("PiP %1%").arg(qRound(c.scale * 100));
            } else {
                if (c.gainDb != 0.0) badges << QStringLiteral("%1%2 dB").arg(c.gainDb > 0 ? "+" : "").arg(c.gainDb, 0, 'f', 1);
                if (c.muted) badges << L("muted", "음소거");
            }
            if (!c.enabled) badges << L("off", "꺼짐");
            if (text.height() > 28 && !badges.isEmpty()) {
                p.setFont(monoFont(10));
                p.setPen(QColor(255, 255, 255, 165));
                p.drawText(QRectF(text.left(), text.top() + 15, text.width(), 14), Qt::AlignLeft | Qt::AlignTop,
                           QFontMetrics(p.font()).elidedText(badges.join(QStringLiteral(" · ")), Qt::ElideRight, int(text.width())));
            }
        }
        p.restore();
        // outline + trim grips
        p.setBrush(Qt::NoBrush);
        p.setPen(sel ? QPen(Pal::accent, 2) : QPen(QColor(0, 0, 0, 130), 1));
        p.drawPath(path);
        if (sel && r.width() > 18) {
            p.setPen(Qt::NoPen);
            p.setBrush(Pal::accent);
            p.drawRoundedRect(QRectF(r.left(), r.top() + r.height() * 0.25, 3, r.height() * 0.5), 1.5, 1.5);
            p.drawRoundedRect(QRectF(r.right() - 3, r.top() + r.height() * 0.25, 3, r.height() * 0.5), 1.5, 1.5);
        }
        Q_UNUSED(d);
    }

    void paintNote(QPainter& p, const NoteView& n, int index)
    {
        qint64 delta = 0, g0 = 0, g1 = 0;
        int lane = -1;
        const bool dragging = dragStarted && noteIndex == index;
        if (dragging && drag == Drag::NoteMove && !duplicate) {
            delta = noteDelta;
            lane = noteLane;
        }
        if (dragging && drag == Drag::NoteTrim) (noteEdge < 0 ? g0 : g1) = noteDelta;
        QRectF r = noteRect(n, delta, lane, g0, g1).adjusted(0.5, 0.5, -0.5, -0.5);
        if (r.isEmpty() || r.right() < kGutter || r.left() > width()) return;
        const bool sel = n.sourceId == selNoteSource && n.clip.key == selNoteKey;
        QColor c = n.clip.color.isValid() ? n.clip.color : QColor(0xe5, 0x48, 0x3e);
        QColor fill = c;
        fill.setAlpha(sel ? 235 : 190);
        QPainterPath path;
        path.addRoundedRect(r, 3, 3);
        p.fillPath(path, fill.darker(sel ? 105 : 130));
        p.save();
        p.setClipPath(path);
        const bool light = c.lightnessF() > 0.62;
        const QColor ink = light ? QColor(20, 20, 20) : QColor(250, 250, 250);
        double x = r.left() + 4;
        if (r.width() > 18) {
            p.drawPixmap(QPointF(x, r.center().y() - 7), Icons::pixmap(n.clip.icon, ink, 14, devicePixelRatioF()));
            x += 17;
        }
        if (r.right() - x > 16) {
            p.setFont(uiFont(10, sel));
            p.setPen(ink);
            QString label = n.clip.label;
            if (n.clip.tracked) label = QStringLiteral("◎ ") + label;
            p.drawText(QRectF(x, r.top(), r.right() - x - 3, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       QFontMetrics(p.font()).elidedText(label, Qt::ElideRight, int(r.right() - x - 3)));
        }
        if (n.clip.locked && r.width() > 40)
            p.drawPixmap(QPointF(r.right() - 15, r.center().y() - 6), Icons::pixmap("lock", ink, 12, devicePixelRatioF()));
        p.restore();
        p.setBrush(Qt::NoBrush);
        p.setPen(sel ? QPen(Pal::accent, 2) : QPen(QColor(0, 0, 0, 120), 1));
        p.drawPath(path);
        // part of the clip is outside this instance: soft edge
        p.setPen(QPen(ink, 1, Qt::DotLine));
        if (n.visFirst > n.clip.first) p.drawLine(QPointF(r.left() + 1, r.top() + 3), QPointF(r.left() + 1, r.bottom() - 3));
        if (n.visLast < n.clip.last) p.drawLine(QPointF(r.right() - 1, r.top() + 3), QPointF(r.right() - 1, r.bottom() - 3));
        if (dragging && drag == Drag::NoteMove && duplicate) {
            const QRectF ghost = noteRect(n, noteDelta, noteLane);
            p.setPen(QPen(Pal::textStrong, 1, Qt::DashLine));
            p.setBrush(QColor(255, 255, 255, 30));
            p.drawRoundedRect(ghost, 3, 3);
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        if (drag != Drag::None) invalidate();  // the preview (live clips, lanes) changes while dragging
        QPainter p(this);
        p.fillRect(rect(), Pal::viewer);
        p.setRenderHint(QPainter::Antialiasing, true);
        const TimelineDoc& d = shown();
        const QVector<Row> rs = rows();
        const double fps = std::max(1.0, d.fps);

        // ---- rows
        p.save();
        p.setClipRect(QRectF(0, kRuler, width(), height() - kRuler));
        for (const Row& r : rs) {
            const QRectF rr = rowRect(r);
            if (rr.bottom() < kRuler || rr.top() > height()) continue;
            QColor bg = r.kind == kRowNote ? QColor(0x1d, 0x1c, 0x20) : r.kind == kRowVideo ? QColor(0x1b, 0x1c, 0x1f) : QColor(0x19, 0x1c, 0x1a);
            if (dropRow >= 0 && rs.value(dropRow).kind == r.kind && rs.value(dropRow).track == r.track) bg = bg.lighter(130);
            p.fillRect(rr, bg);
            const bool lockedRow = r.kind == kRowNote ? d.noteTracks.value(r.track).locked
                                                      : tl::trackLocked(d, r.kind == kRowVideo ? Kind::Video : Kind::Audio, r.track);
            if (lockedRow) {
                p.setPen(QPen(QColor(255, 255, 255, 12), 1));
                for (double x = rr.left() - rr.height(); x < rr.right(); x += 10) p.drawLine(QPointF(x, rr.bottom()), QPointF(x + rr.height(), rr.top()));
            }
            p.setPen(QPen(QColor(0, 0, 0, 90), 1));
            p.drawLine(QPointF(kGutter, rr.bottom() - 0.5), QPointF(width(), rr.bottom() - 0.5));
            // selected gap
            if (gapKind >= 0 && gapKind == r.kind && gapTrack == r.track && gapEnd > gapStart) {
                p.fillRect(QRectF(xFor(gapStart), rr.top() + 2, (gapEnd - gapStart) * ppf, rr.height() - 4), QColor(217, 119, 87, 50));
                p.setPen(QPen(Pal::accent, 1, Qt::DashLine));
                p.drawRect(QRectF(xFor(gapStart), rr.top() + 2, (gapEnd - gapStart) * ppf, rr.height() - 4));
            }
        }
        // divider between picture and sound
        if (const int a1 = rowOf(kRowAudio, 0); a1 >= 0) {
            const double y = rowY(rs[a1]) - kDivider;
            p.fillRect(QRectF(0, y, width(), kDivider), QColor(0x2c, 0x2b, 0x29));
        }
        p.setClipRect(QRectF(kGutter, kRuler, width() - kGutter, height() - kRuler));
        for (const TimelineClip& c : d.clips) paintClip(p, c, d);
        const QVector<NoteView>& ns = notes();
        for (int i = 0; i < ns.size(); ++i) paintNote(p, ns[i], i);
        if (d.clips.isEmpty()) {
            p.setPen(Pal::textFaint);
            p.setFont(uiFont(12));
            p.drawText(QRectF(kGutter, kRuler, width() - kGutter, height() - kRuler), Qt::AlignCenter,
                       L("Drag sources here from the Project panel, or drop media files.",
                         "프로젝트 패널의 소스나 미디어 파일을 여기로 끌어 놓으세요."));
        }
        // moving ghost outline of the original place
        if (drag == Drag::Move && dragStarted && !duplicate)
            for (const TimelineClip& c : doc.clips)
                if (dragIds.contains(c.uid)) {
                    p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::DashLine));
                    p.setBrush(Qt::NoBrush);
                    p.drawRoundedRect(clipRect(c), 4, 4);
                }
        // marquee
        if (drag == Drag::Marquee && dragStarted) {
            p.setPen(QPen(QColor(255, 255, 255, 170), 1, Qt::DashLine));
            p.setBrush(QColor(255, 255, 255, 18));
            p.drawRect(marquee);
        }
        p.restore();

        // ---- headers
        p.save();
        p.setClipRect(QRectF(0, kRuler, kGutter, height() - kRuler));
        for (const Row& r : rs) paintHeader(p, r);
        p.restore();

        // ---- ruler
        p.save();
        p.setClipRect(QRectF(kGutter, 0, width() - kGutter, height()));
        const QRectF ruler(kGutter, 0, width() - kGutter, kRuler);
        p.fillRect(ruler, Pal::window);
        if (loop && loopOut >= loopIn && current >= 0)
            p.fillRect(QRectF(xFor(loopIn), 0, std::max(2.0, (loopOut - loopIn + 1) * ppf), kRuler), Pal::accentSoft);
        const qint64 step = rulerStep();
        const qint64 first = std::max<qint64>(0, qint64(scroll) / step * step);
        const qint64 last = qint64(scroll + visibleFrames()) + step;
        p.setFont(monoFont(10));
        for (qint64 f = first; f <= std::max(last, first); f += step) {
            const double x = xFor(f);
            p.setPen(QPen(Pal::border, 1));
            p.drawLine(QPointF(x + 0.5, kRuler - 9), QPointF(x + 0.5, kRuler));
            p.setPen(Pal::textFaint);
            p.drawText(QPointF(x + 4, kRuler - 11), timecode ? shortTimecode(f, fps) : QString::number(f));
            if (step * ppf >= 40) {
                const qint64 minor = std::max<qint64>(1, step / 5);
                p.setPen(QPen(Pal::divider, 1));
                for (qint64 m = f + minor; m < f + step; m += minor) p.drawLine(QPointF(xFor(m) + 0.5, kRuler - 4), QPointF(xFor(m) + 0.5, kRuler));
            }
        }
        const qint64 tot = total();
        if (current >= 0 && cache.size() == tot && tot > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(Pal::cached);
            qint64 run = -1;
            const qint64 lo = std::max<qint64>(0, qint64(scroll)), hi = std::min<qint64>(tot, qint64(scroll + visibleFrames()) + 2);
            for (qint64 i = lo; i <= hi; ++i) {
                const bool on = i < hi && i < tot && cache.testBit(i);
                if (on && run < 0) run = i;
                if (!on && run >= 0) {
                    p.drawRect(QRectF(xFor(run), kRuler - 3, (i - run) * ppf, 2));
                    run = -1;
                }
            }
        }
        // loop edges, snap line, insertion, blade, drop marker
        if (loop && loopOut >= loopIn && current >= 0) {
            p.setPen(QPen(Pal::accent, 1, Qt::DashLine));
            for (double x : { xFor(loopIn), xFor(loopOut + 1) }) p.drawLine(QPointF(x, kRuler), QPointF(x, height()));
        }
        if (snapAt >= 0 && dragStarted) {
            p.setPen(QPen(QColor(0xf2, 0xd2, 0x4c), 1.5));
            p.drawLine(QPointF(xFor(snapAt), 0), QPointF(xFor(snapAt), height()));
        }
        if (insertAt >= 0 && drag == Drag::Move && dragStarted) {
            p.setPen(QPen(Pal::accent, 3));
            p.drawLine(QPointF(xFor(insertAt), kRuler), QPointF(xFor(insertAt), height()));
        }
        if (bladeAt >= 0 && tool == TimelineEditor::Tool::Blade) {
            p.setPen(QPen(QColor(255, 255, 255, 220), 1, Qt::DashLine));
            p.drawLine(QPointF(xFor(bladeAt), kRuler), QPointF(xFor(bladeAt), height()));
        }
        if (dropFrame >= 0) {
            p.setPen(QPen(Pal::accent, 2));
            p.drawLine(QPointF(xFor(dropFrame), kRuler), QPointF(xFor(dropFrame), height()));
        }
        // playhead
        if (current >= 0) {
            const double x = xFor(current + 0.5);
            p.setPen(QPen(QColor(0xe5, 0x48, 0x3e), 1.5));
            p.drawLine(QPointF(x, kRuler - 2), QPointF(x, height()));
            QPolygonF head{ QPointF(x - 6, 2), QPointF(x + 6, 2), QPointF(x + 6, kRuler - 10), QPointF(x, kRuler - 4), QPointF(x - 6, kRuler - 10) };
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xe5, 0x48, 0x3e));
            p.drawPolygon(head);
        }
        p.restore();

        // ---- corner readout
        p.fillRect(QRectF(0, 0, kGutter, kRuler), Pal::window);
        p.setFont(monoFont(11));
        p.setPen(Pal::textStrong);
        if (current >= 0) p.drawText(QRectF(8, 0, kGutter - 12, kRuler), Qt::AlignVCenter | Qt::AlignLeft, timecode ? timecodeOf(current, fps) : QString::number(current));
        p.setPen(QPen(Pal::divider, 1));
        p.drawLine(QPointF(kGutter - 0.5, 0), QPointF(kGutter - 0.5, height()));
        p.drawLine(QPointF(0, kRuler - 0.5), QPointF(width(), kRuler - 0.5));
    }

    // ------------------------------------------------------------------ mouse
    void mousePressEvent(QMouseEvent* e) override
    {
        setFocus(Qt::MouseFocusReason);
        lastMouse = pressPos = e->position();
        pressMods = e->modifiers();
        dragStarted = false;
        snapAt = insertAt = -1;
        if (e->button() == Qt::LeftButton) {
            resizeLane = resizeLaneAt(e->position());
            if (resizeLane >= 0) {
                resizeStartHeight = noteHeight(resizeLane);
                drag = Drag::NoteResize;
                setCursor(Qt::SizeVerCursor);
                return;
            }
        }
        if (e->button() == Qt::MiddleButton) {
            drag = Drag::Pan;
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        const Hit h = hit(e->position());
        const qint64 frame = clampFrame(frameAtX(e->position().x()));
        if (e->button() == Qt::RightButton) {
            if (h.what == Hit::Header) {
                const Row r = rows()[h.row];
                emit ed->trackMenuRequested(r.kind, r.track, e->globalPosition().toPoint());
            } else if (h.what == Hit::Note) {
                const NoteView& n = notes()[h.note];
                selectNote(n);
                emit ed->noteMenuRequested(n.sourceId, n.clip.key, frame, e->globalPosition().toPoint());
            } else {
                if (h.what == Hit::Clip && !selected.contains(h.uid)) select(expand({ h.uid }));
                emit ed->contextMenuRequested(h.what == Hit::Clip ? h.uid : QString(), frame, e->globalPosition().toPoint());
            }
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        if (h.what == Hit::Ruler) {
            drag = Drag::Scrub;
            emit ed->scrubbed(frame);
            return;
        }
        if (h.what == Hit::Header) {
            if (h.button >= 0) toggleSwitch(rows()[h.row], h.button);
            return;
        }
        if (tool == TimelineEditor::Tool::Blade && (h.what == Hit::Clip || h.what == Hit::Note)) {
            blade(h, e->modifiers());
            return;
        }
        if (h.what == Hit::Note) {
            const NoteView& n = notes()[h.note];
            selectNote(n);
            if (n.clip.locked || doc.noteTracks.value(n.clip.lane).locked) return;
            noteIndex = h.note;
            noteDelta = 0;
            noteLane = n.clip.lane;
            noteEdge = h.edge;
            duplicate = e->modifiers() & Qt::AltModifier;
            drag = h.edge ? Drag::NoteTrim : Drag::NoteMove;
            snapCache = snapPoints({}, n.clip.key);
            return;
        }
        if (h.what == Hit::Clip) {
            const TimelineClip* c = clipOf(h.uid);
            if (!c) return;
            const bool toggle = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
            const bool solo = e->modifiers() & Qt::AltModifier && !toggle;  // Alt-click: this one only (unlinked)
            if (toggle) {
                QSet<QString> next = selected;
                const QSet<QString> group = expand({ h.uid });
                if (next.contains(h.uid)) next -= group;
                else next += group;
                select(next);
                return;
            }
            if (!selected.contains(h.uid)) select(solo ? QSet<QString>{ h.uid } : expand({ h.uid }));
            if (locked(*c)) return;
            pressUid = h.uid;
            pressKind = rows()[h.row].kind;
            pressTrack = rows()[h.row].track;
            if (h.handle == 1) {
                drag = Drag::Gain;
                gainStart = c->gainDb;
                dragIds.clear();
                for (const QString& uid : selected)
                    if (const TimelineClip* x = clipOf(uid); x && x->isAudio()) dragIds.insert(uid);
                dragIds.insert(h.uid);
                return;
            }
            if (h.handle == 2 || h.handle == 3) {
                drag = Drag::Fade;
                fadeSide = h.handle == 2 ? -1 : 1;
                dragIds = { h.uid };
                return;
            }
            dragRipple = ripple != bool(e->modifiers() & Qt::ControlModifier);
            if (h.edge != 0) {
                drag = Drag::Trim;
                trimEdge = h.edge;
                dragIds = solo ? QSet<QString>{ h.uid } : expand({ h.uid });
            } else {
                drag = Drag::Move;
                duplicate = e->modifiers() & Qt::AltModifier;
                dragIds = selected;
            }
            snapCache = snapPoints(dragIds);
            return;
        }
        // empty space: clear the selection, pick the gap under the pointer, start a marquee
        if (!(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) select({});
        if (h.row >= 0) {
            const Row r = rows()[h.row];
            if (r.kind != kRowNote) {
                const auto [a, b] = tl::gapAt(doc, r.kind == kRowVideo ? Kind::Video : Kind::Audio, r.track, frame, len());
                if (a >= 0) {
                    gapKind = r.kind;
                    gapTrack = r.track;
                    gapStart = a;
                    gapEnd = b;
                    update();
                }
            }
        }
        drag = Drag::Marquee;
        marquee = QRectF(pressPos, pressPos);
    }

    void toggleSwitch(const Row& r, int button)
    {
        if (r.kind == kRowNote) {
            TrackState st = doc.noteTracks.value(r.track);
            if (button == 0) st.visible = !st.visible;
            else st.locked = !st.locked;
            emit ed->trackChanged(kRowNote, r.track, st);
            return;
        }
        const Kind kind = r.kind == kRowVideo ? Kind::Video : Kind::Audio;
        TrackState st = doc.tracks(kind).value(r.track);
        if (r.kind == kRowAudio) {
            if (button == 0) st.visible = !st.visible;
            else if (button == 1) st.solo = !st.solo;
            else st.locked = !st.locked;
        } else {
            if (button == 0) st.visible = !st.visible;
            else st.locked = !st.locked;
        }
        emit ed->trackChanged(r.kind, r.track, st);
    }

    void selectNote(const NoteView& n)
    {
        selected.clear();
        gapKind = -1;
        selNoteSource = n.sourceId;
        selNoteKey = n.clip.key;
        update();
        emit ed->selectionChanged({});
        emit ed->noteSelected(n.sourceId, n.clip.key);
    }

    void blade(const Hit& h, Qt::KeyboardModifiers mods)
    {
        qint64 f = qint64(std::llround(frameAtX(lastMouse.x())));
        if (snap) f += snapped({ f }, 0, snapPoints({}));
        if (h.what == Hit::Note) {
            const NoteView& n = notes()[h.note];
            if (f > n.start && f < n.start + n.count) emit ed->noteSplit(n.sourceId, n.clip.key, shiftedNoteFrame(n.sourceId, n.visFirst, f - n.start));
            return;
        }
        QSet<QString> ids;
        if (mods & Qt::ShiftModifier) {
            for (const TimelineClip& c : doc.clips)
                if (!locked(c) && tl::span(c, len()).contains(f)) ids.insert(c.uid);
        } else {
            ids = expand({ h.uid });
        }
        TimelineDoc next = doc;
        QStringList created;
        if (tl::split(next, ids, f, len(), &created)) commit(next, L("Cut", "자르기"), created);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QPointF pos = e->position();
        const double dxStep = pos.x() - lastMouse.x();
        const double dyStep = pos.y() - lastMouse.y();
        lastMouse = pos;
        if (drag == Drag::None) {
            hover(pos);
            return;
        }
        if (!dragStarted && drag != Drag::Scrub && drag != Drag::Pan) {
            if (std::abs(pos.x() - pressPos.x()) < 4 && std::abs(pos.y() - pressPos.y()) < 4) return;
            dragStarted = true;
        }
        const qint64 rawDelta = qint64(std::llround((pos.x() - pressPos.x()) / ppf));
        invalidate();
        switch (drag) {
        case Drag::None: return;
        case Drag::NoteResize:
            noteHeights[resizeLane] = std::clamp(resizeStartHeight + int(std::lround(pos.y() - pressPos.y())), kMinNoteH, kMaxNoteH);
            clampScroll();
            ed->updateZoomUi();
            QToolTip::showText(e->globalPosition().toPoint(), QStringLiteral("N%1  %2 px").arg(resizeLane + 1).arg(noteHeight(resizeLane)), this);
            update();
            return;
        case Drag::Pan:
            scroll -= dxStep / ppf;
            vscroll -= dyStep;
            clampScroll();
            ed->updateZoomUi();
            update();
            return;
        case Drag::Scrub:
            emit ed->scrubbed(clampFrame(frameAtX(pos.x())));
            return;
        case Drag::Move: return previewMove(pos, rawDelta);
        case Drag::Trim: {
            const TimelineClip* c = clipOf(pressUid);
            if (!c) return;
            const tl::Span s = tl::span(*c, len());
            const qint64 edgeFrame = trimEdge < 0 ? s.start : s.end();
            qint64 delta = snapped({ edgeFrame }, rawDelta, snapCache);
            delta = tl::clampTrim(doc, dragIds, trimEdge, delta, dragRipple, len());
            live = doc;
            tl::trim(live, dragIds, trimEdge, delta, dragRipple, len());
            const TimelineClip* t = live.clip(pressUid);
            if (t) {
                const tl::Span ts = tl::span(*t, len());
                QToolTip::showText(e->globalPosition().toPoint(),
                                   QStringLiteral("%1  In %2  Out %3  (%4 f)  %5%6")
                                       .arg(dragRipple ? L("Ripple", "리플") : L("Trim", "트림"))
                                       .arg(ts.in).arg(ts.last()).arg(ts.count)
                                       .arg(delta >= 0 ? "+" : "").arg(delta), this);
            }
            update();
            return;
        }
        case Drag::Gain: {
            const TimelineClip* c = clipOf(pressUid);
            if (!c) return;
            const QRectF r = clipRect(*c);
            const double target = yToGain(pressPos.y() + (pos.y() - pressPos.y()) * (e->modifiers() & Qt::ShiftModifier ? 0.25 : 1.0) +
                                              (gainToY(gainStart, r) - pressPos.y()), r);
            const double delta = std::round((target - gainStart) * 2) / 2;
            live = doc;
            for (TimelineClip& x : live.clips)
                if (dragIds.contains(x.uid)) x.gainDb = std::clamp(doc.clip(x.uid)->gainDb + delta, -40.0, 12.0);
            QToolTip::showText(e->globalPosition().toPoint(), QStringLiteral("%1%2 dB").arg(gainStart + delta > 0 ? "+" : "").arg(gainStart + delta, 0, 'f', 1), this);
            update();
            return;
        }
        case Drag::Fade: {
            const TimelineClip* c = clipOf(pressUid);
            if (!c) return;
            const tl::Span s = tl::span(*c, len());
            const qint64 f = qint64(std::llround(frameAtX(pos.x())));
            live = doc;
            TimelineClip* x = live.clip(pressUid);
            if (fadeSide < 0) x->fadeIn = int(std::clamp<qint64>(f - s.start, 0, s.count - x->fadeOut));
            else x->fadeOut = int(std::clamp<qint64>(s.end() - f, 0, s.count - x->fadeIn));
            QToolTip::showText(e->globalPosition().toPoint(), QStringLiteral("%1 %2 f").arg(fadeSide < 0 ? L("Fade in", "페이드 인") : L("Fade out", "페이드 아웃"))
                                   .arg(fadeSide < 0 ? x->fadeIn : x->fadeOut), this);
            update();
            return;
        }
        case Drag::Marquee: {
            marquee = QRectF(pressPos, pos).normalized();
            QSet<QString> hits;
            for (const TimelineClip& c : doc.clips)
                if (!locked(c) && clipRect(c).intersects(marquee)) hits.insert(c.uid);
            if (pressMods & (Qt::ControlModifier | Qt::ShiftModifier)) hits += selected;
            selected = linked ? tl::withLinks(doc, hits) : hits;
            update();
            return;
        }
        case Drag::NoteMove:
        case Drag::NoteTrim: {
            if (noteIndex < 0 || noteIndex >= notes().size()) return;
            const NoteView n = notes()[noteIndex];
            if (drag == Drag::NoteMove) {
                qint64 delta = snapped({ n.start, n.start + n.count }, rawDelta, snapCache);
                // stay inside the source (a clip held to the end gets shorter when it moves later)
                delta = std::clamp<qint64>(delta, noteDistance(n.sourceId, n.clip.first, 0), std::max<qint64>(0, noteDistance(n.sourceId, n.clip.first, int(sources.value(n.sourceId).frames - 1))));
                noteDelta = delta;
                const int ri = rowIndexAt(pos.y());
                if (ri >= 0 && rows()[ri].kind == kRowNote) noteLane = rows()[ri].track;
                else if (pos.y() < kRuler + 4) noteLane = noteLanes();  // above the top lane: a new one
            } else {
                const qint64 edgeFrame = noteEdge < 0 ? n.start : n.start + n.count;
                qint64 delta = snapped({ edgeFrame }, rawDelta, snapCache);
                if (noteEdge < 0) delta = std::clamp<qint64>(delta, noteDistance(n.sourceId, n.visFirst, 0), n.count - 1);
                else delta = std::clamp<qint64>(delta, 1 - n.count, noteDistance(n.sourceId, n.visLast, int(sources.value(n.sourceId).frames - 1)));
                noteDelta = delta;
            }
            update();
            return;
        }
        }
    }

    void previewMove(const QPointF& pos, qint64 rawDelta)
    {
        const TimelineClip* pressed = clipOf(pressUid);
        if (!pressed) return;
        // edges of the moving block for snapping
        QVector<qint64> edges;
        for (const QString& uid : dragIds)
            if (const TimelineClip* c = clipOf(uid)) {
                const tl::Span s = tl::span(*c, len());
                edges << s.start << s.end();
            }
        qint64 delta = snapped(edges, rawDelta, snapCache);
        // track change for the kind that was grabbed
        int vtd = 0, atd = 0;
        const int ri = rowIndexAt(pos.y());
        const Row* over = ri >= 0 ? &rows()[ri] : nullptr;
        if (over && over->kind == pressKind && over->kind != kRowNote) {
            (pressKind == kRowVideo ? vtd : atd) = over->track - pressTrack;
        } else if (pressKind == kRowVideo && over && over->kind == kRowNote) {
            vtd = int(doc.videoTracks.size()) - pressTrack;  // above the top video track: a new track
        } else if (pressKind == kRowAudio && !over && pos.y() > kRuler + contentHeight() - vscroll - 30) {
            atd = int(doc.audioTracks.size()) - pressTrack;  // below the last audio track: a new track
        }
        live = doc;
        insertAt = -1;
        if (dragRipple && !duplicate) {
            const tl::Span s = tl::span(*pressed, len());
            const Kind kind = pressed->kind;
            const int track = pressed->track + (pressed->isVideo() ? vtd : atd);
            qint64 first = s.start;
            for (const QString& uid : dragIds)
                if (const TimelineClip* c = clipOf(uid)) first = std::min(first, tl::span(*c, len()).start);
            insertAt = tl::insertionPoint(doc, dragIds, kind, track, first + delta, len());
            tl::moveInsert(live, dragIds, insertAt, vtd, atd, len());
            snapAt = -1;
        } else {
            tl::moveOverwrite(live, dragIds, delta, vtd, atd, duplicate, len());
        }
        update();
    }

    void hover(const QPointF& pos)
    {
        if (resizeLaneAt(pos) >= 0) {
            bladeAt = -1;
            setCursor(Qt::SizeVerCursor);
            setToolTip(L("Drag to resize this annotation lane. Double-click to reset.", "끌어서 주석 레인 높이 조절 · 더블 클릭으로 기본 높이 복원"));
            update();
            return;
        }
        const Hit h = hit(pos);
        const qint64 bladeBefore = bladeAt;
        bladeAt = -1;
        Qt::CursorShape shape = Qt::ArrowCursor;
        QString tip;
        if (tool == TimelineEditor::Tool::Blade && (h.what == Hit::Clip || h.what == Hit::Note)) {
            shape = Qt::SplitHCursor;
            bladeAt = qint64(std::llround(frameAtX(pos.x())));
        } else if (h.what == Hit::Clip) {
            const TimelineClip* c = clipOf(h.uid);
            if (c && !locked(*c)) {
                if (h.edge) shape = Qt::SizeHorCursor;
                else if (h.handle == 1) shape = Qt::SizeVerCursor;
                else if (h.handle == 2 || h.handle == 3) shape = Qt::SizeHorCursor;
            }
            if (c) {
                const tl::Span s = tl::span(*c, len());
                tip = QStringLiteral("%1\n%2 %3  ·  %4 %5–%6 (%7 f)")
                          .arg(nameOf(*c), tl::trackName(doc, c->kind, c->track), shortTimecode(s.start, doc.fps), L("Source", "소스"))
                          .arg(s.in).arg(s.last()).arg(s.count);
                if (c->isAudio()) tip += QStringLiteral("\n%1 %2 dB").arg(L("Gain", "게인")).arg(c->gainDb, 0, 'f', 1);
            }
        } else if (h.what == Hit::Note) {
            const NoteView& n = notes()[h.note];
            if (h.edge) shape = Qt::SizeHorCursor;
            tip = QStringLiteral("%1\n%2 %3–%4").arg(n.clip.label, L("Source frames", "소스 프레임")).arg(n.clip.first).arg(n.clip.last);
        } else if (h.what == Hit::Header && h.button >= 0) {
            shape = Qt::PointingHandCursor;
        }
        setCursor(shape);
        setToolTip(tip);
        if (bladeAt != bladeBefore) update();
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        invalidate();
        const Drag was = drag;
        const bool started = dragStarted;
        drag = Drag::None;
        dragStarted = false;
        unsetCursor();
        QToolTip::hideText();
        const QString pressed = pressUid;
        pressUid.clear();
        switch (was) {
        case Drag::NoteResize:
            if (started) saveNoteHeights();
            resizeLane = -1;
            break;
        case Drag::Move:
            if (started) {
                QStringList select;
                if (duplicate) {
                    // copies are the clips that are new in the result
                    for (const TimelineClip& c : live.clips)
                        if (!doc.clip(c.uid)) select << c.uid;
                } else {
                    select = QStringList(dragIds.begin(), dragIds.end());
                }
                commit(live, duplicate ? L("Duplicate", "복제") : insertAt >= 0 ? L("Ripple Move", "리플 이동") : L("Move", "이동"), select);
            } else if (!(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) && !pressed.isEmpty()) {
                const QSet<QString> one = (e->modifiers() & Qt::AltModifier) ? QSet<QString>{ pressed } : expand({ pressed });
                if (one != selected) select(one);
            }
            break;
        case Drag::Trim:
            if (started) commit(live, dragRipple ? L("Ripple Trim", "리플 트림") : L("Trim", "트림"), QStringList(dragIds.begin(), dragIds.end()));
            break;
        case Drag::Gain:
            if (started) commit(live, L("Clip Gain", "클립 게인"), QStringList(dragIds.begin(), dragIds.end()));
            break;
        case Drag::Fade:
            if (started) commit(live, L("Fade", "페이드"), { pressed });
            break;
        case Drag::Marquee:
            if (started) emit ed->selectionChanged(QStringList(selected.begin(), selected.end()));
            break;
        case Drag::NoteMove:
            if (started && noteIndex >= 0 && noteIndex < notes().size()) {
                const NoteView n = notes()[noteIndex];
                if (duplicate) emit ed->noteDuplicated(n.sourceId, n.clip.key, shiftedNoteFrame(n.sourceId, n.clip.first, noteDelta));
                else if (noteDelta != 0 || noteLane != n.clip.lane)
                    emit ed->noteMoved(n.sourceId, n.clip.key, shiftedNoteFrame(n.sourceId, n.clip.first, noteDelta), noteLane);
            }
            break;
        case Drag::NoteTrim:
            if (started && noteIndex >= 0 && noteIndex < notes().size() && noteDelta != 0) {
                const NoteView n = notes()[noteIndex];
                // A clip shown cut off by its video clip trims from what is shown.
                const int first = noteEdge < 0 ? shiftedNoteFrame(n.sourceId, n.visFirst, noteDelta) : n.clip.first;
                const int last = noteEdge > 0 ? shiftedNoteFrame(n.sourceId, n.visLast, noteDelta) : n.clip.last;
                emit ed->noteTrimmed(n.sourceId, n.clip.key, first, last);
            }
            break;
        default: break;
        }
        noteIndex = -1;
        noteDelta = 0;
        duplicate = false;
        snapAt = insertAt = -1;
        invalidate();
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        const int lane = resizeLaneAt(e->position());
        if (e->button() == Qt::LeftButton && lane >= 0) {
            noteHeights[lane] = kNoteH;
            drag = Drag::None;
            resizeLane = -1;
            invalidate();
            clampScroll();
            ed->updateZoomUi();
            saveNoteHeights();
            update();
            return;
        }
        const Hit h = hit(e->position());
        if (h.what == Hit::Clip && e->button() == Qt::LeftButton) emit ed->clipActivated(h.uid);
    }

    void wheelEvent(QWheelEvent* e) override
    {
        const double steps = e->angleDelta().y() / 120.0 + e->angleDelta().x() / 120.0;
        if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
            setZoom(ppf * std::pow(1.25, steps), e->position().x());
        } else if (e->modifiers() & Qt::ShiftModifier) {
            vscroll -= steps * 40;
            clampScroll();
            ed->updateZoomUi();
            update();
        } else {
            scroll -= steps * visibleFrames() / 8.0;
            clampScroll();
            ed->updateZoomUi();
            update();
        }
        e->accept();
    }

    // ------------------------------------------------------------------ keys
    bool handles(const QKeyEvent* k) const
    {
        const auto mods = k->modifiers() & ~Qt::KeypadModifier;
        switch (k->key()) {
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
        case Qt::Key_V: case Qt::Key_A: case Qt::Key_C: case Qt::Key_X:
            return mods == Qt::NoModifier || mods == Qt::ControlModifier;  // tools, and copy / paste / select all of clips
        case Qt::Key_B: case Qt::Key_S: case Qt::Key_D:
        case Qt::Key_J: case Qt::Key_K: case Qt::Key_L: case Qt::Key_Plus: case Qt::Key_Equal: case Qt::Key_Minus:
            return mods == Qt::NoModifier;
        case Qt::Key_Z:
            return mods == Qt::ShiftModifier;
        case Qt::Key_Left: case Qt::Key_Right:
            return mods == Qt::AltModifier || mods == (Qt::AltModifier | Qt::ShiftModifier);
        case Qt::Key_Escape:
            return !ed->selectedClips().isEmpty() || !selNoteKey.isEmpty() || gapKind >= 0;
        default:
            if (mods == Qt::ControlModifier)
                return k->key() == Qt::Key_A || k->key() == Qt::Key_C || k->key() == Qt::Key_X || k->key() == Qt::Key_V;
            return false;
        }
    }

    bool event(QEvent* e) override
    {
        // Keys the timeline uses while it has focus win over the window-wide shortcuts.
        if (e->type() == QEvent::ShortcutOverride && handles(static_cast<QKeyEvent*>(e))) {
            e->accept();
            return true;
        }
        return QWidget::event(e);
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (!handles(e)) return QWidget::keyPressEvent(e);
        e->accept();
        const auto mods = e->modifiers() & ~Qt::KeypadModifier;
        const bool ctrl = mods == Qt::ControlModifier;
        switch (e->key()) {
        case Qt::Key_Delete:
        case Qt::Key_Backspace: ed->deleteSelection(mods == Qt::ShiftModifier ? !ripple : ripple); return;
        case Qt::Key_V: if (mods == Qt::NoModifier) return ed->setTool(TimelineEditor::Tool::Select); if (ctrl) return ed->pasteAtPlayhead(); break;
        case Qt::Key_A: if (mods == Qt::NoModifier) return ed->setTool(TimelineEditor::Tool::Select); if (ctrl) return ed->selectAllClips(); break;
        case Qt::Key_B: if (mods == Qt::NoModifier) return ed->setTool(TimelineEditor::Tool::Blade); break;
        case Qt::Key_C: if (mods == Qt::NoModifier) return ed->setTool(TimelineEditor::Tool::Blade); if (ctrl) return ed->copySelection(); break;
        case Qt::Key_X: if (ctrl) return ed->cutSelection(ripple); return;
        case Qt::Key_S: if (mods == Qt::NoModifier) return ed->setSnap(!snap); break;
        case Qt::Key_D: if (mods == Qt::NoModifier) return toggleEnabled(); break;
        case Qt::Key_J: emit ed->shuttle(-1); return;
        case Qt::Key_K: emit ed->shuttle(0); return;
        case Qt::Key_L: emit ed->shuttle(1); return;
        case Qt::Key_Plus:
        case Qt::Key_Equal: return setZoom(ppf * 1.5, current >= 0 ? xFor(current) : width() / 2.0);
        case Qt::Key_Minus: return setZoom(ppf / 1.5, current >= 0 ? xFor(current) : width() / 2.0);
        case Qt::Key_Z: if (mods == Qt::ShiftModifier) return ed->zoomToFit(); break;
        case Qt::Key_Left: if (mods & Qt::AltModifier) return ed->nudgeSelection(mods & Qt::ShiftModifier ? -10 : -1); break;
        case Qt::Key_Right: if (mods & Qt::AltModifier) return ed->nudgeSelection(mods & Qt::ShiftModifier ? 10 : 1); break;
        case Qt::Key_Escape: select({}); return;
        default: break;
        }
        QWidget::keyPressEvent(e);
    }

    void toggleEnabled()
    {
        if (selected.isEmpty()) return;
        TimelineDoc next = doc;
        bool anyOn = false;
        for (const TimelineClip& c : doc.clips)
            if (selected.contains(c.uid)) anyOn = anyOn || c.enabled;
        for (TimelineClip& c : next.clips)
            if (selected.contains(c.uid) && !locked(c)) c.enabled = !anyOn;
        commit(next, anyOn ? L("Disable Clip", "클립 끄기") : L("Enable Clip", "클립 켜기"), QStringList(selected.begin(), selected.end()));
    }

    // ------------------------------------------------------------------ drops
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasUrls() || !sourceIdsFrom(e->mimeData()).isEmpty()) {
            e->setDropAction(Qt::CopyAction);
            e->accept();
        }
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        const int track = dropTrack(rowIndexAt(e->position().y()));
        if (tl::trackLocked(doc, Kind::Video, track) || tl::trackLocked(doc, Kind::Audio, track)) {
            dropFrame = dropRow = -1;
            update();
            e->ignore();
            return;
        }
        qint64 f = std::max<qint64>(0, qint64(std::floor(frameAtX(e->position().x()))));
        if (snap) {
            const qint64 d = snapped({ f }, 0, snapPoints({}));
            f += d;
        }
        dropFrame = f;
        dropRow = rowIndexAt(e->position().y());
        update();
        e->setDropAction(Qt::CopyAction);
        e->accept();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override
    {
        dropFrame = -1;
        dropRow = -1;
        snapAt = -1;
        update();
    }
    int dropTrack(int row) const
    {
        const QVector<Row> rs = rows();
        if (row < 0 || row >= rs.size()) return 0;
        if (rs[row].kind == kRowNote) return int(doc.videoTracks.size());  // over the annotation lanes: a new video track
        return rs[row].track;
    }
    void dropEvent(QDropEvent* e) override
    {
        const qint64 frame = dropFrame >= 0 ? dropFrame : std::max<qint64>(0, qint64(frameAtX(e->position().x())));
        const int track = dropTrack(rowIndexAt(e->position().y()));
        dropFrame = -1;
        dropRow = -1;
        snapAt = -1;
        update();
        if (tl::trackLocked(doc, Kind::Video, track) || tl::trackLocked(doc, Kind::Audio, track)) {
            e->ignore();
            return;
        }
        const QStringList ids = sourceIdsFrom(e->mimeData());
        if (!ids.isEmpty()) {
            setFocus(Qt::MouseFocusReason);
            e->setDropAction(Qt::CopyAction);
            e->accept();
            emit ed->sourcesDropped(ids, track, frame);
            return;
        }
        QStringList paths;
        for (const QUrl& url : e->mimeData()->urls())
            if (url.isLocalFile()) paths << url.toLocalFile();
        if (!paths.isEmpty()) {
            setFocus(Qt::MouseFocusReason);
            e->setDropAction(Qt::CopyAction);
            e->accept();
            emit ed->filesDropped(paths, track, frame);
        }
    }

    void resizeEvent(QResizeEvent*) override
    {
        clampScroll();
        ed->updateZoomUi();
    }
};

// ============================================================================ editor

TimelineEditor::TimelineEditor(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("TimelineEditor");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* bar = new QWidget;
    bar->setObjectName("PanelHeader");
    bar->setAttribute(Qt::WA_StyledBackground);
    bar->setFixedHeight(34);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(10, 0, 8, 0);
    h->setSpacing(3);
    m_clock = new QLabel(QStringLiteral("00:00:00:00"));
    m_clock->setObjectName("TimelineClock");
    m_clock->setStyleSheet(QStringLiteral("QLabel { font-family: 'Cascadia Mono'; font-size: 15px; color: #f0efe9; padding-right: 8px; }"));
    h->addWidget(m_clock);
    m_title = new QLabel;
    m_title->setObjectName("PanelTitle");
    h->addWidget(m_title);
    m_banner = new QLabel;
    m_banner->setObjectName("Muted");
    h->addSpacing(8);
    h->addWidget(m_banner, 1);

    auto button = [&](const QString& icon, const QString& tip, bool checkable = false) {
        auto* b = new QToolButton;
        b->setIcon(Icons::icon(icon));
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(26, 26);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        h->addWidget(b);
        return b;
    };
    m_selectTool = button("cursor", L("Selection Tool  V / A", "선택 도구  V / A"), true);
    m_selectTool->setObjectName("TimelineSelectTool");
    m_bladeTool = button("split", L("Blade Tool  B / C  (Shift+click: all tracks)", "자르기 도구  B / C  (Shift+클릭: 모든 트랙)"), true);
    m_bladeTool->setObjectName("TimelineBladeTool");
    h->addSpacing(6);
    m_rippleBtn = button("ripple", L("Ripple Editing: moves, trims and deletes push or pull the clips after them (Ctrl inverts while dragging)",
                                     "리플 편집: 이동·트림·삭제가 뒤 클립을 밀거나 당깁니다 (드래그 중 Ctrl로 반전)"), true);
    m_rippleBtn->setObjectName("TimelineRipple");
    m_snapBtn = button("magnet", L("Snapping  S", "스냅  S"), true);
    m_snapBtn->setObjectName("TimelineSnap");
    m_linkBtn = button("link", L("Linked Selection: picture and sound move together (Alt+click picks one)",
                                 "연결 선택: 영상과 소리가 함께 움직입니다 (Alt+클릭은 하나만)"), true);
    m_linkBtn->setObjectName("TimelineLinked");
    h->addSpacing(6);
    auto* split = button("split", L("Cut at Playhead  Ctrl+B", "재생 위치에서 자르기  Ctrl+B"));
    split->setObjectName("TimelineSplit");
    auto* gap = button("gap", L("Close Gap at Playhead", "재생 위치의 빈 공간 닫기"));
    gap->setObjectName("TimelineCloseGap");
    auto* remove = button("trash", L("Delete (leave gap)  Delete   ·   Ripple Delete  Shift+Delete", "삭제 (빈 공간 유지)  Delete   ·   리플 삭제  Shift+Delete"));
    remove->setObjectName("TimelineRemove");
    auto* addTrackBtn = button("track-add", L("Add Video Track (right-click: Audio Track)", "비디오 트랙 추가 (우클릭: 오디오 트랙)"));
    addTrackBtn->setObjectName("TimelineAddTrack");
    m_timecode = button("clock", L("Ruler: timecode / frame numbers", "눈금: 타임코드 / 프레임 번호"), true);
    h->addSpacing(6);
    auto* out = button("zoom-out", L("Zoom Out  −", "축소  −"));
    m_zoom = new QSlider(Qt::Horizontal);
    m_zoom->setRange(0, 1000);
    m_zoom->setFixedWidth(100);
    m_zoom->setFocusPolicy(Qt::NoFocus);
    m_zoom->setToolTip(L("Zoom (Ctrl+wheel)", "확대 / 축소 (Ctrl+휠)"));
    h->addWidget(m_zoom);
    auto* in = button("zoom-in", L("Zoom In  +", "확대  +"));
    auto* fit = button("fit", L("Fit Timeline  Shift+Z", "타임라인 전체 보기  Shift+Z"));
    v->addWidget(bar);

    auto* body = new QWidget;
    auto* bh = new QHBoxLayout(body);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->setSpacing(0);
    m_canvas = new TimelineCanvas(this);
    setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(m_canvas);
    bh->addWidget(m_canvas, 1);
    m_vscroll = new QScrollBar(Qt::Vertical);
    m_vscroll->setFocusPolicy(Qt::NoFocus);
    bh->addWidget(m_vscroll);
    v->addWidget(body, 1);
    m_scroll = new QScrollBar(Qt::Horizontal);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    v->addWidget(m_scroll);

    auto ppfFromSlider = [](int v) { return kMinPpf * std::pow(kMaxPpf / kMinPpf, v / 1000.0); };
    connect(m_zoom, &QSlider::valueChanged, this, [this, ppfFromSlider](int v) {
        const double next = ppfFromSlider(v);
        if (std::abs(next - m_canvas->ppf) / m_canvas->ppf > 0.01)
            m_canvas->setZoom(next, m_canvas->current >= 0 ? m_canvas->xFor(m_canvas->current) : kGutter + (m_canvas->width() - kGutter) / 2.0);
    });
    connect(in, &QToolButton::clicked, this, &TimelineEditor::zoomIn);
    connect(out, &QToolButton::clicked, this, &TimelineEditor::zoomOut);
    connect(fit, &QToolButton::clicked, this, &TimelineEditor::zoomToFit);
    connect(m_scroll, &QScrollBar::valueChanged, this, [this](int v) {
        const double next = v / m_canvas->ppf;
        if (std::abs(next - m_canvas->scroll) * m_canvas->ppf >= 1) {
            m_canvas->scroll = next;
            m_canvas->update();
        }
    });
    connect(m_vscroll, &QScrollBar::valueChanged, this, [this](int v) {
        if (std::abs(v - m_canvas->vscroll) >= 1) {
            m_canvas->vscroll = v;
            m_canvas->update();
        }
    });
    connect(m_selectTool, &QToolButton::clicked, this, [this] { setTool(Tool::Select); });
    connect(m_bladeTool, &QToolButton::clicked, this, [this] { setTool(Tool::Blade); });
    connect(m_rippleBtn, &QToolButton::toggled, this, [this](bool on) { setRipple(on); });
    connect(m_snapBtn, &QToolButton::toggled, this, [this](bool on) { setSnap(on); });
    connect(m_linkBtn, &QToolButton::toggled, this, [this](bool on) { setLinkedSelection(on); });
    connect(m_timecode, &QToolButton::toggled, this, [this](bool on) {
        m_canvas->timecode = on;
        m_canvas->update();
    });
    connect(split, &QToolButton::clicked, this, [this] { splitAtPlayhead(); });
    connect(gap, &QToolButton::clicked, this, [this] { closeGapAtPlayhead(); });
    connect(remove, &QToolButton::clicked, this, [this] { deleteSelection(m_canvas->ripple); });
    connect(addTrackBtn, &QToolButton::clicked, this, [this] { addTrack(TimelineClip::Kind::Video); });
    addTrackBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(addTrackBtn, &QToolButton::customContextMenuRequested, this, [this] { addTrack(TimelineClip::Kind::Audio); });
    syncModeButtons();
    updateZoomUi();
}

void TimelineEditor::setModel(const TimelineDoc& doc, QHash<QString, SourceInfo> sources, const QString& title)
{
    const bool wasEmpty = m_canvas->doc.clips.isEmpty();
    m_canvas->doc = doc;
    m_canvas->sources = std::move(sources);
    m_canvas->invalidate();
    // Drop what no longer exists from the selection.
    QSet<QString> keep;
    for (const QString& uid : m_canvas->selected)
        if (doc.clip(uid)) keep.insert(uid);
    m_canvas->selected = keep;
    if (m_canvas->gapKind >= 0) {
        const auto [a, b] = tl::gapAt(doc, m_canvas->gapKind == kRowVideo ? Kind::Video : Kind::Audio, m_canvas->gapTrack,
                                      m_canvas->gapStart, m_canvas->len());
        if (a != m_canvas->gapStart || b != m_canvas->gapEnd) m_canvas->gapKind = -1;
    }
    if (!m_canvas->selNoteKey.isEmpty()) {
        bool found = false;
        for (const NoteClip& n : m_canvas->sources.value(m_canvas->selNoteSource).noteClips) found = found || n.key == m_canvas->selNoteKey;
        if (!found) m_canvas->selNoteKey.clear();
    }
    m_title->setText(title);
    if (wasEmpty && !doc.clips.isEmpty()) zoomToFit();
    m_canvas->clampScroll();
    updateZoomUi();
    m_canvas->update();
}

const TimelineDoc& TimelineEditor::doc() const { return m_canvas->doc; }

void TimelineEditor::setCurrent(qint64 frame)
{
    if (frame == m_canvas->current) return;
    m_canvas->current = frame;
    m_clock->setText(frame >= 0 ? timecodeOf(frame, m_canvas->doc.fps) : QStringLiteral("--:--:--:--"));
    if (isVisible()) m_canvas->ensureVisible(frame);
    m_canvas->update();
}

void TimelineEditor::setLoop(bool enabled, qint64 in, qint64 out)
{
    m_canvas->loop = enabled;
    m_canvas->loopIn = in;
    m_canvas->loopOut = out;
    m_canvas->update();
}

void TimelineEditor::setCacheMask(QBitArray mask)
{
    m_canvas->cache = std::move(mask);
    if (isVisible()) m_canvas->update();
}

void TimelineEditor::setBanner(const QString& text) { m_banner->setText(text); }

QStringList TimelineEditor::selectedClips() const
{
    QStringList out(m_canvas->selected.begin(), m_canvas->selected.end());
    out.sort();
    return out;
}

QString TimelineEditor::selectedClip() const
{
    // The video clip of the selection first (what 1.0 meant by "the selected clip").
    for (const TimelineClip& c : m_canvas->doc.clips)
        if (m_canvas->selected.contains(c.uid) && c.isVideo()) return c.uid;
    return m_canvas->selected.isEmpty() ? QString() : *m_canvas->selected.begin();
}

void TimelineEditor::setSelectedClips(const QStringList& uids)
{
    QSet<QString> ids;
    for (const QString& uid : uids)
        if (m_canvas->doc.clip(uid)) ids.insert(uid);
    m_canvas->selected = ids;
    m_canvas->selNoteKey.clear();
    m_canvas->gapKind = -1;
    m_canvas->update();
}

void TimelineEditor::setSelectedClip(const QString& uid) { setSelectedClips(uid.isEmpty() ? QStringList() : QStringList{ uid }); }
QString TimelineEditor::selectedNoteSource() const { return m_canvas->selNoteSource; }
QString TimelineEditor::selectedNoteKey() const { return m_canvas->selNoteKey; }
int TimelineEditor::annotationLaneHeight(int lane) const { return m_canvas->noteHeight(lane); }

void TimelineEditor::setTool(Tool tool)
{
    m_canvas->tool = tool;
    m_canvas->bladeAt = -1;
    syncModeButtons();
    m_canvas->update();
}

TimelineEditor::Tool TimelineEditor::tool() const { return m_canvas->tool; }

void TimelineEditor::setRipple(bool on)
{
    if (m_canvas->ripple == on) return;
    m_canvas->ripple = on;
    syncModeButtons();
    emit modesChanged();
}

bool TimelineEditor::ripple() const { return m_canvas->ripple; }

void TimelineEditor::setSnap(bool on)
{
    if (m_canvas->snap == on) return;
    m_canvas->snap = on;
    syncModeButtons();
    emit modesChanged();
}

bool TimelineEditor::snap() const { return m_canvas->snap; }

void TimelineEditor::setLinkedSelection(bool on)
{
    if (m_canvas->linked == on) return;
    m_canvas->linked = on;
    syncModeButtons();
    emit modesChanged();
}

bool TimelineEditor::linkedSelection() const { return m_canvas->linked; }

void TimelineEditor::syncModeButtons()
{
    const auto set = [](QToolButton* b, bool on) {
        const QSignalBlocker block(b);
        b->setChecked(on);
    };
    set(m_selectTool, m_canvas->tool == Tool::Select);
    set(m_bladeTool, m_canvas->tool == Tool::Blade);
    set(m_rippleBtn, m_canvas->ripple);
    set(m_snapBtn, m_canvas->snap);
    set(m_linkBtn, m_canvas->linked);
}

void TimelineEditor::deleteSelection(bool ripple)
{
    TimelineCanvas& c = *m_canvas;
    if (!c.selNoteKey.isEmpty()) {
        for (const auto& n : c.notes())
            if (n.sourceId == c.selNoteSource && n.clip.key == c.selNoteKey &&
                (n.clip.locked || c.doc.noteTracks.value(n.clip.lane).locked)) return;
        emit noteErased(c.selNoteSource, c.selNoteKey);
        return;
    }
    if (c.selected.isEmpty() && c.gapKind >= 0) {
        TimelineDoc next = c.doc;
        if (tl::closeGap(next, c.gapKind == kRowVideo ? Kind::Video : Kind::Audio, c.gapTrack, c.gapStart, c.len())) {
            c.gapKind = -1;
            c.commit(next, L("Close Gap", "빈 공간 닫기"));
        }
        return;
    }
    if (c.selected.isEmpty()) return;
    TimelineDoc next = c.doc;
    if (tl::remove(next, c.selected, ripple, c.len())) {
        c.selected.clear();
        c.commit(next, ripple ? L("Ripple Delete", "리플 삭제") : L("Delete", "삭제"));
    }
}

bool TimelineEditor::splitAtPlayhead()
{
    TimelineCanvas& c = *m_canvas;
    const qint64 f = c.current;
    if (f < 0) return false;
    if (!c.selNoteKey.isEmpty()) {
        for (const TimelineCanvas::NoteView& n : c.notes())
            if (n.sourceId == c.selNoteSource && n.clip.key == c.selNoteKey && f > n.start && f < n.start + n.count) {
                emit noteSplit(n.sourceId, n.clip.key, c.shiftedNoteFrame(n.sourceId, n.visFirst, f - n.start));
                return true;
            }
    }
    QSet<QString> ids;
    for (const QString& uid : c.selected)
        if (const TimelineClip* x = c.doc.clip(uid); x && tl::span(*x, c.len()).contains(f)) ids.insert(uid);
    if (ids.isEmpty()) {
        for (const TimelineClip& x : c.doc.clips)
            if (!c.locked(x) && tl::span(x, c.len()).contains(f)) ids.insert(x.uid);
    } else {
        ids = c.expand(ids);
    }
    TimelineDoc next = c.doc;
    QStringList created;
    if (!tl::split(next, ids, f, c.len(), &created)) return false;
    c.commit(next, L("Cut", "자르기"), created);
    return true;
}

void TimelineEditor::nudgeSelection(int frames)
{
    TimelineCanvas& c = *m_canvas;
    if (c.selected.isEmpty()) return;
    TimelineDoc next = c.doc;
    if (tl::moveOverwrite(next, c.selected, frames, 0, 0, false, c.len()))
        c.commit(next, L("Nudge", "미세 이동"), QStringList(c.selected.begin(), c.selected.end()));
}

void TimelineEditor::selectAllClips()
{
    QSet<QString> all;
    for (const TimelineClip& x : m_canvas->doc.clips)
        if (!m_canvas->locked(x)) all.insert(x.uid);
    m_canvas->select(all);
}

void TimelineEditor::copySelection()
{
    m_canvas->clipboard.clear();
    for (const TimelineClip& x : m_canvas->doc.clips)
        if (m_canvas->selected.contains(x.uid)) m_canvas->clipboard << x;
}

void TimelineEditor::cutSelection(bool ripple)
{
    copySelection();
    deleteSelection(ripple);
}

void TimelineEditor::pasteAtPlayhead()
{
    TimelineCanvas& c = *m_canvas;
    if (c.clipboard.isEmpty() || c.current < 0) return;
    qint64 first = std::numeric_limits<qint64>::max();
    for (const TimelineClip& x : c.clipboard) first = std::min(first, tl::span(x, c.len()).start);
    QHash<QString, QString> renamed;
    for (const TimelineClip& x : c.clipboard) renamed[x.uid] = TimelineClip::newUid();
    TimelineDoc next = c.doc;
    QVector<TimelineClip> pasted;
    for (TimelineClip x : c.clipboard) {
        if (!c.sources.contains(x.sourceId)) continue;
        const tl::Span s = tl::span(x, c.len());
        x.uid = renamed[x.uid];
        x.link = renamed.value(x.link);
        x.start = c.current + (s.start - first);
        pasted << x;
    }
    for (const TimelineClip& x : pasted) {
        const tl::Span s = tl::span(x, c.len());
        while (next.tracks(x.kind).size() <= x.track) next.tracks(x.kind) << TrackState{};
        tl::clearRange(next, x.kind, x.track, s.start, s.end(), {}, c.len());
    }
    QStringList select;
    for (const TimelineClip& x : pasted) {
        next.clips << x;
        select << x.uid;
    }
    tl::normalize(next, c.len());
    c.commit(next, L("Paste", "붙여넣기"), select);
}

bool TimelineEditor::closeGapAtPlayhead()
{
    TimelineCanvas& c = *m_canvas;
    if (c.current < 0) return false;
    Kind kind = Kind::Video;
    int track = 0;
    if (!c.selected.isEmpty())
        if (const TimelineClip* x = c.doc.clip(*c.selected.begin())) {
            kind = x->kind;
            track = x->track;
        }
    TimelineDoc next = c.doc;
    if (!tl::closeGap(next, kind, track, c.current, c.len())) return false;
    c.commit(next, L("Close Gap", "빈 공간 닫기"));
    return true;
}

void TimelineEditor::addTrack(TimelineClip::Kind kind)
{
    TimelineDoc next = m_canvas->doc;
    if (next.tracks(kind).size() >= 64) return;
    next.tracks(kind) << TrackState{};
    m_canvas->commit(next, kind == Kind::Video ? L("Add Video Track", "비디오 트랙 추가") : L("Add Audio Track", "오디오 트랙 추가"));
}

void TimelineEditor::setThumbnailProvider(std::function<QImage(const QString&, qint64)> provider)
{
    m_canvas->thumbs = std::move(provider);
}

void TimelineEditor::clearThumbnails() { m_canvas->thumbCache.clear(); }
int TimelineEditor::clipCount() const { return m_canvas->doc.videoClipCount(); }
qint64 TimelineEditor::currentFrame() const { return m_canvas->current; }

QWidget* TimelineEditor::canvas() const { return m_canvas; }

QPointF TimelineEditor::clipPoint(const QString& uid, double fraction) const
{
    const TimelineClip* c = m_canvas->doc.clip(uid);
    if (!c) return {};
    const QRectF r = m_canvas->clipRect(*c);
    // Edges a hair inside so they hit the trim handle, not the neighbour.
    const double x = fraction <= 0 ? r.left() + 2 : fraction >= 1 ? r.right() - 2 : r.left() + r.width() * fraction;
    return QPointF(x, r.center().y() + (c->isAudio() ? r.height() * 0.3 : 0));
}

QPointF TimelineEditor::trackPoint(int kind, int track, qint64 frame) const
{
    const int ri = m_canvas->rowOf(kind, track);
    if (ri < 0) return {};
    const auto& r = m_canvas->rows()[ri];
    return QPointF(m_canvas->xFor(double(frame) + 0.5), m_canvas->rowY(r) + r.h / 2);
}

QPointF TimelineEditor::notePoint(const QString& sourceId, const QString& key, double fraction) const
{
    for (const TimelineCanvas::NoteView& n : m_canvas->notes())
        if (n.sourceId == sourceId && n.clip.key == key) {
            const QRectF r = m_canvas->noteRect(n);
            const double x = fraction <= 0 ? r.left() + 2 : fraction >= 1 ? r.right() - 2 : r.left() + r.width() * fraction;
            return QPointF(x, r.center().y());
        }
    return {};
}

void TimelineEditor::zoomIn() { m_canvas->setZoom(m_canvas->ppf * 1.5, m_canvas->current >= 0 ? m_canvas->xFor(m_canvas->current) : kGutter + (m_canvas->width() - kGutter) / 2.0); }
void TimelineEditor::zoomOut() { m_canvas->setZoom(m_canvas->ppf / 1.5, m_canvas->current >= 0 ? m_canvas->xFor(m_canvas->current) : kGutter + (m_canvas->width() - kGutter) / 2.0); }

void TimelineEditor::zoomToFit()
{
    const double frames = double(std::max<qint64>(1, m_canvas->total()));
    m_canvas->ppf = std::clamp((m_canvas->width() - kGutter - 24) / frames, kMinPpf, kMaxPpf);
    m_canvas->scroll = 0;
    updateZoomUi();
    m_canvas->update();
}

void TimelineEditor::updateZoomUi()
{
    if (!m_zoom || !m_scroll || !m_vscroll) return;
    {
        const QSignalBlocker block(m_zoom);
        m_zoom->setValue(int(std::round(1000.0 * std::log(m_canvas->ppf / kMinPpf) / std::log(kMaxPpf / kMinPpf))));
    }
    {
        const QSignalBlocker block(m_scroll);
        const double content = (m_canvas->total() + 10) * m_canvas->ppf;
        const double view = std::max(1, m_canvas->width() - kGutter);
        m_scroll->setRange(0, int(std::max(0.0, content - view * 0.5)));
        m_scroll->setPageStep(int(view));
        m_scroll->setSingleStep(std::max(1, int(view / 20)));
        m_scroll->setValue(int(m_canvas->scroll * m_canvas->ppf));
    }
    const QSignalBlocker block(m_vscroll);
    const double contentH = m_canvas->contentHeight();
    const double viewH = std::max(1, m_canvas->height() - kRuler);
    m_vscroll->setRange(0, int(std::max(0.0, contentH - viewH)));
    m_vscroll->setPageStep(int(viewH));
    m_vscroll->setSingleStep(20);
    m_vscroll->setValue(int(m_canvas->vscroll));
    m_vscroll->setVisible(contentH > viewH + 1);
}
