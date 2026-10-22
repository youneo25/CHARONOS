// MainWindow: timeline editing, Window menu and layout, animator panels (frame notes, motion
// graph, onion skin settings) and Help. Split from MainWindow.cpp to keep both readable.
#include "MainWindow.h"
#include "Lang.h"

#include "Icons.h"
#include "Palette.h"
#include "ui/ConsoleDock.h"
#include "ui/HotkeyManager.h"
#include "ui/MotionGraph.h"
#include "ui/Timeline.h"
#include "ui/TimelineEditor.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTextStream>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <QPainter>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

QString tc(int64_t frame, double fps)
{
    const int f = std::max(1, int(std::round(fps)));
    const int64_t secs = frame / f;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(secs / 3600, 2, 10, QChar('0'))
        .arg((secs / 60) % 60, 2, 10, QChar('0'))
        .arg(secs % 60, 2, 10, QChar('0'))
        .arg(int(frame % f), 2, 10, QChar('0'));
}

QAction* addTitle(QMenu& menu, const QString& text)
{
    QAction* a = menu.addAction(text);
    a->setEnabled(false);
    return a;
}

QIcon colorIcon(const QColor& c)
{
    QPixmap pm(28, 28);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 60), 1));
    p.setBrush(c);
    p.drawRoundedRect(QRectF(1.5, 1.5, 11, 11), 3, 3);
    return QIcon(pm);
}

} // namespace

// ============================================================================ timeline editor

int64_t MainWindow::sourceLength(const QString& id) const
{
    if (FrameServer* srv = m_media.server(id)) return srv->frameCount();
    if (const Source* s = m_project.find(id)) return s->frameCount;
    return 0;
}

tl::LengthFn MainWindow::lengthFn() const
{
    QHash<QString, SourceTiming> timings;
    for (const Source& s : m_project.sources) {
        SourceTiming t(sourceLength(s.id));
        if (FrameServer* srv = m_media.server(s.id); srv && t.frames > 0) {
            auto& info = m_timingInfo[s.id];
            if (!info || info->frameCount != srv->frameCount() || info->fps != srv->info().fps)
                info = std::make_shared<MediaInfo>(srv->info());
            t.info = info;
            t.fps = std::max(1.0, m_project.timeline.fps);
        }
        timings.insert(s.id, std::move(t));
    }
    return [timings = std::move(timings)](const QString& id) { return timings.value(id); };
}

tl::OnlineFn MainWindow::onlineFn() const
{
    return [this](const QString& id) {
        FrameServer* srv = m_media.server(id);
        return srv && srv->frameCount() > 0;
    };
}

QVector<TimelineClip> MainWindow::clipsFor(const QStringList& ids) const
{
    QVector<TimelineClip> out;
    for (const QString& id : ids) {
        const Source* s = m_project.find(id);
        if (!s) continue;
        TimelineClip v = TimelineClip::of(id);
        FrameServer* srv = m_media.server(id);
        if (srv && srv->info().hasAudio && s->media.kind == MediaRef::Kind::Video) {
            TimelineClip a = TimelineClip::of(id, TimelineClip::Kind::Audio);
            a.link = v.uid;
            v.link = a.uid;
            out << v << a;
        } else {
            out << v;
        }
    }
    return out;
}

void MainWindow::appendToTimeline(const QStringList& ids)
{
    // Import: after everything that is already there (no undo step, like 1.0's import).
    if (m_project.timeline.videoClipCount() == 0)
        if (const Source* s = m_project.find(ids.value(0))) m_project.timeline.fps = s->fps;
    tl::insertClips(m_project.timeline, clipsFor(ids), tl::duration(m_project.timeline, lengthFn()), 0, 0, false, lengthFn());
    tl::normalize(m_project.timeline, lengthFn());
    invalidateMix();
}

void MainWindow::linkLegacyAudio()
{
    // A 1.0 clip carried its sound; in 1.1 that is a linked clip on the audio track of the same number.
    TimelineDoc& doc = m_project.timeline;
    doc.linkAudioOnOpen = false;
    QVector<TimelineClip> sound;
    for (TimelineClip& c : doc.clips) {
        if (!c.isVideo() || !c.link.isEmpty()) continue;
        FrameServer* srv = m_media.server(c.sourceId);
        const Source* s = m_project.find(c.sourceId);
        if (!srv || !s || !srv->info().hasAudio || s->media.kind != MediaRef::Kind::Video) continue;
        TimelineClip a = c;
        a.uid = TimelineClip::newUid();
        a.kind = TimelineClip::Kind::Audio;
        a.link = c.uid;
        a.opacity = 1.0;
        a.scale = 1.0;
        a.posX = a.posY = 0.0;
        a.blend = BlendMode::Normal;
        c.link = a.uid;
        sound << a;
    }
    doc.clips += sound;
    tl::normalize(doc, lengthFn());
}

QStringList MainWindow::videoClipOrder() const
{
    QVector<const TimelineClip*> video;
    for (const TimelineClip& c : m_project.timeline.clips)
        if (c.isVideo()) video << &c;
    const tl::LengthFn len = lengthFn();
    std::stable_sort(video.begin(), video.end(), [&](const TimelineClip* a, const TimelineClip* b) {
        const int64_t sa = tl::span(*a, len).start, sb = tl::span(*b, len).start;
        return sa != sb ? sa < sb : a->track < b->track;
    });
    QStringList out;
    for (const TimelineClip* c : video) out << c->uid;
    return out;
}

void MainWindow::buildTimelineEditor()
{
    m_editor = new TimelineEditor;
    m_editorDock = new QDockWidget(L("Timeline", "타임라인"), this);
    m_editorDock->setObjectName("TimelineEditorDock");
    m_editorDock->setWidget(m_editor);
    m_editorDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_editorDock);
    m_editorDock->hide();  // Window > Timeline shows it
    {
        QSettings s;
        m_editor->setRipple(s.value("timeline/ripple", false).toBool());
        m_editor->setSnap(s.value("timeline/snap", true).toBool());
        m_editor->setLinkedSelection(s.value("timeline/linked", true).toBool());
    }
    m_editor->setThumbnailProvider([this](const QString& id, qint64 frame) { return thumbnailFor(id, frame); });
    connect(m_editorDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) refreshTimelineEditor(true);
    });
    connect(m_editor, &TimelineEditor::scrubbed, this, [this](qint64 frame) {
        if (!m_soloId.isEmpty()) setSolo(QString());
        if (m_playing) stopPlayback();
        goToFrame(frame);
        scrubSound();
    });
    connect(m_editor, &TimelineEditor::edited, this,
            [this](const TimelineDoc& doc, const QString& what, const QStringList& select) { commitTimeline(doc, what, select); });
    connect(m_editor, &TimelineEditor::selectionChanged, this, [this](const QStringList& uids) {
        const QSignalBlocker block(m_clipList);
        for (int i = 0; i < m_clipList->count(); ++i)
            if (uids.contains(m_clipList->item(i)->data(RoleClip).toString())) {
                m_clipList->setCurrentRow(i);
                break;
            }
    });
    connect(m_editor, &TimelineEditor::clipActivated, this, [this](const QString& uid) {
        if (!m_soloId.isEmpty()) setSolo(QString());
        if (const TimelineClip* c = m_project.timeline.clip(uid)) goToFrame(tl::span(*c, lengthFn()).start);
    });
    connect(m_editor, &TimelineEditor::contextMenuRequested, this,
            [this](const QString& uid, qint64 frame, QPoint global) { showClipMenu(uid, frame, global); });
    connect(m_editor, &TimelineEditor::trackMenuRequested, this, [this](int kind, int track, QPoint global) { showTrackMenu(kind, track, global); });
    connect(m_editor, &TimelineEditor::trackChanged, this, [this](int kind, int track, const TrackState& st) { setTrackState(kind, track, st); });
    connect(m_editor, &TimelineEditor::sourcesDropped, this, [this](const QStringList& ids, int track, qint64 frame) { dropSources(ids, track, frame); });
    connect(m_editor, &TimelineEditor::filesDropped, this, [this](const QStringList& paths, int track, qint64 frame) {
        const QString suffix = QFileInfo(paths.value(0)).suffix().toLower();
        if (suffix == Project::kExtension || suffix == QLatin1String("json")) return openPaths(paths);
        const QStringList added = importPaths(paths, false);
        if (!added.isEmpty()) dropSources(added, track, frame);
    });
    connect(m_editor, &TimelineEditor::shuttle, this, [this](int direction) { shuttle(direction); });
    connect(m_editor, &TimelineEditor::modesChanged, this, [this] {
        QSettings s;
        s.setValue("timeline/ripple", m_editor->ripple());
        s.setValue("timeline/snap", m_editor->snap());
        s.setValue("timeline/linked", m_editor->linkedSelection());
    });
    // annotation clips
    connect(m_editor, &TimelineEditor::noteSelected, this, [this](const QString& src, const QString& key) { selectNoteClip(src, key); });
    connect(m_editor, &TimelineEditor::noteMenuRequested, this,
            [this](const QString& src, const QString& key, qint64 frame, QPoint global) { showNoteMenu(src, key, frame, global); });
    connect(m_editor, &TimelineEditor::noteMoved, this, [this](const QString& src, const QString& key, int first, int lane) {
        editNoteClips(src, [&](Source& s, int length) { return notes::move(s, length, key, first, lane); }, L("Move Annotation", "주석 이동"));
    });
    connect(m_editor, &TimelineEditor::noteTrimmed, this, [this](const QString& src, const QString& key, int first, int last) {
        editNoteClips(src, [&](Source& s, int length) { return notes::trim(s, length, key, first, last); }, L("Trim Annotation", "주석 길이"));
    });
    connect(m_editor, &TimelineEditor::noteSplit, this, [this](const QString& src, const QString& key, int frame) {
        editNoteClips(src, [&](Source& s, int length) { return notes::split(s, length, key, frame); }, L("Cut Annotation", "주석 자르기"));
    });
    connect(m_editor, &TimelineEditor::noteErased, this, [this](const QString& src, const QString& key) {
        editNoteClips(src, [&](Source& s, int length) { return notes::erase(s, length, { key }); }, L("Delete Annotation", "주석 삭제"));
    });
    connect(m_editor, &TimelineEditor::noteDuplicated, this, [this](const QString& src, const QString& key, int first) {
        editNoteClips(src, [&](Source& s, int length) { return notes::duplicate(s, length, key, first); }, L("Duplicate Annotation", "주석 복제"));
    });
}

QImage MainWindow::thumbnailFor(const QString& sourceId, qint64 frame)
{
    FrameServer* srv = m_media.server(sourceId);
    if (!srv) return {};
    const FrameCache::Ptr f = srv->frame(frame);
    if (!f) {
        srv->requestPoster(frame);  // ready on a later repaint
        return {};
    }
    return f->toImage(QSize(200, 56));  // converts and shrinks in one pass
}

void MainWindow::refreshTimelineEditor(bool force)
{
    if (!m_editor) return;
    if (!force && (!m_editorDock || !m_editorDock->isVisible())) {
        m_editorStale = true;
        return;
    }
    m_editorStale = false;
    const bool onTimeline = m_soloId.isEmpty();
    const tl::LengthFn len = lengthFn();
    QHash<QString, TimelineEditor::SourceInfo> infos;
    int offline = 0;
    for (const TimelineClip& c : m_project.timeline.clips) {
        if (infos.contains(c.sourceId)) continue;
        const Source* s = m_project.find(c.sourceId);
        if (!s) continue;
        TimelineEditor::SourceInfo info;
        info.name = s->displayName();
        info.frames = sourceLength(c.sourceId);
        info.timing = len(c.sourceId);
        FrameServer* srv = m_media.server(c.sourceId);
        info.online = srv && srv->frameCount() > 0;
        offline += !info.online;
        info.hasAudio = srv && srv->info().hasAudio;
        for (const auto& [f, list] : s->annotations) info.keys << f;
        for (const Track& t : s->tracks)
            for (int f : t.keys) info.keys << f;
        info.bookmarks = s->bookmarkFrames();
        for (const auto& [f, text] : s->notes) info.notes << f;
        info.noteClips = notes::clips(*s, int(info.frames));
        info.peaks = m_peaks.count(c.sourceId) ? m_peaks.at(c.sourceId) : QVector<float>();
        infos.insert(c.sourceId, info);
    }
    m_editor->setModel(m_project.timeline, std::move(infos),
                       QStringLiteral("%1  ·  %2  ·  %3")
                           .arg(m_project.timeline.name)
                           .arg(L("%1 clips", "클립 %1개").arg(m_project.timeline.videoClipCount()))
                           .arg(tc(tl::duration(m_project.timeline, len), viewFps())));
    m_editor->setLoop(onTimeline && m_project.timeline.loopEnabled, m_project.timeline.loopIn, m_project.timeline.loopOut);
    m_editor->setCurrent(onTimeline ? m_current : -1);
    QString banner;
    if (!onTimeline) banner = L("Viewing one source — click the ruler to return to the timeline", "소스 하나를 보는 중 — 눈금을 클릭하면 타임라인으로 돌아갑니다");
    if (offline) banner += (banner.isEmpty() ? QString() : QStringLiteral("  ·  ")) + L("%1 offline clip(s)", "오프라인 클립 %1개").arg(offline);
    if (m_filter.enabled()) banner += (banner.isEmpty() ? QString() : QStringLiteral("  ·  ")) + AnalysisFilter::name(m_filter.mode);
    m_editor->setBanner(banner);
}

void MainWindow::applyWaveform(QVector<float> peaks)
{
    m_wave = peaks;
    m_timeline->setWaveform(std::move(peaks));
}

bool MainWindow::editTimeline(const std::function<bool(TimelineDoc&)>& fn, const QString& what)
{
    TimelineDoc next = m_project.timeline;
    if (!fn(next)) {
        refreshClipList();
        return false;
    }
    return commitTimeline(std::move(next), what);
}

bool MainWindow::commitTimeline(TimelineDoc next, const QString& what, const QStringList& select)
{
    m_view->commitEdits();
    if (m_pending) commitEdit();
    tl::normalize(next, lengthFn());
    if (next.sameLayout(m_project.timeline)) {
        refreshClipList();
        return false;
    }
    Edit e;
    e.isTimeline = true;
    e.docBefore = std::make_shared<TimelineDoc>(m_project.timeline);
    e.docAfter = std::make_shared<TimelineDoc>(next);
    m_undo.push_back(e);
    if (m_undo.size() > 1000) m_undo.removeFirst();
    m_redo.clear();
    applyTimeline(next);
    if (m_editor && !select.isEmpty()) m_editor->setSelectedClips(select);
    setDirty(true);
    if (!what.isEmpty() && m_console) m_console->append(QStringLiteral("Timeline: %1").arg(what));
    return true;
}

void MainWindow::applyTimeline(const TimelineDoc& doc)
{
    // The playhead keeps its time, like an editing program.
    m_project.timeline.takeLayout(doc);
    if (m_project.timeline.loopIn < 0) m_project.timeline.loopIn = 0;
    m_layersKey.clear();
    invalidateMix();
    refreshClipList();
    if (!m_soloId.isEmpty()) {
        refreshTimelineEditor();
        return;
    }
    rebuildSequence();
}

void MainWindow::setTrackState(int kind, int track, const TrackState& state)
{
    // Track switches are view settings: saved with the project, not undo steps.
    QVector<TrackState>& list = kind == 2 ? m_project.timeline.noteTracks
                                          : m_project.timeline.tracks(kind == 0 ? TimelineClip::Kind::Video : TimelineClip::Kind::Audio);
    while (list.size() <= track) list << TrackState{};
    if (list[track] == state) return;
    list[track] = state;
    setDirty(true);
    m_layersKey.clear();
    if (kind == 1) {
        invalidateMix();
        refreshWaveform();
        refreshTimelineEditor();
    } else if (kind == 0 && m_soloId.isEmpty()) {
        rebuildSequence();
    } else {
        refreshAnnotations();
        refreshTimelineEditor();
    }
}

void MainWindow::insertSources(const QStringList& ids, int index)
{
    // 1.0 entry point (project tree, Sources list): -1 = at the end, otherwise before the
    // index-th clip in timeline order.
    const QStringList order = videoClipOrder();
    int64_t frame = tl::duration(m_project.timeline, lengthFn());
    if (index >= 0 && index < order.size())
        if (const TimelineClip* c = m_project.timeline.clip(order[index])) frame = tl::span(*c, lengthFn()).start;
    QStringList valid;
    for (const QString& id : ids)
        if (m_project.find(id)) valid << id;
    if (valid.isEmpty()) return;
    if (m_project.timeline.videoClipCount() == 0)
        if (const Source* s = m_project.find(valid.first())) m_project.timeline.fps = s->fps;
    const bool ripple = index >= 0 && index < order.size();
    editTimeline([&](TimelineDoc& doc) { return tl::insertClips(doc, clipsFor(valid), frame, 0, 0, ripple, lengthFn()); },
                 L("Add Clips", "클립 추가"));
    showToast(L("Added %1 clip(s) to the timeline", "타임라인에 클립 %1개 추가").arg(valid.size()));
}

void MainWindow::dropSources(const QStringList& ids, int videoTrack, int64_t frame)
{
    QStringList valid;
    for (const QString& id : ids)
        if (m_project.find(id) && m_media.server(id)) valid << id;
    if (valid.isEmpty()) return;
    if (m_project.timeline.videoClipCount() == 0)
        if (const Source* s = m_project.find(valid.first())) m_project.timeline.fps = s->fps;
    const bool ripple = m_editor && m_editor->ripple();
    QStringList select;
    const bool done = editTimeline(
        [&](TimelineDoc& doc) {
            const QVector<TimelineClip> clips = clipsFor(valid);
            for (const TimelineClip& c : clips) select << c.uid;
            return tl::insertClips(doc, clips, frame, videoTrack, videoTrack, ripple, lengthFn());
        },
        ripple ? L("Insert", "삽입") : L("Overwrite", "덮어쓰기"));
    if (done && m_editor) m_editor->setSelectedClips(select);
    if (done) showToast(L("Placed %1 clip(s) on V%2 at %3", "V%2의 %3에 클립 %1개 배치").arg(valid.size()).arg(videoTrack + 1).arg(tc(frame, viewFps())));
}

void MainWindow::removeClip(const QString& uid)
{
    if (!m_project.timeline.clip(uid)) return;
    const bool linked = !m_editor || m_editor->linkedSelection();
    const bool ripple = m_editor && m_editor->ripple();
    editTimeline([&](TimelineDoc& doc) { return tl::remove(doc, linked ? tl::withLinks(doc, { uid }) : QSet<QString>{ uid }, ripple, lengthFn()); },
                 L("Remove Clip", "클립 빼기"));
}

void MainWindow::moveClip(const QString& uid, int index)
{
    // Order commands (Sources list, menus): reorder the clips of the track, packed back to back.
    const TimelineClip* c = m_project.timeline.clip(uid);
    if (!c || !c->isVideo()) return;
    const int track = c->track;
    QStringList order;
    for (const QString& id : videoClipOrder())
        if (const TimelineClip* x = m_project.timeline.clip(id); x && x->track == track) order << id;
    const qsizetype from = order.indexOf(uid);
    if (from < 0) return;
    order.removeAt(from);
    order.insert(std::clamp<qsizetype>(index, 0, order.size()), uid);
    editTimeline([&](TimelineDoc& doc) { return tl::reorderTrack(doc, order, lengthFn()); }, L("Reorder Clips", "클립 순서"));
}

void MainWindow::trimClip(const QString& uid, int in, int out)
{
    const TimelineClip* c = m_project.timeline.clip(uid);
    if (!c) return;
    const int64_t n = sourceLength(c->sourceId);
    if (n <= 0) return;
    in = std::clamp(in, 0, int(n - 1));
    out = out < 0 ? int(n - 1) : std::clamp(out, in, int(n - 1));
    const tl::Span s = tl::span(*c, lengthFn());
    const int64_t headDelta = s.timing.timed() ? std::llround((s.timing.info->timeOf(in) - s.timeStart) * s.timing.fps) : in - s.in;
    const int64_t tailDelta = s.timing.timed() ? std::llround((s.timing.info->timeOf(out + 1) - s.timeEnd) * s.timing.fps) : out - s.last();
    const bool ripple = m_editor && m_editor->ripple();
    const bool linked = !m_editor || m_editor->linkedSelection();
    editTimeline(
        [&](TimelineDoc& doc) {
            const QSet<QString> ids = linked ? tl::withLinks(doc, { uid }) : QSet<QString>{ uid };
            bool changed = false;
            // Lengthen first so the clip never goes through zero frames.
            if (tailDelta > 0) changed |= tl::trim(doc, ids, 1, tailDelta, ripple, lengthFn());
            if (headDelta != 0) changed |= tl::trim(doc, ids, -1, headDelta, ripple, lengthFn());
            if (tailDelta < 0) changed |= tl::trim(doc, ids, 1, tailDelta, ripple, lengthFn());
            return changed;
        },
        L("Trim", "트림"));
}

void MainWindow::splitClip(const QString& uid, int64_t global)
{
    const TimelineClip* c = m_project.timeline.clip(uid);
    if (!c) return;
    const tl::Span s = tl::span(*c, lengthFn());
    if (global <= s.start || global >= s.end()) {
        showToast(L("Put the playhead inside the clip (after its first frame) to split it.", "클립 안(첫 프레임 이후)에 재생 위치를 두고 자르세요."));
        return;
    }
    const bool linked = !m_editor || m_editor->linkedSelection();
    QStringList created;
    editTimeline([&](TimelineDoc& doc) { return tl::split(doc, linked ? tl::withLinks(doc, { uid }) : QSet<QString>{ uid }, global, lengthFn(), &created); },
                 L("Cut", "자르기"));
    if (m_editor) m_editor->setSelectedClips(created);
    showToast(L("Split at source frame %1", "소스 프레임 %1에서 잘랐습니다").arg(s.sourceAt(global - s.start)));
}

void MainWindow::splitAtPlayhead()
{
    if (!m_soloId.isEmpty()) {
        showToast(L("Splitting works on the timeline. Return to the timeline view first.", "자르기는 타임라인에서 동작합니다. 먼저 타임라인 보기로 돌아가세요."));
        return;
    }
    if (m_editorStale) refreshTimelineEditor(true);
    m_editor->setCurrent(m_current);
    if (!m_editor->splitAtPlayhead())
        showToast(L("Put the playhead inside a clip (after its first frame) to split it.", "클립 안(첫 프레임 이후)에 재생 위치를 두고 자르세요."));
}

void MainWindow::showClipMenu(const QString& uid, int64_t frame, QPoint global)
{
    QMenu menu(this);
    const TimelineClip* found = m_project.timeline.clip(uid);
    if (!found) {
        menu.addAction(Icons::icon("import"), L("Import Media...", "미디어 가져오기..."), this, &MainWindow::importDialog);
        QAction* paste = menu.addAction(L("Paste at Playhead\tCtrl+V", "재생 위치에 붙여넣기\tCtrl+V"), this, [this] { m_editor->pasteAtPlayhead(); });
        Q_UNUSED(paste);
        menu.addAction(Icons::icon("gap"), L("Close Gap at Playhead", "재생 위치의 빈 공간 닫기"), this, [this] { m_editor->closeGapAtPlayhead(); });
        menu.addSeparator();
        menu.addAction(Icons::icon("track-add"), L("Add Video Track", "비디오 트랙 추가"), this, [this] { m_editor->addTrack(TimelineClip::Kind::Video); });
        menu.addAction(Icons::icon("track-add"), L("Add Audio Track", "오디오 트랙 추가"), this, [this] { m_editor->addTrack(TimelineClip::Kind::Audio); });
        menu.addAction(Icons::icon("fit"), L("Fit Timeline", "타임라인 전체 보기"), this, [this] { m_editor->zoomToFit(); });
        menu.exec(global);
        return;
    }
    const TimelineClip clip = *found;
    const Source* s = m_project.find(clip.sourceId);
    if (!s) return;
    const tl::LengthFn len = lengthFn();
    const tl::Span sp = tl::span(clip, len);
    const QStringList selection = m_editor->selectedClips().isEmpty() ? QStringList{ uid } : m_editor->selectedClips();
    const QSet<QString> sel(selection.begin(), selection.end());
    QAction* title = menu.addAction(QFontMetrics(menu.font()).elidedText(clip.name.isEmpty() ? s->displayName() : clip.name, Qt::ElideMiddle, 260) +
                                    QStringLiteral("   %1 · %2–%3").arg(tl::trackName(m_project.timeline, clip.kind, clip.track)).arg(sp.in).arg(sp.last()));
    title->setEnabled(false);
    menu.addSeparator();
    const bool timelineView = m_soloId.isEmpty();
    menu.addAction(L("Go to Clip Start", "클립 처음으로"), this, [this, sp] {
        if (!m_soloId.isEmpty()) setSolo(QString());
        goToFrame(sp.start);
    });
    menu.addAction(L("Go to Clip End", "클립 끝으로"), this, [this, sp] {
        if (!m_soloId.isEmpty()) setSolo(QString());
        goToFrame(sp.end() - 1);
    });
    menu.addAction(L("Mark Clip (In / Out)\tX", "클립 구간 표시 (In / Out)\tX"), this, [this, sp] {
        m_project.timeline.loopIn = int(sp.start);
        m_project.timeline.loopOut = int(sp.end() - 1);
        m_project.timeline.loopEnabled = true;
        setDirty(true);
        updateLoopUi();
    });
    menu.addSeparator();
    const bool inside = timelineView && m_current > sp.start && m_current < sp.end();
    QAction* split = menu.addAction(Icons::icon("split"), L("Cut at Playhead\tCtrl+B", "재생 위치에서 자르기\tCtrl+B"), this, [this, uid] { splitClip(uid, m_current); });
    split->setEnabled(inside);
    const int64_t playheadSource = timelineView && sp.contains(m_current) ? sp.sourceAt(m_current - sp.start) : -1;
    QAction* setIn = menu.addAction(L("Trim Start to Playhead", "시작을 재생 위치로 트림"), this,
                                    [this, uid, playheadSource, sp] { trimClip(uid, int(playheadSource), int(sp.last())); });
    setIn->setEnabled(playheadSource > sp.in);
    QAction* setOut = menu.addAction(L("Trim End to Playhead", "끝을 재생 위치로 트림"), this,
                                     [this, uid, playheadSource, sp] { trimClip(uid, int(sp.in), int(playheadSource)); });
    setOut->setEnabled(playheadSource >= sp.in && playheadSource < sp.last());
    QAction* reset = menu.addAction(L("Reset Trim", "트림 초기화"), this, [this, uid] { trimClip(uid, 0, -1); });
    reset->setEnabled(clip.trimmed());
    menu.addAction(L("Clip Properties...", "클립 속성..."), this, [this, uid] { showClipProperties(uid); });
    menu.addSeparator();

    auto applyToSelection = [this, sel](const QString& what, std::function<void(TimelineClip&)> fn, bool sound) {
        editTimeline(
            [&](TimelineDoc& doc) {
                bool any = false;
                for (TimelineClip& c : doc.clips)
                    if (sel.contains(c.uid) && (sound ? c.isAudio() : c.isVideo()) && !tl::trackLocked(doc, c.kind, c.track)) {
                        fn(c);
                        any = true;
                    }
                return any;
            },
            what);
    };
    auto applyAll = [this, sel](const QString& what, std::function<void(TimelineClip&)> fn) {
        editTimeline(
            [&](TimelineDoc& doc) {
                for (TimelineClip& c : doc.clips)
                    if (sel.contains(c.uid)) fn(c);
                return true;
            },
            what);
    };
    QAction* enable = menu.addAction(clip.enabled ? L("Disable Clip\tD", "클립 끄기\tD") : L("Enable Clip\tD", "클립 켜기\tD"), this,
                                     [applyAll, on = !clip.enabled] { applyAll(on ? L("Enable Clip", "클립 켜기") : L("Disable Clip", "클립 끄기"), [on](TimelineClip& c) { c.enabled = on; }); });
    Q_UNUSED(enable);
    if (!clip.link.isEmpty()) {
        menu.addAction(Icons::icon("link"), L("Unlink Picture / Sound", "영상 / 소리 연결 해제"), this, [this, uid, partner = clip.link] {
            editTimeline(
                [&](TimelineDoc& doc) {
                    for (TimelineClip& c : doc.clips)
                        if (c.uid == uid || c.uid == partner) c.link.clear();
                    return true;
                },
                L("Unlink", "연결 해제"));
        });
    } else {
        // Link a video and a sound clip of the selection.
        QString v, a;
        for (const QString& id : selection)
            if (const TimelineClip* c = m_project.timeline.clip(id); c && c->link.isEmpty()) (c->isVideo() ? v : a) = id;
        if (!v.isEmpty() && !a.isEmpty())
            menu.addAction(Icons::icon("link"), L("Link Picture / Sound", "영상 / 소리 연결"), this, [this, v, a] {
                editTimeline(
                    [&](TimelineDoc& doc) {
                        doc.clip(v)->link = a;
                        doc.clip(a)->link = v;
                        return true;
                    },
                    L("Link", "연결"));
            });
    }
    QMenu* colours = menu.addMenu(L("Clip Color", "클립 색상"));
    const QList<std::pair<QString, QColor>> palette = {
        { L("Automatic", "자동"), QColor() },
        { L("Orange", "주황"), QColor(0xe0, 0x82, 0x3d) }, { L("Red", "빨강"), QColor(0xd9, 0x4f, 0x4f) },
        { L("Yellow", "노랑"), QColor(0xd6, 0xb6, 0x3a) }, { L("Green", "초록"), QColor(0x5c, 0xae, 0x63) },
        { L("Teal", "청록"), QColor(0x3e, 0xa8, 0xa0) }, { L("Blue", "파랑"), QColor(0x4f, 0x7f, 0xd9) },
        { L("Purple", "보라"), QColor(0x95, 0x6b, 0xd6) }, { L("Pink", "분홍"), QColor(0xd9, 0x6b, 0xae) },
    };
    for (const auto& [name, c] : palette)
        colours->addAction(c.isValid() ? colorIcon(c) : QIcon(), name, this,
                           [applyAll, c] { applyAll(L("Clip Color", "클립 색상"), [c](TimelineClip& x) { x.color = c; }); });

    if (clip.isVideo()) {
        QMenu* opacity = menu.addMenu(L("Opacity", "불투명도"));
        for (int pct : { 100, 75, 50, 25 }) {
            QAction* a = opacity->addAction(QStringLiteral("%1%").arg(pct), this, [applyToSelection, pct] {
                applyToSelection(L("Opacity", "불투명도"), [pct](TimelineClip& c) { c.opacity = pct / 100.0; }, false);
            });
            a->setCheckable(true);
            a->setChecked(qRound(clip.opacity * 100) == pct);
        }
        QMenu* blend = menu.addMenu(L("Composite Mode", "합성 모드"));
        const QList<std::pair<QString, BlendMode>> modes = {
            { L("Normal", "보통"), BlendMode::Normal }, { L("Add", "더하기"), BlendMode::Add },
            { L("Screen", "스크린"), BlendMode::Screen }, { L("Multiply", "곱하기"), BlendMode::Multiply },
            { L("Difference (compare takes)", "차이 (테이크 비교)"), BlendMode::Difference },
        };
        for (const auto& [name, mode] : modes) {
            QAction* a = blend->addAction(name, this, [applyToSelection, mode] {
                applyToSelection(L("Composite Mode", "합성 모드"), [mode](TimelineClip& c) { c.blend = mode; }, false);
            });
            a->setCheckable(true);
            a->setChecked(clip.blend == mode);
        }
        QMenu* pip = menu.addMenu(L("Picture in Picture", "화면 속 화면"));
        const QList<std::tuple<QString, double, double, double>> spots = {
            { L("Full Frame", "전체 화면"), 1.0, 0.0, 0.0 },
            { L("Top Left · 35%", "왼쪽 위 · 35%"), 0.35, -0.3, -0.3 }, { L("Top Right · 35%", "오른쪽 위 · 35%"), 0.35, 0.3, -0.3 },
            { L("Bottom Left · 35%", "왼쪽 아래 · 35%"), 0.35, -0.3, 0.3 }, { L("Bottom Right · 35%", "오른쪽 아래 · 35%"), 0.35, 0.3, 0.3 },
            { L("Left Half", "왼쪽 절반"), 0.5, -0.25, 0.0 }, { L("Right Half", "오른쪽 절반"), 0.5, 0.25, 0.0 },
        };
        for (const auto& [name, scale, x, y] : spots)
            pip->addAction(name, this, [applyToSelection, scale, x, y] {
                applyToSelection(L("Picture in Picture", "화면 속 화면"), [=](TimelineClip& c) {
                    c.scale = scale;
                    c.posX = x;
                    c.posY = y;
                }, false);
            });
    } else {
        QMenu* gain = menu.addMenu(Icons::icon("volume"), L("Clip Gain", "클립 게인"));
        for (double db : { 6.0, 3.0, 0.0, -3.0, -6.0, -12.0, -24.0 }) {
            QAction* a = gain->addAction(QStringLiteral("%1%2 dB").arg(db > 0 ? "+" : "").arg(db, 0, 'f', 0), this, [applyToSelection, db] {
                applyToSelection(L("Clip Gain", "클립 게인"), [db](TimelineClip& c) { c.gainDb = db; }, true);
            });
            a->setCheckable(true);
            a->setChecked(std::abs(clip.gainDb - db) < 0.05);
        }
        menu.addAction(Icons::icon(clip.muted ? "volume" : "mute"), clip.muted ? L("Unmute Clip", "클립 소리 켜기") : L("Mute Clip", "클립 음소거"), this,
                       [applyToSelection, on = !clip.muted] { applyToSelection(L("Mute Clip", "클립 음소거"), [on](TimelineClip& c) { c.muted = on; }, true); });
        QMenu* fades = menu.addMenu(L("Fades", "페이드"));
        const int fps = std::max(1, int(std::round(viewFps())));
        for (int frames : { 0, fps / 4, fps / 2, fps }) {
            fades->addAction(frames == 0 ? L("No Fades", "페이드 없음") : L("%1 frames in / out", "%1프레임 인 / 아웃").arg(frames), this, [applyToSelection, frames] {
                applyToSelection(L("Fades", "페이드"), [frames](TimelineClip& c) {
                    c.fadeIn = frames;
                    c.fadeOut = frames;
                }, true);
            });
        }
    }
    menu.addSeparator();
    menu.addAction(L("Copy\tCtrl+C", "복사\tCtrl+C"), this, [this] { m_editor->copySelection(); });
    menu.addAction(L("Duplicate After", "뒤에 복제"), this, [this, sel] {
        editTimeline(
            [&](TimelineDoc& doc) {
                int64_t first = std::numeric_limits<int64_t>::max(), last = 0;
                for (const TimelineClip& c : doc.clips)
                    if (sel.contains(c.uid)) {
                        const tl::Span s = tl::span(c, lengthFn());
                        first = std::min(first, s.start);
                        last = std::max(last, s.end());
                    }
                return first < last && tl::moveOverwrite(doc, sel, last - first, 0, 0, true, lengthFn());
            },
            L("Duplicate", "복제"));
    });
    menu.addAction(Icons::icon("trash"), L("Delete (leave gap)\tDelete", "삭제 (빈 공간 유지)\tDelete"), this, [this] { m_editor->deleteSelection(false); });
    menu.addAction(Icons::icon("ripple"), L("Ripple Delete\tShift+Delete", "리플 삭제\tShift+Delete"), this, [this] { m_editor->deleteSelection(true); });
    menu.addSeparator();
    const QString sourceId = clip.sourceId;
    menu.addAction(L("View Source as A", "소스를 A로 보기"), this, [this, sourceId] { chooseA(sourceId); });
    menu.addAction(Icons::icon("compare"), L("Compare as B", "B로 비교"), this, [this, sourceId] { chooseB(sourceId); });
    menu.addAction(L("Show in Explorer", "탐색기에서 보기"), this, [file = s->media.firstFile()] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
    });
    Q_UNUSED(frame);
    menu.exec(global);
}

void MainWindow::showClipProperties(const QString& uid)
{
    const TimelineClip* found = m_project.timeline.clip(uid);
    if (!found) return;
    const TimelineClip c = *found;
    const tl::Span sp = tl::span(c, lengthFn());
    const int64_t n = sourceLength(c.sourceId);
    QDialog d(this);
    d.setObjectName("ClipPropertiesDialog");
    d.setWindowTitle(L("Clip Properties", "클립 속성"));
    auto* form = new QFormLayout(&d);
    auto* name = new QLineEdit(c.name);
    if (const Source* s = m_project.find(c.sourceId)) name->setPlaceholderText(s->displayName());
    form->addRow(L("Name", "이름"), name);
    auto* start = new QSpinBox;
    start->setRange(0, 100000000);
    start->setValue(int(sp.start));
    form->addRow(L("Timeline start (frame)", "타임라인 시작 (프레임)"), start);
    auto* in = new QSpinBox;
    auto* out = new QSpinBox;
    in->setRange(0, int(std::max<int64_t>(0, n - 1)));
    out->setRange(0, int(std::max<int64_t>(0, n - 1)));
    in->setValue(int(sp.in));
    out->setValue(int(sp.last()));
    form->addRow(L("In (source frame)", "In (소스 프레임)"), in);
    form->addRow(L("Out (source frame)", "Out (소스 프레임)"), out);
    auto* track = new QSpinBox;
    track->setRange(1, 64);
    track->setValue(c.track + 1);
    track->setPrefix(c.isVideo() ? QStringLiteral("V") : QStringLiteral("A"));
    form->addRow(L("Track", "트랙"), track);
    QDoubleSpinBox *opacity = nullptr, *scale = nullptr, *x = nullptr, *y = nullptr, *gain = nullptr;
    QSpinBox *fadeIn = nullptr, *fadeOut = nullptr;
    QComboBox* blend = nullptr;
    if (c.isVideo()) {
        opacity = new QDoubleSpinBox;
        opacity->setRange(0, 100);
        opacity->setSuffix(QStringLiteral(" %"));
        opacity->setValue(c.opacity * 100);
        form->addRow(L("Opacity", "불투명도"), opacity);
        blend = new QComboBox;
        blend->addItems({ L("Normal", "보통"), L("Add", "더하기"), L("Screen", "스크린"), L("Multiply", "곱하기"), L("Difference", "차이") });
        blend->setCurrentIndex(int(c.blend));
        form->addRow(L("Composite", "합성"), blend);
        scale = new QDoubleSpinBox;
        scale->setRange(1, 2000);
        scale->setSuffix(QStringLiteral(" %"));
        scale->setValue(c.scale * 100);
        form->addRow(L("Zoom", "크기"), scale);
        x = new QDoubleSpinBox;
        y = new QDoubleSpinBox;
        for (QDoubleSpinBox* b : { x, y }) {
            b->setRange(-1000, 1000);
            b->setSuffix(QStringLiteral(" %"));
            b->setDecimals(1);
        }
        x->setValue(c.posX * 100);
        y->setValue(c.posY * 100);
        form->addRow(L("Position X (canvas %)", "위치 X (캔버스 %)"), x);
        form->addRow(L("Position Y (canvas %)", "위치 Y (캔버스 %)"), y);
    } else {
        gain = new QDoubleSpinBox;
        gain->setRange(-60, 24);
        gain->setDecimals(1);
        gain->setSuffix(QStringLiteral(" dB"));
        gain->setValue(c.gainDb);
        form->addRow(L("Gain", "게인"), gain);
        fadeIn = new QSpinBox;
        fadeOut = new QSpinBox;
        fadeIn->setRange(0, int(sp.count));
        fadeOut->setRange(0, int(sp.count));
        fadeIn->setValue(c.fadeIn);
        fadeOut->setValue(c.fadeOut);
        form->addRow(L("Fade in (frames)", "페이드 인 (프레임)"), fadeIn);
        form->addRow(L("Fade out (frames)", "페이드 아웃 (프레임)"), fadeOut);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    editTimeline(
        [&](TimelineDoc& doc) {
            TimelineClip* t = doc.clip(uid);
            if (!t) return false;
            t->name = name->text().trimmed();
            const int lo = std::min(in->value(), out->value()), hi = std::max(in->value(), out->value());
            if (lo != sp.in) t->sourceStart = -1;
            if (hi != sp.last()) t->sourceEnd = -1;
            t->in = lo;
            t->out = n > 0 && hi >= n - 1 ? -1 : hi;
            t->start = start->value();
            t->track = track->value() - 1;
            if (c.isVideo()) {
                t->opacity = opacity->value() / 100.0;
                t->blend = BlendMode(blend->currentIndex());
                t->scale = scale->value() / 100.0;
                t->posX = x->value() / 100.0;
                t->posY = y->value() / 100.0;
            } else {
                t->gainDb = gain->value();
                t->fadeIn = fadeIn->value();
                t->fadeOut = fadeOut->value();
            }
            // The clip goes where the numbers say; whatever was there gives way (overwrite).
            const tl::Span ns = tl::span(*t, lengthFn());
            while (doc.tracks(t->kind).size() <= t->track) doc.tracks(t->kind) << TrackState{};
            tl::clearRange(doc, t->kind, t->track, ns.start, ns.end(), { uid }, lengthFn());
            return true;
        },
        L("Clip Properties", "클립 속성"));
}

void MainWindow::showTrackMenu(int kind, int track, QPoint global)
{
    QMenu menu(this);
    const bool notesLane = kind == 2;
    const TimelineClip::Kind k = kind == 0 ? TimelineClip::Kind::Video : TimelineClip::Kind::Audio;
    QVector<TrackState>& list = notesLane ? m_project.timeline.noteTracks : m_project.timeline.tracks(k);
    const TrackState st = list.value(track);
    const QString name = notesLane ? (st.name.isEmpty() ? QStringLiteral("N%1").arg(track + 1) : st.name) : tl::trackName(m_project.timeline, k, track);
    addTitle(menu, notesLane ? L("Annotation lane %1", "주석 레인 %1").arg(name) : name);
    menu.addSeparator();
    menu.addAction(L("Rename...", "이름 바꾸기..."), this, [this, kind, track, st, name] {
        bool ok = false;
        const QString text = QInputDialog::getText(this, L("Track Name", "트랙 이름"), L("Name", "이름"), QLineEdit::Normal, name, &ok);
        if (!ok) return;
        TrackState next = st;
        next.name = text.trimmed();
        setTrackState(kind, track, next);
    });
    if (kind == 1) {
        menu.addAction(st.visible ? L("Mute Track", "트랙 음소거") : L("Unmute Track", "트랙 소리 켜기"), this, [this, kind, track, st] {
            TrackState next = st;
            next.visible = !st.visible;
            setTrackState(kind, track, next);
        });
        menu.addAction(st.solo ? L("Unsolo", "솔로 해제") : L("Solo", "솔로"), this, [this, kind, track, st] {
            TrackState next = st;
            next.solo = !st.solo;
            setTrackState(kind, track, next);
        });
        QMenu* gain = menu.addMenu(L("Track Gain", "트랙 게인"));
        for (double db : { 6.0, 3.0, 0.0, -3.0, -6.0, -12.0 }) {
            QAction* a = gain->addAction(QStringLiteral("%1%2 dB").arg(db > 0 ? "+" : "").arg(db, 0, 'f', 0), this, [this, kind, track, st, db] {
                TrackState next = st;
                next.gainDb = db;
                setTrackState(kind, track, next);
            });
            a->setCheckable(true);
            a->setChecked(std::abs(st.gainDb - db) < 0.05);
        }
    } else {
        menu.addAction(st.visible ? L("Hide Track", "트랙 숨기기") : L("Show Track", "트랙 보이기"), this, [this, kind, track, st] {
            TrackState next = st;
            next.visible = !st.visible;
            setTrackState(kind, track, next);
        });
    }
    menu.addAction(st.locked ? L("Unlock Track", "트랙 잠금 해제") : L("Lock Track", "트랙 잠금"), this, [this, kind, track, st] {
        TrackState next = st;
        next.locked = !st.locked;
        setTrackState(kind, track, next);
    });
    menu.addSeparator();
    if (notesLane) {
        menu.addAction(Icons::icon("track-add"), L("Add Annotation Lane", "주석 레인 추가"), this, [this] {
            m_project.timeline.noteTracks << TrackState{};
            setDirty(true);
            refreshTimelineEditor();
        });
    } else {
        menu.addAction(Icons::icon("track-add"), kind == 0 ? L("Add Video Track", "비디오 트랙 추가") : L("Add Audio Track", "오디오 트랙 추가"), this,
                       [this, k] { m_editor->addTrack(k); });
        bool empty = true;
        for (const TimelineClip& c : m_project.timeline.clips) empty = empty && !(c.kind == k && c.track == track);
        QAction* del = menu.addAction(Icons::icon("trash"), L("Delete Track", "트랙 삭제"), this, [this, k, track, empty] {
            if (!empty && QMessageBox::question(this, L("Delete Track", "트랙 삭제"), L("Delete the track and its clips?", "트랙과 그 안의 클립을 삭제할까요?")) != QMessageBox::Yes)
                return;
            editTimeline(
                [&](TimelineDoc& doc) {
                    QSet<QString> gone;
                    for (const TimelineClip& c : doc.clips)
                        if (c.kind == k && c.track == track) gone.insert(c.uid);
                    tl::remove(doc, gone, false, lengthFn());
                    for (TimelineClip& c : doc.clips)
                        if (c.kind == k && c.track > track) --c.track;
                    QVector<TrackState>& tracks = doc.tracks(k);
                    if (tracks.size() > 1 && track < tracks.size()) tracks.removeAt(track);
                    return true;
                },
                L("Delete Track", "트랙 삭제"));
        });
        del->setEnabled(m_project.timeline.tracks(k).size() > 1);
    }
    menu.exec(global);
}

// ---------------------------------------------------------------------------- annotation clips

bool MainWindow::editNoteClips(const QString& sourceId, const std::function<bool(Source&, int)>& fn, const QString& what)
{
    m_view->commitEdits();
    if (m_pending) commitEdit();
    Source* s = m_project.find(sourceId);
    if (!s) return false;
    auto before = std::make_shared<std::map<int, AnnotationList>>(s->annotations);
    if (!fn(*s, int(std::max<int64_t>(sourceLength(sourceId), s->frameCount)))) return false;
    Edit e;
    e.isNoteClips = true;
    e.sourceId = sourceId;
    e.keysBefore = before;
    e.keysAfter = std::make_shared<std::map<int, AnnotationList>>(s->annotations);
    m_undo.push_back(e);
    if (m_undo.size() > 1000) m_undo.removeFirst();
    m_redo.clear();
    ++m_annotationRev;
    setDirty(true);
    refreshAnnotations();
    refreshMarks();
    if (m_console) m_console->append(QStringLiteral("Annotations: %1").arg(what));
    return true;
}

QHash<QString, int> MainWindow::noteLanes(const Source& s)
{
    if (m_laneRev.value(s.id, ~quint64(0)) != m_annotationRev) {
        m_laneCache.insert(s.id, notes::lanes(s, int(std::max<int64_t>(sourceLength(s.id), s.frameCount))));
        m_laneRev.insert(s.id, m_annotationRev);
    }
    return m_laneCache.value(s.id);
}

void MainWindow::selectNoteClip(const QString& sourceId, const QString& key)
{
    const Source* s = m_project.find(sourceId);
    if (!s) return;
    const QVector<NoteClip> list = notes::clips(*s, int(std::max<int64_t>(sourceLength(sourceId), s->frameCount)));
    const auto it = std::find_if(list.begin(), list.end(), [&](const NoteClip& n) { return n.key == key; });
    if (it == list.end()) return;
    // Show it: stay if the playhead is already on it, otherwise go to its first frame.
    const Loc loc = locate(m_current);
    const bool visible = loc && loc.source->id == sourceId && loc.rawLocal >= it->first && loc.rawLocal <= it->last;
    if (!visible) {
        const int64_t g = m_seq.globalFor(sourceId, it->first, m_seq.indexAt(m_current));
        if (g >= 0) {
            if (m_playing) stopPlayback();
            goToFrame(g);
        }
    }
    if (m_view->tool() != Tool::Select) setTool(Tool::Select);
    m_view->setSelection(it->ids);
}

void MainWindow::showNoteMenu(const QString& sourceId, const QString& key, int64_t frame, QPoint global)
{
    const Source* s = m_project.find(sourceId);
    if (!s) return;
    const int length = int(std::max<int64_t>(sourceLength(sourceId), s->frameCount));
    const QVector<NoteClip> list = notes::clips(*s, length);
    const auto it = std::find_if(list.begin(), list.end(), [&](const NoteClip& n) { return n.key == key; });
    if (it == list.end()) return;
    const NoteClip clip = *it;
    QMenu menu(this);
    addTitle(menu, QStringLiteral("%1   %2–%3").arg(QFontMetrics(menu.font()).elidedText(clip.label, Qt::ElideRight, 220)).arg(clip.first).arg(clip.last));
    menu.addSeparator();
    const Loc loc = locate(m_current);
    const int local = loc && loc.source->id == sourceId ? int(loc.rawLocal) : -1;
    menu.addAction(L("Select Objects", "객체 선택"), this, [this, sourceId, key] { selectNoteClip(sourceId, key); });
    QAction* cut = menu.addAction(Icons::icon("split"), L("Cut at Playhead", "재생 위치에서 자르기"), this, [this, sourceId, key, local] {
        editNoteClips(sourceId, [&](Source& src, int len) { return notes::split(src, len, key, local); }, L("Cut Annotation", "주석 자르기"));
    });
    cut->setEnabled(local > clip.first && local <= clip.last);
    QAction* toHere = menu.addAction(L("Extend to Playhead", "재생 위치까지 늘이기"), this, [this, sourceId, key, local, clip] {
        editNoteClips(sourceId, [&](Source& src, int len) { return notes::trim(src, len, key, std::min(clip.first, local), std::max(clip.last, local)); },
                      L("Trim Annotation", "주석 길이"));
    });
    toHere->setEnabled(local >= 0 && (local < clip.first || local > clip.last));
    QAction* endHere = menu.addAction(L("End at Playhead", "재생 위치에서 끝내기"), this, [this, sourceId, key, local, clip] {
        editNoteClips(sourceId, [&](Source& src, int len) { return notes::trim(src, len, key, clip.first, local); }, L("Trim Annotation", "주석 길이"));
    });
    endHere->setEnabled(local >= clip.first && local < clip.last);
    menu.addAction(L("Hold to End of Clip", "클립 끝까지 유지"), this, [this, sourceId, key, clip, length] {
        editNoteClips(sourceId, [&](Source& src, int len) { return notes::trim(src, len, key, clip.first, length - 1); }, L("Trim Annotation", "주석 길이"));
    });
    menu.addAction(L("Set Duration...", "길이 지정..."), this, [this, sourceId, key, clip] {
        bool ok = false;
        const int frames = QInputDialog::getInt(this, L("Annotation Duration", "주석 길이"), L("Frames", "프레임"), clip.last - clip.first + 1, 1, 1000000, 1, &ok);
        if (ok) editNoteClips(sourceId, [&](Source& src, int len) { return notes::trim(src, len, key, clip.first, clip.first + frames - 1); }, L("Trim Annotation", "주석 길이"));
    });
    menu.addSeparator();
    QMenu* lane = menu.addMenu(L("Move to Lane", "레인으로 이동"));
    for (int l = 0; l < std::max<int>(int(m_project.timeline.noteTracks.size()), clip.lane + 2); ++l) {
        QAction* a = lane->addAction(QStringLiteral("N%1").arg(l + 1), this, [this, sourceId, key, clip, l] {
            editNoteClips(sourceId, [&](Source& src, int len) { return notes::move(src, len, key, clip.first, l); }, L("Move Annotation", "주석 이동"));
        });
        a->setCheckable(true);
        a->setChecked(l == clip.lane);
    }
    QMenu* colours = menu.addMenu(L("Clip Color", "클립 색상"));
    const QList<std::pair<QString, QColor>> palette = {
        { L("Automatic (drawing colour)", "자동 (그림 색)"), QColor() },
        { L("Red", "빨강"), QColor(0xe5, 0x48, 0x3e) }, { L("Orange", "주황"), QColor(0xe8, 0x8a, 0x3a) },
        { L("Yellow", "노랑"), QColor(0xe8, 0xc5, 0x3a) }, { L("Green", "초록"), QColor(0x4c, 0xb0, 0x5c) },
        { L("Blue", "파랑"), QColor(0x4a, 0x86, 0xe0) }, { L("Purple", "보라"), QColor(0x9a, 0x6a, 0xe0) },
        { L("White", "흰색"), QColor(0xf0, 0xf0, 0xf0) },
    };
    for (const auto& [name, c] : palette) {
        colours->addAction(c.isValid() ? colorIcon(c) : QIcon(), name, this, [this, sourceId, key, c] {
            editNoteClips(sourceId, [&](Source& src, int len) { return notes::setColor(src, len, key, c, false); }, L("Clip Color", "클립 색상"));
        });
    }
    colours->addSeparator();
    for (const auto& [name, c] : palette)
        if (c.isValid())
            colours->addAction(colorIcon(c), L("Recolour drawing: %1", "그림 색 바꾸기: %1").arg(name), this, [this, sourceId, key, c] {
                editNoteClips(sourceId, [&](Source& src, int len) { return notes::setColor(src, len, key, c, true); }, L("Recolour", "색 바꾸기"));
            });
    menu.addAction(Icons::icon(clip.locked ? "unlock" : "lock"), clip.locked ? L("Unlock", "잠금 해제") : L("Lock", "잠금"), this,
                   [this, sourceId, key, on = !clip.locked] {
                       editNoteClips(sourceId, [&](Source& src, int len) { return notes::setLocked(src, len, key, on); }, L("Lock Annotation", "주석 잠금"));
                   });
    menu.addAction(L("Duplicate After", "뒤에 복제"), this, [this, sourceId, key, clip] {
        editNoteClips(sourceId, [&](Source& src, int len) { return notes::duplicate(src, len, key, clip.last + 1); }, L("Duplicate Annotation", "주석 복제"));
    });
    menu.addSeparator();
    menu.addAction(Icons::icon("trash"), L("Delete", "삭제"), this, [this, sourceId, key] {
        editNoteClips(sourceId, [&](Source& src, int len) { return notes::erase(src, len, { key }); }, L("Delete Annotation", "주석 삭제"));
    });
    Q_UNUSED(frame);
    menu.exec(global);
}

// ============================================================================ window menu / layout

void MainWindow::buildWindowMenu()
{
    QMenu* win = menuBar()->addMenu(L("Window", "창"));
    win->setObjectName("WindowMenu");
    struct Entry {
        QWidget* widget;
        QAction* action;
    };
    auto entries = std::make_shared<QVector<Entry>>();
    auto panel = [&](const QString& text, QWidget* widget, const QKeySequence& key, const QString& icon, const char* name) {
        QAction* a = addCommand(win, text, key, [this, widget] {
            const bool hidden = widget->isHidden() || (widget->parentWidget() == m_leftSplit && m_leftSplit->isHidden());
            setPanelVisible(widget, hidden);
        });
        a->setCheckable(true);
        a->setObjectName(QStringLiteral("Window/") + QLatin1String(name));
        if (!icon.isEmpty()) a->setIcon(Icons::icon(icon));
        entries->append({ widget, a });
        return a;
    };
    panel(L("Project View", "프로젝트 뷰"), m_projectPanel, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_1), "sidebar", "Project");
    panel(L("Sources / Properties", "소스 / 속성"), m_timelinePanel, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_2), "film", "Sources");
    panel(L("Timeline", "타임라인"), m_editorDock, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_3), "layout", "Timeline");
    panel(L("Frame Notes", "프레임 메모"), m_notesDock, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_4), "notes", "Notes");
    panel(L("Motion Graph", "모션 그래프"), m_graphDock, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_5), "graph", "MotionGraph");
    panel(L("Tracking Manager", "트래킹 매니저"), m_trackingManager, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_6), "target", "Tracking");
    panel(L("Console", "콘솔"), m_console, QKeySequence(), QString(), "Console");
    win->addSeparator();
    panel(L("Annotation Toolbar", "주석 도구 막대"), m_toolStrip, QKeySequence(), "toolbar", "Tools");
    panel(L("Clip Bar (Source Range)", "클립 바 (소스 범위)"), m_clipBar, QKeySequence(), QString(), "ClipBar");
    panel(L("Scrub Bar", "재생 바"), m_timeline, QKeySequence(), QString(), "ScrubBar");
    panel(L("Transport Bar", "재생 컨트롤 바"), m_bottomBar, QKeySequence(), QString(), "Transport");
    win->addSeparator();
    QAction* focus = addCommand(win, L("Viewer Only", "뷰어만 보기"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [this] { setViewerOnly(!m_viewerOnly); });
    focus->setCheckable(true);
    focus->setIcon(Icons::icon("fit"));
    QAction* full = addCommand(win, L("Full Screen", "전체 화면"), QKeySequence(Qt::Key_F11), [this] {
        if (isFullScreen()) showNormal();
        else showFullScreen();
    });
    full->setCheckable(true);
    win->addSeparator();
    win->addAction(L("Show All Panels", "모든 패널 표시"), this, [this] {
        if (m_viewerOnly) setViewerOnly(false);
        for (QWidget* w : { m_projectPanel, m_timelinePanel, m_toolStrip, static_cast<QWidget*>(m_clipBar), static_cast<QWidget*>(m_timeline), m_bottomBar })
            setPanelVisible(w, true);
    });
    win->addAction(Icons::icon("layout"), L("Reset Layout", "레이아웃 초기화"), this, &MainWindow::resetLayout);
    connect(win, &QMenu::aboutToShow, this, [this, entries, focus, full] {
        for (const Entry& e : *entries) {
            bool shown = !e.widget->isHidden();
            if (e.widget->parentWidget() == m_leftSplit) shown = shown && !m_leftSplit->isHidden();
            e.action->setChecked(shown);
        }
        focus->setChecked(m_viewerOnly);
        full->setChecked(isFullScreen());
    });
}

void MainWindow::setPanelVisible(QWidget* panel, bool visible)
{
    if (!panel) return;
    if (auto* dock = qobject_cast<QDockWidget*>(panel)) {
        if (visible && dock->isFloating() && !QApplication::screenAt(dock->geometry().center())) dock->setFloating(false);  // lost off-screen
        dock->setVisible(visible);
        if (visible) dock->raise();
        return;
    }
    panel->setVisible(visible);
    if (panel == m_projectPanel || panel == m_timelinePanel) {
        const bool any = !m_projectPanel->isHidden() || !m_timelinePanel->isHidden();
        m_leftSplit->setVisible(any);
        if (any && m_mainSplit->sizes().value(0) < 120) m_mainSplit->setSizes({ 260, std::max(400, width() - 260) });
    }
}

void MainWindow::saveLayout()
{
    QSettings s;
    s.setValue("layout/state", saveState(kLayoutVersion));
    s.setValue("layout/mainSplit", m_mainSplit->saveState());
    s.setValue("layout/leftSplit", m_leftSplit->saveState());
    s.setValue("layout/projectPanel", !m_projectPanel->isHidden() && !m_leftSplit->isHidden());
    s.setValue("layout/sourcesPanel", !m_timelinePanel->isHidden() && !m_leftSplit->isHidden());
    s.setValue("layout/toolStrip", !m_toolStrip->isHidden());
    s.setValue("layout/clipBar", !m_clipBar->isHidden());
    s.setValue("layout/scrubBar", !m_timeline->isHidden());
    s.setValue("layout/transport", !m_bottomBar->isHidden());
    // isVisible() would also reflect the main window being hidden during shutdown.
    s.setValue("layout/timelineVisible", !m_editorDock->isHidden());
}

void MainWindow::restoreLayout()
{
    QSettings s;
    const QByteArray state = s.value("layout/state").toByteArray();
    const bool restored = !state.isEmpty() && restoreState(state, kLayoutVersion);
    // New installs start closed; older saved layouts retain their last dock visibility.
    m_editorDock->setVisible(s.value("layout/timelineVisible", restored && !m_editorDock->isHidden()).toBool());
    if (const QByteArray v = s.value("layout/mainSplit").toByteArray(); !v.isEmpty()) m_mainSplit->restoreState(v);
    if (const QByteArray v = s.value("layout/leftSplit").toByteArray(); !v.isEmpty()) m_leftSplit->restoreState(v);
    m_projectPanel->setVisible(s.value("layout/projectPanel", true).toBool());
    m_timelinePanel->setVisible(s.value("layout/sourcesPanel", true).toBool());
    m_leftSplit->setVisible(!m_projectPanel->isHidden() || !m_timelinePanel->isHidden());
    m_toolStrip->setVisible(s.value("layout/toolStrip", true).toBool());
    m_clipBar->setVisible(s.value("layout/clipBar", true).toBool());
    m_timeline->setVisible(s.value("layout/scrubBar", true).toBool());
    m_bottomBar->setVisible(s.value("layout/transport", true).toBool());
    // A dock restored floating on a monitor that is gone comes back docked.
    for (QDockWidget* dock : findChildren<QDockWidget*>())
        if (dock->isFloating() && !QApplication::screenAt(dock->geometry().center())) dock->setFloating(false);
}

void MainWindow::resetLayout()
{
    if (m_viewerOnly) setViewerOnly(false);
    if (isFullScreen()) showNormal();
    restoreState(m_defaultState, kLayoutVersion);
    for (QDockWidget* dock : { m_editorDock, m_notesDock, m_graphDock, m_trackingManager, static_cast<QDockWidget*>(m_console) }) {
        dock->setFloating(false);
        dock->hide();
    }
    for (QWidget* w : { m_projectPanel, m_timelinePanel, m_toolStrip, static_cast<QWidget*>(m_clipBar), static_cast<QWidget*>(m_timeline), m_bottomBar })
        w->show();
    m_leftSplit->show();
    m_leftSplit->setSizes({ 1, 1 });
    m_mainSplit->setSizes({ 260, std::max(400, width() - 260) });
    m_view->fitToView();
    showToast(L("Layout reset", "레이아웃을 초기화했습니다"));
}

void MainWindow::setViewerOnly(bool on)
{
    if (on == m_viewerOnly) return;
    QWidget* const widgets[] = { m_projectPanel, m_timelinePanel, m_toolStrip, m_leftSplit };
    if (on) {
        m_beforeViewerOnly = saveState(kLayoutVersion);
        m_panelsBeforeViewerOnly.clear();
        for (QWidget* w : widgets) m_panelsBeforeViewerOnly[w->objectName() + QString::number(quintptr(w))] = !w->isHidden();
        for (QWidget* w : widgets) w->hide();
        for (QDockWidget* dock : findChildren<QDockWidget*>()) dock->hide();
        m_viewerOnly = true;
        showToast(L("Viewer only · Ctrl+Shift+F to bring the panels back", "뷰어만 보기 · Ctrl+Shift+F로 패널 복원"));
    } else {
        for (QWidget* w : widgets) w->setVisible(m_panelsBeforeViewerOnly.value(w->objectName() + QString::number(quintptr(w)), true));
        restoreState(m_beforeViewerOnly, kLayoutVersion);
        m_viewerOnly = false;
    }
    m_view->fitToView();
}

// ============================================================================ frame notes

void MainWindow::buildNotesPanel()
{
    m_notesDock = new QDockWidget(L("Frame Notes", "프레임 메모"), this);
    m_notesDock->setObjectName("FrameNotesDock");
    m_notesDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    auto* panel = new QWidget;
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(10, 8, 10, 10);
    v->setSpacing(6);
    m_noteLabel = new QLabel;
    m_noteLabel->setObjectName("FormKey");
    v->addWidget(m_noteLabel);
    m_noteEdit = new QPlainTextEdit;
    m_noteEdit->setObjectName("FrameNoteEdit");
    m_noteEdit->setPlaceholderText(L("Note for this frame — timing, spacing, pose, acting… (Ctrl+Enter to save)",
                                     "이 프레임의 메모 — 타이밍, 간격, 포즈, 연기… (Ctrl+Enter로 저장)"));
    m_noteEdit->setFixedHeight(84);
    m_noteEdit->installEventFilter(this);
    v->addWidget(m_noteEdit);
    auto* row = new QHBoxLayout;
    auto* save = new QPushButton(L("Save Note", "메모 저장"));
    save->setObjectName("Primary");
    auto* del = new QPushButton(L("Delete", "삭제"));
    auto* exportBtn = new QPushButton(L("Export...", "내보내기..."));
    row->addWidget(save);
    row->addWidget(del);
    row->addStretch(1);
    row->addWidget(exportBtn);
    v->addLayout(row);
    m_notesTree = new QTreeWidget;
    m_notesTree->setObjectName("FrameNotesList");
    m_notesTree->setColumnCount(3);
    m_notesTree->setHeaderLabels({ L("Frame", "프레임"), L("Clip", "클립"), L("Note", "메모") });
    m_notesTree->setRootIsDecorated(false);
    m_notesTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_notesTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_notesTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_notesTree->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(m_notesTree, 1);
    m_notesDock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, m_notesDock);
    m_notesDock->hide();

    connect(m_noteEdit, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_syncingNote) m_noteDirty = true;
    });
    connect(save, &QPushButton::clicked, this, [this] { commitNoteEditor(); });
    connect(del, &QPushButton::clicked, this, [this] {
        if (m_noteSource.isEmpty() || m_noteFrame < 0) return;
        setNote(m_noteSource, m_noteFrame, QString());
        syncNoteEditor();
    });
    connect(exportBtn, &QPushButton::clicked, this, &MainWindow::exportNotes);
    connect(m_notesTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString id = item->data(0, Qt::UserRole).toString();
        const int frame = item->data(0, Qt::UserRole + 1).toInt();
        if (!m_soloId.isEmpty() && m_soloId != id) setSolo(QString());
        int64_t g = m_seq.globalFor(id, frame, m_seq.indexAt(m_current));
        if (g < 0 && m_soloId.isEmpty()) {
            setSolo(id);  // trimmed out of the timeline: view the source itself
            g = m_seq.globalFor(id, frame);
        }
        if (g >= 0) goToFrame(g);
        m_noteEdit->setFocus();
    });
    connect(m_notesTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_notesTree->itemAt(pos);
        if (!item) return;
        QMenu menu(this);
        const QString id = item->data(0, Qt::UserRole).toString();
        const int frame = item->data(0, Qt::UserRole + 1).toInt();
        menu.addAction(Icons::icon("trash"), L("Delete Note", "메모 삭제"), this, [this, id, frame] { setNote(id, frame, QString()); });
        menu.exec(m_notesTree->viewport()->mapToGlobal(pos));
    });
    connect(m_notesDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            refreshNotes();
            syncNoteEditor();
        }
    });
}

void MainWindow::commitNoteEditor()
{
    if (!m_noteDirty || m_noteSource.isEmpty() || m_noteFrame < 0) return;
    m_noteDirty = false;
    setNote(m_noteSource, m_noteFrame, m_noteEdit->toPlainText());
}

void MainWindow::syncNoteEditor()
{
    if (!m_notesDock || !m_noteEdit) return;
    const EditTarget t = editTarget();
    const QString id = t ? t.source->id : QString();
    const int frame = t ? t.frame : -1;
    if (id == m_noteSource && frame == m_noteFrame && !m_noteDirty) return;
    if (id != m_noteSource || frame != m_noteFrame) commitNoteEditor();  // keep what was typed on the previous frame
    m_noteSource = id;
    m_noteFrame = frame;
    m_syncingNote = true;
    QString text;
    if (t)
        if (auto it = t.source->notes.find(frame); it != t.source->notes.end()) text = it->second;
    if (m_noteEdit->toPlainText() != text) m_noteEdit->setPlainText(text);
    m_syncingNote = false;
    m_noteDirty = false;
    m_noteEdit->setEnabled(bool(t));
    m_noteLabel->setText(t ? L("%1 · source frame %2", "%1 · 소스 프레임 %2").arg(t.source->displayName()).arg(frame)
                           : L("No clip", "클립 없음"));
}

void MainWindow::setNote(const QString& sourceId, int frame, const QString& rawText)
{
    Source* s = m_project.find(sourceId);
    if (!s || frame < 0) return;
    const QString text = rawText.trimmed();
    Edit e;
    e.isNote = true;
    e.sourceId = sourceId;
    e.frame = frame;
    if (auto it = s->notes.find(frame); it != s->notes.end()) e.noteBefore = it->second;
    if (!text.isEmpty()) e.noteAfter = text;
    if (e.noteBefore == e.noteAfter) return;
    if (m_pending) commitEdit();
    m_undo.push_back(e);
    if (m_undo.size() > 1000) m_undo.removeFirst();
    m_redo.clear();
    if (e.noteAfter) s->notes[frame] = *e.noteAfter;
    else s->notes.erase(frame);
    setDirty(true);
    if (sourceId == m_noteSource && frame == m_noteFrame) {
        m_syncingNote = true;
        if (m_noteEdit->toPlainText().trimmed() != text) m_noteEdit->setPlainText(text);
        m_syncingNote = false;
        m_noteDirty = false;
    }
    refreshNotes();
    refreshTimelineEditor();
    showToast(text.isEmpty() ? L("Note deleted", "메모를 삭제했습니다") : L("Note saved · frame %1", "메모 저장 · 프레임 %1").arg(frame), 1500);
}

void MainWindow::refreshNotes()
{
    if (!m_notesTree || !m_notesDock || !m_notesDock->isVisible()) return;
    m_notesTree->clear();
    for (const Source& s : m_project.sources)
        for (const auto& [frame, text] : s.notes) {
            const int64_t g = m_seq.globalFor(s.id, frame);
            auto* item = new QTreeWidgetItem(m_notesTree);
            item->setText(0, g >= 0 ? QString::number(g) : QStringLiteral("(%1)").arg(frame));
            item->setToolTip(0, L("Timeline frame (source frame %1)", "타임라인 프레임 (소스 프레임 %1)").arg(frame));
            item->setText(1, s.displayName());
            item->setText(2, QString(text).replace('\n', QStringLiteral(" ⏎ ")));
            item->setToolTip(2, text);
            item->setData(0, Qt::UserRole, s.id);
            item->setData(0, Qt::UserRole + 1, frame);
        }
    m_notesTree->sortItems(0, Qt::AscendingOrder);
}

void MainWindow::exportNotes()
{
    commitNoteEditor();
    QSettings settings;
    const QString base = m_projectPath.isEmpty() ? QStringLiteral("notes") : QFileInfo(m_projectPath).completeBaseName() + QStringLiteral("_notes");
    QString path = QFileDialog::getSaveFileName(this, L("Export Frame Notes", "프레임 메모 내보내기"),
                                                QDir(settings.value("lastExportDir", QDir::homePath()).toString()).filePath(base + ".csv"),
                                                L("CSV (*.csv);;Markdown (*.md);;Text (*.txt)", "CSV (*.csv);;마크다운 (*.md);;텍스트 (*.txt)"));
    if (path.isEmpty()) return;
    const QString ext = QFileInfo(path).suffix().toLower();
    QString out;
    auto csvField = [](QString v) { return QStringLiteral("\"%1\"").arg(v.replace('"', QStringLiteral("\"\""))); };
    if (ext == QLatin1String("md")) out = QStringLiteral("# %1 — frame notes\n\n| Frame | Clip | Source frame | Time | Note |\n|---:|---|---:|---|---|\n").arg(m_project.timeline.name);
    else if (ext != QLatin1String("txt")) out = QStringLiteral("frame,clip,source_frame,timecode,note\n");
    int count = 0;
    for (const Source& s : m_project.sources)
        for (const auto& [frame, text] : s.notes) {
            const int64_t g = m_seq.globalFor(s.id, frame);
            const QString time = tc(frame, s.fps);
            if (ext == QLatin1String("md"))
                out += QStringLiteral("| %1 | %2 | %3 | %4 | %5 |\n").arg(g).arg(s.displayName()).arg(frame).arg(time, QString(text).replace('\n', QStringLiteral("<br>")).replace('|', QStringLiteral("\\|")));
            else if (ext == QLatin1String("txt"))
                out += QStringLiteral("[%1] %2 @ %3 (%4)\n%5\n\n").arg(g).arg(s.displayName()).arg(frame).arg(time, text);
            else
                out += QStringLiteral("%1,%2,%3,%4,%5\n").arg(g).arg(csvField(s.displayName())).arg(frame).arg(time, csvField(text));
            ++count;
        }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, L("Export Failed", "내보내기 실패"), f.errorString());
        return;
    }
    if (ext == QLatin1String("csv")) f.write("\xEF\xBB\xBF");  // Excel opens UTF-8 CSV correctly with a BOM
    f.write(out.toUtf8());
    if (!f.commit()) {
        QMessageBox::warning(this, L("Export Failed", "내보내기 실패"), f.errorString());
        return;
    }
    settings.setValue("lastExportDir", QFileInfo(path).absolutePath());
    showToast(L("Exported %1 notes", "메모 %1개를 내보냈습니다").arg(count));
}

// ============================================================================ motion graph

void MainWindow::buildMotionGraph()
{
    m_graphDock = new QDockWidget(L("Motion Graph", "모션 그래프"), this);
    m_graphDock->setObjectName("MotionGraphDock");
    m_graphDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_graph = new MotionGraph;
    m_graphDock->setWidget(m_graph);
    addDockWidget(Qt::BottomDockWidgetArea, m_graphDock);
    m_graphDock->hide();
    connect(m_graphDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            m_graphSignature.clear();
            refreshMotionGraph();
        }
    });
    connect(m_graph, &MotionGraph::trackChosen, this, [this](const QString& id) {
        m_selectedTrackId = id;
        refreshAnnotations();
        refreshMotionGraph();
    });
    connect(m_graph, &MotionGraph::frameClicked, this, [this](int sourceFrame) {
        const EditTarget t = editTarget();
        if (!t) return;
        if (m_playing) stopPlayback();
        if (t.isB) {
            const int64_t a = compareALocalFor(sourceFrame);
            if (a >= 0 && !m_seq.segments.isEmpty()) goToFrame(m_seq.segments.front().start + a);
            return;
        }
        const int64_t g = m_seq.globalFor(t.source->id, sourceFrame, m_seq.indexAt(m_current));
        if (g >= 0) goToFrame(g);
    });
    connect(m_graph, &MotionGraph::bookmarkRequested, this, [this](const QVector<int>& frames) {
        const EditTarget t = editTarget();
        if (!t) return;
        int added = 0;
        for (int f : frames)
            if (!t.source->bookmarks.count(f)) {
                AnnotationList snapshot;
                for (const Source::Entry& e : t.source->renderable(f)) snapshot.append(t.source->evaluated(*e.annotation, f));
                t.source->bookmarks[f] = snapshot;
                ++added;
            }
        setDirty(true);
        refreshMarks();
        showToast(L("Bookmarked %1 key pose(s) · Ctrl+Space plays them stepped", "키 포즈 %1개 북마크 · Ctrl+Space로 단계 재생").arg(added), 3500);
    });
}

void MainWindow::refreshMotionGraph()
{
    if (!m_graph || !m_graphDock || !m_graphDock->isVisible()) return;
    const EditTarget t = editTarget();
    if (!t) {
        if (!m_graphSignature.isEmpty()) {
            m_graph->setTracks({}, QString());
            m_graph->setData({}, QSizeF(1, 1), 24, QString());
            m_graphSignature.clear();
        }
        return;
    }
    const Track* track = t.source->track(m_selectedTrackId);
    if (!track && !t.source->tracks.isEmpty()) track = &t.source->tracks.front();
    // Rebuild only when the clip, the track or its data changed (this runs on every frame).
    QString signature = t.source->id + QLatin1Char('|');
    for (const Track& tr : t.source->tracks) signature += tr.id + tr.name + QLatin1Char(',');
    if (track) {
        signature += QStringLiteral("|%1|%2|").arg(track->id).arg(track->points.size());
        if (!track->points.empty())
            signature += QStringLiteral("%1,%2,%3").arg(track->points.begin()->first).arg(track->points.rbegin()->first)
                             .arg(track->points.rbegin()->second.x() + track->points.begin()->second.y(), 0, 'f', 6);
    }
    if (signature != m_graphSignature) {
        m_graphSignature = signature;
        QVector<std::tuple<QString, QString, QColor>> list;
        for (const Track& tr : t.source->tracks) list.append({ tr.id, tr.name, tr.color });
        m_graph->setTracks(list, track ? track->id : QString());
        m_graph->setData(track ? track->points : std::map<int, QPointF>(), t.size, std::max(1.0, t.source->fps),
                         t.source->displayName() + (track ? QStringLiteral(" · ") + track->name : QString()));
    }
    m_graph->setCurrentFrame(t.frame);
}

// ============================================================================ animator viewing aids

void MainWindow::playOnNs(int n)
{
    if (m_seq.total <= 0) return;
    if (m_playing) stopPlayback();
    m_stepEvery = std::clamp(n, 2, 12);
    togglePlay(true);
}

void MainWindow::setFlip(bool on)
{
    m_view->setFlipHorizontal(on);
    if (m_flipAction) m_flipAction->setChecked(on);
    showToast(on ? L("Mirrored view (display only — exports are not flipped)", "좌우 반전 보기 (화면 표시만, 내보내기는 원본)") : L("Mirror off", "좌우 반전 끔"), 1800);
}

void MainWindow::updateHud()
{
    if (!m_view || !m_view->showHud()) return;
    const Loc loc = locate(m_current);
    if (!loc) {
        m_view->setHud(QString());
        return;
    }
    const double fps = viewFps();
    QString line1 = QStringLiteral("F %1   %2").arg(m_current).arg(tc(m_current, fps));
    QString line2 = QStringLiteral("%1  %2 / %3").arg(QFontMetrics(font()).elidedText(loc.source->displayName(), Qt::ElideMiddle, 220))
                        .arg(loc.rawLocal).arg(std::max<int64_t>(0, loc.server->frameCount() - 1));
    QString line3 = QStringLiteral("%1 fps").arg(fps, 0, 'f', fps == std::floor(fps) ? 0 : 3);
    if (m_stepped && m_playing) line3 += m_stepEvery > 1 ? L("  ·  on %1s", "  ·  %1프레임씩").arg(m_stepEvery) : L("  ·  stepped", "  ·  단계 재생");
    if (m_view->flipHorizontal()) line3 += L("  ·  mirrored", "  ·  반전");
    m_view->setHud(line1 + '\n' + line2 + '\n' + line3);
}

void MainWindow::reloadMedia()
{
    stopPlayback();
    m_view->commitEdits();
    if (m_pending) commitEdit();
    for (Source& s : m_project.sources) {
        m_media.remove(s.id);
        if (s.media.exists()) openServerFor(s);
    }
    m_shownKey.clear();
    m_shownCompareKey.clear();
    if (m_editor) m_editor->clearThumbnails();
    rebuildSequence();
    refreshProjectTree();
    refreshClipList();
    refreshProperties();
    showToast(L("Media reopened", "미디어를 다시 열었습니다"));
}

void MainWindow::populateOnionMenu()
{
    if (!m_onionMenu) return;
    m_onionMenu->clear();
    auto* box = new QWidget;
    box->setObjectName("OnionSettings");
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(14, 10, 14, 10);
    v->setSpacing(8);
    auto* title = new QLabel(L("<b>Onion Skin</b>", "<b>어니언 스킨</b>"));
    v->addWidget(title);
    auto* enabled = new QCheckBox(L("Show onion skin  (Shift+O)", "어니언 스킨 표시  (Shift+O)"));
    enabled->setObjectName("OnionEnabled");
    enabled->setChecked(m_onion);
    v->addWidget(enabled);
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(6);
    auto spin = [&](int value, int lo, int hi, const char* name) {
        auto* s = new QSpinBox;
        s->setRange(lo, hi);
        s->setValue(value);
        s->setFixedWidth(64);
        s->setObjectName(name);
        return s;
    };
    auto* mode = new QComboBox;
    mode->setObjectName("OnionMode");
    mode->addItems({ L("Drawings by key", "그림: 키 기준"), L("Drawings by frame distance", "그림: 프레임 거리 기준") });
    mode->setCurrentIndex(m_onionMode);
    form->addRow(L("Mode", "방식"), mode);
    auto pair = [&](QSpinBox* a, QSpinBox* b) {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(new QLabel(L("before", "이전")));
        h->addWidget(a);
        h->addSpacing(8);
        h->addWidget(new QLabel(L("after", "이후")));
        h->addWidget(b);
        h->addStretch(1);
        return w;
    };
    auto* before = spin(m_onionBefore, 0, 8, "OnionBefore");
    auto* after = spin(m_onionAfter, 0, 8, "OnionAfter");
    form->addRow(L("Drawings", "그림"), pair(before, after));
    auto* vBefore = spin(m_onionVideoBefore, 0, 8, "OnionVideoBefore");
    auto* vAfter = spin(m_onionVideoAfter, 0, 8, "OnionVideoAfter");
    form->addRow(L("Video frames", "영상 프레임"), pair(vBefore, vAfter));
    auto* step = spin(m_onionStep, 1, 12, "OnionStep");
    step->setToolTip(L("Frames between ghosts: 2 = every other frame (animation on twos)", "고스트 사이 프레임 간격: 2 = 한 프레임 건너 (2s 애니메이션)"));
    form->addRow(L("Every N frames", "N프레임마다"), step);
    auto* opacity = new QSlider(Qt::Horizontal);
    opacity->setObjectName("OnionOpacity");
    opacity->setRange(5, 90);
    opacity->setValue(int(std::round(m_onionOpacity * 100)));
    opacity->setFixedWidth(150);
    form->addRow(L("Opacity (nearest)", "불투명도 (가장 가까운)"), opacity);
    auto* tint = new QCheckBox(L("Tint", "색 입히기"));
    tint->setObjectName("OnionTint");
    tint->setChecked(m_onionTint);
    auto* beforeColor = new QToolButton;
    auto* afterColor = new QToolButton;
    beforeColor->setToolTip(L("Before colour", "이전 색"));
    afterColor->setToolTip(L("After colour", "이후 색"));
    beforeColor->setIcon(colorIcon(m_onionBeforeColor));
    afterColor->setIcon(colorIcon(m_onionAfterColor));
    auto* tintRow = new QWidget;
    auto* th = new QHBoxLayout(tintRow);
    th->setContentsMargins(0, 0, 0, 0);
    th->addWidget(tint);
    th->addSpacing(8);
    th->addWidget(beforeColor);
    th->addWidget(afterColor);
    th->addStretch(1);
    form->addRow(L("Colours", "색상"), tintRow);
    auto* playing = new QCheckBox(L("Show while playing", "재생 중에도 표시"));
    playing->setObjectName("OnionWhilePlaying");
    playing->setChecked(m_onionWhilePlaying);
    form->addRow(QString(), playing);
    v->addLayout(form);
    auto* hint = new QLabel(L("Before = warm, after = cool, fading with distance — like 2D animation packages.",
                              "이전 = 따뜻한 색, 이후 = 차가운 색, 멀수록 흐려짐 — 2D 애니메이션 툴과 같은 방식."));
    hint->setObjectName("Muted");
    hint->setWordWrap(true);
    hint->setMaximumWidth(300);
    v->addWidget(hint);
    auto* act = new QWidgetAction(m_onionMenu);
    act->setDefaultWidget(box);
    m_onionMenu->addAction(act);

    auto save = [this] {
        QSettings s;
        s.setValue("onion/mode", m_onionMode);
        s.setValue("onion/before", m_onionBefore);
        s.setValue("onion/after", m_onionAfter);
        s.setValue("onion/videoBefore", m_onionVideoBefore);
        s.setValue("onion/videoAfter", m_onionVideoAfter);
        s.setValue("onion/step", m_onionStep);
        s.setValue("onion/opacity", m_onionOpacity);
        s.setValue("onion/tint", m_onionTint);
        s.setValue("onion/whilePlaying", m_onionWhilePlaying);
        s.setValue("onion/beforeColor", m_onionBeforeColor.name());
        s.setValue("onion/afterColor", m_onionAfterColor.name());
        s.remove("onion/video");
        refreshAnnotations();
    };
    connect(enabled, &QCheckBox::toggled, this, [this](bool on) { if (on != m_onion) setOnion(on); });
    connect(mode, &QComboBox::currentIndexChanged, this, [this, save](int i) { m_onionMode = i; save(); });
    connect(before, &QSpinBox::valueChanged, this, [this, save](int x) { m_onionBefore = x; save(); });
    connect(after, &QSpinBox::valueChanged, this, [this, save](int x) { m_onionAfter = x; save(); });
    connect(vBefore, &QSpinBox::valueChanged, this, [this, save](int x) { m_onionVideoBefore = x; save(); });
    connect(vAfter, &QSpinBox::valueChanged, this, [this, save](int x) { m_onionVideoAfter = x; save(); });
    connect(step, &QSpinBox::valueChanged, this, [this, save](int x) { m_onionStep = x; save(); });
    connect(opacity, &QSlider::valueChanged, this, [this, save](int x) { m_onionOpacity = x / 100.0; save(); });
    connect(tint, &QCheckBox::toggled, this, [this, save](bool on) { m_onionTint = on; save(); });
    connect(playing, &QCheckBox::toggled, this, [this, save](bool on) { m_onionWhilePlaying = on; save(); });
    auto pick = [this, save](QToolButton* button, QColor* target) {
        const QColor c = QColorDialog::getColor(*target, this, L("Onion Colour", "어니언 색상"));
        if (!c.isValid()) return;
        *target = c;
        button->setIcon(colorIcon(c));
        save();
    };
    connect(beforeColor, &QToolButton::clicked, this, [pick, beforeColor, this] { pick(beforeColor, &m_onionBeforeColor); });
    connect(afterColor, &QToolButton::clicked, this, [pick, afterColor, this] { pick(afterColor, &m_onionAfterColor); });
    // Keep the checkbox in step with Shift+O / the toolbar button.
    connect(m_onionMenu, &QMenu::aboutToShow, enabled, [this, enabled] {
        const QSignalBlocker block(enabled);
        enabled->setChecked(m_onion);
    });
}

// ============================================================================ help

void MainWindow::showShortcuts()
{
    QDialog dialog(this);
    dialog.setObjectName("ShortcutsDialog");
    dialog.setWindowTitle(L("Keyboard Shortcuts", "단축키"));
    dialog.resize(980, 720);
    auto* v = new QVBoxLayout(&dialog);
    v->setContentsMargins(18, 16, 18, 14);
    v->setSpacing(10);
    auto* head = new QHBoxLayout;
    auto* title = new QLabel(L("<span style='font-size:18px; font-weight:600'>Keyboard Shortcuts</span>"
                               "<br><span style='color:#a3a097'>Your current keys — change them in Tools › HotKey Manager.</span>",
                               "<span style='font-size:18px; font-weight:600'>단축키</span>"
                               "<br><span style='color:#a3a097'>현재 설정된 키입니다 — 도구 › 단축키 관리자에서 바꿀 수 있습니다.</span>"));
    head->addWidget(title, 1);
    auto* search = new QLineEdit;
    search->setObjectName("ShortcutSearch");
    search->setPlaceholderText(L("Search commands or keys…", "명령 또는 키 검색…"));
    search->setClearButtonEnabled(true);
    search->setFixedWidth(260);
    head->addWidget(search, 0, Qt::AlignBottom);
    v->addLayout(head);

    // Categories follow the menus; mouse / tablet / timeline gestures are listed separately.
    struct Row {
        QString command, keys;
    };
    QMap<QString, QVector<Row>> groups;
    QStringList order;
    auto group = [&](const QString& name) -> QVector<Row>& {
        if (!order.contains(name)) order << name;
        return groups[name];
    };
    for (QAction* top : menuBar()->actions()) {
        QMenu* menu = top->menu();
        if (!menu) continue;
        std::function<void(QMenu*, const QString&)> walk = [&](QMenu* m, const QString& cat) {
            for (QAction* a : m->actions()) {
                if (a->menu()) {
                    walk(a->menu(), cat);
                    continue;
                }
                if (a->isSeparator() || a->shortcut().isEmpty()) continue;
                QString text = a->text().section('\t', 0, 0);
                text.remove('&');
                group(cat).append({ text, a->shortcut().toString(QKeySequence::NativeText) });
            }
        };
        QString cat = top->text();
        cat.remove('&');
        walk(menu, cat);
    }
    // Alternate keys that live outside the menus (Ctrl+Y, Backspace, Esc...)
    for (QAction* a : m_hotkeys)
        if (!a->property("commandMenu").toString().isEmpty() || a->shortcut().isEmpty()) continue;
        else group(L("Also", "보조 키")).append({ a->property("commandLabel").toString(), a->shortcut().toString(QKeySequence::NativeText) });
    auto& mouse = group(L("Mouse & Pen", "마우스 & 펜"));
    mouse << Row{ L("Scrub anywhere (Shift: slower)", "어디서나 스크럽 (Shift: 천천히)"), QStringLiteral("Alt+Drag") }
          << Row{ L("Pan the view", "화면 이동"), L("Middle Drag", "가운데 버튼 드래그") }
          << Row{ L("Zoom", "확대 / 축소"), L("Wheel", "휠") }
          << Row{ L("Straight pen stroke", "직선 펜 획"), L("Shift+Drag", "Shift+드래그") }
          << Row{ L("Keep proportions / snap rotation 15°", "비율 유지 / 15° 회전 스냅"), L("Shift+Handle", "Shift+핸들") }
          << Row{ L("Context menu (notes, markers, bars)", "컨텍스트 메뉴 (주석, 마커, 바)"), L("Right Click", "우클릭") };
    auto& tl = group(L("Timeline Panel", "타임라인 패널"));
    tl << Row{ L("Move a clip", "클립 이동"), L("Drag Clip", "클립 드래그") }
       << Row{ L("Trim In / Out (snaps to the playhead)", "In / Out 트림 (재생 위치에 스냅)"), L("Drag Clip Edge", "클립 가장자리 드래그") }
       << Row{ L("Remove the selected clip", "선택한 클립 삭제"), QStringLiteral("Delete") }
       << Row{ L("Zoom around the pointer", "포인터 기준 확대"), QStringLiteral("Ctrl+Wheel") }
       << Row{ L("Zoom in / out", "확대 / 축소"), QStringLiteral("+ / −") }
       << Row{ L("Add a source", "소스 추가"), L("Drag from Project View", "프로젝트 뷰에서 드래그") };

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    // Two balanced columns (each card goes to the shorter one), so short groups don't leave holes.
    auto* columns = new QHBoxLayout(content);
    columns->setContentsMargins(0, 0, 6, 0);
    columns->setSpacing(14);
    QVBoxLayout* column[2] = { new QVBoxLayout, new QVBoxLayout };
    int columnRows[2] = { 0, 0 };
    for (QVBoxLayout* c : column) {
        c->setSpacing(14);
        columns->addLayout(c, 1);
    }
    struct Card {
        QWidget* card;
        QVector<std::pair<QWidget*, QString>> rows;  // row widget, search text
    };
    auto cards = std::make_shared<QVector<Card>>();
    const QString keycap = QStringLiteral("background:%1; border:1px solid %2; border-bottom-width:2px; border-radius:5px; padding:1px 7px; "
                                          "color:%3; font-family:'Cascadia Mono',Consolas,monospace; font-size:11px;")
                               .arg(Pal::raised.name(), Pal::border.name(), Pal::textStrong.name());
    int index = 0;
    for (const QString& name : order) {
        const QVector<Row>& rows = groups[name];
        if (rows.isEmpty()) continue;
        auto* card = new QWidget;
        card->setObjectName("ShortcutCard");
        card->setAttribute(Qt::WA_StyledBackground);
        card->setStyleSheet(QStringLiteral("#ShortcutCard { background:%1; border:1px solid %2; border-radius:10px; }").arg(Pal::panel.name(), Pal::divider.name()));
        auto* cv = new QVBoxLayout(card);
        cv->setContentsMargins(14, 10, 14, 12);
        cv->setSpacing(5);
        auto* heading = new QLabel(name.toUpper());
        heading->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:600; letter-spacing:1px;").arg(Pal::accent.name()));
        cv->addWidget(heading);
        Card c{ card, {} };
        for (const Row& r : rows) {
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 1, 0, 1);
            h->setSpacing(4);
            auto* label = new QLabel(r.command);
            label->setStyleSheet(QStringLiteral("color:%1;").arg(Pal::text.name()));
            h->addWidget(label, 1);
            for (const QString& chord : r.keys.split(QStringLiteral(", "))) {
                auto* key = new QLabel(chord);
                key->setStyleSheet(keycap);
                h->addWidget(key);
            }
            cv->addWidget(row);
            c.rows.append({ row, (r.command + ' ' + r.keys).toLower() });
        }
        const int side = columnRows[0] <= columnRows[1] ? 0 : 1;
        column[side]->addWidget(card);
        columnRows[side] += int(rows.size()) + 2;  // + heading and padding
        cards->append(c);
        ++index;
    }
    for (QVBoxLayout* c : column) c->addStretch(1);
    scroll->setWidget(content);
    v->addWidget(scroll, 1);
    connect(search, &QLineEdit::textChanged, &dialog, [cards](const QString& text) {
        const QString q = text.trimmed().toLower();
        for (const Card& c : *cards) {
            bool any = false;
            for (const auto& [row, hay] : c.rows) {
                const bool match = q.isEmpty() || hay.contains(q);
                row->setVisible(match);
                any = any || match;
            }
            c.card->setVisible(any);
        }
    });

    auto* buttons = new QHBoxLayout;
    auto* manager = new QPushButton(Icons::icon("keyboard"), L("HotKey Manager...", "단축키 관리자..."));
    auto* guide = new QPushButton(Icons::icon("help"), L("User Guide", "사용 설명서"));
    auto* close = new QPushButton(L("Close", "닫기"));
    close->setDefault(true);
    buttons->addWidget(manager);
    buttons->addWidget(guide);
    buttons->addStretch(1);
    buttons->addWidget(close);
    v->addLayout(buttons);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(manager, &QPushButton::clicked, &dialog, [this, &dialog] {
        dialog.accept();
        HotkeyManager hm(m_hotkeys, this);
        hm.exec();
    });
    connect(guide, &QPushButton::clicked, &dialog, [this] { openUserGuide(); });
    search->setFocus();
    dialog.exec();
}

void MainWindow::openUserGuide()
{
    const QStringList candidates = {
        QApplication::applicationDirPath() + QStringLiteral("/docs/CHARONOS-User-Guide.html"),
        QApplication::applicationDirPath() + QStringLiteral("/CHARONOS-User-Guide.html"),
    };
    for (const QString& path : candidates)
        if (QFileInfo::exists(path)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            return;
        }
    QMessageBox::information(this, L("User Guide", "사용 설명서"),
                             L("The guide wasn't found next to CHARONOS.exe (docs\\CHARONOS-User-Guide.html). Reinstall CHARONOS to restore it.",
                               "CHARONOS.exe 옆에서 설명서(docs\\CHARONOS-User-Guide.html)를 찾지 못했습니다. 다시 설치하면 복원됩니다."));
}
