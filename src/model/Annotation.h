#pragma once

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QVector>

// One drawn object. Coordinates are normalized to the media size (0..1), exactly like
// the Electron version, so old project files load unchanged. `size` and `fontSize` are
// in media pixels.
struct Annotation {
    QString id;
    QString type;  // stroke | line | arrow | rect | circle | text | image
    QColor color = QColor(0x11, 0x11, 0x11);
    double size = 2.0;
    double opacity = 1.0;
    bool locked = false;

    QVector<QPointF> points;                  // stroke
    QVector<float> pressures;                 // stroke, per point 0..1 (pen tablets only; empty for mouse)
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;    // line / arrow
    double x = 0, y = 0, w = 0, h = 0;        // rect / text / image
    double cx = 0, cy = 0, rx = 0, ry = 0;    // circle
    QString text;
    double fontSize = 28;
    QString fontFamily = QStringLiteral("Malgun Gothic");
    bool bold = false;
    bool italic = false;
    bool useBg = false;
    QString bgColor;
    QString src;  // image data URL or path

    // transform (normalized translate, uniform scale, rotation in radians around bbox centre)
    double tx = 0, ty = 0, scale = 1, angle = 0;
    double stretchX = 1, stretchY = 1;
    QString penStyle = QStringLiteral("pencil"); // pencil | pen | highlighter
    QString trackId;
    QPointF trackAnchor; // tracked position when joined; translation is relative to this anchor
    // 1.1 annotation clips: objects drawn together share a clip; its lane is the annotation track
    // (higher lanes draw on top). -1 = not placed yet (laid out automatically).
    QString clipId;
    int lane = -1;
    QColor clipColor;  // label colour of the clip block (invalid = automatic)

    QJsonObject extra;  // unknown keys, preserved on save

    QJsonObject toJson() const;
    static Annotation fromJson(const QJsonObject& o);
    static QString newId();
};

using AnnotationList = QVector<Annotation>;

QJsonArray annotationsToJson(const AnnotationList& list);
AnnotationList annotationsFromJson(const QJsonArray& arr);
