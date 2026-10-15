#include "AnnotationPainter.h"

#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QtMath>

#include <mutex>
#include <unordered_map>

namespace {

QPointF mapPt(const Annotation& a, double nx, double ny, const QSizeF& m)
{
    return QPointF((nx * a.scale + a.tx) * m.width(), (ny * a.scale + a.ty) * m.height());
}

QFont textFont(const Annotation& a)
{
    QFont f(a.fontFamily);
    f.setBold(a.bold);
    f.setItalic(a.italic);
    f.setPixelSize(std::max(1, int(std::round(a.fontSize * a.scale))));
    return f;
}

const QImage& imageFor(const QString& src)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, QImage> cache;
    std::lock_guard lock(mutex);
    const std::string key = src.toStdString();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    QImage img;
    const int comma = src.indexOf(',');
    if (src.startsWith("data:") && comma > 0)
        img.loadFromData(QByteArray::fromBase64(src.mid(comma + 1).toLatin1()));
    else
        img.load(src);
    return cache.emplace(key, img).first->second;
}

double segmentDistance(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = len2 > 0 ? ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + ab * t;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

// Undo the annotation's rotation so a point can be compared against unrotated geometry.
QPointF toLocal(const Annotation& a, const QSizeF& m, const QPointF& pt)
{
    const QPointF c = AnnotationPainter::boundingBox(a, m).center();
    const double s = std::sin(-a.angle), co = std::cos(-a.angle);
    const QPointF d = pt - c;
    return c + QPointF((d.x() * co - d.y() * s) / a.stretchX, (d.x() * s + d.y() * co) / a.stretchY);
}

struct StrokePt {
    QPointF p;
    double w;  // full width in media px
};

// Chaikin corner cutting: rounds the polyline the mouse/pen produced without moving its ends.
QVector<StrokePt> smooth(QVector<StrokePt> pts, int iterations)
{
    for (int it = 0; it < iterations && pts.size() > 2; ++it) {
        QVector<StrokePt> out;
        out.reserve(pts.size() * 2);
        out.append(pts.first());
        for (qsizetype i = 0; i + 1 < pts.size(); ++i) {
            const StrokePt& a = pts[i];
            const StrokePt& b = pts[i + 1];
            out.append({ a.p * 0.75 + b.p * 0.25, a.w * 0.75 + b.w * 0.25 });
            out.append({ a.p * 0.25 + b.p * 0.75, a.w * 0.25 + b.w * 0.75 });
        }
        out.append(pts.last());
        pts = std::move(out);
    }
    return pts;
}

// Pen pressure -> width factor. Light touches still leave a visible line.
double pressureWidth(float pressure) { return 0.12 + 0.88 * std::pow(std::clamp(double(pressure), 0.0, 1.0), 0.85); }

void paintStroke(QPainter& p, const Annotation& a, const QSizeF& m, const QColor& color)
{
    if (a.points.isEmpty()) return;
    const bool pressure = a.pressures.size() == a.points.size();
    const QBrush ink(color, a.penStyle == "pencil" ? Qt::Dense1Pattern : Qt::SolidPattern);
    QVector<StrokePt> pts;
    pts.reserve(a.points.size());
    for (qsizetype i = 0; i < a.points.size(); ++i)
        pts.append({ mapPt(a, a.points[i].x(), a.points[i].y(), m), a.size * (pressure ? pressureWidth(a.pressures[i]) : 1.0) });

    if (pts.size() == 1) {
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(pts[0].p, pts[0].w / 2, pts[0].w / 2);
        return;
    }
    pts = smooth(pts, 2);

    if (!pressure) {
        QPen pen = p.pen();
        pen.setBrush(ink);
        if (a.penStyle == "highlighter") pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        QPainterPath path(pts[0].p);
        for (qsizetype i = 1; i < pts.size(); ++i) path.lineTo(pts[i].p);
        p.drawPath(path);
        return;
    }

    // Variable width: a single closed outline (left edge, round end cap, right edge back, round
    // start cap) filled once, so a translucent stroke doesn't darken where pieces would overlap.
    const qsizetype n = pts.size();
    QVector<QPointF> tangents(n), normals(n);
    QPolygonF left, right;
    for (qsizetype i = 0; i < n; ++i) {
        const QPointF prev = pts[std::max<qsizetype>(0, i - 1)].p, next = pts[std::min<qsizetype>(n - 1, i + 1)].p;
        QPointF t = next - prev;
        const double len = std::hypot(t.x(), t.y());
        t = len > 1e-9 ? t / len : QPointF(1, 0);
        tangents[i] = t;
        normals[i] = QPointF(-t.y(), t.x());
        const double h = pts[i].w / 2;
        left << pts[i].p + normals[i] * h;
        right << pts[i].p - normals[i] * h;
    }
    auto cap = [&](QPolygonF& poly, qsizetype i, double dir) {
        if (a.penStyle == "highlighter") return; // flat chisel tip
        const double h = pts[i].w / 2;
        for (int k = 1; k < 8; ++k) {
            const double th = M_PI * k / 8;
            poly << pts[i].p + normals[i] * (dir * h * std::cos(th)) + tangents[i] * (dir * h * std::sin(th));
        }
    };
    QPolygonF outline = left;
    cap(outline, n - 1, 1.0);  // end cap: from the left edge around the front
    for (auto it = right.crbegin(); it != right.crend(); ++it) outline << *it;
    cap(outline, 0, -1.0);     // start cap: from the right edge around the back
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawPolygon(outline, Qt::WindingFill);
}

} // namespace

namespace AnnotationPainter {

QStringList textLines(const Annotation& a) { return a.text.split('\n'); }
double textLineHeight(const Annotation& a) { return a.fontSize * 1.2 * a.scale; }

QRectF boundingBox(const Annotation& a, const QSizeF& m)
{
    if (a.stretchX != 1 || a.stretchY != 1) {
        Annotation base = a;
        base.stretchX = base.stretchY = 1;
        const QRectF r = boundingBox(base, m);
        const QSizeF size(r.width() * a.stretchX, r.height() * a.stretchY);
        return QRectF(r.center() - QPointF(size.width()/2, size.height()/2), size);
    }
    if (a.type == "stroke") {
        if (a.points.isEmpty()) return {};
        double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
        for (const QPointF& p : a.points) {
            const QPointF q = mapPt(a, p.x(), p.y(), m);
            minX = std::min(minX, q.x()); maxX = std::max(maxX, q.x());
            minY = std::min(minY, q.y()); maxY = std::max(maxY, q.y());
        }
        return QRectF(minX - 5, minY - 5, maxX - minX + 10, maxY - minY + 10);
    }
    if (a.type == "line" || a.type == "arrow") {
        const QPointF p1 = mapPt(a, a.x1, a.y1, m), p2 = mapPt(a, a.x2, a.y2, m);
        return QRectF(std::min(p1.x(), p2.x()) - 10, std::min(p1.y(), p2.y()) - 10,
                      std::abs(p1.x() - p2.x()) + 20, std::abs(p1.y() - p2.y()) + 20);
    }
    if (a.type == "rect" || a.type == "image") {
        const QPointF o = mapPt(a, a.x, a.y, m);
        const double w = (a.type == "image" && a.w == 0 ? 0.25 : a.w) * a.scale * m.width();
        const double h = (a.type == "image" && a.h == 0 ? 0.25 : a.h) * a.scale * m.height();
        return QRectF(o.x() - 5, o.y() - 5, w + 10, h + 10).normalized();
    }
    if (a.type == "circle") {
        const QPointF c = mapPt(a, a.cx, a.cy, m);
        const double rx = std::abs(a.rx * a.scale * m.width()), ry = std::abs(a.ry * a.scale * m.height());
        return QRectF(c.x() - rx - 5, c.y() - ry - 5, rx * 2 + 10, ry * 2 + 10);
    }
    if (a.type == "text") {
        const QPointF o = mapPt(a, a.x, a.y, m);
        const QFontMetricsF fm(textFont(a));
        double w = 0;
        const QStringList lines = textLines(a);
        for (const QString& l : lines) w = std::max(w, fm.horizontalAdvance(l));
        const double h = textLineHeight(a) * std::max<qsizetype>(1, lines.size());
        return QRectF(o.x() - 5, o.y() - 5, w + 10, h + 10);
    }
    return {};
}

QPolygonF outline(const Annotation& a, const QSizeF& m)
{
    const QRectF box = boundingBox(a, m);
    QPolygonF poly(box);
    poly.removeLast();  // QPolygonF(QRectF) closes the polygon with a 5th point
    if (a.angle == 0.0) return poly;
    QTransform t;
    t.translate(box.center().x(), box.center().y());
    t.rotateRadians(a.angle);
    t.translate(-box.center().x(), -box.center().y());
    return t.map(poly);
}

void paint(QPainter& p, const Annotation& a, const QSizeF& m, double globalOpacity, bool dimmed, const QColor& tint)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setOpacity((dimmed ? 0.45 : 1.0) * a.opacity * globalOpacity);

    if (a.angle != 0.0) {
        const QPointF c = boundingBox(a, m).center();
        p.translate(c);
        p.rotate(qRadiansToDegrees(a.angle));
        p.translate(-c);
    }
    const QPointF center = boundingBox(a, m).center();
    p.translate(center);
    p.scale(a.stretchX, a.stretchY);
    p.translate(-center);
    if (a.type == "stroke") {
        if (a.penStyle == "highlighter") p.setOpacity(p.opacity() * 0.35);
        else if (a.penStyle == "pencil") p.setOpacity(p.opacity() * 0.8);
    }

    const QColor color = tint.isValid() ? tint : a.color;
    QPen pen(color, std::max(0.1, a.size), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (a.type == "stroke") {
        paintStroke(p, a, m, color);
    } else if (a.type == "line" || a.type == "arrow") {
        const QPointF p1 = mapPt(a, a.x1, a.y1, m), p2 = mapPt(a, a.x2, a.y2, m);
        p.drawLine(p1, p2);
        if (a.type == "arrow") {
            const double ang = std::atan2(p2.y() - p1.y(), p2.x() - p1.x());
            const double len = 15 * a.scale + a.size * 1.2;
            QPolygonF head;
            head << p2 << QPointF(p2.x() - len * std::cos(ang - M_PI / 6), p2.y() - len * std::sin(ang - M_PI / 6))
                 << QPointF(p2.x() - len * std::cos(ang + M_PI / 6), p2.y() - len * std::sin(ang + M_PI / 6));
            p.setBrush(color);
            p.drawPolygon(head);
        }
    } else if (a.type == "rect") {
        const QPointF o = mapPt(a, a.x, a.y, m);
        p.drawRect(QRectF(o.x(), o.y(), a.w * a.scale * m.width(), a.h * a.scale * m.height()));
    } else if (a.type == "circle") {
        const QPointF c = mapPt(a, a.cx, a.cy, m);
        p.drawEllipse(c, std::abs(a.rx * a.scale * m.width()), std::abs(a.ry * a.scale * m.height()));
    } else if (a.type == "text") {
        const QPointF o = mapPt(a, a.x, a.y, m);
        const QFont font = textFont(a);
        p.setFont(font);
        const QFontMetricsF fm(font);
        const QStringList lines = textLines(a);
        const double lh = textLineHeight(a);
        if (a.useBg && !a.bgColor.isEmpty() && !tint.isValid()) {
            double w = 0;
            for (const QString& l : lines) w = std::max(w, fm.horizontalAdvance(l));
            p.fillRect(QRectF(o.x() - 5 * a.scale, o.y() - 2 * a.scale, w + 10 * a.scale, lh * lines.size()),
                       QColor::fromString(a.bgColor));
        }
        for (qsizetype i = 0; i < lines.size(); ++i) p.drawText(QPointF(o.x(), o.y() + i * lh + fm.ascent()), lines[i]);
    } else if (a.type == "image") {
        const QPointF o = mapPt(a, a.x, a.y, m);
        const QRectF r(o.x(), o.y(), (a.w ? a.w : 0.25) * a.scale * m.width(), (a.h ? a.h : 0.25) * a.scale * m.height());
        const QImage& img = imageFor(a.src);
        if (!img.isNull())
            p.drawImage(r, img);
        else
            p.fillRect(r, QColor(127, 127, 127, 90));
    }
    p.restore();
}

bool hit(const Annotation& a, const QSizeF& m, const QPointF& pt, double radius)
{
    Annotation base = a;
    base.stretchX = base.stretchY = 1;
    const QRectF box = boundingBox(base, m);
    const QPointF local = toLocal(a, m, pt);
    const double dx = std::max({ box.left() - local.x(), 0.0, local.x() - box.right() });
    const double dy = std::max({ box.top() - local.y(), 0.0, local.y() - box.bottom() });
    return std::hypot(dx * a.stretchX, dy * a.stretchY) <= radius;
}

bool hitShape(const Annotation& a, const QSizeF& m, const QPointF& pt, double tol)
{
    const QPointF local = toLocal(a, m, pt);
    const double reach = a.size / 2 + tol;
    if (a.type == "stroke") {
        if (a.points.size() == 1) return QLineF(local, mapPt(a, a.points[0].x(), a.points[0].y(), m)).length() <= reach;
        for (qsizetype i = 1; i < a.points.size(); ++i) {
            const QPointF p0 = mapPt(a, a.points[i - 1].x(), a.points[i - 1].y(), m);
            const QPointF p1 = mapPt(a, a.points[i].x(), a.points[i].y(), m);
            if (segmentDistance(local, p0, p1) <= reach) return true;
        }
        return false;
    }
    if (a.type == "line" || a.type == "arrow")
        return segmentDistance(local, mapPt(a, a.x1, a.y1, m), mapPt(a, a.x2, a.y2, m)) <= reach;
    Annotation base = a;
    base.stretchX = base.stretchY = 1;
    return boundingBox(base, m).adjusted(-tol, -tol, tol, tol).contains(local);
}

} // namespace AnnotationPainter

namespace AnnotationTransform {

void translate(Annotation& a, const QPointF& d, const QSizeF& m)
{
    a.tx += d.x() / m.width();
    a.ty += d.y() / m.height();
}

void scaleAbout(Annotation& a, double k, const QPointF& c, const QSizeF& m)
{
    // point = (n*s + t) * W ; want c + (point - c) * k  =>  t' = c/W * (1-k) + t*k, s' = s*k
    a.tx = c.x() / m.width() * (1 - k) + a.tx * k;
    a.ty = c.y() / m.height() * (1 - k) + a.ty * k;
    a.scale *= k;
}

void rotateAbout(Annotation& a, double rad, const QPointF& g, const QSizeF& m)
{
    // Rendering rotates around the annotation's own bbox centre, so move that centre around g first.
    const QPointF c = AnnotationPainter::boundingBox(a, m).center();
    const QPointF d = c - g;
    const QPointF c2 = g + QPointF(d.x() * std::cos(rad) - d.y() * std::sin(rad), d.x() * std::sin(rad) + d.y() * std::cos(rad));
    translate(a, c2 - c, m);
    a.angle += rad;
}

void stretchAbout(Annotation& a, double x, double y, const QPointF& g, const QSizeF& m)
{
    const QPointF c = AnnotationPainter::boundingBox(a, m).center();
    const QPointF d = c - g;
    // Stretch along the annotation's local axes, including after rotation.
    const double co = std::cos(a.angle), si = std::sin(a.angle);
    const QPointF local(d.x()*co + d.y()*si, -d.x()*si + d.y()*co);
    const QPointF scaled(local.x()*x, local.y()*y);
    translate(a, QPointF(scaled.x()*co - scaled.y()*si, scaled.x()*si + scaled.y()*co) - d, m);
    a.stretchX *= x;
    a.stretchY *= y;
}

} // namespace AnnotationTransform
