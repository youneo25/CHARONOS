#pragma once

#include <QString>
#include <QVector>
#include "media/MediaInfo.h"
#include <memory>

#include <algorithm>
#include <cstdint>

// Native source frames and the clock of the sequence using them. Without timing metadata
// the editing engine retains its frame-for-frame behaviour (also useful for offline media).
struct SourceTiming {
    int64_t frames = 0;
    std::shared_ptr<const MediaInfo> info;
    double fps = 0;
    SourceTiming(int64_t n = 0) : frames(n) {}
    bool timed() const { return info && fps > 0; }
};

// What the viewer plays: either a whole timeline or a single clip. Maps a global frame to
// (clip, source frame). A timeline clip may be trimmed, so a segment covers source frames
// from offset, using source timestamps when the timeline rate differs. On a multi-track timeline each segment is the clip the viewer
// edits at that time (the top visible one); frames with no picture are gap segments.
struct Sequence {
    struct Segment {
        QString sourceId;
        int64_t start = 0;   // first global frame
        int64_t count = 0;
        int64_t offset = 0;  // source frame shown at `start` (the clip's In point)
        QString clipId;      // timeline entry; empty when viewing a single source
        SourceTiming timing;
        double timeStart = 0;
        bool isGap() const { return sourceId.isEmpty(); }
        int64_t sourceFrame(int64_t relative) const {
            return timing.timed() ? timing.info->frameAt(timeStart + relative / timing.fps) : offset + relative;
        }
    };
    QVector<Segment> segments;
    int64_t total = 0;

    void append(const QString& id, int64_t count, int64_t offset = 0, const QString& clipId = QString(),
                SourceTiming timing = {}, double timeStart = 0)
    {
        if (count <= 0) return;
        segments.append({ id, total, count, std::max<int64_t>(0, offset), clipId, std::move(timing), timeStart });
        total += count;
    }

    void appendGap(int64_t count)
    {
        if (count <= 0) return;
        if (!segments.isEmpty() && segments.back().isGap()) {
            segments.back().count += count;
        } else {
            segments.append({ QString(), total, count, 0, QString() });
        }
        total += count;
    }

    int indexAt(int64_t global) const
    {
        if (segments.isEmpty() || total <= 0) return -1;
        global = std::clamp<int64_t>(global, 0, total - 1);
        auto it = std::upper_bound(segments.begin(), segments.end(), global,
                                   [](int64_t g, const Segment& s) { return g < s.start; });
        return int(it - segments.begin()) - 1;
    }

    const Segment* at(int64_t global) const
    {
        const int i = indexAt(global);
        return i < 0 ? nullptr : &segments[i];
    }

    int indexOfClip(const QString& clipId) const
    {
        for (int i = 0; i < segments.size(); ++i)
            if (segments[i].clipId == clipId) return i;
        return -1;
    }

    // Global frame of source frame `local` inside segment `s`, or -1 when trimmed away.
    static int64_t globalIn(const Segment& s, int64_t local)
    {
        if (s.timing.timed()) {
            const double f = (s.timing.info->timeOf(local) - s.timeStart) * s.timing.fps;
            if (local < s.sourceFrame(0) || f >= s.count - 1e-6) return -1;
            return s.start + std::clamp<int64_t>(int64_t(std::ceil(f - 1e-6)), 0, s.count - 1);
        }
        if (local < s.offset || local >= s.offset + s.count) return -1;
        return s.start + local - s.offset;
    }

    // First global frame showing source frame `local` of `id` (the segment `prefer` wins when it
    // contains it, so a source used twice keeps the playhead on the same instance), or -1.
    int64_t globalFor(const QString& id, int64_t local, int prefer = -1) const
    {
        if (prefer >= 0 && prefer < segments.size() && segments[prefer].sourceId == id)
            if (const int64_t g = globalIn(segments[prefer], local); g >= 0) return g;
        if (id.isEmpty()) return -1;
        for (const Segment& s : segments)
            if (s.sourceId == id)
                if (const int64_t g = globalIn(s, local); g >= 0) return g;
        return -1;
    }
};
