#include "FrameExportDialog.h"
#include "Lang.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

FrameExportDialog::FrameExportDialog(const QImage& frame, const Source& source, int localFrame, QWidget* parent, int trackRange)
    : QDialog(parent), m_frame(frame), m_source(source), m_local(localFrame), m_trackRange(trackRange)
{
    setObjectName("FrameExportOptions");
    setWindowTitle(L("Export Current Frame", "현재 프레임 이미지 내보내기"));
    setMinimumWidth(450);
    QSettings s;
    auto* layout = new QVBoxLayout(this);
    auto* clip = new QLabel(L("Clip: %1 · Frame %2", "클립: %1 · 프레임 %2").arg(source.displayName()).arg(localFrame));
    clip->setWordWrap(true); layout->addWidget(clip);
    auto* form = new QFormLayout;
    m_format = new QComboBox; m_format->setObjectName("FrameFormat");
    m_format->addItems({"PNG", "JPEG", "TIFF"});
    m_format->setCurrentIndex(std::clamp(s.value("still/format", 0).toInt(), 0, 2));
    m_scale = new QDoubleSpinBox; m_scale->setObjectName("FrameScale");
    m_scale->setRange(10, 400); m_scale->setSuffix(" %"); m_scale->setValue(s.value("still/scale", 100).toDouble());
    m_padding = new QSpinBox; m_padding->setRange(0, 2048); m_padding->setSuffix(" px"); m_padding->setValue(s.value("still/padding", 16).toInt());
    m_background = new QComboBox; m_background->setObjectName("FrameBackground");
    m_background->addItems({L("Black", "검정"), L("White", "흰색"), L("Transparent", "투명")});
    m_background->setCurrentIndex(std::clamp(s.value("still/background", 0).toInt(), 0, 2));
    m_quality = new QSpinBox; m_quality->setRange(1, 100); m_quality->setValue(s.value("still/quality", 95).toInt());
    form->addRow(L("Format", "파일 형식"), m_format);
    form->addRow(L("Scale", "출력 배율"), m_scale);
    form->addRow(L("Outside padding", "바깥 여백"), m_padding);
    form->addRow(L("Background", "배경"), m_background);
    form->addRow(L("JPEG quality", "JPEG 품질"), m_quality);
    layout->addLayout(form);
    auto check = [&](QCheckBox*& box, const QString& text, const char* key, bool value) {
        box = new QCheckBox(text); box->setChecked(s.value(key, value).toBool()); layout->addWidget(box);
        connect(box, &QCheckBox::toggled, this, [this] { refresh(); });
    };
    check(m_outside, L("Automatically include content outside the video", "영상 바깥의 내용을 자동으로 포함"), "still/outside", true);
    m_outside->setObjectName("FrameIncludeOutside");
    check(m_video, L("Include video / canvas", "영상 / 캔버스 포함"), "still/video", true);
    check(m_notes, L("Include annotations and edited images", "주석 및 편집한 이미지 포함"), "still/notes", true);
    check(m_tracks, L("Include tracking trails", "트래킹 경로 포함"), "still/tracks", true);
    check(m_caption, L("Include clip name and frame number", "클립 이름 및 프레임 번호 포함"), "still/caption", false);
    m_size = new QLabel; m_size->setWordWrap(true); layout->addWidget(m_size);
    auto* hint = new QLabel(L("Exports the active clip (A or B) at native resolution before scaling. PNG/TIFF support transparency; disable video for annotations only.",
        "현재 편집 중인 클립(A 또는 B)을 원본 해상도 기준으로 저장합니다. PNG/TIFF는 투명 배경을 지원하며, 영상을 끄면 주석만 저장합니다."));
    hint->setWordWrap(true); layout->addWidget(hint);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);
    connect(m_format, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(m_background, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(m_scale, &QDoubleSpinBox::valueChanged, this, [this] { refresh(); });
    connect(m_padding, &QSpinBox::valueChanged, this, [this] { refresh(); });
    refresh();
}

StillFrameOptions FrameExportDialog::options() const
{
    StillFrameOptions o;
    o.trackRange = m_trackRange;
    o.scale = m_scale->value() / 100.; o.padding = m_padding->value();
    o.includeOutside = m_outside->isChecked(); o.notes = m_notes->isChecked();
    o.video = m_video->isChecked(); o.tracks = m_tracks->isChecked();
    o.background = m_background->currentIndex() == 1 ? QColor(Qt::white)
        : m_background->currentIndex() == 2 && m_format->currentIndex() != 1 ? QColor(Qt::transparent) : QColor(Qt::black);
    if (m_caption->isChecked()) o.caption = QStringLiteral("%1   #%2").arg(m_source.displayName()).arg(m_local);
    return o;
}

QByteArray FrameExportDialog::format() const { return m_format->currentIndex() == 1 ? "jpg" : m_format->currentIndex() == 2 ? "tif" : "png"; }
int FrameExportDialog::quality() const { return m_quality->value(); }

void FrameExportDialog::refresh()
{
    if (m_format->currentIndex() == 1 && m_background->currentIndex() == 2) m_background->setCurrentIndex(0);
    m_quality->setEnabled(m_format->currentIndex() == 1);
    m_padding->setEnabled(m_outside->isChecked());
    const auto o = options();
    const QSizeF size = stillFrameBounds(m_frame.size(), m_source, m_local, o).size() * o.scale;
    const double w = std::ceil(size.width()), h = std::ceil(size.height());
    const bool valid = w >= 1 && h >= 1 && w <= 16384 && h <= 16384 && w * h <= 64000000;
    m_size->setText(valid ? L("Output: %1 × %2 px", "출력 크기: %1 × %2 px").arg(w).arg(h)
        : L("Output too large. Reduce scale or content extent (maximum 64 MP / 16384 px per edge).", "출력 크기가 너무 큽니다. 배율이나 내용의 범위를 줄이세요 (최대 64MP / 한 변 16384px)."));
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(valid);
}

void FrameExportDialog::saveSettings() const
{
    QSettings s;
    s.setValue("still/format", m_format->currentIndex()); s.setValue("still/background", m_background->currentIndex());
    s.setValue("still/scale", m_scale->value()); s.setValue("still/padding", m_padding->value());
    s.setValue("still/quality", m_quality->value()); s.setValue("still/outside", m_outside->isChecked());
    s.setValue("still/video", m_video->isChecked()); s.setValue("still/notes", m_notes->isChecked());
    s.setValue("still/tracks", m_tracks->isChecked()); s.setValue("still/caption", m_caption->isChecked());
}
