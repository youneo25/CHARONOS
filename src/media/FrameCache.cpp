#include "FrameCache.h"

#include <algorithm>

namespace {
qint64 sizeOf(const FrameCache::Ptr& p) { return p ? p->sizeInBytes() : 0; }
}

void FrameCache::setBudget(qint64 bytes)
{
    std::lock_guard lock(m_mutex);
    m_budget = std::max<qint64>(bytes, qint64(256) << 20);
    evictLocked();
}

qint64 FrameCache::budget() const
{
    std::lock_guard lock(m_mutex);
    return m_budget;
}

qint64 FrameCache::usedBytes() const
{
    std::lock_guard lock(m_mutex);
    return m_used;
}

bool FrameCache::hasRoomFor(qint64 bytes) const
{
    std::lock_guard lock(m_mutex);
    return m_used + bytes <= m_budget;
}

FrameCache::Ptr FrameCache::get(int64_t index)
{
    std::lock_guard lock(m_mutex);
    auto it = m_map.find(index);
    if (it == m_map.end()) return {};
    m_lru.splice(m_lru.begin(), m_lru, it->second.pos);
    return it->second.frame;
}

bool FrameCache::contains(int64_t index) const
{
    std::lock_guard lock(m_mutex);
    return m_map.count(index) != 0;
}

void FrameCache::insert(int64_t index, Ptr frame)
{
    std::lock_guard lock(m_mutex);
    auto it = m_map.find(index);
    if (it != m_map.end()) {
        m_used -= sizeOf(it->second.frame);
        it->second.frame = std::move(frame);
        m_used += sizeOf(it->second.frame);
        m_lru.splice(m_lru.begin(), m_lru, it->second.pos);
    } else {
        m_lru.push_front(index);
        m_used += sizeOf(frame);
        m_map.emplace(index, Entry{ std::move(frame), m_lru.begin() });
    }
    evictLocked();
}

void FrameCache::clear()
{
    std::lock_guard lock(m_mutex);
    m_map.clear();
    m_lru.clear();
    m_used = 0;
}

void FrameCache::removeRange(int64_t base, int64_t count)
{
    std::lock_guard lock(m_mutex);
    for (auto it = m_map.begin(); it != m_map.end();) {
        if (it->first >= base && it->first < base + count) {
            m_used -= sizeOf(it->second.frame);
            m_lru.erase(it->second.pos);
            it = m_map.erase(it);
        } else {
            ++it;
        }
    }
}

QBitArray FrameCache::mask(int64_t base, int64_t frameCount) const
{
    QBitArray bits(static_cast<qsizetype>(std::max<int64_t>(frameCount, 0)));
    std::lock_guard lock(m_mutex);
    for (const auto& [key, entry] : m_map) {
        const int64_t index = key - base;
        if (index >= 0 && index < frameCount) bits.setBit(static_cast<qsizetype>(index));
    }
    return bits;
}

void FrameCache::evictLocked()
{
    // Never evict the most recently inserted frame, even if it alone exceeds the budget.
    while (m_used > m_budget && m_lru.size() > 1) {
        const int64_t victim = m_lru.back();
        m_lru.pop_back();
        auto it = m_map.find(victim);
        m_used -= sizeOf(it->second.frame);
        m_map.erase(it);
    }
}
