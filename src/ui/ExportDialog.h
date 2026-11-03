#pragma once

#include "export/Exporter.h"

#include <QDialog>
#include <QImage>
#include <QJsonObject>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// Export window laid out like an editing program's render page: presets on the left, the
// settings in Video / Audio / Overlays tabs in the middle, a preview and a live summary
// (codec, size, duration, estimated file size) on the right, and the destination at the bottom.
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
        QSize size;          // picture size at 100% (canvas / source); empty = unknown
        bool hasAudio = true;
        QImage preview;      // the current viewer picture, shown in the summary
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

    // Index into the format table (presets store it): 0 MP4 H.264, 1 MP4 H.264 NVENC,
    // 2 MP4 H.265 NVENC, 3 MOV ProRes 422 HQ, 4 PNG, 5 JPG, 6 TIFF sequence.
    int formatIndex() const;
    void setFormatIndex(int index);

private:
    friend class FeatureRegression;
    QJsonObject presetValues() const;
    void applyPreset(const QJsonObject& values);
    void refreshPresets();
    void refreshCodecs();
    void updateOutputSuffix();
    void browse();
    void updateSummary();
    QWidget* buildVideoTab();
    QWidget* buildAudioTab();
    QWidget* buildOverlaysTab();
    QWidget* buildSummary();
    QWidget* buildPresets();

    Ranges m_ranges;
    QComboBox* m_container = nullptr;  // MP4 / MOV / image sequence
    QComboBox* m_codec = nullptr;      // codecs of that container
    QButtonGroup* m_rangeGroup = nullptr;
    QComboBox* m_scale = nullptr;
    QComboBox* m_margin = nullptr;
    QListWidget* m_presets = nullptr;
    QPushButton* m_deletePreset = nullptr;
    QDoubleSpinBox* m_fps = nullptr;
    QCheckBox* m_notes = nullptr;
    QCheckBox* m_caption = nullptr;
    QCheckBox* m_tracks = nullptr;
    QCheckBox* m_audio = nullptr;
    QCheckBox* m_compare = nullptr;
    QCheckBox* m_filter = nullptr;
    QLineEdit* m_name = nullptr;
    QLineEdit* m_folder = nullptr;
    QLabel* m_sizeLabel = nullptr;
    QLabel* m_rangeLabel = nullptr;
    QLabel* m_audioInfo = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_estimate = nullptr;
    bool m_applying = false;
};
