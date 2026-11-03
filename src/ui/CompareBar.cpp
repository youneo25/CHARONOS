#include "CompareBar.h"

#include "Icons.h"
#include "Lang.h"

#include <QBoxLayout>
#include <QButtonGroup>
#include <QComboBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QResizeEvent>
#include <QToolButton>

namespace {

QLabel* badge(const QString& text, const char* name)
{
    auto* l = new QLabel(text);
    l->setObjectName(QLatin1String(name));
    l->setAlignment(Qt::AlignCenter);
    l->setFixedSize(22, 22);
    return l;
}

QLabel* caption(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName("CompareCaption");
    return l;
}

QWidget* divider()
{
    auto* w = new QWidget;
    w->setObjectName("CompareDivider");
    w->setAttribute(Qt::WA_StyledBackground);
    w->setFixedSize(1, 22);
    return w;
}

} // namespace

QToolButton* CompareBar::segment(const QString& icon, const QString& text, const QString& tip)
{
    auto* b = new QToolButton;
    b->setObjectName("Segment");
    b->setIcon(Icons::icon(icon));
    b->setIconSize(QSize(16, 16));
    b->setText(text);
    b->setToolTip(tip);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

CompareBar::CompareBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("CompareBar");
    setAttribute(Qt::WA_StyledBackground);

    // ---- row 1: what is compared
    auto* first = new QWidget;
    auto* h1 = new QHBoxLayout(first);
    h1->setContentsMargins(0, 0, 0, 0);
    h1->setSpacing(6);
    m_a = new QComboBox;
    m_a->setObjectName("CompareA");
    m_a->setMinimumContentsLength(14);
    m_a->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_a->setToolTip(L("A: the clip you play and annotate", "A: 재생하고 주석을 다는 클립"));
    m_b = new QComboBox;
    m_b->setObjectName("CompareB");
    m_b->setMinimumContentsLength(14);
    m_b->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_b->setToolTip(L("B: the clip shown next to A (a reference, an earlier take...)", "B: A 옆에 보여 줄 클립 (레퍼런스, 이전 버전 등)"));
    auto* swap = new QToolButton;
    swap->setIcon(Icons::icon("swap"));
    swap->setToolTip(L("Swap A and B  (Tab)", "A와 B 바꾸기  (Tab)"));
    swap->setFocusPolicy(Qt::NoFocus);
    h1->addWidget(badge(QStringLiteral("A"), "BadgeA"));
    h1->addWidget(m_a, 1);
    h1->addWidget(swap);
    h1->addWidget(badge(QStringLiteral("B"), "BadgeB"));
    h1->addWidget(m_b, 1);

    // ---- row 2 (beside row 1 on a wide screen): how
    m_second = new QWidget;
    auto* h2 = new QHBoxLayout(m_second);
    h2->setContentsMargins(0, 0, 0, 0);
    h2->setSpacing(4);
    m_modes = new QButtonGroup(this);
    m_modes->setExclusive(true);
    const std::tuple<CompareMode, QString, QString, QString> modes[] = {
        { CompareMode::SideBySide, "side", L("Side by side", "좌우"), L("A left, B right  (Shift+2)", "A 왼쪽, B 오른쪽  (Shift+2)") },
        { CompareMode::TopBottom, "stack", L("Stacked", "위아래"), L("A on top, B below  (Shift+3)", "A 위, B 아래  (Shift+3)") },
        { CompareMode::Wipe, "wipe", L("Wipe", "와이프"), L("One picture, split by a draggable line  (Shift+1)", "한 화면을 드래그할 수 있는 선으로 나눔  (Shift+1)") },
        { CompareMode::Blend, "blend", L("Overlay", "겹쳐 보기"), L("B at 50% over A, for spacing and silhouettes  (Shift+4)", "A 위에 B를 50%로 겹침, 간격과 실루엣 확인용  (Shift+4)") },
    };
    for (const auto& [mode, icon, text, tip] : modes) {
        QToolButton* b = segment(icon, text, tip);
        m_modes->addButton(b, int(mode));
        h2->addWidget(b);
    }
    h2->addSpacing(6);
    h2->addWidget(divider());
    h2->addSpacing(6);

    h2->addWidget(caption(L("Sync", "동기화")));
    m_sync = new QComboBox;
    m_sync->addItem(L("By time", "시간 기준"), true);
    m_sync->addItem(L("By frame", "프레임 기준"), false);
    m_sync->setToolTip(L("By time: B shows what it showed at the same moment (clips with different frame rates line up).\n"
                         "By frame: frame 10 of A next to frame 10 of B.",
                         "시간 기준: 같은 순간의 B 화면을 보여 줍니다 (FPS가 달라도 맞음).\n프레임 기준: A의 10번 프레임 옆에 B의 10번 프레임."));
    h2->addWidget(m_sync);
    h2->addWidget(caption(L("B offset", "B 오프셋")));
    m_offset = new QSpinBox;
    m_offset->setObjectName("CompareOffset");
    m_offset->setRange(-100000, 100000);
    m_offset->setSuffix(L(" f", " f"));
    m_offset->setButtonSymbols(QAbstractSpinBox::PlusMinus);
    m_offset->setToolTip(L("Slide B against A in B frames (+ = B runs ahead). Line up a reference with your shot.",
                           "B를 A에 대해 프레임 단위로 밀기 (+ = B가 앞섬). 레퍼런스와 내 샷의 타이밍을 맞출 때 사용."));
    m_offset->setFixedWidth(84);
    auto* minus = new QToolButton;
    minus->setText(QStringLiteral("−"));
    minus->setToolTip(L("B one frame back", "B 한 프레임 뒤로"));
    auto* plus = new QToolButton;
    plus->setText(QStringLiteral("+"));
    plus->setToolTip(L("B one frame ahead", "B 한 프레임 앞으로"));
    auto* zero = new QToolButton;
    zero->setText(QStringLiteral("0"));
    zero->setToolTip(L("No offset", "오프셋 없음"));
    for (QToolButton* b : { minus, plus, zero }) {
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedWidth(24);
    }
    h2->addWidget(minus);
    h2->addWidget(m_offset);
    h2->addWidget(plus);
    h2->addWidget(zero);
    h2->addSpacing(6);
    h2->addWidget(divider());
    h2->addSpacing(6);

    h2->addWidget(caption(L("Draw on", "주석 대상")));
    m_edit = new QButtonGroup(this);
    for (int side : { 0, 1 }) {
        auto* b = new QToolButton;
        b->setObjectName("Segment");
        b->setText(side ? QStringLiteral("B") : QStringLiteral("A"));
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(side ? L("Annotate B  (Shift+Tab, or click B's picture)", "B에 주석  (Shift+Tab, 또는 B 화면 클릭)")
                           : L("Annotate A", "A에 주석"));
        m_edit->addButton(b, side);
        h2->addWidget(b);
    }
    h2->addWidget(caption(L("Sound", "소리")));
    m_audio = new QComboBox;
    m_audio->addItem(L("Auto", "자동"), 0);
    m_audio->addItem(QStringLiteral("A"), 1);
    m_audio->addItem(QStringLiteral("B"), 2);
    m_audio->setToolTip(L("Which clip is heard. Auto: A if it has sound, otherwise B.", "어느 클립의 소리를 들을지. 자동: A에 소리가 있으면 A, 없으면 B."));
    h2->addWidget(m_audio);
    h2->addStretch(1);
    auto* close = new QToolButton;
    close->setIcon(Icons::icon("close"));
    close->setToolTip(L("Stop comparing  (Shift+0)", "비교 끝내기  (Shift+0)"));
    close->setFocusPolicy(Qt::NoFocus);
    h2->addWidget(close);

    m_rows = new QBoxLayout(QBoxLayout::LeftToRight, this);
    m_rows->setContentsMargins(10, 6, 8, 6);
    m_rows->setSpacing(14);
    m_rows->addWidget(first, 4);
    m_rows->addWidget(m_second, 6);

    connect(m_a, &QComboBox::activated, this, [this](int i) { emit sourceAChosen(m_a->itemData(i).toString()); });
    connect(m_b, &QComboBox::activated, this, [this](int i) { emit sourceBChosen(m_b->itemData(i).toString()); });
    connect(swap, &QToolButton::clicked, this, &CompareBar::swapRequested);
    connect(m_modes, &QButtonGroup::idClicked, this, [this](int id) { emit modeChosen(CompareMode(id)); });
    connect(m_sync, &QComboBox::activated, this, [this](int i) { emit syncByTimeChanged(m_sync->itemData(i).toBool()); });
    connect(m_offset, &QSpinBox::valueChanged, this, [this](int v) {
        if (!m_filling) emit offsetChanged(v);
    });
    connect(minus, &QToolButton::clicked, m_offset, [this] { m_offset->setValue(m_offset->value() - 1); });
    connect(plus, &QToolButton::clicked, m_offset, [this] { m_offset->setValue(m_offset->value() + 1); });
    connect(zero, &QToolButton::clicked, m_offset, [this] { m_offset->setValue(0); });
    connect(m_edit, &QButtonGroup::idClicked, this, &CompareBar::editSideChosen);
    connect(m_audio, &QComboBox::activated, this, [this](int i) { emit audioSourceChosen(m_audio->itemData(i).toInt()); });
    connect(close, &QToolButton::clicked, this, &CompareBar::closeRequested);
}

void CompareBar::setSources(const QVector<SourceItem>& items, const QString& a, const QString& b)
{
    m_filling = true;
    for (QComboBox* combo : { m_a, m_b }) {
        const QSignalBlocker block(combo);
        combo->clear();
        for (const SourceItem& s : items) {
            combo->addItem(s.name, s.id);
            const int row = combo->count() - 1;
            if (!s.available) combo->setItemData(row, QVariant(0), Qt::UserRole - 1);  // offline media can't be picked
        }
    }
    m_a->setCurrentIndex(m_a->findData(a));
    m_b->setCurrentIndex(m_b->findData(b));
    m_filling = false;
}

void CompareBar::setMode(CompareMode mode)
{
    if (QAbstractButton* b = m_modes->button(int(mode))) b->setChecked(true);
}

void CompareBar::setSyncByTime(bool byTime)
{
    const QSignalBlocker block(m_sync);
    m_sync->setCurrentIndex(byTime ? 0 : 1);
}

void CompareBar::setOffset(int frames)
{
    m_filling = true;
    m_offset->setValue(frames);
    m_filling = false;
}

void CompareBar::setEditSide(int side)
{
    if (QAbstractButton* b = m_edit->button(side)) b->setChecked(true);
}

void CompareBar::setAudioSource(int mode)
{
    const QSignalBlocker block(m_audio);
    m_audio->setCurrentIndex(std::clamp(mode, 0, 2));
}

void CompareBar::setCompact(bool compact)
{
    if (compact == m_compact) return;
    m_compact = compact;
    m_rows->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_rows->setSpacing(compact ? 6 : 14);
}

void CompareBar::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    // One row when everything fits, two rows on a laptop, icons only on a phone.
    setCompact(width() < 1380);
    const bool iconsOnly = width() < 760;
    if (iconsOnly != m_iconsOnly) {
        m_iconsOnly = iconsOnly;
        for (QAbstractButton* b : m_modes->buttons())
            static_cast<QToolButton*>(b)->setToolButtonStyle(iconsOnly ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
}
