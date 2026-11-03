#pragma once

#include "VideoFrame.h"

#include <QBitArray>

#include <algorithm>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

// Thread-safe LRU cache of decoded frames with a byte budget. Video frames are stored as
// planar Y'CbCr (see VideoFrame), so the same budget holds about 2.6x more 4:2:0 frames
// than the RGBA cache did.
class FrameCache {
public:
    using Ptr = std::shared_ptr<const VideoFrame>;

    void setBudget(qint64 bytes);
    // Effective limit: the budget, or less while the system is short of memory.
    qint64 budget() const;
    qint64 configuredBudget() const;
    // A temporary lower limit (memory pressure); <= 0 lifts it. Evicts down to it at once.
    void setPressureLimit(qint64 bytes);
    qint64 pressureLimit() const;
    // Evicts least recently used frames until at most `bytes` are cached (one-off).
    void trimTo(qint64 bytes);
    // Changes whenever frames are added or dropped (a cheap "did anything change" check).
    quint64 revision() const { return m_revision.load(); }
    qint64 usedBytes() const;
    bool hasRoomFor(qint64 bytes) const;

    Ptr get(int64_t index);
    bool contains(int64_t index) const;
    void insert(int64_t index, Ptr frame);
    void clear();
    void removeRange(int64_t base, int64_t count);
    QBitArray mask(int64_t base, int64_t frameCount) const;

private:
    void evictLocked();

    mutable std::mutex m_mutex;
    std::list<int64_t> m_lru;  // front = most recently used
    struct Entry {
        Ptr frame;
        std::list<int64_t>::iterator pos;
    };
    std::unordered_map<int64_t, Entry> m_map;
    qint64 limitLocked() const { return m_pressure > 0 ? std::min(m_budget, m_pressure) : m_budget; }
    qint64 m_used = 0;
    qint64 m_budget = qint64(4) << 30;
    qint64 m_pressure = 0;
    std::atomic<quint64> m_revision{ 0 };
};
