// MainWindow (1.1): multi-track playback (layers, gaps, the timeline's sound mix), editing
// navigation (edit points, J/K/L shuttle), the Timeline menu and the analysis filters (Filter
// menu). Split from MainWindow.cpp to keep both readable.
#include "MainWindow.h"
#include "Lang.h"

#include "Icons.h"
#include "Palette.h"
#include "media/CanvasSource.h"
#include "ui/ConsoleDock.h"
#include "ui/Timeline.h"
#include "ui/TimelineEditor.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

QRectF fitRect(const QSizeF& src, const QRectF& box)
{
    if (src.isEmpty()) return box;
    const double s = std::min(box.width() / src.width(), box.height() / src.height());
    const QSizeF sz = src * s;
    return QRectF(box.center() - QPointF(sz.width() / 2, sz.height() / 2), sz);
}

QString tcOf(int64_t frame, double fps)
{
    const int f = std::max(1, int(std::round(fps)));
    const int64_t secs = frame / f;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(secs / 3600, 2, 10, QChar('0'))
        .arg((secs / 60) % 60, 2, 10, QChar('0'))
        .arg(secs % 60, 2, 10, QChar('0'))
        .arg(int(frame % f), 2, 10, QChar('0'));
}

} // namespace

// ============================================================================ canvas and layers

QSize MainWindow::canvasSize() const
{
    const TimelineDoc& d = m_project.timeline;
    if (d.width > 0 && d.height > 0) return { d.width, d.height };
    // The first picture of the timeline sets the frame, like a sequence created from a clip.
    const TimelineClip* first = nullptr;
    int64_t best = std::numeric_limits<int64_t>::max();
    const tl::LengthFn len = lengthFn();
    for (const TimelineClip& c : d.clips) {
        if (!c.isVideo() || !m_media.server(c.sourceId)) continue;
        const int64_t start = tl::span(c, len).start;
        if (start < best || (start == best && first && c.track < first->track)) {
            best = start;
            first = &c;
        }
    }
    if (first) {
        const MediaInfo& info = m_media.server(first->sourceId)->info();
        if (info.width > 0 && info.height > 0) return { info.width, info.height };
    }
    return { 1920, 1080 };
}

QRectF MainWindow::placementFor(const TimelineClip& c, QSize media, QSize canvas) const
{
    // Fitted into the canvas (aspect kept), then the clip's own zoom around its centre and offset.
    const QRectF fit = fitRect(QSizeF(media), QRectF(QPointF(0, 0), QSizeF(canvas)));
    const QPointF centre = fit.center() + QPointF(c.posX * canvas.width(), c.posY * canvas.height());
    const QSizeF size = fit.size() * c.scale;
    return QRectF(centre - QPointF(size.width() / 2, size.height() / 2), size);
}

QVector<MainWindow::PlacedLayer> MainWindow::visibleLayers(int64_t global) const
{
    QVector<PlacedLayer> out;
    const QSize canvas = canvasSize();
    const QRectF full(QPointF(0, 0), QSizeF(canvas));
    for (const tl::Layer& l : tl::layersAt(m_project.timeline, global, lengthFn(), onlineFn())) {
        FrameServer* srv = m_media.server(l.clip->sourceId);
        if (!srv) continue;
        PlacedLayer p;
        p.clip = l.clip;
        p.local = std::clamp<int64_t>(l.local, 0, std::max<int64_t>(0, srv->frameCount() - 1));
        p.rect = placementFor(*l.clip, QSize(srv->info().width, srv->info().height), canvas);
        p.server = srv;
        out << p;
    }
    // Everything under an opaque, normal, full-frame clip is hidden: don't decode it.
    for (int i = int(out.size()) - 1; i > 0; --i) {
        const TimelineClip& c = *out[i].clip;
        if (c.opacity >= 0.999 && c.blend == BlendMode::Normal && out[i].rect.adjusted(-0.5, -0.5, 0.5, 0.5).contains(full)) {
            out.remove(0, i);
            break;
        }
    }
    return out;
}

void MainWindow::updateLayers(const Loc& loc)
{
    const QVector<PlacedLayer> layers = visibleLayers(m_current);
    QVector<Viewport::Layer> below, above;
    QSet<QString> sources;
    QVector<int64_t> extra;
    QStringList key;
    bool passedPrimary = false;
    for (const PlacedLayer& p : layers) {
        if (p.clip->uid == loc.segment->clipId) {
            passedPrimary = true;
            continue;
        }
        const tl::Span s = tl::span(*p.clip, lengthFn());
        if (p.server == loc.server) extra << p.local;  // A's source is busy decoding A's own frame
        else p.server->request(p.local, m_playing, s.in, s.last());
        sources.insert(p.clip->sourceId);
        Viewport::Layer l;
        l.frame = p.server->frame(p.local);
        l.rect = p.rect;
        l.opacity = float(p.clip->opacity);
        l.blend = p.clip->blend;
        if (m_filter.mode == AnalysisFilter::Mode::MovingSilhouette) {
            l.reference = m_plates.value(p.clip->sourceId);
            if (!l.reference) requestPlate(p.clip->sourceId);
        } else if (m_filter.mode == AnalysisFilter::Mode::MotionHighlight && p.local > 0) {
            l.reference = p.server->frame(p.local - 1);
        }
        (passedPrimary ? above : below) << l;
        key << QStringLiteral("%1:%2:%3%4").arg(p.clip->uid).arg(p.local).arg(l.frame ? 1 : 0).arg(l.reference ? 1 : 0);
    }
    m_layerSources = sources;
    if (extra != m_layerExtra) {
        m_layerExtra = extra;
        loc.server->requestExtra(m_onionVideoFrames + m_layerExtra);
    }
    const QString k = key.join(QLatin1Char('|')) + QStringLiteral("/%1x%2").arg(canvasSize().width()).arg(canvasSize().height());
    if (k == m_layersKey && m_view->layerCount() == below.size() + above.size()) return;
    m_layersKey = k;
    m_view->setLayers(below, above);
}

bool MainWindow::layersReady(int64_t global)
{
    const Loc l = locate(global);
    if (!l) return true;  // a gap is always ready
    bool ready = l.server->isCached(l.local);
    if (!ready) l.server->request(l.local, true, l.segment->sourceFrame(0), l.segment->sourceFrame(l.segment->count - 1));
    for (const PlacedLayer& p : visibleLayers(global)) {
        if (p.clip->uid == l.segment->clipId || p.server->isCached(p.local)) continue;
        ready = false;
        if (p.server != l.server) {
            const tl::Span s = tl::span(*p.clip, lengthFn());
            p.server->request(p.local, true, s.in, s.last());
        }
    }
    return ready;
}

void MainWindow::showGap(int64_t global)
{
    // No picture here: black, like an editing program (or "Media Offline" over a missing file).
    m_shownKey.clear();
    m_layersKey.clear();
    m_layerSources.clear();
    m_view->setFrame(nullptr);
    m_view->setLayers({}, {});
    m_view->setPrimaryPlacement({}, {});
    m_view->setMediaSize(canvasSize());
    bool offline = false;
    for (const TimelineClip& c : m_project.timeline.clips)
        if (c.isVideo() && c.enabled && tl::trackVisible(m_project.timeline, c.kind, c.track) && !m_media.server(c.sourceId) &&
            tl::span(c, lengthFn()).contains(global))
            offline = true;
    m_view->setPlaceholder(offline ? L("Media Offline", "미디어 없음") : QString());
    m_view->setAnnotations({});
    m_view->setCompareAnnotations({});
    m_view->setOnion({}, {});
    m_view->setTracks({}, 0, QString(), 0, false);
    m_view->setPassiveTracks({}, 0);
    // While playing, warm up the next picture.
    QStringList active;
    const int idx = m_seq.indexAt(global);
    for (int i = idx + 1; i < m_seq.segments.size(); ++i) {
        const Sequence::Segment& next = m_seq.segments[i];
        if (next.isGap()) continue;
        if (m_playing && next.start - global < int64_t(viewFps() * 2))
            if (FrameServer* ns = m_media.server(next.sourceId)) {
                ns->request(next.sourceFrame(0), true, next.sourceFrame(0), next.sourceFrame(next.count - 1));
                active << next.sourceId;
            }
        break;
    }
    m_media.setActive(active);
    const Sequence::Segment* seg = m_seq.at(global);
    m_timeline->setCurrent(global);
    m_clipBar->setClip(offline ? L("Media Offline", "미디어 없음") : L("Gap", "빈 공간"), seg ? seg->count : 0, seg ? global - seg->start : 0,
                       seg ? seg->start : 0, 0);
    if (m_editor && m_editorDock && m_editorDock->isVisible()) m_editor->setCurrent(m_soloId.isEmpty() ? global : -1);
    updateReadout();
    if (!m_lastSourceId.isEmpty()) {
        m_lastSourceId.clear();
        refreshProperties();
        updateTitle();
    }
    if (m_notesDock && m_notesDock->isVisible()) syncNoteEditor();
}

// ============================================================================ playback

void MainWindow::timelineTick()
{
    const auto [a, b] = playRange();
    const double fps = viewFps();
    // With sound, the sound card is the clock (the mix is in timeline seconds).
    if (m_audio.playingMix()) {
        const double pos = m_audio.position() - m_audioOffsetMs / 1000.0;
        const int64_t heard = int64_t(std::floor(pos * fps + 1e-6));
        if (heard > b && (m_realtime || m_current >= b)) {
            goToFrame(a);
            syncAudio(true);
            countShownFrame();
            return;
        }
        if (heard <= m_current || heard < a) return;
        const int64_t next = m_realtime ? std::min(heard, b) : std::min(heard, m_current + 1);
        if (!layersReady(next)) {
            // The pictures aren't there yet: hold the sound until they are.
            m_audio.stop();
            m_audioPlayingId.clear();
            m_clock.restart();
            m_nextDueNs = qint64(1e9 / fps);
            return;
        }
        goToFrame(next);
        if (next < heard) syncAudio(false);  // re-anchor only beyond the normal jitter
        countShownFrame();
        return;
    }
    const qint64 now = m_clock.nsecsElapsed();
    if (now < m_nextDueNs) return;
    const qint64 frameNs = qint64(1e9 / fps);
    const int64_t len = b - a + 1;
    const int64_t steps = m_realtime ? 1 + (now - m_nextDueNs) / frameNs : 1;
    const int64_t base = (m_current < a || m_current > b) ? a - 1 : m_current;
    const int64_t next = a + ((base - a + steps) % len + len) % len;
    if (!layersReady(next)) return;
    const bool wrapped = next < m_current;
    goToFrame(next);
    syncAudio(wrapped);  // the mix starts as soon as the picture is there
    m_nextDueNs += steps * frameNs;
    if (now - m_nextDueNs > 3 * frameNs) m_nextDueNs = now + frameNs;
    countShownFrame();
}

void MainWindow::jumpEditPoint(int direction)
{
    if (m_seq.total <= 0) return;
    if (m_playing) stopPlayback();
    QVector<int64_t> points;
    if (m_soloId.isEmpty()) {
        points = tl::editPoints(m_project.timeline, lengthFn());
    } else {
        for (const Sequence::Segment& s : m_seq.segments) points << s.start << s.start + s.count;
    }
    points << m_seq.total - 1;
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    int64_t target = -1;
    if (direction > 0) {
        for (int64_t p : points)
            if (p > m_current) {
                target = p;
                break;
            }
    } else {
        for (auto it = points.rbegin(); it != points.rend(); ++it)
            if (*it < m_current) {
                target = *it;
                break;
            }
    }
    if (target >= 0) goToFrame(std::min(target, m_seq.total - 1));
}

void MainWindow::markClip()
{
    const Loc loc = locate(m_current);
    if (!loc) return;
    int64_t first = loc.segment->start, last = loc.segment->start + loc.segment->count - 1;
    if (const TimelineClip* c = m_project.timeline.clip(loc.segment->clipId); c && m_soloId.isEmpty()) {
        const tl::Span s = tl::span(*c, lengthFn());
        first = s.start;
        last = s.end() - 1;
    }
    if (m_soloId.isEmpty()) {
        m_project.timeline.loopIn = int(first);
        m_project.timeline.loopOut = int(last);
        m_project.timeline.loopEnabled = true;
        setDirty(true);
    } else {
        m_soloIn = first;
        m_soloOut = last;
        m_soloLoop = true;
    }
    updateLoopUi();
    showToast(L("In / Out = clip  %1 – %2", "In / Out = 클립  %1 – %2").arg(first).arg(last));
}

void MainWindow::shuttle(int direction)
{
    if (m_seq.total <= 0) return;
    if (direction == 0) {
        m_shuttleTimer.stop();
        m_shuttle = 0;
        stopPlayback();
        return;
    }
    int next = direction > 0 ? (m_shuttle > 0 ? std::min(8, m_shuttle * 2) : 1) : (m_shuttle < 0 ? std::max(-8, m_shuttle * 2) : -1);
    if (next == 1 && m_playing && !m_shuttleTimer.isActive()) next = 2;  // already playing forward: faster
    if (next == 1) {
        m_shuttleTimer.stop();
        m_shuttle = 1;
        if (!m_playing) togglePlay();
        showToast(QStringLiteral("▶  1×"), 900);
        return;
    }
    if (m_playing) stopPlayback();
    m_shuttle = next;
    m_shuttlePos = double(m_current);
    m_shuttleClock.restart();
    m_shuttleTimer.start(8);
    showToast(QStringLiteral("%1  %2×").arg(next < 0 ? QStringLiteral("◀") : QStringLiteral("▶")).arg(std::abs(next)), 900);
}

// ============================================================================ sound

bool MainWindow::timelineAudio() const { return m_soloId.isEmpty(); }

void MainWindow::invalidateMix()
{
    m_mix.reset();
    if (m_audio.playingMix()) m_audio.updateMix(timelineMix());  // gain / mute edits are heard right away
}

std::shared_ptr<const AudioMix> MainWindow::timelineMix()
{
    if (m_mix) return m_mix;
    auto mix = std::make_shared<AudioMix>();
    const TimelineDoc& doc = m_project.timeline;
    mix->fps = std::max(1.0, doc.fps);
    const tl::LengthFn len = lengthFn();
    for (const TimelineClip& c : doc.clips) {
        if (!c.isAudio() || !c.enabled || c.muted || !tl::trackAudible(doc, c.track)) continue;
        auto it = m_audioClips.find(c.sourceId);
        FrameServer* srv = m_media.server(c.sourceId);
        if (it == m_audioClips.end() || !it->second || !srv) continue;
        const tl::Span s = tl::span(c, len);
        AudioMix::Item item;
        item.pcm = it->second;
        item.start = s.start;
        item.count = s.count;
        item.times.resize(size_t(s.count) + 1);
        // Sound advances in actual seconds. Never stretch a native video frame to one
        // timeline frame: 60 fps footage on a 24 fps timeline must keep its pitch and length.
        for (int64_t k = 0; k <= s.count; ++k)
            item.times[size_t(k)] = s.timing.timed() ? s.timeAt(k) : srv->info().timeOf(s.in + k);
        item.gain = dbToGain(c.gainDb) * dbToGain(doc.audioTracks.value(c.track).gainDb);
        item.fadeIn = c.fadeIn;
        item.fadeOut = c.fadeOut;
        if (s.timing.timed()) item.sourceEnd = s.timeEnd;
        mix->items.push_back(std::move(item));
    }
    m_mix = mix;
    return m_mix;
}

void MainWindow::computePeaks(const QString& id)
{
    auto it = m_audioClips.find(id);
    FrameServer* srv = m_media.server(id);
    if (it == m_audioClips.end() || !it->second || !srv) {
        m_peaks.erase(id);
        return;
    }
    const AudioClip& clip = *it->second;
    const MediaInfo& info = srv->info();
    const int64_t frames = srv->frameCount();
    const int bins = TimelineEditor::kPeakBins;
    QVector<float> peaks(qsizetype(frames * bins), 0.f);
    const int64_t n = clip.frames();
    for (int64_t f = 0; f < frames; ++f) {
        const double t0 = info.timeOf(f), t1 = info.timeOf(f + 1);
        for (int b = 0; b < bins; ++b) {
            const double ta = t0 + (t1 - t0) * b / bins, tb = t0 + (t1 - t0) * (b + 1) / bins;
            const int64_t s0 = std::max<int64_t>(0, int64_t((ta - clip.startTime) * clip.rate));
            const int64_t s1 = std::min<int64_t>(n, int64_t((tb - clip.startTime) * clip.rate) + 1);
            int peak = 0;
            for (int64_t s = s0; s < s1; s += 2)
                peak = std::max({ peak, std::abs(int(clip.pcm[size_t(s) * 2])), std::abs(int(clip.pcm[size_t(s) * 2 + 1])) });
            peaks[qsizetype(f * bins + b)] = std::pow(peak / 32768.0f, 0.6f);
        }
    }
    m_peaks[id] = std::move(peaks);
}

// ============================================================================ Timeline menu

void MainWindow::buildTimelineMenu()
{
    connect(&m_shuttleTimer, &QTimer::timeout, this, [this] {
        const double dt = m_shuttleClock.nsecsElapsed() / 1e9;
        m_shuttleClock.restart();
        m_shuttlePos += m_shuttle * viewFps() * dt;
        const double last = double(std::max<int64_t>(0, m_seq.total - 1));
        if (m_shuttlePos <= 0 || m_shuttlePos >= last) {
            m_shuttlePos = std::clamp(m_shuttlePos, 0.0, last);
            m_shuttleTimer.stop();
            m_shuttle = 0;
        }
        const int64_t f = int64_t(std::llround(m_shuttlePos));
        if (f != m_current) goToFrame(f);
    });

    QMenu* menu = menuBar()->addMenu(L("Timeline", "타임라인"));
    menu->setObjectName("TimelineMenu");
    auto editor = [this] {
        if (m_editorStale) refreshTimelineEditor(true);
        m_editor->setCurrent(m_soloId.isEmpty() ? m_current : -1);
        return m_editor;
    };
    addCommand(menu, L("Show Timeline", "타임라인 열기"), QKeySequence(), [this] { setPanelVisible(m_editorDock, true); })->setIcon(Icons::icon("layout"));
    menu->addSeparator();
    QAction* select = addCommand(menu, L("Selection Tool  (V in the timeline)", "선택 도구  (타임라인에서 V)"), QKeySequence(), [this] { m_editor->setTool(TimelineEditor::Tool::Select); });
    select->setIcon(Icons::icon("cursor"));
    QAction* blade = addCommand(menu, L("Blade Tool  (B in the timeline)", "자르기 도구  (타임라인에서 B)"), QKeySequence(), [this] { m_editor->setTool(TimelineEditor::Tool::Blade); });
    blade->setIcon(Icons::icon("split"));
    menu->addSeparator();
    QAction* ripple = addCommand(menu, L("Ripple Editing", "리플 편집"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), [this] {
        m_editor->setRipple(!m_editor->ripple());
        showToast(m_editor->ripple() ? L("Ripple editing on: later clips follow edits", "리플 편집 켬: 뒤 클립이 편집을 따라갑니다")
                                     : L("Ripple editing off: gaps stay where they are", "리플 편집 끔: 빈 공간이 그대로 남습니다"));
    });
    ripple->setCheckable(true);
    ripple->setObjectName("Timeline/Ripple");
    QAction* snap = addCommand(menu, L("Snapping", "스냅"), QKeySequence(), [this] { m_editor->setSnap(!m_editor->snap()); });
    snap->setCheckable(true);
    QAction* linked = addCommand(menu, L("Linked Selection", "연결 선택"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L), [this] {
        m_editor->setLinkedSelection(!m_editor->linkedSelection());
    });
    linked->setCheckable(true);
    menu->addSeparator();
    addCommand(menu, L("Delete Clips (leave gap)", "클립 삭제 (빈 공간 유지)"), QKeySequence(), [editor] { editor()->deleteSelection(false); });
    addCommand(menu, L("Ripple Delete Clips", "클립 리플 삭제"), QKeySequence(), [editor] { editor()->deleteSelection(true); });
    addCommand(menu, L("Close Gap at Playhead", "재생 위치의 빈 공간 닫기"), QKeySequence(), [this, editor] {
        if (!editor()->closeGapAtPlayhead()) showToast(L("No gap under the playhead on that track.", "그 트랙의 재생 위치에 빈 공간이 없습니다."));
    })->setIcon(Icons::icon("gap"));
    addCommand(menu, L("Nudge Clips Left", "클립 왼쪽으로 미세 이동"), QKeySequence(Qt::ALT | Qt::Key_Comma), [editor] { editor()->nudgeSelection(-1); });
    addCommand(menu, L("Nudge Clips Right", "클립 오른쪽으로 미세 이동"), QKeySequence(Qt::ALT | Qt::Key_Period), [editor] { editor()->nudgeSelection(1); });
    menu->addSeparator();
    addCommand(menu, L("Go to Previous Edit", "이전 편집점"), QKeySequence(), [this] { jumpEditPoint(-1); });
    addCommand(menu, L("Go to Next Edit", "다음 편집점"), QKeySequence(), [this] { jumpEditPoint(1); });
    addCommand(menu, L("Mark Clip (In / Out)", "클립 구간 표시 (In / Out)"), QKeySequence(Qt::Key_X), [this] { markClip(); });
    addCommand(menu, L("Go to In", "In으로 이동"), QKeySequence(Qt::ALT | Qt::Key_I), [this] {
        stopPlayback();
        goToFrame(m_soloId.isEmpty() ? m_project.timeline.loopIn : m_soloIn);
    });
    addCommand(menu, L("Go to Out", "Out으로 이동"), QKeySequence(Qt::ALT | Qt::Key_O), [this] {
        stopPlayback();
        goToFrame(m_soloId.isEmpty() ? m_project.timeline.loopOut : m_soloOut);
    });
    addCommand(menu, L("Play Reverse", "역재생"), QKeySequence(Qt::Key_J), [this] { shuttle(-1); });
    menu->addSeparator();
    addCommand(menu, L("Add Video Track", "비디오 트랙 추가"), QKeySequence(), [editor] { editor()->addTrack(TimelineClip::Kind::Video); })->setIcon(Icons::icon("track-add"));
    addCommand(menu, L("Add Audio Track", "오디오 트랙 추가"), QKeySequence(), [editor] { editor()->addTrack(TimelineClip::Kind::Audio); });
    addCommand(menu, L("Add Annotation Lane", "주석 레인 추가"), QKeySequence(), [this] {
        m_project.timeline.noteTracks << TrackState{};
        setDirty(true);
        refreshTimelineEditor();
    });
    addCommand(menu, L("Delete Empty Tracks", "빈 트랙 삭제"), QKeySequence(), [this] {
        editTimeline([](TimelineDoc& doc) {
            const TimelineDoc before = doc;
            tl::trimTrackLists(doc);
            return !doc.sameLayout(before);
        }, L("Delete Empty Tracks", "빈 트랙 삭제"));
    });
    menu->addSeparator();
    addCommand(menu, L("Timeline Settings...", "타임라인 설정..."), QKeySequence(), [this] {
        QDialog d(this);
        d.setWindowTitle(L("Timeline Settings", "타임라인 설정"));
        auto* form = new QFormLayout(&d);
        auto* autoSize = new QCheckBox(L("Same as the first clip", "첫 클립과 같게"));
        autoSize->setChecked(m_project.timeline.width <= 0);
        auto* w = new QSpinBox;
        auto* h = new QSpinBox;
        const QSize canvas = canvasSize();
        for (QSpinBox* b : { w, h }) b->setRange(16, 16384);
        w->setValue(canvas.width());
        h->setValue(canvas.height());
        w->setEnabled(!autoSize->isChecked());
        h->setEnabled(!autoSize->isChecked());
        connect(autoSize, &QCheckBox::toggled, &d, [w, h](bool on) { w->setEnabled(!on); h->setEnabled(!on); });
        auto* size = new QWidget;
        auto* sl = new QHBoxLayout(size);
        sl->setContentsMargins(0, 0, 0, 0);
        sl->addWidget(w);
        sl->addWidget(new QLabel(QStringLiteral("×")));
        sl->addWidget(h);
        form->addRow(L("Canvas", "캔버스"), autoSize);
        form->addRow(QString(), size);
        auto* fps = new QDoubleSpinBox;
        fps->setRange(1, 240);
        fps->setDecimals(3);
        fps->setValue(m_project.timeline.fps);
        form->addRow(L("Frame rate", "프레임 레이트"), fps);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return;
        editTimeline([&](TimelineDoc& doc) {
            doc.width = autoSize->isChecked() ? 0 : w->value();
            doc.height = autoSize->isChecked() ? 0 : h->value();
            return true;
        }, L("Canvas", "캔버스"));
        if (std::abs(fps->value() - m_project.timeline.fps) > 1e-6) {
            if (!m_soloId.isEmpty()) m_project.timeline.fps = fps->value(), setDirty(true);
            else setViewFps(fps->value());
        }
        m_layersKey.clear();
        goToFrame(m_current);
    });
    connect(menu, &QMenu::aboutToShow, this, [this, ripple, snap, linked, select, blade] {
        ripple->setChecked(m_editor->ripple());
        snap->setChecked(m_editor->snap());
        linked->setChecked(m_editor->linkedSelection());
        select->setCheckable(true);
        blade->setCheckable(true);
        select->setChecked(m_editor->tool() == TimelineEditor::Tool::Select);
        blade->setChecked(m_editor->tool() == TimelineEditor::Tool::Blade);
    });
}

// ============================================================================ analysis filters

double MainWindow::effectiveThreshold()
{
    if (!m_filter.usesAutoThreshold()) return m_filter.threshold();
    const Loc loc = locate(m_current);
    if (!loc) return m_filter.threshold();
    auto it = m_otsu.find(loc.source->id);
    if (it == m_otsu.end()) {
        // One automatic split per source (per frame it would flicker).
        const FrameCache::Ptr frame = loc.server->frame(loc.local);
        if (!frame) return m_filter.threshold();
        it = m_otsu.insert(loc.source->id, analysis::otsu(*frame));
    }
    return *it;
}

void MainWindow::refreshFilter()
{
    if (!m_filter.enabled()) {
        if (m_view->filter().enabled()) m_view->setFilter(m_filter, m_filter.threshold());
        m_view->setFilterReference(nullptr, nullptr);
        return;
    }
    const Loc loc = locate(m_current);
    if (!loc) return;
    m_view->setFilter(m_filter, effectiveThreshold());
    FrameCache::Ptr a, b;
    FrameServer* bs = m_compareMode != CompareMode::Off ? m_media.server(m_compareId) : nullptr;
    if (m_filter.mode == AnalysisFilter::Mode::MovingSilhouette) {
        a = m_plates.value(loc.source->id);
        if (!a) requestPlate(loc.source->id);
        if (bs) {
            b = m_plates.value(m_compareId);
            if (!b) requestPlate(m_compareId);
        }
    } else if (m_filter.mode == AnalysisFilter::Mode::MotionHighlight) {
        if (loc.local > 0) a = loc.server->frame(loc.local - 1);
        if (bs) {
            const int64_t bl = compareFrameFor(loc.rawLocal);
            if (bl > 0) b = bs->frame(bl - 1);
        }
    }
    m_view->setFilterReference(a, b);
}

void MainWindow::requestPlate(const QString& sourceId)
{
    if (m_plates.contains(sourceId) || m_platePending.contains(sourceId)) return;
    const Source* s = m_project.find(sourceId);
    if (!s || s->media.kind == MediaRef::Kind::Canvas) return;
    m_platePending.insert(sourceId);
    for (auto it = m_plateThreads.begin(); it != m_plateThreads.end();) {
        if ((*it)->isFinished()) {
            delete *it;
            it = m_plateThreads.erase(it);
        } else {
            ++it;
        }
    }
    const MediaRef media = s->media;
    const double fps = s->fps;
    showToast(L("Building the background plate of %1…", "%1의 배경 플레이트를 만드는 중…").arg(s->displayName()), 3000);
    QThread* t = QThread::create([this, sourceId, media, fps, cancel = m_audioCancel] {
        // The shot's still background: the median of frames spread over the whole clip.
        QImage plate;
        try {
            QString err;
            std::unique_ptr<FrameSource> src = MediaLibrary::openSource(media.isSequence(), media.path, media.frames, fps, &err);
            if (src) {
                const int64_t n = src->info().frameCount;
                const int samples = int(std::clamp<int64_t>(n, 1, 25));
                QVector<QImage> frames;
                for (int i = 0; i < samples && !cancel->load(); ++i) {
                    QImage img;
                    const int64_t f = samples == 1 ? 0 : (n - 1) * i / (samples - 1);
                    if (src->decode(f, img, &err)) frames << (img.width() > 640 ? img.scaledToWidth(640, Qt::SmoothTransformation) : img);
                }
                if (!cancel->load()) plate = analysis::backgroundPlate(frames);
            }
        } catch (const std::bad_alloc&) {
            plate = QImage();
        }
        if (cancel->load()) return;
        QMetaObject::invokeMethod(this, [this, sourceId, plate] {
            m_platePending.remove(sourceId);
            if (!m_project.find(sourceId) || plate.isNull()) return;
            m_plates.insert(sourceId, VideoFrame::share(plate));
            m_layersKey.clear();
            refreshFilter();
            if (const Loc loc = locate(m_current); loc && m_soloId.isEmpty()) updateLayers(loc);
            if (m_console) m_console->append(QStringLiteral("Filter: background plate ready (%1×%2)").arg(plate.width()).arg(plate.height()));
        }, Qt::QueuedConnection);
    });
    m_plateThreads.push_back(t);
    t->start(QThread::LowPriority);
}

void MainWindow::setFilterMode(AnalysisFilter::Mode mode)
{
    m_filter.mode = mode;
    QSettings().setValue("filter/state", QString::fromUtf8(QJsonDocument(m_filter.toJson()).toJson(QJsonDocument::Compact)));
    for (auto it = m_filterActions.begin(); it != m_filterActions.end(); ++it) it.value()->setChecked(it.key() == int(mode));
    m_layersKey.clear();
    refreshFilter();
    if (const Loc loc = locate(m_current); loc && m_soloId.isEmpty()) updateLayers(loc);
    refreshAnnotations();  // motion highlight wants the previous frame decoded
    refreshTimelineEditor();
    if (mode != AnalysisFilter::Mode::None) showToast(AnalysisFilter::name(mode) + QStringLiteral("  ·  ") + AnalysisFilter::description(mode), 3200);
}

void MainWindow::buildFilterMenu()
{
    m_filter = AnalysisFilter::fromJson(QJsonDocument::fromJson(QSettings().value("filter/state").toString().toUtf8()).object());
    m_filter.mode = AnalysisFilter::Mode::None;  // the picture starts as it is
    QMenu* menu = menuBar()->addMenu(L("Filter", "필터"));
    menu->setObjectName("FilterMenu");
    auto* group = new QActionGroup(menu);
    using Mode = AnalysisFilter::Mode;
    const QList<std::pair<Mode, QKeySequence>> modes = {
        { Mode::None, QKeySequence(Qt::ALT | Qt::Key_0) },
        { Mode::Silhouette, QKeySequence(Qt::ALT | Qt::Key_S) },
        { Mode::MovingSilhouette, QKeySequence(Qt::ALT | Qt::Key_M) },
        { Mode::Edges, QKeySequence(Qt::ALT | Qt::Key_E) },
        { Mode::Value, QKeySequence(Qt::ALT | Qt::Key_G) },
        { Mode::Notan, QKeySequence(Qt::ALT | Qt::Key_N) },
        { Mode::MotionHighlight, QKeySequence(Qt::ALT | Qt::Key_H) },
    };
    for (const auto& [mode, key] : modes) {
        const Mode m = mode;
        QAction* a = addCommand(menu, AnalysisFilter::name(m), key, [this, m] { setFilterMode(m_filter.mode == m && m != Mode::None ? Mode::None : m); });
        a->setCheckable(true);
        a->setChecked(m == m_filter.mode);
        a->setStatusTip(AnalysisFilter::description(m));
        a->setToolTip(AnalysisFilter::description(m));
        a->setObjectName(QStringLiteral("Filter/%1").arg(int(m)));
        if (m == Mode::Silhouette || m == Mode::MovingSilhouette) a->setIcon(Icons::icon("silhouette"));
        group->addAction(a);
        m_filterActions.insert(int(m), a);
        if (m == Mode::None) menu->addSeparator();
    }
    menu->setToolTipsVisible(true);
    menu->addSeparator();
    QAction* invert = menu->addAction(L("Bright Subject (invert)", "밝은 피사체 (반전)"));
    invert->setCheckable(true);
    connect(invert, &QAction::toggled, this, [this](bool on) {
        m_filter.invert = on;
        setFilterMode(m_filter.mode);
    });
    QMenu* behind = menu->addMenu(L("Picture Behind the Shape", "형태 뒤의 원본"));
    for (int pct : { 0, 25, 50 }) {
        behind->addAction(pct == 0 ? L("Paper only", "종이만") : QStringLiteral("%1%").arg(pct), this, [this, pct] {
            m_filter.showOriginal = pct / 100.0;
            setFilterMode(m_filter.mode);
        });
    }
    addCommand(menu, L("Filter Settings...", "필터 설정..."), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F), [this] { showFilterSettings(); })->setIcon(Icons::icon("filter"));
    menu->addAction(L("Rebuild Background Plate", "배경 플레이트 다시 만들기"), this, [this] {
        const Loc loc = locate(m_current);
        if (!loc) return;
        m_plates.remove(loc.source->id);
        m_otsu.remove(loc.source->id);
        requestPlate(loc.source->id);
    });
    connect(menu, &QMenu::aboutToShow, this, [this, invert] {
        const QSignalBlocker block(invert);
        invert->setChecked(m_filter.invert);
    });
}

void MainWindow::showFilterSettings()
{
    auto* d = findChild<QDialog*>("FilterSettings");
    if (d) {
        d->show();
        d->raise();
        return;
    }
    d = new QDialog(this);
    d->setObjectName("FilterSettings");
    d->setWindowTitle(L("Filter Settings", "필터 설정"));
    d->setAttribute(Qt::WA_DeleteOnClose);
    auto* form = new QFormLayout(d);
    auto* mode = new QComboBox;
    for (int i = 0; i < AnalysisFilter::kModes; ++i) mode->addItem(AnalysisFilter::name(AnalysisFilter::Mode(i)));
    mode->setCurrentIndex(int(m_filter.mode));
    form->addRow(L("Filter", "필터"), mode);
    auto* about = new QLabel;
    about->setWordWrap(true);
    about->setObjectName("Muted");
    about->setMinimumWidth(320);
    form->addRow(about);
    auto* autoThr = new QCheckBox(L("Automatic (split the brightness of this clip)", "자동 (이 클립의 밝기를 둘로 나눔)"));
    form->addRow(QString(), autoThr);
    auto* thr = new QSlider(Qt::Horizontal);
    thr->setRange(0, 100);
    auto* thrValue = new QLabel;
    auto* thrRow = new QWidget;
    auto* tl_ = new QHBoxLayout(thrRow);
    tl_->setContentsMargins(0, 0, 0, 0);
    tl_->addWidget(thr, 1);
    tl_->addWidget(thrValue);
    form->addRow(L("Threshold", "임계값"), thrRow);
    auto* soft = new QSlider(Qt::Horizontal);
    soft->setRange(0, 30);
    form->addRow(L("Edge softness", "가장자리 부드럽게"), soft);
    auto* behind = new QSlider(Qt::Horizontal);
    behind->setRange(0, 100);
    form->addRow(L("Picture behind", "뒤의 원본"), behind);
    auto* invert = new QCheckBox(L("Bright subject on a dark background", "어두운 배경의 밝은 피사체"));
    form->addRow(QString(), invert);
    auto* levels = new QSpinBox;
    levels->setRange(2, 3);
    form->addRow(L("Notan values", "노탄 단계"), levels);
    auto colourButton = [d](QColor* target, const std::function<void()>& changed) {
        auto* b = new QPushButton;
        auto paint = [b, target] { b->setStyleSheet(QStringLiteral("QPushButton { background: %1; min-width: 60px; min-height: 18px; }").arg(target->name())); };
        paint();
        QObject::connect(b, &QPushButton::clicked, d, [d, target, paint, changed] {
            const QColor c = QColorDialog::getColor(*target, d);
            if (!c.isValid()) return;
            *target = c;
            paint();
            changed();
        });
        return b;
    };
    auto apply = [this] { setFilterMode(m_filter.mode); };
    auto* colours = new QWidget;
    auto* cl = new QHBoxLayout(colours);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(new QLabel(L("Shape", "형태")));
    cl->addWidget(colourButton(&m_filter.ink, apply));
    cl->addWidget(new QLabel(L("Paper", "종이")));
    cl->addWidget(colourButton(&m_filter.paper, apply));
    cl->addWidget(new QLabel(L("Motion", "움직임")));
    cl->addWidget(colourButton(&m_filter.accent, apply));
    form->addRow(L("Colours", "색"), colours);
    auto* plate = new QPushButton(L("Rebuild Background Plate", "배경 플레이트 다시 만들기"));
    form->addRow(QString(), plate);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    form->addRow(close);
    connect(close, &QDialogButtonBox::rejected, d, &QDialog::close);

    auto sync = [this, mode, about, autoThr, thr, thrValue, soft, behind, invert, levels] {
        const QSignalBlocker b1(mode), b2(autoThr), b3(thr), b4(soft), b5(behind), b6(invert), b7(levels);
        mode->setCurrentIndex(int(m_filter.mode));
        about->setText(AnalysisFilter::description(m_filter.mode));
        const bool autoCapable = m_filter.mode == AnalysisFilter::Mode::Silhouette || m_filter.mode == AnalysisFilter::Mode::Notan;
        autoThr->setVisible(autoCapable);
        autoThr->setChecked(m_filter.autoThreshold);
        const double t = effectiveThreshold();
        thr->setValue(int(std::lround(t * 100)));
        thr->setEnabled(!m_filter.usesAutoThreshold());
        thrValue->setText(QString::number(t, 'f', 2));
        soft->setValue(int(std::lround(m_filter.softness * 100)));
        behind->setValue(int(std::lround(m_filter.showOriginal * 100)));
        invert->setChecked(m_filter.invert);
        levels->setValue(m_filter.levels);
    };
    sync();
    connect(mode, &QComboBox::currentIndexChanged, d, [this, sync](int i) {
        setFilterMode(AnalysisFilter::Mode(i));
        sync();
    });
    connect(autoThr, &QCheckBox::toggled, d, [this, sync](bool on) {
        m_filter.autoThreshold = on;
        setFilterMode(m_filter.mode);
        sync();
    });
    connect(thr, &QSlider::valueChanged, d, [this, thrValue](int v) {
        m_filter.setThreshold(v / 100.0);
        thrValue->setText(QString::number(v / 100.0, 'f', 2));
        setFilterMode(m_filter.mode);
    });
    connect(soft, &QSlider::valueChanged, d, [this](int v) {
        m_filter.softness = v / 100.0;
        setFilterMode(m_filter.mode);
    });
    connect(behind, &QSlider::valueChanged, d, [this](int v) {
        m_filter.showOriginal = v / 100.0;
        setFilterMode(m_filter.mode);
    });
    connect(invert, &QCheckBox::toggled, d, [this](bool on) {
        m_filter.invert = on;
        setFilterMode(m_filter.mode);
    });
    connect(levels, &QSpinBox::valueChanged, d, [this](int v) {
        m_filter.levels = v;
        setFilterMode(m_filter.mode);
    });
    connect(plate, &QPushButton::clicked, d, [this] {
        const Loc loc = locate(m_current);
        if (!loc) return;
        m_plates.remove(loc.source->id);
        m_otsu.remove(loc.source->id);
        requestPlate(loc.source->id);
    });
    d->show();
}
