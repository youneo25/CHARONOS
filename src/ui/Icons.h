#pragma once

#include <QIcon>
#include <QRectF>
#include <QString>
#include <QStringList>

// Line icons drawn from inline SVG (24x24 grid, 1.6px rounded strokes), tinted per state
// so they follow the palette instead of shipping coloured artwork.
//
// Every glyph is normalised when it is first drawn: its ink box is measured and the glyph is
// scaled and centred so the larger side is the same for all icons (17 of 24 units), with the
// stroke width compensated so lines keep one weight. Glyphs that replace each other in one
// button (play/pause, volume/mute, ...) share a scale.
namespace Icons {

QIcon icon(const QString& name);
QPixmap pixmap(const QString& name, const QColor& color, int size, qreal dpr = 2.0);
QRectF inkBounds(const QString& name);  // normalised ink box on the 24 grid (QA)
QStringList names();

} // namespace Icons
