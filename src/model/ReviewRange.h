#pragma once

#include <QVector>
#include <algorithm>

// Inclusive frame ranges shared by both playback bars.
struct ReviewRange {
    qint64 first = 0;
    qint64 last = 0;
    bool operator==(const ReviewRange&) const = default;
};

inline QVector<ReviewRange> mergedReviewRanges(QVector<ReviewRange> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QVector<ReviewRange> out;
    for (const auto& r : ranges) {
        if (r.last < r.first) continue;
        if (!out.isEmpty() && r.first <= out.back().last + 1) out.back().last = std::max(out.back().last, r.last);
        else out.append(r);
    }
    return out;
}

inline bool inReviewRange(const QVector<ReviewRange>& ranges, qint64 frame)
{
    auto it = std::upper_bound(ranges.begin(), ranges.end(), frame,
                              [](qint64 f, const ReviewRange& r) { return f < r.first; });
    return it != ranges.begin() && frame <= std::prev(it)->last;
}
