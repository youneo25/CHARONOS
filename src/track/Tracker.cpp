#include "Tracker.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kMinScore = 0.55;  // below this the point is considered lost

void normalize(std::vector<float>& v)
{
    double mean = 0;
    for (float x : v) mean += x;
    mean /= double(v.size());
    double norm = 0;
    for (float& x : v) {
        x = float(x - mean);
        norm += double(x) * x;
    }
    norm = std::sqrt(norm);
    if (norm > 1e-6)
        for (float& x : v) x = float(x / norm);
}
} // namespace

GrayFrame toGray(const QImage& src, int maxDim)
{
    QImage img = src;
    const int longest = std::max(img.width(), img.height());
    GrayFrame g;
    g.scale = 1.0;
    if (longest > maxDim) {
        g.scale = double(maxDim) / longest;
        img = img.scaled(qRound(img.width() * g.scale), qRound(img.height() * g.scale), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    }
    img = img.convertToFormat(QImage::Format_Grayscale8);
    g.w = img.width();
    g.h = img.height();
    g.px.resize(size_t(g.w) * g.h);
    for (int y = 0; y < g.h; ++y) std::copy_n(img.constScanLine(y), g.w, g.px.data() + size_t(y) * g.w);
    return g;
}

PointTracker::PointTracker(int patch, int searchRadius)
    : m_half(patch / 2)
    , m_radius(searchRadius)
{
}

bool PointTracker::sample(const GrayFrame& f, const QPointF& c, std::vector<float>& out) const
{
    const int n = 2 * m_half + 1;
    if (c.x() - m_half < 0 || c.y() - m_half < 0 || c.x() + m_half + 1 >= f.w || c.y() + m_half + 1 >= f.h) return false;
    out.resize(size_t(n) * n);
    const double ox = c.x() - m_half, oy = c.y() - m_half;
    const int ix = int(std::floor(ox)), iy = int(std::floor(oy));
    const float fx = float(ox - ix), fy = float(oy - iy);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float a = f.at(ix + x, iy + y), b = f.at(ix + x + 1, iy + y);
            const float c0 = f.at(ix + x, iy + y + 1), d = f.at(ix + x + 1, iy + y + 1);
            out[size_t(y) * n + x] = (a * (1 - fx) + b * fx) * (1 - fy) + (c0 * (1 - fx) + d * fx) * fy;
        }
    normalize(out);
    return true;
}

bool PointTracker::init(const GrayFrame& frame, const QPointF& pos)
{
    m_pos = pos;
    m_vel = {};
    if (!sample(frame, pos, m_tmpl)) return false;
    m_key = m_tmpl;
    return true;
}

bool PointTracker::match(const GrayFrame& f, const std::vector<float>& tmpl, const QPoint& around, QPointF& best,
                         double& score) const
{
    const int n = 2 * m_half + 1;
    const int R = m_radius;
    const int side = 2 * R + 1;
    std::vector<double> scores(size_t(side) * side, -2.0);

    // Integral images over the search area for the window mean / energy.
    const int x0 = around.x() - R - m_half, y0 = around.y() - R - m_half;
    const int span = side + n - 1;
    std::vector<double> s1(size_t(span + 1) * (span + 1), 0.0), s2(s1.size(), 0.0);
    auto idx = [&](int x, int y) { return size_t(y) * (span + 1) + x; };
    for (int y = 0; y < span; ++y)
        for (int x = 0; x < span; ++x) {
            const int gx = x0 + x, gy = y0 + y;
            const double v = (gx >= 0 && gy >= 0 && gx < f.w && gy < f.h) ? f.at(gx, gy) : 0.0;
            s1[idx(x + 1, y + 1)] = v + s1[idx(x, y + 1)] + s1[idx(x + 1, y)] - s1[idx(x, y)];
            s2[idx(x + 1, y + 1)] = v * v + s2[idx(x, y + 1)] + s2[idx(x + 1, y)] - s2[idx(x, y)];
        }

    const double count = double(n) * n;
    int bx = -1, by = -1;
    score = -2.0;
    for (int dy = 0; dy < side; ++dy) {
        const int top = around.y() - R + dy - m_half;
        if (top < 0 || top + n > f.h) continue;
        for (int dx = 0; dx < side; ++dx) {
            const int left = around.x() - R + dx - m_half;
            if (left < 0 || left + n > f.w) continue;
            const double sum = s1[idx(dx + n, dy + n)] - s1[idx(dx, dy + n)] - s1[idx(dx + n, dy)] + s1[idx(dx, dy)];
            const double sq = s2[idx(dx + n, dy + n)] - s2[idx(dx, dy + n)] - s2[idx(dx + n, dy)] + s2[idx(dx, dy)];
            const double energy = sq - sum * sum / count;
            if (energy < 1e-3) continue;
            double cross = 0;
            const float* t = tmpl.data();
            for (int y = 0; y < n; ++y) {
                const uint8_t* row = f.px.data() + size_t(top + y) * f.w + left;
                for (int x = 0; x < n; ++x) cross += double(row[x]) * t[y * n + x];
            }
            const double s = cross / std::sqrt(energy);
            scores[size_t(dy) * side + dx] = s;
            if (s > score) {
                score = s;
                bx = dx;
                by = dy;
            }
        }
    }
    if (bx < 0) return false;

    // Sub-pixel: fit a parabola through the peak and its neighbours on each axis.
    auto sc = [&](int x, int y) { return (x < 0 || y < 0 || x >= side || y >= side) ? -2.0 : scores[size_t(y) * side + x]; };
    auto vertex = [](double l, double c, double r) {
        const double d = l - 2 * c + r;
        return (l > -2 && r > -2 && std::abs(d) > 1e-9) ? std::clamp(0.5 * (l - r) / d, -0.5, 0.5) : 0.0;
    };
    const double ox = vertex(sc(bx - 1, by), sc(bx, by), sc(bx + 1, by));
    const double oy = vertex(sc(bx, by - 1), sc(bx, by), sc(bx, by + 1));
    best = QPointF(around.x() - R + bx + ox, around.y() - R + by + oy);
    return true;
}

PointTracker::Result PointTracker::step(const GrayFrame& frame)
{
    Result r;
    const QPointF predicted = m_pos + m_vel;
    const QPoint around(qRound(predicted.x()), qRound(predicted.y()));

    QPointF best;
    double score = -2;
    if (!match(frame, m_tmpl, around, best, score) || score < kMinScore) {
        // Appearance changed too much frame-to-frame: retry against the original key patch.
        QPointF b2;
        double s2 = -2;
        if (match(frame, m_key, around, b2, s2) && s2 > score) {
            best = b2;
            score = s2;
        }
    }
    r.score = score;
    if (score < kMinScore) return r;

    m_vel = (best - m_pos) * 0.8;
    m_pos = best;
    std::vector<float> fresh;
    if (sample(frame, best, fresh)) {
        for (size_t i = 0; i < fresh.size(); ++i) m_tmpl[i] = 0.7f * fresh[i] + 0.3f * m_key[i];
        normalize(m_tmpl);
    }
    r.ok = true;
    r.pos = best;
    return r;
}
