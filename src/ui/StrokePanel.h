#pragma once

#include <QFrame>

class QDoubleSpinBox;
class QSlider;

// Popup with free values for line width, opacity and text size, plus quick presets.
class StrokePanel : public QFrame {
    Q_OBJECT
public:
    explicit StrokePanel(QWidget* parent = nullptr);

    void setValues(double width, double opacity, double textSize);
    void popupBelow(QWidget* anchor);
    void popupAt(QPoint global);

signals:
    void widthChanged(double px);
    void opacityChanged(double opacity01);
    void textSizeChanged(double px);
    void closed();

protected:
    void hideEvent(QHideEvent* e) override;

private:
    struct Row {
        QSlider* slider;
        QDoubleSpinBox* spin;
    };
    Row addRow(class QGridLayout* grid, int row, const QString& label, double min, double max, double step,
               const QString& suffix, bool logScale);

    Row m_width{};
    Row m_opacity{};
    Row m_text{};
};
