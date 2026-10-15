#include "Project.h"
#include "Lang.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

namespace {

QJsonObject framesToJson(const std::map<int, AnnotationList>& m)
{
    QJsonObject o;
    for (const auto& [frame, list] : m) o[QString::number(frame)] = annotationsToJson(list);
    return o;
}

std::map<int, AnnotationList> framesFromJson(const QJsonValue& v)
{
    std::map<int, AnnotationList> m;
    if (v.isArray()) {  // very old files: one flat list for frame 0
        m[0] = annotationsFromJson(v.toArray());
        return m;
    }
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool ok = false;
        const int frame = it.key().toInt(&ok);
        if (ok && frame >= 0) m[frame] = annotationsFromJson(it.value().toArray());
    }
    return m;
}

QString resolvePath(const QString& absolute, const QString& relative, const QString& projectDir)
{
    if (!absolute.isEmpty() && QFileInfo::exists(absolute)) return absolute;
    if (!relative.isEmpty() && !projectDir.isEmpty()) {
        const QString p = QDir(projectDir).absoluteFilePath(relative);
        if (QFileInfo::exists(p)) return QDir::cleanPath(p);
    }
    return absolute;
}

QJsonObject mediaToJson(const MediaRef& m, const QDir& dir)
{
    QJsonObject o;
    if (m.kind == MediaRef::Kind::Video) {
        o["type"] = "video";
        o["path"] = m.path;
        o["relativePath"] = dir.relativeFilePath(m.path);
    } else if (m.kind == MediaRef::Kind::ImageSequence) {
        o["type"] = "imageSequence";
        QJsonArray abs, rel;
        for (const QString& f : m.frames) {
            abs.append(f);
            rel.append(dir.relativeFilePath(f));
        }
        o["frames"] = abs;
        o["relativeFrames"] = rel;
    } else if (m.kind == MediaRef::Kind::Canvas) {
        o["type"] = "canvas";
    } else {
        o["type"] = "none";
    }
    o["width"] = m.width;
    o["height"] = m.height;
    o["fileName"] = m.fileName;
    return o;
}

MediaRef mediaFromJson(const QJsonObject& m, const QString& dir)
{
    MediaRef r;
    const QString type = m.value("type").toString();
    r.fileName = m.value("fileName").toString();
    r.width = m.value("width").toInt();
    r.height = m.value("height").toInt();
    if (type == "canvas") {
        r.kind = MediaRef::Kind::Canvas;
    } else if (type == "video") {
        r.kind = MediaRef::Kind::Video;
        r.path = resolvePath(m.value("path").toString(), m.value("relativePath").toString(), dir);
    } else if (type == "imageSequence") {
        r.kind = MediaRef::Kind::ImageSequence;
        const QJsonArray abs = m.value("frames").toArray(), rel = m.value("relativeFrames").toArray();
        for (qsizetype i = 0; i < abs.size(); ++i)
            r.frames << resolvePath(abs[i].toString(), i < rel.size() ? rel[i].toString() : QString(), dir);
    }
    return r;
}

} // namespace

bool MediaRef::exists() const
{
    if (kind == Kind::Canvas) return true;
    const QString f = firstFile();
    return !f.isEmpty() && QFileInfo::exists(f);
}

// ---------------------------------------------------------------- Source

QVector<Source::Entry> Source::renderable(int frame) const
{
    QVector<Entry> out;
    auto it = annotations.upper_bound(frame);
    if (it == annotations.begin()) return out;
    --it;
    const bool inherited = it->first != frame;
    out.reserve(it->second.size());
    for (const Annotation& a : it->second) out.append({ &a, inherited });
    return out;
}

std::optional<AnnotationList> Source::explicitAt(int frame) const
{
    auto it = annotations.find(frame);
    if (it == annotations.end()) return std::nullopt;
    return it->second;
}

Annotation Source::evaluated(Annotation a, int frame, bool inverse) const
{
    for (const Track& t : tracks) {
        if (t.id != a.trackId || t.points.empty()) continue;
        // Hold the nearest endpoint outside the solved range; interpolate internal gaps.
        auto hi = t.points.lower_bound(frame);
        QPointF pos;
        if (hi == t.points.end()) pos = t.points.rbegin()->second;
        else if (hi == t.points.begin() || hi->first == frame) pos = hi->second;
        else {
            const auto lo = std::prev(hi);
            const double k = double(frame - lo->first) / (hi->first - lo->first);
            pos = lo->second * (1-k) + hi->second * k;
        }
        const QPointF delta = (pos - a.trackAnchor) * (inverse ? -1 : 1);
        a.tx += delta.x();
        a.ty += delta.y();
        break;
    }
    return a;
}

void Source::setExplicit(int frame, std::optional<AnnotationList> list)
{
    if (list)
        annotations[frame] = std::move(*list);
    else
        annotations.erase(frame);
}

AnnotationList& Source::editable(int frame)
{
    auto it = annotations.find(frame);
    if (it != annotations.end()) return it->second;
    AnnotationList copy;
    for (const Entry& e : renderable(frame)) copy.append(*e.annotation);
    return annotations[frame] = std::move(copy);
}

QVector<int> Source::annotatedFrames() const
{
    QVector<int> out;
    for (const auto& [frame, list] : annotations)
        if (!list.isEmpty()) out.append(frame);
    return out;
}

int Source::keyAt(int frame) const
{
    auto it = annotations.upper_bound(frame);
    if (it == annotations.begin()) return -1;
    return std::prev(it)->first;
}

QVector<ReviewRange> Source::reviewRanges(qint64 count) const
{
    QVector<ReviewRange> ranges;
    if (count <= 0) return ranges;
    for (auto it = annotations.begin(); it != annotations.end(); ++it) {
        const auto next = std::next(it);
        // Empty keys are edits too, but do not color the empty held interval.
        const qint64 end = it->second.isEmpty() ? it->first :
            (next == annotations.end() ? count - 1 : qint64(next->first) - 1);
        const qint64 start = std::max<qint64>(0, it->first);
        if (start < count && end >= start) ranges.append({start, std::min(end, count - 1)});
    }
    for (const Track& track : tracks) {
        if (track.points.empty()) continue;
        const qint64 first = std::max<qint64>(0, track.points.begin()->first);
        const qint64 last = std::min<qint64>(count - 1, track.points.rbegin()->first);
        if (first <= last) ranges.append({first, last});
    }
    return mergedReviewRanges(std::move(ranges));
}

Track* Source::track(const QString& id)
{
    for (Track& t : tracks)
        if (t.id == id) return &t;
    return nullptr;
}

QVector<int> Source::bookmarkFrames() const
{
    QVector<int> out;
    for (const auto& [frame, list] : bookmarks) out.append(frame);
    return out;
}

// ---------------------------------------------------------------- Project

Source* Project::find(const QString& id)
{
    for (Source& s : sources)
        if (s.id == id) return &s;
    return nullptr;
}

const Source* Project::find(const QString& id) const
{
    for (const Source& s : sources)
        if (s.id == id) return &s;
    return nullptr;
}

QString Project::newSourceId() { return QStringLiteral("s") + QUuid::createUuid().toString(QUuid::Id128).left(10); }

QJsonObject Project::toJson(const QString& projectPath) const
{
    const QDir dir = QFileInfo(projectPath).absoluteDir();
    QJsonArray srcs;
    for (const Source& s : sources) {
        QJsonObject o;
        o["id"] = s.id;
        o["folder"] = s.folder;
        o["media"] = mediaToJson(s.media, dir);
        o["fps"] = s.fps;
        o["frameCount"] = s.frameCount;
        o["annotations"] = framesToJson(s.annotations);
        o["keyframes"] = framesToJson(s.bookmarks);
        QJsonArray tracks;
        for (const Track& t : s.tracks) tracks.append(t.toJson());
        if (!tracks.isEmpty()) o["tracks"] = tracks;
        srcs.append(o);
    }
    QJsonObject tl;
    tl["name"] = timeline.name;
    tl["sources"] = QJsonArray::fromStringList(timeline.sourceIds);
    tl["fps"] = timeline.fps;
    tl["loopEnabled"] = timeline.loopEnabled;
    tl["loopIn"] = timeline.loopIn;
    tl["loopOut"] = timeline.loopOut;

    QJsonObject o;
    o["format"] = "FrameReview";
    o["version"] = 5;
    o["folders"] = QJsonArray::fromStringList(folders);
    o["app"] = QStringLiteral("CHARONOS " FRAMEREVIEW_VERSION);
    o["sources"] = srcs;
    o["timelines"] = QJsonArray{ tl };
    return o;
}

void Project::fromJson(const QJsonObject& o, const QString& projectPath)
{
    const QString dir = QFileInfo(projectPath).absolutePath();
    *this = Project();

    if (o.value("version").toInt() >= 4) {
        for (const auto& name : o.value("folders").toArray())
            if (!name.toString().trimmed().isEmpty() && !folders.contains(name.toString())) folders << name.toString();
        for (const QJsonValue& v : o.value("sources").toArray()) {
            const QJsonObject so = v.toObject();
            Source s;
            s.id = so.value("id").toString();
            s.folder = so.value("folder").toString();
            if (!s.folder.isEmpty() && !folders.contains(s.folder)) folders << s.folder;
            if (s.id.isEmpty()) s.id = newSourceId();
            s.media = mediaFromJson(so.value("media").toObject(), dir);
            s.fps = so.value("fps").toDouble(24.0);
            s.frameCount = so.value("frameCount").toInt();
            s.annotations = framesFromJson(so.value("annotations"));
            s.bookmarks = framesFromJson(so.value("keyframes"));
            for (const QJsonValue& t : so.value("tracks").toArray()) s.tracks.append(Track::fromJson(t.toObject()));
            sources.append(s);
        }
        const QJsonObject tl = o.value("timelines").toArray().first().toObject();
        timeline.name = tl.value("name").toString(timeline.name);
        for (const QJsonValue& v : tl.value("sources").toArray())
            if (find(v.toString())) timeline.sourceIds << v.toString();
        timeline.fps = tl.value("fps").toDouble(sources.isEmpty() ? 24.0 : sources.first().fps);
        timeline.loopEnabled = tl.value("loopEnabled").toBool();
        timeline.loopIn = tl.value("loopIn").toInt();
        timeline.loopOut = tl.value("loopOut").toInt();
        return;
    }

    // v3 .frvm or legacy Electron .json: a single clip.
    Source s;
    s.id = newSourceId();
    s.fps = o.value("fps").toDouble(24.0);
    if (s.fps <= 0) s.fps = 24.0;
    s.annotations = framesFromJson(o.value("annotations"));
    s.bookmarks = framesFromJson(o.value("keyframes"));
    if (o.contains("media")) {
        s.media = mediaFromJson(o.value("media").toObject(), dir);
    } else {
        s.media.fileName = o.value("videoFileName").toString();
        const QJsonObject seq = o.value("imageSequence").toObject();
        if (o.value("mode").toString() == "imageSequence" && !seq.isEmpty()) {
            s.media.kind = MediaRef::Kind::ImageSequence;
            for (const QJsonValue& v : seq.value("frames").toArray()) s.media.frames << v.toString();
            s.media.width = seq.value("width").toInt();
            s.media.height = seq.value("height").toInt();
        } else if (!s.media.fileName.isEmpty()) {
            s.media.kind = MediaRef::Kind::Video;
            s.media.path = resolvePath(QString(), s.media.fileName, dir);
        }
    }
    sources.append(s);
    timeline.sourceIds << s.id;
    timeline.fps = s.fps;
    timeline.loopEnabled = o.value("loopEnabled").toBool();
    timeline.loopIn = o.value("loopIn").toInt();
    timeline.loopOut = o.value("loopOut").toInt();
}

bool Project::save(const QString& path, QString* error) const
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(toJson(path)).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool Project::load(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        if (error) *error = L("Invalid project file: %1", "프로젝트 파일 형식이 올바르지 않습니다: %1").arg(pe.errorString());
        return false;
    }
    fromJson(doc.object(), path);
    return true;
}
