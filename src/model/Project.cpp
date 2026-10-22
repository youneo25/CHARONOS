#include "Project.h"
#include "Lang.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

namespace {

constexpr int kMaxTracks = 64;

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

QString TimelineClip::newUid() { return QStringLiteral("c") + QUuid::createUuid().toString(QUuid::Id128).left(10); }

QStringList TimelineDoc::sourceIds() const
{
    QVector<const TimelineClip*> video;
    for (const TimelineClip& c : clips)
        if (c.isVideo()) video << &c;
    std::stable_sort(video.begin(), video.end(), [](const TimelineClip* a, const TimelineClip* b) {
        return a->start != b->start ? a->start < b->start : a->track < b->track;
    });
    QStringList ids;
    for (const TimelineClip* c : video) ids << c->sourceId;
    return ids;
}

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
        if (!s.notes.empty()) {
            QJsonObject notes;
            for (const auto& [frame, text] : s.notes) notes[QString::number(frame)] = text;
            o["notes"] = notes;
        }
        srcs.append(o);
    }
    QJsonObject tl;
    tl["name"] = timeline.name;
    // "sources" and the video entries of "clips" keep builds before 1.1 able to open the file
    // (they play the video clips back to back and ignore tracks, gaps and sound clips).
    tl["sources"] = QJsonArray::fromStringList(timeline.sourceIds());
    QVector<const TimelineClip*> ordered;
    for (const TimelineClip& c : timeline.clips) ordered << &c;
    std::stable_sort(ordered.begin(), ordered.end(), [](const TimelineClip* a, const TimelineClip* b) {
        return a->start != b->start ? a->start < b->start : a->track < b->track;
    });
    QJsonArray clips, audioClips;
    for (const TimelineClip* c : ordered) {
        QJsonObject co;
        co["uid"] = c->uid;
        co["source"] = c->sourceId;
        co["in"] = c->in;
        co["out"] = c->out;
        if (c->sourceStart >= 0) co["sourceStart"] = c->sourceStart;
        if (c->sourceEnd >= 0) co["sourceEnd"] = c->sourceEnd;
        co["track"] = c->track;
        co["start"] = c->start;
        if (!c->enabled) co["enabled"] = false;
        if (!c->link.isEmpty()) co["link"] = c->link;
        if (!c->name.isEmpty()) co["name"] = c->name;
        if (c->color.isValid()) co["color"] = c->color.name(QColor::HexRgb);
        if (c->isVideo()) {
            if (c->opacity != 1.0) co["opacity"] = c->opacity;
            if (c->scale != 1.0) co["scale"] = c->scale;
            if (c->posX != 0.0) co["x"] = c->posX;
            if (c->posY != 0.0) co["y"] = c->posY;
            if (c->blend != BlendMode::Normal) co["blend"] = int(c->blend);
            clips.append(co);
        } else {
            if (c->gainDb != 0.0) co["gain"] = c->gainDb;
            if (c->muted) co["muted"] = true;
            if (c->fadeIn > 0) co["fadeIn"] = c->fadeIn;
            if (c->fadeOut > 0) co["fadeOut"] = c->fadeOut;
            audioClips.append(co);
        }
    }
    tl["clips"] = clips;
    tl["audioClips"] = audioClips;
    auto tracksJson = [](const QVector<TrackState>& tracks) {
        QJsonArray arr;
        for (const TrackState& t : tracks) {
            QJsonObject o;
            if (!t.name.isEmpty()) o["name"] = t.name;
            o["visible"] = t.visible;
            if (t.locked) o["locked"] = true;
            if (t.solo) o["solo"] = true;
            if (t.gainDb != 0.0) o["gain"] = t.gainDb;
            arr.append(o);
        }
        return arr;
    };
    tl["videoTracks"] = tracksJson(timeline.videoTracks);
    tl["audioTracks"] = tracksJson(timeline.audioTracks);
    tl["noteTracks"] = tracksJson(timeline.noteTracks);
    if (timeline.width > 0 && timeline.height > 0) {
        tl["width"] = timeline.width;
        tl["height"] = timeline.height;
    }
    tl["fps"] = timeline.fps;
    tl["loopEnabled"] = timeline.loopEnabled;
    tl["loopIn"] = timeline.loopIn;
    tl["loopOut"] = timeline.loopOut;

    QJsonObject o;
    o["format"] = "FrameReview";
    o["version"] = 7;
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
            const QJsonObject notes = so.value("notes").toObject();
            for (auto it = notes.begin(); it != notes.end(); ++it) {
                bool ok = false;
                const int frame = it.key().toInt(&ok);
                if (ok && frame >= 0 && !it.value().toString().trimmed().isEmpty()) s.notes[frame] = it.value().toString();
            }
            if (!(s.fps > 0 && s.fps <= 1000)) s.fps = 24.0;
            // Two sources with one id would make every lookup ambiguous; keep the first.
            if (find(s.id)) s.id = newSourceId();
            sources.append(s);
        }
        const QJsonArray timelines = o.value("timelines").toArray();
        const QJsonObject tl = timelines.isEmpty() ? QJsonObject() : timelines.first().toObject();
        timeline.name = tl.value("name").toString(timeline.name);
        auto readClip = [&](const QJsonObject& co, TimelineClip::Kind kind) {
            TimelineClip c;
            c.kind = kind;
            c.uid = co.value("uid").toString();
            c.sourceId = co.value("source").toString();
            c.in = std::max(0, co.value("in").toInt());
            c.out = co.value("out").toInt(-1);
            c.sourceStart = co.value("sourceStart").toDouble(-1);
            c.sourceEnd = co.value("sourceEnd").toDouble(-1);
            if (c.out >= 0 && c.out < c.in) c.out = c.in;
            if (c.out < -1) c.out = -1;
            c.track = std::clamp(co.value("track").toInt(0), 0, kMaxTracks - 1);
            c.start = co.contains("start") ? std::max<qint64>(-1, co.value("start").toInteger(-1)) : -1;
            c.enabled = co.value("enabled").toBool(true);
            c.link = co.value("link").toString();
            c.name = co.value("name").toString();
            if (const QColor label = QColor::fromString(co.value("color").toString()); label.isValid()) c.color = label;
            c.opacity = std::clamp(co.value("opacity").toDouble(1.0), 0.0, 1.0);
            c.scale = std::clamp(co.value("scale").toDouble(1.0), 0.01, 20.0);
            c.posX = std::clamp(co.value("x").toDouble(0.0), -10.0, 10.0);
            c.posY = std::clamp(co.value("y").toDouble(0.0), -10.0, 10.0);
            c.blend = BlendMode(std::clamp(co.value("blend").toInt(0), 0, int(BlendMode::Difference)));
            c.gainDb = std::clamp(co.value("gain").toDouble(0.0), -60.0, 24.0);
            c.muted = co.value("muted").toBool();
            c.fadeIn = std::max(0, co.value("fadeIn").toInt());
            c.fadeOut = std::max(0, co.value("fadeOut").toInt());
            if (c.uid.isEmpty() || timeline.indexOf(c.uid) >= 0) c.uid = TimelineClip::newUid();
            if (find(c.sourceId)) timeline.clips << c;
        };
        if (tl.contains("clips")) {
            for (const QJsonValue& v : tl.value("clips").toArray()) readClip(v.toObject(), TimelineClip::Kind::Video);
            for (const QJsonValue& v : tl.value("audioClips").toArray()) readClip(v.toObject(), TimelineClip::Kind::Audio);
        } else {
            for (const QJsonValue& v : tl.value("sources").toArray())
                if (find(v.toString())) timeline.clips << TimelineClip::of(v.toString());
        }
        // Before 1.1 a clip's sound was simply part of it: make the sound clips once the media is open.
        timeline.linkAudioOnOpen = !tl.contains("audioClips");
        // A link must point at an existing partner of the other kind.
        for (TimelineClip& c : timeline.clips)
            if (const TimelineClip* p = timeline.clip(c.link); !p || p->kind == c.kind) c.link.clear();
        auto readTracks = [&](const char* key, QVector<TrackState>& out) {
            const QJsonArray arr = tl.value(QLatin1String(key)).toArray();
            out.clear();
            for (const QJsonValue& v : arr) {
                if (out.size() >= kMaxTracks) break;
                const QJsonObject o = v.toObject();
                TrackState t;
                t.name = o.value("name").toString();
                t.visible = o.value("visible").toBool(true);
                t.locked = o.value("locked").toBool();
                t.solo = o.value("solo").toBool();
                t.gainDb = std::clamp(o.value("gain").toDouble(0.0), -60.0, 24.0);
                out << t;
            }
            if (out.isEmpty()) out << TrackState{};
        };
        readTracks("videoTracks", timeline.videoTracks);
        readTracks("audioTracks", timeline.audioTracks);
        readTracks("noteTracks", timeline.noteTracks);
        for (const TimelineClip& c : timeline.clips)
            while (timeline.tracks(c.kind).size() <= c.track) timeline.tracks(c.kind) << TrackState{};
        timeline.width = std::clamp(tl.value("width").toInt(0), 0, 16384);
        timeline.height = std::clamp(tl.value("height").toInt(0), 0, 16384);
        if (timeline.width <= 0 || timeline.height <= 0) timeline.width = timeline.height = 0;
        timeline.fps = tl.value("fps").toDouble(sources.isEmpty() ? 24.0 : sources.first().fps);
        if (!(timeline.fps > 0 && timeline.fps <= 1000)) timeline.fps = 24.0;
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
    timeline.clips << TimelineClip::of(s.id);
    timeline.linkAudioOnOpen = true;
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
