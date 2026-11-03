#pragma once

#include "model/NoteClips.h"
#include "model/TimelineModel.h"

#include <QBitArray>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QWidget>

#include <functional>

class QLabel;
class QScrollBar;
class QSlider;
class QToolButton;
class TimelineCanvas;

// Editing-program style timeline (Window > Timeline). From the top: annotation lanes (each
// drawing, text or shape is a clip block), video tracks V1..Vn stacked upwards (higher tracks
// draw over lower ones in the viewer), then sound tracks A1..An below the centre line.
//
// Clips are dragged, trimmed, cut (blade or Ctrl+B), deleted (lift or ripple), nudged,
// copied and pasted, with snapping and linked picture / sound. Edits are applied to a copy of
// the timeline with the tl:: functions (the same ones the menus use) and previewed live; the
// finished edit is reported through edited() and MainWindow commits it to the undo stack.
class TimelineEditor : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Blade };
    static constexpr int kPeakBins = 8;  // waveform bins per source frame

    // What the editor needs to know about each source on the timeline.
    struct SourceInfo {
        QString name;
        int64_t frames = 0;
        SourceTiming timing;
        bool online = true;
        bool hasAudio = false;
        QVector<int> keys, bookmarks, notes;  // source frames
        QVector<NoteClip> noteClips;          // annotation clips (source frames)
        QVector<float> peaks;                 // 0..1, kPeakBins per source frame
    };

    explicit TimelineEditor(QWidget* parent = nullptr);

    void setModel(const TimelineDoc& doc, QHash<QString, SourceInfo> sources, const QString& title);
    const TimelineDoc& doc() const;
    void setCurrent(qint64 frame);  // -1 = the viewer isn't on this timeline
    void setLoop(bool enabled, qint64 in, qint64 out);
    void setCacheMask(QBitArray mask);
    void setBanner(const QString& text);
    void setThumbnailProvider(std::function<QImage(const QString& sourceId, qint64 frame)> provider);
    void clearThumbnails();

    QStringList selectedClips() const;
    QString selectedClip() const;
    void setSelectedClips(const QStringList& uids);
    void setSelectedClip(const QString& uid);
    QString selectedNoteSource() const;
    QString selectedNoteKey() const;

    void setTool(Tool tool);
    Tool tool() const;
    void setRipple(bool on);
    bool ripple() const;
    void setSnap(bool on);
    bool snap() const;
    void setLinkedSelection(bool on);
    bool linkedSelection() const;

    // Commands (also in the Timeline menu). Each reports its result through edited().
    void deleteSelection(bool ripple);
    bool splitAtPlayhead();  // the selected clips under the playhead, or every unlocked track there
    void nudgeSelection(int frames);
    void selectAllClips();
    void copySelection();
    void cutSelection(bool ripple);
    void pasteAtPlayhead();
    bool closeGapAtPlayhead();
    void addTrack(TimelineClip::Kind kind);

    void zoomIn();
    void zoomOut();
    void zoomToFit();
    int clipCount() const;  // video clips
    // Widget positions on the timeline canvas (automation and tests): a point inside a clip
    // (fraction 0..1 of its length; 0 / 1 = its edges) and a point on a track at a frame.
    QWidget* canvas() const;
    QPointF clipPoint(const QString& uid, double fraction) const;
    QPointF trackPoint(int kind, int track, qint64 frame) const;
    QPointF notePoint(const QString& sourceId, const QString& key, double fraction) const;
    qint64 currentFrame() const;
    int annotationLaneHeight(int lane) const;
    QSize sizeHint() const override { return { 900, 340 }; }

signals:
    void scrubbed(qint64 frame);
    void edited(TimelineDoc doc, QString what, QStringList select);
    void selectionChanged(QStringList uids);
    void clipActivated(QString uid);
    void contextMenuRequested(QString uid, qint64 frame, QPoint globalPos);
    void trackMenuRequested(int kind, int track, QPoint globalPos);  // kind: 0 video, 1 audio, 2 annotation
    void trackChanged(int kind, int track, TrackState state);       // header switches (eye, lock, mute, solo)
    void noteSelected(QString sourceId, QString key);
    void noteMoved(QString sourceId, QString key, int newFirst, int newLane);
    void noteTrimmed(QString sourceId, QString key, int newFirst, int newLast);
    void noteSplit(QString sourceId, QString key, int frame);
    void noteErased(QString sourceId, QString key);
    void noteDuplicated(QString sourceId, QString key, int newFirst);
    void noteMenuRequested(QString sourceId, QString key, qint64 frame, QPoint globalPos);
    void sourcesDropped(QStringList sourceIds, int videoTrack, qint64 frame);
    void filesDropped(QStringList paths, int videoTrack, qint64 frame);
    void shuttle(int direction);  // J / L (K = 0) while the timeline has focus
    void modesChanged();

private:
    friend class FeatureRegression;
    friend class TimelineCanvas;
    void updateZoomUi();
    void syncModeButtons();

    TimelineCanvas* m_canvas = nullptr;
    QScrollBar* m_scroll = nullptr;
    QScrollBar* m_vscroll = nullptr;
    QSlider* m_zoom = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_banner = nullptr;
    QLabel* m_clock = nullptr;
    QToolButton* m_selectTool = nullptr;
    QToolButton* m_bladeTool = nullptr;
    QToolButton* m_rippleBtn = nullptr;
    QToolButton* m_snapBtn = nullptr;
    QToolButton* m_linkBtn = nullptr;
    QToolButton* m_timecode = nullptr;
};
