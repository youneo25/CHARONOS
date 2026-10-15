#include "StrokePanel.h"
#include "Lang.h"

#include "Palette.h"

#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QGridLayout>
#include <QLabel>
#include <QScreen>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {
// Sliders run 0..1000; width and text size use a log mapping so small values get room.
int toSlider(double v, double min, double max, bool log)
{
    const double t = log ? std::log(v / min) / std::log(max / min) : (v - min) / (max - min);
    return int(std::round(std::clamp(t, 0.0, 1.0) * 1000));
}
double fromSlider(int s, double min, double max, bool log)
{
    const double t = s / 1000.0;
    return log ? min * std::pow(max / min, t) : min + (max - min) * t;
}
} // namespace

StrokePanel::StrokePanel(QWidget* parent)
    : QFrame(parent, Qt::Popup)
{
    setObjectName("StrokePanel");
    setStyleSheet(QStringLiteral("#StrokePanel { background: %1; border: 1px solid %2; border-radius: 10px; }")
                      .arg(Pal::panel.name(), Pal::border.name()));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(14, 12, 14, 12);
    v->setSpacing(10);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    m_width = addRow(grid, 0, L("Width", "굵기"), 0.5, 200, 0.5, QStringLiteral(" px"), true);
    m_opacity = addRow(grid, 1, L("Opacity", "불투명도"), 1, 100, 1, QStringLiteral(" %"), false);
    m_text = addRow(grid, 2, L("Text Size", "글자 크기"), 6, 600, 1, QStringLiteral(" px"), true);
    v->addLayout(grid);

    auto* presets = new QHBoxLayout;
    presets->setSpacing(4);
    auto* hint = new QLabel(L("Width", "굵기"));
    hint->setObjectName("FormKey");
    presets->addWidget(hint);
    for (double w : { 1.0, 2.0, 3.0, 5.0, 8.0, 12.0, 20.0 }) {
        auto* b = new QToolButton;
        b->setText(QString::number(w));
        b->setFixedSize(30, 24);
        b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(QStringLiteral("QToolButton { background: %1; border-radius: 6px; } QToolButton:hover { background: %2; }")
                             .arg(Pal::raised.name(), Pal::hover.name()));
        connect(b, &QToolButton::clicked, this, [this, w] { m_width.spin->setValue(w); });
        presets->addWidget(b);
    }
    presets->addStretch(1);
    v->addLayout(presets);
    setFixedWidth(340);
}

StrokePanel::Row StrokePanel::addRow(QGridLayout* grid, int row, const QString& label, double min, double max, double step,
                                     const QString& suffix, bool logScale)
{
    auto* l = new QLabel(label);
    l->setObjectName("FormKey");
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 1000);
    slider->setFocusPolicy(Qt::NoFocus);
    auto* spin = new QDoubleSpinBox;
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(step < 1 ? 1 : 0);
    spin->setSuffix(suffix);
    spin->setFixedWidth(84);
    spin->setKeyboardTracking(false);
    grid->addWidget(l, row, 0);
    grid->addWidget(slider, row, 1);
    grid->addWidget(spin, row, 2);

    connect(slider, &QSlider::valueChanged, spin, [=](int s) {
        if (slider->isSliderDown()) spin->setValue(fromSlider(s, min, max, logScale));
    });
    connect(spin, &QDoubleSpinBox::valueChanged, this, [=](double val) {
        QSignalBlocker block(slider);
        slider->setValue(toSlider(val, min, max, logScale));
        if (spin == m_width.spin) emit widthChanged(val);
        else if (spin == m_opacity.spin) emit opacityChanged(val / 100.0);
        else emit textSizeChanged(val);
    });
    return { slider, spin };
}

void StrokePanel::setValues(double width, double opacity, double textSize)
{
    for (auto [row, v] : { std::pair{ m_width, width }, std::pair{ m_opacity, opacity * 100 }, std::pair{ m_text, textSize } }) {
        QSignalBlocker b1(row.spin), b2(row.slider);
        row.spin->setValue(v);
    }
    m_width.slider->setValue(toSlider(width, 0.5, 200, true));
    m_opacity.slider->setValue(toSlider(opacity * 100, 1, 100, false));
    m_text.slider->setValue(toSlider(textSize, 6, 600, true));
}

void StrokePanel::popupBelow(QWidget* anchor)
{
    adjustSize();
    QPoint p = anchor->mapToGlobal(QPoint(anchor->width() + 6, 0));
    const QRect screen = anchor->screen()->availableGeometry();
    p.setY(std::min(p.y(), screen.bottom() - height() - 8));
    move(p);
    show();
}

void StrokePanel::popupAt(QPoint global)
{
    adjustSize();
    QScreen* screen = QGuiApplication::screenAt(global);
    const QRect area = screen ? screen->availableGeometry() : QRect(global, size());
    move(std::clamp(global.x(), area.left(), area.right() - width()), std::clamp(global.y(), area.top(), area.bottom() - height()));
    show();
}

void StrokePanel::hideEvent(QHideEvent* e)
{
    QFrame::hideEvent(e);
    emit closed();
}
