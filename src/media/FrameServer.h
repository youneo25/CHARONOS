#pragma once

#include "FrameCache.h"
#include "FrameSource.h"

#include <QObject>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

// Owns one FrameSource and its background decode threads. Frames go into a cache that
// may be shared by several servers (one RAM budget per project); each server uses its
// own key range. The UI asks for a frame with request(); workers decode it first, then
// prefetch ahead of the playhead and, while the budget allows, fill the rest of the range.
class FrameServer : public QObject {
    Q_OBJECT
public:
    FrameServer(std::unique_ptr<FrameSource> source, std::shared_ptr<FrameCache> cache, int64_t keyBase,
                QObject* parent = nullptr);
    ~FrameServer() override;

    const MediaInfo& info() const { return m_info; }
    int64_t frameCount() const { return m_info.frameCount; }

    FrameCache::Ptr frame(int64_t index) { return m_cache->get(m_keyBase + index); }
    bool isCached(int64_t index) const { return m_cache->contains(m_keyBase + index); }
    QBitArray cachedMask() const { return m_cache->mask(m_keyBase, m_info.frameCount); }

    void request(int64_t index, bool playing, int64_t rangeStart, int64_t rangeEnd);
    void setIdle();  // stop background filling (another source is active)

signals:
    void frameReady(qint64 index);
    void decodeFailed(qint64 index, QString message);

private:
    void workerLoop();
    bool pickLocked(int64_t& out);

    std::unique_ptr<FrameSource> m_source;
    std::shared_ptr<FrameCache> m_cache;
    int64_t m_keyBase = 0;
    MediaInfo m_info;
    qint64 m_frameBytes = 0;

    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    int64_t m_want = -1;
    bool m_playing = false;
    bool m_fill = true;
    int64_t m_rangeStart = 0;
    int64_t m_rangeEnd = -1;
    int64_t m_fillCursor = 0;
    std::unordered_set<int64_t> m_inFlight;
    std::unordered_set<int64_t> m_failed;
};
