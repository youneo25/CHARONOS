#include "Timeline.h"

#include "Palette.h"
#include "Lang.h"

#include <QMouseEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <algorithm>

namespace {

constexpr int kSideLabelW = 92;

QFont smallFont(const QWidget* w, int px)
{
    QFont f = w->font();
    f.setPixelSize(px);
    return f;
}

QFont monoFont(int px)
{
    QFont f(QStringLiteral("Cascadia Mono"));
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(px);
    return f;
}

} // namespace

// ================================================================ Timeline

Timeline::Timeline(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("TimelineBar");
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(60);
    setFocusPolicy(Qt::NoFocus);
}

void Timeline::setWaveform(QVector<float> peaks) { m_wave = std::move(peaks); update(); }

void Timeline::setFrameCount(qint64 count) { m_count = std::max<qint64>(0, count); update(); }
void Timeline::setCurrent(qint64 frame) { if (frame != m_current) { m_current = frame; update(); } }
void Timeline::setMarks(QVector<qint64> annotated, QVector<qint64> bookmarks)
{
    m_annotated = std::move(annotated);
    std::sort(m_annotated.begin(), m_annotated.end());
    m_annotated.erase(std::unique(m_annotated.begin(), m_annotated.end()), m_annotated.end());
    m_bookmarks = std::move(bookmarks);
    update();
}
void Timeline::setSegments(QVector<qint64> starts) { m_segments = std::move(starts); update(); }
void Timeline::setLoop(bool enabled, qint64 in, qint64 out) { m_loop = enabled; m_in = in; m_out = out; update(); }
void Timeline::setCacheMask(QBitArray mask) { m_cache = std::move(mask); update(); }
void Timeline::setRates(double target, double measured) { m_target = target; m_measured = measured; update(); }

QRectF Timeline::trackRect() const { return QRectF(kSideLabelW, 25, width() - 2 * kSideLabelW, 7); }

double Timeline::xFor(double frame) const
{
    const QRectF t = trackRect();
    if (m_count <= 1) return t.left();
    return t.left() + t.width() * (frame / double(m_count - 1));
}

qint64 Timeline::frameAt(double x) const
{
    if (m_count <= 1) return 0;
    const QRectF t = trackRect();
    return std::clamp<qint64>(qRound64((x - t.left()) / t.width() * (m_count - 1)), 0, m_count - 1);
}

QRectF Timeline::rateRect() const { return QRectF(width() - kSideLabelW + 6, 16, kSideLabelW - 12, 24); }

void Timeline::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF t = trackRect();

    // side labels
    p.setFont(smallFont(this, 11));
    p.setPen(Pal::textDim);
    p.drawText(QRectF(10, 16, kSideLabelW - 16, 24), Qt::AlignLeft | Qt::AlignVCenter,
               m_count ? QStringLiteral("%1 frames").arg(m_count) : QStringLiteral("—"));
    p.setFont(monoFont(11));
    const QRectF rr = rateRect();
    p.setPen(Pal::text);
    p.drawText(m_stepped ? rr : rr.adjusted(0, 0, -36, 0), Qt::AlignRight | Qt::AlignVCenter,
               m_stepped ? L("Stepped", "단계 재생") : QString::number(m_target, 'f', 2));
    p.setPen(m_measured > 0 && m_measured < m_target * 0.97 ? Pal::accent : Pal::textFaint);
    if (!m_stepped) p.drawText(rr, Qt::AlignRight | Qt::AlignVCenter,
               m_measured > 0 ? QString::number(m_measured, 'f', 1) : QStringLiteral("fps"));

    // track
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x2a, 0x29, 0x27));
    p.drawRoundedRect(t, 2, 2);
    if (m_count <= 0) return;

    if (m_cache.size() == m_count) {
        p.setBrush(Pal::cached);
        qint64 run = -1;
        for (qint64 i = 0; i <= m_count; ++i) {
            const bool on = i < m_count && m_cache.testBit(i);
            if (on && run < 0) run = i;
            if (!on && run >= 0) {
                const double x0 = xFor(run - 0.5), x1 = xFor(i - 0.5);
                p.drawRect(QRectF(std::max(x0, t.left()), t.top(), std::min(x1, t.right()) - std::max(x0, t.left()), t.height()));
                run = -1;
            }
        }
    }

    if (m_loop && m_out >= m_in) {
        const double x0 = xFor(m_in), x1 = xFor(m_out);
        p.setBrush(Pal::accentSoft);
        p.drawRect(QRectF(x0, t.top() - 3, x1 - x0, t.height() + 6));
        p.setPen(QPen(Pal::accent, 1.5));
        p.drawLine(QPointF(x0, t.top() - 4), QPointF(x0, t.bottom() + 4));
        p.drawLine(QPointF(x1, t.top() - 4), QPointF(x1, t.bottom() + 4));
        p.setPen(Qt::NoPen);
    }

    // audio waveform under the track (max of the frames under each pixel column)
    if (m_wave.size() == m_count) {
        const QRectF band(t.left(), t.bottom() + 10, t.width(), 14);
        p.setPen(QPen(QColor(0x80, 0x7d, 0x75), 1));
        const double mid = band.center().y();
        for (int x = int(band.left()); x <= int(band.right()); ++x) {
            const qint64 f0 = frameAt(x - 0.5), f1 = std::max(f0, frameAt(x + 0.5));
            float peak = 0;
            for (qint64 f = f0; f <= f1 && f < m_count; ++f) peak = std::max(peak, m_wave[f]);
            if (peak <= 0.002f) continue;
            const double h = std::max(0.5, peak * band.height() / 2);
            p.drawLine(QPointF(x + 0.5, mid - h), QPointF(x + 0.5, mid + h));
        }
        p.setPen(Qt::NoPen);
    }

    // clip boundaries
    p.setPen(QPen(Pal::viewer, 2));
    for (qint64 s : m_segments)
        if (s > 0) p.drawLine(QPointF(xFor(s - 0.5), t.top()), QPointF(xFor(s - 0.5), t.bottom()));

    // annotation ticks under the track, bookmarks above
    p.setPen(Qt::NoPen);
    p.setBrush(Pal::review);
    for (qint64 f : m_annotated)
        if (f >= 0 && f < m_count) p.drawRect(QRectF(xFor(f) - 1, t.bottom() + 3, 2, 4));
    p.setBrush(Pal::text);
    for (qint64 f : m_bookmarks) {
        const double x = xFor(f);
        QPolygonF tri;
        tri << QPointF(x - 3.5, t.top() - 7) << QPointF(x + 3.5, t.top() - 7) << QPointF(x, t.top() - 2);
        p.drawPolygon(tri);
    }

    // playhead + frame number above it
    const double x = xFor(m_current);
    p.setBrush(Pal::textStrong);
    p.drawRoundedRect(QRectF(x - 1.5, t.top() - 4, 3, t.height() + 8), 1.5, 1.5);
    p.setFont(monoFont(12));
    p.setPen(std::binary_search(m_annotated.begin(), m_annotated.end(), m_current) ? Pal::reviewActive : Pal::textStrong);
    const QString label = QString::number(m_current);
    const QFontMetricsF fm(p.font());
    const double w = fm.horizontalAdvance(label);
    p.drawText(QPointF(std::clamp(x - w / 2, t.left() - 10, t.right() - w + 10), 13), label);
}

void Timeline::mousePressEvent(QMouseEvent* e)
{
    if (m_count > 0 && (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier)))) {
        m_dragging = m_relative = true; m_lastMouse = e->position(); m_scrubAcc = 0;
        setCursor(Qt::SizeHorCursor); return;
    }
    if (e->button() == Qt::RightButton && m_count > 0) {
        emit contextMenuAt(frameAt(e->position().x()), e->globalPosition().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (rateRect().contains(e->position())) {
        emit rateClicked();
        return;
    }
    if (m_count <= 0) return;
    m_dragging = true;
    emit scrubbed(frameAt(e->position().x()));
}

void Timeline::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging && m_relative) {
        m_scrubAcc += (e->position().x() - m_lastMouse.x()) / ((e->modifiers() & Qt::ShiftModifier) ? 18.0 : 6.0);
        m_lastMouse = e->position();
        const int frames = int(m_scrubAcc);
        m_scrubAcc -= frames;
        if (frames) emit scrubBy(frames);
        const QPoint global = mapToGlobal(e->position().toPoint());
        if (auto* screen = m_endlessScrubbing ? QGuiApplication::screenAt(global) : nullptr) {
            const QRect area = screen->geometry();
            QPoint next = global;
            if (global.x() <= area.left() + 3) next.setX(area.right() - 40);
            else if (global.x() >= area.right() - 3) next.setX(area.left() + 40);
            if (next != global) { QCursor::setPos(next); m_lastMouse = mapFromGlobal(next); }
        }
        return;
    }
    if (m_dragging) emit scrubbed(frameAt(e->position().x()));
}

void Timeline::mouseReleaseEvent(QMouseEvent*) { m_dragging = m_relative = false; unsetCursor(); }

// ================================================================ ClipBar

ClipBar::ClipBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ClipBar");
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(48);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(L("Red ticks: annotation / edit / tracking keys. Mint frame number: on a marked key.\nRight-click for range review and playback options.",
                 "빨간 눈금: 주석 / 편집 / 트래킹 키 위치. 연한 초록 프레임 숫자: 표시된 키 위.\n우클릭: 구간 반복 및 재생 옵션."));
}

void ClipBar::setClip(const QString& name, qint64 count, qint64 local, qint64 offset)
{
    if (name == m_name && count == m_count && local == m_local && offset == m_offset) return;
    m_name = name;
    m_count = count;
    m_local = local;
    m_offset = offset;
    update();
}

void ClipBar::setReviewMarks(QVector<qint64> keys, QVector<qint64> bookmarks)
{
    m_keys = std::move(keys);
    std::sort(m_keys.begin(), m_keys.end());
    m_keys.erase(std::unique(m_keys.begin(), m_keys.end()), m_keys.end());
    m_bookmarks = std::move(bookmarks);
    update();
}

QRectF ClipBar::lineRect() const { return QRectF(52, 30, std::max(1, width() - 104), 1); }

qint64 ClipBar::frameAt(double x) const
{
    if (m_count <= 1) return 0;
    const QRectF l = lineRect();
    return std::clamp<qint64>(qRound64((x - l.left()) / l.width() * (m_count - 1)), 0, m_count - 1);
}

void ClipBar::paintEvent(QPaintEvent*)
{
    if (m_count <= 0) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF l = lineRect();

    p.setPen(QPen(Pal::border, 1));
    p.drawLine(l.topLeft(), l.topRight());
    p.drawLine(QPointF(l.left(), l.top() - 5), QPointF(l.left(), l.top() + 5));
    p.drawLine(QPointF(l.right(), l.top() - 5), QPointF(l.right(), l.top() + 5));

    const auto xFor = [&](double local) { return l.left() + l.width() * local / std::max<qint64>(1, m_count - 1); };
    // Mark only the explicit key position; held annotations do not fill the bar.
    p.setPen(Qt::NoPen);
    p.setBrush(Pal::review);
    for (qint64 key : m_keys)
        if (key >= m_offset && key < m_offset + m_count)
            p.drawRect(QRectF(xFor(key - m_offset) - 1.5, l.top(), 3, 10));
    p.setBrush(Pal::textDim);
    for (qint64 f : m_bookmarks)
        if (f >= m_offset && f < m_offset + m_count) {
            const double x = xFor(f - m_offset);
            p.drawPolygon(QPolygonF{QPointF(x - 3, l.top() - 10), QPointF(x + 3, l.top() - 10), QPointF(x, l.top() - 6)});
        }

    p.setFont(monoFont(10));
    p.setPen(Pal::textFaint);
    p.drawText(QRectF(0, l.top() - 8, l.left() - 8, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("0"));
    p.drawText(QRectF(l.right() + 8, l.top() - 8, 50, 16), Qt::AlignLeft | Qt::AlignVCenter, QString::number(m_count - 1));

    const double x = m_count <= 1 ? l.left() : l.left() + l.width() * (m_local / double(m_count - 1));
    p.setPen(Qt::NoPen);
    p.setBrush(Pal::text);
    p.drawRoundedRect(QRectF(x - 1.5, l.top() - 6, 3, 12), 1.5, 1.5);

    p.setFont(monoFont(11));
    p.setPen(std::binary_search(m_keys.begin(), m_keys.end(), m_offset + m_local) ? Pal::reviewActive : Pal::text);
    const QString label = QString::number(m_local);
    const double w = QFontMetricsF(p.font()).horizontalAdvance(label);
    p.drawText(QPointF(std::clamp(x - w / 2, l.left() - 8, l.right() - w + 8), 13), label);

    QFont nf = font();
    nf.setPixelSize(11);
    p.setFont(nf);
    p.setPen(Pal::textFaint);
    const double nameLeft = std::max(l.center().x(), x + w / 2 + 16);
    const double nameWidth = std::max(0.0, l.right() - nameLeft);
    const QString name = QFontMetrics(nf).elidedText(m_name, Qt::ElideMiddle, int(nameWidth));
    p.drawText(QRectF(nameLeft, 2, nameWidth, 14), Qt::AlignRight | Qt::AlignVCenter, name);
}

void ClipBar::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::RightButton && m_count > 0) {
        emit contextMenuAt(frameAt(e->position().x()), e->globalPosition().toPoint());
        return;
    }
    if (e->button() == Qt::LeftButton && m_count > 0) emit scrubbed(frameAt(e->position().x()));
}

void ClipBar::mouseMoveEvent(QMouseEvent* e)
{
    if ((e->buttons() & Qt::LeftButton) && m_count > 0) emit scrubbed(frameAt(e->position().x()));
}
