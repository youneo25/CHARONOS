#include "Icons.h"

#include "Palette.h"

#include <QHash>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>

namespace {

// Each entry is the inner SVG markup on a 24x24 grid. Strokes use currentColor; `F` marks filled shapes.
const QHash<QString, QString>& shapes()
{
    static const QHash<QString, QString> s = {
        { "volume", R"(<path d="M4.5 9.5h3l4.5-4v13l-4.5-4h-3z"/><path d="M15.5 9.2a4 4 0 0 1 0 5.6M18.2 6.6a7.6 7.6 0 0 1 0 10.8"/>)" },
        { "mute", R"(<path d="M4.5 9.5h3l4.5-4v13l-4.5-4h-3z"/><path d="M16 9.5l5 5M21 9.5l-5 5"/>)" },
        { "cursor", R"(<path class="F" d="M5.6 4.9C5.6 3.8 6.8 3.2 7.7 3.8L19.6 11.8C20.6 12.5 20.3 14 19.2 14.3C14.9 15.4 11.2 17.4 8 20.3C7.1 21.1 5.6 20.5 5.6 19.3Z"/>)" },
        { "target", R"(<circle cx="12" cy="12" r="6.5"/><path d="M12 2.5v4M12 17.5v4M2.5 12h4M17.5 12h4"/><circle class="F" cx="12" cy="12" r="1.3"/>)" },
        { "onion", R"(<rect x="3.5" y="8" width="10" height="10" rx="2" opacity=".4"/><rect x="7" y="6" width="10" height="10" rx="2" opacity=".7"/><rect x="10.5" y="4" width="10" height="10" rx="2"/>)" },
        { "pencil", R"(<path d="M4.5 19.5l1-4.2L15.8 5a2 2 0 0 1 2.9 0l.3.3a2 2 0 0 1 0 2.9L8.7 18.5z"/><path d="M14 7l3 3"/>)" },
        { "pen", R"(<path d="M5 19l3-9 8-6 4 4-6 8z"/><path d="M5 19l7-7"/><circle cx="13" cy="11" r="1.5"/>)" },
        { "highlighter", R"(<path d="M5 16l3 3 4-3-4-4zM8 12l8-8 5 5-9 7"/><path d="M3 21h17" stroke-width="3"/>)" },
        { "eraser", R"(<path d="M8.5 19.5h11"/><path d="M5.2 14.6l8.6-8.6a2 2 0 0 1 2.8 0l2.4 2.4a2 2 0 0 1 0 2.8l-7.8 7.8a2 2 0 0 1-1.4.5H8.6a2 2 0 0 1-1.4-.6l-2-2a1.6 1.6 0 0 1 0-2.3z"/><path d="M9.5 10.3l4.2 4.2"/>)" },
        { "lock", R"(<rect x="5.5" y="10.5" width="13" height="9" rx="2"/><path d="M8.5 10.5V8a3.5 3.5 0 0 1 7 0v2.5"/>)" },
        { "unlock", R"(<rect x="5.5" y="10.5" width="13" height="9" rx="2"/><path d="M8.5 10.5V8a3.5 3.5 0 0 1 6.8-1.2"/>)" },
        { "link", R"(<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1.2 1.2"/><path d="M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1.2-1.2"/>)" },
        { "ripple", R"(<rect x="3.5" y="7.5" width="7" height="9" rx="1.2"/><path d="M13.5 12h7M17.8 9.2l2.7 2.8-2.7 2.8"/>)" },
        { "plus", R"(<path d="M12 5v14M5 12h14"/>)" },
        { "eye-off", R"(<path d="M3 12s3.5-6 9-6 9 6 9 6-3.5 6-9 6-9-6-9-6z"/><circle cx="12" cy="12" r="2.5"/><path d="M4.5 4.5l15 15"/>)" },
        { "silhouette", R"(<circle class="F" cx="12" cy="5.2" r="2.6"/><path class="F" d="M8.4 9.4h7.2l-1.1 6.2h-1.6l-.5 5.2h-2.8l-.5-5.2H9.5z"/>)" },
        { "image", R"(<rect x="4" y="5.5" width="16" height="13" rx="2"/><circle cx="9" cy="10" r="1.5"/><path d="M5 17l4.5-4.5 3 3 2.5-2.5 4 4"/>)" },
        { "layers", R"(<path d="M12 4l8 4-8 4-8-4z"/><path d="M4 12l8 4 8-4"/><path d="M4 16l8 4 8-4"/>)" },
        { "gap", R"(<rect x="3" y="7.5" width="4.5" height="9" rx="1"/><rect x="16.5" y="7.5" width="4.5" height="9" rx="1"/><path d="M9.5 12h5M12.5 10l2 2-2 2"/>)" },
        { "filter", R"(<path d="M4.5 5.5h15l-6 7.5v5l-3 1.5v-6.5z"/>)" },
        { "track-add", R"(<path d="M4 7h10M4 12h10M4 17h7"/><path d="M17.5 13.5v7M14 17h7"/>)" },
        { "text", R"(<path d="M5.5 7V5h13v2"/><path d="M12 5v14"/><path d="M9.5 19h5"/>)" },
        { "arrow", R"(<path d="M5.5 18.5l13-13"/><path d="M9.5 5.5h9v9"/>)" },
        { "ellipse", R"(<ellipse cx="12" cy="12" rx="7.5" ry="6.5"/>)" },
        { "rect", R"(<rect x="4.5" y="6" width="15" height="12" rx="1.5"/>)" },
        { "line", R"(<path d="M5.5 18.5l13-13"/>)" },
        { "move", R"(<path d="M12 4v16M4 12h16"/><path d="M9.5 6.5L12 4l2.5 2.5M9.5 17.5L12 20l2.5-2.5M6.5 9.5L4 12l2.5 2.5M17.5 9.5L20 12l-2.5 2.5"/>)" },
        { "width", R"(<path d="M5 7h14" stroke-width="1"/><path d="M5 12h14" stroke-width="2"/><path d="M5 17.5h14" stroke-width="3.2"/>)" },
        { "undo", R"(<path d="M9 14.5L4.5 10 9 5.5"/><path d="M4.5 10h10a5 5 0 0 1 0 10H11"/>)" },
        { "redo", R"(<path d="M15 14.5l4.5-4.5L15 5.5"/><path d="M19.5 10h-10a5 5 0 0 0 0 10H13"/>)" },
        { "clear", R"(<path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/>)" },
        { "trash", R"(<path d="M4.5 7h15"/><path d="M9.5 7V5h5v2"/><path d="M6.5 7l.8 11.2a2 2 0 0 0 2 1.8h5.4a2 2 0 0 0 2-1.8L17.5 7"/><path d="M10.2 11v5M13.8 11v5"/>)" },
        { "start", R"(<path d="M6 5.5v13"/><path class="F" d="M18.5 6v12l-9-6z"/>)" },
        { "end", R"(<path d="M18 5.5v13"/><path class="F" d="M5.5 6v12l9-6z"/>)" },
        { "step-back", R"(<path d="M14.5 6.5L9 12l5.5 5.5"/>)" },
        { "step-fwd", R"(<path d="M9.5 6.5L15 12l-5.5 5.5"/>)" },
        { "prev-mark", R"(<path d="M15.5 6.5L10 12l5.5 5.5"/><path d="M7 6.5v11"/>)" },
        { "next-mark", R"(<path d="M8.5 6.5L14 12l-5.5 5.5"/><path d="M17 6.5v11"/>)" },
        { "play", R"(<path class="F" d="M8 5.2v13.6a.8.8 0 0 0 1.2.7l10.6-6.8a.8.8 0 0 0 0-1.4L9.2 4.5A.8.8 0 0 0 8 5.2z"/>)" },
        { "pause", R"(<rect class="F" x="6.5" y="5" width="4" height="14" rx="1"/><rect class="F" x="13.5" y="5" width="4" height="14" rx="1"/>)" },
        { "loop", R"(<path d="M17 3.5l2.5 2.5L17 8.5"/><path d="M4.5 11.5V10a4 4 0 0 1 4-4h11"/><path d="M7 20.5L4.5 18 7 15.5"/><path d="M19.5 12.5V14a4 4 0 0 1-4 4h-11"/>)" },
        { "sidebar", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M9.5 4.5v15"/>)" },
        { "toolbar", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M7 8.5h.01M7 12h.01M7 15.5h.01" stroke-width="2.4"/>)" },
        { "compare", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M12 3v18"/>)" },
        { "wipe", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M12 2.5v19"/><path class="F" d="M12 4.5h6.5a2 2 0 0 1 2 2v11a2 2 0 0 1-2 2H12z" opacity=".35"/>)" },
        { "side", R"(<rect x="2.5" y="6.5" width="8.5" height="11" rx="1.5"/><rect x="13" y="6.5" width="8.5" height="11" rx="1.5"/>)" },
        { "stack", R"(<rect x="5.5" y="3" width="13" height="8" rx="1.5"/><rect x="5.5" y="13" width="13" height="8" rx="1.5"/>)" },
        { "blend", R"(<circle cx="9" cy="12" r="5.5"/><circle cx="15" cy="12" r="5.5"/>)" },
        { "swap", R"(<path d="M16.5 3.5L20 7l-3.5 3.5"/><path d="M20 7H8"/><path d="M7.5 20.5L4 17l3.5-3.5"/><path d="M4 17h12"/>)" },
        { "import", R"(<path d="M13.5 3.5H7a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V9z"/><path d="M13.5 3.5V9H19"/><path d="M12 11.5v6M9 14.5h6"/>)" },
        { "folder", R"(<path d="M3.5 7.5a2 2 0 0 1 2-2h3.8l2 2.2h7.2a2 2 0 0 1 2 2v7.8a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/>)" },
        { "film", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M7.5 4.5v15M16.5 4.5v15M3.5 9.5h4M3.5 14.5h4M16.5 9.5h4M16.5 14.5h4"/>)" },
        { "video", R"(<path d="M13.5 3.5H7a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V9z"/><path d="M13.5 3.5V9H19"/><path d="M10.2 12v5l4-2.5z"/>)" },
        { "images", R"(<rect x="3.5" y="6.5" width="13" height="11" rx="1.5"/><path d="M7.5 20.5h11a2 2 0 0 0 2-2v-9"/><path d="M3.5 15l3.5-3.2 3 2.6 2-1.6 4.5 3.7"/>)" },
        { "timeline-add", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M3.5 9h17"/><path d="M12 12v5M9.5 14.5h5"/>)" },
        { "up", R"(<path d="M12 19V5.5M6.5 11L12 5.5l5.5 5.5"/>)" },
        { "down", R"(<path d="M12 5v13.5M6.5 13l5.5 5.5 5.5-5.5"/>)" },
        { "sort", R"(<path d="M7 5v14M4 16l3 3 3-3"/><path d="M13.5 6h6M13.5 10.5h4.5M13.5 15h3"/>)" },
        { "export", R"(<path d="M12 15V4M7.5 8.5L12 4l4.5 4.5"/><path d="M5 13.5v4.5a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-4.5"/>)" },
        { "fit", R"(<path d="M4.5 9V4.5H9M15 4.5h4.5V9M19.5 15v4.5H15M9 19.5H4.5V15"/>)" },
        { "bookmark", R"(<path d="M7 4.5a1 1 0 0 1 1-1h8a1 1 0 0 1 1 1v15.5l-5-3.8-5 3.8z"/>)" },
        { "grid", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M9.2 4.5v15M14.8 4.5v15M3.5 9.5h17M3.5 14.5h17"/>)" },
        { "eye", R"(<path d="M2.5 12s3.5-6.5 9.5-6.5 9.5 6.5 9.5 6.5-3.5 6.5-9.5 6.5S2.5 12 2.5 12z"/><circle cx="12" cy="12" r="2.7"/>)" },
        { "close", R"(<path d="M7.5 7.5l9 9M16.5 7.5l-9 9"/>)" },
        { "chevron-down", R"(<path d="M7 10l5 5 5-5"/>)" },
        { "zoom", R"(<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5 5"/>)" },
        { "zoom-in", R"(<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5 5M8 10.5h5M10.5 8v5"/>)" },
        { "zoom-out", R"(<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5 5M8 10.5h5"/>)" },
        { "settings", R"(<circle cx="12" cy="12" r="3"/><path d="M12 3.5v2.2M12 18.3v2.2M3.5 12h2.2M18.3 12h2.2M6 6l1.6 1.6M16.4 16.4L18 18M6 18l1.6-1.6M16.4 7.6L18 6"/>)" },
        // 1.0
        { "split", R"(<rect x="3.5" y="7" width="6.5" height="10" rx="1.5"/><rect x="14" y="7" width="6.5" height="10" rx="1.5"/><path d="M12 3.5v17"/>)" },
        { "magnet", R"(<path d="M5.5 4.5h4v7a2.5 2.5 0 0 0 5 0v-7h4v7a6.5 6.5 0 0 1-13 0z"/><path d="M5.5 8.5h4M14.5 8.5h4"/>)" },
        { "clock", R"(<circle cx="12" cy="12" r="8"/><path d="M12 7.5V12l3 2"/>)" },
        { "notes", R"(<path d="M5 5h14a1.5 1.5 0 0 1 1.5 1.5v8.5a1.5 1.5 0 0 1-1.5 1.5h-8.5l-4 3v-3H5a1.5 1.5 0 0 1-1.5-1.5V6.5A1.5 1.5 0 0 1 5 5z"/><path d="M8 9.5h8M8 12.5h5"/>)" },
        { "graph", R"(<path d="M4 4.5v15h16"/><path d="M6.5 16c2.5-.5 3.5-8.5 6.5-9s3.5 5.5 6 5"/>)" },
        { "flip", R"(<path d="M12 3.5v17" stroke-dasharray="2 2.2"/><path d="M9.5 7L4 17h5.5z"/><path d="M14.5 7L20 17h-5.5z"/>)" },
        { "hud", R"(<rect x="3.5" y="5" width="17" height="14" rx="2"/><path d="M7 15.5h3.5M13 15.5h4"/>)" },
        { "layout", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M3.5 9h17M9.5 9v10.5"/>)" },
        { "ai", R"(<path d="M11 3.5l1.9 5 5 1.9-5 1.9-1.9 5-1.9-5-5-1.9 5-1.9z"/><path d="M18 14.5l.9 2.1 2.1.9-2.1.9-.9 2.1-.9-2.1-2.1-.9 2.1-.9z"/>)" },
        { "keyboard", R"(<rect x="2.5" y="6" width="19" height="12" rx="2"/><path d="M6 9.5h1M9.5 9.5h1M13 9.5h1M16.5 9.5h1M6 12.5h1M16.5 12.5h1M8.5 15h7"/>)" },
        { "help", R"(<circle cx="12" cy="12" r="8.5"/><path d="M9.6 9.3a2.5 2.5 0 0 1 4.8 1c0 1.7-2.4 2-2.4 3.4"/><path d="M12 16.8h.01" stroke-width="2.2"/>)" },
        { "steps", R"(<path d="M4 18.5h4v-4h4v-4h4v-4h4"/>)" },
    };
    return s;
}

// Glyphs that swap with each other in one button keep one shared scale.
const QHash<QString, QString>& families()
{
    static const QHash<QString, QString> f = {
        { "play", "transport" }, { "pause", "transport" },
        { "volume", "audio" }, { "mute", "audio" },
        { "start", "ends" }, { "end", "ends" },
        { "step-back", "step" }, { "step-fwd", "step" },
        { "prev-mark", "marks" }, { "next-mark", "marks" },
        { "undo", "history" }, { "redo", "history" },
        { "up", "order" }, { "down", "order" },
        { "zoom", "zoom" }, { "zoom-in", "zoom" }, { "zoom-out", "zoom" },
    };
    return f;
}

constexpr double kStroke = 1.6;
constexpr double kTarget = 17.0;  // larger side of every glyph's ink box, on the 24 grid
constexpr double kMinScale = 0.8, kMaxScale = 1.8;

QString rawSvg(const QString& body, const QColor& color, double strokeWidth)
{
    const QString c = color.name(QColor::HexRgb);
    QString b = body;
    b.replace(QStringLiteral("class=\"F\""), QStringLiteral("fill=\"%1\" stroke=\"none\"").arg(c));
    return QStringLiteral(
               "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"%1\" "
               "stroke-width=\"%2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%3</svg>")
        .arg(c)
        .arg(strokeWidth, 0, 'f', 3)
        .arg(b);
}

// Ink bounding box of a glyph on the 24 grid (strokes included), measured once.
QRectF inkBox(const QString& name)
{
    static QHash<QString, QRectF> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return *it;
    constexpr int kSize = 96;  // 4 px per grid unit
    QImage img(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QSvgRenderer renderer(rawSvg(shapes().value(name), Qt::black, kStroke).toUtf8());
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p);
    }
    int left = kSize, top = kSize, right = -1, bottom = -1;
    for (int y = 0; y < kSize; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < kSize; ++x)
            if (qAlpha(line[x]) > 24) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    }
    const QRectF box = right < 0 ? QRectF(0, 0, 24, 24) : QRectF(left / 4.0, top / 4.0, (right - left + 1) / 4.0, (bottom - top + 1) / 4.0);
    cache.insert(name, box);
    return box;
}

double scaleFor(const QString& name)
{
    // The stroke keeps its width when the geometry scales, so only the geometry part counts.
    auto own = [](const QString& n) {
        const QRectF b = inkBox(n);
        const double side = std::max({ b.width(), b.height(), kStroke + 1.0 }) - kStroke;
        return std::clamp((kTarget - kStroke) / side, kMinScale, kMaxScale);
    };
    const QString family = families().value(name);
    if (family.isEmpty()) return own(name);
    double s = kMaxScale;
    for (auto it = families().begin(); it != families().end(); ++it)
        if (it.value() == family) s = std::min(s, own(it.key()));
    return s;
}

QByteArray svgFor(const QString& name, const QColor& color)
{
    const QRectF box = inkBox(name);
    const double s = scaleFor(name);
    QString body = shapes().value(name);
    // Strokes keep their on-screen weight after scaling.
    static const QRegularExpression strokeAttr(QStringLiteral("stroke-width=\"([0-9.]+)\""));
    QString scaled;
    qsizetype last = 0;
    for (auto m = strokeAttr.globalMatch(body); m.hasNext();) {
        const auto match = m.next();
        scaled += body.mid(last, match.capturedStart() - last);
        scaled += QStringLiteral("stroke-width=\"%1\"").arg(match.captured(1).toDouble() / s, 0, 'f', 3);
        last = match.capturedEnd();
    }
    scaled += body.mid(last);
    const QPointF c = box.center();
    const QString group = QStringLiteral("<g transform=\"translate(12 12) scale(%1) translate(%2 %3)\">%4</g>")
                              .arg(s, 0, 'f', 4)
                              .arg(-c.x(), 0, 'f', 3)
                              .arg(-c.y(), 0, 'f', 3)
                              .arg(scaled);
    return rawSvg(group, color, kStroke / s).toUtf8();
}

} // namespace

namespace Icons {

QPixmap pixmap(const QString& name, const QColor& color, int size, qreal dpr)
{
    QSvgRenderer renderer(svgFor(name, color));
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p);
    p.end();
    pm.setDevicePixelRatio(dpr);
    return pm;
}

QRectF inkBounds(const QString& name)
{
    // Ink box after normalisation, on the 24 grid.
    const QRectF b = inkBox(name);
    const double s = scaleFor(name);
    const double w = (b.width() - kStroke) * s + kStroke, h = (b.height() - kStroke) * s + kStroke;
    return QRectF(12 - w / 2, 12 - h / 2, w, h);
}

QStringList names()
{
    QStringList n = shapes().keys();
    n.sort();
    return n;
}

QIcon icon(const QString& name)
{
    static QHash<QString, QIcon> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return *it;

    QIcon ic;
    for (int size : { 16, 18, 20, 24, 32 }) {
        ic.addPixmap(pixmap(name, Pal::iconIdle, size), QIcon::Normal, QIcon::Off);
        ic.addPixmap(pixmap(name, Pal::text, size), QIcon::Active, QIcon::Off);
        ic.addPixmap(pixmap(name, Pal::textStrong, size), QIcon::Normal, QIcon::On);
        ic.addPixmap(pixmap(name, Pal::textStrong, size), QIcon::Active, QIcon::On);
        ic.addPixmap(pixmap(name, Pal::textFaint, size), QIcon::Disabled, QIcon::Off);
        ic.addPixmap(pixmap(name, Pal::textFaint, size), QIcon::Disabled, QIcon::On);
    }
    cache.insert(name, ic);
    return ic;
}

} // namespace Icons
