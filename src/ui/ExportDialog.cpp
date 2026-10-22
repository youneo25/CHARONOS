#include "ExportDialog.h"
#include "Lang.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QInputDialog>
#include <QMessageBox>
#include <QJsonDocument>
#include <QDate>
#include <QVBoxLayout>

namespace {

struct FormatInfo {
    ExportJob::Format format;
    const char* label;
    const char* labelKo;
    const char* suffix;  // empty for sequences
};

const FormatInfo kFormats[] = {
    { ExportJob::Format::Mp4H264, "MP4 · H.264 (CPU)", "MP4 · H.264 (CPU)", "mp4" },
    { ExportJob::Format::Mp4Nvenc, "MP4 · H.264 (NVIDIA GPU)", "MP4 · H.264 (NVIDIA GPU)", "mp4" },
    { ExportJob::Format::Mp4HevcNvenc, "MP4 · H.265 (NVIDIA GPU)", "MP4 · H.265 (NVIDIA GPU)", "mp4" },
    { ExportJob::Format::MovProRes, "MOV · ProRes 422 HQ", "MOV · ProRes 422 HQ", "mov" },
    { ExportJob::Format::PngSequence, "PNG Sequence", "PNG 시퀀스", "" },
    { ExportJob::Format::JpgSequence, "JPG Sequence", "JPG 시퀀스", "" },
    { ExportJob::Format::TiffSequence, "TIFF Sequence", "TIFF 시퀀스", "" },
};

QLabel* key(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName("FormKey");
    return l;
}

} // namespace

ExportDialog::ExportDialog(const Ranges& ranges, QWidget* parent)
    : QDialog(parent)
    , m_ranges(ranges)
{
    setWindowTitle(L("Export", "내보내기"));
    setMinimumWidth(460);
    QSettings settings;

    m_format = new QComboBox;
    for (const FormatInfo& f : kFormats) m_format->addItem(L(f.label, f.labelKo));
    m_format->setCurrentIndex(settings.value("export/format", 0).toInt());

    m_range = new QComboBox;
    m_range->addItem(L("All  (0 – %1)", "전체  (0 – %1)").arg(ranges.total - 1));
    m_range->addItem(L("Current Clip  %1  (%2 – %3)", "현재 클립  %1  (%2 – %3)").arg(ranges.clipName).arg(ranges.clipStart).arg(ranges.clipEnd));
    if (ranges.hasLoop) {
        m_range->addItem(QStringLiteral("In / Out  (%1 – %2)").arg(ranges.loopIn).arg(ranges.loopOut));
        m_range->setCurrentIndex(2);
    }

    m_fps = new QDoubleSpinBox;
    m_fps->setRange(1, 240);
    m_fps->setDecimals(3);
    m_fps->setValue(ranges.fps);

    m_scale = new QComboBox;
    m_scale->addItems({ "100%", "75%", "50%", "25%" });
    m_margin = new QComboBox;
    m_margin->addItems({"0%", "10%", "25%", "50%", "100%"});
    m_margin->setToolTip(L("Extra canvas around the video for annotations outside its edges", "영상 바깥의 주석을 내보내기 위한 추가 캔버스 여백"));

    m_notes = new QCheckBox(L("Annotations", "주석"));
    m_notes->setChecked(settings.value("export/notes", true).toBool());
    m_caption = new QCheckBox(L("Clip Name and Frame", "클립 이름과 프레임"));
    m_caption->setChecked(settings.value("export/caption", false).toBool());

    m_tracks = new QCheckBox(L("Motion Trails", "트래킹 커브"));
    m_tracks->setChecked(settings.value("export/tracks", true).toBool());

    m_audio = new QCheckBox(L("Audio", "오디오"));
    m_audio->setChecked(settings.value("export/audio", true).toBool());

    m_compare = new QCheckBox(L("A / B Comparison", "A / B 비교 화면"));
    m_compare->setChecked(ranges.comparing);
    m_compare->setVisible(ranges.comparing);

    m_path = new QLineEdit;
    const QString dir = settings.value("export/dir", "F:/Codex/outputs/" + QDate::currentDate().toString("yyyy-MM-dd") + "/charonos-exports").toString();
    m_path->setText(dir + "/" + ranges.suggestedName);
    auto* browseBtn = new QPushButton(L("Browse", "찾아보기"));
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::browse);
    auto* pathRow = new QHBoxLayout;
    pathRow->setSpacing(6);
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browseBtn);

    m_summary = new QLabel;
    m_summary->setObjectName("Muted");
    m_summary->setWordWrap(true);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(10);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_presets = new QComboBox;
    m_presets->setObjectName("ExportPresets");
    refreshPresets();
    auto* savePreset = new QPushButton(L("Save...", "저장..."));
    savePreset->setObjectName("SaveExportPreset");
    auto* removePreset = new QPushButton(L("Delete", "삭제"));
    connect(m_presets, &QComboBox::activated, this, [this](int index) {
        const auto values = m_presets->itemData(index).toJsonObject();
        if (!values.isEmpty()) applyPreset(values);
    });
    connect(savePreset, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, L("Save Export Preset", "내보내기 프리셋 저장"), L("Preset name", "프리셋 이름"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        QSettings settings;
        auto custom = QJsonDocument::fromJson(settings.value("export/presets").toByteArray()).object();
        if (custom.contains(name) && QMessageBox::question(this, L("Replace Preset", "프리셋 변경"), L("Replace %1?", "%1 프리셋을 변경할까요?").arg(name)) != QMessageBox::Yes) return;
        custom[name] = presetValues();
        settings.setValue("export/presets", QJsonDocument(custom).toJson(QJsonDocument::Compact));
        refreshPresets();
        for (int i = 0; i < m_presets->count(); ++i)
            if (m_presets->itemData(i, Qt::UserRole + 1).toString() == name) m_presets->setCurrentIndex(i);
    });
    connect(removePreset, &QPushButton::clicked, this, [this] {
        const QString name = m_presets->currentData(Qt::UserRole + 1).toString();
        if (name.isEmpty()) return;
        QSettings settings;
        auto custom = QJsonDocument::fromJson(settings.value("export/presets").toByteArray()).object();
        custom.remove(name); settings.setValue("export/presets", QJsonDocument(custom).toJson(QJsonDocument::Compact));
        refreshPresets();
    });
    connect(m_presets, &QComboBox::currentIndexChanged, removePreset, [this, removePreset] { removePreset->setEnabled(!m_presets->currentData(Qt::UserRole + 1).toString().isEmpty()); });
    removePreset->setEnabled(false);
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(m_presets, 1); presetRow->addWidget(savePreset); presetRow->addWidget(removePreset);
    form->addRow(key(L("Preset", "프리셋")), presetRow);
    form->addRow(key(L("Format", "형식")), m_format);
    form->addRow(key(L("Range", "범위")), m_range);
    form->addRow(key(QStringLiteral("FPS")), m_fps);
    form->addRow(key(L("Size", "크기")), m_scale);
    form->addRow(key(L("Canvas Border", "캔버스 여백")), m_margin);
    form->addRow(QString(), m_notes);
    form->addRow(QString(), m_caption);
    form->addRow(QString(), m_tracks);
    m_filter = new QCheckBox(L("Viewer Filter: %1", "뷰어 필터: %1").arg(m_ranges.filterName));
    m_filter->setObjectName("ExportFilter");
    m_filter->setChecked(!m_ranges.filterName.isEmpty());
    m_filter->setVisible(!m_ranges.filterName.isEmpty());
    form->addRow(QString(), m_filter);
    form->addRow(QString(), m_audio);
    if (ranges.comparing) form->addRow(QString(), m_compare);
    form->addRow(key(L("Save To", "저장 위치")), pathRow);

    auto* cancel = new QPushButton(L("Cancel", "취소"));
    auto* ok = new QPushButton(L("Export", "내보내기"));
    ok->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, [this] {
        if (output().isEmpty()) { QMessageBox::warning(this, L("Export", "내보내기"), L("Choose an output path.", "저장 위치를 선택하세요.")); return; }
        QSettings s;
        s.setValue("export/format", m_format->currentIndex());
        s.setValue("export/notes", m_notes->isChecked());
        s.setValue("export/caption", m_caption->isChecked());
        s.setValue("export/tracks", m_tracks->isChecked());
        s.setValue("export/audio", m_audio->isChecked());
        s.setValue("export/dir", QFileInfo(output()).absolutePath());
        accept();
    });
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 20, 22, 18);
    v->setSpacing(14);
    v->addLayout(form);
    v->addWidget(m_summary);
    v->addLayout(buttons);

    connect(m_format, &QComboBox::currentIndexChanged, this, [this] {
        updateOutputSuffix();
        updateSummary();
    });
    connect(m_range, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    connect(m_scale, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    connect(m_fps, &QDoubleSpinBox::valueChanged, this, &ExportDialog::updateSummary);
    connect(m_margin, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    updateOutputSuffix();
    updateSummary();
}

ExportJob::Format ExportDialog::format() const { return kFormats[m_format->currentIndex()].format; }
QString ExportDialog::output() const { return m_path->text().trimmed(); }
double ExportDialog::fps() const { return m_fps->value(); }
double ExportDialog::scale() const { return QList<double>{ 1.0, 0.75, 0.5, 0.25 }.value(m_scale->currentIndex(), 1.0); }
bool ExportDialog::notes() const { return m_notes->isChecked(); }
bool ExportDialog::caption() const { return m_caption->isChecked(); }
bool ExportDialog::tracks() const { return m_tracks->isChecked(); }
bool ExportDialog::audio() const { return m_audio->isChecked() && m_audio->isEnabled(); }
bool ExportDialog::comparison() const { return m_ranges.comparing && m_compare->isChecked(); }
bool ExportDialog::filter() const { return m_filter && !m_ranges.filterName.isEmpty() && m_filter->isChecked(); }
double ExportDialog::margin() const { return QList<double>{0, .10, .25, .50, 1}.value(m_margin->currentIndex()); }

QJsonObject ExportDialog::presetValues() const
{
    return {{"format", m_format->currentIndex()}, {"range", m_range->currentIndex()}, {"fps", fps()},
            {"scale", m_scale->currentIndex()}, {"margin", m_margin->currentIndex()},
            {"notes", notes()}, {"caption", caption()}, {"tracks", tracks()}, {"audio", m_audio->isChecked()}, {"compare", m_compare->isChecked()}};
}

void ExportDialog::applyPreset(const QJsonObject& v)
{
    m_format->setCurrentIndex(std::clamp(v.value("format").toInt(), 0, m_format->count() - 1));
    m_range->setCurrentIndex(std::clamp(v.value("range").toInt(), 0, m_range->count() - 1));
    m_fps->setValue(v.value("fps").toDouble(m_ranges.fps));
    m_scale->setCurrentIndex(std::clamp(v.value("scale").toInt(), 0, m_scale->count() - 1));
    m_margin->setCurrentIndex(std::clamp(v.value("margin").toInt(), 0, m_margin->count() - 1));
    m_notes->setChecked(v.value("notes").toBool(true)); m_caption->setChecked(v.value("caption").toBool());
    m_tracks->setChecked(v.value("tracks").toBool(true)); m_audio->setChecked(v.value("audio").toBool(true));
    m_compare->setChecked(v.value("compare").toBool(m_ranges.comparing));
    updateOutputSuffix(); updateSummary();
}

void ExportDialog::refreshPresets()
{
    m_presets->clear();
    m_presets->addItem(L("Custom settings", "사용자 설정"));
    m_presets->addItem(L("Review MP4", "리뷰 MP4"), QJsonObject{{"format",0},{"notes",true},{"audio",true}});
    m_presets->addItem(L("Compact MP4 (50%)", "작은 MP4 (50%)"), QJsonObject{{"format",0},{"scale",2},{"notes",true},{"audio",true}});
    m_presets->addItem(L("ProRes Master", "ProRes 마스터"), QJsonObject{{"format",3},{"notes",true},{"audio",true}});
    m_presets->addItem(L("2D PNG Sequence", "2D PNG 시퀀스"), QJsonObject{{"format",4},{"notes",true},{"tracks",false},{"audio",false}});
    const auto custom = QJsonDocument::fromJson(QSettings().value("export/presets").toByteArray()).object();
    for (auto it = custom.begin(); it != custom.end(); ++it) {
        m_presets->addItem(it.key(), it.value().toObject());
        m_presets->setItemData(m_presets->count() - 1, it.key(), Qt::UserRole + 1);
    }
}

std::pair<int64_t, int64_t> ExportDialog::range() const
{
    switch (m_range->currentIndex()) {
    case 1: return { m_ranges.clipStart, m_ranges.clipEnd };
    case 2: return { m_ranges.loopIn, m_ranges.loopOut };
    default: return { 0, m_ranges.total - 1 };
    }
}

void ExportDialog::updateOutputSuffix()
{
    const QString suffix = QString::fromLatin1(kFormats[m_format->currentIndex()].suffix);
    QFileInfo fi(m_path->text());
    QString base = fi.path() + "/" + fi.completeBaseName();
    if (fi.suffix().isEmpty() || fi.suffix() == "mp4" || fi.suffix() == "mov") {
        m_path->setText(suffix.isEmpty() ? base : base + "." + suffix);
    }
}

void ExportDialog::browse()
{
    const QString suffix = QString::fromLatin1(kFormats[m_format->currentIndex()].suffix);
    QString path;
    if (suffix.isEmpty()) {
        const QString dir = QFileDialog::getExistingDirectory(this, L("Sequence Folder", "시퀀스 폴더"),
                                                              QFileInfo(output()).absolutePath());
        if (dir.isEmpty()) return;
        path = dir + "/" + QFileInfo(output()).completeBaseName();
    } else {
        path = QFileDialog::getSaveFileName(this, L("Export", "내보내기"), output(),
                                            QStringLiteral("%1 (*.%2)").arg(suffix.toUpper(), suffix));
        if (path.isEmpty()) return;
    }
    m_path->setText(path);
}

void ExportDialog::updateSummary()
{
    m_audio->setEnabled(!QString::fromLatin1(kFormats[m_format->currentIndex()].suffix).isEmpty());  // sequences have no sound
    const auto [a, b] = range();
    const int64_t n = b - a + 1;
    const bool seq = QString::fromLatin1(kFormats[m_format->currentIndex()].suffix).isEmpty();
    QString text = L("%1 frames · %2 s (%3 fps)", "%1프레임 · %2초 (%3 fps)")
                       .arg(n)
                       .arg(n / std::max(1.0, m_fps->value()), 0, 'f', 2)
                       .arg(m_fps->value(), 0, 'f', 3);
    if (seq) text += L("  name_0000.ext", "  이름_0000.확장자");
    if (margin() > 0) text += L(" · %1% border on each edge", " · 각 가장자리 %1% 여백").arg(qRound(margin() * 100));
    m_summary->setText(text);
}
