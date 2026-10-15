// Verifies frame-exact access: decodes every frame sequentially, then again in random
// order (forcing seeks), and checks that each frame index yields identical pixels.
//   frame_verify <video> [randomSamples]
#include "media/VideoDecoder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>

#include <cstdio>
#include <random>

static QByteArray hashOf(const QImage& img)
{
    const size_t h = qHashBits(img.constBits(), size_t(img.sizeInBytes()), 0x9e3779b9);
    return QByteArray(reinterpret_cast<const char*>(&h), sizeof h);
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage: frame_verify <video> [randomSamples]\n");
        return 2;
    }
    const QString path = QString::fromLocal8Bit(argv[1]);
    const int samples = argc > 2 ? std::atoi(argv[2]) : 300;

    VideoDecoder dec;
    QString err;
    QElapsedTimer t;
    t.start();
    if (!dec.open(path, &err)) {
        std::printf("open failed: %s\n", qPrintable(err));
        return 1;
    }
    const MediaInfo& info = dec.info();
    std::printf("%dx%d  %.3f fps  %lld frames  %s %s%s  (index %.0f ms)\n", info.width, info.height, info.fps,
                (long long)info.frameCount, qPrintable(info.codec), qPrintable(info.pixelFormat),
                info.variableFrameRate ? "  VFR" : "", t.nsecsElapsed() / 1e6);

    std::vector<QByteArray> seq(size_t(info.frameCount));
    QImage img;
    t.restart();
    for (int64_t i = 0; i < info.frameCount; ++i) {
        if (!dec.decode(i, img, &err)) {
            std::printf("sequential decode failed at %lld: %s\n", (long long)i, qPrintable(err));
            return 1;
        }
        seq[size_t(i)] = hashOf(img);
    }
    const double seqMs = t.nsecsElapsed() / 1e6;
    std::printf("  decoded %lld frames, %lld seeks\n", (long long)dec.stats().framesDecoded, (long long)dec.stats().seeks);
    std::printf("sequential: %lld frames in %.0f ms  (%.1f fps)\n", (long long)info.frameCount, seqMs,
                info.frameCount * 1000.0 / seqMs);

    std::mt19937 rng(1234);
    std::uniform_int_distribution<int64_t> dist(0, info.frameCount - 1);
    int mismatches = 0;
    t.restart();
    for (int s = 0; s < samples; ++s) {
        const int64_t i = dist(rng);
        if (!dec.decode(i, img, &err) || hashOf(img) != seq[size_t(i)]) {
            if (++mismatches <= 10) std::printf("MISMATCH at frame %lld\n", (long long)i);
        }
    }
    const double rndMs = t.nsecsElapsed() / 1e6;
    std::printf("random: %d seeks in %.0f ms  (%.1f ms/seek)\n", samples, rndMs, rndMs / samples);

    // Backwards stepping (worst case for long-GOP codecs)
    t.restart();
    const int64_t back = std::min<int64_t>(info.frameCount, 48);
    for (int64_t k = 0; k < back; ++k) {
        const int64_t i = info.frameCount - 1 - k;
        if (!dec.decode(i, img, &err) || hashOf(img) != seq[size_t(i)]) {
            if (++mismatches <= 10) std::printf("MISMATCH (backward) at frame %lld\n", (long long)i);
        }
    }
    std::printf("backward: %lld steps in %.0f ms\n", (long long)back, t.nsecsElapsed() / 1e6);

    // Ground truth: the frame FFmpeg's own `select=eq(n,i)` returns must match our frame i
    // better than its neighbours i-1 / i+1 (colour conversion may differ slightly, so compare by distance).
    const QString ffmpeg = QCoreApplication::applicationDirPath() + "/ffmpeg.exe";
    if (QFileInfo::exists(ffmpeg)) {
        auto meanDiff = [](const QImage& a, const QByteArray& b) {
            const uchar* p = a.constBits();
            const qsizetype n = std::min<qsizetype>(a.sizeInBytes(), b.size());
            double sum = 0;
            for (qsizetype k = 0; k < n; k += 4) sum += std::abs(int(p[k]) - int(uchar(b[k]))) + std::abs(int(p[k + 1]) - int(uchar(b[k + 1])));
            return sum / (n / 2.0);
        };
        const int64_t last = info.frameCount - 1;
        for (int64_t i : { int64_t(0), int64_t(1), last / 3, last / 2, last - 1, last }) {
            QProcess proc;
            proc.start(ffmpeg, { "-v", "error", "-i", path, "-vf", QStringLiteral("select=eq(n\\,%1)").arg(i), "-frames:v", "1",
                                 "-f", "rawvideo", "-pix_fmt", "rgba", "-" });
            proc.waitForFinished(120000);
            const QByteArray truth = proc.readAllStandardOutput();
            double best = 1e9;
            int64_t bestIdx = -1;
            for (int64_t j = std::max<int64_t>(0, i - 1); j <= std::min(last, i + 1); ++j) {
                dec.decode(j, img, &err);
                const double d = meanDiff(img, truth);
                if (d < best) { best = d; bestIdx = j; }
            }
            const bool ok = bestIdx == i;  // offset of ~13 = BT.709 (ours) vs BT.601 (ffmpeg CLI default) on untagged HD
            std::printf("  ground truth frame %lld: best match %lld (mean diff %.2f) %s\n", (long long)i, (long long)bestIdx, best, ok ? "ok" : "WRONG");
            if (!ok) ++mismatches;
        }
    }

    std::printf(mismatches ? "RESULT: FAIL (%d mismatches)\n" : "RESULT: PASS%.0d\n", mismatches);
    return mismatches ? 1 : 0;
}
