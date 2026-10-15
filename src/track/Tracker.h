#pragma once

#include <QImage>
#include <QPointF>

#include <cstdint>
#include <vector>

// Luminance frame at tracking resolution.
struct GrayFrame {
    int w = 0;
    int h = 0;
    double scale = 1.0;  // gray pixels per media pixel
    std::vector<uint8_t> px;
    uint8_t at(int x, int y) const { return px[size_t(y) * w + x]; }
};

GrayFrame toGray(const QImage& rgba, int maxDim = 1280);

// Single-point tracker: zero-mean normalized cross-correlation of a square patch inside a
// search window around the velocity-predicted position, with sub-pixel peak refinement.
// The template follows the point frame to frame but is blended with the first (key)
// template so it does not drift away from what the user clicked.
class PointTracker {
public:
    struct Result {
        bool ok = false;
        QPointF pos;  // gray pixels
        double score = 0.0;
    };

    explicit PointTracker(int patch = 25, int searchRadius = 40);

    bool init(const GrayFrame& frame, const QPointF& pos);
    Result step(const GrayFrame& frame);
    QPointF position() const { return m_pos; }

private:
    bool sample(const GrayFrame& f, const QPointF& center, std::vector<float>& out) const;
    bool match(const GrayFrame& f, const std::vector<float>& tmpl, const QPoint& around, QPointF& best, double& score) const;

    int m_half;
    int m_radius;
    std::vector<float> m_tmpl;
    std::vector<float> m_key;
    QPointF m_pos;
    QPointF m_vel;
};
