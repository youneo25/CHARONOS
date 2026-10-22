#include "FrameServer.h"

#include <QThread>
#include <algorithm>
#include <utility>

FrameServer::FrameServer(std::unique_ptr<FrameSource> source, std::shared_ptr<FrameCache> cache, int64_t keyBase,
                         QObject* parent)
    : QObject(parent)
    , m_source(std::move(source))
    , m_cache(std::move(cache))
    , m_keyBase(keyBase)
    , m_info(m_source->info())
{
    // Until the first frame arrives assume RGBA; video then reports its planar size (1.5 B/px for 4:2:0).
    m_frameBytes = std::max<qint64>(1, qint64(m_info.width) * m_info.height * 4);
    m_want = 0;
    m_rangeEnd = m_info.frameCount - 1;
    const int workers = m_source->supportsParallelDecode() ? std::clamp(QThread::idealThreadCount() / 2, 2, 8) : 1;
    for (int i = 0; i < workers; ++i) m_workers.emplace_back([this] { workerLoop(); });
}

FrameServer::~FrameServer()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_workers) t.join();
    m_cache->removeRange(m_keyBase, m_info.frameCount);
}

void FrameServer::request(int64_t index, bool playing, int64_t rangeStart, int64_t rangeEnd)
{
    {
        std::lock_guard lock(m_mutex);
        m_want = std::clamp<int64_t>(index, 0, m_info.frameCount - 1);
        m_playing = playing;
        m_fill = true;
        m_rangeStart = std::max<int64_t>(0, rangeStart);
        m_rangeEnd = std::min<int64_t>(m_info.frameCount - 1, rangeEnd);
        if (m_rangeEnd < m_rangeStart) {
            m_rangeStart = 0;
            m_rangeEnd = m_info.frameCount - 1;
        }
    }
    m_cv.notify_all();
}

void FrameServer::setIdle()
{
    std::lock_guard lock(m_mutex);
    m_playing = false;
    m_fill = false;
    m_extra.clear();
}

void FrameServer::requestExtra(QVector<int64_t> frames)
{
    {
        std::lock_guard lock(m_mutex);
        m_extra = std::move(frames);
    }
    m_cv.notify_all();
}

void FrameServer::requestPoster(int64_t frame)
{
    if (frame < 0 || frame >= m_info.frameCount || m_cache->contains(m_keyBase + frame)) return;
    {
        std::lock_guard lock(m_mutex);
        m_poster = frame;
    }
    m_cv.notify_all();
}

bool FrameServer::pickLocked(int64_t& out)
{
    const int64_t count = m_info.frameCount;
    if (count <= 0) return false;

    auto usable = [&](int64_t i) {
        return i >= 0 && i < count && !m_inFlight.count(i) && !m_failed.count(i) && !m_cache->contains(m_keyBase + i);
    };
    if (m_fill && m_want >= 0 && usable(m_want)) {
        out = m_want;
        return true;
    }
    // Onion-skin ghosts and a timeline poster frame come right after the wanted frame, even while
    // this source isn't the one being played (they are few and on screen).
    for (int64_t f : m_extra)
        if (usable(f)) {
            out = f;
            return true;
        }
    if (m_poster >= 0) {
        const int64_t p = std::exchange(m_poster, -1);
        if (usable(p)) {
            out = p;
            return true;
        }
    }
    if (!m_fill || m_want < 0) return false;

    const int64_t lo = m_rangeStart;
    const int64_t hi = m_rangeEnd;
    const int64_t len = hi - lo + 1;
    const bool inRange = m_want >= lo && m_want <= hi;
    auto at = [&](int64_t offset) -> int64_t {
        const int64_t p = m_want + offset;
        if (!inRange) return p;
        return lo + (((p - lo) % len) + len) % len;
    };

    const int fps = std::max(1, int(m_info.fps + 0.5));
    // Reserve room for A, B and the next timeline clip. An unbounded two-second
    // window can exceed the entire cache at UHD/high FPS and evict the playhead
    // while repeatedly decoding the same GOP.
    const int64_t capacity = std::max<int64_t>(1, m_cache->budget() / std::max<qint64>(1, m_frameBytes) / 3);
    const int64_t ahead = std::min({len - 1, capacity - 1, int64_t(m_playing ? std::max(12, fps * 2) : std::max(6, fps / 2))});
    for (int64_t k = 1; k <= ahead; ++k)
        if (usable(at(k))) {
            out = at(k);
            return true;
        }

    if (!m_playing)
        for (int64_t k = 1; k <= std::min<int64_t>(capacity - ahead - 1, std::max(3, fps / 2)); ++k) {
            const int64_t i = m_want - k;
            if (i < lo) break;
            if (usable(i)) {
                out = i;
                return true;
            }
        }

    // Background fill of the whole range, but only into free budget (never evicts).
    if (m_playing || !m_cache->hasRoomFor(m_frameBytes)) return false;
    const int64_t cursor = std::clamp(m_fillCursor, lo, hi);
    for (int64_t k = 0; k < len; ++k) {
        const int64_t i = lo + (cursor - lo + k) % len;
        if (usable(i)) {
            out = i;
            m_fillCursor = i == hi ? lo : i + 1;
            return true;
        }
    }
    return false;
}

void FrameServer::workerLoop()
{
    for (;;) {
        int64_t index = -1;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop || pickLocked(index); });
            if (m_stop) return;
            m_inFlight.insert(index);
        }

        // Frames decoded on the way to `index` (after a seek, the rest of the GOP) are cached when they
        // are near it, so stepping backwards or scrubbing nearby doesn't decode the GOP again.
        const int64_t window = std::min<int64_t>(std::max<int64_t>(48, int64_t(m_info.fps * 2)),
            std::max<int64_t>(0, m_cache->budget() / std::max<qint64>(1, m_frameBytes) / 3 - 1));
        FrameSink sink;
        sink.wants = [&](int64_t i) {
            if (std::abs(i - index) > window || m_cache->contains(m_keyBase + i)) return false;
            std::lock_guard lock(m_mutex);
            return !m_inFlight.count(i) && !m_stop;
        };
        sink.deliver = [&](int64_t i, VideoFrame&& f) {
            if (f.isNull()) return;
            m_frameBytes = std::max<qint64>(1, f.sizeInBytes());
            m_cache->insert(m_keyBase + i, std::make_shared<const VideoFrame>(std::move(f)));
            emit frameReady(i);
        };

        VideoFrame frame;
        QString error;
        bool ok = false;
        try {
            ok = m_source->decodeFrame(index, frame, &error, &sink) && !frame.isNull();
        } catch (const std::bad_alloc&) {
            // A huge frame against a nearly full address space: report it instead of terminating.
            error = QStringLiteral("Out of memory");
            ok = false;
        }
        if (ok) {
            m_frameBytes = std::max<qint64>(1, frame.sizeInBytes());
            m_cache->insert(m_keyBase + index, std::make_shared<const VideoFrame>(std::move(frame)));
        } else if (error.isEmpty()) {
            error = QStringLiteral("Decode failed");
        }
        {
            std::lock_guard lock(m_mutex);
            m_inFlight.erase(index);
            if (!ok) m_failed.insert(index);
        }
        m_cv.notify_all();

        if (ok)
            emit frameReady(index);
        else
            emit decodeFailed(index, error);
    }
}
