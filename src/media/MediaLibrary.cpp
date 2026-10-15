#include "MediaLibrary.h"

#include "ImageSequenceSource.h"
#include "VideoDecoder.h"

MediaLibrary::MediaLibrary(QObject* parent)
    : QObject(parent)
    , m_cache(std::make_shared<FrameCache>())
{
}

MediaLibrary::~MediaLibrary() { clear(); }

std::unique_ptr<FrameSource> MediaLibrary::openSource(bool sequence, const QString& path, const QStringList& frames,
                                                      double fps, QString* error)
{
    if (sequence) {
        auto seq = std::make_unique<ImageSequenceSource>();
        if (seq->open(frames, fps, error)) return seq;
        return {};
    }
    auto dec = std::make_unique<VideoDecoder>();
    if (dec->open(path, error)) return dec;
    return {};
}

void MediaLibrary::add(const QString& id, std::unique_ptr<FrameSource> source)
{
    remove(id);
    m_nextBase += int64_t(1) << 32;  // each source gets its own 4G-frame key range in the shared cache
    auto server = std::make_unique<FrameServer>(std::move(source), m_cache, m_nextBase);
    connect(server.get(), &FrameServer::frameReady, this, [this, id](qint64 index) { emit frameReady(id, index); });
    connect(server.get(), &FrameServer::decodeFailed, this,
            [this, id](qint64 index, const QString& msg) { emit decodeFailed(id, index, msg); });
    m_servers[id] = std::move(server);
}

void MediaLibrary::remove(const QString& id) { m_servers.erase(id); }

void MediaLibrary::clear()
{
    m_servers.clear();
    m_cache->clear();
}

FrameServer* MediaLibrary::server(const QString& id) const
{
    auto it = m_servers.find(id);
    return it == m_servers.end() ? nullptr : it->second.get();
}

void MediaLibrary::setActive(const QStringList& active)
{
    for (auto& [id, server] : m_servers)
        if (!active.contains(id)) server->setIdle();
}
