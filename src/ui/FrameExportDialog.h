#pragma once
#include "export/Exporter.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

class FrameExportDialog : public QDialog {
public:
    FrameExportDialog(const QImage& frame, const Source& source, int localFrame, QWidget* parent = nullptr, int trackRange = 0);
    StillFrameOptions options() const;
    QByteArray format() const;
    int quality() const;
    void saveSettings() const;
private:
    void refresh();
    const QImage& m_frame;
    const Source& m_source;
    int m_local;
    int m_trackRange;
    QComboBox *m_format, *m_background;
    QDoubleSpinBox* m_scale;
    QSpinBox *m_padding, *m_quality;
    QCheckBox *m_outside, *m_notes, *m_tracks, *m_video, *m_caption;
    QLabel* m_size;
    class QDialogButtonBox* m_buttons;
};
