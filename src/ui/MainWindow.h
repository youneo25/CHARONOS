#pragma once

#include "media/AudioEngine.h"
#include "media/MediaLibrary.h"
#include "model/Project.h"
#include "model/Sequence.h"
#include "ui/Viewport.h"

#include <QElapsedTimer>
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
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QThread;
class ABDelegate;
class StrokePanel;
class Timeline;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openPaths(const QStringList& paths);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    friend class FeatureRegression;
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
    void importPaths(const QStringList& paths);
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
    void addSourceActions(QMenu& menu, const QString& id, int timelineRow);
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
    Project m_project;
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

    bool m_onion = false;
    int m_onionBefore = 2;
    int m_onionAfter = 1;
    int m_onionVideo = 0;

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
    };
    QVector<Edit> m_undo;
    QVector<Edit> m_redo;
    std::optional<Edit> m_pending;
    bool m_eraseOpen = false;
};
