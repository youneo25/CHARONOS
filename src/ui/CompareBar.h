#pragma once

#include "ui/Viewport.h"

#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QSpinBox;
class QToolButton;
class QBoxLayout;

// The compare controls, shown above the viewer while comparing: pick A and B from lists
// (no hunting for small A / B buttons), swap them, choose how they are laid out, line B up
// with A (sync by time or frame, plus a frame offset), and pick which side is annotated and
// which side is heard. Everything also stays on the keyboard (Shift+0..4, Tab, Shift+Tab).
class CompareBar : public QWidget {
    Q_OBJECT
public:
    struct SourceItem {
        QString id;
        QString name;
        bool available = true;  // media is open
    };

    explicit CompareBar(QWidget* parent = nullptr);

    void setSources(const QVector<SourceItem>& items, const QString& a, const QString& b);
    void setMode(CompareMode mode);
    void setSyncByTime(bool byTime);
    void setOffset(int frames);
    void setEditSide(int side);  // 0 = A, 1 = B
    void setAudioSource(int mode);  // 0 = auto, 1 = A, 2 = B
    // Two rows on a narrow screen (phone), one row otherwise.
    void setCompact(bool compact);

protected:
    void resizeEvent(QResizeEvent* e) override;

signals:
    void sourceAChosen(const QString& id);
    void sourceBChosen(const QString& id);
    void swapRequested();
    void modeChosen(CompareMode mode);
    void syncByTimeChanged(bool byTime);
    void offsetChanged(int frames);
    void editSideChosen(int side);
    void audioSourceChosen(int mode);
    void closeRequested();

private:
    QToolButton* segment(const QString& icon, const QString& text, const QString& tip);

    QComboBox* m_a = nullptr;
    QComboBox* m_b = nullptr;
    QButtonGroup* m_modes = nullptr;
    QButtonGroup* m_edit = nullptr;
    QComboBox* m_sync = nullptr;
    QComboBox* m_audio = nullptr;
    QSpinBox* m_offset = nullptr;
    QBoxLayout* m_rows = nullptr;
    QWidget* m_second = nullptr;
    bool m_filling = false;
    bool m_compact = false;
    bool m_iconsOnly = false;
};
