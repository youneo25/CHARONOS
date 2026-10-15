#include "Annotation.h"

#include <QJsonArray>
#include <QUuid>

#include <cmath>
#include <algorithm>

namespace {

const QStringList kKnownKeys = {
    "id", "type", "color", "size", "opacity", "locked", "points", "pressure", "x1", "y1", "x2", "y2", "x", "y", "w", "h",
    "cx", "cy", "rx", "ry", "text", "fontSize", "fontFamily", "bold", "italic", "useBg", "bgColor", "src", "transform", "penStyle", "trackId", "trackAnchor",
};

double num(const QJsonObject& o, const char* key, double fallback = 0.0)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

} // namespace

QString Annotation::newId()
{
    return QStringLiteral("n") + QUuid::createUuid().toString(QUuid::Id128).left(12);
}

QJsonObject Annotation::toJson() const
{
    QJsonObject o = extra;
    o["id"] = id;
    o["type"] = type;
    o["color"] = color.name(QColor::HexRgb);
    o["size"] = size;
    o["opacity"] = opacity;
    o["penStyle"] = penStyle;
    if (!trackId.isEmpty()) {
        o["trackId"] = trackId;
        o["trackAnchor"] = QJsonObject{{"x", trackAnchor.x()}, {"y", trackAnchor.y()}};
    }
    if (locked) o["locked"] = true;

    if (type == "stroke") {
        QJsonArray pts;
        for (const QPointF& p : points) pts.append(QJsonObject{ { "x", p.x() }, { "y", p.y() } });
        o["points"] = pts;
        if (!pressures.isEmpty()) {
            QJsonArray pr;
            for (float v : pressures) pr.append(std::round(v * 1000) / 1000.0);
            o["pressure"] = pr;
        }
    } else if (type == "line" || type == "arrow") {
        o["x1"] = x1; o["y1"] = y1; o["x2"] = x2; o["y2"] = y2;
    } else if (type == "rect" || type == "image") {
        o["x"] = x; o["y"] = y; o["w"] = w; o["h"] = h;
        if (type == "image") o["src"] = src;
    } else if (type == "circle") {
        o["cx"] = cx; o["cy"] = cy; o["rx"] = rx; o["ry"] = ry;
    } else if (type == "text") {
        o["x"] = x; o["y"] = y;
        o["text"] = text;
        o["fontSize"] = fontSize;
        o["fontFamily"] = fontFamily;
        o["bold"] = bold;
        o["italic"] = italic;
        o["useBg"] = useBg;
        o["bgColor"] = bgColor.isEmpty() ? QJsonValue() : QJsonValue(bgColor);
    }

    QJsonObject t{ { "tx", tx }, { "ty", ty }, { "scale", scale } };
    t["stretchX"] = stretchX;
    t["stretchY"] = stretchY;
    if (angle != 0.0) t["angle"] = angle;
    o["transform"] = t;
    return o;
}

Annotation Annotation::fromJson(const QJsonObject& o)
{
    Annotation a;
    a.extra = o;
    for (const QString& k : kKnownKeys) a.extra.remove(k);

    a.id = o.value("id").toString();
    if (a.id.isEmpty()) a.id = newId();
    a.type = o.value("type").toString();
    const QColor c = QColor::fromString(o.value("color").toString());
    if (c.isValid()) a.color = c;
    a.size = num(o, "size", 2.0);
    a.opacity = num(o, "opacity", 1.0);
    a.locked = o.value("locked").toBool();
    a.penStyle = o.value("penStyle").toString("pencil");
    a.trackId = o.value("trackId").toString();
    const auto anchor = o.value("trackAnchor").toObject();
    a.trackAnchor = QPointF(num(anchor, "x"), num(anchor, "y"));

    for (const QJsonValue& v : o.value("points").toArray()) {
        const QJsonObject p = v.toObject();
        a.points.append(QPointF(num(p, "x"), num(p, "y")));
    }
    for (const QJsonValue& v : o.value("pressure").toArray()) a.pressures.append(float(v.toDouble(1.0)));
    if (a.pressures.size() != a.points.size()) a.pressures.clear();
    a.x1 = num(o, "x1"); a.y1 = num(o, "y1"); a.x2 = num(o, "x2"); a.y2 = num(o, "y2");
    a.x = num(o, "x"); a.y = num(o, "y"); a.w = num(o, "w"); a.h = num(o, "h");
    a.cx = num(o, "cx"); a.cy = num(o, "cy"); a.rx = num(o, "rx"); a.ry = num(o, "ry");
    a.text = o.value("text").toString();
    a.fontSize = num(o, "fontSize", 28.0);
    a.fontFamily = o.value("fontFamily").toString(QStringLiteral("Malgun Gothic"));
    a.bold = o.value("bold").toBool();
    a.italic = o.value("italic").toBool();
    a.useBg = o.value("useBg").toBool();
    a.bgColor = o.value("bgColor").toString();
    a.src = o.value("src").toString();

    const QJsonObject t = o.value("transform").toObject();
    a.tx = num(t, "tx");
    a.ty = num(t, "ty");
    a.scale = num(t, "scale", 1.0);
    a.angle = num(t, "angle");
    a.stretchX = std::clamp(num(t, "stretchX", 1), 0.001, 1000.0);
    a.stretchY = std::clamp(num(t, "stretchY", 1), 0.001, 1000.0);
    return a;
}

QJsonArray annotationsToJson(const AnnotationList& list)
{
    QJsonArray arr;
    for (const Annotation& a : list) arr.append(a.toJson());
    return arr;
}

AnnotationList annotationsFromJson(const QJsonArray& arr)
{
    AnnotationList list;
    list.reserve(arr.size());
    for (const QJsonValue& v : arr)
        if (v.isObject()) list.append(Annotation::fromJson(v.toObject()));
    return list;
}
