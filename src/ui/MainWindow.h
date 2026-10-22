#pragma once

#include "analysis/AnalysisFilter.h"
#include "media/AudioEngine.h"
#include "media/AudioMix.h"
#include "media/MediaLibrary.h"
#include "model/NoteClips.h"
#include "model/Project.h"
#include "model/Sequence.h"
#include "model/TimelineModel.h"
#include "ui/Viewport.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QMainWindow>
#include <QMap>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class ConsoleDock;
class QDockWidget;
class ClipBar;
class QLabel;
class QListWidget;
class QSlider;
class QMenu;
class QPlainTextEdit;
class QSplitter;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QThread;
class ABDelegate;
class StrokePanel;
class Timeline;
class TimelineEditor;
class MotionGraph;
class McpServer;
struct TimelineClip;

// Item data roles in the project tree / clip list. RoleId and RoleKind must stay Qt::UserRole
// and Qt::UserRole + 1: the timeline editor reads them from tree drags.
enum TreeRole { RoleId = Qt::UserRole, RoleKind, RoleFolder, RoleClip };

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openPaths(const QStringList& paths);

    // AI tool calls from the built-in MCP server (Tools > AI Assistant (MCP)). Runs on the UI thread.
    QJsonValue runTool(const QString& name, const QJsonObject& args, QString* error);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    friend class FeatureRegression;
    static constexpr int kLayoutVersion = 100;  // QMainWindow::saveState() version for 1.0 layouts
    // Where the playhead is: which clip and which of its frames.
    struct Loc {
        const Sequence::Segment* segment = nullptr;
        Source* source = nullptr;
        FrameServer* server = nullptr;
        int64_t local = 0;     // A's frame (held on its last frame past the end)
        int64_t rawLocal = 0;  // playhead position within the segment
        explicit operator bool() const { return source && server; }
    };

    // ---- UI construction
    void buildMenus();
    // A menu command with a rebindable shortcut (listed in HotKey Manager and Help > Shortcuts).
    QAction* addCommand(QMenu* menu, const QString& text, const QKeySequence& key, std::function<void()> slot);
    QWidget* buildProjectPanel();
    QWidget* buildTimelinePanel();
    QWidget* buildToolStrip();
    QWidget* buildBottomBar();
    QWidget* panelHeader(const QString& title, QWidget* panel, QLabel** titleLabel = nullptr);
    QToolButton* iconButton(const QString& icon, const QString& tip, int box = 28, int iconPx = 18);
    void showToast(const QString& text, int ms = 2400);

    // ---- media / project
    void importDialog();
    void importFolderDialog();
    void addFolder();
    void createCanvas(QString name, QSize size, int count, double fps);
    void newCanvasDialog();
    void scrubRelative(int frames);
    void runConsoleCommand(const QString& command);
    QStringList importPaths(const QStringList& paths, bool toTimeline = true);  // the new source ids
    bool addSource(MediaRef ref, double fps, bool toTimeline);
    bool openServerFor(Source& s);
    void removeSource(const QString& id);
    void openProjectDialog();
    bool loadProject(const QString& path);
    bool saveProject(bool saveAs);
    bool maybeSave();
    void newProject();
    void showNewProjectDialog(bool animation);
    void exportDialog();
    void exportCurrentFrame();

    // ---- view / navigation
    void setSolo(const QString& sourceId);  // empty = timeline
    void rebuildSequence();
    Sequence timelineSequence() const;      // the project timeline with trims, regardless of solo view
    static std::pair<int64_t, int64_t> segmentRange(const Sequence::Segment& s);
    QVector<qint64> annotatedGlobals() const;

    // ---- timeline editor (Window > Timeline)
    void buildTimelineEditor();
    void buildTimelineMenu();
    void refreshTimelineEditor(bool force = false);
    void applyWaveform(QVector<float> peaks);
    // Applies `fn` to a copy of the timeline; when it changed anything the change becomes one undo step.
    bool editTimeline(const std::function<bool(TimelineDoc&)>& fn, const QString& what = QString());
    bool commitTimeline(TimelineDoc next, const QString& what, const QStringList& select = {});
    void applyTimeline(const TimelineDoc& doc);
    tl::LengthFn lengthFn() const;
    tl::OnlineFn onlineFn() const;
    int64_t sourceLength(const QString& id) const;
    QVector<TimelineClip> clipsFor(const QStringList& sourceIds) const;  // video + linked sound
    void appendToTimeline(const QStringList& sourceIds);
    void insertSources(const QStringList& sourceIds, int index);
    void dropSources(const QStringList& sourceIds, int videoTrack, int64_t frame);
    void linkLegacyAudio();
    void removeClip(const QString& uid);
    void splitClip(const QString& uid, int64_t global);
    void splitAtPlayhead();
    void trimClip(const QString& uid, int in, int out);
    void moveClip(const QString& uid, int index);
    void showClipMenu(const QString& uid, int64_t frame, QPoint global);
    void showTrackMenu(int kind, int track, QPoint global);
    void showClipProperties(const QString& uid);
    void setTrackState(int kind, int track, const TrackState& state);
    QStringList videoClipOrder() const;  // video clips in timeline order (Sources list rows)
    QImage thumbnailFor(const QString& sourceId, qint64 frame);
    QSize canvasSize() const;
    QRectF placementFor(const TimelineClip& c, QSize media, QSize canvas) const;
    struct PlacedLayer {
        const TimelineClip* clip = nullptr;
        int64_t local = 0;
        QRectF rect;
        FrameServer* server = nullptr;
    };
    // Visible video clips at a frame, bottom first, minus whatever an opaque full-frame clip hides.
    QVector<PlacedLayer> visibleLayers(int64_t global) const;
    void updateLayers(const Loc& loc);
    bool layersReady(int64_t global);
    void showGap(int64_t global);
    void timelineTick();
    void jumpEditPoint(int direction);
    void markClip();
    void shuttle(int direction);
    // ---- annotation clips (annotation lanes of the timeline)
    bool editNoteClips(const QString& sourceId, const std::function<bool(Source&, int length)>& fn, const QString& what);
    void showNoteMenu(const QString& sourceId, const QString& key, int64_t frame, QPoint global);
    void selectNoteClip(const QString& sourceId, const QString& key);
    QHash<QString, int> noteLanes(const Source& s);
    // ---- timeline sound
    void invalidateMix();
    std::shared_ptr<const AudioMix> timelineMix();
    void computePeaks(const QString& sourceId);
    bool timelineAudio() const;  // the timeline view plays its own mix (not a single source's sound)
    // ---- analysis filters (Filter menu)
    void buildFilterMenu();
    void setFilterMode(AnalysisFilter::Mode mode);
    void showFilterSettings();
    void refreshFilter();
    void requestPlate(const QString& sourceId);
    double effectiveThreshold();

    // ---- window menu / layout
    void buildWindowMenu();
    void setPanelVisible(QWidget* panel, bool visible);
    void resetLayout();
    void restoreLayout();
    void saveLayout();
    void setViewerOnly(bool on);

    // ---- animator tools
    void buildNotesPanel();
    void refreshNotes();
    void syncNoteEditor();
    void commitNoteEditor();
    void setNote(const QString& sourceId, int frame, const QString& text);
    void exportNotes();
    void buildMotionGraph();
    void refreshMotionGraph();
    void playOnNs(int n);
    void setFlip(bool on);
    void updateHud();
    void populateOnionMenu();
    void reloadMedia();

    // ---- help
    void showShortcuts();
    void openUserGuide();
    Loc locate(int64_t global);
    void goToFrame(int64_t global);
    void step(int delta);
    void togglePlay(bool stepped = false);
    void stopPlayback();
    void onPlayTick();
    void onFrameReady(const QString& id, qint64 index);
    std::pair<int64_t, int64_t> playRange() const;
    double viewFps() const;
    void setViewFps(double fps);
    bool loopEnabled() const;

    // ---- compare
    void setCompareMode(CompareMode mode);
    void setCompareSource(const QString& id);
    void swapCompare();
    void chooseA(const QString& id);
    void chooseB(const QString& id);
    ABDelegate* makeABDelegate(QObject* parent);

    // ---- audio
    struct AudioTarget {
        std::shared_ptr<AudioClip> clip;
        QString id;
        double videoSec = 0;
        double speed = 1;
    };
    void startAudioDecode(const Source& s);
    AudioTarget audioTarget();
    bool compareAudioFromB() const;
    void syncAudio(bool force);
    void scrubSound();
    void playPendingScrubSound();
    bool audioDrives();
    void countShownFrame();
    void refreshWaveform();
    void setVolume(double v);
    void setMuted(bool muted);

    // ---- context menus
    void addSourceActions(QMenu& menu, const QString& id, int timelineRow, const QString& clipUid = QString());
    void showViewportMenu(QPoint global, QString noteId, QString trackId);
    void showTimelineMenu(int64_t frame, QPoint global);
    void refreshShortcutHints(QMenu& menu);
    void openStrokePanel(QPoint global);
    void updateCompareFrame(bool playing);
    double compareFps() const;
    int64_t compareFrameFor(int64_t aFrame) const;
    int64_t compareSpan() const;  // B's length in A frames
    double compareTimeFor(int64_t aFrame) const;  // B container time matching A's frame
    const MediaInfo* currentInfo() const;

    // ---- annotations
    void refreshAnnotations();
    void refreshMarks();
    void jumpReviewKey(int direction);
    void loopReviewRange(int64_t frame);
    QVector<qint64> playbackBookmarks() const;
    int64_t steppedFrameFor(int64_t position) const;
    struct Edit;
    void beginEdit(const QString& sourceId, int frame);
    void commitEdit();
    void applyEdit(const Edit& e, bool forward);
    void undo();
    void redo();
    void clearCurrentFrame();
    void clearAllInClip();
    void onAnnotationCreated(Annotation a);
    void onEraseAt(QPointF mediaPoint, double radius);
    void onAnnotationsEdited(QVector<Annotation> modified);
    void removeAnnotations(QStringList ids);
    bool editShownKey(const std::function<void(AnnotationList&)>& fn, bool commit = true);
    const Annotation* firstSelected();
    // Where note edits go: A at the current frame, or B (side-by-side / top-bottom) at the frame B shows.
    struct EditTarget {
        Source* source = nullptr;
        int frame = 0;
        QSizeF size;
        FrameServer* server = nullptr;
        int64_t frameCount = 0;
        bool isB = false;
        explicit operator bool() const { return source != nullptr; }
    };
    EditTarget editTarget();
    // First A frame (clip-local) whose B picture is B frame bFrame, or -1.
    int64_t compareALocalFor(int64_t bFrame) const;
    bool applyToSelection(const std::function<void(Annotation&)>& fn);
    void copySelection();
    void pasteClipboard();
    void deleteContextual();
    bool timelineHasFocus() const;
    void setOnion(bool on);
    void joinSelectionToTrack(bool detach = false);

    // ---- tracking
    void beginTrackEdit(Source& s, const QString& trackId);
    void placeTrack(QPointF normalized);
    void moveTrack(QString id, QPointF normalized);
    void deleteTrack(const QString& id);
    void startTracking(int direction);
    void pollTracking();
    void buildTrackingManager();
    void refreshTrackingManager();
    void toggleBookmark();
    void jumpBookmark(int direction);
    void setLoopIn();
    void setLoopOut();
    void clearLoop();
    void toggleLoop();
    void setTool(Tool tool);
    void setColor(const QColor& color);
    void setBrushSize(double size);

    // ---- panels / status
    void refreshProjectTree();
    void refreshClipList();
    void refreshProperties();
    void setDirty(bool dirty);
    void updateTitle();
    void updateReadout();
    void updateLoopUi();
    void changeCacheBudget();
    static qint64 totalPhysicalMemory();

    MediaLibrary m_media;
    mutable QHash<QString, std::shared_ptr<const MediaInfo>> m_timingInfo;
    Project m_project;
    // 1.1 timeline
    bool m_editorStale = true;
    std::shared_ptr<const AudioMix> m_mix;
    std::map<QString, QVector<float>> m_peaks;  // per source: TimelineEditor::kPeakBins per frame
    int m_shuttle = 0;                           // J / L speed (negative = reverse)
    QElapsedTimer m_shuttleClock;
    double m_shuttlePos = 0;
    QTimer m_shuttleTimer;
    QString m_layersKey;                         // what the viewer's layers show (skip identical updates)
    QHash<QString, QHash<QString, int>> m_laneCache;  // source -> object -> display lane
    quint64 m_annotationRev = 0;
    QHash<QString, quint64> m_laneRev;
    // analysis filters
    AnalysisFilter m_filter;
    QHash<QString, double> m_otsu;               // automatic threshold per source
    QHash<QString, FrameCache::Ptr> m_plates;    // background plates (moving silhouette)
    QSet<QString> m_platePending;
    std::vector<QThread*> m_plateThreads;
    QSet<QString> m_layerSources;                // sources drawn as layers right now
    QVector<int64_t> m_layerExtra;               // frames of A's source shown on another track
    QMap<int, QAction*> m_filterActions;
    QString m_projectPath;
    bool m_dirty = false;

    QString m_soloId;
    Sequence m_seq;
    bool m_soloLoop = false;
    int64_t m_soloIn = 0, m_soloOut = 0;

    CompareMode m_compareMode = CompareMode::Off;
    QString m_compareId;
    bool m_compareAutoSolo = false;
    bool m_compareByTime = true;  // B follows A by seconds (its own fps), not by frame number  // compare switched the view from the timeline to clip A

    // widgets
    Viewport* m_view = nullptr;
    Timeline* m_timeline = nullptr;
    ClipBar* m_clipBar = nullptr;
    QWidget* m_projectPanel = nullptr;
    QWidget* m_timelinePanel = nullptr;
    QWidget* m_toolStrip = nullptr;
    QTreeWidget* m_tree = nullptr;
    QListWidget* m_clipList = nullptr;
    QTabWidget* m_tabs = nullptr;
    QLabel* m_timelineTitle = nullptr;
    QWidget* m_props = nullptr;
    QToolButton* m_playBtn = nullptr;
    QToolButton* m_loopBtn = nullptr;
    QToolButton* m_swatch = nullptr;
    QToolButton* m_compareBtn = nullptr;
    QToolButton* m_zoomBtn = nullptr;
    QToolButton* m_onionBtn = nullptr;
    QMenu* m_onionMenu = nullptr;
    StrokePanel* m_strokePanel = nullptr;
    QLabel* m_readout = nullptr;
    QLabel* m_toast = nullptr;
    QTimer m_toastTimer;
    QMap<Tool, QToolButton*> m_toolButtons;
    ConsoleDock* m_console = nullptr;
    QMap<QString, QAction*> m_hotkeys;
    QToolButton* m_penButton = nullptr;
    Tool m_penStyle = Tool::Pencil;
    bool m_endlessScrubbing = true;
    QAction* m_gridAction = nullptr;
    QAction* m_realtimeAction = nullptr;

    QColor m_color{ 0xe5, 0x48, 0x3e };
    double m_brushSize = 3.0;
    double m_brushOpacity = 1.0;
    double m_textSize = 36.0;
    bool m_continueDrawing = false;
    AnnotationList m_clipboard;

    // Onion skin (like 2D animation packages): annotation ghosts by key or by frame distance,
    // video ghosts before / after with a step, tint colours and a falloff from the nearest ghost.
    bool m_onion = false;
    int m_onionMode = 0;  // 0 = keys, 1 = frames
    int m_onionBefore = 2;
    int m_onionAfter = 1;
    int m_onionVideoBefore = 0;
    int m_onionVideoAfter = 0;
    int m_onionStep = 1;
    double m_onionOpacity = 0.45;  // nearest ghost
    bool m_onionTint = true;
    bool m_onionWhilePlaying = false;
    QColor m_onionBeforeColor{ 0xe0, 0x6c, 0x5a };
    QColor m_onionAfterColor{ 0x5a, 0xb0, 0x7a };
    QVector<int64_t> m_onionVideoFrames;  // source frames ghosted on A (decoded on purpose)
    int m_stepEvery = 0;                  // stepped playback: 0 = bookmarks, N = every Nth frame

    // Window menu, docks, animator panels
    TimelineEditor* m_editor = nullptr;
    QDockWidget* m_editorDock = nullptr;
    QVector<float> m_wave;
    QDockWidget* m_notesDock = nullptr;
    QTreeWidget* m_notesTree = nullptr;
    QPlainTextEdit* m_noteEdit = nullptr;
    QLabel* m_noteLabel = nullptr;
    bool m_syncingNote = false;
    QString m_noteSource;   // what the note editor is showing
    int m_noteFrame = -1;
    bool m_noteDirty = false;
    QString m_graphSignature;
    QDockWidget* m_graphDock = nullptr;
    MotionGraph* m_graph = nullptr;
    QSplitter* m_leftSplit = nullptr;
    QSplitter* m_mainSplit = nullptr;
    QWidget* m_bottomBar = nullptr;
    QByteArray m_defaultState;
    bool m_viewerOnly = false;
    QByteArray m_beforeViewerOnly;
    QMap<QString, bool> m_panelsBeforeViewerOnly;
    QAction* m_flipAction = nullptr;
    QAction* m_hudAction = nullptr;
    QAction* m_hwAction = nullptr;
    QAction* m_mcpAction = nullptr;
    McpServer* m_mcp = nullptr;

    QString m_selectedTrackId;
    QDockWidget* m_trackingManager = nullptr;
    QTreeWidget* m_trackTree = nullptr;
    QLabel* m_trackSourceLabel = nullptr;
    QAction* m_showTracksAction = nullptr;
    bool m_trailTail = true;     // show only the recent past of the path
    double m_trailSeconds = 5.0;
    int trailFrames() const { return m_trailTail ? std::max(1, int(std::round(m_trailSeconds * viewFps()))) : 0; }
    bool m_trackNumbers = false;
    struct TrackJob {
        QString sourceId, trackId;
        std::mutex mutex;
        std::vector<std::pair<int, QPointF>> results;
        std::atomic<bool> cancel{ false };
        std::atomic<bool> done{ false };
        int lostAt = -1;
        int total = 0;
        int processed = 0;
        QString error;
    };
    std::shared_ptr<TrackJob> m_trackJob;
    QThread* m_trackThread = nullptr;
    QTimer m_trackPoll;

    AudioEngine m_audio;
    std::map<QString, std::shared_ptr<AudioClip>> m_audioClips;
    std::vector<QThread*> m_audioThreads;
    std::shared_ptr<std::atomic<bool>> m_audioCancel = std::make_shared<std::atomic<bool>>(false);  // closing
    QString m_audioPlayingId;
    bool m_scrubAudio = true;
    bool m_scrubSoundPending = false;
    int m_audioOffsetMs = 0;  // + = picture later (compensates audio output latency)
    enum class CompareAudio { Auto, A, B };
    CompareAudio m_compareAudio = CompareAudio::Auto;  // Auto: A if it has sound, else B
    bool m_muted = false;
    double m_volume = 0.8;
    QToolButton* m_muteBtn = nullptr;
    QSlider* m_volumeSlider = nullptr;

    int64_t m_current = 0;
    QString m_shownKey;
    QString m_lastSourceId;
    bool m_fillingClipList = false;
    QString m_shownCompareKey;

    bool m_playing = false;
    bool m_realtime = false;
    bool m_stepped = false;
    int64_t m_steppedPosition = 0;
    QVector<qint64> m_steppedFrames;
    QVector<ReviewRange> m_reviewRanges;
    QVector<qint64> m_reviewKeys;
    QTimer m_playTimer;
    QElapsedTimer m_clock;
    qint64 m_nextDueNs = 0;
    int m_statFrames = 0;
    QElapsedTimer m_statClock;
    double m_measuredFps = 0.0;
    QTimer m_cacheTimer;

    struct Edit {
        QString sourceId;
        int frame = 0;
        std::optional<AnnotationList> before;
        std::optional<AnnotationList> after;
        bool isTrack = false;
        QString trackId;
        std::optional<Track> trackBefore;
        std::optional<Track> trackAfter;
        // Timeline edits keep the whole layout (clips, tracks).
        bool isTimeline = false;
        std::shared_ptr<TimelineDoc> docBefore;
        std::shared_ptr<TimelineDoc> docAfter;
        // Annotation clip edits keep the source's keys.
        bool isNoteClips = false;
        std::shared_ptr<std::map<int, AnnotationList>> keysBefore;
        std::shared_ptr<std::map<int, AnnotationList>> keysAfter;
        // Frame note edits
        bool isNote = false;
        std::optional<QString> noteBefore;
        std::optional<QString> noteAfter;
    };
    QVector<Edit> m_undo;
    QVector<Edit> m_redo;
    std::optional<Edit> m_pending;
    bool m_eraseOpen = false;
};
