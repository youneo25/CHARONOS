#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QVector>

#include <map>
#include <optional>
#include <set>

class QPainter;

// A tracked point on one clip: a position per frame (normalized 0..1 to the media size).
// `keys` are frames the user placed or corrected by hand; tracking never overwrites them.
struct Track {
    QString id;
    QString name;
    QColor color;
    std::map<int, QPointF> points;
    std::set<int> keys;
    int createdFrame = -1;  // Kept independently of backward tracking / later corrections.
    bool visible = true;
    bool hideBeforeCreation = true;

    int creationFrame() const;
    bool visibleAt(int frame) const;
    std::optional<QPointF> at(int frame) const;
    QJsonObject toJson() const;
    static Track fromJson(const QJsonObject& o);
    static QColor colorFor(int index);
};

// Maya-style motion trail: a polyline through the tracked positions with a dot per frame,
// squares on keys and a crosshair on the current frame.
namespace TrackPainter {

struct Style {
    int range = 0;          // 0 = whole path; N = only the last N frames before the playhead, fading out
    bool frameNumbers = false;
    double unit = 1.0;      // media pixels per screen pixel, keeps markers a constant size
    QString selectedId;
};

void paint(QPainter& p, const QVector<Track>& tracks, int frame, const QSizeF& media, const Style& style);

} // namespace TrackPainter
