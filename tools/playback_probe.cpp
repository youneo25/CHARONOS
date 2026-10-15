#include "media/VideoDecoder.h"
#include "media/FrameServer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>
#include <algorithm>
#include <cmath>

class MeasuredDecoder : public FrameSource {
public:
    std::unique_ptr<VideoDecoder> decoder;
    std::atomic<int64_t> requests{0}, decoded{0}, seeks{0};
    explicit MeasuredDecoder(std::unique_ptr<VideoDecoder> value) : decoder(std::move(value)) {}
    const MediaInfo& info() const override { return decoder->info(); }
    bool decode(int64_t index,QImage& out,QString* error,const FrameSink* sink) override {
        ++requests;
        const auto before=decoder->stats();
        const bool result=decoder->decode(index,out,error,sink);
        decoded += decoder->stats().framesDecoded-before.framesDecoded;
        seeks += decoder->stats().seeks-before.seeks;
        return result;
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    QTextStream log(stdout);
    if (argc<2) { log << "usage: playback_probe video [cacheMiB=256]\n"; return 1; }
    const QString path=QString::fromLocal8Bit(argv[1]); QString error;
    VideoDecoder sequential; QElapsedTimer clock; clock.start();
    if (!sequential.open(path,&error)) { log << error << '\n'; return 2; }
    const qint64 openMs=clock.elapsed();
    const auto info=sequential.info();
    const int count=int(std::min<int64_t>(180,info.frameCount));
    QImage image; clock.restart();
    for(int i=0;i<count;++i) if(!sequential.decode(i,image,&error)) { log<<error<<'\n'; return 3; }
    const double sequentialMs=clock.nsecsElapsed()/1e6;
    log << QFileInfo(path).fileName() << " " << info.width << 'x' << info.height << " native_fps=" << info.fps
        << " open_index_ms=" << openMs << " decode_convert_fps=" << count*1000./sequentialMs
        << " sequential_seeks=" << sequential.stats().seeks << '\n';
    auto decoder=std::make_unique<VideoDecoder>();
    if(!decoder->open(path,&error)) { log<<error<<'\n'; return 4; }
    auto cache=std::make_shared<FrameCache>();
    cache->setBudget(qint64(argc>2 ? QString::fromLocal8Bit(argv[2]).toInt() : 256)<<20);
    auto measured=std::make_unique<MeasuredDecoder>(std::move(decoder)); auto* stats=measured.get();
    FrameServer server(std::move(measured),cache,0);
    clock.restart(); server.request(0,true,0,info.frameCount-1);
    while(!server.isCached(0) && clock.elapsed()<10000) QThread::msleep(1);
    if(!server.isCached(0)) return 5;
    const qint64 startupMs=clock.elapsed();
    QThread::msleep(150); // short preroll, no full-range cache warm-up
    int displayed=1, misses=0, requested=-1, shown=0;
    const int durationFrames=int(std::min<int64_t>(std::llround(info.fps*3),info.frameCount-1));
    clock.restart();
    while(clock.nsecsElapsed() < qint64(durationFrames*1e9/info.fps)) {
        const int target=std::min(durationFrames,int(clock.nsecsElapsed()*info.fps/1e9));
        if (target != requested) {
            if (!server.isCached(target)) ++misses;
            server.request(target,true,0,info.frameCount-1); requested=target;
        }
        if(target != shown && server.isCached(target)) { shown=target; ++displayed; }
        QThread::msleep(1);
    }
    const double seconds=clock.nsecsElapsed()/1e9;
    server.setIdle(); QThread::msleep(50);
    log << "server_cache_MiB=" << cache->budget()/(1024*1024) << " startup_ms=" << startupMs
        << " observed_delivery_fps=" << displayed/seconds << " deadlines_without_cached_frame=" << misses
        << " sampled_frames=" << durationFrames << " cache_used_MiB=" << cache->usedBytes()/double(1024*1024)
        << " server_decode_requests=" << stats->requests.load() << " server_decoded_frames=" << stats->decoded.load() << " server_seeks=" << stats->seeks.load()
        << " (decode/cache delivery only; excludes GPU/UI/audio)\n";
    return 0;
}
