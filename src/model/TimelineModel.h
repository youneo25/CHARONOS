#pragma once

#include "Project.h"
#include "Sequence.h"

#include <QSet>

#include <functional>

// Multi-track timeline layout and editing, kept free of UI so every operation can be checked
// on its own. Clips sit at explicit frames on their tracks; the operations here keep a track
// free of overlaps (overwrite) or push the following clips (ripple), like an editing program.
namespace tl {

using Kind = TimelineClip::Kind;
// Length of a source in frames (0 = unknown, e.g. offline media that was never opened).
using LengthFn = std::function<SourceTiming(const QString& sourceId)>;
// Whether a source can be shown (its media is open).
using OnlineFn = std::function<bool(const QString& sourceId)>;

struct Span {
    int64_t start = 0, count = 0, in = 0;
    SourceTiming timing;
    double timeStart = 0, timeEnd = 0;
    int64_t end() const { return start + count; }  // exclusive
    bool contains(int64_t f) const { return f >= start && f < start + count; }
    double timeAt(int64_t relative) const { return timeStart + relative / timing.fps; }
    int64_t sourceAt(int64_t relative) const {
        return timing.timed() ? timing.info->frameAt(timeAt(relative)) : in + relative;
    }
    int64_t last() const { return timing.timed() ? timing.info->frameAt(timeEnd - 1e-7) : in + count - 1; }
    // Preserve exact edit times, including boundaries between native source frames.
    void setIn(TimelineClip& c, int64_t relative) const {
        c.in = int(sourceAt(relative));
        if (timing.timed()) { c.sourceStart = timeAt(relative); c.sourceEnd = timeEnd; }
    }
    void setEnd(TimelineClip& c, int64_t relative) const {
        c.out = int(timing.timed() ? timing.info->frameAt(timeAt(relative) - 1e-7) : in + relative - 1);
        if (timing.timed()) { c.sourceEnd = timeAt(relative); c.sourceStart = timeStart; }
    }
};

// Where a clip sits once its Out point is resolved against the source length.
Span span(const TimelineClip& c, const LengthFn& len);
int64_t lastSourceFrame(const TimelineClip& c, const LengthFn& len);

// Places 1.0 clips (start = -1) right after the previous clip of their track, clamps values,
// sizes the track lists and orders the clip list by track and time.
void normalize(TimelineDoc& doc, const LengthFn& len);

// Last frame + 1 of any clip.
int64_t duration(const TimelineDoc& doc, const LengthFn& len);

bool trackVisible(const TimelineDoc& doc, Kind kind, int track);
bool trackLocked(const TimelineDoc& doc, Kind kind, int track);
QString trackName(const TimelineDoc& doc, Kind kind, int track);
// Audio heard from a track: not muted and, when any track is soloed, soloed itself.
bool trackAudible(const TimelineDoc& doc, int track);

// The picture the viewer edits at each frame: the top visible clip, preferring one without a
// transform (a picture-in-picture inset over a full frame leaves the full frame editable).
// Frames without any visible clip are gap segments; the sequence runs to duration().
Sequence primary(const TimelineDoc& doc, const LengthFn& len, const OnlineFn& online);

struct Layer {
    const TimelineClip* clip = nullptr;
    int64_t local = 0;  // source frame
};
// Visible video clips at a frame, bottom track first (drawing order).
QVector<Layer> layersAt(const TimelineDoc& doc, int64_t frame, const LengthFn& len, const OnlineFn& online);

// Clip under a frame on one track, or null.
const TimelineClip* clipAt(const TimelineDoc& doc, Kind kind, int track, int64_t frame, const LengthFn& len);

// Clip boundaries of every visible track, sorted (for edit-point navigation and snapping).
QVector<int64_t> editPoints(const TimelineDoc& doc, const LengthFn& len);

// Adds the linked partners of the clips (a video clip and its sound move together).
QSet<QString> withLinks(const TimelineDoc& doc, const QSet<QString>& uids);

// ---- editing; every function returns true when it changed the document

// Overwrite: [a, b) of a track becomes empty, trimming, splitting or removing what is there.
bool clearRange(TimelineDoc& doc, Kind kind, int track, int64_t a, int64_t b, const QSet<QString>& except, const LengthFn& len);

// Moves clips by `delta` frames and across tracks (per kind), overwriting what they land on.
// With `duplicate` the originals stay and copies (with new uids, links kept between copies) move.
bool moveOverwrite(TimelineDoc& doc, const QSet<QString>& uids, int64_t delta, int videoTrackDelta, int audioTrackDelta,
                   bool duplicate, const LengthFn& len, QStringList* created = nullptr);

// Ripple move: the clips leave their place (later clips close up) and are inserted at
// `insertAt` (a frame of the timeline without them); later clips move aside.
bool moveInsert(TimelineDoc& doc, const QSet<QString>& uids, int64_t insertAt, int videoTrackDelta, int audioTrackDelta,
                const LengthFn& len);

// Frame of the timeline without `uids` that a ripple move to `frame` on (kind, track) uses:
// the nearest clip boundary there.
int64_t insertionPoint(const TimelineDoc& doc, const QSet<QString>& uids, Kind kind, int track, int64_t frame, const LengthFn& len);

// Moves an edge of the clips by `delta` frames (edge -1 = head / In, +1 = tail / Out). The
// same delta applies to all of them, limited so every clip stays valid. Without ripple a clip
// stops at its neighbour; with ripple the later clips of those tracks follow.
bool trim(TimelineDoc& doc, const QSet<QString>& uids, int edge, int64_t delta, bool ripple, const LengthFn& len);
// The delta trim() would actually apply.
int64_t clampTrim(const TimelineDoc& doc, const QSet<QString>& uids, int edge, int64_t delta, bool ripple, const LengthFn& len);

// Cuts the clips at `frame` (clips that don't cross it are left alone). New tail uids go to `created`.
bool split(TimelineDoc& doc, const QSet<QString>& uids, int64_t frame, const LengthFn& len, QStringList* created = nullptr);

// Lift (leave a gap) or ripple delete (later clips of those tracks close up).
bool remove(TimelineDoc& doc, const QSet<QString>& uids, bool ripple, const LengthFn& len);

// Closes the empty space under `frame` on a track: the following clips move left.
bool closeGap(TimelineDoc& doc, Kind kind, int track, int64_t frame, const LengthFn& len);
// The gap under `frame` on a track as [first, end), or {-1, -1}.
std::pair<int64_t, int64_t> gapAt(const TimelineDoc& doc, Kind kind, int track, int64_t frame, const LengthFn& len);

// Puts new clips (video, optionally with their linked sound) at `frame`; `ripple` pushes the
// clips after that point instead of overwriting them. The clips' start and track are set here.
bool insertClips(TimelineDoc& doc, QVector<TimelineClip> clips, int64_t frame, int videoTrack, int audioTrack, bool ripple,
                 const LengthFn& len);

// Swaps a clip with its neighbour on the track (direction -1 / +1), keeping the gap between them.
bool swapNeighbour(TimelineDoc& doc, const QString& uid, int direction, const LengthFn& len);

// Packs the video clips of track V1 in the given order from the first clip's start (1.0's
// "order" commands); their linked sound follows.
bool reorderTrack(TimelineDoc& doc, const QStringList& orderedUids, const LengthFn& len);

// Removes empty tracks at the top (keeps at least one of each kind).
void trimTrackLists(TimelineDoc& doc);

} // namespace tl
