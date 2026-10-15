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

struct TimelineDoc {
    QString name = QStringLiteral("Default Timeline");
    QStringList sourceIds;  // clips played back to back
    double fps = 24.0;
    bool loopEnabled = false;
    int loopIn = 0;
    int loopOut = 0;
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
