// Renders pressure strokes to a PNG to check the variable-width outline.
//   pressure_test <out.png>
#include "model/AnnotationPainter.h"

#include <QGuiApplication>
#include <QPainter>

#include <cmath>

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    const QSizeF media(800, 400);
    QImage img(media.toSize(), QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(40, 40, 38));
    QPainter p(&img);

    // 1) wave with pressure rising and falling, opaque
    Annotation a;
    a.type = "stroke";
    a.color = QColor(0xe5, 0x48, 0x3e);
    a.size = 18;
    for (int i = 0; i <= 120; ++i) {
        const double t = i / 120.0;
        a.points.append(QPointF(0.05 + 0.9 * t, 0.25 + 0.12 * std::sin(t * 12)));
        a.pressures.append(float(std::sin(t * M_PI)));
    }
    AnnotationPainter::paint(p, a, media, 1.0, false);

    // 2) self-overlapping loop, 50% opacity: overlaps must not get darker
    Annotation b;
    b.type = "stroke";
    b.color = QColor(0x3d, 0x8b, 0xfd);
    b.size = 22;
    b.opacity = 0.5;
    for (int i = 0; i <= 160; ++i) {
        const double t = i / 160.0 * 2 * M_PI;
        b.points.append(QPointF(0.5 + 0.18 * std::sin(t * 1.0), 0.7 + 0.2 * std::sin(t * 2.0)));
        b.pressures.append(float(0.4 + 0.6 * std::abs(std::sin(t * 1.5))));
    }
    AnnotationPainter::paint(p, b, media, 1.0, false);
    p.end();
    return img.save(QString::fromLocal8Bit(argv[1])) ? 0 : 1;
}
