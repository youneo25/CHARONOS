// Tracks one point through a video and prints "frame x y score" (media pixels).
//   track_test <video> <x> <y> [startFrame] [-1 for backwards]
#include "media/VideoDecoder.h"
#include "track/Tracker.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <cstdio>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 4) return 2;
    VideoDecoder dec;
    QString err;
    if (!dec.open(QString::fromLocal8Bit(argv[1]), &err)) return 1;
    const int start = argc > 4 ? std::atoi(argv[4]) : 0;
    const int dir = argc > 5 ? std::atoi(argv[5]) : 1;
    QImage img;
    dec.decode(start, img, &err);
    GrayFrame g = toGray(img);
    PointTracker tr;
    if (!tr.init(g, QPointF(std::atof(argv[2]) * g.scale, std::atof(argv[3]) * g.scale))) return 3;
    QElapsedTimer t;
    t.start();
    int n = 0;
    for (int f = start + dir; f >= 0 && f < dec.info().frameCount; f += dir) {
        dec.decode(f, img, &err);
        g = toGray(img);
        const auto r = tr.step(g);
        if (!r.ok) {
            std::printf("LOST %d %.3f\n", f, r.score);
            break;
        }
        std::printf("%d %.2f %.2f %.3f\n", f, r.pos.x() / g.scale, r.pos.y() / g.scale, r.score);
        ++n;
    }
    std::fprintf(stderr, "%d frames in %.0f ms\n", n, t.nsecsElapsed() / 1e6);
    return 0;
}
