#include "Icons.h"

#include "Palette.h"

#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace {

// Each entry is the inner SVG markup. Strokes use currentColor; `F:` marks filled shapes.
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
        { "settings", R"(<circle cx="12" cy="12" r="3"/><path d="M12 3.5v2.2M12 18.3v2.2M3.5 12h2.2M18.3 12h2.2M6 6l1.6 1.6M16.4 16.4L18 18M6 18l1.6-1.6M16.4 7.6L18 6"/>)" },
    };
    return s;
}

QByteArray svgFor(const QString& name, const QColor& color)
{
    QString body = shapes().value(name);
    const QString c = color.name(QColor::HexRgb);
    body.replace(QStringLiteral("class=\"F\""), QStringLiteral("fill=\"%1\" stroke=\"none\"").arg(c));
    return QStringLiteral(
               "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"%1\" "
               "stroke-width=\"1.6\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%2</svg>")
        .arg(c, body)
        .toUtf8();
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
