#include "Track.h"
#include "Lang.h"

#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <optional>

std::optional<QPointF> Track::at(int frame) const
{
    auto it = points.find(frame);
    if (it == points.end()) return std::nullopt;
    return it->second;
}

int Track::creationFrame() const
{
    if (createdFrame >= 0) return createdFrame;
    // Older projects did not record creation time; hand-placed keys are the best evidence.
    if (!keys.empty()) return *keys.begin();
    return points.empty() ? 0 : points.begin()->first;
}

bool Track::visibleAt(int frame) const
{
    return visible && (!hideBeforeCreation || frame >= creationFrame());
}

QColor Track::colorFor(int index)
{
    // Muted, distinguishable on both bright and dark footage.
    static const QColor palette[] = { QColor(0xf2, 0xc9, 0x4c), QColor(0x5e, 0xc8, 0xe5), QColor(0xe8, 0x7a, 0x9a),
                                      QColor(0x8f, 0xd4, 0x7e), QColor(0xc3, 0x9b, 0xf2), QColor(0xf0, 0x9a, 0x5a) };
    return palette[index % 6];
}

QJsonObject Track::toJson() const
{
    QJsonObject pts;
    for (const auto& [f, p] : points) pts[QString::number(f)] = QJsonArray{ p.x(), p.y() };
    QJsonArray k;
    for (int f : keys) k.append(f);
    return { { "id", id }, { "name", name }, { "color", color.name() }, { "points", pts }, { "keys", k },
             { "createdFrame", creationFrame() }, { "visible", visible }, { "hideBeforeCreation", hideBeforeCreation } };
}

Track Track::fromJson(const QJsonObject& o)
{
    Track t;
    t.id = o.value("id").toString();
    t.name = o.value("name").toString();
    t.color = QColor::fromString(o.value("color").toString("#f2c94c"));
    const QJsonObject pts = o.value("points").toObject();
    for (auto it = pts.begin(); it != pts.end(); ++it) {
        const QJsonArray a = it.value().toArray();
        if (a.size() == 2) t.points[it.key().toInt()] = QPointF(a[0].toDouble(), a[1].toDouble());
    }
    for (const QJsonValue& v : o.value("keys").toArray()) t.keys.insert(v.toInt());
    t.createdFrame = o.value("createdFrame").toInt(-1);
    t.createdFrame = t.creationFrame();
    t.visible = o.value("visible").toBool(true);
    t.hideBeforeCreation = o.value("hideBeforeCreation").toBool(true);
    return t;
}

namespace TrackPainter {

namespace {

const QColor kEndColor(0xff, 0x5c, 0x5c);  // where the track ends

void crosshair(QPainter& p, const QPointF& q, double r, double u, const QColor& color)
{
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 140), 3 * u));
    p.drawEllipse(q, r, r);
    p.setPen(QPen(color, 1.5 * u));
    p.drawEllipse(q, r, r);
    p.drawLine(q - QPointF(r + 4 * u, 0), q - QPointF(3 * u, 0));
    p.drawLine(q + QPointF(3 * u, 0), q + QPointF(r + 4 * u, 0));
    p.drawLine(q - QPointF(0, r + 4 * u), q - QPointF(0, 3 * u));
    p.drawLine(q + QPointF(0, 3 * u), q + QPointF(0, r + 4 * u));
}

} // namespace

void paint(QPainter& p, const QVector<Track>& tracks, int frame, const QSizeF& m, const Style& st)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const double u = st.unit;
    QFont f(QStringLiteral("Segoe UI"));
    f.setPixelSize(std::max(1, int(11 * u)));
    p.setFont(f);

    // range > 0: comet tail — only the last `range` frames before the playhead, fading with age.
    const bool tail = st.range > 0;
    auto visible = [&](int fr) { return !tail || (fr <= frame && fr >= frame - st.range); };
    auto fade = [&](int fr) { return tail ? std::clamp(1.0 - double(frame - fr) / (st.range + 1), 0.0, 1.0) : 1.0; };
    auto toMedia = [&](const QPointF& n) { return QPointF(n.x() * m.width(), n.y() * m.height()); };

    for (const Track& t : tracks) {
        if (t.points.empty() || !t.visibleAt(frame)) continue;
        const bool selected = t.id == st.selectedId;
        const int endFrame = t.points.rbegin()->first;
        const double baseAlpha = selected ? 0.95 : 0.7;
        const double shadowW = (selected ? 3.2 : 2.6) * u, lineW = (selected ? 1.8 : 1.3) * u;

        if (!tail) {
            // Whole path as one polyline, broken where frames are missing (lost tracking).
            QPainterPath path;
            int prev = INT_MIN;
            for (const auto& [fr, pt] : t.points) {
                const QPointF q = toMedia(pt);
                if (fr == prev + 1) path.lineTo(q); else path.moveTo(q);
                prev = fr;
            }
            QColor line = t.color;
            line.setAlphaF(baseAlpha);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 110), shadowW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
            p.setPen(QPen(line, lineW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
        } else {
            // Segment by segment so each one can fade with its age.
            for (auto it = t.points.begin(); it != t.points.end(); ++it) {
                auto next = std::next(it);
                if (next == t.points.end() || next->first != it->first + 1) continue;
                if (!visible(it->first) || !visible(next->first)) continue;
                const double a = fade(next->first);
                const QPointF q0 = toMedia(it->second), q1 = toMedia(next->second);
                p.setPen(QPen(QColor(0, 0, 0, int(110 * a)), shadowW, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(q0, q1);
                QColor line = t.color;
                line.setAlphaF(baseAlpha * a);
                p.setPen(QPen(line, lineW, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(q0, q1);
            }
        }

        // Frame dots (past frames brighter than future, like Maya's trail).
        for (const auto& [fr, pt] : t.points) {
            if (!visible(fr) || fr == frame || fr == endFrame) continue;
            const QPointF q = toMedia(pt);
            QColor c = t.color;
            c.setAlphaF((fr < frame ? 0.9 : 0.55) * fade(fr));
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            if (t.keys.count(fr))
                p.drawRect(QRectF(q.x() - 3 * u, q.y() - 3 * u, 6 * u, 6 * u));
            else
                p.drawEllipse(q, 1.9 * u, 1.9 * u);
            if (st.frameNumbers && (fr % 5 == 0 || t.keys.count(fr))) {
                p.setPen(QColor(255, 255, 255, int(190 * fade(fr))));
                p.drawText(q + QPointF(5 * u, -4 * u), QString::number(fr));
            }
        }

        // End of the track in its own colour, with the frame number.
        if (endFrame != frame && visible(endFrame)) {
            const QPointF q = toMedia(t.points.rbegin()->second);
            p.setPen(QPen(QColor(0, 0, 0, 170), 2 * u));
            p.setBrush(kEndColor);
            p.drawEllipse(q, 6 * u, 6 * u);
            QFont bold = f;
            bold.setBold(true);
            p.setFont(bold);
            const QString label = L("End · %1", "끝 · %1").arg(endFrame);
            p.setPen(QColor(0, 0, 0, 170));
            p.drawText(q + QPointF(10 * u + u, 4 * u + u), label);  // shadow for bright footage
            p.setPen(kEndColor);
            p.drawText(q + QPointF(10 * u, 4 * u), label);
            p.setFont(f);
        }

        // Current frame marker (in the end colour when the playhead sits on the last tracked frame).
        if (const auto cur = t.at(frame)) {
            const QPointF q = toMedia(*cur);
            const double r = (selected ? 9 : 7) * u;
            const bool atEnd = frame == endFrame && t.points.size() > 1;
            crosshair(p, q, r, u, atEnd ? kEndColor : t.color);
            if (atEnd) {
                p.setPen(kEndColor);
                p.drawText(q + QPointF(r + 6 * u, -r), L("End · %1", "끝 · %1").arg(endFrame));
            }
            if (selected && !t.name.isEmpty()) {
                p.setPen(t.color);
                p.drawText(q + QPointF(r + 6 * u, r + 10 * u), t.name);
            }
        }
    }
    p.restore();
}

} // namespace TrackPainter
