#pragma once

#include "export/Exporter.h"

#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    struct Ranges {
        int64_t total = 0;
        bool hasLoop = false;
        int64_t loopIn = 0, loopOut = 0;
        int64_t clipStart = 0, clipEnd = 0;
        QString clipName;
        double fps = 24.0;
        QString suggestedName;
        bool comparing = false;
        QString filterName;  // the viewer's analysis filter, empty = none
    };

    ExportDialog(const Ranges& ranges, QWidget* parent = nullptr);

    ExportJob::Format format() const;
    QString output() const;
    std::pair<int64_t, int64_t> range() const;
    double fps() const;
    double scale() const;
    bool notes() const;
    bool caption() const;
    bool tracks() const;
    bool audio() const;
    bool comparison() const;
    bool filter() const;  // burn in the viewer's analysis filter
    double margin() const;

private:
    friend class FeatureRegression;
    QJsonObject presetValues() const;
    void applyPreset(const QJsonObject& values);
    void refreshPresets();
    void updateOutputSuffix();
    void browse();
    void updateSummary();

    Ranges m_ranges;
    QComboBox* m_format;
    QComboBox* m_range;
    QComboBox* m_scale;
    QComboBox* m_margin;
    QComboBox* m_presets;
    QDoubleSpinBox* m_fps;
    QCheckBox* m_notes;
    QCheckBox* m_caption;
    QCheckBox* m_tracks;
    QCheckBox* m_audio;
    QCheckBox* m_compare;
    QCheckBox* m_filter = nullptr;
    QLineEdit* m_path;
    QLabel* m_summary;
};
