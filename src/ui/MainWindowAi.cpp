// MainWindow: tools for AI assistants (MCP). Every tool goes through the same functions as
// the menus, so undo, dirty state and the UI stay consistent with what the user does by hand.
#include "MainWindow.h"
#include "ui/TimelineEditor.h"
#include "Lang.h"

#include "export/Exporter.h"
#include "ui/ConsoleDock.h"
#include "ui/MotionGraph.h"
#include "ui/Timeline.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {

QJsonObject annotationSummary(const Annotation& a)
{
    QJsonObject o{ { "id", a.id }, { "type", a.type }, { "color", a.color.name() }, { "size", a.size } };
    if (a.type == "text") o["text"] = a.text, o["x"] = a.x + a.tx, o["y"] = a.y + a.ty, o["font_size"] = a.fontSize * a.scale;
    else if (a.type == "line" || a.type == "arrow")
        o["x1"] = a.x1 + a.tx, o["y1"] = a.y1 + a.ty, o["x2"] = a.x2 + a.tx, o["y2"] = a.y2 + a.ty;
    else if (a.type == "rect" || a.type == "image") o["x"] = a.x + a.tx, o["y"] = a.y + a.ty, o["w"] = a.w, o["h"] = a.h;
    else if (a.type == "circle") o["cx"] = a.cx + a.tx, o["cy"] = a.cy + a.ty, o["rx"] = a.rx, o["ry"] = a.ry;
    else if (a.type == "stroke") o["points"] = int(a.points.size());
    if (!a.trackId.isEmpty()) o["track_id"] = a.trackId;
    if (a.locked) o["locked"] = true;
    return o;
}

QString compareName(CompareMode m)
{
    switch (m) {
    case CompareMode::Wipe: return "wipe";
    case CompareMode::SideBySide: return "side_by_side";
    case CompareMode::TopBottom: return "top_bottom";
    case CompareMode::Blend: return "blend";
    default: return "off";
    }
}

} // namespace

QJsonValue MainWindow::runTool(const QString& name, const QJsonObject& args, QString* error)
{
    auto fail = [&](const QString& message) {
        if (error) *error = message;
        return QJsonValue();
    };
    auto integer = [&](const char* key, qint64 fallback) -> qint64 {
        const QJsonValue v = args.value(QLatin1String(key));
        return v.isDouble() ? qint64(std::llround(v.toDouble())) : fallback;
    };
    auto number = [&](const char* key, double fallback) {
        const QJsonValue v = args.value(QLatin1String(key));
        return v.isDouble() && std::isfinite(v.toDouble()) ? v.toDouble() : fallback;
    };
    auto needMedia = [&]() { return m_seq.total > 0; };
    const QString noMedia = QStringLiteral("No media is loaded. Use open_media or open_project first.");
    auto frameArg = [&]() { return std::clamp<qint64>(integer("frame", m_current), 0, std::max<qint64>(0, m_seq.total - 1)); };
    auto sourceJson = [&](const Source& s) {
        QJsonObject o{ { "id", s.id }, { "name", s.displayName() }, { "path", QDir::toNativeSeparators(s.media.firstFile()) },
                       { "kind", s.media.kind == MediaRef::Kind::Video ? "video" : s.media.kind == MediaRef::Kind::Canvas ? "canvas" : "image_sequence" },
                       { "fps", s.fps }, { "frames", s.frameCount }, { "width", s.media.width }, { "height", s.media.height },
                       { "online", m_media.server(s.id) != nullptr }, { "folder", s.folder } };
        if (FrameServer* srv = m_media.server(s.id)) o["decoder"] = srv->info().decoderName, o["codec"] = srv->info().codec;
        QJsonArray keys, bookmarks, notes, tracks;
        for (const auto& [f, list] : s.annotations) keys.append(QJsonObject{ { "frame", f }, { "count", int(list.size()) } });
        for (int f : s.bookmarkFrames()) bookmarks.append(f);
        for (const auto& [f, text] : s.notes) notes.append(QJsonObject{ { "frame", f }, { "text", text } });
        for (const Track& t : s.tracks)
            tracks.append(QJsonObject{ { "id", t.id }, { "name", t.name }, { "frames", int(t.points.size()) },
                                       { "first", t.points.empty() ? -1 : t.points.begin()->first },
                                       { "last", t.points.empty() ? -1 : t.points.rbegin()->first } });
        o["annotation_keys"] = keys;
        o["bookmarks"] = bookmarks;
        o["notes"] = notes;
        o["tracks"] = tracks;
        return o;
    };
    auto sourceOrCurrent = [&]() -> Source* {
        if (args.contains("source_id")) return m_project.find(args.value("source_id").toString());
        const Loc loc = locate(m_current);
        return loc ? loc.source : nullptr;
    };

    // ------------------------------------------------------------------ state
    if (name == "get_state") {
        QJsonObject o{ { "app", "CHARONOS" }, { "version", QStringLiteral(FRAMEREVIEW_VERSION) } };
        o["project"] = QJsonObject{ { "path", QDir::toNativeSeparators(m_projectPath) }, { "timeline_name", m_project.timeline.name },
                                    { "unsaved_changes", m_dirty }, { "sources", int(m_project.sources.size()) },
                                    { "timeline_clips", m_project.timeline.videoClipCount() } };
        o["view"] = m_soloId.isEmpty() ? QJsonObject{ { "mode", "timeline" } } : QJsonObject{ { "mode", "source" }, { "source_id", m_soloId } };
        o["frame"] = m_current;
        o["frame_count"] = m_seq.total;
        o["fps"] = viewFps();
        o["playing"] = m_playing;
        const auto [in, out] = playRange();
        o["loop"] = QJsonObject{ { "enabled", loopEnabled() }, { "in", in }, { "out", out } };
        if (const Loc loc = locate(m_current)) {
            QJsonObject clip{ { "source_id", loc.source->id }, { "name", loc.source->displayName() }, { "source_frame", loc.rawLocal },
                              { "seconds", loc.server->info().timeOf(loc.local) - loc.server->info().startTime() },
                              { "clip_first_frame", loc.segment->start }, { "clip_length", loc.segment->count } };
            if (!loc.segment->clipId.isEmpty()) clip["clip_uid"] = loc.segment->clipId;
            if (auto it = loc.source->notes.find(int(loc.rawLocal)); it != loc.source->notes.end()) clip["note"] = it->second;
            clip["annotation_key"] = loc.source->keyAt(int(loc.rawLocal));
            clip["bookmarked"] = loc.source->bookmarks.count(int(loc.local)) > 0;
            o["clip"] = clip;
        }
        o["compare"] = QJsonObject{ { "mode", compareName(m_compareMode) }, { "source_id", m_compareMode == CompareMode::Off ? QString() : m_compareId } };
        o["onion"] = QJsonObject{ { "enabled", m_onion }, { "mode", m_onionMode ? "frames" : "keys" }, { "before", m_onionBefore }, { "after", m_onionAfter },
                                  { "video_before", m_onionVideoBefore }, { "video_after", m_onionVideoAfter }, { "step", m_onionStep } };
        o["view_options"] = QJsonObject{ { "flip", m_view->flipHorizontal() }, { "grid", m_view->showGrid() }, { "hud", m_view->showHud() },
                                         { "channel", QStringList{ "rgb", "red", "green", "blue" }.value(int(m_view->colorChannel())) } };
        if (!m_selectedTrackId.isEmpty()) o["selected_track"] = m_selectedTrackId;
        return o;
    }
    if (name == "list_sources") {
        QJsonArray list;
        for (const Source& s : m_project.sources) list.append(sourceJson(s));
        return QJsonObject{ { "sources", list } };
    }

    // ------------------------------------------------------------------ media / project
    if (name == "open_media") {
        QStringList paths;
        for (const QJsonValue& v : args.value("paths").toArray()) {
            const QString p = QDir::fromNativeSeparators(v.toString());
            if (!QFileInfo::exists(p)) return fail(QStringLiteral("Not found: ") + v.toString());
            paths << QFileInfo(p).absoluteFilePath();
        }
        if (paths.isEmpty()) return fail(QStringLiteral("Give at least one path."));
        const int before = m_project.sources.size();
        importPaths(paths);
        QJsonArray added;
        for (int i = before; i < m_project.sources.size(); ++i) added.append(sourceJson(m_project.sources[i]));
        if (added.isEmpty()) return fail(QStringLiteral("Nothing could be imported from those paths."));
        return QJsonObject{ { "imported", added } };
    }
    if (name == "save_project") {
        QString path = args.value("path").toString();
        if (path.isEmpty()) path = m_projectPath;
        if (path.isEmpty()) return fail(QStringLiteral("The project has never been saved; pass `path` (an absolute .frvm path)."));
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".") + Project::kExtension;
        if (!QFileInfo(QFileInfo(path).absolutePath()).isDir()) return fail(QStringLiteral("Folder does not exist: ") + QFileInfo(path).absolutePath());
        if (QFileInfo(path).absoluteFilePath() != QFileInfo(m_projectPath).absoluteFilePath() && QFileInfo::exists(path) &&
            !args.value("overwrite").toBool())
            return fail(QStringLiteral("A file already exists there. Pass overwrite=true to replace it."));
        m_view->commitEdits();
        commitNoteEditor();
        if (m_pending) commitEdit();
        QString err;
        if (!m_project.save(path, &err)) return fail(err);
        m_projectPath = path;
        setDirty(false);
        showToast(L("Saved by AI · %1", "AI가 저장함 · %1").arg(QFileInfo(path).fileName()));
        return QJsonObject{ { "saved", QDir::toNativeSeparators(path) } };
    }
    if (name == "open_project") {
        const QString path = args.value("path").toString();
        if (!QFileInfo::exists(path)) return fail(QStringLiteral("Not found: ") + path);
        if (m_dirty && !args.value("discard_changes").toBool())
            return fail(QStringLiteral("The current project has unsaved changes. Save it first (save_project) or pass discard_changes=true."));
        if (m_dirty) {
            // Unsaved work is only thrown away when the person at the keyboard agrees.
            const auto answer = QMessageBox::question(this, L("AI Assistant", "AI 어시스턴트"),
                L("An AI assistant wants to open\n%1\nand discard the unsaved changes in this project. Allow it?",
                  "AI 어시스턴트가 다음 프로젝트를 열고 현재 프로젝트의 저장하지 않은 변경 사항을 버리려고 합니다.\n%1\n허용할까요?")
                    .arg(QDir::toNativeSeparators(path)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) return fail(QStringLiteral("The user kept the current project. Save it first or ask the user."));
        }
        setDirty(false);
        if (!loadProject(path)) return fail(QStringLiteral("Couldn't open the project."));
        return QJsonObject{ { "opened", QDir::toNativeSeparators(path) }, { "sources", int(m_project.sources.size()) }, { "frames", m_seq.total } };
    }

    // ------------------------------------------------------------------ navigation
    if (name == "go_to_frame") {
        if (!needMedia()) return fail(noMedia);
        if (args.contains("source_id")) {
            const QString id = args.value("source_id").toString();
            if (!m_project.find(id) || !m_media.server(id)) return fail(QStringLiteral("Unknown or offline source: ") + id);
            const qint64 local = std::clamp<qint64>(integer("source_frame", 0), 0, m_media.server(id)->frameCount() - 1);
            qint64 g = m_seq.globalFor(id, local, m_seq.indexAt(m_current));
            if (g < 0) {
                setSolo(id);  // not on the timeline (or trimmed away): view the source itself
                g = m_seq.globalFor(id, local);
            }
            if (m_playing) stopPlayback();
            goToFrame(std::max<qint64>(0, g));
        } else {
            if (m_playing) stopPlayback();
            goToFrame(frameArg());
        }
        const Loc loc = locate(m_current);
        return QJsonObject{ { "frame", m_current }, { "source_id", loc ? loc.source->id : QString() }, { "source_frame", loc ? loc.rawLocal : 0 } };
    }
    if (name == "step") {
        if (!needMedia()) return fail(noMedia);
        step(int(std::clamp<qint64>(integer("frames", 1), -100000, 100000)));
        return QJsonObject{ { "frame", m_current } };
    }
    if (name == "playback") {
        if (!needMedia()) return fail(noMedia);
        const QString action = args.value("action").toString();
        if (action == "play" && !m_playing) togglePlay();
        else if (action == "pause") stopPlayback();
        else if (action == "toggle") togglePlay();
        return QJsonObject{ { "playing", m_playing }, { "frame", m_current } };
    }
    if (name == "set_in_out") {
        if (!needMedia()) return fail(noMedia);
        if (args.value("clear").toBool()) {
            clearLoop();
            return QJsonObject{ { "loop", false } };
        }
        const auto [curIn, curOut] = playRange();
        const qint64 last = m_seq.total - 1;
        const qint64 in = std::clamp<qint64>(integer("in", curIn), 0, last);
        const qint64 out = std::clamp<qint64>(integer("out", curOut), 0, last);
        if (out < in) return fail(QStringLiteral("`out` must be at or after `in`."));
        const bool loop = args.contains("loop") ? args.value("loop").toBool() : true;
        if (m_soloId.isEmpty()) {
            m_project.timeline.loopIn = int(in);
            m_project.timeline.loopOut = int(out);
            m_project.timeline.loopEnabled = loop;
            setDirty(true);
        } else {
            m_soloIn = in;
            m_soloOut = out;
            m_soloLoop = loop;
        }
        updateLoopUi();
        refreshTimelineEditor();
        return QJsonObject{ { "in", in }, { "out", out }, { "loop", loop } };
    }

    // ------------------------------------------------------------------ looking at frames
    if (name == "get_frame_image" || name == "export_frame") {
        if (!needMedia()) return fail(noMedia);
        const qint64 g = frameArg();
        const Loc loc = locate(g);
        if (!loc) return fail(QStringLiteral("No clip at that frame."));
        FrameCache::Ptr frame = loc.server->frame(loc.local);
        if (!frame) {
            // Decode it now and wait (the viewer prefetches around the playhead only).
            loc.server->requestExtra({ loc.local });
            QElapsedTimer clock;
            clock.start();
            QEventLoop wait;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &wait, [&] {
                if (loc.server->isCached(loc.local) || clock.elapsed() > 10000) wait.quit();
            });
            poll.start(15);
            wait.exec(QEventLoop::ExcludeUserInputEvents);
            frame = loc.server->frame(loc.local);
            refreshAnnotations();  // restores the onion-skin extra list
        }
        if (!frame) return fail(QStringLiteral("The frame could not be decoded in time; try again."));
        const QImage picture = frame->toImage();
        if (picture.isNull()) return fail(QStringLiteral("Could not convert the frame."));
        StillFrameOptions options;
        options.includeOutside = false;
        options.padding = 0;
        options.trackRange = trailFrames();
        options.notes = args.contains("annotations") ? args.value("annotations").toBool() : true;
        options.tracks = options.notes;
        if (name == "get_frame_image") {
            const int maxSide = int(std::clamp<qint64>(integer("max_size", 1024), 64, 2048));
            options.scale = std::min(1.0, double(maxSide) / std::max(picture.width(), picture.height()));
        } else {
            options.scale = std::clamp(number("scale", 1.0), 0.1, 4.0);
        }
        const QImage out = composeStillFrame(picture, *loc.source, int(loc.rawLocal), options);
        if (out.isNull()) return fail(QStringLiteral("Could not compose the frame."));
        QJsonObject info{ { "frame", g }, { "source_id", loc.source->id }, { "source", loc.source->displayName() },
                          { "source_frame", loc.rawLocal }, { "width", out.width() }, { "height", out.height() },
                          { "media_width", picture.width() }, { "media_height", picture.height() } };
        if (auto it = loc.source->notes.find(int(loc.rawLocal)); it != loc.source->notes.end()) info["note"] = it->second;
        if (name == "export_frame") {
            QString path = QDir::fromNativeSeparators(args.value("path").toString());
            if (path.isEmpty() || QFileInfo(path).isRelative()) return fail(QStringLiteral("`path` must be an absolute file path."));
            QString suffix = QFileInfo(path).suffix().toLower();
            if (suffix.isEmpty()) suffix = "png", path += ".png";
            if (!QStringList{ "png", "jpg", "jpeg", "tif", "tiff" }.contains(suffix)) return fail(QStringLiteral("Use .png, .jpg or .tif."));
            if (!QFileInfo(QFileInfo(path).absolutePath()).isDir()) return fail(QStringLiteral("Folder does not exist: ") + QFileInfo(path).absolutePath());
            if (QFileInfo::exists(path) && !args.value("overwrite").toBool())
                return fail(QStringLiteral("A file already exists there. Pass overwrite=true to replace it."));
            QImageWriter writer(path);
            if (suffix.startsWith("jp")) writer.setQuality(95);
            if (!writer.write(suffix.startsWith("jp") ? out.convertToFormat(QImage::Format_RGB888) : out)) return fail(writer.errorString());
            info["saved"] = QDir::toNativeSeparators(path);
            return info;
        }
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        out.save(&buffer, "PNG");
        info["image_png_base64"] = QString::fromLatin1(png.toBase64());
        return info;
    }

    // ------------------------------------------------------------------ annotations
    if (name == "list_annotations") {
        Source* s = sourceOrCurrent();
        if (!s) return fail(QStringLiteral("No source."));
        QJsonArray keys;
        for (const auto& [f, list] : s->annotations) {
            QJsonArray items;
            for (const Annotation& a : list) items.append(annotationSummary(a));
            QJsonObject key{ { "source_frame", f }, { "items", items } };
            if (const qint64 g = m_seq.globalFor(s->id, f); g >= 0) key["frame"] = g;
            if (list.isEmpty()) key["cleared"] = true;
            keys.append(key);
        }
        return QJsonObject{ { "source_id", s->id }, { "source", s->displayName() }, { "keys", keys } };
    }
    if (name == "add_annotation") {
        if (!needMedia()) return fail(noMedia);
        static const QStringList types{ "text", "arrow", "line", "rect", "circle" };
        const QString type = args.value("type").toString();
        if (!types.contains(type)) return fail(QStringLiteral("type must be one of text, arrow, line, rect, circle."));
        if (m_playing) stopPlayback();
        goToFrame(frameArg());
        if (m_view->activeSide() == 1) m_view->setActiveSide(0);  // AI notes go on A
        Annotation a;
        a.id = Annotation::newId();
        a.type = type;
        const QColor color(args.value("color").toString());
        a.color = color.isValid() ? color : m_color;
        a.size = std::clamp(number("size", std::max(3.0, m_brushSize)), 0.5, 200.0);
        a.opacity = 1.0;
        auto coord = [&](const char* key, double fallback) { return std::clamp(number(key, fallback), -1.0, 2.0); };
        if (type == "text") {
            a.text = args.value("text").toString().trimmed();
            if (a.text.isEmpty()) return fail(QStringLiteral("text is required for type=text."));
            a.x = coord("x", 0.05);
            a.y = coord("y", 0.05);
            a.fontSize = std::clamp(number("font_size", m_textSize), 6.0, 600.0);
        } else if (type == "arrow" || type == "line") {
            a.x1 = coord("x", 0.4);
            a.y1 = coord("y", 0.4);
            a.x2 = coord("x2", a.x1 + 0.1);
            a.y2 = coord("y2", a.y1 + 0.1);
        } else if (type == "rect") {
            a.x = coord("x", 0.4);
            a.y = coord("y", 0.4);
            a.w = std::clamp(number("w", 0.2), 0.001, 2.0);
            a.h = std::clamp(number("h", 0.2), 0.001, 2.0);
        } else {
            a.cx = coord("x", 0.5);
            a.cy = coord("y", 0.5);
            a.rx = std::clamp(number("w", 0.1), 0.001, 2.0);
            a.ry = std::clamp(number("h", a.rx), 0.001, 2.0);
        }
        onAnnotationCreated(a);
        const Loc loc = locate(m_current);
        return QJsonObject{ { "id", a.id }, { "frame", m_current }, { "source_frame", loc ? loc.rawLocal : 0 } };
    }
    if (name == "clear_frame") {
        if (!needMedia()) return fail(noMedia);
        goToFrame(frameArg());
        clearCurrentFrame();
        return QJsonObject{ { "cleared", m_current } };
    }
    if (name == "bookmark") {
        const QString action = args.value("action").toString();
        if (action == "list") {
            QJsonArray list;
            for (const Sequence::Segment& s : m_seq.segments)
                if (const Source* src = m_project.find(s.sourceId))
                    for (int f : src->bookmarkFrames())
                        if (const qint64 g = Sequence::globalIn(s, f); g >= 0)
                            list.append(QJsonObject{ { "frame", g }, { "source_id", src->id }, { "source_frame", f } });
            return QJsonObject{ { "bookmarks", list } };
        }
        if (!needMedia()) return fail(noMedia);
        goToFrame(frameArg());
        const Loc loc = locate(m_current);
        if (!loc) return fail(QStringLiteral("No clip at that frame."));
        const bool marked = loc.source->bookmarks.count(int(loc.local)) > 0;
        if ((action == "add" && !marked) || (action == "remove" && marked) || action == "toggle") toggleBookmark();
        return QJsonObject{ { "frame", m_current }, { "bookmarked", loc.source->bookmarks.count(int(loc.local)) > 0 } };
    }
    if (name == "notes") {
        const QString action = args.value("action").toString();
        if (action == "list") {
            QJsonArray list;
            for (const Source& s : m_project.sources) {
                if (args.contains("source_id") && s.id != args.value("source_id").toString()) continue;
                for (const auto& [f, text] : s.notes) {
                    QJsonObject n{ { "source_id", s.id }, { "source", s.displayName() }, { "source_frame", f }, { "text", text } };
                    if (const qint64 g = m_seq.globalFor(s.id, f); g >= 0) n["frame"] = g;
                    list.append(n);
                }
            }
            return QJsonObject{ { "notes", list } };
        }
        if (!needMedia()) return fail(noMedia);
        goToFrame(frameArg());
        const Loc loc = locate(m_current);
        if (!loc) return fail(QStringLiteral("No clip at that frame."));
        const QString text = action == "set" ? args.value("text").toString() : QString();
        if (action == "set" && text.trimmed().isEmpty()) return fail(QStringLiteral("text is required for action=set."));
        setNote(loc.source->id, int(loc.rawLocal), text);
        syncNoteEditor();
        return QJsonObject{ { "frame", m_current }, { "source_frame", loc.rawLocal }, { "text", text.trimmed() } };
    }

    // ------------------------------------------------------------------ tracking / analysis
    if (name == "track_point") {
        if (!needMedia()) return fail(noMedia);
        if (m_trackJob) return fail(QStringLiteral("Tracking is already running; wait for it (list_tracks shows progress)."));
        const double x = number("x", -1), y = number("y", -1);
        if (x < 0 || x > 1 || y < 0 || y > 1) return fail(QStringLiteral("x and y must be 0..1."));
        if (m_playing) stopPlayback();
        goToFrame(frameArg());
        if (m_view->activeSide() == 1) m_view->setActiveSide(0);
        placeTrack(QPointF(x, y));
        const QString id = m_selectedTrackId;
        if (const EditTarget t = editTarget(); t && args.contains("name"))
            if (Track* tr = t.source->track(id)) {
                beginTrackEdit(*t.source, id);
                tr->name = args.value("name").toString().left(80);
                commitEdit();
            }
        const QString direction = args.value("direction").toString("forward");
        if (direction == "forward") startTracking(1);
        else if (direction == "backward") startTracking(-1);
        return QJsonObject{ { "track_id", id }, { "tracking", bool(m_trackJob) }, { "direction", direction },
                            { "hint", "Poll list_tracks until tracking is false, then call motion_analysis." } };
    }
    if (name == "list_tracks") {
        Source* s = sourceOrCurrent();
        if (!s) return fail(QStringLiteral("No source."));
        QJsonArray list;
        for (const Track& t : s->tracks)
            list.append(QJsonObject{ { "id", t.id }, { "name", t.name }, { "color", t.color.name() }, { "frames", int(t.points.size()) },
                                     { "first", t.points.empty() ? -1 : t.points.begin()->first },
                                     { "last", t.points.empty() ? -1 : t.points.rbegin()->first }, { "manual_keys", int(t.keys.size()) } });
        QJsonObject o{ { "source_id", s->id }, { "tracks", list }, { "tracking", bool(m_trackJob) } };
        if (m_trackJob) o["progress"] = QJsonObject{ { "done", m_trackJob->processed }, { "total", m_trackJob->total } };
        return o;
    }
    if (name == "motion_analysis") {
        const EditTarget t = editTarget();
        if (!t) return fail(noMedia);
        const QString wanted = args.value("track_id").toString(m_selectedTrackId);
        const Track* track = t.source->track(wanted);
        if (!track && !t.source->tracks.isEmpty()) track = &t.source->tracks.front();
        if (!track) return fail(QStringLiteral("This clip has no tracks. Use track_point first."));
        const MotionGraph::Analysis a = MotionGraph::analyze(track->points, t.size);
        const double fps = std::max(1.0, t.source->fps);
        QJsonArray keys, fastest;
        for (int k : a.keys) keys.append(k);
        for (int k : a.fastest) fastest.append(k);
        QJsonObject o{ { "source_id", t.source->id }, { "track_id", track->id }, { "track", track->name }, { "fps", fps },
                       { "first_frame", a.samples.isEmpty() ? -1 : a.samples.front().frame },
                       { "last_frame", a.samples.isEmpty() ? -1 : a.samples.back().frame },
                       { "path_length_px", a.distance }, { "mean_speed_px_per_frame", a.meanSpeed }, { "max_speed_px_per_frame", a.maxSpeed },
                       { "suggested_keys", keys }, { "speed_peaks", fastest },
                       { "note", "Frames are source frames. suggested_keys = stops and direction changes (extremes / contacts); "
                                 "speed_peaks = fastest moments (breakdowns / passing positions)." } };
        if (args.value("include_samples").toBool()) {
            QJsonArray samples;
            for (const auto& s : a.samples)
                samples.append(QJsonObject{ { "frame", s.frame }, { "x", std::round(s.pos.x() * 10) / 10 }, { "y", std::round(s.pos.y() * 10) / 10 },
                                            { "speed", s.hasSpeed ? QJsonValue(std::round(s.speed * 100) / 100) : QJsonValue() } });
            o["samples"] = samples;
        }
        if (args.value("bookmark_keys").toBool()) {
            for (int f : a.keys)
                if (!t.source->bookmarks.count(f)) t.source->bookmarks[f] = {};
            setDirty(true);
            refreshMarks();
            o["bookmarked"] = int(a.keys.size());
        }
        return o;
    }

    // ------------------------------------------------------------------ view settings
    if (name == "set_compare") {
        static const QMap<QString, CompareMode> modes{ { "off", CompareMode::Off }, { "wipe", CompareMode::Wipe }, { "side_by_side", CompareMode::SideBySide },
                                                       { "top_bottom", CompareMode::TopBottom }, { "blend", CompareMode::Blend } };
        const QString mode = args.value("mode").toString();
        if (!modes.contains(mode)) return fail(QStringLiteral("Unknown mode."));
        if (args.contains("source_id")) {
            const QString id = args.value("source_id").toString();
            if (!m_media.server(id)) return fail(QStringLiteral("Unknown or offline source: ") + id);
            m_compareId = id;
        }
        setCompareMode(modes.value(mode));
        return QJsonObject{ { "mode", compareName(m_compareMode) }, { "source_id", m_compareId } };
    }
    if (name == "set_view") {
        if (args.contains("flip")) setFlip(args.value("flip").toBool());
        if (args.contains("grid")) {
            m_gridAction->setChecked(args.value("grid").toBool());
            m_view->setShowGrid(m_gridAction->isChecked());
        }
        if (args.contains("hud")) {
            m_hudAction->setChecked(args.value("hud").toBool());
            m_view->setShowHud(m_hudAction->isChecked());
            updateHud();
        }
        if (args.contains("annotations")) m_view->setShowAnnotations(args.value("annotations").toBool());
        if (args.contains("channel")) {
            const int c = QStringList{ "rgb", "red", "green", "blue" }.indexOf(args.value("channel").toString());
            if (c < 0) return fail(QStringLiteral("channel must be rgb, red, green or blue."));
            m_view->setColorChannel(ColorChannel(c));
        }
        if (args.value("fit").toBool()) m_view->fitToView();
        return runTool("get_state", {}, error).toObject().value("view_options");
    }
    if (name == "set_onion") {
        if (args.contains("mode")) m_onionMode = args.value("mode").toString() == "frames" ? 1 : 0;
        m_onionBefore = int(std::clamp<qint64>(integer("before", m_onionBefore), 0, 8));
        m_onionAfter = int(std::clamp<qint64>(integer("after", m_onionAfter), 0, 8));
        m_onionVideoBefore = int(std::clamp<qint64>(integer("video_before", m_onionVideoBefore), 0, 8));
        m_onionVideoAfter = int(std::clamp<qint64>(integer("video_after", m_onionVideoAfter), 0, 8));
        m_onionStep = int(std::clamp<qint64>(integer("step", m_onionStep), 1, 12));
        if (args.contains("tint")) m_onionTint = args.value("tint").toBool();
        m_onionOpacity = std::clamp(number("opacity", m_onionOpacity), 0.05, 0.9);
        populateOnionMenu();  // the settings popup shows the new values
        QSettings s;
        s.setValue("onion/mode", m_onionMode);
        s.setValue("onion/before", m_onionBefore);
        s.setValue("onion/after", m_onionAfter);
        s.setValue("onion/videoBefore", m_onionVideoBefore);
        s.setValue("onion/videoAfter", m_onionVideoAfter);
        s.setValue("onion/step", m_onionStep);
        s.setValue("onion/tint", m_onionTint);
        s.setValue("onion/opacity", m_onionOpacity);
        if (args.contains("enabled") && args.value("enabled").toBool() != m_onion) setOnion(args.value("enabled").toBool());
        else refreshAnnotations();
        return runTool("get_state", {}, error).toObject().value("onion");
    }

    // ------------------------------------------------------------------ timeline
    if (name == "timeline") {
        const QString action = args.value("action").toString();
        const QString uid = args.value("clip_uid").toString();
        const tl::LengthFn len = lengthFn();
        const bool ripple = args.contains("ripple") ? args.value("ripple").toBool() : (m_editor && m_editor->ripple());
        const bool needsClip = action != "list" && action != "add" && action != "close_gap" && action != "add_track";
        if (needsClip && !m_project.timeline.clip(uid)) return fail(QStringLiteral("Unknown clip_uid; use action=list."));
        const TimelineClip::Kind kind = args.value("kind").toString() == "audio" ? TimelineClip::Kind::Audio : TimelineClip::Kind::Video;
        if (action == "add") {
            const QString id = args.value("source_id").toString();
            if (!m_project.find(id) || !m_media.server(id)) return fail(QStringLiteral("Unknown or offline source_id."));
            const int64_t frame = integer("frame", tl::duration(m_project.timeline, len));
            const int track = int(std::clamp<int64_t>(integer("track", 0), 0, 63));
            if (m_project.timeline.videoClipCount() == 0) m_project.timeline.fps = m_project.find(id)->fps;
            editTimeline([&](TimelineDoc& doc) { return tl::insertClips(doc, clipsFor({ id }), frame, track, track, ripple, len); },
                         QStringLiteral("AI: add clip"));
        } else if (action == "remove") {
            editTimeline([&](TimelineDoc& doc) { return tl::remove(doc, tl::withLinks(doc, { uid }), ripple, len); }, QStringLiteral("AI: remove clip"));
        } else if (action == "split") {
            const tl::Span s = tl::span(*m_project.timeline.clip(uid), len);
            const qint64 frame = integer("frame", m_soloId.isEmpty() ? m_current : -1);
            if (frame <= s.start || frame >= s.end())
                return fail(QStringLiteral("`frame` must be inside the clip (timeline frame after its first frame)."));
            splitClip(uid, frame);
        } else if (action == "trim") {
            const TimelineClip& c = *m_project.timeline.clip(uid);
            const bool keep = m_editor && m_editor->ripple();
            if (m_editor) m_editor->setRipple(ripple);
            trimClip(uid, int(integer("in", c.in)), int(integer("out", c.out)));
            if (m_editor) m_editor->setRipple(keep);
        } else if (action == "move") {
            moveClip(uid, int(integer("index", m_project.timeline.videoClipCount())));
        } else if (action == "place") {
            const TimelineClip& c = *m_project.timeline.clip(uid);
            const tl::Span s = tl::span(c, len);
            const int64_t frame = std::max<int64_t>(0, integer("frame", s.start));
            const int track = int(std::clamp<int64_t>(integer("track", c.track), 0, 63));
            const int dv = c.isVideo() ? track - c.track : 0, da = c.isAudio() ? track - c.track : 0;
            editTimeline([&](TimelineDoc& doc) { return tl::moveOverwrite(doc, tl::withLinks(doc, { uid }), frame - s.start, dv, da, false, len); },
                         QStringLiteral("AI: place clip"));
        } else if (action == "set") {
            static const QStringList blends{ "normal", "add", "screen", "multiply", "difference" };
            if (args.contains("blend") && !blends.contains(args.value("blend").toString())) return fail(QStringLiteral("Unknown blend."));
            editTimeline(
                [&](TimelineDoc& doc) {
                    TimelineClip* c = doc.clip(uid);
                    if (args.contains("enabled")) c->enabled = args.value("enabled").toBool();
                    if (c->isVideo()) {
                        if (args.contains("opacity")) c->opacity = std::clamp(args.value("opacity").toDouble(), 0.0, 1.0);
                        if (args.contains("scale")) c->scale = std::clamp(args.value("scale").toDouble(), 0.01, 20.0);
                        if (args.contains("x")) c->posX = std::clamp(args.value("x").toDouble(), -10.0, 10.0);
                        if (args.contains("y")) c->posY = std::clamp(args.value("y").toDouble(), -10.0, 10.0);
                        if (args.contains("blend")) c->blend = BlendMode(blends.indexOf(args.value("blend").toString()));
                    } else {
                        if (args.contains("gain_db")) c->gainDb = std::clamp(args.value("gain_db").toDouble(), -60.0, 24.0);
                        if (args.contains("muted")) c->muted = args.value("muted").toBool();
                    }
                    return true;
                },
                QStringLiteral("AI: clip settings"));
        } else if (action == "close_gap") {
            const int track = int(std::clamp<int64_t>(integer("track", 0), 0, 63));
            const int64_t frame = integer("frame", m_current);
            if (!editTimeline([&](TimelineDoc& doc) { return tl::closeGap(doc, kind, track, frame, len); }, QStringLiteral("AI: close gap")))
                return fail(QStringLiteral("No gap at that frame on that track."));
        } else if (action == "add_track") {
            editTimeline(
                [&](TimelineDoc& doc) {
                    doc.tracks(kind) << TrackState{};
                    return true;
                },
                QStringLiteral("AI: add track"));
        } else if (action != "list") {
            return fail(QStringLiteral("Unknown action."));
        }
        static const char* blendNames[] = { "normal", "add", "screen", "multiply", "difference" };
        QJsonArray clips;
        for (const TimelineClip& c : m_project.timeline.clips) {
            const Source* s = m_project.find(c.sourceId);
            const tl::Span sp = tl::span(c, len);
            QJsonObject o{ { "clip_uid", c.uid }, { "source_id", c.sourceId }, { "name", c.name.isEmpty() && s ? s->displayName() : c.name },
                           { "kind", c.isVideo() ? "video" : "audio" }, { "track", c.track },
                           { "track_name", tl::trackName(m_project.timeline, c.kind, c.track) },
                           { "start", sp.start }, { "length", sp.count }, { "in", sp.in }, { "out", sp.last() },
                           { "enabled", c.enabled } };
            if (!c.link.isEmpty()) o["linked_to"] = c.link;
            if (!m_media.server(c.sourceId)) o["offline"] = true;
            if (c.isVideo()) {
                o["opacity"] = c.opacity;
                o["scale"] = c.scale;
                o["x"] = c.posX;
                o["y"] = c.posY;
                o["blend"] = blendNames[int(c.blend)];
            } else {
                o["gain_db"] = c.gainDb;
                o["muted"] = c.muted;
            }
            clips.append(o);
        }
        auto tracks = [](const QVector<TrackState>& list, const char* prefix) {
            QJsonArray arr;
            for (int i = 0; i < list.size(); ++i)
                arr.append(QJsonObject{ { "name", list[i].name.isEmpty() ? QStringLiteral("%1%2").arg(QLatin1String(prefix)).arg(i + 1) : list[i].name },
                                        { "visible", list[i].visible }, { "locked", list[i].locked }, { "solo", list[i].solo }, { "gain_db", list[i].gainDb } });
            return arr;
        };
        const QSize canvas = canvasSize();
        return QJsonObject{ { "clips", clips }, { "total_frames", tl::duration(m_project.timeline, len) }, { "fps", m_project.timeline.fps },
                            { "canvas", QJsonObject{ { "width", canvas.width() }, { "height", canvas.height() } } },
                            { "video_tracks", tracks(m_project.timeline.videoTracks, "V") }, { "audio_tracks", tracks(m_project.timeline.audioTracks, "A") },
                            { "annotation_lanes", tracks(m_project.timeline.noteTracks, "N") },
                            { "ripple", m_editor && m_editor->ripple() } };
    }

    // ------------------------------------------------------------------ annotation clips
    if (name == "annotation_clips") {
        const Source* src = sourceOrCurrent();
        if (!src) return fail(QStringLiteral("Unknown source_id (or no clip under the playhead)."));
        const QString id = src->id;
        const QString action = args.value("action").toString();
        const QString key = args.value("key").toString();
        const int length = int(std::max<int64_t>(sourceLength(id), src->frameCount));
        const QVector<NoteClip> before = notes::clips(*src, length);
        const auto found = std::find_if(before.begin(), before.end(), [&](const NoteClip& n) { return n.key == key; });
        if (action != "list" && found == before.end()) return fail(QStringLiteral("Unknown key; use action=list."));
        bool ok = true;
        if (action == "move") {
            ok = editNoteClips(id, [&](Source& s, int n) { return notes::move(s, n, key, int(integer("first", found->first)), int(integer("lane", found->lane))); },
                               QStringLiteral("AI: move annotation"));
        } else if (action == "trim") {
            ok = editNoteClips(id, [&](Source& s, int n) { return notes::trim(s, n, key, int(integer("first", found->first)), int(integer("last", found->last))); },
                               QStringLiteral("AI: trim annotation"));
        } else if (action == "split") {
            ok = editNoteClips(id, [&](Source& s, int n) { return notes::split(s, n, key, int(integer("frame", -1))); }, QStringLiteral("AI: split annotation"));
        } else if (action == "delete") {
            ok = editNoteClips(id, [&](Source& s, int n) { return notes::erase(s, n, { key }); }, QStringLiteral("AI: delete annotation"));
        } else if (action == "duplicate") {
            ok = editNoteClips(id, [&](Source& s, int n) { return notes::duplicate(s, n, key, int(integer("first", found->last + 1))); },
                               QStringLiteral("AI: duplicate annotation"));
        } else if (action == "color") {
            const QColor c = QColor::fromString(args.value("color").toString());
            ok = editNoteClips(id, [&](Source& s, int n) { return notes::setColor(s, n, key, c, false); }, QStringLiteral("AI: annotation colour"));
        } else if (action != "list") {
            return fail(QStringLiteral("Unknown action."));
        }
        if (!ok) return fail(QStringLiteral("Nothing changed (check the frames: split needs a frame inside the clip, after its first frame)."));
        QJsonArray list;
        for (const NoteClip& n : notes::clips(*m_project.find(id), length))
            list.append(QJsonObject{ { "key", n.key }, { "kind", n.kind }, { "label", n.label }, { "first", n.first }, { "last", n.last },
                                     { "lane", n.lane }, { "color", n.color.name() }, { "objects", int(n.ids.size()) }, { "tracked", n.tracked },
                                     { "locked", n.locked } });
        return QJsonObject{ { "source_id", id }, { "clips", list } };
    }

    // ------------------------------------------------------------------ filters
    if (name == "set_filter") {
        static const QStringList modes{ "none", "silhouette", "moving_silhouette", "edges", "value", "notan", "motion_highlight" };
        const int mode = int(modes.indexOf(args.value("mode").toString()));
        if (mode < 0) return fail(QStringLiteral("Unknown mode."));
        m_filter.mode = AnalysisFilter::Mode(mode);
        if (args.contains("threshold")) {
            m_filter.setThreshold(std::clamp(args.value("threshold").toDouble(), 0.0, 1.0));
            m_filter.autoThreshold = false;
        }
        if (args.contains("auto")) m_filter.autoThreshold = args.value("auto").toBool();
        if (args.contains("invert")) m_filter.invert = args.value("invert").toBool();
        if (args.contains("show_original")) m_filter.showOriginal = std::clamp(args.value("show_original").toDouble(), 0.0, 1.0);
        if (args.contains("levels")) m_filter.levels = int(std::clamp<int64_t>(integer("levels", 2), 2, 3));
        setFilterMode(m_filter.mode);
        return QJsonObject{ { "mode", modes[mode] }, { "threshold", effectiveThreshold() }, { "auto", m_filter.usesAutoThreshold() },
                            { "description", AnalysisFilter::description(m_filter.mode) } };
    }

    // ------------------------------------------------------------------ console
    if (name == "console") {
        const QString command = args.value("command").toString().trimmed();
        if (command.isEmpty()) return fail(QStringLiteral("command is required."));
        auto* log = m_console->findChild<QPlainTextEdit*>("ConsoleLog");
        const int before = log ? log->blockCount() : 0;
        runConsoleCommand(command);
        QString output;
        if (log) {
            const QStringList lines = log->toPlainText().split('\n');
            output = lines.mid(std::max(0, before - 1)).join('\n').trimmed();
        }
        return QJsonObject{ { "command", command }, { "output", output } };
    }
    return fail(QStringLiteral("Unknown tool: ") + name);
}
