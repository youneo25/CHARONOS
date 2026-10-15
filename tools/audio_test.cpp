// Checks audio decoding (beep onsets) and the playback clock of AudioEngine (muted).
//   audio_test <video-with-audio>
#include "media/AudioClip.h"
#include "media/AudioEngine.h"
#include "media/VideoDecoder.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QString err;
    QElapsedTimer t;
    t.start();
    auto clip = AudioClip::decode(QString::fromLocal8Bit(argv[1]), &err);
    if (!clip) {
        std::printf("no audio: %s\n", qPrintable(err));
        return 1;
    }
    std::printf("decoded %.3f s of audio in %lld ms, start time %.4f s\n", clip->duration(), t.elapsed(), clip->startTime);

    // Video frame <-> time mapping used for A/V sync (real timestamps, VFR-safe).
    VideoDecoder vdec;
    if (vdec.open(QString::fromLocal8Bit(argv[1]), &err)) {
        const MediaInfo& vi = vdec.info();
        std::printf("video: %lld frames, avg %.3f fps, first frame at %.4f s\n", (long long)vi.frameCount, vi.fps, vi.startTime());
        for (double s : { 3.0, 6.0, 9.0, 11.0 }) {
            const int64_t f = vi.frameAt(s);
            std::printf("  at %.1f s -> frame %lld (pts %.4f)\n", s, (long long)f, vi.timeOf(f));
        }
    }

    // Beep onsets: first sample above threshold after 0.2 s of quiet.
    int64_t quietSince = 0;
    for (int64_t i = 0; i < clip->frames(); ++i) {
        const int a = std::abs(clip->pcm[size_t(i) * 2]);
        if (a > 1500) {
            if (i - quietSince > clip->rate / 5) std::printf("  beep at %.4f s\n", clip->startTime + double(i) / clip->rate);
            quietSince = i;
        }
    }

    AudioEngine engine;
    if (!engine.available()) {
        std::printf("no output device\n");
        return 0;
    }
    engine.setVolume(0.0);  // silent check of the clock
    engine.play(clip, 2.0, 1.0);
    QElapsedTimer wall;
    wall.start();
    for (int k = 0; k < 6; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::printf("  wall %.3f s  audio position %.3f s (started at 2.000)\n", wall.nsecsElapsed() / 1e9, engine.position());
    }
    engine.play(clip, 5.0, 0.5);  // half speed
    wall.restart();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    std::printf("  half speed: wall %.3f s  audio position %.3f s (started at 5.000, expect ~5.5)\n", wall.nsecsElapsed() / 1e9,
                engine.position());
    engine.stop();
    return 0;
}
