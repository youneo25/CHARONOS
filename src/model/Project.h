#pragma once

#include "Annotation.h"
#include "Track.h"
#include "ReviewRange.h"

#include <QStringList>

#include <map>
#include <optional>

struct MediaRef {
    enum class Kind { None, Video, ImageSequence, Canvas };
    Kind kind = Kind::None;
    QString path;        // video file (absolute)
    QStringList frames;  // image sequence files (absolute)
    int width = 0;
    int height = 0;
    QString fileName;    // display name / relink hint

    bool isSequence() const { return kind == Kind::ImageSequence; }
    QString firstFile() const { return isSequence() ? frames.value(0) : path; }
    bool exists() const;
};

// One imported clip with its own annotations and bookmarks (keyed by the clip's own frame).
// Inheritance rule (same as the Electron app): a frame with an explicit entry shows exactly
// that list (an empty list breaks inheritance); otherwise the nearest earlier explicit entry.
struct Source {
    QString id;
    MediaRef media;
    QString folder;  // project bin, independent of timeline order
    double fps = 24.0;
    int frameCount = 0;  // cached from the decoder, informational
    std::map<int, AnnotationList> annotations;
    std::map<int, AnnotationList> bookmarks;
    std::map<int, QString> notes;  // review notes per source frame (Window > Frame Notes)
    QVector<Track> tracks;

    struct Entry {
        const Annotation* annotation;
        bool inherited;
    };
    QVector<Entry> renderable(int frame) const;
    Annotation evaluated(Annotation a, int frame, bool inverse = false) const;
    std::optional<AnnotationList> explicitAt(int frame) const;
    void setExplicit(int frame, std::optional<AnnotationList> list);
    AnnotationList& editable(int frame);  // materializes inherited annotations into this frame
    QVector<int> annotatedFrames() const;
    QVector<ReviewRange> reviewRanges(qint64 count) const;
    int keyAt(int frame) const;  // explicit frame whose annotations show at this frame, or -1
    Track* track(const QString& id);
    QVector<int> bookmarkFrames() const;
    QString displayName() const { return media.fileName; }
};

// How a video clip is composited over the tracks below it.
enum class BlendMode { Normal, Add, Screen, Multiply, Difference };

// One entry on the timeline: a source, optionally trimmed, on a video or audio track. The same
// source can appear more than once (e.g. after splitting); annotations stay on the source, so
// every instance shows them. Clips sit at explicit timeline frames, so gaps between them are kept.
struct TimelineClip {
    enum class Kind { Video, Audio };
    QString uid;
    QString sourceId;
    int in = 0;    // first source frame used
    int out = -1;  // last source frame used (inclusive); -1 = to the end of the source

    Kind kind = Kind::Video;
    int track = 0;         // V1 / A1 = 0
    qint64 start = -1;     // timeline frame showing `in`; -1 = right after the previous clip on the track (1.0 files)
    double sourceStart = -1, sourceEnd = -1;  // exact source seconds after a timeline edit; -1 resolves in/out
    bool enabled = true;
    QString link;          // uid of the linked partner (a video clip's sound, or the picture of a sound clip)
    QString name;          // label override (empty = source name)
    QColor color;          // label colour (invalid = automatic)
    // video compositing (relative to the timeline canvas)
    double opacity = 1.0;
    double scale = 1.0;
    double posX = 0.0, posY = 0.0;  // offset in canvas widths / heights
    BlendMode blend = BlendMode::Normal;
    // sound
    double gainDb = 0.0;
    bool muted = false;
    int fadeIn = 0, fadeOut = 0;  // frames

    static QString newUid();
    static TimelineClip of(const QString& sourceId, Kind kind = Kind::Video)
    {
        TimelineClip c;
        c.uid = newUid();
        c.sourceId = sourceId;
        c.kind = kind;
        return c;
    }
    bool trimmed() const { return in > 0 || out >= 0; }
    bool isVideo() const { return kind == Kind::Video; }
    bool isAudio() const { return kind == Kind::Audio; }
    bool hasTransform() const { return scale != 1.0 || posX != 0.0 || posY != 0.0; }
    bool operator==(const TimelineClip&) const = default;
};

// Per-track switches. Video and annotation tracks: shown / hidden. Audio tracks: audible / muted.
struct TrackState {
    QString name;          // empty = "V1", "A2", "N1"...
    bool visible = true;
    bool locked = false;
    bool solo = false;     // audio
    double gainDb = 0.0;   // audio
    bool operator==(const TrackState&) const = default;
};

struct TimelineDoc {
    QString name = QStringLiteral("Default Timeline");
    QVector<TimelineClip> clips;  // video and audio clips, each at its own track and start frame
    double fps = 24.0;
    bool loopEnabled = false;
    int loopIn = 0;
    int loopOut = 0;
    QVector<TrackState> videoTracks{ TrackState{} };
    QVector<TrackState> audioTracks{ TrackState{} };
    QVector<TrackState> noteTracks{ TrackState{} };  // annotation lanes above the video tracks
    int width = 0, height = 0;  // canvas; 0 = the size of the first video clip
    bool linkAudioOnOpen = false;  // 1.0 file: create the sound clips once the media is open (not saved)

    // Video sources in timeline order (what builds before 1.1 play back to back).
    QStringList sourceIds() const;
    int indexOf(const QString& uid) const
    {
        for (int i = 0; i < clips.size(); ++i)
            if (clips[i].uid == uid) return i;
        return -1;
    }
    TimelineClip* clip(const QString& uid)
    {
        const int i = indexOf(uid);
        return i < 0 ? nullptr : &clips[i];
    }
    const TimelineClip* clip(const QString& uid) const
    {
        const int i = indexOf(uid);
        return i < 0 ? nullptr : &clips[i];
    }
    QVector<TrackState>& tracks(TimelineClip::Kind k) { return k == TimelineClip::Kind::Video ? videoTracks : audioTracks; }
    const QVector<TrackState>& tracks(TimelineClip::Kind k) const { return k == TimelineClip::Kind::Video ? videoTracks : audioTracks; }
    int videoClipCount() const
    {
        int n = 0;
        for (const TimelineClip& c : clips) n += c.isVideo();
        return n;
    }
    // The editable part (what undo restores): clips, tracks, canvas and timeline rate (not the loop).
    bool sameLayout(const TimelineDoc& o) const
    {
        return clips == o.clips && videoTracks == o.videoTracks && audioTracks == o.audioTracks &&
               noteTracks == o.noteTracks && width == o.width && height == o.height && fps == o.fps;
    }
    void takeLayout(const TimelineDoc& o)
    {
        clips = o.clips;
        videoTracks = o.videoTracks;
        audioTracks = o.audioTracks;
        noteTracks = o.noteTracks;
        width = o.width;
        height = o.height;
        fps = o.fps;
    }
};

class Project {
public:
    static constexpr const char* kExtension = "frvm";

    QStringList folders;
    QVector<Source> sources;
    TimelineDoc timeline;

    Source* find(const QString& id);
    const Source* find(const QString& id) const;
    static QString newSourceId();

    bool save(const QString& path, QString* error) const;
    bool load(const QString& path, QString* error);  // .frvm v3/v4 and legacy Electron .json

private:
    QJsonObject toJson(const QString& projectPath) const;
    void fromJson(const QJsonObject& o, const QString& projectPath);
};
