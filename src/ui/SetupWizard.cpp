#include "SetupWizard.h"

#include "Lang.h"
#include "app/Edition.h"
#include "platform/Platform.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QLocale>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kSetupVersion = 1;  // bump to show the wizard again after a big change

QString gb(qint64 bytes) { return QStringLiteral("%1 GB").arg(bytes / double(qint64(1) << 30), 0, 'f', 1); }

struct PanelInfo {
    const char* key;   // QSettings key under layout/
    const char* en;
    const char* ko;
    bool def;
    bool fullOnly;
};

// What a new install shows: Project View and the Default Timeline panel (plus the viewer's own
// scrub and transport bars). Every other tool starts hidden and is one click away in Window.
const PanelInfo kPanels[] = {
    { "projectPanel", "Project View", "프로젝트 뷰", true, false },
    { "sourcesPanel", "Default Timeline (Sources / Properties)", "Default Timeline (소스 / 속성)", true, false },
    { "scrubBar", "Scrub Bar (under the viewer)", "재생 바 (뷰어 아래)", true, false },
    { "transport", "Transport Bar (play controls)", "재생 컨트롤 바", true, false },
    { "toolStrip", "Annotation Toolbar", "주석 도구 막대", false, false },
    { "clipBar", "Clip Bar (source range)", "클립 바 (소스 범위)", false, false },
    { "timelineVisible", "Timeline Editor (multi-track)", "타임라인 편집기 (멀티 트랙)", false, true },
};

} // namespace

bool SetupWizard::needed() { return QSettings().value("setup/version", 0).toInt() < kSetupVersion; }

void SetupWizard::writePanelDefaults(bool toolbar, bool clipBar, bool scrubBar, bool transport, bool timeline)
{
    QSettings s;
    s.remove("layout/state");  // no saved dock layout: every dock starts closed
    s.setValue("layout/projectPanel", true);
    s.setValue("layout/sourcesPanel", true);
    s.setValue("layout/toolStrip", toolbar);
    s.setValue("layout/clipBar", clipBar);
    s.setValue("layout/scrubBar", scrubBar);
    s.setValue("layout/transport", transport);
    s.setValue("layout/timelineVisible", timeline && !Edition::lite);
    // What Window > Reset Layout returns to.
    s.setValue("startLayout/toolStrip", toolbar);
    s.setValue("startLayout/clipBar", clipBar);
    s.setValue("startLayout/scrubBar", scrubBar);
    s.setValue("startLayout/transport", transport);
    s.setValue("startLayout/timeline", timeline && !Edition::lite);
}

void SetupWizard::applyDefaults()
{
    QSettings s;
    if (!s.contains("layout/projectPanel")) writePanelDefaults(false, false, true, true, false);
    if (!s.contains("cacheBudgetBytes") && !s.contains("cache/preset")) s.setValue("cache/preset", 1);
    s.setValue("setup/version", kSetupVersion);
}

SetupWizard::SetupWizard(QWidget* parent)
    : QDialog(parent)
{
    setObjectName("SetupWizard");
    QSettings s;
    m_korean = s.value("ui/language", QLocale::system().language() == QLocale::Korean ? "ko" : "en").toString() == "ko";
    for (const PanelInfo& p : kPanels) m_panels[QString::fromLatin1(p.key)] = s.value(QStringLiteral("layout/") + p.key, p.def).toBool();
    // An upgrade from 1.1 keeps what was there, except that the extra tools start hidden.
    m_panels["toolStrip"] = false;
    m_panels["clipBar"] = false;
    m_panels["timelineVisible"] = false;
    m_panels["projectPanel"] = true;
    m_panels["sourcesPanel"] = true;
    m_cachePreset = std::clamp(s.value("cache/preset", 1).toInt(), 0, 2);
    m_adaptive = s.value("memory/adaptive", true).toBool();
    m_trimMinimized = s.value("memory/trimWhenMinimized", true).toBool();
    m_hardware = s.value("decode/hardware", true).toBool();
    m_updates = s.value("update/autoCheck", true).toBool();
    m_mcp = s.value("ai/mcpEnabled", true).toBool();
    m_drawingMode = s.value("drawing/mode", false).toBool();
    m_exportDir = s.value("export/dir", Platform::exportDir()).toString();
    if (m_exportDir.startsWith(QLatin1String("F:/Codex"))) m_exportDir = Platform::exportDir();  // 1.1's developer default

    resize(820, 540);
    setMinimumSize(640, 460);
    rebuild();
}

void SetupWizard::rebuild()
{
    Lang::set(m_korean ? Lang::Korean : Lang::English);
    setWindowTitle(L("Setup", "초기 설정"));  // the platform adds the product name
    delete layout();
    delete m_body;
    m_steps.clear();

    m_body = new QWidget(this);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(m_body);
    auto* h = new QHBoxLayout(m_body);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // Step list on the left (hidden on narrow screens).
    auto* side = new QWidget;
    side->setObjectName("WizardSide");
    side->setAttribute(Qt::WA_StyledBackground);
    side->setFixedWidth(200);
    auto* sv = new QVBoxLayout(side);
    sv->setContentsMargins(14, 22, 14, 18);
    sv->setSpacing(4);
    auto* brand = new QLabel(Edition::productName());
    brand->setObjectName("WizardBrand");
    sv->addWidget(brand);
    sv->addSpacing(14);
    const QStringList names = { L("Language", "언어"), L("Workspace", "작업 공간"), L("Performance", "성능"),
                                L("Files & Updates", "파일 및 업데이트"), L("Ready", "완료") };
    for (int i = 0; i < names.size(); ++i) {
        auto* l = new QLabel(QStringLiteral("%1   %2").arg(i + 1).arg(names[i]));
        l->setObjectName("WizardStep");
        m_steps << l;
        sv->addWidget(l);
    }
    sv->addStretch(1);
    auto* version = new QLabel(QStringLiteral("v%1").arg(QApplication::applicationVersion()));
    version->setObjectName("Muted");
    sv->addWidget(version);
    h->addWidget(side);
    if (width() < 600) side->hide();

    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(30, 26, 30, 20);
    rv->setSpacing(12);
    m_pages = new QStackedWidget;
    m_pages->addWidget(welcomePage());
    m_pages->addWidget(workspacePage());
    m_pages->addWidget(performancePage());
    m_pages->addWidget(filesPage());
    m_pages->addWidget(readyPage());
    rv->addWidget(m_pages, 1);

    auto* skip = new QPushButton(L("Skip (use defaults)", "건너뛰기 (기본값 사용)"));
    skip->setFlat(true);
    auto* back = new QPushButton(L("Back", "이전"));
    auto* next = new QPushButton(L("Next", "다음"));
    next->setObjectName("Primary");
    next->setDefault(true);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(skip);
    buttons->addStretch(1);
    buttons->addWidget(back);
    buttons->addWidget(next);
    rv->addLayout(buttons);
    h->addWidget(right, 1);

    connect(skip, &QPushButton::clicked, this, [this] {
        applyDefaults();
        QSettings().setValue("ui/language", m_korean ? "ko" : "en");
        QSettings().setValue("drawing/mode", m_drawingMode);
        accept();
    });
    connect(back, &QPushButton::clicked, this, [this] { go(m_page - 1); });
    connect(next, &QPushButton::clicked, this, [this] {
        if (m_page == m_pages->count() - 1) finish();
        else go(m_page + 1);
    });
    connect(m_pages, &QStackedWidget::currentChanged, this, [this, back, next, skip](int i) {
        back->setEnabled(i > 0);
        skip->setVisible(i < m_pages->count() - 1);
        next->setText(i == m_pages->count() - 1 ? L("Start", "시작하기") : L("Next", "다음"));
        for (int k = 0; k < m_steps.size(); ++k) {
            m_steps[k]->setProperty("current", k == i);
            m_steps[k]->setProperty("done", k < i);
            m_steps[k]->style()->unpolish(m_steps[k]);
            m_steps[k]->style()->polish(m_steps[k]);
        }
    });
    m_pages->setCurrentIndex(m_page);
    emit m_pages->currentChanged(m_page);
}

QLabel* SetupWizard::heading(const QString& title, const QString& subtitle, QVBoxLayout* into)
{
    auto* t = new QLabel(title);
    t->setObjectName("WizardTitle");
    t->setWordWrap(true);
    auto* s = new QLabel(subtitle);
    s->setObjectName("WizardSubtitle");
    s->setWordWrap(true);
    into->addWidget(t);
    into->addWidget(s);
    into->addSpacing(10);
    return t;
}

QWidget* SetupWizard::welcomePage()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    heading(L("Welcome to %1", "%1에 오신 것을 환영합니다").arg(Edition::productName()),
            L("Frame-accurate reference review for animators. A few questions and you are ready; everything can be changed later.",
              "애니메이터를 위한 프레임 단위 레퍼런스 리뷰 도구입니다. 몇 가지만 정하면 바로 시작할 수 있고, 모든 설정은 나중에 바꿀 수 있습니다."),
            v);
    auto* group = new QButtonGroup(page);
    for (const auto& [korean, label, hint] : { std::tuple{ false, QStringLiteral("English"), QStringLiteral("Menus and dialogs in English") },
                                               std::tuple{ true, QStringLiteral("한국어"), QStringLiteral("메뉴와 대화 상자를 한국어로 표시") } }) {
        auto* r = new QRadioButton(label);
        r->setObjectName("BigChoice");
        r->setChecked(m_korean == korean);
        group->addButton(r);
        auto* d = new QLabel(hint);
        d->setObjectName("Muted");
        d->setContentsMargins(28, 0, 0, 8);
        v->addWidget(r);
        v->addWidget(d);
        connect(r, &QRadioButton::toggled, this, [this, korean = korean](bool on) {
            if (!on || m_korean == korean) return;
            m_korean = korean;
            QTimer::singleShot(0, this, &SetupWizard::rebuild);  // not from inside the button's own signal
        });
    }
    v->addStretch(1);
    return page;
}

QWidget* SetupWizard::workspacePage()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    heading(L("Choose what is on screen", "처음 화면에 표시할 패널"),
            L("CHARONOS starts clean: Project View and the Default Timeline panel. Turn tools on now, or any time from the Window menu.",
              "CHARONOS는 프로젝트 뷰와 Default Timeline 패널만으로 깔끔하게 시작합니다. 다른 도구는 지금 켜거나, 나중에 언제든 창 메뉴에서 켜고 끌 수 있습니다."),
            v);
    for (const PanelInfo& p : kPanels) {
        if (p.fullOnly && Edition::lite) continue;
        const QString key = QString::fromLatin1(p.key);
        auto* c = new QCheckBox(L(p.en, p.ko));
        c->setChecked(m_panels.value(key));
        if (key == "projectPanel" || key == "sourcesPanel") {
            c->setEnabled(false);  // always there at start; they can still be closed later
            c->setChecked(true);
        }
        connect(c, &QCheckBox::toggled, this, [this, key](bool on) { m_panels[key] = on; });
        v->addWidget(c);
    }
    v->addSpacing(8);
    auto* hint = new QLabel(L("Tip: Window > Show All Panels brings every panel back; Window > Reset Layout returns to this start layout.",
                              "팁: 창 > 모든 패널 표시로 모든 패널을 켜고, 창 > 레이아웃 초기화로 이 시작 화면으로 돌아갑니다."));
    hint->setObjectName("Muted");
    hint->setWordWrap(true);
    v->addWidget(hint);
    auto* drawingMode = new QCheckBox(L("Drawing Mode: show annotations only on the frame where they were drawn",
                                        "드로잉 모드: 주석을 그린 프레임에서만 표시"));
    drawingMode->setChecked(m_drawingMode);
    connect(drawingMode, &QCheckBox::toggled, this, [this](bool on) { m_drawingMode = on; });
    v->addSpacing(8);
    v->addWidget(drawingMode);
    v->addStretch(1);
    return page;
}

QWidget* SetupWizard::performancePage()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    heading(L("Memory and speed", "메모리와 속도"),
            L("Decoded frames are kept in RAM so playback and scrubbing stay real-time. This PC has %1.",
              "디코딩한 프레임을 RAM에 보관해 재생과 스크럽이 끊기지 않게 합니다. 이 기기의 메모리: %1").arg(gb(Platform::totalPhysicalMemory())),
            v);
    auto* group = new QButtonGroup(page);
    const std::tuple<int, QString, QString> presets[] = {
        { 0, L("Light", "가볍게"), L("Smallest footprint; good next to other heavy programs.", "메모리를 가장 적게 씁니다. 다른 무거운 프로그램과 함께 쓸 때 좋습니다.") },
        { 1, L("Balanced (recommended)", "균형 (권장)"), L("Smooth playback for typical review work.", "일반적인 리뷰 작업에 충분히 부드럽습니다.") },
        { 2, L("High", "높게"), L("Caches long clips completely; uses much more RAM.", "긴 클립도 전부 캐시합니다. RAM을 훨씬 많이 씁니다.") },
    };
    for (const auto& [preset, label, hint] : presets) {
        auto* r = new QRadioButton(QStringLiteral("%1  ·  %2").arg(label, gb(Platform::cacheBudgetForPreset(preset))));
        r->setChecked(m_cachePreset == preset);
        group->addButton(r);
        connect(r, &QRadioButton::toggled, this, [this, p = preset](bool on) { if (on) m_cachePreset = p; });
        auto* d = new QLabel(hint);
        d->setObjectName("Muted");
        d->setContentsMargins(28, 0, 0, 6);
        v->addWidget(r);
        v->addWidget(d);
    }
    v->addSpacing(6);
    auto* adaptive = new QCheckBox(L("Adaptive memory: give cache back when the system runs low on RAM",
                                     "적응형 메모리: 시스템 RAM이 부족해지면 캐시를 자동으로 줄이기"));
    adaptive->setChecked(m_adaptive);
    connect(adaptive, &QCheckBox::toggled, this, [this](bool on) { m_adaptive = on; });
    v->addWidget(adaptive);
    auto* trim = new QCheckBox(Platform::isAndroid() ? L("Free memory when the app goes to the background", "앱이 백그라운드로 가면 메모리 비우기")
                                                     : L("Free memory while minimized", "최소화하면 메모리 비우기"));
    trim->setChecked(m_trimMinimized);
    connect(trim, &QCheckBox::toggled, this, [this](bool on) { m_trimMinimized = on; });
    v->addWidget(trim);
#ifdef Q_OS_WIN
    auto* hw = new QCheckBox(L("Hardware decoding on NVIDIA GPUs (NVDEC)", "NVIDIA GPU 하드웨어 디코딩 (NVDEC)"));
    hw->setChecked(m_hardware);
    connect(hw, &QCheckBox::toggled, this, [this](bool on) { m_hardware = on; });
    v->addWidget(hw);
#endif
    v->addStretch(1);
    return page;
}

QWidget* SetupWizard::filesPage()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    heading(L("Files and updates", "파일 및 업데이트"),
            L("Projects (.frvm) open on Windows and Android alike; keep the media next to the project to move them together.",
              "프로젝트(.frvm)는 Windows와 Android에서 그대로 열립니다. 미디어를 프로젝트 옆에 두면 함께 옮길 수 있습니다."),
            v);
    auto* key = new QLabel(L("Export folder", "내보내기 폴더"));
    key->setObjectName("FormKey");
    v->addWidget(key);
    auto* row = new QHBoxLayout;
    auto* path = new QLineEdit(m_exportDir);
    auto* browse = new QPushButton(L("Browse", "찾아보기"));
    row->addWidget(path, 1);
    row->addWidget(browse);
    v->addLayout(row);
    connect(path, &QLineEdit::textChanged, this, [this](const QString& t) { m_exportDir = t.trimmed(); });
    connect(browse, &QPushButton::clicked, this, [this, path] {
        const QString dir = QFileDialog::getExistingDirectory(this, L("Export Folder", "내보내기 폴더"), path->text());
        if (!dir.isEmpty()) path->setText(dir);
    });
    v->addSpacing(10);
    auto* updates = new QCheckBox(L("Check GitHub for a new version at startup", "시작할 때 GitHub에서 새 버전 확인"));
    updates->setChecked(m_updates);
    connect(updates, &QCheckBox::toggled, this, [this](bool on) { m_updates = on; });
    v->addWidget(updates);
    auto* uhint = new QLabel(L("If you pass on an update, CHARONOS stays quiet until the next version. Help > Check for Updates checks any time.",
                               "업데이트를 미루면 다음 버전이 나올 때까지 다시 묻지 않습니다. 도움말 > 업데이트 확인으로 언제든 확인할 수 있습니다."));
    uhint->setObjectName("Muted");
    uhint->setWordWrap(true);
    uhint->setContentsMargins(22, 0, 0, 0);
    v->addWidget(uhint);
    if (!Edition::lite) {
        auto* mcp = new QCheckBox(L("Allow AI assistants to connect (local MCP server)", "AI 어시스턴트 연결 허용 (로컬 MCP 서버)"));
        mcp->setChecked(m_mcp);
        connect(mcp, &QCheckBox::toggled, this, [this](bool on) { m_mcp = on; });
        v->addWidget(mcp);
    }
    if (Platform::isAndroid()) {
        v->addSpacing(10);
        auto* storage = new QPushButton;
        auto refresh = [storage] {
            const bool ok = Platform::hasStorageAccess();
            storage->setText(ok ? L("Storage access granted", "저장소 접근 허용됨") : L("Allow access to videos and projects...", "동영상과 프로젝트 접근 허용..."));
            storage->setEnabled(!ok);
        };
        refresh();
        connect(storage, &QPushButton::clicked, this, [refresh] {
            Platform::requestStorageAccess();
            QTimer::singleShot(1500, refresh);
        });
        auto* shint = new QLabel(L("Needed to open media from shared storage and save projects that also open on Windows.",
                                   "공유 저장소의 미디어를 열고, Windows에서도 열리는 프로젝트를 저장하려면 필요합니다."));
        shint->setObjectName("Muted");
        shint->setWordWrap(true);
        v->addWidget(storage);
        v->addWidget(shint);
    }
    v->addStretch(1);
    return page;
}

QWidget* SetupWizard::readyPage()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    heading(L("You're all set", "준비가 끝났습니다"),
            L("Drop a video or image sequence on the window, or start with File > New Project.",
              "창에 동영상이나 이미지 시퀀스를 끌어다 놓거나, 파일 > 새 프로젝트로 시작하세요."),
            v);
    const QStringList tips = {
        L("Space play / pause · ← → step frames · I / O set In / Out", "Space 재생 / 정지 · ← → 프레임 이동 · I / O 구간 지정"),
        L("Window menu: show or hide every panel and tool", "창 메뉴: 모든 패널과 도구 표시 / 숨기기"),
        L("Compare button: pick A and B, then choose a layout", "비교 버튼: A와 B를 고른 뒤 보기 방식을 선택"),
        L("F1 lists every keyboard shortcut", "F1: 모든 단축키 보기"),
    };
    for (const QString& t : tips) {
        auto* l = new QLabel(QStringLiteral("•  ") + t);
        l->setWordWrap(true);
        v->addWidget(l);
    }
    auto* tutorial = new QPushButton(L("Open User Guide (HTML)", "사용 설명서 열기 (HTML)"));
    connect(tutorial, &QPushButton::clicked, this, [this] {
        const QString path = QDir(QApplication::applicationDirPath()).filePath(QStringLiteral("docs/CHARONOS-User-Guide.html"));
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    v->addWidget(tutorial, 0, Qt::AlignLeft);
    v->addStretch(1);
    return page;
}

void SetupWizard::go(int page)
{
    m_page = std::clamp(page, 0, m_pages->count() - 1);
    m_pages->setCurrentIndex(m_page);
}

void SetupWizard::finish()
{
    QSettings s;
    s.setValue("ui/language", m_korean ? "ko" : "en");
    s.setValue("drawing/mode", m_drawingMode);
    writePanelDefaults(m_panels.value("toolStrip"), m_panels.value("clipBar"), m_panels.value("scrubBar", true),
                       m_panels.value("transport", true), m_panels.value("timelineVisible"));
    s.remove("cacheBudgetBytes");  // a preset from now on (Tools > Cache Memory can set GB again)
    s.setValue("cache/preset", m_cachePreset);
    s.setValue("memory/adaptive", m_adaptive);
    s.setValue("memory/trimWhenMinimized", m_trimMinimized);
    s.setValue("decode/hardware", m_hardware);
    s.setValue("update/autoCheck", m_updates);
    if (!Edition::lite) s.setValue("ai/mcpEnabled", m_mcp);
    if (!m_exportDir.isEmpty()) {
        QDir().mkpath(m_exportDir);
        s.setValue("export/dir", m_exportDir);
    }
    s.setValue("setup/version", kSetupVersion);
    accept();
}
