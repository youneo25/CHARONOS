#include "TimelineModel.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace tl {

namespace {

constexpr int kMaxTracks = 64;
constexpr int64_t kFar = std::numeric_limits<int64_t>::max() / 4;

using TrackKey = std::pair<int, int>;  // kind, track
TrackKey keyOf(const TimelineClip& c) { return { int(c.kind), c.track }; }
bool onTrack(const TimelineClip& c, const TrackKey& k) { return int(c.kind) == k.first && c.track == k.second; }

void ensureTrack(TimelineDoc& doc, Kind kind, int track)
{
    QVector<TrackState>& list = doc.tracks(kind);
    while (list.size() <= track && list.size() < kMaxTracks) list << TrackState{};
}

QVector<TrackKey> allTracks(const TimelineDoc& doc)
{
    QVector<TrackKey> out;
    for (int t = 0; t < doc.videoTracks.size(); ++t) out.append({ int(Kind::Video), t });
    for (int t = 0; t < doc.audioTracks.size(); ++t) out.append({ int(Kind::Audio), t });
    return out;
}

bool lockedKey(const TimelineDoc& doc, const TrackKey& k) { return trackLocked(doc, Kind(k.first), k.second); }

// Clips of a track starting at or after `at` move by `delta`.
void shiftFrom(TimelineDoc& doc, const TrackKey& k, int64_t at, int64_t delta, const QSet<QString>& except)
{
    for (TimelineClip& c : doc.clips)
        if (onTrack(c, k) && !except.contains(c.uid) && c.start >= at) c.start = std::max<int64_t>(0, c.start + delta);
}

bool emptyIn(const TimelineDoc& doc, const TrackKey& k, int64_t a, int64_t b, const QSet<QString>& except, const LengthFn& len)
{
    for (const TimelineClip& c : doc.clips) {
        if (!onTrack(c, k) || except.contains(c.uid)) continue;
        const Span s = span(c, len);
        if (s.start < b && s.end() > a) return false;
    }
    return true;
}

bool crosses(const TimelineDoc& doc, const TrackKey& k, int64_t at, const QSet<QString>& except, const LengthFn& len)
{
    for (const TimelineClip& c : doc.clips) {
        if (!onTrack(c, k) || except.contains(c.uid)) continue;
        const Span s = span(c, len);
        if (s.start < at && s.end() > at) return true;
    }
    return false;
}

// Ripple: the frames [a, b) leave the given tracks (later clips close up). Other unlocked
// tracks follow when nothing of theirs is in that range, so overlays stay in sync.
void rippleClose(TimelineDoc& doc, const QSet<TrackKey>& tracks, int64_t a, int64_t b, const QSet<QString>& except,
                 const LengthFn& len, bool syncOthers)
{
    if (b <= a) return;
    for (const TrackKey& k : allTracks(doc)) {
        const bool own = tracks.contains(k);
        if (!own && (!syncOthers || lockedKey(doc, k) || !emptyIn(doc, k, a, b, except, len))) continue;
        shiftFrom(doc, k, b, a - b, except);
    }
}

// Ripple: `count` frames open at `at` on the given tracks (and on others that have no clip
// crossing that point).
void rippleOpen(TimelineDoc& doc, const QSet<TrackKey>& tracks, int64_t at, int64_t count, const QSet<QString>& except,
                const LengthFn& len, bool syncOthers)
{
    if (count <= 0) return;
    for (const TrackKey& k : allTracks(doc)) {
        const bool own = tracks.contains(k);
        if (!own && (!syncOthers || lockedKey(doc, k) || crosses(doc, k, at, except, len))) continue;
        shiftFrom(doc, k, at, count, except);
    }
}

void unlinkPartnerOf(TimelineDoc& doc, const QString& uid)
{
    for (TimelineClip& c : doc.clips)
        if (c.link == uid) c.link.clear();
}

// Cuts every clip of a track crossing `at` (used before a ripple insert).
void cutAt(TimelineDoc& doc, const TrackKey& k, int64_t at, const LengthFn& len)
{
    QSet<QString> ids;
    for (const TimelineClip& c : doc.clips)
        if (onTrack(c, k)) {
            const Span s = span(c, len);
            if (s.start < at && s.end() > at) ids.insert(c.uid);
        }
    if (!ids.isEmpty()) split(doc, ids, at, len);
}

int64_t prevEnd(const TimelineDoc& doc, const TimelineClip& c, const LengthFn& len)
{
    const Span own = span(c, len);
    int64_t best = 0;
    for (const TimelineClip& o : doc.clips) {
        if (o.uid == c.uid || o.kind != c.kind || o.track != c.track) continue;
        const Span s = span(o, len);
        if (s.end() <= own.start) best = std::max(best, s.end());
    }
    return best;
}

int64_t nextStart(const TimelineDoc& doc, const TimelineClip& c, const LengthFn& len)
{
    const Span own = span(c, len);
    int64_t best = kFar;
    for (const TimelineClip& o : doc.clips) {
        if (o.uid == c.uid || o.kind != c.kind || o.track != c.track) continue;
        const Span s = span(o, len);
        if (s.start >= own.end()) best = std::min(best, s.start);
    }
    return best;
}

void sortClips(TimelineDoc& doc)
{
    std::stable_sort(doc.clips.begin(), doc.clips.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.start != b.start) return a.start < b.start;
        return a.track < b.track;
    });
}

} // namespace

// ============================================================================ layout

Span span(const TimelineClip& c, const LengthFn& len)
{
    const SourceTiming timing = len ? len(c.sourceId) : SourceTiming{};
    const int64_t n = timing.frames;
    int64_t in = std::max(0, c.in);
    int64_t last = c.out >= 0 ? c.out : (n > 0 ? n - 1 : in);
    if (n > 0) {
        in = std::min<int64_t>(in, n - 1);
        last = std::clamp<int64_t>(last, in, n - 1);
    }
    last = std::max(last, in);
    Span s{ std::max<int64_t>(0, c.start), last - in + 1, in, timing };
    if (timing.timed()) {
        const MediaInfo& info = *timing.info;
        s.timeStart = std::clamp(c.sourceStart >= 0 ? c.sourceStart : info.timeOf(in), info.startTime(), info.timeOf(n));
        s.timeEnd = std::clamp(c.sourceEnd >= 0 ? c.sourceEnd : info.timeOf(last + 1), s.timeStart, info.timeOf(n));
        s.in = info.frameAt(s.timeStart);
        s.count = std::max<int64_t>(1, int64_t(std::ceil((s.timeEnd - s.timeStart) * timing.fps - 1e-6)));
    }
    return s;
}

int64_t lastSourceFrame(const TimelineClip& c, const LengthFn& len)
{
    const Span s = span(c, len);
    return s.last();
}

void normalize(TimelineDoc& doc, const LengthFn& len)
{
    std::map<TrackKey, int64_t> ends;
    for (TimelineClip& c : doc.clips) {
        c.track = std::clamp(c.track, 0, kMaxTracks - 1);
        c.in = std::max(0, c.in);
        if (c.out >= 0 && c.out < c.in) c.out = c.in;
        if (c.out < -1) c.out = -1;
        c.opacity = std::clamp(c.opacity, 0.0, 1.0);
        c.scale = std::clamp(c.scale, 0.01, 20.0);
        c.gainDb = std::clamp(c.gainDb, -60.0, 24.0);
        c.fadeIn = std::max(0, c.fadeIn);
        c.fadeOut = std::max(0, c.fadeOut);
        const TrackKey k = keyOf(c);
        if (c.start < 0) c.start = ends.count(k) ? ends[k] : 0;  // 1.0: back to back
        ends[k] = std::max(ends[k], span(c, len).end());
        ensureTrack(doc, c.kind, c.track);
    }
    if (doc.videoTracks.isEmpty()) doc.videoTracks << TrackState{};
    if (doc.audioTracks.isEmpty()) doc.audioTracks << TrackState{};
    if (doc.noteTracks.isEmpty()) doc.noteTracks << TrackState{};
    for (TimelineClip& c : doc.clips)
        if (const TimelineClip* p = doc.clip(c.link); !p || p->kind == c.kind) c.link.clear();
    sortClips(doc);
}

int64_t duration(const TimelineDoc& doc, const LengthFn& len)
{
    int64_t end = 0;
    for (const TimelineClip& c : doc.clips) end = std::max(end, span(c, len).end());
    return end;
}

bool trackVisible(const TimelineDoc& doc, Kind kind, int track)
{
    const QVector<TrackState>& list = doc.tracks(kind);
    return track < 0 || track >= list.size() || list[track].visible;
}

bool trackLocked(const TimelineDoc& doc, Kind kind, int track)
{
    const QVector<TrackState>& list = doc.tracks(kind);
    return track >= 0 && track < list.size() && list[track].locked;
}

QString trackName(const TimelineDoc& doc, Kind kind, int track)
{
    const QVector<TrackState>& list = doc.tracks(kind);
    if (track >= 0 && track < list.size() && !list[track].name.trimmed().isEmpty()) return list[track].name;
    return (kind == Kind::Video ? QStringLiteral("V") : QStringLiteral("A")) + QString::number(track + 1);
}

bool trackAudible(const TimelineDoc& doc, int track)
{
    bool anySolo = false;
    for (const TrackState& t : doc.audioTracks) anySolo = anySolo || t.solo;
    const TrackState t = doc.audioTracks.value(track);
    return t.visible && (!anySolo || t.solo);
}

Sequence primary(const TimelineDoc& doc, const LengthFn& len, const OnlineFn& online)
{
    struct Cand {
        const TimelineClip* clip;
        Span s;
    };
    QVector<Cand> cands;
    QVector<int64_t> cuts;
    const int64_t total = duration(doc, len);
    cuts << 0 << total;
    for (const TimelineClip& c : doc.clips) {
        if (!c.isVideo() || !c.enabled || !trackVisible(doc, Kind::Video, c.track)) continue;
        if (online && !online(c.sourceId)) continue;
        const Span s = span(c, len);
        cands.append({ &c, s });
        cuts << s.start << s.end();
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    Sequence seq;
    for (int i = 0; i + 1 < cuts.size(); ++i) {
        const int64_t a = cuts[i], b = std::min(cuts[i + 1], total);
        if (b <= a) continue;
        const Cand* best = nullptr;
        bool bestPlain = false;
        for (const Cand& c : cands) {
            if (c.s.start > a || c.s.end() < b) continue;
            const bool plain = !c.clip->hasTransform();
            if (!best || (plain && !bestPlain) || (plain == bestPlain && c.clip->track > best->clip->track)) {
                best = &c;
                bestPlain = plain;
            }
        }
        if (!best) {
            seq.appendGap(b - a);
            continue;
        }
        const int64_t offset = best->s.sourceAt(a - best->s.start);
        if (!seq.segments.isEmpty()) {
            Sequence::Segment& last = seq.segments.back();
            if (last.clipId == best->clip->uid) {
                last.count += b - a;
                seq.total += b - a;
                continue;
            }
        }
        seq.append(best->clip->sourceId, b - a, offset, best->clip->uid, best->s.timing,
                   best->s.timing.timed() ? best->s.timeAt(a - best->s.start) : 0);
    }
    return seq;
}

QVector<Layer> layersAt(const TimelineDoc& doc, int64_t frame, const LengthFn& len, const OnlineFn& online)
{
    QVector<Layer> out;
    for (const TimelineClip& c : doc.clips) {
        if (!c.isVideo() || !c.enabled || !trackVisible(doc, Kind::Video, c.track)) continue;
        if (online && !online(c.sourceId)) continue;
        const Span s = span(c, len);
        if (s.contains(frame)) out.append({ &c, s.sourceAt(frame - s.start) });
    }
    std::stable_sort(out.begin(), out.end(), [](const Layer& a, const Layer& b) { return a.clip->track < b.clip->track; });
    return out;
}

const TimelineClip* clipAt(const TimelineDoc& doc, Kind kind, int track, int64_t frame, const LengthFn& len)
{
    for (const TimelineClip& c : doc.clips)
        if (c.kind == kind && c.track == track && span(c, len).contains(frame)) return &c;
    return nullptr;
}

QVector<int64_t> editPoints(const TimelineDoc& doc, const LengthFn& len)
{
    QVector<int64_t> out{ 0 };
    for (const TimelineClip& c : doc.clips) {
        if (c.isVideo() && !trackVisible(doc, Kind::Video, c.track)) continue;
        const Span s = span(c, len);
        out << s.start << s.end();
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

QSet<QString> withLinks(const TimelineDoc& doc, const QSet<QString>& uids)
{
    QSet<QString> out = uids;
    for (const TimelineClip& c : doc.clips) {
        if (c.link.isEmpty()) continue;
        if (uids.contains(c.uid)) out.insert(c.link);
        if (uids.contains(c.link)) out.insert(c.uid);
    }
    return out;
}

// ============================================================================ editing

bool clearRange(TimelineDoc& doc, Kind kind, int track, int64_t a, int64_t b, const QSet<QString>& except, const LengthFn& len)
{
    if (b <= a) return false;
    bool changed = false;
    QVector<TimelineClip> added;
    QStringList removed;
    for (int i = int(doc.clips.size()) - 1; i >= 0; --i) {
        TimelineClip& c = doc.clips[i];
        if (c.kind != kind || c.track != track || except.contains(c.uid)) continue;
        const Span s = span(c, len);
        if (s.end() <= a || s.start >= b) continue;
        changed = true;
        if (s.start >= a && s.end() <= b) {  // covered: gone
            removed << c.uid;
            doc.clips.removeAt(i);
            continue;
        }
        if (s.start < a && s.end() > b) {  // the range is inside: keep both ends
            TimelineClip tail = c;
            tail.uid = TimelineClip::newUid();
            tail.link.clear();
            tail.start = b;
            s.setIn(tail, b - s.start);
            tail.fadeIn = 0;
            s.setEnd(c, a - s.start);
            c.fadeOut = 0;
            added << tail;
            continue;
        }
        if (s.start < a) {  // keep the head
            s.setEnd(c, a - s.start);
            c.fadeOut = 0;
        } else {  // keep the tail
            s.setIn(c, b - s.start);
            c.start = b;
            c.fadeIn = 0;
        }
    }
    for (const QString& uid : removed) unlinkPartnerOf(doc, uid);
    doc.clips += added;
    return changed;
}

bool moveOverwrite(TimelineDoc& doc, const QSet<QString>& uids, int64_t delta, int videoTrackDelta, int audioTrackDelta,
                   bool duplicate, const LengthFn& len, QStringList* created)
{
    QVector<TimelineClip> moving;
    for (const TimelineClip& c : doc.clips)
        if (uids.contains(c.uid) && !trackLocked(doc, c.kind, c.track)) moving << c;
    if (moving.isEmpty()) return false;
    int64_t minStart = kFar;
    int minVideo = kMaxTracks, minAudio = kMaxTracks, maxVideo = 0, maxAudio = 0;
    for (const TimelineClip& c : moving) {
        minStart = std::min(minStart, span(c, len).start);
        (c.isVideo() ? minVideo : minAudio) = std::min(c.isVideo() ? minVideo : minAudio, c.track);
        (c.isVideo() ? maxVideo : maxAudio) = std::max(c.isVideo() ? maxVideo : maxAudio, c.track);
    }
    delta = std::max(delta, -minStart);
    videoTrackDelta = std::clamp(videoTrackDelta, -minVideo, kMaxTracks - 1 - maxVideo);
    audioTrackDelta = std::clamp(audioTrackDelta, -minAudio, kMaxTracks - 1 - maxAudio);
    if (delta == 0 && videoTrackDelta == 0 && audioTrackDelta == 0 && !duplicate) return false;
    for (const TimelineClip& c : moving)
        if (trackLocked(doc, c.kind, c.track + (c.isVideo() ? videoTrackDelta : audioTrackDelta))) return false;

    QHash<QString, QString> renamed;
    if (duplicate)
        for (const TimelineClip& c : moving) renamed[c.uid] = TimelineClip::newUid();
    else
        doc.clips.removeIf([&](const TimelineClip& c) { return uids.contains(c.uid) && !trackLocked(doc, c.kind, c.track); });
    for (TimelineClip& c : moving) {
        const Span s = span(c, len);
        c.start = s.start + delta;
        c.track += c.isVideo() ? videoTrackDelta : audioTrackDelta;
        if (duplicate) {
            c.uid = renamed[c.uid];
            c.link = renamed.value(c.link);
        }
        ensureTrack(doc, c.kind, c.track);
    }
    for (const TimelineClip& c : moving) {
        const Span s = span(c, len);
        clearRange(doc, c.kind, c.track, s.start, s.end(), {}, len);
    }
    doc.clips += moving;
    if (created)
        for (const TimelineClip& c : moving) *created << c.uid;
    sortClips(doc);
    return true;
}

int64_t insertionPoint(const TimelineDoc& doc, const QSet<QString>& uids, Kind kind, int track, int64_t frame, const LengthFn& len)
{
    TimelineDoc rest = doc;
    remove(rest, uids, true, len);
    QVector<int64_t> points{ 0 };
    for (const TimelineClip& c : rest.clips)
        if (c.kind == kind && c.track == track) {
            const Span s = span(c, len);
            points << s.start << s.end();
        }
    int64_t best = 0;
    for (int64_t p : points)
        if (std::abs(p - frame) < std::abs(best - frame)) best = p;
    return best;
}

bool moveInsert(TimelineDoc& doc, const QSet<QString>& uids, int64_t insertAt, int videoTrackDelta, int audioTrackDelta,
                const LengthFn& len)
{
    QVector<TimelineClip> moving;
    for (const TimelineClip& c : doc.clips)
        if (uids.contains(c.uid) && !trackLocked(doc, c.kind, c.track)) moving << c;
    if (moving.isEmpty()) return false;
    int64_t first = kFar, last = 0;
    for (const TimelineClip& c : moving) {
        const Span s = span(c, len);
        first = std::min(first, s.start);
        last = std::max(last, s.end());
    }
    const TimelineDoc before = doc;
    QSet<QString> ids;
    for (const TimelineClip& c : moving) ids.insert(c.uid);
    remove(doc, ids, true, len);
    insertAt = std::max<int64_t>(0, insertAt);
    QSet<TrackKey> targets;
    for (TimelineClip& c : moving) {
        c.start = insertAt + (span(c, len).start - first);
        c.track = std::clamp(c.track + (c.isVideo() ? videoTrackDelta : audioTrackDelta), 0, kMaxTracks - 1);
        if (trackLocked(doc, c.kind, c.track)) {
            doc = before;
            return false;
        }
        ensureTrack(doc, c.kind, c.track);
        targets.insert(keyOf(c));
    }
    for (const TrackKey& k : targets) cutAt(doc, k, insertAt, len);
    rippleOpen(doc, targets, insertAt, last - first, {}, len, true);
    doc.clips += moving;
    sortClips(doc);
    return !doc.sameLayout(before);
}

int64_t clampTrim(const TimelineDoc& doc, const QSet<QString>& uids, int edge, int64_t delta, bool ripple, const LengthFn& len)
{
    int64_t lo = -kFar, hi = kFar;
    bool any = false;
    for (const TimelineClip& c : doc.clips) {
        if (!uids.contains(c.uid) || trackLocked(doc, c.kind, c.track)) continue;
        any = true;
        const Span s = span(c, len);
        const int64_t n = len ? len(c.sourceId).frames : 0;
        if (edge < 0) {
            lo = std::max(lo, s.timing.timed() ? -int64_t(std::floor((s.timeStart - s.timing.info->startTime()) * s.timing.fps + 1e-6)) : -s.in);
            hi = std::min(hi, s.count - 1);  // at least one frame
            if (!ripple) {
                lo = std::max(lo, -s.start);
                lo = std::max(lo, prevEnd(doc, c, len) - s.start);
            }
        } else {
            const int64_t room = s.timing.timed()
                ? int64_t(std::floor((s.timing.info->timeOf(n) - s.timeEnd) * s.timing.fps + 1e-6))
                : (n > 0 ? n - s.in - s.count : int64_t(0));
            hi = std::min(hi, room);
            lo = std::max(lo, 1 - s.count);
            if (!ripple) hi = std::min(hi, nextStart(doc, c, len) - s.end());
        }
    }
    if (!any || lo > hi) return 0;
    return std::clamp(delta, lo, hi);
}

bool trim(TimelineDoc& doc, const QSet<QString>& uids, int edge, int64_t delta, bool ripple, const LengthFn& len)
{
    const int64_t d = clampTrim(doc, uids, edge, delta, ripple, len);
    if (d == 0) return false;
    QSet<TrackKey> tracks;
    QSet<int64_t> points;
    std::map<TrackKey, int64_t> pointOf;
    for (TimelineClip& c : doc.clips) {
        if (!uids.contains(c.uid) || trackLocked(doc, c.kind, c.track)) continue;
        const Span s = span(c, len);
        if (edge < 0) {
            s.setIn(c, d);
            if (!ripple) c.start = s.start + d;
            if (c.out >= 0 && c.out < c.in) c.out = c.in;
        } else {
            s.setEnd(c, s.count + d);
            if (s.timing.timed()) {
                c.sourceEnd = s.timeEnd + d / s.timing.fps;
                c.out = int(s.timing.info->frameAt(c.sourceEnd - 1e-7));
            }
        }
        tracks.insert(keyOf(c));
        pointOf[keyOf(c)] = s.end();
        points.insert(s.end());
    }
    if (ripple) {
        const int64_t shift = edge < 0 ? -d : d;
        const bool sync = points.size() == 1;
        const int64_t point = sync ? *points.begin() : 0;
        for (const TrackKey& k : allTracks(doc)) {
            if (tracks.contains(k)) {
                shiftFrom(doc, k, pointOf[k], shift, uids);
                continue;
            }
            if (!sync || lockedKey(doc, k)) continue;
            if (shift < 0 ? emptyIn(doc, k, point + shift, point, uids, len) : !crosses(doc, k, point, uids, len))
                shiftFrom(doc, k, point, shift, uids);
        }
    }
    sortClips(doc);
    return true;
}

bool split(TimelineDoc& doc, const QSet<QString>& uids, int64_t frame, const LengthFn& len, QStringList* created)
{
    QHash<QString, QString> tailOf;
    QVector<TimelineClip> tails;
    for (TimelineClip& c : doc.clips) {
        if (!uids.contains(c.uid) || trackLocked(doc, c.kind, c.track)) continue;
        const Span s = span(c, len);
        if (frame <= s.start || frame >= s.end()) continue;
        TimelineClip tail = c;
        tail.uid = TimelineClip::newUid();
        tail.start = frame;
        s.setIn(tail, frame - s.start);
        tail.fadeIn = 0;
        s.setEnd(c, frame - s.start);
        c.fadeOut = 0;
        tailOf[c.uid] = tail.uid;
        tails << tail;
    }
    if (tails.isEmpty()) return false;
    for (TimelineClip& t : tails) {
        t.link = tailOf.value(t.link);  // the partner's tail, when it was cut too
        if (created) *created << t.uid;
    }
    // A partner that wasn't cut stays linked to the head only.
    doc.clips += tails;
    sortClips(doc);
    return true;
}

bool remove(TimelineDoc& doc, const QSet<QString>& uids, bool ripple, const LengthFn& len)
{
    std::map<TrackKey, QVector<std::pair<int64_t, int64_t>>> ranges;
    for (const TimelineClip& c : doc.clips) {
        if (!uids.contains(c.uid) || trackLocked(doc, c.kind, c.track)) continue;
        const Span s = span(c, len);
        ranges[keyOf(c)].append({ s.start, s.end() });
    }
    if (ranges.empty()) return false;
    QStringList gone;
    doc.clips.removeIf([&](const TimelineClip& c) {
        const bool out = uids.contains(c.uid) && !trackLocked(doc, c.kind, c.track);
        if (out) gone << c.uid;
        return out;
    });
    for (const QString& uid : gone) unlinkPartnerOf(doc, uid);
    if (!ripple) return true;

    // Merge each track's removed ranges, then close them from the right so positions stay valid.
    QSet<std::pair<int64_t, int64_t>> distinct;
    for (auto& [k, list] : ranges) {
        std::sort(list.begin(), list.end());
        QVector<std::pair<int64_t, int64_t>> merged;
        for (const auto& r : list) {
            if (!merged.isEmpty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
            else merged << r;
        }
        list = merged;
        for (const auto& r : merged) distinct.insert(r);
    }
    const bool sync = distinct.size() == 1;
    if (sync) {
        const auto [a, b] = *distinct.begin();
        QSet<TrackKey> own;
        for (const auto& [k, list] : ranges) own.insert(k);
        rippleClose(doc, own, a, b, {}, len, true);
    } else {
        for (const auto& [k, list] : ranges)
            for (auto it = list.rbegin(); it != list.rend(); ++it) shiftFrom(doc, k, it->second, it->first - it->second, {});
    }
    sortClips(doc);
    return true;
}

std::pair<int64_t, int64_t> gapAt(const TimelineDoc& doc, Kind kind, int track, int64_t frame, const LengthFn& len)
{
    int64_t before = 0, after = kFar;
    for (const TimelineClip& c : doc.clips) {
        if (c.kind != kind || c.track != track) continue;
        const Span s = span(c, len);
        if (s.contains(frame)) return { -1, -1 };
        if (s.end() <= frame) before = std::max(before, s.end());
        if (s.start > frame) after = std::min(after, s.start);
    }
    if (after == kFar || after <= before) return { -1, -1 };
    return { before, after };
}

bool closeGap(TimelineDoc& doc, Kind kind, int track, int64_t frame, const LengthFn& len)
{
    if (trackLocked(doc, kind, track)) return false;
    const auto [a, b] = gapAt(doc, kind, track, frame, len);
    if (a < 0 || b <= a) return false;
    rippleClose(doc, { { int(kind), track } }, a, b, {}, len, true);
    sortClips(doc);
    return true;
}

bool insertClips(TimelineDoc& doc, QVector<TimelineClip> clips, int64_t frame, int videoTrack, int audioTrack, bool ripple,
                 const LengthFn& len)
{
    if (clips.isEmpty()) return false;
    frame = std::max<int64_t>(0, frame);
    videoTrack = std::clamp(videoTrack, 0, kMaxTracks - 1);
    audioTrack = std::clamp(audioTrack, 0, kMaxTracks - 1);
    if (trackLocked(doc, Kind::Video, videoTrack) || trackLocked(doc, Kind::Audio, audioTrack)) return false;
    // Video clips one after another; a linked sound clip starts with its picture.
    QHash<QString, int64_t> startOf;
    int64_t at = frame;
    for (TimelineClip& c : clips)
        if (c.isVideo()) {
            c.track = videoTrack;
            c.start = at;
            startOf[c.uid] = at;
            at += span(c, len).count;
        }
    for (TimelineClip& c : clips)
        if (c.isAudio()) {
            c.track = audioTrack;
            if (startOf.contains(c.link)) {
                c.start = startOf[c.link];
            } else {
                c.start = at;
                at += span(c, len).count;
            }
        }
    int64_t end = frame;
    for (const TimelineClip& c : clips) end = std::max(end, span(c, len).end());
    ensureTrack(doc, Kind::Video, videoTrack);
    ensureTrack(doc, Kind::Audio, audioTrack);
    QSet<TrackKey> targets;
    for (const TimelineClip& c : clips) targets.insert(keyOf(c));
    if (ripple) {
        for (const TrackKey& k : targets) cutAt(doc, k, frame, len);
        rippleOpen(doc, targets, frame, end - frame, {}, len, true);
    } else {
        for (const TimelineClip& c : clips) {
            const Span s = span(c, len);
            clearRange(doc, c.kind, c.track, s.start, s.end(), {}, len);
        }
    }
    doc.clips += clips;
    sortClips(doc);
    return true;
}

bool swapNeighbour(TimelineDoc& doc, const QString& uid, int direction, const LengthFn& len)
{
    const TimelineClip* self = doc.clip(uid);
    if (!self || trackLocked(doc, self->kind, self->track)) return false;
    const Span own = span(*self, len);
    const TimelineClip* other = nullptr;
    Span os;
    for (const TimelineClip& c : doc.clips) {
        if (c.uid == uid || c.kind != self->kind || c.track != self->track) continue;
        const Span s = span(c, len);
        if (direction < 0 ? s.end() <= own.start && (!other || s.start > os.start)
                          : s.start >= own.end() && (!other || s.start < os.start)) {
            other = &c;
            os = s;
        }
    }
    if (!other) return false;
    const QString otherUid = other->uid;
    const Span first = direction < 0 ? os : own, second = direction < 0 ? own : os;
    const QString firstUid = direction < 0 ? otherUid : uid, secondUid = direction < 0 ? uid : otherUid;
    const int64_t gap = second.start - first.end();
    const int64_t secondTo = first.start, firstTo = first.start + second.count + gap;
    const QHash<QString, int64_t> deltas{ { secondUid, secondTo - second.start }, { firstUid, firstTo - first.start } };
    for (TimelineClip& c : doc.clips) {
        if (deltas.contains(c.uid)) c.start += deltas[c.uid];
        else if (deltas.contains(c.link)) c.start = std::max<int64_t>(0, c.start + deltas[c.link]);
    }
    sortClips(doc);
    return true;
}

bool reorderTrack(TimelineDoc& doc, const QStringList& orderedUids, const LengthFn& len)
{
    int64_t at = kFar;
    for (const QString& uid : orderedUids)
        if (const TimelineClip* c = doc.clip(uid)) at = std::min(at, span(*c, len).start);
    if (at == kFar) return false;
    QHash<QString, int64_t> deltas;
    for (const QString& uid : orderedUids) {
        const TimelineClip* c = doc.clip(uid);
        if (!c) continue;
        const Span s = span(*c, len);
        deltas[uid] = at - s.start;
        at += s.count;
    }
    const TimelineDoc before = doc;
    for (TimelineClip& c : doc.clips) {
        if (deltas.contains(c.uid)) c.start += deltas[c.uid];
        else if (deltas.contains(c.link)) c.start = std::max<int64_t>(0, c.start + deltas[c.link]);
    }
    sortClips(doc);
    return !doc.sameLayout(before);
}

void trimTrackLists(TimelineDoc& doc)
{
    for (Kind kind : { Kind::Video, Kind::Audio }) {
        int used = 0;
        for (const TimelineClip& c : doc.clips)
            if (c.kind == kind) used = std::max(used, c.track + 1);
        QVector<TrackState>& list = doc.tracks(kind);
        while (list.size() > std::max(1, used)) list.removeLast();
    }
}

} // namespace tl
