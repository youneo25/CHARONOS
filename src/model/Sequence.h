#pragma once

#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdint>

// What the viewer plays: either a whole timeline (clips back to back) or a single clip.
// Maps a global frame to (clip, local frame).
struct Sequence {
    struct Segment {
        QString sourceId;
        int64_t start = 0;  // first global frame
        int64_t count = 0;
    };
    QVector<Segment> segments;
    int64_t total = 0;

    void append(const QString& id, int64_t count)
    {
        if (count <= 0) return;
        segments.append({ id, total, count });
        total += count;
    }

    int indexAt(int64_t global) const
    {
        if (segments.isEmpty()) return -1;
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

    int64_t globalFor(const QString& id, int64_t local) const
    {
        for (const Segment& s : segments)
            if (s.sourceId == id) return s.start + local;
        return -1;
    }
};
