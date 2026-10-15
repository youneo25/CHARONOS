#pragma once

#include "Annotation.h"

#include <QPolygonF>
#include <QRectF>
#include <QSizeF>

class QPainter;

namespace AnnotationPainter {

// Unrotated bounding box in media pixels (same padding as the Electron version).
QRectF boundingBox(const Annotation& a, const QSizeF& media);

// The bounding box rotated by the annotation's angle (what the user sees).
QPolygonF outline(const Annotation& a, const QSizeF& media);

// Paints in media-pixel space; the caller sets the painter transform for zoom/pan.
// A valid `tint` replaces the annotation colour (onion skin ghosts).
void paint(QPainter& p, const Annotation& a, const QSizeF& media, double globalOpacity, bool dimmed,
           const QColor& tint = QColor());

// Eraser hit test: distance from point to the (rotated) bbox <= radius.
bool hit(const Annotation& a, const QSizeF& media, const QPointF& pt, double radius);

// Selection hit test: near the actual stroke for lines, inside the shape for areas.
bool hitShape(const Annotation& a, const QSizeF& media, const QPointF& pt, double tolerance);

// Text layout helpers (multi-line).
QStringList textLines(const Annotation& a);
double textLineHeight(const Annotation& a);

} // namespace AnnotationPainter

// Edits on the transform (tx, ty, scale, angle) that keep the visual result exact.
namespace AnnotationTransform {

void translate(Annotation& a, const QPointF& deltaMedia, const QSizeF& media);
void scaleAbout(Annotation& a, double factor, const QPointF& centerMedia, const QSizeF& media);
void stretchAbout(Annotation& a, double x, double y, const QPointF& centerMedia, const QSizeF& media);
void rotateAbout(Annotation& a, double radians, const QPointF& centerMedia, const QSizeF& media);

} // namespace AnnotationTransform
