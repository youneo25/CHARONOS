#include "MainWindow.h"
#include "Lang.h"

#include "Icons.h"
#include "Palette.h"
#include "export/Exporter.h"
#include "media/ImageSequenceSource.h"
#include "model/AnnotationPainter.h"
#include "ui/ABDelegate.h"
#include "ui/ExportDialog.h"
#include "ui/FrameExportDialog.h"
#include "ui/ConsoleDock.h"
#include "ui/HotkeyManager.h"
#include "media/CanvasSource.h"
#include "media/VideoDecoder.h"
#include "ai/McpServer.h"
#include "ui/MotionGraph.h"
#include "ui/TimelineEditor.h"
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QDebug>
#include "ui/StrokePanel.h"
#include "track/Tracker.h"
#include "ui/Timeline.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QRegularExpression>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QMimeData>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageWriter>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <atomic>

#include <windows.h>

namespace {

QString formatBytes(qint64 bytes)
{
    const double gb = bytes / double(qint64(1) << 30);
    if (gb >= 1.0) return QStringLiteral("%1 GB").arg(gb, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / double(1 << 20), 0, 'f', 0);
}

QString timecode(int64_t frame, double fps)
{
    const int f = std::max(1, int(std::round(fps)));
    const int64_t secs = frame / f;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(secs / 3600, 2, 10, QChar('0'))
        .arg((secs / 60) % 60, 2, 10, QChar('0'))
        .arg(secs % 60, 2, 10, QChar('0'))
        .arg(int(frame % f), 2, 10, QChar('0'));
}

QIcon swatchIcon(const QColor& c)
{
    QPixmap pm(QSize(20, 20) * 2);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 60), 1));
    p.setBrush(c);
    p.drawRoundedRect(QRectF(2.5, 2.5, 15, 15), 4, 4);
    return QIcon(pm);
}

// Built on use (not at static-init time) so they follow the UI language.
QString mediaFilter()
{
    return L("Media", "미디어") +
           QStringLiteral(" (*.mp4 *.mov *.m4v *.mkv *.avi *.webm *.mxf *.wmv *.mpg *.mpeg *.gif "
                          "*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.tga);;") +
           L("All Files", "모든 파일") + QStringLiteral(" (*.*)");
}

QString projectFilter()
{
    return L("CHARONOS Project (*.frvm);;Older Project (*.json);;All Files (*.*)",
             "CHARONOS 프로젝트 (*.frvm);;이전 버전 프로젝트 (*.json);;모든 파일 (*.*)");
}

QList<QPair<QString, QColor>> noteColors()
{
    return {
        { L("Red", "빨강"), QColor(0xe5, 0x48, 0x3e) },  { L("Orange", "주황"), QColor(0xf0, 0x8c, 0x2e) },
        { L("Yellow", "노랑"), QColor(0xf2, 0xc9, 0x4c) },  { L("Green", "초록"), QColor(0x4c, 0xbb, 0x6c) },
        { L("Blue", "파랑"), QColor(0x3d, 0x8b, 0xfd) },  { L("Purple", "보라"), QColor(0x9b, 0x6b, 0xf2) },
        { L("White", "흰색"), QColor(0xf5, 0xf4, 0xef) },  { L("Black", "검정"), QColor(0x1a, 0x1a, 0x19) },
    };
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    QSettings settings;
    setObjectName("MainWindow");
    m_realtime = settings.value("realtimePlayback", false).toBool();
    {
        // Keep the cache inside physical RAM even if the setting came from a bigger machine.
        const qint64 ram = totalPhysicalMemory();
        const qint64 saved = settings.value("cacheBudgetBytes", ram / 2).toLongLong();
        m_media.setCacheBudget(std::clamp<qint64>(saved, qint64(256) << 20, std::max<qint64>(qint64(512) << 20, ram - (qint64(2) << 30))));
    }
    VideoDecoder::setHardwareDecodingEnabled(settings.value("decode/hardware", true).toBool());

    m_endlessScrubbing = settings.value("endlessScrubbing", true).toBool();
    m_view = new Viewport;
    m_timeline = new Timeline;
    m_clipBar = new ClipBar;
    m_view->setEndlessScrubbing(m_endlessScrubbing);
    m_timeline->setEndlessScrubbing(m_endlessScrubbing);

    auto* viewerColumn = new QWidget;
    auto* vc = new QVBoxLayout(viewerColumn);
    vc->setContentsMargins(0, 0, 0, 0);
    vc->setSpacing(0);
    vc->addWidget(m_clipBar);
    vc->addWidget(m_view, 1);
    vc->addWidget(m_timeline);

    m_toast = new QLabel(m_view);
    m_toast->setStyleSheet(QStringLiteral("background: rgba(38,38,36,235); color: %1; border: 1px solid %2; border-radius: 8px; padding: 7px 12px;")
                               .arg(Pal::text.name(), Pal::border.name()));
    m_toast->hide();
    m_toastTimer.setSingleShot(true);
    connect(&m_toastTimer, &QTimer::timeout, m_toast, &QLabel::hide);

    auto* left = new QSplitter(Qt::Vertical);
    m_leftSplit = left;
    left->setObjectName("LeftPanels");
    left->setChildrenCollapsible(false);
    left->addWidget(buildProjectPanel());
    left->addWidget(buildTimelinePanel());
    for (QWidget* panel : {m_projectPanel, m_timelinePanel}) {
        QList<QWidget*> targets = panel->findChildren<QWidget*>();
        targets.append(panel);
        for (QWidget* target : targets) {
            target->setAcceptDrops(true);
            target->installEventFilter(this);
        }
    }
    left->setStretchFactor(0, 1);
    left->setStretchFactor(1, 1);

    auto* main = new QSplitter(Qt::Horizontal);
    m_mainSplit = main;
    main->setObjectName("MainSplit");
    main->setChildrenCollapsible(false);
    main->addWidget(left);
    auto* strip = buildToolStrip();
    auto* right = new QWidget;
    auto* rh = new QHBoxLayout(right);
    rh->setContentsMargins(0, 0, 0, 0);
    rh->setSpacing(0);
    rh->addWidget(strip);
    rh->addWidget(viewerColumn, 1);
    main->addWidget(right);
    main->setStretchFactor(0, 0);
    main->setStretchFactor(1, 1);
    main->setSizes({ 260, 1300 });
    connect(main, &QSplitter::splitterMoved, this, [left] { left->setVisible(left->width() > 0); });

    auto* root = new QWidget;
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(main, 1);
    m_bottomBar = buildBottomBar();
    rootLayout->addWidget(m_bottomBar);
    setCentralWidget(root);
    setDockNestingEnabled(true);

    m_console = new ConsoleDock(this);
    m_console->setObjectName("ConsoleDock");
    addDockWidget(Qt::BottomDockWidgetArea, m_console);
    m_console->hide();
    connect(m_console, &ConsoleDock::commandEntered, this, &MainWindow::runConsoleCommand);
    buildTimelineEditor();
    buildNotesPanel();
    buildMotionGraph();
    buildMenus();

    connect(&m_media, &MediaLibrary::frameReady, this, &MainWindow::onFrameReady);
    connect(&m_media, &MediaLibrary::decodeFailed, this, [this](const QString&, qint64 index, const QString& msg) {
        showToast(L("Frame %1 failed to decode: %2", "프레임 %1 디코딩 실패: %2").arg(index).arg(msg), 4000);
    });
    connect(m_timeline, &Timeline::scrubbed, this, [this](qint64 f) {
        if (m_playing) stopPlayback();
        goToFrame(f);
        scrubSound();
    });
    connect(m_timeline, &Timeline::rateClicked, this, [this] {
        bool ok = false;
        const double fps = QInputDialog::getDouble(this, L("Playback FPS", "재생 FPS"), L("Frames per second", "초당 프레임"), viewFps(), 1, 240, 3, &ok);
        if (ok) setViewFps(fps);
    });
    connect(m_clipBar, &ClipBar::scrubbed, this, [this](qint64 local) {
        const Loc loc = locate(m_current);
        if (!loc) return;
        if (m_playing) stopPlayback();
        goToFrame(loc.segment->start + local);
    });
    connect(m_view, &Viewport::annotationCreated, this, &MainWindow::onAnnotationCreated);
    connect(m_view, &Viewport::editStarted, this, &MainWindow::stopPlayback);
    connect(m_view, &Viewport::eraseBegin, this, [this] { m_eraseOpen = false; });
    connect(m_view, &Viewport::eraseAt, this, &MainWindow::onEraseAt);
    connect(m_view, &Viewport::eraseEnd, this, [this] {
        if (m_eraseOpen) commitEdit();
        m_eraseOpen = false;
    });
    connect(m_view, &Viewport::annotationsEdited, this, &MainWindow::onAnnotationsEdited);
    connect(m_view, &Viewport::annotationsRemoved, this, &MainWindow::removeAnnotations);
    connect(m_view, &Viewport::trackPlaced, this, &MainWindow::placeTrack);
    connect(m_view, &Viewport::trackMoved, this, &MainWindow::moveTrack);
    connect(m_view, &Viewport::trackSelected, this, [this](const QString& id) {
        m_selectedTrackId = id;
        refreshAnnotations();
        refreshMotionGraph();
    });
    connect(m_view, &Viewport::filesDropped, this, &MainWindow::openPaths);
    connect(m_view, &Viewport::scrubBy, this, &MainWindow::scrubRelative);
    connect(m_timeline, &Timeline::scrubBy, this, &MainWindow::scrubRelative);
    connect(m_view, &Viewport::contextMenuAt, this, &MainWindow::showViewportMenu);
    connect(m_view, &Viewport::activeSideChanged, this, [this] {
        m_selectedTrackId.clear();  // tracks belong to one clip
        refreshAnnotations();
        refreshMotionGraph();
        if (m_notesDock && m_notesDock->isVisible()) syncNoteEditor();
    });
    connect(m_timeline, &Timeline::contextMenuAt, this, [this](qint64 f, QPoint g) { showTimelineMenu(f, g); });
    connect(m_clipBar, &ClipBar::contextMenuAt, this, [this](qint64 local, QPoint g) {
        const Loc loc = locate(m_current);
        if (loc) showTimelineMenu(loc.segment->start + local, g);
    });
    connect(m_view, &Viewport::zoomChanged, this, [this](double s) {
        m_zoomBtn->setText(QStringLiteral("%1%").arg(qRound(s * 100)));
    });

    m_playTimer.setTimerType(Qt::PreciseTimer);
    m_playTimer.setInterval(4);
    connect(&m_playTimer, &QTimer::timeout, this, &MainWindow::onPlayTick);

    m_cacheTimer.setInterval(250);
    connect(&m_cacheTimer, &QTimer::timeout, this, [this] {
        if (m_seq.total <= 0) return;
        QBitArray mask(m_seq.total);
        for (const Sequence::Segment& s : m_seq.segments) {
            FrameServer* srv = m_media.server(s.sourceId);
            if (!srv) continue;
            const QBitArray part = srv->cachedMask();
            for (qsizetype i = 0; i < s.count; ++i) {
                const int64_t f = s.sourceFrame(i);
                if (f >= 0 && f < part.size() && part.testBit(qsizetype(f))) mask.setBit(qsizetype(s.start + i));
            }
        }
        m_timeline->setCacheMask(mask);
        if (m_editor && m_editorDock && m_editorDock->isVisible() && m_soloId.isEmpty()) m_editor->setCacheMask(mask);
        if (m_playing) m_timeline->setRates(viewFps(), m_measuredFps);
    });
    m_cacheTimer.start();

    m_trackPoll.setInterval(40);
    connect(&m_trackPoll, &QTimer::timeout, this, &MainWindow::pollTracking);

    m_continueDrawing = settings.value("continueDrawing", false).toBool();
    m_onion = settings.value("onion/on", false).toBool();
    m_onionMode = std::clamp(settings.value("onion/mode", 0).toInt(), 0, 1);
    m_onionBefore = std::clamp(settings.value("onion/before", 2).toInt(), 0, 8);
    m_onionAfter = std::clamp(settings.value("onion/after", 1).toInt(), 0, 8);
    {
        // 0.0.1 had one "video frames" count for both directions.
        const int legacy = std::clamp(settings.value("onion/video", 0).toInt(), 0, 8);
        m_onionVideoBefore = std::clamp(settings.value("onion/videoBefore", legacy).toInt(), 0, 8);
        m_onionVideoAfter = std::clamp(settings.value("onion/videoAfter", legacy).toInt(), 0, 8);
    }
    m_onionStep = std::clamp(settings.value("onion/step", 1).toInt(), 1, 12);
    m_onionOpacity = std::clamp(settings.value("onion/opacity", 0.45).toDouble(), 0.05, 0.9);
    m_onionTint = settings.value("onion/tint", true).toBool();
    m_onionWhilePlaying = settings.value("onion/whilePlaying", false).toBool();
    if (const QColor c(settings.value("onion/beforeColor").toString()); c.isValid()) m_onionBeforeColor = c;
    if (const QColor c(settings.value("onion/afterColor").toString()); c.isValid()) m_onionAfterColor = c;
    populateOnionMenu();
    m_textSize = settings.value("textSize", 36.0).toDouble();
    m_brushOpacity = settings.value("brushOpacity", 1.0).toDouble();
    m_view->setTextSize(m_textSize);
    m_onionBtn->setChecked(m_onion);
    setVolume(settings.value("audio/volume", 0.8).toDouble());
    setMuted(settings.value("audio/muted", false).toBool());

    m_penStyle = static_cast<Tool>(std::clamp(settings.value("penStyle", int(Tool::Pencil)).toInt(), int(Tool::Pencil), int(Tool::Highlighter)));
    setTool(Tool::Select);
    setColor(QColor::fromString(settings.value("noteColor", m_color.name()).toString()));
    setBrushSize(settings.value("brushSize", 3.0).toDouble());
    m_view->setPlaceholder(L("Drop media or start File > New Project", "미디어를 놓거나 파일 > 새 프로젝트로 시작하세요"));
    resize(1600, 940);
    m_defaultState = saveState(kLayoutVersion);  // every dock hidden: Window > Reset Layout
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreLayout();
    refreshProjectTree();
    refreshClipList();
    refreshProperties();
    updateTitle();
    updateReadout();
    updateHud();

    // Built-in MCP endpoint for AI assistants (local named pipe, this Windows user only).
    m_mcp = new McpServer(this);
    if (settings.value("ai/mcpEnabled", true).toBool()) m_mcp->start();
    if (m_mcpAction) m_mcpAction->setChecked(m_mcp->isListening());
}

MainWindow::~MainWindow()
{
    if (m_mcp) m_mcp->stop();
    *m_audioCancel = true;  // long soundtrack decodes stop instead of holding up the exit
    if (m_trackJob) m_trackJob->cancel = true;
    if (m_trackThread) {
        m_trackThread->wait();
        delete m_trackThread;
    }
    m_playTimer.stop();
    m_shuttleTimer.stop();
    m_audio.stop();
    for (QThread* t : m_audioThreads) {
        t->wait();
        delete t;
    }
    for (QThread* t : m_plateThreads) {
        t->wait();
        delete t;
    }
    m_media.clear();
}

// ================================================================== UI construction

QToolButton* MainWindow::iconButton(const QString& icon, const QString& tip, int box, int iconPx)
{
    auto* b = new QToolButton;
    b->setIcon(Icons::icon(icon));
    b->setIconSize(QSize(iconPx, iconPx));
    b->setFixedSize(box, box);
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);
    b->setAutoRaise(true);
    return b;
}

QWidget* MainWindow::panelHeader(const QString& title, QWidget* panel, QLabel** titleLabel)
{
    auto* header = new QWidget;
    header->setObjectName("PanelHeader");
    header->setAttribute(Qt::WA_StyledBackground);
    header->setFixedHeight(30);
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(10, 0, 4, 0);
    auto* label = new QLabel(title);
    label->setObjectName("PanelTitle");
    auto* close = iconButton("close", L("Close Panel", "패널 닫기"), 22, 14);
    connect(close, &QToolButton::clicked, panel, &QWidget::hide);
    h->addSpacing(22);
    h->addStretch(1);
    h->addWidget(label);
    h->addStretch(1);
    h->addWidget(close);
    if (titleLabel) *titleLabel = label;
    return header;
}

QWidget* MainWindow::buildProjectPanel()
{
    m_projectPanel = new QWidget;
    m_projectPanel->setObjectName("Panel");
    m_projectPanel->setAttribute(Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(m_projectPanel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(panelHeader(QStringLiteral("Project View"), m_projectPanel));

    m_tree = new QTreeWidget;
    m_tree->setColumnCount(1);
    m_tree->setHeaderLabels({ L("Name", "이름") });
    m_tree->setItemDelegate(makeABDelegate(m_tree));
    m_tree->setIndentation(14);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop); // file imports are handled by eventFilter
    m_tree->setDefaultDropAction(Qt::CopyAction);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString kind = item->data(0, RoleKind).toString();
        if (kind == "source") setSolo(item->data(0, RoleId).toString());
        if (kind == "timeline") setSolo(QString());
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_tree->itemAt(pos);
        const QString kind = item ? item->data(0, RoleKind).toString() : QString();
        QMenu menu(this);
        if (kind == "folder") {
            const QString name = item->data(0, Qt::UserRole + 2).toString();
            menu.addAction(L("Rename Folder...", "폴더 이름 변경..."), this, [this, name] {
                bool ok; const QString next = QInputDialog::getText(this, L("Folder Name", "폴더 이름"), QString(), QLineEdit::Normal, name, &ok).trimmed();
                if (!ok || next.isEmpty() || m_project.folders.contains(next)) return;
                m_project.folders[m_project.folders.indexOf(name)] = next;
                for (auto& s : m_project.sources) if (s.folder == name) s.folder = next;
                setDirty(true); refreshProjectTree();
            });
            menu.addAction(L("Remove Folder (keep sources)", "폴더 제거 (소스 유지)"), this, [this, name] {
                m_project.folders.removeAll(name);
                for (auto& s : m_project.sources) if (s.folder == name) s.folder.clear();
                setDirty(true); refreshProjectTree();
            });
        } else if (kind == "source") {
            addSourceActions(menu, item->data(0, RoleId).toString(), -1);
        } else if (kind == "timeline") {
            menu.addAction(Icons::icon("film"), L("View Timeline", "타임라인 보기"), this, [this] { setSolo(QString()); });
            menu.addAction(L("Rename...", "이름 바꾸기..."), this, [this] {
                bool ok = false;
                const QString name = QInputDialog::getText(this, L("Timeline Name", "타임라인 이름"), QString(), QLineEdit::Normal,
                                                           m_project.timeline.name, &ok);
                if (!ok || name.trimmed().isEmpty()) return;
                m_project.timeline.name = name.trimmed();
                m_timelineTitle->setText(m_project.timeline.name);
                setDirty(true);
                refreshProjectTree();
                updateTitle();
            });
        } else {
            menu.addAction(Icons::icon("import"), L("Import Media...", "미디어 가져오기..."), this, &MainWindow::importDialog);
            menu.addAction(L("Open Project...", "프로젝트 열기..."), this, &MainWindow::openProjectDialog);
        }
        menu.addSeparator();
        menu.addAction(L("New Folder...", "새 폴더..."), this, &MainWindow::addFolder);
        menu.addAction(L("Import Folder...", "폴더 가져오기..."), this, &MainWindow::importFolderDialog);
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });
    v->addWidget(m_tree, 1);

    auto* footer = new QWidget;
    footer->setObjectName("PanelFooter");
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* f = new QHBoxLayout(footer);
    f->setContentsMargins(6, 3, 6, 3);
    f->setSpacing(2);
    auto* imp = iconButton("import", L("Import Media", "미디어 가져오기"), 26, 16);
    connect(imp, &QToolButton::clicked, this, &MainWindow::importDialog);
    auto* addTl = iconButton("timeline-add", L("Add to Timeline", "타임라인에 추가"), 26, 16);
    connect(addTl, &QToolButton::clicked, this, [this] {
        QStringList ids;
        for (QTreeWidgetItem* it : m_tree->selectedItems())
            if (it->data(0, RoleKind).toString() == "source") ids << it->data(0, RoleId).toString();
        if (!ids.isEmpty()) insertSources(ids, -1);
    });
    auto* cmp = iconButton("compare", L("Compare as B", "B로 비교"), 26, 16);
    connect(cmp, &QToolButton::clicked, this, [this] {
        for (QTreeWidgetItem* it : m_tree->selectedItems())
            if (it->data(0, RoleKind).toString() == "source") return setCompareSource(it->data(0, RoleId).toString());
    });
    auto* del = iconButton("trash", L("Remove from Project", "프로젝트에서 제거"), 26, 16);
    connect(del, &QToolButton::clicked, this, [this] {
        QStringList ids;
        for (QTreeWidgetItem* it : m_tree->selectedItems())
            if (it->data(0, RoleKind).toString() == "source") ids << it->data(0, RoleId).toString();
        for (const QString& id : ids) removeSource(id);
    });
    f->addWidget(imp);
    f->addWidget(addTl);
    f->addWidget(cmp);
    f->addStretch(1);
    f->addWidget(del);
    v->addWidget(footer);
    return m_projectPanel;
}

QWidget* MainWindow::buildTimelinePanel()
{
    m_timelinePanel = new QWidget;
    m_timelinePanel->setObjectName("Panel");
    m_timelinePanel->setAttribute(Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(m_timelinePanel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(panelHeader(m_project.timeline.name, m_timelinePanel, &m_timelineTitle));

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_clipList = new QListWidget;
    // Also allow copying a source out to the timeline canvas. Same-list drops still move.
    m_clipList->setDragDropMode(QAbstractItemView::DragDrop);
    m_clipList->setDefaultDropAction(Qt::MoveAction);
    m_clipList->setFocusPolicy(Qt::NoFocus);
    m_clipList->setIconSize(QSize(16, 16));
    m_clipList->setItemDelegate(makeABDelegate(m_clipList));
    m_clipList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_clipList->setTextElideMode(Qt::ElideMiddle);
    m_clipList->setResizeMode(QListView::Adjust);  // re-layout rows to the new width
    connect(m_clipList->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        if (m_fillingClipList) return;
        QStringList order;
        for (int i = 0; i < m_clipList->count(); ++i) order << m_clipList->item(i)->data(RoleClip).toString();
        // Deferred: the list is still inside its own drop handling here. The new order packs the
        // clips of V1 back to back (clips on other tracks keep their place).
        QTimer::singleShot(0, this, [this, order] {
            QStringList v1;
            for (const QString& uid : order)
                if (const TimelineClip* c = m_project.timeline.clip(uid); c && c->track == 0) v1 << uid;
            editTimeline([&](TimelineDoc& doc) { return tl::reorderTrack(doc, v1, lengthFn()); }, L("Reorder Clips", "클립 순서"));
        });
    });
    connect(m_clipList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!m_soloId.isEmpty()) setSolo(QString());
        if (const TimelineClip* c = m_project.timeline.clip(item->data(RoleClip).toString())) goToFrame(tl::span(*c, lengthFn()).start);
        if (m_editor) {
            m_editor->setSelectedClip(item->data(RoleClip).toString());
            if (m_editorDock && m_editorDock->isVisible()) m_editor->setFocus(Qt::MouseFocusReason);
        }
    });
    m_clipList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_clipList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_clipList->itemAt(pos);
        QMenu menu(this);
        if (item)
            addSourceActions(menu, item->data(RoleId).toString(), m_clipList->row(item), item->data(RoleClip).toString());
        else
            menu.addAction(Icons::icon("import"), L("Import Media...", "미디어 가져오기..."), this, &MainWindow::importDialog);
        menu.addSeparator();
        menu.addAction(L("Import Folder...", "폴더 가져오기..."), this, &MainWindow::importFolderDialog);
        menu.addAction(L("New Folder...", "새 폴더..."), this, &MainWindow::addFolder);
        menu.addAction(L("New Project...", "새 프로젝트..."), this, &MainWindow::newProject);
        menu.exec(m_clipList->viewport()->mapToGlobal(pos));
    });
    m_tabs->addTab(m_clipList, L("Sources", "소스"));

    m_props = new QWidget;
    new QFormLayout(m_props);
    m_tabs->addTab(m_props, L("Properties", "속성"));
    v->addWidget(m_tabs, 1);

    auto* footer = new QWidget;
    footer->setObjectName("PanelFooter");
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* f = new QHBoxLayout(footer);
    f->setContentsMargins(6, 3, 6, 3);
    f->setSpacing(2);
    auto move = [this](int delta) {
        QListWidgetItem* item = m_clipList->currentItem();
        if (!item) return;
        const QString uid = item->data(RoleClip).toString();
        if (editTimeline([&](TimelineDoc& doc) { return tl::swapNeighbour(doc, uid, delta, lengthFn()); }, L("Reorder Clips", "클립 순서")))
            for (int i = 0; i < m_clipList->count(); ++i)
                if (m_clipList->item(i)->data(RoleClip).toString() == uid) m_clipList->setCurrentRow(i);
    };
    auto* up = iconButton("up", L("Move Up", "위로"), 26, 16);
    auto* down = iconButton("down", L("Move Down", "아래로"), 26, 16);
    connect(up, &QToolButton::clicked, this, [move] { move(-1); });
    connect(down, &QToolButton::clicked, this, [move] { move(1); });
    auto* sort = iconButton("sort", L("Sort by Name", "이름순 정렬"), 26, 16);
    connect(sort, &QToolButton::clicked, this, [this] {
        QCollator col;
        col.setNumericMode(true);
        auto nameOf = [this](const QString& uid) {
            const TimelineClip* c = m_project.timeline.clip(uid);
            const Source* s = c ? m_project.find(c->sourceId) : nullptr;
            return s ? s->displayName() : QString();
        };
        QStringList v1;
        for (const QString& uid : videoClipOrder())
            if (const TimelineClip* c = m_project.timeline.clip(uid); c && c->track == 0) v1 << uid;
        std::stable_sort(v1.begin(), v1.end(), [&](const QString& a, const QString& b) {
            const int byName = col.compare(nameOf(a), nameOf(b));
            return byName != 0 ? byName < 0 : m_project.timeline.clip(a)->in < m_project.timeline.clip(b)->in;
        });
        editTimeline([&](TimelineDoc& doc) { return tl::reorderTrack(doc, v1, lengthFn()); }, L("Sort by Name", "이름순 정렬"));
    });
    auto* remove = iconButton("trash", L("Remove from Timeline", "타임라인에서 빼기"), 26, 16);
    connect(remove, &QToolButton::clicked, this, [this] {
        if (QListWidgetItem* item = m_clipList->currentItem()) removeClip(item->data(RoleClip).toString());
    });
    auto* folder = iconButton("folder", L("Import Folder (right-click: New Folder)", "폴더 가져오기 (우클릭: 새 폴더)"), 26, 16);
    folder->setObjectName("SourcesFolderButton");
    connect(folder, &QToolButton::clicked, this, &MainWindow::importFolderDialog);
    folder->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(folder, &QToolButton::customContextMenuRequested, this, [this] { addFolder(); });
    f->addWidget(folder);
    f->addWidget(up);
    f->addWidget(down);
    f->addWidget(sort);
    f->addStretch(1);
    f->addWidget(remove);
    v->addWidget(footer);
    return m_timelinePanel;
}

QWidget* MainWindow::buildToolStrip()
{
    m_toolStrip = new QWidget;
    m_toolStrip->setObjectName("ToolStrip");
    m_toolStrip->setAttribute(Qt::WA_StyledBackground);
    m_toolStrip->setFixedWidth(42);
    auto* v = new QVBoxLayout(m_toolStrip);
    v->setContentsMargins(5, 8, 5, 8);
    v->setSpacing(3);

    const QList<std::tuple<Tool, QString, QString>> tools = {
        { Tool::Select, "cursor", L("Select  V", "선택  V") },
        { Tool::Pencil, "pencil", L("Pencil  P", "펜슬  P") },
        { Tool::Eraser, "eraser", L("Eraser  E", "지우개  E") }, { Tool::Text, "text", L("Text  T", "텍스트  T") },
        { Tool::Arrow, "arrow", L("Arrow  A", "화살표  A") },   { Tool::Line, "line", L("Line  L", "직선  L") },
        { Tool::Rect, "rect", L("Rectangle  R", "사각형  R") },     { Tool::Circle, "ellipse", L("Ellipse  C", "원  C") },
        { Tool::Track, "target", L("Tracker  M", "트래커  M") },
    };
    for (const auto& [tool, icon, tip] : tools) {
        auto* b = iconButton(icon, tip, 32, 18);
        b->setCheckable(true);
        const Tool t = tool;
        connect(b, &QToolButton::clicked, this, [this, t] { setTool(t == Tool::Pencil ? m_penStyle : t); });
        if (t == Tool::Pencil) {
            m_penButton = b;
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QToolButton::customContextMenuRequested, this, [this, b](QPoint pos) {
                QMenu menu(this);
                for (const auto& [style, name, icon] : {
                    std::tuple{Tool::Pencil, L("Pencil", "연필"), QString("pencil")},
                    std::tuple{Tool::Pen, L("Pen", "펜"), QString("pen")},
                    std::tuple{Tool::Highlighter, L("Highlighter", "형광펜"), QString("highlighter")} }) {
                    auto* action = menu.addAction(Icons::icon(icon), name, this, [this, style] { setTool(style); });
                    action->setCheckable(true); action->setChecked(m_penStyle == style);
                }
                menu.exec(b->mapToGlobal(pos));
            });
        }
        m_toolButtons[tool] = b;
        v->addWidget(b);
    }
    v->addSpacing(6);

    m_swatch = new QToolButton;
    m_swatch->setObjectName("Swatch");
    m_swatch->setFixedSize(32, 32);
    m_swatch->setIconSize(QSize(20, 20));
    m_swatch->setToolTip(L("Annotation Color", "주석 색상"));
    m_swatch->setFocusPolicy(Qt::NoFocus);
    m_swatch->setPopupMode(QToolButton::InstantPopup);
    auto* colorMenu = new QMenu(m_swatch);
    for (const auto& [name, color] : noteColors()) {
        const QColor c = color;
        colorMenu->addAction(swatchIcon(c), name, this, [this, c] { setColor(c); });
    }
    colorMenu->addSeparator();
    colorMenu->addAction(L("More Colors...", "다른 색상..."), this, [this] {
        const QColor c = QColorDialog::getColor(m_color, this, L("Annotation Color", "주석 색상"));
        if (c.isValid()) setColor(c);
    });
    m_swatch->setMenu(colorMenu);
    v->addWidget(m_swatch);

    // Free values for width / opacity / text size. With a selection the values apply to it.
    auto* width = iconButton("width", L("Stroke", "선 설정"), 32, 18);
    m_strokePanel = new StrokePanel(this);
    connect(width, &QToolButton::clicked, this, [this, width] {
        double w = m_brushSize, o = m_brushOpacity, t = m_textSize;
        if (const Annotation* a = firstSelected()) {
            w = a->size;
            o = a->opacity;
            if (a->type == "text") t = a->fontSize;
        }
        m_strokePanel->setValues(w, o, t);
        m_strokePanel->popupBelow(width);
    });
    connect(m_strokePanel, &StrokePanel::widthChanged, this, [this](double v) {
        if (!applyToSelection([v](Annotation& a) { a.size = v; })) setBrushSize(v);
    });
    connect(m_strokePanel, &StrokePanel::opacityChanged, this, [this](double v) {
        if (applyToSelection([v](Annotation& a) { a.opacity = v; })) return;
        m_brushOpacity = v;
        m_view->setBrush(m_color, m_brushSize, m_brushOpacity);
        QSettings().setValue("brushOpacity", v);
    });
    connect(m_strokePanel, &StrokePanel::textSizeChanged, this, [this](double v) {
        if (applyToSelection([v](Annotation& a) { if (a.type == "text") a.fontSize = v / a.scale; })) return;
        m_textSize = v;
        m_view->setTextSize(v);
        QSettings().setValue("textSize", v);
    });
    connect(m_strokePanel, &StrokePanel::closed, this, [this] {
        if (m_pending) commitEdit();  // one undo step for the whole panel session
    });
    v->addWidget(width);

    m_onionBtn = iconButton("onion", L("Onion Skin  Shift+O", "어니언 스킨  Shift+O"), 32, 18);
    m_onionBtn->setCheckable(true);
    connect(m_onionBtn, &QToolButton::clicked, this, [this](bool on) { setOnion(on); });
    m_onionMenu = new QMenu(m_onionBtn);
    m_onionMenu->setObjectName("OnionMenu");
    m_onionBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_onionBtn, &QToolButton::customContextMenuRequested, this, [this] {
        m_onionMenu->exec(m_onionBtn->mapToGlobal(QPoint(m_onionBtn->width() + 4, 0)));
    });
    m_onionBtn->setToolTip(L("Onion Skin  Shift+O · right-click: settings", "어니언 스킨  Shift+O · 우클릭: 설정"));
    v->addWidget(m_onionBtn);
    v->addSpacing(6);

    auto* undoBtn = iconButton("undo", L("Undo  Ctrl+Z", "실행 취소  Ctrl+Z"), 32, 18);
    auto* redoBtn = iconButton("redo", L("Redo  Ctrl+Shift+Z", "다시 실행  Ctrl+Shift+Z"), 32, 18);
    auto* clearBtn = iconButton("clear", L("Clear Frame  Shift+Del", "현재 프레임 비우기  Shift+Del"), 32, 18);
    auto* trashBtn = iconButton("trash", L("Clear Clip Annotations", "클립 주석 모두 삭제"), 32, 18);
    connect(undoBtn, &QToolButton::clicked, this, &MainWindow::undo);
    connect(redoBtn, &QToolButton::clicked, this, &MainWindow::redo);
    connect(clearBtn, &QToolButton::clicked, this, &MainWindow::clearCurrentFrame);
    connect(trashBtn, &QToolButton::clicked, this, &MainWindow::clearAllInClip);
    v->addWidget(undoBtn);
    v->addWidget(redoBtn);
    v->addWidget(clearBtn);
    v->addWidget(trashBtn);
    v->addStretch(1);
    return m_toolStrip;
}

QWidget* MainWindow::buildBottomBar()
{
    auto* bar = new QWidget;
    bar->setObjectName("BottomBar");
    bar->setAttribute(Qt::WA_StyledBackground);
    bar->setFixedHeight(46);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(8, 0, 10, 0);
    h->setSpacing(2);

    // left: panel toggles + compare
    auto* sidebar = iconButton("sidebar", L("Project Panel", "프로젝트 패널"), 30, 18);
    connect(sidebar, &QToolButton::clicked, this, [this] {
        const bool show = !(m_projectPanel->isVisible() || m_timelinePanel->isVisible());
        m_projectPanel->setVisible(show);
        m_timelinePanel->setVisible(show);
        m_projectPanel->parentWidget()->setVisible(show);
    });
    auto* strip = iconButton("toolbar", L("Annotation Toolbar", "주석 도구 막대"), 30, 18);
    connect(strip, &QToolButton::clicked, this, [this] { m_toolStrip->setVisible(!m_toolStrip->isVisible()); });
    m_compareBtn = iconButton("compare", L("Compare", "비교"), 30, 18);
    m_compareBtn->setCheckable(true);
    m_compareBtn->setPopupMode(QToolButton::InstantPopup);
    auto* cmpMenu = new QMenu(m_compareBtn);
    const QList<std::tuple<CompareMode, QString, QString>> modes = {
        { CompareMode::Off, "", L("Compare Off", "비교 끄기") },
        { CompareMode::Wipe, "wipe", L("Wipe", "와이프") },
        { CompareMode::SideBySide, "side", L("Side by Side", "좌우") },
        { CompareMode::TopBottom, "stack", L("Top / Bottom", "위아래") },
        { CompareMode::Blend, "blend", L("Blend", "겹쳐 보기") },
    };
    for (const auto& [mode, icon, label] : modes) {
        const CompareMode m = mode;
        cmpMenu->addAction(icon.isEmpty() ? QIcon() : Icons::icon(icon), label, this, [this, m] { setCompareMode(m); });
    }
    cmpMenu->addSeparator();
    cmpMenu->addAction(Icons::icon("swap"), L("Swap A / B", "A / B 바꾸기"), this, &MainWindow::swapCompare);
    m_compareBtn->setMenu(cmpMenu);
    h->addWidget(sidebar);
    h->addWidget(strip);
    h->addWidget(m_compareBtn);
    h->addSpacing(10);
    m_readout = new QLabel;
    m_readout->setObjectName("Readout");
    m_readout->setMinimumWidth(210);
    h->addWidget(m_readout);
    h->addStretch(1);

    // center: transport
    auto add = [&](const QString& icon, const QString& tip, auto slot, int box = 30, int px = 18) {
        auto* b = iconButton(icon, tip, box, px);
        connect(b, &QToolButton::clicked, this, slot);
        h->addWidget(b);
        return b;
    };
    add("start", L("First Frame  Home", "처음  Home"), [this] { stopPlayback(); goToFrame(playRange().first); });
    add("prev-mark", L("Previous Bookmark  Ctrl+←", "이전 북마크  Ctrl+←"), [this] { jumpBookmark(-1); });
    add("step-back", L("Previous Frame  ←", "이전 프레임  ←"), [this] { step(-1); });
    m_playBtn = add("play", L("Play / Pause  Space · Ctrl: bookmarked frames only", "재생 / 정지  Space · Ctrl: 북마크 단계 재생"), [this] { togglePlay(QApplication::keyboardModifiers() & Qt::ControlModifier); }, 36, 22);
    m_playBtn->setObjectName("PlayButton");
    add("step-fwd", L("Next Frame  →", "다음 프레임  →"), [this] { step(1); });
    add("next-mark", L("Next Bookmark  Ctrl+→", "다음 북마크  Ctrl+→"), [this] { jumpBookmark(1); });
    add("end", L("Last Frame  End", "끝  End"), [this] { stopPlayback(); goToFrame(playRange().second); });
    h->addSpacing(6);
    m_loopBtn = add("loop", L("Loop In/Out  Ctrl+L", "In/Out 반복  Ctrl+L"), [this] { toggleLoop(); });
    m_loopBtn->setCheckable(true);
    h->addStretch(1);

    // right: zoom
    m_zoomBtn = new QToolButton;
    m_zoomBtn->setText("100%");
    m_zoomBtn->setToolTip(L("Zoom", "확대 비율"));
    m_zoomBtn->setFocusPolicy(Qt::NoFocus);
    m_zoomBtn->setPopupMode(QToolButton::InstantPopup);
    m_zoomBtn->setMinimumWidth(56);
    auto* zoomMenu = new QMenu(m_zoomBtn);
    zoomMenu->addAction(L("Fit  F", "화면에 맞추기  F"), this, [this] { m_view->fitToView(); });
    zoomMenu->addSeparator();
    for (int z : { 25, 50, 100, 200, 400 })
        zoomMenu->addAction(QStringLiteral("%1%").arg(z), this, [this, z] { m_view->setZoom(z / 100.0); });
    m_zoomBtn->setMenu(zoomMenu);
    auto* fit = iconButton("fit", L("Fit  F", "화면에 맞추기  F"), 30, 18);
    connect(fit, &QToolButton::clicked, m_view, &Viewport::fitToView);
    // volume (Keyframe Pro puts it next to the zoom readout)
    m_muteBtn = iconButton("volume", L("Mute", "소리 끄기"), 30, 18);
    m_muteBtn->setCheckable(true);
    connect(m_muteBtn, &QToolButton::clicked, this, [this](bool on) { setMuted(on); });
    m_volumeSlider = new QSlider(Qt::Horizontal);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(84);
    m_volumeSlider->setFocusPolicy(Qt::NoFocus);
    m_volumeSlider->setToolTip(L("Volume", "볼륨"));
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int v) { setVolume(v / 100.0); });
    h->addWidget(m_muteBtn);
    h->addWidget(m_volumeSlider);
    h->addSpacing(14);
    h->addWidget(iconButton("zoom", QString(), 24, 15));
    h->addWidget(m_zoomBtn);
    h->addWidget(fit);
    return bar;
}

QAction* MainWindow::addCommand(QMenu* menu, const QString& text, const QKeySequence& key, std::function<void()> slot)
{
    QAction* a = menu ? menu->addAction(text) : new QAction(text, this);
    const QString defaultKey = key.toString(QKeySequence::PortableText);
    QString id = !defaultKey.isEmpty() ? defaultKey : QStringLiteral("command-%1").arg(m_hotkeys.size());
    if (m_hotkeys.contains(id)) {
        // Two commands must never share one default key; keep the first binding.
        qWarning().noquote() << "Shortcut" << defaultKey << "is already used by" << m_hotkeys[id]->text();
        id = QStringLiteral("command-%1").arg(m_hotkeys.size());
    }
    a->setObjectName("command/" + id);
    a->setProperty("defaultShortcut", id.startsWith(QLatin1String("command-")) ? QString() : defaultKey);
    a->setProperty("commandLabel", text.isEmpty() ? L("Alternate: %1", "보조: %1").arg(defaultKey) : text);
    a->setProperty("commandMenu", menu ? menu->title() : QString());
    HotkeyManager::assign(a, QKeySequence(QSettings().value("hotkeys/" + id, a->property("defaultShortcut").toString()).toString(), QKeySequence::PortableText));
    m_hotkeys[id] = a;
    a->setShortcutContext(Qt::WindowShortcut);
    addAction(a);
    connect(a, &QAction::triggered, this, [slot = std::move(slot)] { slot(); });
    return a;
}

void MainWindow::buildMenus()
{
    auto add = [this](QMenu* menu, const QString& text, const QKeySequence& key, auto slot) {
        return addCommand(menu, text, key, std::function<void()>(slot));
    };

    QMenu* file = menuBar()->addMenu(L("File", "파일"));
    add(file, L("New Project", "새 프로젝트"), QKeySequence::New, [this] { newProject(); });
    add(file, L("Open Project...", "프로젝트 열기..."), QKeySequence(Qt::CTRL | Qt::Key_O), [this] { openProjectDialog(); });
    add(file, L("Import Media...", "미디어 가져오기..."), QKeySequence(Qt::CTRL | Qt::Key_I), [this] { importDialog(); });
    add(file, L("Import Folder...", "폴더 가져오기..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), [this] { importFolderDialog(); });
    add(nullptr, L("New Project (2D Animation)", "새 프로젝트 (2D 애니메이션)"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N), [this] { newCanvasDialog(); });
    file->addSeparator();
    add(file, L("Save", "저장"), QKeySequence::Save, [this] { saveProject(false); });
    add(file, L("Save As...", "다른 이름으로 저장..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this] { saveProject(true); });
    file->addSeparator();
    add(file, L("Export...", "내보내기..."), QKeySequence(Qt::CTRL | Qt::Key_E), [this] { exportDialog(); });
    add(file, L("Export Frame...", "현재 프레임 저장..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this] { exportCurrentFrame(); });
    file->addSeparator();
    add(file, L("Exit", "종료"), QKeySequence(Qt::ALT | Qt::Key_F4), [this] { close(); });

    QMenu* edit = menuBar()->addMenu(L("Edit", "편집"));
    add(edit, L("Undo", "실행 취소"), QKeySequence::Undo, [this] { undo(); });
    add(edit, L("Redo", "다시 실행"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this] { redo(); });
    add(nullptr, QString(), QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { redo(); });
    edit->addSeparator();
    add(edit, L("Select All", "모두 선택"), QKeySequence::SelectAll, [this] {
        setTool(Tool::Select);
        m_view->selectAll();
    });
    add(edit, L("Copy", "복사"), QKeySequence::Copy, [this] { copySelection(); });
    add(edit, L("Paste", "붙여넣기"), QKeySequence::Paste, [this] { pasteClipboard(); });
    add(edit, L("Join to Selected Track", "선택한 트래킹 마커에 결합"), QKeySequence(Qt::CTRL | Qt::Key_J), [this] { joinSelectionToTrack(); });
    add(edit, L("Detach from Track", "트래킹 결합 해제"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J), [this] { joinSelectionToTrack(true); });
    add(edit, L("Delete", "삭제"), QKeySequence(Qt::Key_Delete), [this] { deleteContextual(); });
    add(nullptr, QString(), QKeySequence(Qt::Key_Backspace), [this] { deleteContextual(); });
    add(nullptr, QString(), QKeySequence(Qt::Key_Escape), [this] {
        if (m_trackJob) {
            m_trackJob->cancel = true;
            return;
        }
        m_view->clearSelection();
        if (!m_selectedTrackId.isEmpty()) {
            m_selectedTrackId.clear();
            refreshAnnotations();
        }
    });
    edit->addSeparator();
    add(edit, L("Split Clips at Playhead", "재생 위치에서 클립 자르기"), QKeySequence(Qt::CTRL | Qt::Key_B), [this] { splitAtPlayhead(); });
    add(edit, L("Add / Edit Frame Note...", "프레임 메모 추가 / 편집..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] {
        m_notesDock->show();
        m_notesDock->raise();
        syncNoteEditor();
        m_noteEdit->setFocus();
    });
    edit->addSeparator();
    add(edit, L("Clear Frame", "현재 프레임 비우기"), QKeySequence(Qt::SHIFT | Qt::Key_Delete), [this] {
        if (timelineHasFocus()) m_editor->deleteSelection(!m_editor->ripple());
        else clearCurrentFrame();
    });
    add(edit, L("Clear Clip Annotations", "클립 주석 모두 삭제"), QKeySequence(), [this] { clearAllInClip(); });
    edit->addSeparator();
    QAction* cont = edit->addAction(L("Continue Previous Drawing", "이전 주석에 이어 그리기"));
    cont->setCheckable(true);
    cont->setChecked(m_continueDrawing);
    connect(cont, &QAction::toggled, this, [this](bool on) {
        m_continueDrawing = on;
        QSettings().setValue("continueDrawing", on);
    });
    QMenu* tools = edit->addMenu(L("Tools", "도구"));
    const QList<std::tuple<Tool, QString, Qt::Key>> list = {
        { Tool::Select, L("Select", "선택"), Qt::Key_V },   { Tool::Pencil, L("Pencil", "펜슬"), Qt::Key_P },
        { Tool::Pen, L("Pen", "펜"), Qt::Key_N }, { Tool::Highlighter, L("Highlighter", "형광펜"), Qt::Key_B },
        { Tool::Eraser, L("Eraser", "지우개"), Qt::Key_E }, { Tool::Text, L("Text", "텍스트"), Qt::Key_T },
        { Tool::Arrow, L("Arrow", "화살표"), Qt::Key_A },  { Tool::Line, L("Line", "직선"), Qt::Key_L },
        { Tool::Rect, L("Rectangle", "사각형"), Qt::Key_R },   { Tool::Circle, L("Ellipse", "원"), Qt::Key_C },
        { Tool::Track, L("Tracker", "트래커"), Qt::Key_M },
    };
    for (const auto& [tool, name, key] : list) {
        const Tool t = tool;
        add(tools, name, QKeySequence(key), [this, t] { setTool(t); });
    }

    QMenu* play = menuBar()->addMenu(L("Playback", "재생"));
    add(play, L("Play / Pause", "재생 / 정지"), QKeySequence(Qt::Key_Space), [this] { togglePlay(); });
    add(play, L("Stepped Playback (Bookmarks Only)", "단계 재생 (북마크만)"), QKeySequence(Qt::CTRL | Qt::Key_Space), [this] { togglePlay(true); });
    QMenu* onNs = play->addMenu(Icons::icon("steps"), L("Play on 2s / 3s / 4s", "2s / 3s / 4s로 재생"));
    add(onNs, L("Play on 2s", "2프레임씩 (2s)"), QKeySequence(Qt::ALT | Qt::Key_2), [this] { playOnNs(2); });
    add(onNs, L("Play on 3s", "3프레임씩 (3s)"), QKeySequence(Qt::ALT | Qt::Key_3), [this] { playOnNs(3); });
    add(onNs, L("Play on 4s", "4프레임씩 (4s)"), QKeySequence(Qt::ALT | Qt::Key_4), [this] { playOnNs(4); });
    QMenu* speed = play->addMenu(L("Playback Speed", "재생 속도"));
    for (double factor : { 0.25, 0.5, 1.0, 1.5, 2.0 }) {
        speed->addAction(QStringLiteral("%1×").arg(factor), this, [this, factor] {
            const MediaInfo* info = currentInfo();
            const double native = info ? info->fps : viewFps();
            setViewFps(std::clamp(native * factor, 1.0, 240.0));
            showToast(L("Playback %1× · %2 fps", "재생 %1배 · %2 fps").arg(factor).arg(viewFps(), 0, 'f', 2));
        });
    }
    add(play, L("Next Frame", "다음 프레임"), QKeySequence(Qt::Key_Right), [this] { step(1); });
    add(play, L("Previous Frame", "이전 프레임"), QKeySequence(Qt::Key_Left), [this] { step(-1); });
    add(play, L("Forward 10 Frames", "10프레임 앞으로"), QKeySequence(Qt::SHIFT | Qt::Key_Right), [this] { step(10); });
    add(play, L("Back 10 Frames", "10프레임 뒤로"), QKeySequence(Qt::SHIFT | Qt::Key_Left), [this] { step(-10); });
    add(play, L("First Frame", "처음으로"), QKeySequence(Qt::Key_Home), [this] { stopPlayback(); goToFrame(playRange().first); });
    add(play, L("Last Frame", "끝으로"), QKeySequence(Qt::Key_End), [this] { stopPlayback(); goToFrame(playRange().second); });
    // 1.1: edit points of every visible track (clip starts and ends), like an editing program.
    add(play, L("Next Clip / Edit", "다음 클립 / 편집점"), QKeySequence(Qt::Key_Down), [this] { jumpEditPoint(1); });
    add(play, L("Previous Clip / Edit", "이전 클립 / 편집점"), QKeySequence(Qt::Key_Up), [this] { jumpEditPoint(-1); });
    play->addSeparator();
    add(play, L("Set In", "In 지점"), QKeySequence(Qt::Key_I), [this] { setLoopIn(); });
    add(play, L("Set Out", "Out 지점"), QKeySequence(Qt::Key_O), [this] { setLoopOut(); });
    add(play, L("Clear In/Out", "In/Out 지우기"), QKeySequence(Qt::ALT | Qt::Key_X), [this] { clearLoop(); });
    add(play, L("Loop In/Out", "In/Out 반복"), QKeySequence(Qt::CTRL | Qt::Key_L), [this] { toggleLoop(); });
    play->addSeparator();
    m_realtimeAction = play->addAction(L("Skip Frames to Keep Up", "프레임 건너뛰기"));
    m_realtimeAction->setCheckable(true);
    m_realtimeAction->setChecked(m_realtime);
    connect(m_realtimeAction, &QAction::toggled, this, [this](bool on) {
        m_realtime = on;
        QSettings().setValue("realtimePlayback", on);
    });
    play->addAction(L("Playback FPS...", "재생 FPS..."), this, [this] {
        bool ok = false;
        const double fps = QInputDialog::getDouble(this, L("Playback FPS", "재생 FPS"), L("Frames per second", "초당 프레임"), viewFps(), 1, 240, 3, &ok);
        if (ok) setViewFps(fps);
    });

    buildTimelineMenu();
    QMenu* audio = menuBar()->addMenu(L("Audio", "오디오"));
    {
        QSettings s;
        m_scrubAudio = s.value("audio/scrub", true).toBool();
        const QString ca = s.value("audio/compare", "auto").toString();
        m_compareAudio = ca == "a" ? CompareAudio::A : ca == "b" ? CompareAudio::B : CompareAudio::Auto;
    }
    QAction* mute = add(audio, L("Mute", "소리 끄기"), QKeySequence(Qt::CTRL | Qt::Key_M), [this] { setMuted(!m_muted); });
    connect(audio, &QMenu::aboutToShow, mute, [this, mute] { mute->setText(m_muted ? L("Unmute", "소리 켜기") : L("Mute", "소리 끄기")); });
    add(audio, L("Volume Up", "볼륨 올리기"), QKeySequence(Qt::CTRL | Qt::Key_Up), [this] { setVolume(m_volume + 0.1); });
    add(audio, L("Volume Down", "볼륨 내리기"), QKeySequence(Qt::CTRL | Qt::Key_Down), [this] { setVolume(m_volume - 0.1); });
    audio->addSeparator();
    QAction* scrub = audio->addAction(L("Scrub Audio", "스크럽 오디오"));
    scrub->setCheckable(true);
    scrub->setChecked(m_scrubAudio);
    connect(scrub, &QAction::toggled, this, [this](bool on) {
        m_scrubAudio = on;
        QSettings().setValue("audio/scrub", on);
    });
    m_audioOffsetMs = QSettings().value("audio/offsetMs", 0).toInt();
    audio->addAction(L("Audio Offset...", "오디오 오프셋..."), this, [this] {
        bool ok = false;
        const int ms = QInputDialog::getInt(this, L("Audio Offset", "오디오 오프셋"), L("Milliseconds", "밀리초"), m_audioOffsetMs, -500, 500, 5, &ok);
        if (!ok) return;
        m_audioOffsetMs = ms;
        QSettings().setValue("audio/offsetMs", ms);
        showToast(L("Audio offset %1 ms", "오디오 오프셋 %1 ms").arg(ms));
    });
    QMenu* cmpAudio = audio->addMenu(L("Compare Audio", "비교 오디오"));
    auto* cmpAudioGroup = new QActionGroup(cmpAudio);
    const std::pair<CompareAudio, QString> cmpAudioModes[] = {
        { CompareAudio::Auto, L("Auto", "자동") }, { CompareAudio::A, QStringLiteral("A") }, { CompareAudio::B, QStringLiteral("B") } };
    for (const auto& [mode, label] : cmpAudioModes) {
        QAction* act = cmpAudio->addAction(label);
        act->setCheckable(true);
        act->setChecked(m_compareAudio == mode);
        cmpAudioGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, mode = mode] {
            m_compareAudio = mode;
            QSettings().setValue("audio/compare", mode == CompareAudio::A ? "a" : mode == CompareAudio::B ? "b" : "auto");
            if (m_playing) syncAudio(true);
            refreshWaveform();
        });
    }
    if (!m_audio.available()) {
        audio->addSeparator();
        audio->addAction(L("No audio output device", "오디오 출력 장치 없음"))->setEnabled(false);
    }

    QMenu* video = menuBar()->addMenu(L("Video", "비디오"));
    add(video, L("Fit to View", "화면에 맞추기"), QKeySequence(Qt::Key_F), [this] { m_view->fitToView(); });
    add(video, L("Actual Size", "실제 크기"), QKeySequence(Qt::CTRL | Qt::Key_1), [this] { m_view->setZoom(1.0); });
    video->addSeparator();
    QMenu* channels = video->addMenu(L("Color Channels", "색상 채널"));
    auto* channelGroup = new QActionGroup(channels);
    const std::pair<ColorChannel, QString> channelModes[] = {
        {ColorChannel::RGB, L("RGB (All)", "RGB (전체)")},
        {ColorChannel::Red, L("Red Only", "Red만 보기")},
        {ColorChannel::Green, L("Green Only", "Green만 보기")},
        {ColorChannel::Blue, L("Blue Only", "Blue만 보기")}
    };
    for (const auto& [mode, label] : channelModes) {
        QAction* action = channels->addAction(label);
        action->setObjectName(QStringLiteral("ColorChannel%1").arg(int(mode)));
        action->setCheckable(true);
        action->setChecked(m_view->colorChannel() == mode);
        channelGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { m_view->setColorChannel(mode); });
    }
    m_gridAction = add(video, L("Grid", "그리드"), QKeySequence(Qt::Key_G), [this] { m_view->setShowGrid(m_gridAction->isChecked()); });
    m_gridAction->setCheckable(true);
    QAction* showNotes = add(video, L("Show Annotations", "주석 표시"), QKeySequence(Qt::Key_H), [] {});
    showNotes->setCheckable(true);
    showNotes->setChecked(true);
    connect(showNotes, &QAction::toggled, m_view, &Viewport::setShowAnnotations);
    add(video, L("Onion Skin", "어니언 스킨"), QKeySequence(Qt::SHIFT | Qt::Key_O), [this] { setOnion(!m_onion); });
    video->addAction(L("Onion Skin Settings...", "어니언 스킨 설정..."), this, [this] {
        m_onionMenu->exec(m_onionBtn->isVisible() ? m_onionBtn->mapToGlobal(QPoint(m_onionBtn->width() + 4, 0))
                                                  : m_view->mapToGlobal(QPoint(40, 40)));
    });
    video->addSeparator();
    m_flipAction = add(video, L("Mirror (Flip Horizontal)", "좌우 반전 (미러)"), QKeySequence(Qt::SHIFT | Qt::Key_F), [this] {
        setFlip(m_flipAction->isChecked());
    });
    m_flipAction->setCheckable(true);
    m_flipAction->setIcon(Icons::icon("flip"));
    m_hudAction = add(video, L("Frame Counter Overlay", "프레임 카운터 표시"), QKeySequence(Qt::SHIFT | Qt::Key_H), [this] {
        m_view->setShowHud(m_hudAction->isChecked());
        QSettings().setValue("view/hud", m_hudAction->isChecked());
        updateHud();
    });
    m_hudAction->setCheckable(true);
    m_hudAction->setIcon(Icons::icon("hud"));
    m_hudAction->setChecked(QSettings().value("view/hud", false).toBool());
    m_view->setShowHud(m_hudAction->isChecked());
    video->addSeparator();
    QMenu* cmp = video->addMenu(Icons::icon("compare"), L("Compare", "비교"));
    add(cmp, L("Off", "끄기"), QKeySequence(Qt::SHIFT | Qt::Key_0), [this] { setCompareMode(CompareMode::Off); });
    add(cmp, L("Wipe", "와이프"), QKeySequence(Qt::SHIFT | Qt::Key_1), [this] { setCompareMode(CompareMode::Wipe); });
    add(cmp, L("Side by Side", "좌우"), QKeySequence(Qt::SHIFT | Qt::Key_2), [this] { setCompareMode(CompareMode::SideBySide); });
    add(cmp, L("Top / Bottom", "위아래"), QKeySequence(Qt::SHIFT | Qt::Key_3), [this] { setCompareMode(CompareMode::TopBottom); });
    add(cmp, L("Blend", "겹쳐 보기"), QKeySequence(Qt::SHIFT | Qt::Key_4), [this] { setCompareMode(CompareMode::Blend); });
    cmp->addSeparator();
    add(cmp, L("Swap A / B", "A / B 바꾸기"), QKeySequence(Qt::Key_Tab), [this] { swapCompare(); });
    QAction* annotateB = add(cmp, L("Annotate B", "B에 주석"), QKeySequence(Qt::SHIFT | Qt::Key_Tab), [this] {
        if (m_compareMode == CompareMode::Off) return;
        m_view->setActiveSide(m_view->activeSide() == 1 ? 0 : 1);
        showToast(m_view->activeSide() == 1 ? L("Editing B", "B 편집") : L("Editing A", "A 편집"));
    });
    annotateB->setCheckable(true);
    connect(cmp, &QMenu::aboutToShow, annotateB, [this, annotateB] {
        annotateB->setChecked(m_view->activeSide() == 1);
        annotateB->setEnabled(m_compareMode != CompareMode::Off);
    });
    cmp->addSeparator();
    m_compareByTime = QSettings().value("compare/byTime", true).toBool();
    QAction* byTime = cmp->addAction(L("Sync B by Time", "B를 시간으로 맞추기"));
    byTime->setCheckable(true);
    byTime->setChecked(m_compareByTime);
    connect(byTime, &QAction::toggled, this, [this](bool on) {
        m_compareByTime = on;
        QSettings().setValue("compare/byTime", on);
        m_shownCompareKey.clear();
        if (m_compareMode != CompareMode::Off && !m_soloId.isEmpty()) rebuildSequence();
        showToast(on ? L("B synced by time", "B를 시간으로 맞춤") : L("B synced by frame number", "B를 프레임 번호로 맞춤"));
    });

    buildFilterMenu();
    QMenu* marks = menuBar()->addMenu(L("Bookmarks", "북마크"));
    add(marks, L("Toggle Bookmark", "북마크 추가 / 삭제"), QKeySequence(Qt::Key_K), [this] { toggleBookmark(); });
    add(marks, L("Next Bookmark", "다음 북마크"), QKeySequence(Qt::CTRL | Qt::Key_Right), [this] { jumpBookmark(1); });
    add(marks, L("Previous Bookmark", "이전 북마크"), QKeySequence(Qt::CTRL | Qt::Key_Left), [this] { jumpBookmark(-1); });
    add(marks, L("Next Annotated Frame", "다음 주석 프레임"), QKeySequence(Qt::Key_Period), [this] {
        const QVector<qint64> frames = annotatedGlobals();
        auto it = std::upper_bound(frames.begin(), frames.end(), m_current);
        if (it != frames.end()) goToFrame(*it);
    });
    add(marks, L("Previous Annotated Frame", "이전 주석 프레임"), QKeySequence(Qt::Key_Comma), [this] {
        const QVector<qint64> frames = annotatedGlobals();
        auto it = std::lower_bound(frames.begin(), frames.end(), m_current);
        if (it != frames.begin()) goToFrame(*std::prev(it));
    });
    marks->addSeparator();
    add(marks, L("Next Edit Key", "다음 편집 키"), QKeySequence(Qt::SHIFT | Qt::Key_Period), [this] { jumpReviewKey(1); });
    add(marks, L("Previous Edit Key", "이전 편집 키"), QKeySequence(Qt::SHIFT | Qt::Key_Comma), [this] { jumpReviewKey(-1); });
    add(marks, L("Loop Current Review Range", "현재 주석 / 편집 구간 반복"), QKeySequence(), [this] { loopReviewRange(m_current); });
    marks->addSeparator();
    QMenu* listMenu = marks->addMenu(L("All Bookmarks", "모든 북마크"));
    connect(listMenu, &QMenu::aboutToShow, this, [this, listMenu] {
        listMenu->clear();
        for (const Sequence::Segment& s : m_seq.segments) {
            const Source* src = m_project.find(s.sourceId);
            if (!src) continue;
            for (int f : src->bookmarkFrames()) {
                const int64_t g = Sequence::globalIn(s, f);
                if (g < 0) continue;
                listMenu->addAction(QStringLiteral("%1   %2").arg(g, 5).arg(src->displayName()), this, [this, g] { goToFrame(g); });
            }
        }
        if (listMenu->isEmpty()) listMenu->addAction(L("No bookmarks", "북마크 없음"))->setEnabled(false);
    });

    QMenu* trackMenu = menuBar()->addMenu(L("Tracking", "트래킹"));
    buildTrackingManager();
    trackMenu->addAction(m_trackingManager->toggleViewAction());
    trackMenu->addSeparator();
    add(trackMenu, L("Tracker Tool", "트래커 도구"), QKeySequence(), [this] { setTool(Tool::Track); });
    add(trackMenu, L("Track Forward", "앞으로 추적"), QKeySequence(Qt::CTRL | Qt::Key_T), [this] { startTracking(1); });
    add(trackMenu, L("Track Backward", "뒤로 추적"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), [this] { startTracking(-1); });
    add(trackMenu, L("Stop Tracking  Esc", "추적 멈추기  Esc"), QKeySequence(), [this] {
        if (m_trackJob) m_trackJob->cancel = true;
    });
    trackMenu->addSeparator();
    add(trackMenu, L("Delete Selected Track", "선택한 트랙 삭제"), QKeySequence(), [this] { deleteTrack(m_selectedTrackId); });
    add(trackMenu, L("Delete All Tracks in Clip", "클립 트랙 모두 삭제"), QKeySequence(), [this] {
        const Loc loc = locate(m_current);
        if (!loc || loc.source->tracks.isEmpty()) return;
        if (QMessageBox::question(this, L("Delete Track", "트랙 삭제"), L("Delete all %1 tracks in this clip?", "이 클립의 트랙 %1개를 모두 삭제할까요?").arg(loc.source->tracks.size())) != QMessageBox::Yes)
            return;
        const QStringList ids = [&] { QStringList l; for (const Track& t : loc.source->tracks) l << t.id; return l; }();
        for (const QString& id : ids) deleteTrack(id);
    });
    trackMenu->addSeparator();
    {
        QSettings s;
        m_trailTail = s.value("trail/tail", true).toBool();
        m_trailSeconds = s.value("trail/seconds", 5.0).toDouble();
    }
    auto* trailGroup = new QActionGroup(trackMenu);
    QAction* trailAll = trackMenu->addAction(L("Trail: Full Path", "커브: 전체 경로"));
    trailAll->setCheckable(true);
    trailAll->setChecked(!m_trailTail);
    trailGroup->addAction(trailAll);
    connect(trailAll, &QAction::triggered, this, [this] {
        m_trailTail = false;
        QSettings().setValue("trail/tail", false);
        refreshAnnotations();
    });
    QMenu* tailMenu = trackMenu->addMenu(L("Trail: Recent Path", "커브: 지나간 경로"));
    auto* secondsGroup = new QActionGroup(tailMenu);
    for (double sec : { 1.0, 2.0, 3.0, 5.0, 10.0 }) {
        QAction* a = tailMenu->addAction(L("%1 s", "%1초").arg(sec));
        a->setCheckable(true);
        a->setChecked(m_trailTail && std::abs(sec - m_trailSeconds) < 0.01);
        secondsGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, sec, trailAll] {
            m_trailTail = true;
            m_trailSeconds = sec;
            trailAll->setChecked(false);
            QSettings s;
            s.setValue("trail/tail", true);
            s.setValue("trail/seconds", sec);
            refreshAnnotations();
        });
    }
    connect(trailAll, &QAction::triggered, this, [secondsGroup] {
        for (QAction* a : secondsGroup->actions()) a->setChecked(false);
    });
    add(trackMenu, L("Toggle Trail Mode", "커브 표시 방식 바꾸기"), QKeySequence(Qt::SHIFT | Qt::Key_M), [this, trailAll, secondsGroup] {
        m_trailTail = !m_trailTail;
        trailAll->setChecked(!m_trailTail);
        for (QAction* a : secondsGroup->actions())
            a->setChecked(m_trailTail && a->text().startsWith(QString::number(m_trailSeconds)));
        QSettings().setValue("trail/tail", m_trailTail);
        refreshAnnotations();
        showToast(m_trailTail ? L("Trail: last %1 s", "커브: 지나간 %1초").arg(m_trailSeconds) : L("Trail: full path", "커브: 전체 경로"), 1600);
    });
    QAction* numbers = trackMenu->addAction(L("Frame Numbers", "프레임 번호"));
    numbers->setCheckable(true);
    connect(numbers, &QAction::toggled, this, [this](bool on) {
        m_trackNumbers = on;
        refreshAnnotations();
    });
    trackMenu->addAction(m_showTracksAction);

    QMenu* toolsMenu = menuBar()->addMenu(L("Tools", "도구"));
    add(toolsMenu, L("HotKey Manager...", "단축키 관리자..."), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_K), [this] {
        HotkeyManager manager(m_hotkeys, this); manager.exec();
    });
    add(toolsMenu, L("Console", "콘솔"), QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft), [this] {
        m_console->setVisible(!m_console->isVisible());
    });
    auto* endless = add(toolsMenu, L("Endless Scrubbing", "무한 스크러빙"), QKeySequence(), [this] {
        m_endlessScrubbing = !m_endlessScrubbing;
        m_view->setEndlessScrubbing(m_endlessScrubbing);
        m_timeline->setEndlessScrubbing(m_endlessScrubbing);
        QSettings().setValue("endlessScrubbing", m_endlessScrubbing);
    });
    endless->setCheckable(true); endless->setChecked(m_endlessScrubbing);
    QMenu* langMenu = toolsMenu->addMenu(QStringLiteral("Language / 언어"));
    auto* langGroup = new QActionGroup(langMenu);
    for (const auto& [code, label] : { std::pair{ "en", "English" }, std::pair{ "ko", "한국어" } }) {
        QAction* a = langMenu->addAction(QString::fromUtf8(label));
        a->setCheckable(true);
        a->setChecked((Lang::current() == Lang::Korean) == (QByteArray(code) == "ko"));
        langGroup->addAction(a);
        const QString c = QString::fromLatin1(code);
        connect(a, &QAction::triggered, this, [this, c] {
            const bool korean = c == "ko";
            if (korean == (Lang::current() == Lang::Korean)) return;
            QSettings().setValue("ui/language", c);
            // Every label is built once with the language that was active at startup, so restart.
            if (QMessageBox::question(this, korean ? QStringLiteral("언어") : QStringLiteral("Language"),
                                      korean ? QStringLiteral("다시 시작하면 한국어로 바뀝니다. 지금 다시 시작할까요?")
                                             : QStringLiteral("CHARONOS will switch to English after a restart. Restart now?"))
                != QMessageBox::Yes)
                return;
            if (!maybeSave()) return;
            setDirty(false);
            QStringList args;
            if (!m_projectPath.isEmpty()) args << m_projectPath;
            QProcess::startDetached(QApplication::applicationFilePath(), args);
            close();
        });
    }
    toolsMenu->addSeparator();
    m_hwAction = toolsMenu->addAction(L("Hardware Decoding (NVDEC)", "하드웨어 디코딩 (NVDEC)"));
    m_hwAction->setCheckable(true);
    m_hwAction->setChecked(VideoDecoder::hardwareDecodingEnabled());
    m_hwAction->setToolTip(L("Decode H.264 / HEVC / AV1 / VP9 on an NVIDIA GPU; other files and GPUs use the CPU automatically.",
                             "NVIDIA GPU에서 H.264 / HEVC / AV1 / VP9을 디코딩합니다. 그 외 파일과 GPU는 자동으로 CPU를 사용합니다."));
    connect(m_hwAction, &QAction::toggled, this, [this](bool on) {
        VideoDecoder::setHardwareDecodingEnabled(on);
        QSettings().setValue("decode/hardware", on);
        if (!m_project.sources.isEmpty() &&
            QMessageBox::question(this, L("Hardware Decoding", "하드웨어 디코딩"),
                                  L("Reopen the project's media now with the new setting?", "현재 프로젝트의 미디어를 새 설정으로 다시 열까요?")) == QMessageBox::Yes)
            reloadMedia();
    });
    QMenu* ai = toolsMenu->addMenu(Icons::icon("ai"), L("AI Assistant (MCP)", "AI 어시스턴트 (MCP)"));
    m_mcpAction = ai->addAction(L("Allow AI Assistants to Connect", "AI 어시스턴트 연결 허용"));
    m_mcpAction->setCheckable(true);
    connect(m_mcpAction, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("ai/mcpEnabled", on);
        if (!m_mcp) return;
        if (on) m_mcp->start(); else m_mcp->stop();
        m_mcpAction->setChecked(m_mcp->isListening());
        showToast(m_mcp->isListening() ? L("MCP server on", "MCP 서버 켜짐") : L("MCP server off", "MCP 서버 꺼짐"));
    });
    ai->addAction(L("Copy Claude Code Setup Command", "Claude Code 연결 명령 복사"), this, [this] {
        QApplication::clipboard()->setText(McpServer::claudeCodeCommand());
        showToast(L("Copied · paste it in a terminal", "복사됨 · 터미널에 붙여 넣으세요"));
    });
    ai->addAction(L("Copy MCP Config (JSON)", "MCP 설정 복사 (JSON)"), this, [this] {
        QApplication::clipboard()->setText(McpServer::configJson());
        showToast(L("Copied MCP server config", "MCP 서버 설정을 복사했습니다"));
    });
    ai->addAction(L("About AI Integration...", "AI 연동 안내..."), this, [this] {
        QMessageBox box(this);
        box.setWindowTitle(L("AI Assistant (MCP)", "AI 어시스턴트 (MCP)"));
        box.setTextFormat(Qt::RichText);
        box.setText(McpServer::aboutHtml(m_mcp && m_mcp->isListening()));
        box.exec();
    });
    toolsMenu->addSeparator();
    toolsMenu->addAction(L("Cache Memory...", "캐시 메모리..."), this, [this] { changeCacheBudget(); });
    toolsMenu->addAction(L("Clear Cache", "캐시 비우기"), this, [this] {
        m_media.clearCache();
        m_shownKey.clear();
        goToFrame(m_current);
        showToast(L("Cache cleared", "캐시를 비웠습니다"));
    });

    buildWindowMenu();

    QMenu* help = menuBar()->addMenu(L("Help", "도움말"));
    add(help, L("Keyboard Shortcuts", "단축키"), QKeySequence(Qt::Key_F1), [this] { showShortcuts(); })->setIcon(Icons::icon("keyboard"));
    help->addAction(Icons::icon("help"), L("User Guide (English)", "사용 설명서 (영문)"), this, [this] { openUserGuide(); });
    help->addSeparator();
    help->addAction(L("About CHARONOS", "CHARONOS 정보"), this, [this] {
        const bool nvdec = VideoDecoder::hardwareAvailable();
        QMessageBox::about(this, QStringLiteral("CHARONOS"),
            QStringLiteral("<b>CHARONOS %1</b><br>Reference review and analysis for animators.<br><br>"
                           "C++ · Qt %2 · FFmpeg · OpenGL<br>Decoding: %3<br>Viewer: GPU Y'CbCr → RGB<br>AI: MCP server %4")
                .arg(QStringLiteral(FRAMEREVIEW_VERSION), QString::fromLatin1(qVersion()),
                     nvdec ? QStringLiteral("NVDEC + CPU") : QStringLiteral("CPU"),
                     m_mcp && m_mcp->isListening() ? QStringLiteral("on") : QStringLiteral("off")));
    });
}

void MainWindow::showToast(const QString& text, int ms)
{
    qInfo().noquote() << text;
    m_toast->setText(text);
    m_toast->adjustSize();
    m_toast->move((m_view->width() - m_toast->width()) / 2, m_view->height() - m_toast->height() - 18);
    m_toast->show();
    m_toast->raise();
    m_toastTimer.start(ms);
}

// ================================================================== media / project

void MainWindow::importFolderDialog()
{
    const QString folder = QFileDialog::getExistingDirectory(this, L("Import Folder", "폴더 가져오기"), QSettings().value("import/dir").toString());
    if (!folder.isEmpty()) { QSettings().setValue("import/dir", folder); importPaths({folder}); }
}

void MainWindow::addFolder()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, L("New Folder", "새 폴더"), L("Folder name", "폴더 이름"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (!m_project.folders.contains(name)) m_project.folders << name;
    for (QTreeWidgetItem* item : m_tree->selectedItems())
        if (auto* source = m_project.find(item->data(0, RoleId).toString())) source->folder = name;
    if (m_tabs->currentIndex() == 0 && m_clipList->currentItem())
        if (auto* source = m_project.find(m_clipList->currentItem()->data(RoleId).toString())) source->folder = name;
    setDirty(true); refreshProjectTree(); refreshClipList();
}

void MainWindow::createCanvas(QString name, QSize size, int count, double fps)
{
    stopPlayback();
    Source source;
    source.id = Project::newSourceId();
    source.media.kind = MediaRef::Kind::Canvas;
    source.media.fileName = name.trimmed().isEmpty() ? L("2D Animation", "2D 애니메이션") : name.trimmed();
    source.media.width = size.width(); source.media.height = size.height();
    source.frameCount = count; source.fps = fps;
    if (!openServerFor(source)) return;
    m_project.sources.append(source);
    appendToTimeline({ source.id });
    refreshProjectTree(); refreshClipList(); setSolo(source.id);
    setDirty(true);
    showToast(L("2D animation ready · draw a new key on any frame", "2D 애니메이션 준비 완료 · 원하는 프레임에 새 키를 그리세요"));
}

void MainWindow::newCanvasDialog()
{
    showNewProjectDialog(true);
}

void MainWindow::scrubRelative(int frames)
{
    if (m_seq.total <= 0) return;
    stopPlayback();
    int64_t target = m_current + frames;
    if (m_endlessScrubbing) {
        const auto [first, last] = playRange();
        const int64_t length = last - first + 1;
        if (length > 0) target = first + ((target - first) % length + length) % length;
    }
    goToFrame(target); scrubSound();
}

void MainWindow::runConsoleCommand(const QString& command)
{
    const QStringList words = command.split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const QString verb = words.first().toLower();
    if (verb == "help") m_console->append("help | status | frame <0-based number> | play | pause | tool <select,pencil,pen,highlighter,line,arrow,rect,circle,text,eraser> | clear cache");
    else if (verb == "status") {
        m_console->append(QString("CHARONOS %1 | %2 sources | frame %3 / %4 | %5 fps | %6 | cache %7")
            .arg(FRAMEREVIEW_VERSION).arg(m_project.sources.size()).arg(m_current).arg(m_seq.total).arg(viewFps()).arg(m_playing ? "playing" : "paused").arg(formatBytes(m_media.cacheUsed())));
    } else if (verb == "frame" && words.size() == 2) {
        bool ok; const qint64 frame = words[1].toLongLong(&ok);
        if (ok && frame >= 0 && frame < m_seq.total) { stopPlayback(); goToFrame(frame); }
        else m_console->append("Frame is outside the current sequence.");
    } else if (verb == "play") { if (!m_playing) togglePlay(); }
    else if (verb == "pause") stopPlayback();
    else if (verb == "clear" && words.value(1) == "cache") { m_media.clearCache(); m_shownKey.clear(); goToFrame(m_current); }
    else if (verb == "tool" && words.size() == 2) {
        const QMap<QString, Tool> tools{{"select",Tool::Select},{"pencil",Tool::Pencil},{"pen",Tool::Pen},{"highlighter",Tool::Highlighter},{"line",Tool::Line},{"arrow",Tool::Arrow},{"rect",Tool::Rect},{"circle",Tool::Circle},{"text",Tool::Text},{"eraser",Tool::Eraser}};
        if (tools.contains(words[1].toLower())) setTool(tools[words[1].toLower()]); else m_console->append("Unknown tool.");
    } else m_console->append("Unknown command. Type help.");
}

void MainWindow::importDialog()
{
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(this, L("Import Media", "미디어 가져오기"),
                                                            s.value("lastMediaDir").toString(), mediaFilter());
    if (files.isEmpty()) return;
    s.setValue("lastMediaDir", QFileInfo(files.first()).absolutePath());
    importPaths(files);
}

void MainWindow::openPaths(const QStringList& paths)
{
    if (paths.isEmpty()) return;
    const QString suffix = QFileInfo(paths.first()).suffix().toLower();
    if (suffix == Project::kExtension || suffix == "json") {
        loadProject(paths.first());
        return;
    }
    importPaths(paths);
}

QStringList MainWindow::importPaths(const QStringList& paths, bool toTimeline)
{
    QStringList images, videos;
    const int firstNew = int(m_project.sources.size());
    for (const QString& p : paths) {
        if (QFileInfo(p).isDir()) {
            const QString folder = QFileInfo(p).fileName();
            if (!m_project.folders.contains(folder)) m_project.folders << folder;
            QStringList childFiles, childDirs;
            for (const QFileInfo& child : QDir(p).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                if (child.isDir() && !child.isSymLink()) childDirs << child.absoluteFilePath();
                else if (ImageSequenceSource::isImageFile(child.filePath()) || QStringList{"mp4","mov","m4v","mkv","avi","webm","mxf","wmv","mpg","mpeg","gif"}.contains(child.suffix().toLower())) childFiles << child.absoluteFilePath();
            }
            const int before = m_project.sources.size();
            QSet<QString> imported;
            for (const QString& file : childFiles) {
                if (imported.contains(file)) continue;
                if (ImageSequenceSource::isImageFile(file)) {
                    const QStringList sequence = ImageSequenceSource::expandSequence(file);
                    for (const QString& member : sequence) imported.insert(member);
                }
                importPaths({file}, toTimeline);
            }
            for (int i = before; i < m_project.sources.size(); ++i) m_project.sources[i].folder = folder;
            for (const auto& dir : childDirs) importPaths({dir}, toTimeline);
            setDirty(true); refreshProjectTree();
        } else (ImageSequenceSource::isImageFile(p) ? images : videos) << QFileInfo(p).absoluteFilePath();
    }

    const int before = m_project.sources.size();
    for (const QString& v : videos) {
        MediaRef ref;
        ref.kind = MediaRef::Kind::Video;
        ref.path = v;
        addSource(ref, 0, toTimeline);
    }
    if (!images.isEmpty()) {
        MediaRef ref;
        ref.kind = MediaRef::Kind::ImageSequence;
        ref.frames = images.size() == 1 ? ImageSequenceSource::expandSequence(images.first())
                                        : ImageSequenceSource::naturalSorted(images);
        addSource(ref, m_project.timeline.clips.isEmpty() ? 24.0 : m_project.timeline.fps, toTimeline);
    }
    QStringList added;
    for (int i = firstNew; i < m_project.sources.size(); ++i) added << m_project.sources[i].id;
    if (m_project.sources.size() == before) return added;

    setDirty(true);
    refreshProjectTree();
    refreshClipList();
    rebuildSequence();
    // jump to the first newly imported clip
    const QString first = m_project.sources[before].id;
    if (!toTimeline) return added;
    if (m_soloId.isEmpty()) {
        const int64_t g = m_seq.globalFor(first, 0);
        if (g >= 0) goToFrame(g);
    } else {
        setSolo(first);
    }
    return added;
}

bool MainWindow::addSource(MediaRef ref, double fps, bool toTimeline)
{
    Source s;
    s.id = Project::newSourceId();
    s.media = ref;
    s.fps = fps > 0 ? fps : 24.0;
    if (!openServerFor(s)) return false;
    if (fps <= 0) s.fps = m_media.server(s.id)->info().fps;
    m_project.sources.append(s);
    if (toTimeline) appendToTimeline({ s.id });
    return true;
}

bool MainWindow::openServerFor(Source& s)
{
    m_timingInfo.remove(s.id);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    showToast(L("Analyzing: %1", "분석 중: %1").arg(QFileInfo(s.media.firstFile()).fileName()), 60000);
    QApplication::processEvents();
    QString error;
    std::unique_ptr<FrameSource> source;
    if (s.media.kind == MediaRef::Kind::Canvas)
        source = std::make_unique<CanvasSource>(QSize(s.media.width, s.media.height), s.frameCount, s.fps);
    else source = MediaLibrary::openSource(s.media.isSequence(), s.media.path, s.media.frames, s.fps, &error);
    QApplication::restoreOverrideCursor();
    m_toast->hide();
    if (!source) {
        QMessageBox::warning(this, L("Can't Open", "열 수 없음"),
                             QStringLiteral("%1\n\n%2").arg(QDir::toNativeSeparators(s.media.firstFile()), error));
        return false;
    }
    const MediaInfo& info = source->info();
    s.media.width = info.width;
    s.media.height = info.height;
    s.frameCount = int(info.frameCount);
    if (s.media.fileName.isEmpty()) {
        s.media.fileName = QFileInfo(s.media.firstFile()).fileName();
        if (s.media.isSequence()) {
            static const QRegularExpression digits(QStringLiteral("[._-]?\\d+(\\.[^.]+)$"));
            s.media.fileName = QFileInfo(s.media.firstFile()).fileName().replace(digits, QStringLiteral(" [%1]\\1").arg(s.media.frames.size()));
        }
    }
    m_media.add(s.id, std::move(source));
    startAudioDecode(s);
    return true;
}

void MainWindow::removeSource(const QString& id)
{
    const Source* s = m_project.find(id);
    if (!s) return;
    if (!s->annotations.empty() &&
        QMessageBox::question(this, L("Remove Source", "소스 제거"),
                              L("%1 has annotations. Remove it from the project?", "%1에 주석이 있습니다. 프로젝트에서 제거할까요?").arg(s->displayName())) != QMessageBox::Yes)
        return;
    stopPlayback();
    m_view->commitEdits();
    if (m_pending) commitEdit();
    m_project.timeline.clips.removeIf([&](const TimelineClip& c) { return c.sourceId == id; });
    m_project.sources.erase(std::remove_if(m_project.sources.begin(), m_project.sources.end(),
                                           [&](const Source& x) { return x.id == id; }),
                            m_project.sources.end());
    m_media.remove(id);
    m_timingInfo.remove(id);
    m_audioClips.erase(id);
    m_peaks.erase(id);
    m_plates.remove(id);
    m_otsu.remove(id);
    invalidateMix();
    if (m_soloId == id) m_soloId.clear();
    if (m_compareId == id) setCompareMode(CompareMode::Off), m_compareId.clear();
    m_undo.erase(std::remove_if(m_undo.begin(), m_undo.end(), [&](const Edit& e) { return e.sourceId == id; }), m_undo.end());
    m_redo.erase(std::remove_if(m_redo.begin(), m_redo.end(), [&](const Edit& e) { return e.sourceId == id; }), m_redo.end());
    setDirty(true);
    refreshProjectTree();
    refreshClipList();
    rebuildSequence();
}

void MainWindow::openProjectDialog()
{
    QSettings s;
    const QString file = QFileDialog::getOpenFileName(this, L("Open Project", "프로젝트 열기"),
                                                      s.value("lastProjectDir").toString(), projectFilter());
    if (file.isEmpty()) return;
    s.setValue("lastProjectDir", QFileInfo(file).absolutePath());
    loadProject(file);
}

bool MainWindow::loadProject(const QString& path)
{
    if (!maybeSave()) return false;
    Project p;
    QString error;
    if (!p.load(path, &error)) {
        QMessageBox::warning(this, L("Couldn't Open Project", "프로젝트 열기 실패"), error);
        return false;
    }
    stopPlayback();
    if (m_trackJob) m_trackJob->cancel = true;  // it belongs to the project being closed
    m_media.clear();
    m_audioClips.clear();
    m_timingInfo.clear();
    m_peaks.clear();
    m_plates.clear();
    m_otsu.clear();
    m_layersKey.clear();
    m_undo.clear();
    m_redo.clear();
    m_soloId.clear();
    m_compareId.clear();
    setCompareMode(CompareMode::Off);
    m_project = p;
    m_noteSource.clear();
    m_noteFrame = -1;
    m_noteDirty = false;
    m_graphSignature.clear();
    m_onionVideoFrames.clear();
    if (m_editor) m_editor->clearThumbnails();

    bool relinked = false;
    for (Source& s : m_project.sources) {
        if (!s.media.exists()) {
            const QString hint = s.media.fileName.isEmpty() ? L("(unnamed)", "(이름 없음)") : s.media.fileName;
            if (QMessageBox::question(this, L("Locate Media", "미디어 찾기"),
                                      L("Original media not found.\n%1\n\nLocate it now?", "원본 미디어를 찾을 수 없습니다.\n%1\n\n직접 찾아서 연결할까요?").arg(hint))
                == QMessageBox::Yes) {
                const QStringList files = QFileDialog::getOpenFileNames(this, L("Locate: %1", "원본 찾기: %1").arg(hint),
                                                                        QFileInfo(path).absolutePath(), mediaFilter());
                if (!files.isEmpty()) {
                    if (ImageSequenceSource::isImageFile(files.first())) {
                        s.media.kind = MediaRef::Kind::ImageSequence;
                        s.media.frames = files.size() == 1 ? ImageSequenceSource::expandSequence(files.first())
                                                           : ImageSequenceSource::naturalSorted(files);
                    } else {
                        s.media.kind = MediaRef::Kind::Video;
                        s.media.path = files.first();
                    }
                    relinked = true;
                }
            }
        }
        if (s.media.exists()) openServerFor(s);
    }
    // 1.0 files: clips back to back on V1, their sound becomes linked clips on A1.
    tl::normalize(m_project.timeline, lengthFn());
    if (m_project.timeline.linkAudioOnOpen) linkLegacyAudio();
    ++m_annotationRev;
    invalidateMix();

    const bool legacy = QFileInfo(path).suffix().toLower() != Project::kExtension;
    m_projectPath = legacy ? QString() : path;
    m_timelineTitle->setText(m_project.timeline.name);
    refreshProjectTree();
    refreshClipList();
    rebuildSequence();
    goToFrame(0);
    setDirty(legacy || relinked);
    refreshNotes();
    syncNoteEditor();
    refreshMotionGraph();
    showToast(L("Opened: %1", "열림: %1").arg(QFileInfo(path).fileName()));
    return true;
}

bool MainWindow::saveProject(bool saveAs)
{
    m_view->commitEdits();
    commitNoteEditor();
    if (m_pending) commitEdit();
    if (m_project.sources.isEmpty()) return false;
    QString path = m_projectPath;
    if (saveAs || path.isEmpty()) {
        QSettings s;
        const QString base = QFileInfo(m_project.sources.first().media.fileName).completeBaseName();
        const QString dir = s.value("lastProjectDir", QFileInfo(m_project.sources.first().media.firstFile()).absolutePath()).toString();
        path = QFileDialog::getSaveFileName(this, L("Save Project", "프로젝트 저장"),
                                            dir + "/" + (base.isEmpty() ? "project" : base) + "." + Project::kExtension,
                                            projectFilter());
        if (path.isEmpty()) return false;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".") + Project::kExtension;
        s.setValue("lastProjectDir", QFileInfo(path).absolutePath());
    }
    QString error;
    if (!m_project.save(path, &error)) {
        QMessageBox::warning(this, L("Save Failed", "저장 실패"), error);
        return false;
    }
    m_projectPath = path;
    setDirty(false);
    showToast(L("Saved · %1", "저장됨 · %1").arg(QFileInfo(path).fileName()));
    return true;
}

bool MainWindow::maybeSave()
{
    m_view->commitEdits();
    commitNoteEditor();
    if (m_pending) commitEdit();
    if (!m_dirty || m_project.sources.isEmpty()) return true;
    const auto r = QMessageBox::question(this, L("Unsaved Changes", "저장하지 않은 변경 사항"),
                                         L("Save your changes?", "변경 사항을 저장할까요?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save) return saveProject(false);
    return r == QMessageBox::Discard;
}

void MainWindow::newProject()
{
    showNewProjectDialog(false);
}

void MainWindow::showNewProjectDialog(bool animation)
{
    QDialog dialog(this);
    dialog.setObjectName("NewProjectDialog");
    dialog.setWindowTitle(L("New Project", "새 프로젝트"));
    dialog.setMinimumWidth(420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* type = new QComboBox; type->setObjectName("ProjectType");
    type->addItems({L("Video review / Empty project", "동영상 리뷰 / 빈 프로젝트"), L("2D Animation", "2D 애니메이션")});
    type->setCurrentIndex(animation ? 1 : 0);
    auto* name = new QLineEdit(L("Untitled", "제목 없음")); name->setObjectName("ProjectName");
    form->addRow(L("Project type", "프로젝트 종류"), type);
    form->addRow(L("Name", "이름"), name);
    layout->addLayout(form);
    auto* canvas = new QWidget;
    auto* fields = new QFormLayout(canvas); fields->setContentsMargins(0, 0, 0, 0);
    auto* width = new QSpinBox; width->setObjectName("CanvasWidth"); width->setRange(16, 8192); width->setValue(1920);
    auto* height = new QSpinBox; height->setObjectName("CanvasHeight"); height->setRange(16, 8192); height->setValue(1080);
    auto* frames = new QSpinBox; frames->setObjectName("CanvasFrames"); frames->setRange(1, 100000); frames->setValue(240);
    auto* fps = new QDoubleSpinBox; fps->setObjectName("CanvasFps"); fps->setRange(1, 240); fps->setDecimals(3); fps->setValue(24);
    fields->addRow(L("Width", "너비"), width); fields->addRow(L("Height", "높이"), height);
    fields->addRow(L("Frames", "프레임 수"), frames); fields->addRow("FPS", fps);
    layout->addWidget(canvas); canvas->setVisible(animation);
    connect(type, &QComboBox::currentIndexChanged, canvas, [canvas](int index) { canvas->setVisible(index == 1); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(L("Create", "만들기"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    // Cancellation leaves the current project and playback untouched.
    if (dialog.exec() != QDialog::Accepted) return;
    if (!maybeSave()) return;
    stopPlayback();
    if (m_trackJob) m_trackJob->cancel = true;
    m_media.clear();
    m_audioClips.clear();
    m_timingInfo.clear();
    m_project = Project();
    m_project.timeline.name = name->text().trimmed().isEmpty() ? L("Untitled", "제목 없음") : name->text().trimmed();
    m_projectPath.clear();
    m_soloId.clear();
    m_compareId.clear();
    m_pending.reset();
    m_shownKey.clear(); m_shownCompareKey.clear(); m_lastSourceId.clear();
    m_current = 0; m_selectedTrackId.clear(); m_compareAutoSolo = false;
    m_noteSource.clear(); m_noteFrame = -1; m_noteDirty = false; m_graphSignature.clear(); m_onionVideoFrames.clear();
    if (m_editor) m_editor->clearThumbnails();
    setCompareMode(CompareMode::Off);
    m_undo.clear();
    m_redo.clear();
    m_view->clearFrame();
    m_view->setAnnotations({});
    m_timelineTitle->setText(m_project.timeline.name);
    refreshProjectTree();
    refreshClipList();
    rebuildSequence();
    setDirty(false);
    if (type->currentIndex() == 1)
        createCanvas(m_project.timeline.name, QSize(width->value(), height->value()), frames->value(), fps->value());
}

void MainWindow::exportDialog()
{
    if (m_seq.total <= 0) return;
    stopPlayback();
    const Loc loc = locate(m_current);
    ExportDialog::Ranges r;
    r.total = m_seq.total;
    r.hasLoop = loopEnabled();
    std::tie(r.loopIn, r.loopOut) = playRange();
    if (loc) {
        r.clipStart = loc.segment->start;
        r.clipEnd = loc.segment->start + loc.segment->count - 1;
        r.clipName = loc.source->displayName();
    }
    r.fps = viewFps();
    r.comparing = m_compareMode != CompareMode::Off && m_media.server(m_compareId) && m_seq.segments.size() == 1;
    if (m_filter.enabled()) r.filterName = AnalysisFilter::name(m_filter.mode);
    r.suggestedName = m_soloId.isEmpty()
                          ? (m_projectPath.isEmpty() ? QStringLiteral("timeline") : QFileInfo(m_projectPath).completeBaseName())
                          : QFileInfo(loc.source->displayName()).completeBaseName();
    ExportDialog dlg(r, this);
    if (dlg.exec() != QDialog::Accepted) return;

    ExportJob job;
    job.format = dlg.format();
    job.output = dlg.output();
    job.project = m_project;
    job.sequence = m_seq;
    std::tie(job.first, job.last) = dlg.range();
    job.fps = dlg.fps();
    job.scale = dlg.scale();
    job.margin = dlg.margin();
    job.notes = dlg.notes();
    job.caption = dlg.caption();
    job.tracks = dlg.tracks();
    job.trackRange = trailFrames();
    job.audio = dlg.audio();
    job.audioClips = m_audioClips;
    if (dlg.filter()) {
        job.filter = m_filter;
        job.filterThreshold = effectiveThreshold();
        for (auto it = m_plates.begin(); it != m_plates.end(); ++it)
            if (it.value()) job.plates[it.key()] = it.value()->toImage();
    }
    if (m_soloId.isEmpty() && !dlg.comparison()) {
        // The multi-track timeline as the viewer shows it: layers per frame, the sound mix.
        job.timeline = true;
        job.canvas = canvasSize();
        job.timelineFps = std::max(1.0, m_project.timeline.fps);
        job.mix = timelineMix();
        job.layers.reserve(size_t(job.frameCount()));
        for (int64_t g = job.first; g <= job.last; ++g) {
            const Sequence::Segment* seg = m_seq.at(g);
            std::vector<ExportJob::Layer> frame;
            for (const PlacedLayer& p : visibleLayers(g)) {
                ExportJob::Layer l;
                l.sourceId = p.clip->sourceId;
                l.local = p.local;
                l.rect = p.rect;
                l.opacity = p.clip->opacity;
                l.blend = p.clip->blend;
                l.primary = seg && p.clip->uid == seg->clipId;
                frame.push_back(l);
            }
            job.layers.push_back(std::move(frame));
        }
        // Annotation lanes switched off stay out of the export too.
        bool hidden = false;
        for (const TrackState& t : m_project.timeline.noteTracks) hidden = hidden || !t.visible;
        if (hidden)
            for (const Source& s : m_project.sources) {
                const QHash<QString, int> lanes = noteLanes(s);
                for (auto it = lanes.begin(); it != lanes.end(); ++it)
                    if (!m_project.timeline.noteTracks.value(it.value()).visible) job.hiddenNotes[s.id].insert(it.key());
            }
    }
    if (dlg.comparison()) {
        using Layout = ExportJob::Compare::Layout;
        auto& c = job.compare;
        c.layout = m_compareMode == CompareMode::Wipe         ? Layout::Wipe
                   : m_compareMode == CompareMode::SideBySide ? Layout::SideBySide
                   : m_compareMode == CompareMode::TopBottom  ? Layout::TopBottom
                                                              : Layout::Blend;
        c.sourceId = m_compareId;
        c.wipe = m_view->wipe();
        c.audioFromB = compareAudioFromB();
        const MediaInfo& bi = m_media.server(m_compareId)->info();
        const int64_t count = m_seq.segments.front().count;
        for (int64_t i = 0; i <= count; ++i) {
            if (i < count) c.bFrame.push_back(compareFrameFor(i));
            c.bTime.push_back(m_compareByTime ? compareTimeFor(i) : bi.timeOf(i));
        }
    }
    if (!job.isSequence() && QFileInfo::exists(job.output) &&
        QMessageBox::question(this, L("Overwrite", "덮어쓰기"), L("%1 already exists. Overwrite?", "%1 파일이 이미 있습니다. 덮어쓸까요?").arg(QFileInfo(job.output).fileName())) != QMessageBox::Yes)
        return;

    std::atomic<int64_t> done{ 0 };
    std::atomic<bool> cancel{ false };
    QString error;
    bool ok = false;
    QThread* worker = QThread::create([&] {
        // An exception escaping a QThread ends the process; turn it into an export error instead.
        try {
            ok = runExport(job, done, cancel, &error);
        } catch (const std::exception& e) {
            ok = false;
            error = L("Export stopped: %1 (try a smaller size or shorter range)", "내보내기 중단: %1 (크기나 범위를 줄여 보세요)").arg(QString::fromLocal8Bit(e.what()));
        }
    });

    QProgressDialog progress(L("Exporting...", "내보내는 중..."), L("Cancel", "취소"), 0, int(job.frameCount()), this);
    progress.setWindowTitle(L("Export", "내보내기"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setMinimumWidth(380);
    QElapsedTimer clock;
    clock.start();
    QTimer poll;
    connect(&poll, &QTimer::timeout, &progress, [&] {
        const int64_t n = done.load();
        progress.setValue(int(n));
        const double secs = clock.elapsed() / 1000.0;
        const double rate = secs > 0 ? n / secs : 0;
        const int remain = rate > 0 ? int((job.frameCount() - n) / rate) : 0;
        progress.setLabelText(L("%1 / %2 frames   ·   %3 fps   ·   %4 s left", "%1 / %2 프레임   ·   %3 fps   ·   남은 시간 %4초")
                                  .arg(n).arg(job.frameCount()).arg(rate, 0, 'f', 1).arg(remain));
    });
    connect(&progress, &QProgressDialog::canceled, &progress, [&] { cancel = true; });
    connect(worker, &QThread::finished, &progress, &QProgressDialog::reset);
    worker->start();
    poll.start(100);
    progress.exec();
    worker->wait();
    delete worker;
    poll.stop();
    progress.hide();
    QApplication::processEvents();

    if (!ok && !job.isSequence()) QFile::remove(job.output);  // don't leave a truncated movie behind
    if (ok) {
        showToast(L("Export done · %1 frames · %2 s", "내보내기 완료 · %1프레임 · %2초").arg(job.frameCount()).arg(clock.elapsed() / 1000.0, 0, 'f', 1), 4000);
        const QString folder = QFileInfo(job.output).absolutePath();
        if (QMessageBox::question(this, L("Export Done", "내보내기 완료"), L("Open the folder?\n%1", "폴더를 열까요?\n%1").arg(QDir::toNativeSeparators(job.output))) == QMessageBox::Yes)
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    } else if (!cancel) {
        qWarning().noquote() << "Export failed:" << error;
        QMessageBox::warning(this, L("Export Failed", "내보내기 실패"), error);
    }
}

void MainWindow::exportCurrentFrame()
{
    stopPlayback();
    m_view->commitEdits();
    const Loc loc = locate(m_current);
    if (!loc) return;
    Source* source = loc.source;
    FrameServer* server = loc.server;
    int64_t local = loc.local, notesFrame = loc.rawLocal;
    if (m_compareMode != CompareMode::Off && m_view->activeSide() == 1) {
        if (auto* b = m_project.find(m_compareId); b && m_media.server(m_compareId)) {
            source = b; server = m_media.server(m_compareId);
            local = notesFrame = compareFrameFor(loc.rawLocal);
        }
    }
    FrameCache::Ptr cached = server->frame(local);
    if (!cached) {
        showToast(L("Decoding frame…", "프레임을 디코딩하는 중입니다"));
        return;
    }
    // The cache keeps Y'CbCr planes; stills need RGB pixels.
    const QImage still = cached->toImage();
    if (still.isNull()) {
        showToast(L("Can't convert this frame.", "이 프레임을 변환할 수 없습니다."));
        return;
    }
    const QImage* frame = &still;
    FrameExportDialog dialog(*frame, *source, int(notesFrame), this, trailFrames());
    if (dialog.exec() != QDialog::Accepted) return;
    auto options = dialog.options(); options.trackRange = trailFrames();
    QSettings s;
    const QString base = QFileInfo(source->displayName()).completeBaseName();
    const QString suffix = QString::fromLatin1(dialog.format());
    QString path = QFileDialog::getSaveFileName(
        this, L("Export Frame", "현재 프레임 저장"),
        QDir(s.value("lastExportDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString())
            .filePath(QStringLiteral("%1_%2.%3").arg(base).arg(local, 4, 10, QChar('0')).arg(suffix)),
        QStringLiteral("%1 (*.%2)").arg(QString::fromLatin1(dialog.format()).toUpper(), suffix));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += "." + suffix;
    const QImage out = composeStillFrame(*frame, *source, int(notesFrame), options);
    if (out.isNull()) {
        QMessageBox::warning(this, L("Export Failed", "내보내기 실패"), L("Cannot allocate output image. Reduce the scale or content extent.", "출력 이미지를 만들 수 없습니다. 배율이나 내용의 범위를 줄이세요."));
        return;
    }
    s.setValue("lastExportDir", QFileInfo(path).absolutePath());
    QImageWriter writer(path, dialog.format());
    writer.setQuality(dialog.quality());
    if (dialog.format() == "tif") writer.setCompression(1);
    if (!writer.write(out))
        QMessageBox::warning(this, L("Save Failed", "저장 실패"), writer.errorString());
    else {
        dialog.saveSettings();
        showToast(L("Saved · %1", "저장됨 · %1").arg(QFileInfo(path).fileName()));
    }
}

// ================================================================== view / navigation

void MainWindow::setSolo(const QString& sourceId)
{
    if (sourceId == m_soloId && !m_seq.segments.isEmpty()) return;
    m_view->commitEdits();
    if (m_pending) commitEdit();
    stopPlayback();
    // keep the playhead on the same clip frame when switching views
    const Loc loc = locate(m_current);
    const QString keepId = loc ? loc.source->id : QString();
    const int64_t keepLocal = loc ? loc.local : 0;
    m_soloId = sourceId;
    m_soloLoop = false;
    rebuildSequence();
    int64_t g = m_seq.globalFor(sourceId.isEmpty() ? keepId : sourceId, sourceId == keepId || sourceId.isEmpty() ? keepLocal : 0);
    goToFrame(std::max<int64_t>(0, g));
    refreshProjectTree();
    m_clipList->viewport()->update();
}

void MainWindow::rebuildSequence()
{
    if (m_playing) stopPlayback();
    m_seq = Sequence();
    if (!m_soloId.isEmpty() && !m_media.server(m_soloId)) m_soloId.clear();
    if (!m_soloId.isEmpty()) {
        // While comparing, run as long as the longer of A and B so B isn't cut off when A ends
        // (A then holds its last frame).
        int64_t count = m_media.server(m_soloId)->frameCount();
        if (m_compareMode != CompareMode::Off) count = std::max(count, compareSpan());
        m_seq.append(m_soloId, count);
    } else {
        m_seq = timelineSequence();
    }
    m_timeline->setFrameCount(m_seq.total);
    QVector<qint64> starts;
    for (const Sequence::Segment& s : m_seq.segments) starts << s.start;
    m_timeline->setSegments(starts);
    m_timeline->setRates(viewFps(), 0);
    refreshWaveform();
    m_shownKey.clear();

    if (m_seq.total <= 0) {
        m_view->clearFrame();
        m_view->setAnnotations({});
        m_view->setPlaceholder(L("Drop media or start File > New Project", "미디어를 놓거나 파일 > 새 프로젝트로 시작하세요"));
        m_clipBar->setClip(QString(), 0, 0);
        m_timeline->setMarks({}, {});
        m_reviewRanges.clear();
        m_reviewKeys.clear();
        m_clipBar->setReviewMarks({}, {});
        updateReadout();
        updateTitle();
        return;
    }
    m_current = std::clamp<int64_t>(m_current, 0, m_seq.total - 1);
    refreshMarks();
    updateLoopUi();
    goToFrame(m_current);
    refreshProperties();
    updateTitle();
    refreshTimelineEditor();
}

Sequence MainWindow::timelineSequence() const
{
    // The multi-track timeline as the viewer plays it: per frame the clip being edited (the top
    // visible one), gaps where no picture is. Offline clips keep their time (shown as gaps).
    return tl::primary(m_project.timeline, lengthFn(), onlineFn());
}

std::pair<int64_t, int64_t> MainWindow::segmentRange(const Sequence::Segment& s)
{
    return { s.sourceFrame(0), s.sourceFrame(s.count - 1) };
}

QVector<qint64> MainWindow::annotatedGlobals() const
{
    QVector<qint64> out;
    for (const Sequence::Segment& s : m_seq.segments)
        if (const Source* src = m_project.find(s.sourceId))
            for (int f : src->annotatedFrames())
                if (const int64_t g = Sequence::globalIn(s, f); g >= 0) out << g;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

MainWindow::Loc MainWindow::locate(int64_t global)
{
    Loc loc;
    loc.segment = m_seq.at(global);
    if (!loc.segment) return loc;
    loc.source = m_project.find(loc.segment->sourceId);
    loc.server = m_media.server(loc.segment->sourceId);
    // rawLocal is the source frame (trim offset applied); compare mode may run past A's end.
    loc.rawLocal = loc.segment->sourceFrame(std::clamp<int64_t>(global - loc.segment->start, 0, loc.segment->count - 1));
    loc.local = loc.server ? std::clamp<int64_t>(loc.rawLocal, 0, std::max<int64_t>(0, loc.server->frameCount() - 1)) : loc.rawLocal;
    return loc;
}

double MainWindow::compareFps() const
{
    const Source* b = m_project.find(m_compareId);
    return b ? std::max(1.0, b->fps) : viewFps();
}

const MediaInfo* MainWindow::currentInfo() const
{
    const Sequence::Segment* seg = m_seq.at(m_current);
    FrameServer* srv = seg ? m_media.server(seg->sourceId) : nullptr;
    return srv ? &srv->info() : nullptr;
}

double MainWindow::compareTimeFor(int64_t aFrame) const
{
    // Seconds since A's first frame on the playback clock, then the same moment on B's clock.
    const MediaInfo* a = currentInfo();
    FrameServer* bs = m_media.server(m_compareId);
    if (!a || !bs) return 0.0;
    const double played = (a->timeOf(aFrame) - a->startTime()) * std::max(1.0, a->fps) / viewFps();
    return bs->info().startTime() + played;
}

int64_t MainWindow::compareFrameFor(int64_t aFrame) const
{
    FrameServer* bs = m_media.server(m_compareId);
    if (!bs) return 0;
    // By time: B shows what was on its screen at the same moment (works for different and variable fps).
    const int64_t f = m_compareByTime ? bs->info().frameAt(compareTimeFor(aFrame)) : aFrame;
    return std::clamp<int64_t>(f, 0, bs->frameCount() - 1);
}

int64_t MainWindow::compareSpan() const
{
    FrameServer* bs = m_media.server(m_compareId);
    if (!bs) return 0;
    if (!m_compareByTime) return bs->frameCount();
    const MediaInfo& b = bs->info();
    const double bDuration = b.timeOf(b.frameCount) - b.startTime();
    return int64_t(std::ceil(bDuration * viewFps() - 1e-6));
}

void MainWindow::goToFrame(int64_t global)
{
    if (m_seq.total <= 0) return;
    global = std::clamp<int64_t>(global, 0, m_seq.total - 1);
    if (m_stepped && m_playing) {
        m_steppedFrames = playbackBookmarks();
        if (m_steppedFrames.isEmpty()) { stopPlayback(); return goToFrame(global); }
        global = steppedFrameFor(global);
        m_steppedPosition = global;
    }
    if (global != m_current) {
        m_view->commitEdits();
        if (m_pending) commitEdit();
    }
    m_current = global;
    const Loc loc = locate(global);
    if (!loc) {
        if (m_seq.at(global)) showGap(global);
        return;
    }
    const bool onTimeline = m_soloId.isEmpty();

    // Request this frame (and prefetch within the play range clipped to this clip), in source frames.
    const auto [a, b] = playRange();
    const int64_t segEnd = loc.segment->start + loc.segment->count - 1;
    const int64_t lo = loc.segment->sourceFrame(std::max(a, loc.segment->start) - loc.segment->start);
    const int64_t hi = loc.segment->sourceFrame(std::min(b, segEnd) - loc.segment->start);
    loc.server->request(loc.local, m_playing, lo, hi);

    QStringList active{ loc.source->id };
    // Near the end of a clip during playback, warm up the next one (gaps have no picture).
    const int idx = m_seq.indexAt(global);
    if (m_playing && idx + 1 < m_seq.segments.size() && !m_seq.segments[idx + 1].isGap() && segEnd - global < int64_t(viewFps() * 2)) {
        const Sequence::Segment& next = m_seq.segments[idx + 1];
        if (FrameServer* ns = m_media.server(next.sourceId)) {
            if (next.sourceId != loc.source->id)  // the same source split in two keeps one request
                ns->request(next.sourceFrame(0), true, next.sourceFrame(0), next.sourceFrame(std::min<int64_t>(next.count - 1, std::max<int64_t>(0, b - next.start))));
            active << next.sourceId;
        }
    }
    if (m_compareMode != CompareMode::Off) active << m_compareId;
    if (onTimeline)
        for (const QString& id : m_layerSources)
            if (!active.contains(id)) active << id;
    m_media.setActive(active);

    const QSize media(loc.server->info().width, loc.server->info().height);
    if (onTimeline) {
        // The canvas is the timeline's frame; the edited picture sits in it with its clip's transform.
        const QSize canvas = canvasSize();
        m_view->setMediaSize(canvas);
        if (const TimelineClip* clip = m_project.timeline.clip(loc.segment->clipId))
            m_view->setPrimaryPlacement(media, placementFor(*clip, media, canvas), float(clip->opacity), clip->blend);
        else
            m_view->setPrimaryPlacement({}, {});
    } else {
        m_view->setMediaSize(media);
        m_view->setPrimaryPlacement({}, {});
    }
    const QString key = loc.source->id + ':' + QString::number(loc.local);
    if (key != m_shownKey) {
        if (FrameCache::Ptr img = loc.server->frame(loc.local)) {
            m_view->setFrame(img);
            m_shownKey = key;
        }
    }
    if (onTimeline) {
        updateLayers(loc);
    } else if (m_view->layerCount() > 0) {
        m_view->setLayers({}, {});
        m_layersKey.clear();
        m_layerSources.clear();
    }
    updateCompareFrame(m_playing);
    refreshAnnotations();
    refreshFilter();
    if (loc.source->id != m_lastSourceId) {  // moved onto another clip
        m_lastSourceId = loc.source->id;
        refreshProjectTree();
        m_clipList->viewport()->update();
        refreshProperties();
        updateTitle();
        refreshMotionGraph();
    }
    m_timeline->setCurrent(global);
    m_clipBar->setClip(loc.source->displayName(), loc.segment->count, global - loc.segment->start, loc.segment->start, loc.segment->offset);
    if (m_editor && m_editorDock && m_editorDock->isVisible()) m_editor->setCurrent(m_soloId.isEmpty() ? global : -1);
    updateReadout();
    if (m_graph && m_graphDock && m_graphDock->isVisible()) m_graph->setCurrentFrame(int(loc.rawLocal));
    if (m_notesDock && m_notesDock->isVisible()) syncNoteEditor();
}

void MainWindow::step(int delta)
{
    if (m_playing) stopPlayback();
    goToFrame(m_current + delta);
    scrubSound();
}

QVector<qint64> MainWindow::playbackBookmarks() const
{
    QVector<qint64> frames;
    const auto [first, last] = playRange();
    if (m_stepEvery > 1) {
        // "Play on Ns" holds every Nth frame of the play range (animation shot on twos / threes);
        // steppedFrameFor() computes the held frame, this only marks the start.
        frames.append(first);
        return frames;
    }
    for (const auto& seg : m_seq.segments)
        if (const Source* src = m_project.find(seg.sourceId))
            for (int f : src->bookmarkFrames()) {
                const qint64 global = Sequence::globalIn(seg, f);
                if (global >= 0 && global >= first && global <= last) frames.append(global);
            }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

int64_t MainWindow::steppedFrameFor(int64_t position) const
{
    if (m_stepEvery > 1) {
        const int64_t first = playRange().first;
        return first + ((std::max(first, position) - first) / m_stepEvery) * m_stepEvery;
    }
    if (m_steppedFrames.isEmpty()) return position;
    auto it = std::upper_bound(m_steppedFrames.begin(), m_steppedFrames.end(), position);
    return it == m_steppedFrames.begin() ? m_steppedFrames.front() : *std::prev(it);
}

void MainWindow::togglePlay(bool stepped)
{
    if (m_seq.total <= 0) return;
    if (m_shuttleTimer.isActive()) {  // Space stops J / L shuttling
        m_shuttleTimer.stop();
        m_shuttle = 0;
        return;
    }
    if (m_playing) {
        stopPlayback();
        return;
    }
    const auto [a, b] = playRange();
    if (m_current < a || m_current >= b) goToFrame(a);
    if (!stepped) m_stepEvery = 0;
    m_steppedFrames = stepped ? playbackBookmarks() : QVector<qint64>{};
    if (stepped && m_steppedFrames.isEmpty()) {
        showToast(L("Add a bookmark inside the playback range first (K).", "재생 범위 안에 북마크를 먼저 추가하세요 (K)."));
        return;
    }
    m_stepped = stepped;
    m_timeline->setStepped(stepped, m_stepEvery > 1 ? L("On %1s", "%1프레임씩").arg(m_stepEvery) : QString());
    if (stepped) { m_audio.stop(); m_audioPlayingId.clear(); }
    m_playing = true;
    m_playTimer.setInterval(std::clamp(int(250.0 / viewFps()), 1, 8));
    m_scrubSoundPending = false;
    m_playBtn->setIcon(Icons::icon("pause"));
    m_clock.start();
    m_nextDueNs = qint64(1e9 / viewFps());
    m_statClock.start();
    m_statFrames = 0;
    goToFrame(m_current);
    m_playTimer.start();
    syncAudio(true);
}

void MainWindow::stopPlayback()
{
    const bool wasPlaying = m_playing;
    m_playing = false;
    m_shuttle = 0;
    m_shuttleTimer.stop();
    m_stepped = false;
    m_stepEvery = 0;
    m_timeline->setStepped(false);
    m_playTimer.stop();
    m_audio.stop();
    m_audioPlayingId.clear();
    m_scrubSoundPending = false;
    if (!wasPlaying) return;
    m_playBtn->setIcon(Icons::icon("play"));
    m_measuredFps = 0;
    m_timeline->setRates(viewFps(), 0);
    goToFrame(m_current);
}

bool MainWindow::audioDrives()
{
    if (timelineAudio()) return m_audio.playingMix();
    if (!m_audio.isPlaying() || m_audioPlayingId.isEmpty()) return false;
    if (compareAudioFromB()) return m_audioPlayingId == m_compareId;
    const Loc loc = locate(m_current);
    return loc && loc.source->id == m_audioPlayingId;
}

void MainWindow::countShownFrame()
{
    ++m_statFrames;
    const qint64 statNs = m_statClock.nsecsElapsed();
    if (statNs > 500'000'000) {
        m_measuredFps = m_statFrames * 1e9 / statNs;
        m_statFrames = 0;
        m_statClock.restart();
    }
}

void MainWindow::onPlayTick()
{
    if (!m_playing) return;
    if (m_soloId.isEmpty() && !m_stepped) {
        timelineTick();
        return;
    }
    const auto [a, b] = playRange();
    const Loc current = locate(m_current);
    if (!current && !m_stepped) { stopPlayback(); return; }
    // At a loop/seek boundary the requested image may still be decoding. Do not
    // let the soundtrack run ahead while the old image is frozen on screen.
    if (current && m_shownKey != current.source->id + ':' + QString::number(current.local)) {
        m_audio.stop(); m_audioPlayingId.clear();
        current.server->request(current.local, true, current.segment->sourceFrame(0), current.segment->sourceFrame(current.segment->count - 1));
        m_clock.restart(); m_nextDueNs = qint64(1e9 / viewFps());
        return;
    }

    if (m_stepped) {
        const qint64 now = m_clock.nsecsElapsed();
        if (now < m_nextDueNs) return;
        const auto frames = m_stepEvery > 1 ? m_steppedFrames : playbackBookmarks();
        if (frames.isEmpty()) { stopPlayback(); return; }
        m_steppedFrames = frames;
        const qint64 frameNs = qint64(1e9 / viewFps());
        const qint64 steps = m_realtime ? 1 + (now - m_nextDueNs) / frameNs : 1;
        const qint64 first = m_steppedFrames.front();
        const qint64 length = b - first + 1;
        const qint64 position = first + ((std::max<qint64>(first, m_steppedPosition) - first + steps) % length);
        const qint64 shown = steppedFrameFor(position);
        const Loc target = locate(shown);
        if (target && !target.server->isCached(target.local)) { target.server->request(target.local, true, target.segment->sourceFrame(0), target.segment->sourceFrame(target.segment->count - 1)); return; }
        if (!target && !m_seq.at(shown)) return;
        if (shown != m_current) { goToFrame(shown); countShownFrame(); }
        m_steppedPosition = position;
        m_nextDueNs += steps * frameNs;
        if (now - m_nextDueNs > 3 * frameNs) m_nextDueNs = now + frameNs;
        return;
    }

    // With sound, the sound card is the clock: show the frame that is being heard right now.
    // Two independent clocks (timer + audio) drift apart and had to be re-aligned with audible jumps.
    if (audioDrives()) {
        const Loc loc = locate(m_current);
        const MediaInfo& ai = loc.server->info();
        const double nativeFps = std::max(1.0, ai.fps);
        int64_t heardLocal = 0;
        if (compareAudioFromB()) {
            // B's sound is playing: find the A frame whose B picture is being heard.
            const MediaInfo& bi = m_media.server(m_compareId)->info();
            if (m_compareByTime) {
                const double pos = m_audio.position() - m_audioOffsetMs / 1000.0;
                heardLocal = ai.frameAt(ai.startTime() + (pos - bi.startTime()) * viewFps() / nativeFps);
            } else {
                const double pos = m_audio.position() - m_audioOffsetMs / 1000.0 * (viewFps() / std::max(1.0, bi.fps));
                heardLocal = bi.frameAt(pos);
            }
        } else {
            // Picture = what is being heard, minus the user's output-latency offset (Audio > Audio Offset).
            heardLocal = ai.frameAt(m_audio.position() - m_audioOffsetMs / 1000.0 * (viewFps() / nativeFps));
        }
        const int64_t heard = loc.segment->timing.timed()
            ? loc.segment->start + int64_t(std::floor((ai.timeOf(heardLocal) - loc.segment->timeStart) * loc.segment->timing.fps + 1e-6))
            : loc.segment->start + heardLocal - loc.segment->offset;
        const int64_t segEnd = loc.segment->start + loc.segment->count - 1;
        const int64_t limit = std::min(b, segEnd);
        if (heard > limit && (m_realtime || m_current >= limit)) {
            // Past Out (wrap to In) or past this clip in a timeline (continue with the next one).
            goToFrame(segEnd < b ? segEnd + 1 : a);
            syncAudio(true);
            countShownFrame();
            return;
        }
        // Audio time can advance while the UI is busy. The user's no-skip mode
        // must also apply to the audio clock, including the very first tick.
        // An older/stale device position must never rewind a playing clip.
        if (heard > m_current && heard >= a) {
            const int64_t next = m_realtime ? heard : std::min(heard, m_current + 1);
            const bool catchingUp = next < heard;
            const Loc target = locate(next);
            if (target && target.server->isCached(target.local)) {
                goToFrame(next);
                if (catchingUp) syncAudio(false); // re-anchor only when outside normal jitter tolerance
                countShownFrame();
            } else if (target) {
                m_audio.stop(); m_audioPlayingId.clear();
                goToFrame(next);
                m_clock.restart(); m_nextDueNs = qint64(1e9 / viewFps());
            }
        }
        return;
    }

    const qint64 now = m_clock.nsecsElapsed();
    if (now < m_nextDueNs) return;

    const qint64 frameNs = qint64(1e9 / viewFps());
    const int64_t len = b - a + 1;
    const int64_t steps = m_realtime ? 1 + (now - m_nextDueNs) / frameNs : 1;
    const int64_t base = (m_current < a || m_current > b) ? a - 1 : m_current;
    const int64_t next = a + ((base - a + steps) % len + len) % len;

    const Loc loc = locate(next);
    if (!loc) return;
    if (!loc.server->isCached(loc.local)) {
        loc.server->request(loc.local, true, loc.segment->sourceFrame(0), loc.segment->sourceFrame(loc.segment->count - 1));
        return;
    }
    const bool wrapped = next < m_current;
    goToFrame(next);
    syncAudio(wrapped);  // starts the audio clock as soon as a clip with sound comes up
    m_nextDueNs += steps * frameNs;
    if (now - m_nextDueNs > 3 * frameNs) m_nextDueNs = now + frameNs;
    countShownFrame();
}

void MainWindow::onFrameReady(const QString& id, qint64 index)
{
    const Loc loc = locate(m_current);
    // Other tracks' pictures (and A's previous frame for motion highlight) arrive here too.
    if (loc && m_soloId.isEmpty() && (m_layerSources.contains(id) || (id == loc.source->id && m_layerExtra.contains(index)))) {
        m_layersKey.clear();
        updateLayers(loc);
    }
    if (loc && m_filter.mode == AnalysisFilter::Mode::MotionHighlight && id == loc.source->id && index == loc.local - 1) refreshFilter();
    if (loc && m_filter.usesAutoThreshold() && id == loc.source->id && index == loc.local && !m_otsu.contains(id)) refreshFilter();
    if (loc && loc.source->id == id && loc.local == index) {
        const QString key = id + ':' + QString::number(index);
        if (key != m_shownKey)
            if (FrameCache::Ptr img = loc.server->frame(index)) {
                m_view->setFrame(img);
                m_shownKey = key;
                playPendingScrubSound();
                if (m_playing && !m_stepped && !m_audio.isPlaying()) syncAudio(true);
            }
    }
    if (m_compareMode != CompareMode::Off && id == m_compareId && loc && index == compareFrameFor(loc.rawLocal))
        updateCompareFrame(m_playing);
    const int ghostReach = std::max(m_onionVideoBefore, m_onionVideoAfter) * std::max(1, m_onionStep);
    if (m_onion && (!m_playing || m_onionWhilePlaying) && ghostReach > 0 && loc) {
        const bool nearA = id == loc.source->id && std::abs(index - loc.local) <= ghostReach;
        const bool nearB = m_compareMode != CompareMode::Off && id == m_compareId &&
                           std::abs(index - compareFrameFor(loc.rawLocal)) <= ghostReach;
        if (nearA || nearB) refreshAnnotations();
    }
}

std::pair<int64_t, int64_t> MainWindow::playRange() const
{
    const int64_t last = std::max<int64_t>(0, m_seq.total - 1);
    int64_t in = 0, out = 0;
    if (m_soloId.isEmpty()) {
        in = m_project.timeline.loopIn;
        out = m_project.timeline.loopOut;
    } else {
        in = m_soloIn;
        out = m_soloOut;
    }
    if (loopEnabled() && out >= in) return { std::clamp<int64_t>(in, 0, last), std::clamp<int64_t>(out, 0, last) };
    return { 0, last };
}

bool MainWindow::loopEnabled() const
{
    return m_soloId.isEmpty() ? m_project.timeline.loopEnabled : m_soloLoop;
}

double MainWindow::viewFps() const
{
    if (!m_soloId.isEmpty())
        if (const Source* s = m_project.find(m_soloId)) return std::max(1.0, s->fps);
    return std::max(1.0, m_project.timeline.fps);
}

void MainWindow::setViewFps(double fps)
{
    if (!m_soloId.isEmpty()) {
        if (Source* s = m_project.find(m_soloId)) s->fps = fps;
    } else {
        const double ratio = fps / std::max(1.0, m_project.timeline.fps);
        m_current = std::llround(m_current * ratio);
        m_project.timeline.loopIn = int(std::llround(m_project.timeline.loopIn * ratio));
        m_project.timeline.loopOut = int(std::llround((m_project.timeline.loopOut + 1) * ratio)) - 1;
        editTimeline([&](TimelineDoc& doc) {
            for (TimelineClip& c : doc.clips) {
                if (c.start >= 0) c.start = std::llround(c.start * ratio);
                c.fadeIn = int(std::llround(c.fadeIn * ratio));
                c.fadeOut = int(std::llround(c.fadeOut * ratio));
            }
            doc.fps = fps;
            return true;
        }, L("Timeline Frame Rate", "타임라인 프레임 레이트"));
    }
    setDirty(true);
    m_timeline->setRates(fps, 0);
    if (m_compareMode != CompareMode::Off && !m_soloId.isEmpty()) rebuildSequence();
    invalidateMix();  // the mix is in timeline seconds
    if (m_playing) {
        m_playTimer.setInterval(std::clamp(int(250.0 / fps), 1, 8));
        m_clock.restart();
        m_nextDueNs = qint64(1e9 / fps);
        syncAudio(true);  // new speed
    }
    refreshProperties();
    updateReadout();
}

// ================================================================== compare

void MainWindow::setCompareSource(const QString& id)
{
    if (!m_media.server(id)) return;
    const Loc loc = locate(m_current);
    if (loc && loc.source->id == id) {
        showToast(L("That source is already A. Pick another one for B.", "이미 A인 소스입니다. 다른 소스를 B로 고르세요."));
        return;
    }
    m_view->commitEdits();
    if (m_pending) commitEdit();
    m_compareId = id;
    setCompareMode(m_compareMode == CompareMode::Off ? CompareMode::Wipe : m_compareMode);
}

void MainWindow::chooseA(const QString& id)
{
    if (m_compareMode != CompareMode::Off && id == m_compareId)
        swapCompare();
    else
        setSolo(id);
    refreshProjectTree();
    refreshClipList();
}

void MainWindow::chooseB(const QString& id)
{
    if (m_compareMode != CompareMode::Off && id == m_compareId)
        setCompareMode(CompareMode::Off);
    else
        setCompareSource(id);
}

void MainWindow::setCompareMode(CompareMode mode)
{
    m_view->commitEdits();
    if (m_pending) commitEdit();
    const Loc cur = locate(m_current);
    if (mode != CompareMode::Off && cur && m_compareId == cur.source->id) m_compareId.clear();  // A and B must differ
    if (mode != CompareMode::Off && !m_media.server(m_compareId)) {
        // Pick another clip automatically: the next one in the project.
        const Loc loc = locate(m_current);
        m_compareId.clear();
        for (const Source& s : m_project.sources)
            if (m_media.server(s.id) && (!loc || s.id != loc.source->id)) {
                m_compareId = s.id;
                break;
            }
        if (m_compareId.isEmpty()) {
            showToast(L("Compare needs at least two sources", "비교하려면 소스가 2개 이상 필요합니다"));
            mode = CompareMode::Off;
        }
    }
    m_compareMode = mode;
    m_view->setCompareMode(mode);
    if (m_compareBtn) m_compareBtn->setChecked(mode != CompareMode::Off);
    m_shownCompareKey.clear();

    // While comparing, A plays on its own. In timeline view playback would otherwise run on
    // into the next clip (often B itself), so A seemed to play once before B.
    if (mode != CompareMode::Off && m_soloId.isEmpty() && cur) {
        m_compareAutoSolo = true;
        setSolo(cur.source->id);
    } else if (mode == CompareMode::Off && m_compareAutoSolo) {
        m_compareAutoSolo = false;
        setSolo(QString());
    }
    if (!m_soloId.isEmpty()) rebuildSequence();  // length = longer of A and B
    updateCompareFrame(m_playing);
    refreshAnnotations();
    refreshMarks();
    if (m_playing) syncAudio(true);  // compare audio may switch between A and B
    refreshWaveform();
    refreshProjectTree();
    refreshClipList();
}

void MainWindow::swapCompare()
{
    if (m_compareMode == CompareMode::Off || m_compareId.isEmpty()) return;
    const Loc loc = locate(m_current);
    if (!loc) return;
    const QString a = loc.source->id;
    const QString b = m_compareId;
    const int64_t local = loc.local;
    m_compareId = a;
    m_soloId.clear();
    stopPlayback();
    m_soloId = b;
    rebuildSequence();
    goToFrame(std::min<int64_t>(local, m_seq.total - 1));
    refreshProjectTree();
}

void MainWindow::updateCompareFrame(bool playing)
{
    if (m_compareMode == CompareMode::Off) {
        m_view->setCompareFrame(nullptr, {});
        return;
    }
    const Loc loc = locate(m_current);
    FrameServer* bs = m_media.server(m_compareId);
    const Source* b = m_project.find(m_compareId);
    if (!loc || !bs || !b) return;
    const int64_t bl = compareFrameFor(loc.rawLocal);
    bs->request(bl, playing, 0, bs->frameCount() - 1);
    const QString key = m_compareId + ':' + QString::number(bl);
    if (key != m_shownCompareKey)
        if (FrameCache::Ptr img = bs->frame(bl)) {
            m_view->setCompareFrame(img, QSize(bs->info().width, bs->info().height));
            m_shownCompareKey = key;
        }
    m_view->setCompareLabels(loc.source->displayName(), b->displayName());
}

// ================================================================== annotations

void MainWindow::refreshAnnotations()
{
    QVector<Viewport::Item> items;
    QVector<Viewport::Ghost> ghosts;
    QVector<Viewport::FrameGhost> frameGhosts;
    const Loc loc = locate(m_current);
    if (!loc) {
        m_view->setAnnotations({});
        m_view->setCompareAnnotations({});
        m_view->setOnion({}, {});
        m_view->setTracks({}, 0, {}, 0, false);
        m_view->setPassiveTracks({}, 0);
        refreshTrackingManager();
        return;
    }
    const int local = int(loc.rawLocal);
    for (const Source::Entry& e : loc.source->renderable(local)) items.append({ loc.source->evaluated(*e.annotation, local), e.inherited });

    // Onion skin for one side, the way 2D animation packages do it:
    //  - drawings: the previous / next drawings (Keys mode) or whatever is drawn N frames away
    //    (Frames mode, every `step` frames), tinted warm before and cool after;
    //  - video: neighbouring frames before / after, every `step` frames (e.g. on twos), tinted
    //    the same way. Opacity falls off with distance from the nearest ghost.
    // Video ghosts are decoded on purpose (not just picked from the prefetch window).
    const bool showGhosts = m_onion && (!m_playing || m_onionWhilePlaying);
    auto falloff = [this](int n, int total) {
        // nearest = m_onionOpacity, farthest ~ a quarter of it
        if (total <= 1) return m_onionOpacity;
        return m_onionOpacity * (1.0 - 0.75 * double(n) / double(total - 1));
    };
    QVector<int64_t> wantA, wantB;
    // Annotation lanes switched off or locked in the timeline hide / freeze their clips.
    if (m_soloId.isEmpty()) {
        bool hidden = false, frozen = false;
        for (const TrackState& t : m_project.timeline.noteTracks) {
            hidden = hidden || !t.visible;
            frozen = frozen || t.locked;
        }
        if (hidden || frozen) {
            const QHash<QString, int> lanes = noteLanes(*loc.source);
            QVector<Viewport::Item> kept;
            for (Viewport::Item& it : items) {
                const TrackState st = m_project.timeline.noteTracks.value(lanes.value(it.annotation.id, std::max(0, it.annotation.lane)));
                if (!st.visible) continue;
                if (st.locked) it.annotation.locked = true;
                kept << it;
            }
            items = kept;
        }
    }
    if (m_filter.mode == AnalysisFilter::Mode::MotionHighlight && local > 0) wantA << local - 1;
    auto onion = [&](const Source& src, FrameServer* srv, int at, int64_t count, int side) {
        const int step = std::max(1, m_onionStep);
        const QColor before = m_onionTint ? m_onionBeforeColor : QColor();
        const QColor after = m_onionTint ? m_onionAfterColor : QColor();
        auto addDrawing = [&](int key, int evalFrame, const QColor& tint, double opacity) {
            const auto it = src.annotations.find(key);
            if (it == src.annotations.end()) return;
            for (const Annotation& a : it->second)
                ghosts.append({ src.evaluated(a, evalFrame), tint, opacity, side });  // invalid tint = own colour
        };
        const int shown = src.keyAt(at);
        if (m_onionMode == 0) {
            const QVector<int> keys = src.annotatedFrames();  // non-empty keys only
            int n = 0;
            for (auto it = keys.rbegin(); it != keys.rend() && n < m_onionBefore; ++it)
                if (*it < (shown >= 0 ? shown : at + 1)) addDrawing(*it, *it, before, falloff(n++, m_onionBefore));
            n = 0;
            for (int k : keys)
                if (k > at && n < m_onionAfter) addDrawing(k, k, after, falloff(n++, m_onionAfter));
        } else {
            // Frames mode: what is on screen d*step frames away, skipping the drawing already shown.
            QSet<int> used{ shown };
            for (int d = 1; d <= m_onionBefore; ++d) {
                const int f = at - d * step;
                const int key = f >= 0 ? src.keyAt(f) : -1;
                if (key >= 0 && !used.contains(key) && !src.annotations.at(key).isEmpty()) {
                    used.insert(key);
                    addDrawing(key, f, before, falloff(d - 1, m_onionBefore));
                }
            }
            for (int d = 1; d <= m_onionAfter; ++d) {
                const int f = at + d * step;
                const int key = f < count ? src.keyAt(f) : -1;
                if (key >= 0 && !used.contains(key) && !src.annotations.at(key).isEmpty()) {
                    used.insert(key);
                    addDrawing(key, f, after, falloff(d - 1, m_onionAfter));
                }
            }
        }
        QVector<int64_t>& want = side == 0 ? wantA : wantB;
        auto addFrame = [&](int64_t f, int n, int total, const QColor& tint) {
            if (f < 0 || f >= count) return;
            want << f;
            if (FrameCache::Ptr img = srv->frame(f))
                frameGhosts.append({ img, falloff(n, total) * 0.75, side, tint, tint.isValid() ? 0.55 : 0.0 });
        };
        // Nearest first: the viewer draws the list back to front so these end up on top.
        for (int d = 1; d <= std::max(m_onionVideoBefore, m_onionVideoAfter); ++d) {
            if (d <= m_onionVideoBefore) addFrame(int64_t(at) - int64_t(d) * step, d - 1, m_onionVideoBefore, before);
            if (d <= m_onionVideoAfter) addFrame(int64_t(at) + int64_t(d) * step, d - 1, m_onionVideoAfter, after);
        }
    };
    if (showGhosts) onion(*loc.source, loc.server, local, loc.server->frameCount(), 0);

    QVector<Viewport::Item> bItems;
    const Source* b = m_compareMode != CompareMode::Off ? m_project.find(m_compareId) : nullptr;
    FrameServer* bs = b ? m_media.server(m_compareId) : nullptr;
    const int bLocal = bs ? int(compareFrameFor(loc.rawLocal)) : 0;
    if (bs) {
        for (const Source::Entry& e : b->renderable(bLocal)) bItems.append({ b->evaluated(*e.annotation, bLocal), e.inherited });
        // B's video ghosts only where B has its own area (they would sit on A otherwise)
        if (showGhosts) onion(*b, bs, bLocal, bs->frameCount(), 1);
    }
    // Ask for ghost frames that aren't decoded yet (they arrive through onFrameReady).
    if (wantA != m_onionVideoFrames) {
        m_onionVideoFrames = wantA;
        loc.server->requestExtra(wantA + m_layerExtra);
    }
    if (bs) bs->requestExtra(wantB);
    m_view->setAnnotations(std::move(items));
    m_view->setCompareAnnotations(std::move(bItems));
    m_view->setOnion(std::move(ghosts), std::move(frameGhosts));
    // Tracks: the side being edited can be picked and moved, the other one is just shown.
    const bool onB = bs && m_view->activeSide() == 1;
    const Source& active = onB ? *b : *loc.source;
    m_view->setTracks(active.tracks, onB ? bLocal : local, m_selectedTrackId, trailFrames(), m_trackNumbers);
    if (bs) m_view->setPassiveTracks(onB ? loc.source->tracks : b->tracks, onB ? local : bLocal);
    else m_view->setPassiveTracks({}, 0);
    refreshTrackingManager();
}

void MainWindow::refreshMarks()
{
    QVector<qint64> marks;
    QVector<ReviewRange> ranges;
    m_reviewKeys.clear();
    for (const Sequence::Segment& s : m_seq.segments) {
        const Source* src = m_project.find(s.sourceId);
        if (!src) continue;
        // Everything is keyed by source frame; a trimmed clip shows only its own part.
        for (int f : src->bookmarkFrames())
            if (const int64_t g = Sequence::globalIn(s, f); g >= 0) marks << g;
        for (const auto& r : src->reviewRanges(s.sourceFrame(s.count - 1) + 1)) {
            const qint64 first = std::max<qint64>(r.first, s.sourceFrame(0)), last = std::min<qint64>(r.last, s.sourceFrame(s.count - 1));
            if (first <= last) ranges.append({ Sequence::globalIn(s, first), Sequence::globalIn(s, last) });
        }
        for (const auto& [f, list] : src->annotations)
            if (const int64_t g = Sequence::globalIn(s, f); g >= 0) m_reviewKeys.append(g);
        for (const Track& track : src->tracks)
            for (int f : track.keys)
                if (const int64_t g = Sequence::globalIn(s, f); g >= 0) m_reviewKeys.append(g);
    }
    // While comparing, B's notes show up where the playhead has to be to see them.
    if (m_compareMode != CompareMode::Off && m_seq.segments.size() == 1)
        if (const Source* b = m_project.find(m_compareId); b && m_media.server(m_compareId)) {
            const auto& seg = m_seq.segments.front();
            // Use actual time/FPS mapping; a slow A can skip individual B frames.
            const auto lower = [this, &seg](qint64 f) {
                qint64 lo = 0, hi = seg.count;
                while (lo < hi) {
                    const qint64 mid = lo + (hi - lo) / 2;
                    if (compareFrameFor(mid) < f) lo = mid + 1; else hi = mid;
                }
                return lo;
            };
            for (const auto& r : b->reviewRanges(m_media.server(m_compareId)->frameCount())) {
                const qint64 first = lower(r.first), end = lower(r.last + 1);
                if (first < end) ranges.append({seg.start + first, seg.start + end - 1});
            }
            for (const auto& [f, list] : b->annotations) {
                const qint64 a = compareALocalFor(f);
                if (a >= 0) m_reviewKeys.append(seg.start + a);
            }
            for (const auto& track : b->tracks)
                for (int f : track.keys) {
                    const qint64 a = compareALocalFor(f);
                    if (a >= 0) m_reviewKeys.append(seg.start + a);
                }
        }
    std::sort(m_reviewKeys.begin(), m_reviewKeys.end());
    m_reviewKeys.erase(std::unique(m_reviewKeys.begin(), m_reviewKeys.end()), m_reviewKeys.end());
    m_reviewRanges = mergedReviewRanges(std::move(ranges));
    m_timeline->setMarks(m_reviewKeys, marks);
    m_clipBar->setReviewMarks(m_reviewKeys, marks);
    refreshTimelineEditor();  // keys / bookmarks on the clip blocks (no-op while hidden)
}

void MainWindow::jumpReviewKey(int direction)
{
    if (m_playing) stopPlayback();
    auto it = direction > 0 ? std::upper_bound(m_reviewKeys.begin(), m_reviewKeys.end(), m_current)
                            : std::lower_bound(m_reviewKeys.begin(), m_reviewKeys.end(), m_current);
    if (direction > 0) { if (it != m_reviewKeys.end()) goToFrame(*it); }
    else if (it != m_reviewKeys.begin()) goToFrame(*std::prev(it));
}

void MainWindow::loopReviewRange(int64_t frame)
{
    if (!inReviewRange(m_reviewRanges, frame)) {
        showToast(L("Choose a red review range first.", "빨간 주석 / 편집 구간을 먼저 선택하세요."));
        return;
    }
    auto it = std::upper_bound(m_reviewRanges.begin(), m_reviewRanges.end(), frame,
                              [](qint64 f, const ReviewRange& r) { return f < r.first; });
    const ReviewRange range = *std::prev(it);
    const Loc loc = locate(frame);
    if (!loc) return;
    qint64 first = std::max<qint64>(range.first, loc.segment->start);
    qint64 last = std::min<qint64>(range.last, loc.segment->start + loc.segment->count - 1);
    auto key = std::upper_bound(m_reviewKeys.begin(), m_reviewKeys.end(), frame);
    if (key != m_reviewKeys.begin()) first = std::max(first, *std::prev(key));
    if (key != m_reviewKeys.end()) last = std::min(last, *key - 1);
    if (m_playing) stopPlayback();
    if (m_soloId.isEmpty()) {
        m_project.timeline.loopIn = int(first); m_project.timeline.loopOut = int(last);
        m_project.timeline.loopEnabled = true; setDirty(true);
    } else { m_soloIn = first; m_soloOut = last; m_soloLoop = true; }
    goToFrame(first);
    updateLoopUi();
}

void MainWindow::beginEdit(const QString& sourceId, int frame)
{
    if (m_pending) {
        if (!m_pending->isTrack && m_pending->sourceId == sourceId && m_pending->frame == frame) return;
        commitEdit();
    }
    const Source* s = m_project.find(sourceId);
    if (!s) return;
    Edit e;
    e.sourceId = sourceId;
    e.frame = frame;
    e.before = s->explicitAt(frame);
    m_pending = e;
}

void MainWindow::commitEdit()
{
    if (!m_pending) return;
    ++m_annotationRev;
    if (Source* s = m_project.find(m_pending->sourceId)) {
        if (m_pending->isTrack) {
            const Track* t = s->track(m_pending->trackId);
            m_pending->trackAfter = t ? std::optional<Track>(*t) : std::nullopt;
        } else {
            m_pending->after = s->explicitAt(m_pending->frame);
        }
    }
    const bool track = m_pending->isTrack;
    m_undo.push_back(*m_pending);
    if (m_undo.size() > 1000) m_undo.removeFirst();
    m_redo.clear();
    m_pending.reset();
    setDirty(true);
    refreshAnnotations();
    refreshMarks();
    if (track) refreshMotionGraph();
}

void MainWindow::applyEdit(const Edit& e, bool forward)
{
    if (e.isTimeline) {
        const auto& doc = forward ? e.docAfter : e.docBefore;
        if (doc) {
            // Clips and track count come back; track switches (mute, solo, eye, lock) stay as they are now.
            TimelineDoc next = m_project.timeline;
            next.clips = doc->clips;
            next.width = doc->width;
            next.height = doc->height;
            if (next.fps != doc->fps) {
                const double ratio = doc->fps / next.fps;
                m_current = std::llround(m_current * ratio);
                next.loopIn = int(std::llround(next.loopIn * ratio));
                next.loopOut = int(std::llround((next.loopOut + 1) * ratio)) - 1;
            }
            next.fps = doc->fps;
            auto merge = [](QVector<TrackState>& now, const QVector<TrackState>& then) {
                QVector<TrackState> out = then;
                for (qsizetype i = 0; i < std::min(now.size(), out.size()); ++i) out[i] = now[i];
                now = out;
            };
            merge(next.videoTracks, doc->videoTracks);
            merge(next.audioTracks, doc->audioTracks);
            merge(next.noteTracks, doc->noteTracks);
            applyTimeline(next);
        }
        setDirty(true);
        return;
    }
    Source* s = m_project.find(e.sourceId);
    if (!s) return;
    ++m_annotationRev;
    if (e.isNoteClips) {
        const auto& keys = forward ? e.keysAfter : e.keysBefore;
        if (keys) s->annotations = *keys;
        setDirty(true);
        refreshAnnotations();
        refreshMarks();
        return;
    }
    if (e.isNote) {
        const std::optional<QString>& text = forward ? e.noteAfter : e.noteBefore;
        if (text) s->notes[e.frame] = *text;
        else s->notes.erase(e.frame);
        setDirty(true);
        const int64_t g = m_seq.globalFor(e.sourceId, e.frame, m_seq.indexAt(m_current));
        if (g >= 0 && g != m_current) goToFrame(g);
        m_noteFrame = -1;  // the editor shows the restored text even when the frame didn't change
        m_noteDirty = false;
        syncNoteEditor();
        refreshNotes();
        refreshTimelineEditor();
        return;
    }
    if (e.isTrack) {
        const std::optional<Track>& t = forward ? e.trackAfter : e.trackBefore;
        s->tracks.erase(std::remove_if(s->tracks.begin(), s->tracks.end(), [&](const Track& x) { return x.id == e.trackId; }),
                        s->tracks.end());
        if (t) s->tracks.append(*t);
    } else {
        s->setExplicit(e.frame, forward ? e.after : e.before);
    }
    setDirty(true);
    // Only move the playhead if the change isn't visible from here.
    const Loc loc = locate(m_current);
    const bool visible = (loc && loc.source->id == e.sourceId &&
                          (e.isTrack || e.frame == loc.rawLocal || loc.source->keyAt(int(loc.rawLocal)) == e.frame)) ||
                         (m_compareMode != CompareMode::Off && e.sourceId == m_compareId);
    if (!visible) {
        const int64_t g = m_seq.globalFor(e.sourceId, e.frame, m_seq.indexAt(m_current));
        if (g >= 0) goToFrame(g);
    }
    refreshAnnotations();
    refreshMarks();
    if (e.isTrack) refreshMotionGraph();
}

void MainWindow::undo()
{
    m_view->commitEdits();
    if (m_pending) commitEdit();
    if (m_undo.isEmpty()) return;
    const Edit e = m_undo.takeLast();
    m_redo.push_back(e);
    applyEdit(e, false);
}

void MainWindow::redo()
{
    if (m_redo.isEmpty()) return;
    const Edit e = m_redo.takeLast();
    m_undo.push_back(e);
    applyEdit(e, true);
}

MainWindow::EditTarget MainWindow::editTarget()
{
    EditTarget t;
    const Loc loc = locate(m_current);
    if (!loc) return t;
    if (m_view->activeSide() == 1 && m_compareMode != CompareMode::Off)
        if (Source* b = m_project.find(m_compareId))
            if (FrameServer* bs = m_media.server(m_compareId)) {
                t.source = b;
                t.frame = int(compareFrameFor(loc.rawLocal));
                t.size = QSizeF(bs->info().width, bs->info().height);
                t.server = bs;
                t.frameCount = bs->frameCount();
                t.isB = true;
                return t;
            }
    t.source = loc.source;
    t.frame = int(loc.rawLocal);
    t.size = QSizeF(loc.server->info().width, loc.server->info().height);
    t.server = loc.server;
    t.frameCount = loc.server->frameCount();
    return t;
}

int64_t MainWindow::compareALocalFor(int64_t bFrame) const
{
    const Sequence::Segment* seg = m_seq.at(m_current);
    if (!seg || !m_media.server(m_compareId)) return -1;
    // compareFrameFor only ever moves forward with A, so binary-search it.
    int64_t lo = 0, hi = seg->count;
    while (lo < hi) {
        const int64_t mid = (lo + hi) / 2;
        if (compareFrameFor(mid) < bFrame) lo = mid + 1;
        else hi = mid;
    }
    return lo < seg->count && compareFrameFor(lo) == bFrame ? lo : -1;
}

void MainWindow::clearCurrentFrame()
{
    const EditTarget t = editTarget();
    if (!t) return;
    beginEdit(t.source->id, t.frame);
    t.source->setExplicit(t.frame, AnnotationList{});
    commitEdit();
}

void MainWindow::clearAllInClip()
{
    const EditTarget t = editTarget();
    if (!t || t.source->annotations.empty()) return;
    if (QMessageBox::question(this, L("Clear Clip Annotations", "클립 주석 모두 삭제"),
                              L("Delete all annotations in %1?\nThis can't be undone.", "%1의 주석을 모두 삭제할까요?\n되돌릴 수 없습니다.").arg(t.source->displayName())) != QMessageBox::Yes)
        return;
    t.source->annotations.clear();
    ++m_annotationRev;
    m_undo.erase(std::remove_if(m_undo.begin(), m_undo.end(), [&](const Edit& e) { return e.sourceId == t.source->id && !e.isTrack; }), m_undo.end());
    m_redo.clear();
    setDirty(true);
    refreshAnnotations();
    refreshMarks();
}

void MainWindow::onAnnotationCreated(Annotation a)
{
    const EditTarget t = editTarget();
    if (!t) return;
    const int f = t.frame;
    beginEdit(t.source->id, f);
    // Annotation clips: drawn with the same kind at this key = the same clip; otherwise a new one
    // on the lane above what stays visible with it.
    notes::placeNew(*t.source, int(std::max<int64_t>(t.frameCount, t.source->frameCount)), f, a,
                    m_continueDrawing || t.source->explicitAt(f).has_value());
    if (m_continueDrawing)
        t.source->editable(f).append(std::move(a));
    else
        t.source->annotations[f].append(std::move(a));  // a new key: earlier drawings stop here
    commitEdit();
}

bool MainWindow::editShownKey(const std::function<void(AnnotationList&)>& fn, bool commit)
{
    const EditTarget t = editTarget();
    if (!t) return false;
    if (t.source->keyAt(t.frame) < 0) return false;
    AnnotationList before;
    for (const auto& e : t.source->renderable(t.frame)) before.append(*e.annotation);
    AnnotationList after = before;
    fn(after);
    if (annotationsToJson(before) == annotationsToJson(after)) {
        if (commit && m_pending) commitEdit();
        return true;
    }
    // Copy the held state at the playhead before editing it. Earlier keys and
    // later explicit keys remain intact; this state holds until the next key.
    beginEdit(t.source->id, t.frame);
    t.source->editable(t.frame) = std::move(after);
    if (commit) commitEdit();
    else refreshAnnotations();
    return true;
}

void MainWindow::onEraseAt(QPointF mediaPoint, double radius)
{
    const EditTarget t = editTarget();
    if (!t) return;
    QSet<QString> hits;
    for (const Source::Entry& e : t.source->renderable(t.frame))
        if (!e.annotation->locked && AnnotationPainter::hit(t.source->evaluated(*e.annotation, t.frame), t.size, mediaPoint, radius)) hits.insert(e.annotation->id);
    if (hits.isEmpty()) return;
    // Erasing also starts a new state at the playhead, including an empty state.
    editShownKey([&](AnnotationList& list) { list.removeIf([&](const Annotation& a) { return hits.contains(a.id); }); }, false);
    m_eraseOpen = true;
}

void MainWindow::onAnnotationsEdited(QVector<Annotation> modified)
{
    const EditTarget t = editTarget();
    if (!t) return;
    QHash<QString, Annotation> byId;
    for (const Annotation& a : modified) byId.insert(a.id, t.source->evaluated(a, t.frame, true));
    editShownKey([&](AnnotationList& list) {
        for (Annotation& a : list)
            if (byId.contains(a.id)) a = byId.value(a.id);
    });
}

void MainWindow::removeAnnotations(QStringList ids)
{
    if (ids.isEmpty()) return;
    editShownKey([&](AnnotationList& list) { list.removeIf([&](const Annotation& a) { return ids.contains(a.id); }); });
}

const Annotation* MainWindow::firstSelected()
{
    const QStringList ids = m_view->selectedIds();
    if (ids.isEmpty()) return nullptr;
    const EditTarget t = editTarget();
    if (!t) return nullptr;
    for (const Source::Entry& e : t.source->renderable(t.frame))
        if (ids.contains(e.annotation->id)) return e.annotation;
    return nullptr;
}

bool MainWindow::applyToSelection(const std::function<void(Annotation&)>& fn)
{
    const QStringList ids = m_view->selectedIds();
    if (ids.isEmpty()) return false;
    // Left uncommitted so a whole slider drag / panel session becomes one undo step.
    return editShownKey([&](AnnotationList& list) {
        for (Annotation& a : list)
            if (ids.contains(a.id) && !a.locked) fn(a);
    }, false);
}

void MainWindow::copySelection()
{
    const QStringList ids = m_view->selectedIds();
    const EditTarget t = editTarget();
    if (ids.isEmpty() || !t) return;
    m_clipboard.clear();
    for (const Source::Entry& e : t.source->renderable(t.frame))
        if (ids.contains(e.annotation->id)) {
            Annotation a = t.source->evaluated(*e.annotation, t.frame);
            a.trackId.clear(); // copied notes keep their on-screen position across sources
            m_clipboard.append(a);
        }
    showToast(L("Copied %1 annotation(s)", "주석 %1개 복사").arg(m_clipboard.size()));
}

void MainWindow::pasteClipboard()
{
    const EditTarget t = editTarget();
    if (!t || m_clipboard.isEmpty()) return;
    const int f = t.frame;
    beginEdit(t.source->id, f);
    AnnotationList& list = m_continueDrawing ? t.source->editable(f) : t.source->annotations[f];
    QStringList ids;
    for (Annotation a : m_clipboard) {
        a.id = Annotation::newId();
        ids << a.id;
        list.append(a);
    }
    commitEdit();
    setTool(Tool::Select);
    m_view->setSelection(ids);
}

bool MainWindow::timelineHasFocus() const
{
    const QWidget* focus = QApplication::focusWidget();
    return m_editor && m_editorDock && m_editorDock->isVisible() && focus &&
           (focus == m_editorDock || focus == m_editor || m_editor->isAncestorOf(focus));
}

void MainWindow::deleteContextual()
{
    if (timelineHasFocus()) {
        m_editor->deleteSelection(m_editor->ripple());
        return;
    }
    if (m_view->isEditingText()) return;
    const QStringList ids = m_view->selectedIds();
    if (!ids.isEmpty()) {
        removeAnnotations(ids);
        m_view->clearSelection();
    } else if (!m_selectedTrackId.isEmpty() && (m_view->tool() == Tool::Track || m_view->tool() == Tool::Select)) {
        deleteTrack(m_selectedTrackId);
    } else {
        clearCurrentFrame();
    }
}

void MainWindow::setOnion(bool on)
{
    m_onion = on;
    m_onionBtn->setChecked(on);
    QSettings().setValue("onion/on", on);
    refreshAnnotations();
    showToast(on ? L("Onion skin on", "어니언 스킨 켜짐") : L("Onion skin off", "어니언 스킨 꺼짐"), 1600);
}

void MainWindow::joinSelectionToTrack(bool detach)
{
    m_view->commitEdits();
    const EditTarget t = editTarget();
    const QStringList ids = m_view->selectedIds();
    if (!t || ids.isEmpty()) {
        showToast(L("Select annotations, then Shift-click a tracking marker and press Ctrl+J.",
                    "주석을 선택하고 Shift+클릭으로 트래킹 마커를 추가 선택한 뒤 Ctrl+J를 누르세요."), 4500);
        return;
    }
    const Track* track = detach ? nullptr : t.source->track(m_selectedTrackId);
    const auto anchor = track ? track->at(t.frame) : std::nullopt;
    if (!detach && !anchor) {
        showToast(L("Select a tracking marker on this frame with Shift-click.", "현재 프레임의 트래킹 마커를 Shift+클릭으로 선택하세요."));
        return;
    }
    editShownKey([&](AnnotationList& list) {
        for (Annotation& a : list) {
            if (!ids.contains(a.id) || a.locked) continue;
            a = t.source->evaluated(a, t.frame);
            a.trackId = detach ? QString() : track->id;
            a.trackAnchor = detach ? QPointF() : *anchor;
        }
    });
    showToast(detach ? L("Detached from track", "트래킹 결합 해제") : L("Joined to track", "트래킹 마커에 결합됨"));
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (m_noteEdit && obj == m_noteEdit) {
        // Frame note editor: Ctrl+Enter saves, leaving the field saves too.
        if (event->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(event);
            if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier)) {
                commitNoteEditor();
                return true;
            }
            if (k->key() == Qt::Key_Escape) {
                m_noteDirty = false;
                m_noteFrame = -1;
                syncNoteEditor();
                m_view->setFocus();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            commitNoteEditor();
        }
        return QMainWindow::eventFilter(obj, event);
    }
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
        auto* drop = static_cast<QDropEvent*>(event);
        QStringList paths;
        for (const QUrl& url : drop->mimeData()->urls())
            if (url.isLocalFile()) paths.append(url.toLocalFile());
        if (!paths.isEmpty()) {
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            if (event->type() == QEvent::Drop) openPaths(paths);
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ================================================================== tracking

void MainWindow::buildTrackingManager()
{
    m_trackingManager = new QDockWidget(L("Tracking Manager", "트래킹 매니저"), this);
    m_trackingManager->setObjectName("TrackingManager");
    m_trackingManager->toggleViewAction()->setObjectName("ToggleTrackingManager");
    m_trackingManager->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* panel = new QWidget(m_trackingManager);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    m_showTracksAction = new QAction(L("Show Tracks", "트랙 표시"), this);
    m_showTracksAction->setObjectName("ShowTracks");
    m_showTracksAction->setCheckable(true);
    m_showTracksAction->setChecked(true);
    auto* show = new QCheckBox(L("Show all tracking overlays", "전체 트래킹 오버레이 표시"), panel);
    show->setObjectName("TrackingShowOverlays");
    show->setChecked(true);
    connect(show, &QCheckBox::toggled, m_showTracksAction, &QAction::setChecked);
    connect(m_showTracksAction, &QAction::toggled, show, &QCheckBox::setChecked);
    connect(m_showTracksAction, &QAction::toggled, m_view, &Viewport::setShowTracks);
    layout->addWidget(show);
    m_trackSourceLabel = new QLabel(panel);
    m_trackSourceLabel->setWordWrap(true);
    layout->addWidget(m_trackSourceLabel);
    m_trackTree = new QTreeWidget(panel);
    m_trackTree->setObjectName("TrackingManagerTracks");
    m_trackTree->setStyleSheet("QHeaderView::section { color: #e5e3db; }");
    m_trackTree->setColumnCount(4);
    m_trackTree->setHeaderLabels({ L("Track", "트랙"), L("Show", "표시"),
                                 L("Hide before creation", "생성 이전 숨김"), L("Created", "생성 프레임") });
    m_trackTree->setRootIsDecorated(false);
    m_trackTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_trackTree->setMinimumWidth(390);
    m_trackTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int col = 1; col < 4; ++col) m_trackTree->header()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    layout->addWidget(m_trackTree, 1);
    auto* hint = new QLabel(L("Select a track to edit it. Visibility applies to its marker and trail.\n"
                              "Turn off ‘Hide before creation’ to show earlier tracked frames.",
                              "트랙을 선택하여 편집하세요. 표시 설정은 마커와 경로에 적용됩니다.\n"
                              "‘생성 이전 숨김’을 끄면 생성 전의 추적 결과도 표시합니다."), panel);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    connect(m_trackTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!item) return;
        m_selectedTrackId = item->data(0, Qt::UserRole).toString();
        refreshAnnotations();
    });
    connect(m_trackTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 1 && column != 2) return;
        const EditTarget at = editTarget();
        if (!at || m_trackJob || at.source->id != m_trackTree->property("sourceId").toString()) return;
        const QString id = item->data(0, Qt::UserRole).toString();
        Track* track = at.source->track(id);
        if (!track) return;
        const bool on = item->checkState(column) == Qt::Checked;
        if ((column == 1 ? track->visible : track->hideBeforeCreation) == on) return;
        beginTrackEdit(*at.source, id);
        if (column == 1) track->visible = on;
        else track->hideBeforeCreation = on;
        commitEdit();
    });
    m_trackingManager->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, m_trackingManager);
    m_trackingManager->hide();
    connect(m_trackingManager, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) refreshTrackingManager();
    });
}

void MainWindow::refreshTrackingManager()
{
    if (!m_trackingManager || !m_trackingManager->isVisible()) return;
    const EditTarget at = editTarget();
    const QSignalBlocker blocker(m_trackTree);
    const QString sourceId = at ? at.source->id : QString();
    bool rebuild = m_trackTree->property("sourceId").toString() != sourceId ||
                   m_trackTree->topLevelItemCount() != (at ? at.source->tracks.size() : 0);
    if (!rebuild && at)
        for (int i = 0; i < at.source->tracks.size(); ++i)
            if (m_trackTree->topLevelItem(i)->data(0, Qt::UserRole).toString() != at.source->tracks[i].id) rebuild = true;
    if (rebuild) m_trackTree->clear();
    m_trackTree->setProperty("sourceId", sourceId);
    m_trackTree->setEnabled(at && !m_trackJob);
    m_trackSourceLabel->setText(!at ? L("No active clip", "활성 클립 없음") :
        at.source->displayName() + (at.source->tracks.isEmpty() ? L(" · No tracks. Place a marker with M.", " · 트랙 없음. M 도구로 마커를 추가하세요.") : QString()));
    QTreeWidgetItem* selected = nullptr;
    if (at) for (int i = 0; i < at.source->tracks.size(); ++i) {
        const Track& track = at.source->tracks[i];
        auto* item = rebuild ? new QTreeWidgetItem(m_trackTree) : m_trackTree->topLevelItem(i);
        item->setData(0, Qt::UserRole, track.id);
        item->setText(0, track.name);
        item->setIcon(0, swatchIcon(track.color));
        item->setCheckState(1, track.visible ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(2, track.hideBeforeCreation ? Qt::Checked : Qt::Unchecked);
        item->setText(3, QString::number(track.creationFrame()));
        item->setToolTip(2, L("Hide this marker and its trail before frame %1", "프레임 %1 이전에는 이 마커와 경로를 숨깁니다").arg(track.creationFrame()));
        if (track.id == m_selectedTrackId) selected = item;
    }
    m_trackTree->setCurrentItem(selected);
}

void MainWindow::beginTrackEdit(Source& s, const QString& trackId)
{
    if (m_pending) commitEdit();
    Edit e;
    e.sourceId = s.id;
    e.isTrack = true;
    e.trackId = trackId;
    const Track* t = s.track(trackId);
    e.trackBefore = t ? std::optional<Track>(*t) : std::nullopt;
    e.frame = 0;
    m_pending = e;
}

void MainWindow::placeTrack(QPointF normalized)
{
    const EditTarget at = editTarget();
    if (!at || m_trackJob) return;
    Track t;
    t.id = QStringLiteral("t") + QUuid::createUuid().toString(QUuid::Id128).left(10);
    t.name = QStringLiteral("Track %1").arg(at.source->tracks.size() + 1);
    t.color = Track::colorFor(int(at.source->tracks.size()));
    t.createdFrame = at.frame;
    t.points[at.frame] = normalized;
    t.keys.insert(at.frame);
    beginTrackEdit(*at.source, t.id);
    at.source->tracks.append(t);
    m_selectedTrackId = t.id;
    commitEdit();
    showToast(L("%1 added", "%1 추가").arg(t.name));
}

void MainWindow::moveTrack(QString id, QPointF normalized)
{
    const EditTarget at = editTarget();
    if (!at || m_trackJob) return refreshAnnotations();
    Track* t = at.source->track(id);
    if (!t) return;
    beginTrackEdit(*at.source, id);
    t->points[at.frame] = normalized;
    t->keys.insert(at.frame);
    m_selectedTrackId = id;
    commitEdit();
}

void MainWindow::deleteTrack(const QString& id)
{
    const EditTarget at = editTarget();
    if (!at || id.isEmpty() || m_trackJob || !at.source->track(id)) return;
    beginTrackEdit(*at.source, id);
    at.source->tracks.erase(std::remove_if(at.source->tracks.begin(), at.source->tracks.end(),
                                           [&](const Track& t) { return t.id == id; }),
                            at.source->tracks.end());
    if (m_selectedTrackId == id) m_selectedTrackId.clear();
    commitEdit();
}

void MainWindow::startTracking(int direction)
{
    if (m_trackJob) return;
    const EditTarget loc = editTarget();  // A, or B while annotating B
    if (!loc) return;
    if (m_playing) stopPlayback();
    Track* t = loc.source->track(m_selectedTrackId);
    if (!t && loc.source->tracks.size() == 1) t = &loc.source->tracks.first();
    const int start = loc.frame;
    if (!t || !t->at(start)) {
        showToast(L("No tracking marker on this frame. Click a point with the Tracker (M).", "이 프레임에 트래킹 마커가 없습니다. 트래커(M)로 지점을 클릭하세요."), 3500);
        setTool(Tool::Track);
        return;
    }
    // Run up to (not over) the next hand-placed key in that direction.
    int end = direction > 0 ? int(loc.frameCount - 1) : 0;
    for (int k : t->keys)
        if (direction > 0 ? (k > start && k - 1 < end) : (k < start && k + 1 > end)) end = direction > 0 ? k - 1 : k + 1;
    if (end == start) return;

    beginTrackEdit(*loc.source, t->id);
    for (auto it = t->points.begin(); it != t->points.end();) {
        const bool inside = direction > 0 ? (it->first > start && it->first <= end) : (it->first < start && it->first >= end);
        it = (inside && !t->keys.count(it->first)) ? t->points.erase(it) : std::next(it);
    }

    auto job = std::make_shared<TrackJob>();
    job->sourceId = loc.source->id;
    job->trackId = t->id;
    job->total = std::abs(end - start);
    m_trackJob = job;
    const MediaRef media = loc.source->media;
    const double fps = loc.source->fps;
    const QPointF startPos = *t->at(start);

    m_trackThread = QThread::create([job, media, fps, start, end, direction, startPos] {
      try {
        QString err;
        auto src = MediaLibrary::openSource(media.isSequence(), media.path, media.frames, fps, &err);
        if (!src) {
            job->error = err;
            job->done = true;
            return;
        }
        QImage img;
        auto gray = [&](int f, GrayFrame& g) {
            if (!src->decode(f, img, &err)) return false;
            g = toGray(img);
            return true;
        };
        auto push = [&](int f, const QPointF& p, const GrayFrame& g) {
            std::lock_guard lock(job->mutex);
            job->results.push_back({ f, QPointF(p.x() / g.w, p.y() / g.h) });
        };
        GrayFrame g;
        PointTracker tracker;
        if (!gray(start, g) || !tracker.init(g, QPointF(startPos.x() * g.w, startPos.y() * g.h))) {
            job->error = L("The marker is too close to the edge, or the frame can't be read.", "마커가 가장자리에 너무 가깝거나 프레임을 읽을 수 없습니다.");
            job->done = true;
            return;
        }
        if (direction > 0) {
            for (int f = start + 1; f <= end && !job->cancel; ++f) {
                if (!gray(f, g)) break;
                const auto r = tracker.step(g);
                if (!r.ok) {
                    job->lostAt = f;
                    break;
                }
                push(f, r.pos, g);
            }
        } else {
            // Backwards: decode forward in chunks (cheap for long-GOP video), then walk each chunk in reverse.
            int hi = start - 1;
            while (hi >= end && !job->cancel && job->lostAt < 0) {
                const int lo = std::max(end, hi - 95);
                std::vector<GrayFrame> chunk;
                for (int f = lo; f <= hi && !job->cancel; ++f) {
                    GrayFrame c;
                    if (!gray(f, c)) break;
                    chunk.push_back(std::move(c));
                }
                for (int i = int(chunk.size()) - 1; i >= 0 && !job->cancel; --i) {
                    const auto r = tracker.step(chunk[size_t(i)]);
                    if (!r.ok) {
                        job->lostAt = lo + i;
                        break;
                    }
                    push(lo + i, r.pos, chunk[size_t(i)]);
                }
                hi = lo - 1;
            }
        }
        job->done = true;
      } catch (const std::exception& e) {
        // Never let an allocation failure in the tracker take the whole app down.
        job->error = QString::fromLocal8Bit(e.what());
        job->done = true;
      }
    });
    m_trackThread->start();
    m_trackPoll.start();
    showToast(L("Tracking…  Esc to stop", "추적 중… Esc로 멈춤"), 60000);
}

void MainWindow::pollTracking()
{
    if (!m_trackJob) return;
    std::vector<std::pair<int, QPointF>> batch;
    {
        std::lock_guard lock(m_trackJob->mutex);
        batch.swap(m_trackJob->results);
    }
    Source* s = m_project.find(m_trackJob->sourceId);
    Track* t = s ? s->track(m_trackJob->trackId) : nullptr;
    if (t && !batch.empty()) {
        for (const auto& [f, p] : batch)
            if (!t->keys.count(f)) t->points[f] = p;
        m_trackJob->processed += int(batch.size());
        refreshMarks();
        // Follow the tracker with the playhead so the user sees what it is doing.
        int64_t g = m_seq.globalFor(s->id, batch.back().first);
        if (m_compareMode != CompareMode::Off && s->id == m_compareId && m_seq.segments.size() == 1) {
            // tracking B: move A to where B shows that frame
            const int64_t a = compareALocalFor(batch.back().first);
            g = a >= 0 ? m_seq.segments.front().start + a : -1;
        }
        if (g >= 0) goToFrame(g); else refreshAnnotations();
        refreshMotionGraph();
        showToast(L("Tracking  %1 / %2  ·  Esc to stop", "추적 중  %1 / %2 · Esc로 멈춤").arg(m_trackJob->processed).arg(m_trackJob->total), 60000);
    }
    if (!m_trackJob->done) return;

    m_trackPoll.stop();
    m_trackThread->wait();
    delete m_trackThread;
    m_trackThread = nullptr;
    const auto job = m_trackJob;
    m_trackJob.reset();
    commitEdit();
    if (!job->error.isEmpty())
        showToast(L("Tracking failed: %1", "추적 실패: %1").arg(job->error), 4000);
    else if (job->lostAt >= 0)
        showToast(L("Lost at frame %1 (%2 frames tracked). Move the marker and track again.", "프레임 %1에서 놓쳤습니다 (%2프레임 추적). 마커를 옮긴 뒤 다시 추적하세요.").arg(job->lostAt).arg(job->processed), 5000);
    else if (job->cancel)
        showToast(L("Tracking stopped (%1 frames)", "추적을 멈췄습니다 (%1프레임)").arg(job->processed));
    else
        showToast(L("Tracking done · %1 frames", "추적 완료 · %1프레임").arg(job->processed));
}

void MainWindow::toggleBookmark()
{
    const Loc loc = locate(m_current);
    if (!loc) return;
    const int f = int(loc.local);
    if (loc.source->bookmarks.count(f)) {
        loc.source->bookmarks.erase(f);
    } else {
        AnnotationList snapshot;
        for (const Source::Entry& e : loc.source->renderable(f)) snapshot.append(loc.source->evaluated(*e.annotation, f));
        loc.source->bookmarks[f] = snapshot;
    }
    setDirty(true);
    refreshMarks();
}

void MainWindow::jumpBookmark(int direction)
{
    QVector<int64_t> all;
    for (const Sequence::Segment& s : m_seq.segments)
        if (const Source* src = m_project.find(s.sourceId))
            for (int f : src->bookmarkFrames())
                if (const int64_t g = Sequence::globalIn(s, f); g >= 0) all << g;
    std::sort(all.begin(), all.end());
    if (all.isEmpty()) return;
    if (m_playing) stopPlayback();
    if (direction > 0) {
        for (int64_t g : all)
            if (g > m_current) return goToFrame(g);
    } else {
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (*it < m_current) return goToFrame(*it);
    }
}

void MainWindow::setLoopIn()
{
    if (m_seq.total <= 0) return;
    if (m_soloId.isEmpty()) {
        TimelineDoc& t = m_project.timeline;
        t.loopIn = int(m_current);
        if (t.loopOut <= t.loopIn) t.loopOut = int(m_seq.total - 1);
        t.loopEnabled = true;
        setDirty(true);
    } else {
        m_soloIn = m_current;
        if (m_soloOut <= m_soloIn) m_soloOut = m_seq.total - 1;
        m_soloLoop = true;
    }
    updateLoopUi();
}

void MainWindow::setLoopOut()
{
    if (m_seq.total <= 0) return;
    if (m_soloId.isEmpty()) {
        TimelineDoc& t = m_project.timeline;
        t.loopOut = int(m_current);
        if (t.loopIn >= t.loopOut) t.loopIn = 0;
        t.loopEnabled = true;
        setDirty(true);
    } else {
        m_soloOut = m_current;
        if (m_soloIn >= m_soloOut) m_soloIn = 0;
        m_soloLoop = true;
    }
    updateLoopUi();
}

void MainWindow::clearLoop()
{
    if (m_soloId.isEmpty()) {
        m_project.timeline.loopEnabled = false;
        m_project.timeline.loopIn = m_project.timeline.loopOut = 0;
        setDirty(true);
    } else {
        m_soloLoop = false;
        m_soloIn = m_soloOut = 0;
    }
    updateLoopUi();
}

void MainWindow::toggleLoop()
{
    // With no endpoints yet, enabling the loop means the full sequence.
    if (!loopEnabled() && m_seq.total > 1) {
        if (m_soloId.isEmpty() && m_project.timeline.loopIn == 0 && m_project.timeline.loopOut == 0)
            m_project.timeline.loopOut = int(m_seq.total - 1);
        else if (!m_soloId.isEmpty() && m_soloIn == 0 && m_soloOut == 0) m_soloOut = m_seq.total - 1;
    }
    if (m_soloId.isEmpty()) {
        m_project.timeline.loopEnabled = !m_project.timeline.loopEnabled;
        setDirty(true);
    } else {
        m_soloLoop = !m_soloLoop;
    }
    updateLoopUi();
}

void MainWindow::updateLoopUi()
{
    const bool on = loopEnabled();
    m_loopBtn->setChecked(on);
    const int64_t in = m_soloId.isEmpty() ? m_project.timeline.loopIn : m_soloIn;
    const int64_t out = m_soloId.isEmpty() ? m_project.timeline.loopOut : m_soloOut;
    m_timeline->setLoop(on, in, out);
    if (m_seq.total > 0) goToFrame(m_current);
}

void MainWindow::setTool(Tool tool)
{
    const bool pen = tool == Tool::Pencil || tool == Tool::Pen || tool == Tool::Highlighter;
    if (pen) { m_penStyle = tool; QSettings().setValue("penStyle", int(tool)); }
    if (m_penButton) {
        m_penButton->setIcon(Icons::icon(m_penStyle == Tool::Pen ? "pen" : m_penStyle == Tool::Highlighter ? "highlighter" : "pencil"));
        m_penButton->setToolTip(L("Pen · right-click to choose Pencil / Pen / Highlighter. Shift: straight stroke", "펜 · 우클릭: 연필 / 펜 / 형광펜 선택. Shift: 직선"));
    }
    m_view->setTool(tool);
    for (auto it = m_toolButtons.begin(); it != m_toolButtons.end(); ++it) it.value()->setChecked(it.key() == tool || (it.key() == Tool::Pencil && pen));
}

void MainWindow::setColor(const QColor& color)
{
    if (!color.isValid()) return;
    if (applyToSelection([&](Annotation& a) { a.color = color; })) {
        commitEdit();
        return;  // recolour the selection, keep the brush colour
    }
    m_color = color;
    m_view->setBrush(m_color, m_brushSize, m_brushOpacity);
    m_swatch->setIcon(swatchIcon(color));
    QSettings().setValue("noteColor", color.name());
}

void MainWindow::setBrushSize(double size)
{
    m_brushSize = size;
    m_view->setBrush(m_color, m_brushSize, m_brushOpacity);
    QSettings().setValue("brushSize", size);
}

// ================================================================== panels / status

// ================================================================== audio

void MainWindow::startAudioDecode(const Source& s)
{
    if (s.media.kind == MediaRef::Kind::Canvas || s.media.isSequence() || m_audioClips.count(s.id)) return;
    const QString id = s.id;
    const QString path = s.media.path;
    // Finished decoders are released here instead of piling up until exit.
    for (auto it = m_audioThreads.begin(); it != m_audioThreads.end();) {
        if ((*it)->isFinished()) {
            delete *it;
            it = m_audioThreads.erase(it);
        } else {
            ++it;
        }
    }
    QThread* t = QThread::create([this, id, path, cancel = m_audioCancel] {
        QString err;
        std::shared_ptr<AudioClip> clip;
        try {
            clip = AudioClip::decode(path, &err, cancel.get());
        } catch (const std::bad_alloc&) {
            clip.reset();  // a soundtrack too big for memory plays silent rather than crashing
            qWarning().noquote() << "Audio: out of memory decoding" << QFileInfo(path).fileName();
        }
        if (cancel->load()) return;
        if (!err.isEmpty()) qWarning().noquote() << "Audio:" << QFileInfo(path).fileName() << err;
        QMetaObject::invokeMethod(this, [this, id, clip] {
            if (!m_project.find(id)) return;  // removed meanwhile
            m_audioClips[id] = clip;           // null = no audio track
            computePeaks(id);
            invalidateMix();
            refreshTimelineEditor();
            refreshWaveform();
            refreshProperties();
            // Building a long waveform can occupy the UI thread. Start the
            // newly decoded soundtrack after that work, at the displayed frame.
            if (m_playing) {
                m_clock.restart(); m_nextDueNs = qint64(1e9 / viewFps());
                syncAudio(true);
            }
        }, Qt::QueuedConnection);
    });
    m_audioThreads.push_back(t);
    t->start(QThread::LowPriority);
}

MainWindow::AudioTarget MainWindow::audioTarget()
{
    AudioTarget t;
    const Loc loc = locate(m_current);
    if (!loc) return t;
    if (compareAudioFromB()) {
        auto it = m_audioClips.find(m_compareId);
        if (it == m_audioClips.end() || !it->second) return t;
        FrameServer* bs = m_media.server(m_compareId);
        const double nativeB = std::max(1.0, bs->info().fps);
        t.clip = it->second;
        t.id = m_compareId;
        // Same mapping as the B picture: by seconds (real speed) or by frame number (B at A's rate).
        t.videoSec = m_compareByTime ? compareTimeFor(loc.rawLocal) : bs->info().timeOf(compareFrameFor(loc.rawLocal));
        t.speed = m_compareByTime ? 1.0 : viewFps() / nativeB;
        return t;
    }
    auto it = m_audioClips.find(loc.source->id);
    if (it == m_audioClips.end() || !it->second) return t;
    // The clip's own recorded rate, not the playback rate the user may have changed.
    const double native = std::max(1.0, loc.server->info().fps);
    t.clip = it->second;
    t.id = loc.source->id;
    t.videoSec = loc.server->info().timeOf(loc.rawLocal);  // the frame's real timestamp, not frame / fps
    t.speed = viewFps() / native;                         // slower / faster playback bends the pitch, like tape
    return t;
}

bool MainWindow::compareAudioFromB() const
{
    if (m_compareMode == CompareMode::Off || !m_media.server(m_compareId)) return false;
    if (m_compareAudio != CompareAudio::Auto) return m_compareAudio == CompareAudio::B;
    // Auto: A's sound if it has any, otherwise B's (a silent reference next to a recording with sound).
    auto has = [this](const QString& id) {
        auto it = m_audioClips.find(id);
        return it != m_audioClips.end() && it->second;
    };
    const Sequence::Segment* seg = m_seq.at(m_current);
    return !(seg && has(seg->sourceId)) && has(m_compareId);
}

void MainWindow::syncAudio(bool force)
{
    if (!m_playing || m_stepped || !m_audio.available()) return;
    if (timelineAudio()) {
        // The timeline plays its own mix (every audible sound clip), in timeline seconds.
        const std::shared_ptr<const AudioMix> mix = timelineMix();
        const Loc loc = locate(m_current);
        const bool pictureUp = !loc || m_shownKey == loc.source->id + ':' + QString::number(loc.local);
        if (!mix || mix->empty() || !pictureUp) {
            if (m_audio.isPlaying()) m_audio.stop();
            m_audioPlayingId.clear();
            return;
        }
        const double t = m_current / viewFps();
        bool restart = force || !m_audio.playingMix();
        if (!restart) restart = std::abs(m_audio.position() - t) > std::max(0.08, 1.5 / viewFps());
        if (restart) {
            m_audio.playMix(mix, t, 1.0);
            m_audioPlayingId = QStringLiteral("#mix");
        }
        return;
    }
    const Loc loc = locate(m_current);
    if (!loc || m_shownKey != loc.source->id + ':' + QString::number(loc.local)) {
        m_audio.stop(); m_audioPlayingId.clear();
        return;
    }
    const AudioTarget t = audioTarget();
    if (!t.clip) {
        if (m_audio.isPlaying()) m_audio.stop();
        m_audioPlayingId.clear();
        return;
    }
    bool restart = force || t.id != m_audioPlayingId || !m_audio.isPlaying() || m_audio.clip() != t.clip.get();
    if (!restart) {
        // The picture moves in whole frames, so allow a frame and a half before re-aligning.
        const double tolerance = std::max(0.08, 1.5 / viewFps()) * t.speed;
        restart = std::abs(m_audio.position() - t.videoSec) > tolerance;
    }
    if (restart) {
        m_audio.play(t.clip, t.videoSec, t.speed);
        m_audioPlayingId = t.id;
    }
}

void MainWindow::refreshWaveform()
{
    QVector<float> peaks;
    bool any = false;
    if (timelineAudio() && m_seq.total > 0) {
        // What the mix plays at each timeline frame: the loudest audible clip (gain applied).
        peaks.fill(0.0f, m_seq.total);
        const TimelineDoc& doc = m_project.timeline;
        const tl::LengthFn len = lengthFn();
        const int bins = TimelineEditor::kPeakBins;
        for (const TimelineClip& c : doc.clips) {
            if (!c.isAudio() || !c.enabled || c.muted || !tl::trackAudible(doc, c.track)) continue;
            auto it = m_peaks.find(c.sourceId);
            if (it == m_peaks.end()) continue;
            const QVector<float>& src = it->second;
            const tl::Span s = tl::span(c, len);
            const float gain = float(std::min(1.5, dbToGain(c.gainDb) * dbToGain(doc.audioTracks.value(c.track).gainDb)));
            for (int64_t k = 0; k < s.count && s.start + k < m_seq.total; ++k) {
                float v = 0;
                const int64_t f = s.sourceAt(k);
                for (int b = 0; b < bins && (f * bins + b) < src.size(); ++b) v = std::max(v, src[qsizetype(f * bins + b)]);
                v = std::min(1.0f, v * gain);
                float& slot = peaks[qsizetype(s.start + k)];
                slot = std::max(slot, v);
                any = any || v > 0.002f;
            }
        }
        applyWaveform(any ? peaks : QVector<float>());
        return;
    }
    auto peakOf = [](const AudioClip& clip, double t0, double t1) {
        const int64_t s0 = std::max<int64_t>(0, int64_t((t0 - clip.startTime) * clip.rate));
        const int64_t s1 = std::min<int64_t>(clip.frames(), int64_t((t1 - clip.startTime) * clip.rate) + 1);
        int peak = 0;
        for (int64_t s = s0; s < s1; s += 4)  // every 4th sample is plenty for a peak meter
            peak = std::max({ peak, std::abs(int(clip.pcm[size_t(s) * 2])), std::abs(int(clip.pcm[size_t(s) * 2 + 1])) });
        return std::pow(peak / 32768.0f, 0.6f);  // gentle curve so quiet dialogue is still visible next to loud hits
    };
    if (m_seq.total > 0 && compareAudioFromB()) {
        // Comparing with B's sound: show what is heard under each A frame.
        const AudioClip& clip = *m_audioClips.at(m_compareId);
        const MediaInfo& bi = m_media.server(m_compareId)->info();
        peaks.fill(0.0f, m_seq.total);
        for (const Sequence::Segment& seg : m_seq.segments)
            for (int64_t local = 0; local < seg.count; ++local) {
                // Compare runs on one source with no trim, so the segment frame is A's frame.
                const double t0 = m_compareByTime ? compareTimeFor(local) : bi.timeOf(local);
                const double t1 = m_compareByTime ? compareTimeFor(local + 1) : bi.timeOf(local + 1);
                const float v = peakOf(clip, t0, t1);
                peaks[qsizetype(seg.start + local)] = v;
                any = any || v > 0.002f;
            }
        applyWaveform(any ? peaks : QVector<float>());
        return;
    }
    if (m_seq.total > 0) {
        peaks.fill(0.0f, m_seq.total);
        for (const Sequence::Segment& seg : m_seq.segments) {
            auto it = m_audioClips.find(seg.sourceId);
            FrameServer* srv = m_media.server(seg.sourceId);
            if (it == m_audioClips.end() || !it->second || !srv) continue;
            const AudioClip& clip = *it->second;
            const MediaInfo& info = srv->info();
            const int64_t frames = clip.frames();
            for (int64_t i = 0; i < seg.count; ++i) {
                const int64_t local = seg.offset + i;  // source frame
                const int64_t s0 = std::max<int64_t>(0, int64_t((info.timeOf(local) - clip.startTime) * clip.rate));
                const int64_t s1 = std::min<int64_t>(frames, int64_t((info.timeOf(local + 1) - clip.startTime) * clip.rate) + 1);
                int peak = 0;
                for (int64_t s = s0; s < s1; s += 4)  // every 4th sample is plenty for a peak meter
                    peak = std::max({ peak, std::abs(int(clip.pcm[size_t(s) * 2])), std::abs(int(clip.pcm[size_t(s) * 2 + 1])) });
                // gentle curve so quiet dialogue is still visible next to loud hits
                const float v = std::pow(peak / 32768.0f, 0.6f);
                peaks[qsizetype(seg.start + i)] = v;
                any = any || v > 0.002f;
            }
        }
    }
    applyWaveform(any ? peaks : QVector<float>());
}

void MainWindow::scrubSound()
{
    if (!m_scrubAudio || m_playing || m_muted) return;
    // Sound only once the picture is there: while frames are still decoding (fast Alt-drag,
    // long-GOP backwards) the image would freeze while sound kept going.
    m_scrubSoundPending = true;
    playPendingScrubSound();
}

void MainWindow::playPendingScrubSound()
{
    if (!m_scrubSoundPending || m_playing) return;
    if (timelineAudio()) {
        const Loc loc = locate(m_current);
        if (loc && m_shownKey != loc.source->id + ':' + QString::number(loc.local)) return;
        m_scrubSoundPending = false;
        const std::shared_ptr<const AudioMix> mix = timelineMix();
        if (mix && !mix->empty()) m_audio.snippetMix(mix, m_current / viewFps(), std::max(0.045, 1.0 / viewFps()));
        return;
    }
    const Loc loc = locate(m_current);
    if (!loc || m_shownKey != loc.source->id + ':' + QString::number(loc.local)) return;
    m_scrubSoundPending = false;
    const AudioTarget t = audioTarget();
    if (t.clip) m_audio.snippet(t.clip, t.videoSec, std::max(0.045, 1.0 / viewFps()));
}

void MainWindow::setVolume(double v)
{
    m_volume = std::clamp(v, 0.0, 1.0);
    m_audio.setVolume(m_volume);
    QSettings().setValue("audio/volume", m_volume);
    if (m_volumeSlider) {
        QSignalBlocker block(m_volumeSlider);
        m_volumeSlider->setValue(int(std::round(m_volume * 100)));
    }
    if (m_volume > 0 && m_muted) setMuted(false);
}

void MainWindow::setMuted(bool muted)
{
    m_muted = muted;
    m_audio.setMuted(muted);
    QSettings().setValue("audio/muted", muted);
    if (m_muteBtn) {
        m_muteBtn->setIcon(Icons::icon(muted ? "mute" : "volume"));
        m_muteBtn->setChecked(muted);
    }
}

// ================================================================== context menus

namespace {
QAction* menuTitle(QMenu& menu, const QString& text)
{
    QAction* a = menu.addAction(text);
    a->setEnabled(false);
    return a;
}
} // namespace

void MainWindow::addSourceActions(QMenu& menu, const QString& id, int timelineRow, const QString& clipUid)
{
    auto* folders = menu.addMenu(L("Move to Folder", "폴더로 이동"));
    QStringList names = m_project.folders; names.prepend(QString());
    for (const QString& folder : names) {
        auto* action = folders->addAction(folder.isEmpty() ? L("Sources Root", "소스 최상위") : folder, this, [this, id, folder] {
            if (auto* source = m_project.find(id)) source->folder = folder;
            setDirty(true); refreshProjectTree();
        });
        action->setCheckable(true); action->setChecked(m_project.find(id) && m_project.find(id)->folder == folder);
    }
    const Source* s = m_project.find(id);
    if (!s) return;
    const bool online = m_media.server(id) != nullptr;
    const Loc loc = locate(m_current);
    const bool isA = loc && loc.source->id == id;
    const bool isB = m_compareMode != CompareMode::Off && m_compareId == id;

    menuTitle(menu, QFontMetrics(menu.font()).elidedText(s->displayName(), Qt::ElideMiddle, 280));
    menu.addSeparator();
    if (online) {
        if (!isA) menu.addAction(L("View as A", "A로 보기"), this, [this, id] { chooseA(id); });
        if (isB)
            menu.addAction(Icons::icon("compare"), L("Compare Off", "비교 끄기"), this, [this] { setCompareMode(CompareMode::Off); });
        else if (!isA)
            menu.addAction(Icons::icon("compare"), L("Compare as B", "B로 비교"), this, [this, id] { chooseB(id); });
        if (!clipUid.isEmpty())
            menu.addAction(L("Go to Clip", "클립으로 이동"), this, [this, clipUid] {
                if (!m_soloId.isEmpty()) setSolo(QString());
                const int seg = m_seq.indexOfClip(clipUid);
                if (seg >= 0) goToFrame(m_seq.segments[seg].start);
            });
        menu.addSeparator();
    }
    if (clipUid.isEmpty()) {
        menu.addAction(Icons::icon("timeline-add"), L("Add to Timeline", "타임라인 끝에 추가"), this, [this, id] { insertSources({ id }, -1); });
    } else {
        auto move = [this, clipUid](int delta) {
            editTimeline([&](TimelineDoc& doc) { return tl::swapNeighbour(doc, clipUid, delta, lengthFn()); }, L("Reorder Clips", "클립 순서"));
        };
        QAction* up = menu.addAction(Icons::icon("up"), L("Move Earlier", "앞으로"), this, [move] { move(-1); });
        up->setEnabled(timelineRow > 0);
        QAction* down = menu.addAction(Icons::icon("down"), L("Move Later", "뒤로"), this, [move] { move(1); });
        down->setEnabled(timelineRow + 1 < m_project.timeline.videoClipCount());
        menu.addAction(Icons::icon("trash"), L("Remove from Timeline", "타임라인에서 빼기"), this, [this, clipUid] { removeClip(clipUid); });
    }
    menu.addSeparator();
    const QString file = s->media.firstFile();
    menu.addAction(L("Show in Explorer", "탐색기에서 보기"), this, [file] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
    });
    menu.addAction(L("Copy Path", "경로 복사"), this, [file] { QApplication::clipboard()->setText(QDir::toNativeSeparators(file)); });
    menu.addSeparator();
    menu.addAction(Icons::icon("trash"), L("Remove from Project", "프로젝트에서 제거"), this, [this, id] { removeSource(id); });
}

void MainWindow::refreshShortcutHints(QMenu& menu)
{
    for (auto* action : menu.actions()) {
        if (action->menu()) refreshShortcutHints(*action->menu());
        const int tab = action->text().indexOf('\t');
        if (tab < 0) continue;
        const QString defaultKey = QKeySequence(action->text().mid(tab + 1), QKeySequence::PortableText).toString(QKeySequence::PortableText);
        if (auto* command = m_hotkeys.value(defaultKey)) {
            const QString key = command->shortcut().toString(QKeySequence::NativeText);
            action->setText(action->text().left(tab) + (key.isEmpty() ? QString() : "\t" + key));
        }
    }
}

void MainWindow::showViewportMenu(QPoint global, QString noteId, QString trackId)
{
    const Loc loc = locate(m_current);
    if (!loc) return;
    QMenu menu(this);

    if (!trackId.isEmpty()) {
        const EditTarget at = editTarget();
        Track* t = at ? at.source->track(trackId) : nullptr;
        if (!t) return;
        const int f = at.frame;
        menuTitle(menu, t->name);
        menu.addSeparator();
        menu.addAction(L("Track Forward\tCtrl+T", "앞으로 추적\tCtrl+T"), this, [this] { startTracking(1); });
        menu.addAction(L("Track Backward\tCtrl+Shift+T", "뒤로 추적\tCtrl+Shift+T"), this, [this] { startTracking(-1); });
        menu.addSeparator();
        auto trimmed = [this, trackId](const QString& label, std::function<bool(int)> drop) {
            return [this, trackId, drop] {
                const EditTarget l = editTarget();
                Track* tr = l ? l.source->track(trackId) : nullptr;
                if (!tr || m_trackJob) return;
                beginTrackEdit(*l.source, trackId);
                for (auto it = tr->points.begin(); it != tr->points.end();) {
                    if (drop(it->first)) {
                        tr->keys.erase(it->first);
                        it = tr->points.erase(it);
                    } else {
                        ++it;
                    }
                }
                commitEdit();
            };
        };
        menu.addAction(L("Clear This Frame", "이 프레임 지우기"), this, trimmed({}, [f](int x) { return x == f; }))->setEnabled(t->at(f).has_value());
        menu.addAction(L("Clear After", "이후 지우기"), this, trimmed({}, [f](int x) { return x > f; }));
        menu.addAction(L("Clear Before", "이전 지우기"), this, trimmed({}, [f](int x) { return x < f; }));
        menu.addSeparator();
        menu.addAction(L("Rename...", "이름 바꾸기..."), this, [this, trackId] {
            const EditTarget l = editTarget();
            Track* tr = l ? l.source->track(trackId) : nullptr;
            if (!tr) return;
            bool ok = false;
            const QString name = QInputDialog::getText(this, L("Track Name", "트랙 이름"), QString(), QLineEdit::Normal, tr->name, &ok);
            if (!ok || name.trimmed().isEmpty()) return;
            beginTrackEdit(*l.source, trackId);
            tr->name = name.trimmed();
            commitEdit();
        });
        QMenu* colors = menu.addMenu(L("Color", "색상"));
        for (int i = 0; i < 6; ++i) {
            const QColor c = Track::colorFor(i);
            colors->addAction(swatchIcon(c), QString(), this, [this, trackId, c] {
                const EditTarget l = editTarget();
                Track* tr = l ? l.source->track(trackId) : nullptr;
                if (!tr) return;
                beginTrackEdit(*l.source, trackId);
                tr->color = c;
                commitEdit();
            });
        }
        menu.addSeparator();
        menu.addAction(Icons::icon("trash"), L("Delete Track\tDelete", "트랙 삭제\tDelete"), this, [this, trackId] { deleteTrack(trackId); });
        refreshShortcutHints(menu);
    menu.exec(global);
        return;
    }

    if (!noteId.isEmpty()) {
        const Annotation* note = nullptr;
        const EditTarget t = editTarget();
        if (t)
            for (const Source::Entry& e : t.source->renderable(t.frame))
                if (e.annotation->id == noteId) note = e.annotation;
        if (!note) return;
        if (note->locked) {
            menu.addAction(L("Unlock", "잠금 해제"), this, [this, noteId] {
                editShownKey([&](AnnotationList& list) {
                    for (Annotation& a : list)
                        if (a.id == noteId) a.locked = false;
                });
                m_view->setSelection({ noteId });
            });
            menu.addAction(Icons::icon("trash"), L("Delete", "삭제"), this, [this, noteId] { removeAnnotations({ noteId }); });
            refreshShortcutHints(menu);
    menu.exec(global);
            return;
        }
        const QStringList sel = m_view->selectedIds();
        const QString count = sel.size() > 1 ? L(" (%1)", " (%1)").arg(sel.size()) : QString();
        if (sel.size() == 1 && note->type == "text")
            menu.addAction(Icons::icon("text"), L("Edit Text", "텍스트 편집"), this, [this, noteId] { m_view->editText(noteId); });
        menu.addAction(L("Copy%1\tCtrl+C", "복사%1\tCtrl+C").arg(count), this, [this] { copySelection(); });
        menu.addAction(L("Duplicate%1", "복제%1").arg(count), this, [this] {
            copySelection();
            pasteClipboard();
        });
        menu.addSeparator();
        QMenu* colors = menu.addMenu(L("Color", "색상"));
        for (const auto& [name, color] : noteColors()) {
            const QColor c = color;
            colors->addAction(swatchIcon(c), name, this, [this, c] { setColor(c); });
        }
        menu.addAction(Icons::icon("width"), L("Stroke...", "선 설정..."), this, [this, global] { openStrokePanel(global); });
        menu.addSeparator();
        auto reorder = [this](bool front) {
            const QStringList ids = m_view->selectedIds();
            editShownKey([&](AnnotationList& list) {
                AnnotationList picked, rest;
                for (const Annotation& a : list) (ids.contains(a.id) ? picked : rest).append(a);
                list = front ? rest + picked : picked + rest;
            });
        };
        menu.addAction(L("Bring to Front", "맨 앞으로"), this, [reorder] { reorder(true); });
        menu.addAction(L("Send to Back", "맨 뒤로"), this, [reorder] { reorder(false); });
        menu.addAction(L("Lock", "잠금"), this, [this] {
            const QStringList ids = m_view->selectedIds();
            editShownKey([&](AnnotationList& list) {
                for (Annotation& a : list)
                    if (ids.contains(a.id)) a.locked = true;
            });
            m_view->clearSelection();
        });
        menu.addSeparator();
        menu.addAction(Icons::icon("trash"), L("Delete%1\tDelete", "삭제%1\tDelete").arg(count), this, [this] { deleteContextual(); });
        refreshShortcutHints(menu);
    menu.exec(global);
        return;
    }

    // Empty spot on the image
    QAction* paste = menu.addAction(L("Paste\tCtrl+V", "붙여넣기\tCtrl+V"), this, [this] { pasteClipboard(); });
    paste->setEnabled(!m_clipboard.isEmpty());
    menu.addAction(L("Select All\tCtrl+A", "모두 선택\tCtrl+A"), this, [this] {
        setTool(Tool::Select);
        m_view->selectAll();
    });
    menu.addAction(Icons::icon("clear"), L("Clear Frame\tShift+Del", "현재 프레임 비우기\tShift+Del"), this, [this] { clearCurrentFrame(); });
    menu.addSeparator();
    QMenu* tools = menu.addMenu(L("Tools", "도구"));
    const QList<std::tuple<Tool, QString, QString>> list = {
        { Tool::Select, "cursor", L("Select\tV", "선택\tV") },  { Tool::Pencil, "pencil", L("Pencil\tP", "펜슬\tP") },
        { Tool::Pen, "pen", L("Pen\tN", "펜\tN") }, { Tool::Highlighter, "highlighter", L("Highlighter\tB", "형광펜\tB") },
        { Tool::Eraser, "eraser", L("Eraser\tE", "지우개\tE") }, { Tool::Text, "text", L("Text\tT", "텍스트\tT") },
        { Tool::Arrow, "arrow", L("Arrow\tA", "화살표\tA") },  { Tool::Line, "line", L("Line\tL", "직선\tL") },
        { Tool::Rect, "rect", L("Rectangle\tR", "사각형\tR") },    { Tool::Circle, "ellipse", L("Ellipse\tC", "원\tC") },
        { Tool::Track, "target", L("Tracker\tM", "트래커\tM") },
    };
    for (const auto& [tool, icon, label] : list) {
        const Tool t = tool;
        QAction* a = tools->addAction(Icons::icon(icon), label, this, [this, t] { setTool(t); });
        a->setCheckable(true);
        a->setChecked(m_view->tool() == t);
    }
    QAction* onion = menu.addAction(Icons::icon("onion"), L("Onion Skin\tShift+O", "어니언 스킨\tShift+O"), this, [this] { setOnion(!m_onion); });
    onion->setCheckable(true);
    onion->setChecked(m_onion);
    QAction* grid = menu.addAction(Icons::icon("grid"), L("Grid\tG", "그리드\tG"), this, [this] { m_gridAction->trigger(); });
    grid->setCheckable(true);
    grid->setChecked(m_view->showGrid());
    QMenu* cmp = menu.addMenu(Icons::icon("compare"), L("Compare", "비교"));
    const QList<std::tuple<CompareMode, QString>> modes = {
        { CompareMode::Off, L("Off", "끄기") },       { CompareMode::Wipe, L("Wipe", "와이프") },
        { CompareMode::SideBySide, L("Side by Side", "좌우") }, { CompareMode::TopBottom, L("Top / Bottom", "위아래") },
        { CompareMode::Blend, L("Blend", "겹쳐 보기") },
    };
    for (const auto& [mode, label] : modes) {
        const CompareMode m = mode;
        QAction* a = cmp->addAction(label, this, [this, m] { setCompareMode(m); });
        a->setCheckable(true);
        a->setChecked(m_compareMode == m);
    }
    if (m_compareMode != CompareMode::Off) {
        cmp->addSeparator();
        cmp->addAction(Icons::icon("swap"), L("Swap A / B\tTab", "A / B 바꾸기\tTab"), this, [this] { swapCompare(); });
        QAction* editB = cmp->addAction(L("Annotate B\tShift+Tab", "B에 주석\tShift+Tab"), this, [this] {
            m_view->setActiveSide(m_view->activeSide() == 1 ? 0 : 1);
        });
        editB->setCheckable(true);
        editB->setChecked(m_view->activeSide() == 1);
    }
    menu.addSeparator();
    menu.addAction(L("Set In\tI", "In 지점\tI"), this, [this] { setLoopIn(); });
    menu.addAction(L("Set Out\tO", "Out 지점\tO"), this, [this] { setLoopOut(); });
    QAction* loopAct = menu.addAction(Icons::icon("loop"), L("Loop In/Out\tCtrl+L", "In / Out 반복\tCtrl+L"), this, [this] { toggleLoop(); });
    loopAct->setCheckable(true);
    loopAct->setChecked(loopEnabled());
    QAction* clearAct = menu.addAction(L("Clear In/Out\tAlt+X", "In / Out 지우기\tAlt+X"), this, [this] { clearLoop(); });
    {
        const int64_t in = m_soloId.isEmpty() ? m_project.timeline.loopIn : m_soloIn;
        const int64_t out = m_soloId.isEmpty() ? m_project.timeline.loopOut : m_soloOut;
        clearAct->setEnabled(out > in);
        if (out > in) loopAct->setText(L("Loop In/Out  %1 – %2\tCtrl+L", "In / Out 반복  %1 – %2\tCtrl+L").arg(in).arg(out));
    }
    menu.addSeparator();
    const bool marked = loc.source->bookmarks.count(int(loc.local));
    menu.addAction(Icons::icon("bookmark"), marked ? L("Remove Bookmark\tK", "북마크 삭제\tK") : L("Add Bookmark\tK", "북마크 추가\tK"), this,
                   [this] { toggleBookmark(); });
    menu.addAction(L("Export Frame...\tCtrl+Shift+E", "현재 프레임 저장...\tCtrl+Shift+E"), this, [this] { exportCurrentFrame(); });
    menu.addSeparator();
    menu.addAction(Icons::icon("fit"), L("Fit to View\tF", "화면에 맞추기\tF"), this, [this] { m_view->fitToView(); });
    menu.addAction(L("Actual Size\tCtrl+1", "실제 크기\tCtrl+1"), this, [this] { m_view->setZoom(1.0); });
    refreshShortcutHints(menu);
    menu.exec(global);
}

void MainWindow::showTimelineMenu(int64_t frame, QPoint global)
{
    if (m_seq.total <= 0) return;
    frame = std::clamp<int64_t>(frame, 0, m_seq.total - 1);
    QMenu menu(this);
    menuTitle(menu, L("Frame %1", "프레임 %1").arg(frame));
    menu.addSeparator();
    menu.addAction(L("Go to Frame", "이 프레임으로 이동"), this, [this, frame] { goToFrame(frame); });
    menu.addSeparator();
    menu.addAction(L("Set In Here\tI", "In 지점 여기로\tI"), this, [this, frame] {
        goToFrame(frame);
        setLoopIn();
    });
    menu.addAction(L("Set Out Here\tO", "Out 지점 여기로\tO"), this, [this, frame] {
        goToFrame(frame);
        setLoopOut();
    });
    QAction* loop = menu.addAction(Icons::icon("loop"), L("Loop In/Out\tCtrl+L", "In / Out 반복\tCtrl+L"), this, [this] { toggleLoop(); });
    loop->setCheckable(true);
    loop->setChecked(loopEnabled());
    menu.addAction(L("Clear In/Out\tAlt+X", "In / Out 지우기\tAlt+X"), this, [this] { clearLoop(); });
    auto* reviewLoop = menu.addAction(L("Loop This Review Range", "이 주석 / 편집 구간 반복"), this, [this, frame] { loopReviewRange(frame); });
    reviewLoop->setEnabled(inReviewRange(m_reviewRanges, frame));
    menu.addAction(L("Previous Edit Key", "이전 편집 키"), this, [this] { jumpReviewKey(-1); });
    menu.addAction(L("Next Edit Key", "다음 편집 키"), this, [this] { jumpReviewKey(1); });
    menu.addAction(L("Stepped Playback (Bookmarks Only)", "단계 재생 (북마크만)"), this, [this] { togglePlay(true); });
    menu.addSeparator();
    menu.addAction(Icons::icon("bookmark"), L("Toggle Bookmark Here", "북마크 추가 / 삭제"), this, [this, frame] {
        goToFrame(frame);
        toggleBookmark();
    });
    menu.addSeparator();
    menu.addAction(L("Playback FPS...  %1", "재생 FPS...  %1").arg(viewFps(), 0, 'f', 3), this, [this] {
        bool ok = false;
        const double fps = QInputDialog::getDouble(this, L("Playback FPS", "재생 FPS"), L("Frames per second", "초당 프레임"), viewFps(), 1, 240, 3, &ok);
        if (ok) setViewFps(fps);
    });
    menu.addAction(m_realtimeAction);
    refreshShortcutHints(menu);
    menu.exec(global);
}

void MainWindow::openStrokePanel(QPoint global)
{
    double w = m_brushSize, o = m_brushOpacity, t = m_textSize;
    if (const Annotation* a = firstSelected()) {
        w = a->size;
        o = a->opacity;
        if (a->type == "text") t = a->fontSize * a->scale;
    }
    m_strokePanel->setValues(w, o, t);
    m_strokePanel->popupAt(global);
}

ABDelegate* MainWindow::makeABDelegate(QObject* parent)
{
    auto* d = new ABDelegate(RoleId, parent);
    d->state = [this](const QString& id) {
        if (id == QStringLiteral("@timeline")) return m_soloId.isEmpty() ? int(ABDelegate::IsA) : 0;
        const Loc loc = locate(m_current);
        const bool isA = loc && loc.source->id == id;
        const bool isB = m_compareMode != CompareMode::Off && id == m_compareId;
        int s = (isA ? ABDelegate::IsA : 0) | (isB ? ABDelegate::IsB : 0);
        if (!isA && m_media.server(id)) s |= ABDelegate::CanB;
        if (isB) s |= ABDelegate::CanB;
        return s;
    };
    d->clicked = [this](const QString& id, bool b) {
        if (id == QStringLiteral("@timeline")) {
            if (!b) setSolo(QString());
            return;
        }
        if (b) chooseB(id); else chooseA(id);
    };
    return d;
}

void MainWindow::refreshProjectTree()
{
    const Loc loc = locate(m_current);
    const QString aId = loc ? loc.source->id : QString();
    m_tree->clear();
    QFont bold = m_tree->font();
    bold.setBold(true);

    auto* sources = new QTreeWidgetItem(m_tree, { QStringLiteral("SOURCES") });
    sources->setIcon(0, Icons::icon("folder"));
    sources->setFlags(Qt::ItemIsEnabled);
    QMap<QString, QTreeWidgetItem*> folders;
    for (const QString& name : m_project.folders) {
        auto* folder = new QTreeWidgetItem(sources, {name});
        folder->setIcon(0, Icons::icon("folder"));
        folder->setData(0, RoleKind, "folder");
        folder->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        folder->setData(0, Qt::UserRole + 2, name);
        folders[name] = folder;
    }
    for (const Source& s : m_project.sources) {
        auto* it = new QTreeWidgetItem(folders.value(s.folder, sources), { s.displayName() });
        it->setIcon(0, Icons::icon(s.media.isSequence() ? "images" : "video"));
        it->setData(0, RoleId, s.id);
        it->setData(0, RoleKind, "source");
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        const bool online = m_media.server(s.id) != nullptr;
        it->setToolTip(0, QDir::toNativeSeparators(s.media.firstFile()) + (online ? QString() : L("\n(offline)", "\n(오프라인)")));
        if (!online) it->setForeground(0, Pal::textFaint);
        if (s.id == aId) it->setFont(0, bold);
    }
    auto* timelines = new QTreeWidgetItem(m_tree, { QStringLiteral("TIMELINES") });
    timelines->setIcon(0, Icons::icon("folder"));
    timelines->setFlags(Qt::ItemIsEnabled);
    auto* tl = new QTreeWidgetItem(timelines, { m_project.timeline.name });
    tl->setIcon(0, Icons::icon("film"));
    tl->setData(0, RoleKind, "timeline");
    tl->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    tl->setData(0, RoleId, QStringLiteral("@timeline"));
    if (m_soloId.isEmpty()) tl->setFont(0, bold);
    m_tree->expandAll();
}

void MainWindow::refreshClipList()
{
    m_fillingClipList = true;
    m_clipList->clear();
    for (const QString& uid : videoClipOrder()) {
        const TimelineClip& c = *m_project.timeline.clip(uid);
        const Source* s = m_project.find(c.sourceId);
        if (!s) continue;
        QString label = c.name.isEmpty() ? s->displayName() : c.name;
        if (c.trimmed()) label += QStringLiteral("  [%1–%2]").arg(c.in).arg(c.out < 0 ? std::max(0, s->frameCount - 1) : c.out);
        if (c.track > 0) label += QStringLiteral("  · V%1").arg(c.track + 1);
        auto* item = new QListWidgetItem(Icons::icon(s->media.isSequence() ? "images" : "video"), label);
        item->setData(RoleId, c.sourceId);
        item->setData(RoleKind, "source");
        item->setData(RoleClip, c.uid);
        item->setToolTip(QDir::toNativeSeparators(s->media.firstFile()));
        if (!m_media.server(c.sourceId)) item->setForeground(Pal::textFaint);
        m_clipList->addItem(item);
    }
    m_fillingClipList = false;
}

void MainWindow::refreshProperties()
{
    auto* form = static_cast<QFormLayout*>(m_props->layout());
    // The FPS box below calls back into this function from its own valueChanged signal, so its
    // row must not be destroyed synchronously (that freed the spin box mid-signal and crashed).
    while (form->rowCount() > 0) {
        const QFormLayout::TakeRowResult row = form->takeRow(0);
        for (QLayoutItem* item : { row.labelItem, row.fieldItem }) {
            if (!item) continue;
            if (QWidget* w = item->widget()) {
                w->hide();
                w->deleteLater();
            }
            delete item;
        }
    }
    form->setContentsMargins(12, 12, 12, 12);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    auto row = [&](const QString& k, const QString& v) {
        auto* key = new QLabel(k);
        key->setObjectName("FormKey");
        auto* val = new QLabel(v);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        val->setWordWrap(true);
        form->addRow(key, val);
    };
    const Loc loc = locate(m_current);
    if (!loc) {
        row(L("Timeline", "타임라인"), m_project.timeline.name);
        return;
    }
    const MediaInfo& i = loc.server->info();
    row(L("Name", "이름"), loc.source->displayName());
    row(L("Resolution", "해상도"), QStringLiteral("%1 × %2").arg(i.width).arg(i.height));
    row(L("Codec", "코덱"), i.codec);
    row(L("Pixel Format", "픽셀"), i.pixelFormat);
    row(L("Decoder", "디코더"), i.decoderName.isEmpty() ? QStringLiteral("—") : i.decoderName);
    if (!loc.segment->clipId.isEmpty() && (loc.segment->offset > 0 || loc.segment->count < i.frameCount))
        row(L("Clip Range", "클립 범위"), L("%1 – %2  (%3 of %4 frames)", "%1 – %2  (%4프레임 중 %3)")
                                               .arg(loc.segment->sourceFrame(0)).arg(loc.segment->sourceFrame(loc.segment->count - 1))
                                               .arg(loc.segment->count).arg(i.frameCount));
    row(L("Source FPS", "원본 FPS"), QString::number(i.fps, 'f', 3) + (i.variableFrameRate ? L("  (variable)", "  (가변)") : QString()));
    row(L("Frames", "프레임"), QString::number(i.frameCount));
    row(L("Duration", "길이"), L("%1 s", "%1초").arg(i.duration, 0, 'f', 2));
    {
        QString audio = L("None", "없음");
        auto it = m_audioClips.find(loc.source->id);
        if (loc.source->media.isSequence()) audio = L("None", "없음");
        else if (it == m_audioClips.end()) audio = L("Loading…", "불러오는 중…");
        else if (it->second) audio = L("Stereo 48 kHz · %1 s", "스테레오 48 kHz · %1초").arg(it->second->duration(), 0, 'f', 2);
        row(L("Audio", "오디오"), audio);
    }

    auto* fps = new QDoubleSpinBox;
    fps->setRange(1, 240);
    fps->setDecimals(3);
    fps->setValue(viewFps());
    fps->setKeyboardTracking(false);
    connect(fps, &QDoubleSpinBox::valueChanged, this, [this](double v) { setViewFps(v); });
    auto* key = new QLabel(m_soloId.isEmpty() ? L("Timeline FPS", "타임라인 FPS") : L("Playback FPS", "재생 FPS"));
    key->setObjectName("FormKey");
    form->addRow(key, fps);
    row(L("Cache", "캐시"), L("Up to %1 · GPU Y'CbCr frames", "최대 %1 · GPU Y'CbCr 프레임").arg(formatBytes(m_media.cacheBudget())));
}

void MainWindow::setDirty(bool dirty)
{
    m_dirty = dirty;
    updateTitle();
}

void MainWindow::updateTitle()
{
    QString name = m_projectPath.isEmpty() ? L("Untitled", "제목 없음") : QFileInfo(m_projectPath).completeBaseName();
    const Loc loc = locate(m_current);
    QString where = m_soloId.isEmpty() ? m_project.timeline.name : (loc ? loc.source->displayName() : QString());
    setWindowTitle(QStringLiteral("%1%2  ·  %3 — CHARONOS").arg(name, m_dirty ? " *" : "", where));
}

void MainWindow::updateReadout()
{
    if (m_seq.total <= 0) {
        m_readout->clear();
        return;
    }
    m_readout->setText(QStringLiteral("%1   %2").arg(timecode(m_current, viewFps())).arg(m_current, 5));
    updateHud();
}

void MainWindow::changeCacheBudget()
{
    const qint64 ram = totalPhysicalMemory();
    const int maxGb = std::max(1, int(ram >> 30) - 2);
    bool ok = false;
    const int gb = QInputDialog::getInt(this, L("Cache Memory", "캐시 메모리"),
        L("RAM for decoded frames (GB)\nThis PC: %1", "디코딩한 프레임용 RAM (GB)\n이 PC: %1").arg(formatBytes(ram)),
        int(m_media.cacheBudget() >> 30), 1, maxGb, 1, &ok);
    if (!ok) return;
    const qint64 bytes = qint64(gb) << 30;
    m_media.setCacheBudget(bytes);
    QSettings().setValue("cacheBudgetBytes", bytes);
    refreshProperties();
}

qint64 MainWindow::totalPhysicalMemory()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) return qint64(status.ullTotalPhys);
    return qint64(8) << 30;
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    if (m_viewerOnly) setViewerOnly(false);  // remember the real layout, not the focus view
    QSettings().setValue("geometry", saveGeometry());
    saveLayout();
    e->accept();
}


