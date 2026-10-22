#include "MotionGraph.h"

#include "Icons.h"
#include "Lang.h"
#include "Palette.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

const QColor kX(0xe5, 0x72, 0x66);
const QColor kY(0x7c, 0xc4, 0x8a);
const QColor kSpeed(0xd9, 0x77, 0x57, 150);

QFont mono(int px)
{
    QFont f(QStringLiteral("Cascadia Mono"));
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(px);
    return f;
}

} // namespace

// ============================================================================ plot

class MotionPlot final : public QWidget {
public:
    explicit MotionPlot(MotionGraph* graph)
        : QWidget(graph)
        , g(graph)
    {
        setMinimumHeight(170);
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    MotionGraph* g;
    int hoverFrame = -1;

    QRectF plotRect() const { return QRectF(44, 10, width() - 56, height() - 62); }
    QRectF stripRect() const { return QRectF(44, height() - 40, width() - 56, 30); }
    int firstFrame() const { return g->m_analysis.samples.isEmpty() ? 0 : g->m_analysis.samples.front().frame; }
    int lastFrame() const { return g->m_analysis.samples.isEmpty() ? 0 : g->m_analysis.samples.back().frame; }
    double xFor(double frame) const
    {
        const QRectF r = plotRect();
        const int span = std::max(1, lastFrame() - firstFrame());
        return r.left() + (frame - firstFrame()) / span * r.width();
    }
    int frameAt(double x) const
    {
        const QRectF r = plotRect();
        const int span = std::max(1, lastFrame() - firstFrame());
        return std::clamp(int(std::lround(firstFrame() + (x - r.left()) / std::max(1.0, r.width()) * span)), firstFrame(), lastFrame());
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Pal::viewer);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& a = g->m_analysis;
        const QRectF r = plotRect();
        p.setPen(QPen(Pal::divider, 1));
        p.setBrush(QColor(0x1b, 0x1b, 0x1a));
        p.drawRoundedRect(r.adjusted(-1, -1, 1, 1), 4, 4);
        if (a.samples.size() < 2) {
            p.setPen(Pal::textFaint);
            p.drawText(rect(), Qt::AlignCenter,
                       L("Track a point (Tracker M, then Ctrl+T) to see its spacing and timing here.",
                         "점을 추적하면(트래커 M → Ctrl+T) 간격과 타이밍이 여기에 표시됩니다."));
            return;
        }
        // horizontal guides
        p.setPen(QPen(Pal::divider, 1, Qt::DotLine));
        for (int i = 1; i < 4; ++i) p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));

        // speed as a filled area (px / frame)
        if (g->m_showSpeed->isChecked() && a.maxSpeed > 0) {
            QPainterPath area;
            area.moveTo(xFor(a.samples.front().frame), r.bottom());
            for (const auto& s : a.samples)
                area.lineTo(xFor(s.frame), r.bottom() - (s.hasSpeed ? s.speed / a.maxSpeed : 0) * r.height() * 0.95);
            area.lineTo(xFor(a.samples.back().frame), r.bottom());
            area.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(kSpeed.red(), kSpeed.green(), kSpeed.blue(), 70));
            p.drawPath(area);
            p.setPen(QPen(kSpeed, 1.4));
            p.setBrush(Qt::NoBrush);
            QPainterPath line;
            bool started = false;
            for (const auto& s : a.samples) {
                const QPointF pt(xFor(s.frame), r.bottom() - (s.hasSpeed ? s.speed / a.maxSpeed : 0) * r.height() * 0.95);
                if (!started) line.moveTo(pt), started = true;
                else line.lineTo(pt);
            }
            p.drawPath(line);
        }
        // X and Y position, each normalised to its own range (screen up = up)
        auto plotAxis = [&](bool yAxis, const QColor& color) {
            double lo = 1e18, hi = -1e18;
            for (const auto& s : a.samples) {
                const double v = yAxis ? -s.pos.y() : s.pos.x();
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            const double span = std::max(1e-6, hi - lo);
            QPainterPath path;
            bool started = false;
            int prev = -1;
            for (const auto& s : a.samples) {
                const double v = yAxis ? -s.pos.y() : s.pos.x();
                const QPointF pt(xFor(s.frame), r.bottom() - 4 - (v - lo) / span * (r.height() - 8));
                if (!started || s.frame != prev + 1) path.moveTo(pt), started = true;
                else path.lineTo(pt);
                prev = s.frame;
            }
            p.setPen(QPen(color, 1.6));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        };
        if (g->m_showX->isChecked()) plotAxis(false, kX);
        if (g->m_showY->isChecked()) plotAxis(true, kY);

        // suggested keys
        p.setFont(mono(9));
        for (int k : a.keys) {
            const double x = xFor(k);
            p.setPen(QPen(QColor(0xf2, 0xc9, 0x4c, 200), 1, Qt::DashLine));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(QColor(0xf2, 0xc9, 0x4c));
            p.drawText(QPointF(x + 2, r.top() + 10), QString::number(k));
        }
        // frame labels
        p.setPen(Pal::textFaint);
        p.drawText(QRectF(0, r.bottom() + 2, 44, 14), Qt::AlignRight, QString::number(firstFrame()));
        p.drawText(QRectF(r.right() - 60, r.bottom() + 2, 60, 14), Qt::AlignRight, QString::number(lastFrame()));
        p.drawText(QRectF(0, r.top(), 40, 14), Qt::AlignRight, QStringLiteral("max"));

        // spacing chart: one tick per frame at its distance along the path
        const QRectF sr = stripRect();
        p.setPen(QPen(Pal::border, 1));
        p.drawLine(QPointF(sr.left(), sr.center().y()), QPointF(sr.right(), sr.center().y()));
        p.setPen(Pal::textFaint);
        p.drawText(QRectF(0, sr.top(), 40, sr.height()), Qt::AlignRight | Qt::AlignVCenter, L("spacing", "간격"));
        double travelled = 0;
        for (const auto& s : a.samples) {
            if (s.hasSpeed) travelled += s.speed;
            const double x = sr.left() + (a.distance > 0 ? travelled / a.distance : 0) * sr.width();
            const bool key = std::binary_search(a.keys.begin(), a.keys.end(), s.frame);
            const bool now = s.frame == g->m_current;
            const double h = key ? 11 : 6;
            p.setPen(QPen(now ? Pal::accent : key ? QColor(0xf2, 0xc9, 0x4c) : Pal::textDim, now ? 2.2 : 1.2));
            p.drawLine(QPointF(x, sr.center().y() - h), QPointF(x, sr.center().y() + h));
        }

        // playhead
        if (g->m_current >= firstFrame() && g->m_current <= lastFrame()) {
            const double x = xFor(g->m_current);
            p.setPen(QPen(QColor(0xe5, 0x48, 0x3e), 1.5));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        if (hoverFrame >= 0) {
            p.setPen(QPen(QColor(255, 255, 255, 60), 1));
            p.drawLine(QPointF(xFor(hoverFrame), r.top()), QPointF(xFor(hoverFrame), r.bottom()));
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && g->m_analysis.samples.size() >= 2) emit g->frameClicked(frameAt(e->position().x()));
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (g->m_analysis.samples.size() < 2) return;
        hoverFrame = frameAt(e->position().x());
        if (e->buttons() & Qt::LeftButton) emit g->frameClicked(hoverFrame);
        update();
    }
    void leaveEvent(QEvent*) override
    {
        hoverFrame = -1;
        update();
    }
};

// ============================================================================ graph

MotionGraph::MotionGraph(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("MotionGraph");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 8);
    v->setSpacing(6);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(4);
    m_track = new QComboBox;
    m_track->setObjectName("MotionGraphTrack");
    m_track->setMinimumWidth(140);
    bar->addWidget(m_track);
    auto toggle = [&](const QString& text, const QColor& color, const QString& tip) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setCheckable(true);
        b->setChecked(true);
        b->setToolTip(tip);
        b->setStyleSheet(QStringLiteral("QToolButton { color: %1; font-weight: 600; padding: 3px 8px; }").arg(color.name()));
        bar->addWidget(b);
        connect(b, &QToolButton::toggled, this, [this] { m_plot->update(); });
        return b;
    };
    m_showX = toggle(QStringLiteral("X"), kX, L("Horizontal position", "가로 위치"));
    m_showY = toggle(QStringLiteral("Y"), kY, L("Vertical position (up = up)", "세로 위치 (위 = 위)"));
    m_showSpeed = toggle(L("Speed", "속도"), QColor(0xd9, 0x77, 0x57), L("Distance moved per frame", "프레임당 이동 거리"));
    bar->addStretch(1);
    auto* bookmark = new QToolButton;
    bookmark->setIcon(Icons::icon("bookmark"));
    bookmark->setText(L("Bookmark Keys", "키 북마크"));
    bookmark->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bookmark->setToolTip(L("Add a bookmark on every suggested key pose (stops and direction changes)",
                           "추천 키 포즈(멈춤·방향 전환)마다 북마크를 추가합니다"));
    bar->addWidget(bookmark);
    auto* copy = new QToolButton;
    copy->setText(L("Copy CSV", "CSV 복사"));
    copy->setToolTip(L("frame, x, y, speed (px) for spreadsheets", "프레임, x, y, 속도(px) — 스프레드시트용"));
    bar->addWidget(copy);
    v->addLayout(bar);
    m_plot = new MotionPlot(this);
    v->addWidget(m_plot, 1);
    m_readout = new QLabel;
    m_readout->setObjectName("Readout");
    m_readout->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_readout);
    m_summary = new QLabel;
    m_summary->setObjectName("Muted");
    m_summary->setWordWrap(true);
    v->addWidget(m_summary);

    connect(m_track, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_filling) emit trackChosen(trackId());
    });
    connect(bookmark, &QToolButton::clicked, this, [this] {
        if (!m_analysis.keys.isEmpty()) emit bookmarkRequested(m_analysis.keys);
    });
    connect(copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(csv()); });
}

void MotionGraph::setTracks(const QVector<std::tuple<QString, QString, QColor>>& tracks, const QString& selectedId)
{
    m_filling = true;
    m_track->clear();
    int index = 0;
    for (const auto& [id, name, color] : tracks) {
        QPixmap dot(12, 12);
        dot.fill(Qt::transparent);
        QPainter p(&dot);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(1, 1, 10, 10));
        p.end();
        m_track->addItem(QIcon(dot), name, id);
        if (id == selectedId) index = m_track->count() - 1;
    }
    if (tracks.isEmpty()) m_track->addItem(L("No tracks in this clip", "이 클립에 트랙 없음"));
    m_track->setCurrentIndex(index);
    m_track->setEnabled(!tracks.isEmpty());
    m_filling = false;
}

QString MotionGraph::trackId() const { return m_track->currentData().toString(); }

MotionGraph::Analysis MotionGraph::analyze(const std::map<int, QPointF>& points, QSizeF media)
{
    Analysis a;
    for (const auto& [frame, n] : points) {
        Sample s;
        s.frame = frame;
        s.pos = QPointF(n.x() * media.width(), n.y() * media.height());
        if (!a.samples.isEmpty() && a.samples.back().frame == frame - 1) {
            s.speed = std::hypot(s.pos.x() - a.samples.back().pos.x(), s.pos.y() - a.samples.back().pos.y());
            s.hasSpeed = true;
        }
        a.samples.append(s);
    }
    int counted = 0;
    for (const auto& s : a.samples)
        if (s.hasSpeed) {
            a.maxSpeed = std::max(a.maxSpeed, s.speed);
            a.meanSpeed += s.speed;
            a.distance += s.speed;
            ++counted;
        }
    if (counted) a.meanSpeed /= counted;
    if (a.samples.size() < 3) return a;

    // Smoothed speed so tracker jitter doesn't produce a key on every frame.
    const int n = int(a.samples.size());
    QVector<double> smooth(n, 0.0);
    for (int i = 0; i < n; ++i) {
        double sum = 0;
        int c = 0;
        for (int j = std::max(0, i - 1); j <= std::min(n - 1, i + 1); ++j)
            if (a.samples[j].hasSpeed) sum += a.samples[j].speed, ++c;
        smooth[i] = c ? sum / c : 0;
    }
    auto velocity = [&](int i) {
        if (i <= 0 || !a.samples[i].hasSpeed) return QPointF();
        return a.samples[i].pos - a.samples[i - 1].pos;
    };
    QVector<int> keys{ a.samples.front().frame };
    const double still = std::max(0.25, a.meanSpeed * 0.08);  // "not moving" (a hold)
    bool wasStill = smooth[0] <= still;
    for (int i = 1; i < n - 1; ++i) {
        const bool isStill = smooth[i] <= still;
        const bool slow = smooth[i] <= smooth[i - 1] && smooth[i] <= smooth[i + 1] && smooth[i] < a.meanSpeed * 0.6;
        // A reversal on either axis is an extreme of the arc (top of a jump, end of a swing).
        const QPointF v0 = velocity(i), v1 = velocity(i + 1);
        const double minMove = std::max(0.75, a.meanSpeed * 0.15);
        const bool reverseX = v0.x() * v1.x() < 0 && std::abs(v0.x()) > minMove * 0.5 && std::abs(v1.x()) > minMove * 0.5;
        const bool reverseY = v0.y() * v1.y() < 0 && std::abs(v0.y()) > minMove * 0.5 && std::abs(v1.y()) > minMove * 0.5;
        // A hold gives one key where the motion stops, not one every few frames.
        if (((slow && !(isStill && wasStill)) || reverseX || reverseY) && a.samples[i].frame - keys.back() >= 3)
            keys << a.samples[i].frame;
        wasStill = isStill;
        if (smooth[i] >= smooth[i - 1] && smooth[i] > smooth[i + 1] && smooth[i] > a.meanSpeed * 1.4) a.fastest << a.samples[i].frame;
    }
    if (a.samples.back().frame - keys.back() >= 2) keys << a.samples.back().frame;
    a.keys = keys;
    return a;
}

void MotionGraph::setData(const std::map<int, QPointF>& points, QSizeF mediaSize, double fps, const QString& clipName)
{
    m_analysis = analyze(points, mediaSize);
    m_fps = fps > 0 ? fps : 24.0;
    m_clip = clipName;
    const auto& a = m_analysis;
    if (a.samples.size() >= 2) {
        const double seconds = (a.samples.back().frame - a.samples.front().frame) / m_fps;
        m_summary->setText(L("%1 · frames %2–%3 (%4 s) · path %5 px · mean %6 px/f · peak %7 px/f · %8 suggested keys",
                             "%1 · 프레임 %2–%3 (%4초) · 경로 %5 px · 평균 %6 px/f · 최고 %7 px/f · 추천 키 %8개")
                               .arg(m_clip)
                               .arg(a.samples.front().frame)
                               .arg(a.samples.back().frame)
                               .arg(seconds, 0, 'f', 2)
                               .arg(a.distance, 0, 'f', 0)
                               .arg(a.meanSpeed, 0, 'f', 1)
                               .arg(a.maxSpeed, 0, 'f', 1)
                               .arg(a.keys.size()));
    } else {
        m_summary->setText(m_clip);
    }
    updateReadout();
    m_plot->update();
}

void MotionGraph::setCurrentFrame(int frame)
{
    if (frame == m_current) return;
    m_current = frame;
    updateReadout();
    m_plot->update();
}

void MotionGraph::updateReadout()
{
    const auto& s = m_analysis.samples;
    auto it = std::find_if(s.begin(), s.end(), [&](const Sample& x) { return x.frame == m_current; });
    if (it == s.end()) {
        m_readout->setText(m_current >= 0 ? L("Frame %1 · not tracked", "프레임 %1 · 추적 없음").arg(m_current) : QString());
        return;
    }
    double accel = 0;
    if (it != s.begin() && it->hasSpeed && std::prev(it)->hasSpeed) accel = it->speed - std::prev(it)->speed;
    m_readout->setText(QStringLiteral("F %1   x %2  y %3   %4 px/f  (%5 px/s)   Δ %6")
                           .arg(it->frame)
                           .arg(it->pos.x(), 0, 'f', 1)
                           .arg(it->pos.y(), 0, 'f', 1)
                           .arg(it->speed, 0, 'f', 2)
                           .arg(it->speed * m_fps, 0, 'f', 0)
                           .arg(accel, 0, 'f', 2));
}

QString MotionGraph::csv() const
{
    QString out = QStringLiteral("frame,x_px,y_px,speed_px_per_frame,suggested_key\n");
    for (const auto& s : m_analysis.samples)
        out += QStringLiteral("%1,%2,%3,%4,%5\n")
                   .arg(s.frame)
                   .arg(s.pos.x(), 0, 'f', 2)
                   .arg(s.pos.y(), 0, 'f', 2)
                   .arg(s.hasSpeed ? QString::number(s.speed, 'f', 3) : QString())
                   .arg(m_analysis.keys.contains(s.frame) ? 1 : 0);
    return out;
}
