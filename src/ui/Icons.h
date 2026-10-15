#pragma once

#include <QIcon>
#include <QString>

// Line icons drawn from inline SVG (24x24 grid, 1.6px rounded strokes), tinted per state
// so they follow the palette instead of shipping coloured artwork.
namespace Icons {

QIcon icon(const QString& name);
QPixmap pixmap(const QString& name, const QColor& color, int size, qreal dpr = 2.0);

} // namespace Icons
