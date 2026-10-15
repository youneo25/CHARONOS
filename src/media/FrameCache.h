#pragma once

#include <QBitArray>
#include <QImage>

#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

// Thread-safe LRU cache of decoded frames with a byte budget.
class FrameCache {
public:
    using Ptr = std::shared_ptr<const QImage>;

    void setBudget(qint64 bytes);
    qint64 budget() const;
    qint64 usedBytes() const;
    bool hasRoomFor(qint64 bytes) const;

    Ptr get(int64_t index);
    bool contains(int64_t index) const;
    void insert(int64_t index, Ptr image);
    void clear();
    void removeRange(int64_t base, int64_t count);
    QBitArray mask(int64_t base, int64_t frameCount) const;

private:
    void evictLocked();

    mutable std::mutex m_mutex;
    std::list<int64_t> m_lru;  // front = most recently used
    struct Entry {
        Ptr image;
        std::list<int64_t>::iterator pos;
    };
    std::unordered_map<int64_t, Entry> m_map;
    qint64 m_used = 0;
    qint64 m_budget = qint64(4) << 30;
};
