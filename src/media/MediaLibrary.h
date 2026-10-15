#pragma once

#include "FrameServer.h"

#include <QObject>
#include <QStringList>

#include <map>
#include <memory>

// Opens project sources and keeps one FrameServer per source, all sharing a single
// RAM budget.
class MediaLibrary : public QObject {
    Q_OBJECT
public:
    explicit MediaLibrary(QObject* parent = nullptr);
    ~MediaLibrary() override;

    // Opens a video file or an image sequence. `fps` is only used for sequences.
    static std::unique_ptr<FrameSource> openSource(bool sequence, const QString& path, const QStringList& frames,
                                                   double fps, QString* error);

    void add(const QString& id, std::unique_ptr<FrameSource> source);
    void remove(const QString& id);
    void clear();
    FrameServer* server(const QString& id) const;
    bool contains(const QString& id) const { return m_servers.count(id) != 0; }

    // Only `active` ids keep filling the cache in the background.
    void setActive(const QStringList& active);

    void setCacheBudget(qint64 bytes) { m_cache->setBudget(bytes); }
    void clearCache() { m_cache->clear(); }
    qint64 cacheBudget() const { return m_cache->budget(); }
    qint64 cacheUsed() const { return m_cache->usedBytes(); }

signals:
    void frameReady(const QString& id, qint64 index);
    void decodeFailed(const QString& id, qint64 index, const QString& message);

private:
    std::shared_ptr<FrameCache> m_cache;
    std::map<QString, std::unique_ptr<FrameServer>> m_servers;
    int64_t m_nextBase = 0;
};
