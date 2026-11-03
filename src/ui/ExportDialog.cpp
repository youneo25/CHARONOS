#include "ExportDialog.h"
#include "Lang.h"
#include "platform/Platform.h"

#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSettings>
#include <QTabWidget>

namespace {

enum Container { Mp4 = 0, Mov = 1, ImageSeq = 2 };

struct FormatInfo {
    ExportJob::Format format;
    Container container;
    const char* codec;
    const char* codecKo;
    const char* hint;
    const char* hintKo;
    const char* suffix;  // empty for sequences
    double mbps;         // rough video bit rate at 1080p24 (sequences: MB per 1080p frame)
    bool nvidia;
};

// Order = the "format" index stored in presets and settings (kept from 1.x).
const FormatInfo kFormats[] = {
    { ExportJob::Format::Mp4H264, Mp4, "H.264 (CPU)", "H.264 (CPU)",
      "Plays everywhere. High quality (CRF 16), 1 s keyframe interval for smooth scrubbing.",
      "어디서나 재생됩니다. 고화질(CRF 16), 스크럽이 부드럽도록 1초마다 키프레임.", "mp4", 16.0, false },
    { ExportJob::Format::Mp4Nvenc, Mp4, "H.264 (NVIDIA NVENC)", "H.264 (NVIDIA NVENC)",
      "Encoded on the GPU: much faster, slightly larger files. Needs an NVIDIA graphics card.",
      "GPU 인코딩: 훨씬 빠르고 파일이 조금 큽니다. NVIDIA 그래픽 카드가 필요합니다.", "mp4", 20.0, true },
    { ExportJob::Format::Mp4HevcNvenc, Mp4, "H.265 / HEVC (NVIDIA NVENC)", "H.265 / HEVC (NVIDIA NVENC)",
      "About half the size of H.264 at the same quality. Needs an NVIDIA graphics card.",
      "같은 화질에서 H.264의 약 절반 크기. NVIDIA 그래픽 카드가 필요합니다.", "mp4", 11.0, true },
    { ExportJob::Format::MovProRes, Mov, "Apple ProRes 422 HQ", "Apple ProRes 422 HQ",
      "Edit-friendly master: 10-bit 4:2:2, every frame a keyframe, uncompressed PCM audio. Large files.",
      "편집용 마스터: 10비트 4:2:2, 모든 프레임이 키프레임, 무압축 PCM 오디오. 파일이 큽니다.", "mov", 176.0, false },
    { ExportJob::Format::PngSequence, ImageSeq, "PNG", "PNG",
      "Lossless stills, one file per frame (name_0000.png).", "무손실 이미지, 프레임마다 한 파일 (이름_0000.png).", "", 3.0, false },
    { ExportJob::Format::JpgSequence, ImageSeq, "JPEG (quality 95)", "JPEG (품질 95)",
      "Small stills for sharing, one file per frame.", "공유하기 좋은 작은 이미지, 프레임마다 한 파일.", "", 0.6, false },
    { ExportJob::Format::TiffSequence, ImageSeq, "TIFF (uncompressed)", "TIFF (무압축)",
      "Uncompressed stills for compositing and print.", "합성과 인쇄용 무압축 이미지.", "", 8.0, false },
};
constexpr int kFormatCount = int(sizeof kFormats / sizeof kFormats[0]);

bool formatAvailable(int i) { return !(Platform::isAndroid() && kFormats[i].nvidia); }

QLabel* key(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName("FormKey");
    return l;
}

QLabel* sectionTitle(const QString& text)
{
    auto* l = new QLabel(text.toUpper());
    l->setObjectName("SectionTitle");
    return l;
}

QWidget* section(const QString& title, QLayout* content)
{
    auto* box = new QWidget;
    box->setObjectName("Section");
    box->setAttribute(Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(14, 10, 14, 12);
    v->setSpacing(8);
    v->addWidget(sectionTitle(title));
    v->addLayout(content);
    return box;
}

QString timecode(int64_t frames, double fps)
{
    const int f = std::max(1, int(std::round(fps)));
    const int64_t secs = frames / f;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(secs / 3600, 2, 10, QChar('0'))
        .arg((secs / 60) % 60, 2, 10, QChar('0'))
        .arg(secs % 60, 2, 10, QChar('0'))
        .arg(int(frames % f), 2, 10, QChar('0'));
}

QString formatSize(double bytes)
{
    if (bytes >= double(qint64(1) << 30)) return QStringLiteral("%1 GB").arg(bytes / double(qint64(1) << 30), 0, 'f', 2);
    if (bytes >= double(1 << 20)) return QStringLiteral("%1 MB").arg(bytes / double(1 << 20), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(std::max(1.0, bytes / 1024.0), 0, 'f', 0);
}

QString defaultFolder()
{
    const QString saved = QSettings().value("export/dir").toString();
    // 1.x shipped with a developer folder as the default; never offer that on other machines.
    if (saved.isEmpty() || saved.startsWith(QLatin1String("F:/Codex"), Qt::CaseInsensitive)) return Platform::exportDir();
    return saved;
}

} // namespace

ExportDialog::ExportDialog(const Ranges& ranges, QWidget* parent)
    : QDialog(parent)
    , m_ranges(ranges)
{
    setObjectName("ExportDialog");
    setWindowTitle(L("Export", "내보내기"));
    QSettings settings;

    m_notes = new QCheckBox(L("Annotations", "주석"));
    m_notes->setChecked(settings.value("export/notes", true).toBool());
    m_caption = new QCheckBox(L("Clip name and frame number", "클립 이름과 프레임 번호"));
    m_caption->setChecked(settings.value("export/caption", false).toBool());
    m_tracks = new QCheckBox(L("Motion trails", "트래킹 커브"));
    m_tracks->setChecked(settings.value("export/tracks", true).toBool());
    m_audio = new QCheckBox(L("Export audio", "오디오 내보내기"));
    m_audio->setChecked(settings.value("export/audio", true).toBool());
    m_compare = new QCheckBox(L("A / B comparison layout", "A / B 비교 화면"));
    m_compare->setChecked(ranges.comparing);
    m_filter = new QCheckBox(L("Viewer filter: %1", "뷰어 필터: %1").arg(m_ranges.filterName));
    m_filter->setObjectName("ExportFilter");
    m_filter->setChecked(!m_ranges.filterName.isEmpty());

    auto* tabs = new QTabWidget;
    tabs->setObjectName("ExportTabs");
    tabs->addTab(buildVideoTab(), L("Video", "비디오"));
    tabs->addTab(buildAudioTab(), L("Audio", "오디오"));
    tabs->addTab(buildOverlaysTab(), L("Overlays", "오버레이"));

    // destination
    m_name = new QLineEdit;
    m_name->setObjectName("ExportName");
    m_folder = new QLineEdit(QDir::toNativeSeparators(defaultFolder()));
    m_folder->setObjectName("ExportFolder");
    auto* browseBtn = new QPushButton(L("Browse...", "찾아보기..."));
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::browse);
    auto* dest = new QGridLayout;
    dest->setHorizontalSpacing(10);
    dest->setVerticalSpacing(6);
    dest->addWidget(key(L("File name", "파일 이름")), 0, 0);
    dest->addWidget(m_name, 0, 1, 1, 2);
    dest->addWidget(key(L("Location", "저장 위치")), 1, 0);
    dest->addWidget(m_folder, 1, 1);
    dest->addWidget(browseBtn, 1, 2);
    dest->setColumnStretch(1, 1);

    auto* cancel = new QPushButton(L("Cancel", "취소"));
    auto* ok = new QPushButton(L("Export", "내보내기"));
    ok->setObjectName("Primary");
    ok->setDefault(true);
    ok->setMinimumWidth(120);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, [this] {
        if (m_name->text().trimmed().isEmpty() || m_folder->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, L("Export", "내보내기"), L("Choose a file name and a location.", "파일 이름과 저장 위치를 정하세요."));
            return;
        }
        if (!QDir().mkpath(m_folder->text().trimmed())) {
            QMessageBox::warning(this, L("Export", "내보내기"), L("Can't create the folder:\n%1", "폴더를 만들 수 없습니다:\n%1").arg(m_folder->text()));
            return;
        }
        QSettings s;
        s.setValue("export/format", formatIndex());
        s.setValue("export/notes", m_notes->isChecked());
        s.setValue("export/caption", m_caption->isChecked());
        s.setValue("export/tracks", m_tracks->isChecked());
        s.setValue("export/audio", m_audio->isChecked());
        s.setValue("export/dir", QDir::fromNativeSeparators(m_folder->text().trimmed()));
        accept();
    });
    auto* footer = new QHBoxLayout;
    footer->addLayout(dest, 1);
    footer->addSpacing(18);
    footer->addWidget(cancel, 0, Qt::AlignBottom);
    footer->addWidget(ok, 0, Qt::AlignBottom);

    // Three columns on a desktop; one scrolling column on a narrow (phone) screen.
    auto* columns = new QWidget;
    auto* cols = new QBoxLayout(QBoxLayout::LeftToRight, columns);
    cols->setContentsMargins(0, 0, 0, 0);
    cols->setSpacing(14);
    QWidget* presets = buildPresets();
    QWidget* summary = buildSummary();
    cols->addWidget(presets);
    cols->addWidget(tabs, 1);
    cols->addWidget(summary);
    const QScreen* screen = QGuiApplication::primaryScreen();
    const bool narrow = screen && screen->availableGeometry().width() < 900;
    if (narrow) {
        cols->setDirection(QBoxLayout::TopToBottom);
        presets->setMaximumWidth(QWIDGETSIZE_MAX);
        presets->setMaximumHeight(220);
        summary->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(columns);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(18, 16, 18, 16);
    v->setSpacing(14);
    v->addWidget(scroll, 1);
    auto* line = new QFrame;
    line->setObjectName("Divider");
    line->setFixedHeight(1);
    v->addWidget(line);
    v->addLayout(footer);

    // initial values
    setFormatIndex(std::clamp(settings.value("export/format", 0).toInt(), 0, kFormatCount - 1));
    m_name->setText(m_ranges.suggestedName);
    updateOutputSuffix();

    connect(m_container, &QComboBox::currentIndexChanged, this, [this] {
        refreshCodecs();
        updateOutputSuffix();
        updateSummary();
    });
    connect(m_codec, &QComboBox::currentIndexChanged, this, [this] {
        updateOutputSuffix();
        updateSummary();
    });
    connect(m_rangeGroup, &QButtonGroup::idToggled, this, [this](int, bool on) { if (on) updateSummary(); });
    connect(m_scale, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    connect(m_fps, &QDoubleSpinBox::valueChanged, this, &ExportDialog::updateSummary);
    connect(m_margin, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    for (QCheckBox* c : { m_notes, m_caption, m_tracks, m_audio, m_compare, m_filter })
        connect(c, &QCheckBox::toggled, this, &ExportDialog::updateSummary);
    connect(m_name, &QLineEdit::textChanged, this, &ExportDialog::updateSummary);
    connect(m_folder, &QLineEdit::textChanged, this, &ExportDialog::updateSummary);
    // Changing a setting by hand leaves the preset that was picked.
    for (QObject* o : std::initializer_list<QObject*>{ m_container, m_codec, m_scale, m_margin })
        connect(qobject_cast<QComboBox*>(o), &QComboBox::activated, this, [this] { m_presets->setCurrentRow(-1); });
    updateSummary();

    if (narrow || Platform::isAndroid()) setWindowState(Qt::WindowMaximized);
    else resize(1060, 640);
}

QWidget* ExportDialog::buildPresets()
{
    auto* panel = new QWidget;
    panel->setObjectName("ExportPresets");
    panel->setFixedWidth(210);
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    v->addWidget(sectionTitle(L("Presets", "프리셋")));
    m_presets = new QListWidget;
    m_presets->setObjectName("PresetList");
    v->addWidget(m_presets, 1);
    auto* save = new QPushButton(L("Save...", "저장..."));
    save->setObjectName("SaveExportPreset");
    save->setToolTip(L("Save the current settings as a preset", "현재 설정을 프리셋으로 저장"));
    m_deletePreset = new QPushButton(L("Delete", "삭제"));
    m_deletePreset->setEnabled(false);
    auto* row = new QHBoxLayout;
    row->addWidget(save, 1);
    row->addWidget(m_deletePreset, 1);
    v->addLayout(row);
    refreshPresets();

    connect(m_presets, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        m_deletePreset->setEnabled(item && !item->data(Qt::UserRole + 1).toString().isEmpty());
        if (!item) return;
        const QJsonObject values = item->data(Qt::UserRole).toJsonObject();
        if (!values.isEmpty()) applyPreset(values);
    });
    connect(save, &QPushButton::clicked, this, [this] {
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
            if (m_presets->item(i)->data(Qt::UserRole + 1).toString() == name) {
                const QSignalBlocker block(m_presets);
                m_presets->setCurrentRow(i);
                m_deletePreset->setEnabled(true);
            }
    });
    connect(m_deletePreset, &QPushButton::clicked, this, [this] {
        QListWidgetItem* item = m_presets->currentItem();
        const QString name = item ? item->data(Qt::UserRole + 1).toString() : QString();
        if (name.isEmpty()) return;
        QSettings settings;
        auto custom = QJsonDocument::fromJson(settings.value("export/presets").toByteArray()).object();
        custom.remove(name);
        settings.setValue("export/presets", QJsonDocument(custom).toJson(QJsonDocument::Compact));
        refreshPresets();
        m_deletePreset->setEnabled(false);
    });
    return panel;
}

void ExportDialog::refreshPresets()
{
    const QSignalBlocker block(m_presets);
    m_presets->clear();
    auto add = [this](const QString& title, const QString& detail, const QJsonObject& values, const QString& customName = QString()) {
        auto* item = new QListWidgetItem(detail.isEmpty() ? title : title + QStringLiteral("\n") + detail);
        item->setData(Qt::UserRole, values);
        item->setData(Qt::UserRole + 1, customName);
        item->setData(Qt::UserRole + 2, title);
        m_presets->addItem(item);
    };
    add(L("Review MP4", "리뷰 MP4"), L("H.264 · full size · notes", "H.264 · 원본 크기 · 주석"), QJsonObject{ { "format", 0 }, { "scale", 0 }, { "notes", true }, { "audio", true } });
    add(L("Compact MP4", "작은 MP4"), L("H.264 · 50% · for chat / mail", "H.264 · 50% · 메신저 / 메일용"), QJsonObject{ { "format", 0 }, { "scale", 2 }, { "notes", true }, { "audio", true } });
    if (!Platform::isAndroid())
        add(L("Fast GPU MP4", "빠른 GPU MP4"), L("H.265 NVENC · full size", "H.265 NVENC · 원본 크기"), QJsonObject{ { "format", 2 }, { "scale", 0 }, { "notes", true }, { "audio", true } });
    add(L("ProRes Master", "ProRes 마스터"), L("ProRes 422 HQ · PCM audio", "ProRes 422 HQ · PCM 오디오"), QJsonObject{ { "format", 3 }, { "notes", true }, { "audio", true } });
    add(L("Clean Plate", "깨끗한 원본"), L("H.264 · no annotations", "H.264 · 주석 없음"), QJsonObject{ { "format", 0 }, { "notes", false }, { "tracks", false }, { "caption", false }, { "audio", true } });
    add(L("2D PNG Sequence", "2D PNG 시퀀스"), L("PNG stills · notes, no trails", "PNG 이미지 · 주석, 커브 없음"), QJsonObject{ { "format", 4 }, { "notes", true }, { "tracks", false }, { "audio", false } });
    const auto custom = QJsonDocument::fromJson(QSettings().value("export/presets").toByteArray()).object();
    for (auto it = custom.begin(); it != custom.end(); ++it) {
        const QJsonObject values = it.value().toObject();
        const int f = std::clamp(values.value("format").toInt(), 0, kFormatCount - 1);
        add(it.key(), L("Custom · %1", "사용자 · %1").arg(QString::fromUtf8(kFormats[f].codec)), values, it.key());
    }
    // Plain names make lookups by preset name work (findItems / tests).
    for (int i = 0; i < m_presets->count(); ++i) {
        QListWidgetItem* item = m_presets->item(i);
        item->setToolTip(item->text());
        item->setText(item->data(Qt::UserRole + 2).toString());
        item->setStatusTip(item->toolTip());
    }
}

QWidget* ExportDialog::buildVideoTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 12, 0, 0);
    v->setSpacing(12);

    // FORMAT
    m_container = new QComboBox;
    m_container->setObjectName("ExportContainer");
    m_container->addItem(L("MP4 (H.264 / H.265)", "MP4 (H.264 / H.265)"), Mp4);
    m_container->addItem(L("QuickTime MOV (ProRes)", "QuickTime MOV (ProRes)"), Mov);
    m_container->addItem(L("Image Sequence", "이미지 시퀀스"), ImageSeq);
    m_codec = new QComboBox;
    m_codec->setObjectName("ExportCodec");
    auto* codecHint = new QLabel;
    codecHint->setObjectName("Muted");
    codecHint->setWordWrap(true);
    connect(m_codec, &QComboBox::currentIndexChanged, codecHint, [this, codecHint] {
        const int f = formatIndex();
        if (Platform::isAndroid() && f == 0)
            codecHint->setText(L("MPEG-4 video in an MP4 file, encoded on the device. Plays in common players; 1 s keyframe interval for smooth scrubbing.",
                                 "기기에서 인코딩하는 MP4(MPEG-4 비디오). 일반 플레이어에서 재생되고, 스크럽이 부드럽도록 1초마다 키프레임."));
        else
            codecHint->setText(L(kFormats[f].hint, kFormats[f].hintKo));
    });
    auto* format = new QFormLayout;
    format->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    format->setHorizontalSpacing(14);
    format->addRow(key(L("Format", "형식")), m_container);
    format->addRow(key(L("Codec", "코덱")), m_codec);
    format->addRow(QString(), codecHint);
    v->addWidget(section(L("Format", "형식"), format));

    // RANGE
    m_rangeGroup = new QButtonGroup(this);
    auto* rangeRow = new QHBoxLayout;
    rangeRow->setSpacing(16);
    auto addRange = [&](int id, const QString& text, bool enabled) {
        auto* r = new QRadioButton(text);
        r->setEnabled(enabled);
        m_rangeGroup->addButton(r, id);
        rangeRow->addWidget(r);
    };
    addRange(0, L("Entire", "전체"), true);
    addRange(1, L("Current clip", "현재 클립"), m_ranges.clipEnd >= m_ranges.clipStart);
    addRange(2, QStringLiteral("In / Out"), m_ranges.hasLoop);
    rangeRow->addStretch(1);
    m_rangeGroup->button(m_ranges.hasLoop ? 2 : 0)->setChecked(true);
    m_rangeLabel = new QLabel;
    m_rangeLabel->setObjectName("Muted");
    auto* rangeBox = new QVBoxLayout;
    rangeBox->addLayout(rangeRow);
    rangeBox->addWidget(m_rangeLabel);
    v->addWidget(section(L("Range", "범위"), rangeBox));

    // FRAME
    m_scale = new QComboBox;
    m_scale->addItems({ "100%", "75%", "50%", "25%" });
    m_sizeLabel = new QLabel;
    m_sizeLabel->setObjectName("Muted");
    auto* scaleRow = new QHBoxLayout;
    scaleRow->addWidget(m_scale);
    scaleRow->addWidget(m_sizeLabel, 1);
    m_fps = new QDoubleSpinBox;
    m_fps->setRange(1, 240);
    m_fps->setDecimals(3);
    m_fps->setValue(m_ranges.fps);
    auto* fpsRow = new QHBoxLayout;
    fpsRow->addWidget(m_fps);
    auto* fpsHint = new QLabel(L("timeline %1 fps · other rates speed the sound up or down", "타임라인 %1 fps · 다르게 하면 소리도 같은 비율로 빨라지거나 느려짐").arg(m_ranges.fps, 0, 'f', 3));
    fpsHint->setObjectName("Muted");
    fpsRow->addWidget(fpsHint, 1);
    m_margin = new QComboBox;
    m_margin->addItems({ "0%", "10%", "25%", "50%", "100%" });
    m_margin->setToolTip(L("Extra canvas around the video for annotations outside its edges", "영상 바깥의 주석을 내보내기 위한 추가 캔버스 여백"));
    auto* frame = new QFormLayout;
    frame->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    frame->setHorizontalSpacing(14);
    frame->addRow(key(L("Resolution", "해상도")), scaleRow);
    frame->addRow(key(L("Frame rate", "프레임 레이트")), fpsRow);
    frame->addRow(key(L("Canvas border", "캔버스 여백")), m_margin);
    v->addWidget(section(L("Frame", "프레임"), frame));
    v->addStretch(1);
    return page;
}

QWidget* ExportDialog::buildAudioTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 12, 0, 0);
    v->setSpacing(12);
    m_audioInfo = new QLabel;
    m_audioInfo->setObjectName("Muted");
    m_audioInfo->setWordWrap(true);
    auto* box = new QVBoxLayout;
    box->addWidget(m_audio);
    box->addWidget(m_audioInfo);
    v->addWidget(section(L("Audio", "오디오"), box));
    v->addStretch(1);
    return page;
}

QWidget* ExportDialog::buildOverlaysTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 12, 0, 0);
    v->setSpacing(12);
    auto* burn = new QVBoxLayout;
    burn->addWidget(m_notes);
    burn->addWidget(m_tracks);
    burn->addWidget(m_caption);
    m_filter->setVisible(!m_ranges.filterName.isEmpty());
    burn->addWidget(m_filter);
    v->addWidget(section(L("Burn in", "영상에 포함"), burn));
    if (m_ranges.comparing) {
        auto* cmp = new QVBoxLayout;
        cmp->addWidget(m_compare);
        auto* hint = new QLabel(L("A and B side by side (or wiped / blended) exactly as the viewer shows them.",
                                  "뷰어에 보이는 그대로 A와 B를 나란히 (또는 와이프 / 겹쳐) 내보냅니다."));
        hint->setObjectName("Muted");
        hint->setWordWrap(true);
        cmp->addWidget(hint);
        v->addWidget(section(L("Compare", "비교"), cmp));
    } else {
        m_compare->setVisible(false);
    }
    v->addStretch(1);
    return page;
}

QWidget* ExportDialog::buildSummary()
{
    auto* panel = new QWidget;
    panel->setObjectName("ExportSummary");
    panel->setAttribute(Qt::WA_StyledBackground);
    panel->setFixedWidth(280);
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(14, 12, 14, 14);
    v->setSpacing(10);
    v->addWidget(sectionTitle(L("Summary", "요약")));
    auto* preview = new QLabel;
    preview->setObjectName("ExportPreview");
    preview->setAlignment(Qt::AlignCenter);
    preview->setFixedHeight(146);
    if (!m_ranges.preview.isNull())
        preview->setPixmap(QPixmap::fromImage(m_ranges.preview.scaled(250, 140, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    else
        preview->setText(L("No preview", "미리보기 없음"));
    v->addWidget(preview);
    m_summary = new QLabel;
    m_summary->setObjectName("SummaryText");
    m_summary->setTextFormat(Qt::RichText);
    m_summary->setWordWrap(true);
    m_summary->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    v->addWidget(m_summary, 1);
    m_estimate = new QLabel;
    m_estimate->setObjectName("Estimate");
    v->addWidget(m_estimate);
    return panel;
}

void ExportDialog::refreshCodecs()
{
    const int container = m_container->currentData().toInt();
    const int keep = m_codec->count() ? formatIndex() : -1;
    {
        const QSignalBlocker block(m_codec);
        m_codec->clear();
        for (int i = 0; i < kFormatCount; ++i)
            if (kFormats[i].container == container && formatAvailable(i))
            m_codec->addItem(Platform::isAndroid() && i == 0 ? L("MPEG-4 (high quality)", "MPEG-4 (고화질)") : L(kFormats[i].codec, kFormats[i].codecKo), i);
        m_codec->setCurrentIndex(std::max(0, m_codec->findData(keep)));
    }
    emit m_codec->currentIndexChanged(m_codec->currentIndex());  // codec hint, file suffix, summary
}

int ExportDialog::formatIndex() const
{
    const int f = m_codec ? m_codec->currentData().toInt() : 0;
    return std::clamp(f, 0, kFormatCount - 1);
}

void ExportDialog::setFormatIndex(int index)
{
    index = std::clamp(index, 0, kFormatCount - 1);
    if (!formatAvailable(index)) index = 0;
    {
        const QSignalBlocker block(m_container);
        m_container->setCurrentIndex(m_container->findData(int(kFormats[index].container)));
    }
    refreshCodecs();
    m_codec->setCurrentIndex(std::max(0, m_codec->findData(index)));
    updateOutputSuffix();
    updateSummary();
}

ExportJob::Format ExportDialog::format() const { return kFormats[formatIndex()].format; }

QString ExportDialog::output() const
{
    const QString name = m_name->text().trimmed();
    const QString folder = QDir::fromNativeSeparators(m_folder->text().trimmed());
    if (name.isEmpty() || folder.isEmpty()) return QString();
    return QDir(folder).filePath(name);
}

double ExportDialog::fps() const { return m_fps->value(); }
double ExportDialog::scale() const { return QList<double>{ 1.0, 0.75, 0.5, 0.25 }.value(m_scale->currentIndex(), 1.0); }
bool ExportDialog::notes() const { return m_notes->isChecked(); }
bool ExportDialog::caption() const { return m_caption->isChecked(); }
bool ExportDialog::tracks() const { return m_tracks->isChecked(); }
bool ExportDialog::audio() const { return m_audio->isChecked() && m_audio->isEnabled(); }
bool ExportDialog::comparison() const { return m_ranges.comparing && m_compare->isChecked(); }
bool ExportDialog::filter() const { return m_filter && !m_ranges.filterName.isEmpty() && m_filter->isChecked(); }
double ExportDialog::margin() const { return QList<double>{ 0, .10, .25, .50, 1 }.value(m_margin->currentIndex()); }

QJsonObject ExportDialog::presetValues() const
{
    return { { "format", formatIndex() }, { "range", m_rangeGroup->checkedId() }, { "fps", fps() },
             { "scale", m_scale->currentIndex() }, { "margin", m_margin->currentIndex() },
             { "notes", notes() }, { "caption", caption() }, { "tracks", tracks() }, { "audio", m_audio->isChecked() }, { "compare", m_compare->isChecked() } };
}

void ExportDialog::applyPreset(const QJsonObject& v)
{
    m_applying = true;
    setFormatIndex(v.value("format").toInt());
    if (v.contains("range"))
        if (QAbstractButton* b = m_rangeGroup->button(v.value("range").toInt()); b && b->isEnabled()) b->setChecked(true);
    m_fps->setValue(v.value("fps").toDouble(m_ranges.fps));
    m_scale->setCurrentIndex(std::clamp(v.value("scale").toInt(), 0, m_scale->count() - 1));
    m_margin->setCurrentIndex(std::clamp(v.value("margin").toInt(), 0, m_margin->count() - 1));
    m_notes->setChecked(v.value("notes").toBool(true));
    m_caption->setChecked(v.value("caption").toBool());
    m_tracks->setChecked(v.value("tracks").toBool(true));
    m_audio->setChecked(v.value("audio").toBool(true));
    m_compare->setChecked(v.value("compare").toBool(m_ranges.comparing));
    m_applying = false;
    updateOutputSuffix();
    updateSummary();
}

std::pair<int64_t, int64_t> ExportDialog::range() const
{
    switch (m_rangeGroup->checkedId()) {
    case 1: return { m_ranges.clipStart, m_ranges.clipEnd };
    case 2: return { m_ranges.loopIn, m_ranges.loopOut };
    default: return { 0, m_ranges.total - 1 };
    }
}

void ExportDialog::updateOutputSuffix()
{
    const QString suffix = QString::fromLatin1(kFormats[formatIndex()].suffix);
    QString name = m_name->text().trimmed();
    const QFileInfo fi(name);
    const QString ext = fi.suffix().toLower();
    if (ext == "mp4" || ext == "mov") name = fi.completeBaseName();
    if (!suffix.isEmpty() && !name.isEmpty()) name += QLatin1Char('.') + suffix;
    const QSignalBlocker block(m_name);
    m_name->setText(name);
}

void ExportDialog::browse()
{
    const QString suffix = QString::fromLatin1(kFormats[formatIndex()].suffix);
    if (suffix.isEmpty()) {
        const QString dir = QFileDialog::getExistingDirectory(this, L("Sequence Folder", "시퀀스 폴더"), m_folder->text());
        if (!dir.isEmpty()) m_folder->setText(QDir::toNativeSeparators(dir));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, L("Export", "내보내기"), output(),
                                                      QStringLiteral("%1 (*.%2)").arg(suffix.toUpper(), suffix));
    if (path.isEmpty()) return;
    m_folder->setText(QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
    m_name->setText(QFileInfo(path).fileName());
    updateOutputSuffix();
}

void ExportDialog::updateSummary()
{
    if (m_applying || !m_summary) return;
    const int f = formatIndex();
    const FormatInfo& info = kFormats[f];
    const bool seq = info.container == ImageSeq;
    m_audio->setEnabled(!seq && m_ranges.hasAudio);
    const bool withAudio = audio();

    const auto [a, b] = range();
    const int64_t n = std::max<int64_t>(0, b - a + 1);
    const double seconds = n / std::max(1.0, m_fps->value());
    m_rangeLabel->setText(L("Frames %1 – %2  ·  %3 frames  ·  %4", "프레임 %1 – %2  ·  %3프레임  ·  %4")
                              .arg(a).arg(b).arg(n).arg(timecode(n, m_fps->value())) +
                          (m_rangeGroup->checkedId() == 1 && !m_ranges.clipName.isEmpty() ? QStringLiteral("  ·  ") + m_ranges.clipName : QString()));

    QSize out;
    if (m_ranges.size.isValid() && !m_ranges.size.isEmpty()) {
        const double s = scale() * (1.0 + 2.0 * margin());
        out = QSize(std::max(2, int(std::lround(m_ranges.size.width() * s)) & ~1), std::max(2, int(std::lround(m_ranges.size.height() * s)) & ~1));
        m_sizeLabel->setText(QStringLiteral("%1 × %2").arg(out.width()).arg(out.height()));
    } else {
        m_sizeLabel->setText(QString());
    }

    const QString audioText = seq ? L("None (image sequence)", "없음 (이미지 시퀀스)")
                              : !m_ranges.hasAudio ? L("None (no sound in this range)", "없음 (이 범위에 소리 없음)")
                              : !withAudio ? L("Off", "끔")
                              : info.container == Mov ? L("PCM 16-bit · 48 kHz stereo", "PCM 16비트 · 48 kHz 스테레오")
                                                      : L("AAC · 256 kb/s · 48 kHz stereo", "AAC · 256 kb/s · 48 kHz 스테레오");
    m_audioInfo->setText(seq ? L("Image sequences have no sound. Pick MP4 or MOV to include audio.", "이미지 시퀀스에는 소리가 없습니다. 오디오를 넣으려면 MP4나 MOV를 고르세요.")
                             : audioText);

    // Rough size: bit rate scaled by picture area and frame rate (sequences: bytes per frame).
    const double area = out.isValid() ? out.width() * double(out.height()) / (1920.0 * 1080.0) : scale() * scale();
    double bytes = seq ? info.mbps * area * (1 << 20) * n
                       : (info.mbps * area * (m_fps->value() / 24.0) * 1e6 / 8.0) * seconds;
    if (withAudio) bytes += (info.container == Mov ? 1536e3 : 256e3) / 8.0 * seconds;
    m_estimate->setText(L("Estimated size  ≈ %1", "예상 크기  ≈ %1").arg(formatSize(bytes)));

    QStringList overlays;
    if (notes()) overlays << L("annotations", "주석");
    if (tracks()) overlays << L("trails", "커브");
    if (caption()) overlays << L("caption", "캡션");
    if (filter()) overlays << m_ranges.filterName;
    if (comparison()) overlays << L("A/B layout", "A/B 화면");
    const QString file = seq ? QDir(m_folder->text()).filePath(m_name->text() + QStringLiteral("_0000.") + (f == 4 ? "png" : f == 5 ? "jpg" : "tif"))
                             : output();
    auto row = [](const QString& k, const QString& value) {
        return QStringLiteral("<tr><td style='color:#6e6c66;padding:2px 10px 2px 0'>%1</td><td style='color:#e5e3db'>%2</td></tr>").arg(k, value.toHtmlEscaped());
    };
    QString html = QStringLiteral("<table cellspacing='0'>");
    html += row(L("Output", "출력"), QFileInfo(file).fileName());
    html += row(L("Format", "형식"), m_container->currentText());
    html += row(L("Video", "비디오"), QStringLiteral("%1%2 · %3 fps").arg(m_codec->currentText(),
                                                                       out.isValid() ? QStringLiteral(" · %1×%2").arg(out.width()).arg(out.height()) : QString())
                                                    .arg(m_fps->value(), 0, 'f', m_fps->value() == std::round(m_fps->value()) ? 0 : 3));
    html += row(L("Audio", "오디오"), audioText);
    html += row(L("Range", "범위"), L("%1 frames · %2 s", "%1프레임 · %2초").arg(n).arg(seconds, 0, 'f', 2));
    html += row(L("Overlays", "오버레이"), overlays.isEmpty() ? L("none", "없음") : overlays.join(QStringLiteral(", ")));
    html += row(L("Folder", "폴더"), QDir::toNativeSeparators(m_folder->text()));
    html += QStringLiteral("</table>");
    m_summary->setText(html);
}
