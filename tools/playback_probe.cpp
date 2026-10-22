#include "media/VideoDecoder.h"
#include "media/FrameServer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>
#include <algorithm>
#include <cmath>

// usage: playback_probe video [cacheMiB=256] [--cpu]
// Measures decode + cache delivery (not GPU display) and the cache cost of one frame.
class MeasuredDecoder : public FrameSource {
public:
    std::unique_ptr<VideoDecoder> decoder;
    std::atomic<int64_t> requests{0}, decoded{0}, seeks{0};
    explicit MeasuredDecoder(std::unique_ptr<VideoDecoder> value) : decoder(std::move(value)) {}
    const MediaInfo& info() const override { return decoder->info(); }
    bool decode(int64_t index, QImage& out, QString* error) override { return decoder->decode(index, out, error); }
    bool decodeFrame(int64_t index, VideoFrame& out, QString* error, const FrameSink* sink) override {
        ++requests;
        const auto before=decoder->stats();
        const bool result=decoder->decodeFrame(index,out,error,sink);
        decoded += decoder->stats().framesDecoded-before.framesDecoded;
        seeks += decoder->stats().seeks-before.seeks;
        return result;
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    QTextStream log(stdout);
    QStringList args; for (int i=1;i<argc;++i) args << QString::fromLocal8Bit(argv[i]);
    if (args.removeAll("--cpu")) VideoDecoder::setHardwareDecodingEnabled(false);
    if (args.isEmpty()) { log << "usage: playback_probe video [cacheMiB=256] [--cpu]\n"; return 1; }
    const QString path=args[0]; QString error;
    VideoDecoder sequential; QElapsedTimer clock; clock.start();
    if (!sequential.open(path,&error)) { log << error << '\n'; return 2; }
    const qint64 openMs=clock.elapsed();
    const auto info=sequential.info();
    const int count=int(std::min<int64_t>(180,info.frameCount));
    QImage image; clock.restart();
    for(int i=0;i<count;++i) if(!sequential.decode(i,image,&error)) { log<<error<<'\n'; return 3; }
    const double rgbaMs=clock.nsecsElapsed()/1e6;
    VideoFrame frame; clock.restart();
    for(int i=0;i<count;++i) if(!sequential.decodeFrame(i,frame,&error)) { log<<error<<'\n'; return 3; }
    const double planarMs=clock.nsecsElapsed()/1e6;
    const qint64 rgbaBytes=qint64(info.width)*info.height*4;
    log << QFileInfo(path).fileName() << " " << info.width << 'x' << info.height << " " << info.pixelFormat
        << " native_fps=" << info.fps << " decoder=\"" << info.decoderName << "\" open_index_ms=" << openMs << '\n'
        << "  decode_to_rgba_fps=" << count*1000./rgbaMs << " decode_to_cache_frame_fps=" << count*1000./planarMs
        << " sequential_seeks=" << sequential.stats().seeks << '\n'
        << "  cache_bytes_per_frame=" << frame.sizeInBytes() << " (RGBA would be " << rgbaBytes << ", saving "
        << QString::number(100.0*(1.0-double(frame.sizeInBytes())/rgbaBytes),'f',1) << "%)\n";
    auto decoder=std::make_unique<VideoDecoder>();
    if(!decoder->open(path,&error)) { log<<error<<'\n'; return 4; }
    auto cache=std::make_shared<FrameCache>();
    cache->setBudget(qint64(args.size()>1 ? args[1].toInt() : 256)<<20);
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
    log << "  server_cache_MiB=" << cache->budget()/(1024*1024) << " startup_ms=" << startupMs
        << " observed_delivery_fps=" << displayed/seconds << " deadlines_without_cached_frame=" << misses
        << " sampled_frames=" << durationFrames << " cache_used_MiB=" << cache->usedBytes()/double(1024*1024)
        << " server_decode_requests=" << stats->requests.load() << " server_decoded_frames=" << stats->decoded.load() << " server_seeks=" << stats->seeks.load()
        << " (decode/cache delivery only; excludes GPU/UI/audio)\n";
    return 0;
}
