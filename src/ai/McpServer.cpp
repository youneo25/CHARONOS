#include "McpServer.h"

#include "Lang.h"
#include "ui/MainWindow.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>

namespace {

// Requests larger than this are refused (a frame image is sent the other way, not received).
constexpr qsizetype kMaxMessage = 8 << 20;

QJsonObject prop(const char* type, const QString& description)
{
    return QJsonObject{ { "type", type }, { "description", description } };
}

QJsonObject enumProp(const QStringList& values, const QString& description)
{
    return QJsonObject{ { "type", "string" }, { "enum", QJsonArray::fromStringList(values) }, { "description", description } };
}

QJsonObject tool(const char* name, const QString& description, QJsonObject properties = {}, QStringList required = {},
                 bool readOnly = false)
{
    QJsonObject schema{ { "type", "object" }, { "properties", properties } };
    if (!required.isEmpty()) schema["required"] = QJsonArray::fromStringList(required);
    QJsonObject t{ { "name", name }, { "description", description }, { "inputSchema", schema } };
    t["annotations"] = QJsonObject{ { "readOnlyHint", readOnly } };
    return t;
}

QJsonObject rpcResult(const QJsonValue& id, const QJsonObject& result)
{
    return QJsonObject{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
}

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message)
{
    return QJsonObject{ { "jsonrpc", "2.0" }, { "id", id }, { "error", QJsonObject{ { "code", code }, { "message", message } } } };
}

} // namespace

McpServer::McpServer(MainWindow* window)
    : QObject(window)
    , m_window(window)
{
}

McpServer::~McpServer() { stop(); }

QString McpServer::pipeName()
{
    // One pipe per Windows user; the socket ACL below keeps other users out as well.
    QString user = qEnvironmentVariable("USERNAME", QStringLiteral("user")).toLower();
    user.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]")), QStringLiteral("_"));
    return QStringLiteral("charonos-mcp-") + user;
}

QString McpServer::bridgePath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + QStringLiteral("/charonos-mcp.exe"));
}

QString McpServer::claudeCodeCommand()
{
    return QStringLiteral("claude mcp add charonos --scope user -- \"%1\"").arg(bridgePath());
}

QString McpServer::configJson()
{
    const QJsonObject server{ { "command", QDir::fromNativeSeparators(bridgePath()) }, { "args", QJsonArray() } };
    const QJsonObject root{ { "mcpServers", QJsonObject{ { "charonos", server } } } };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString McpServer::aboutHtml(bool listening)
{
    return L("<p><b>AI assistants can see and drive CHARONOS through MCP.</b></p>"
             "<p>Status: %1</p>"
             "<p><b>Claude Code</b> — run once in a terminal:<br><code>%2</code></p>"
             "<p><b>Claude Desktop / other clients</b> — Tools &gt; AI Assistant &gt; Copy MCP Config, then paste it into the "
             "client's MCP settings.</p>"
             "<p>The assistant can read the project, look at frames, add notes and annotations, bookmark key poses, "
             "track points and read motion analysis. Only this Windows user can connect; turn it off with "
             "<i>Allow AI Assistants to Connect</i>.</p>",
             "<p><b>AI 어시스턴트가 MCP로 CHARONOS를 보고 조작할 수 있습니다.</b></p>"
             "<p>상태: %1</p>"
             "<p><b>Claude Code</b> — 터미널에서 한 번 실행:<br><code>%2</code></p>"
             "<p><b>Claude Desktop / 기타 클라이언트</b> — 도구 &gt; AI 어시스턴트 &gt; MCP 설정 복사 후 클라이언트의 MCP 설정에 붙여 넣으세요.</p>"
             "<p>프로젝트 읽기, 프레임 이미지 보기, 메모·주석 추가, 키 포즈 북마크, 점 추적과 모션 분석을 할 수 있습니다. "
             "현재 Windows 사용자만 연결할 수 있으며 <i>AI 어시스턴트 연결 허용</i>으로 끌 수 있습니다.</p>")
        .arg(listening ? L("listening", "대기 중") : L("off", "꺼짐"), claudeCodeCommand().toHtmlEscaped());
}

bool McpServer::start()
{
    if (isListening()) return true;
    if (!m_server) {
        m_server = new QLocalServer(this);
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        m_server->setMaxPendingConnections(8);
        connect(m_server, &QLocalServer::newConnection, this, &McpServer::onNewConnection);
    }
    if (!m_server->listen(pipeName())) {
        // Another CHARONOS window already serves MCP for this user.
        qInfo().noquote() << "MCP: not listening:" << m_server->errorString();
        return false;
    }
    qInfo().noquote() << "MCP: listening on" << m_server->fullServerName();
    return true;
}

void McpServer::stop()
{
    for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it) {
        it.key()->disconnect(this);
        it.key()->abort();
        it.key()->deleteLater();
    }
    m_buffers.clear();
    if (m_server) m_server->close();
}

bool McpServer::isListening() const { return m_server && m_server->isListening(); }

void McpServer::onNewConnection()
{
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
        qInfo().noquote() << "MCP: client connected";
    }
}

void McpServer::onReadyRead(QLocalSocket* socket)
{
    auto it = m_buffers.find(socket);
    if (it == m_buffers.end()) return;
    it->append(socket->readAll());
    if (it->size() > kMaxMessage) {
        qWarning().noquote() << "MCP: message too large, closing the connection";
        socket->abort();
        return;
    }
    for (;;) {
        it = m_buffers.find(socket);  // a tool may have run an event loop meanwhile
        if (it == m_buffers.end()) return;
        const qsizetype nl = it->indexOf('\n');
        if (nl < 0) return;
        const QByteArray line = it->left(nl).trimmed();
        it->remove(0, nl + 1);
        if (line.isEmpty()) continue;

        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        QJsonObject response;
        if (!doc.isObject()) response = rpcError(QJsonValue::Null, -32700, QStringLiteral("Parse error: ") + pe.errorString());
        else response = handle(doc.object());
        if (response.isEmpty()) continue;
        if (!m_buffers.contains(socket)) return;  // client went away during the call
        socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        socket->write("\n");
        socket->flush();
    }
}

QJsonObject McpServer::handle(const QJsonObject& msg)
{
    const QString method = msg.value("method").toString();
    const bool isRequest = msg.contains("id");
    const QJsonValue id = msg.value("id");
    if (msg.value("jsonrpc").toString() != QLatin1String("2.0") || method.isEmpty())
        return isRequest ? rpcError(id, -32600, QStringLiteral("Invalid request")) : QJsonObject();
    const QJsonObject params = msg.value("params").toObject();

    if (method == QLatin1String("initialize")) {
        static const QStringList supported{ "2025-06-18", "2025-03-26", "2024-11-05" };
        const QString asked = params.value("protocolVersion").toString();
        const QString version = supported.contains(asked) ? asked : QString::fromLatin1(kLatestProtocol);
        QJsonObject result{
            { "protocolVersion", version },
            { "capabilities", QJsonObject{ { "tools", QJsonObject{ { "listChanged", false } } } } },
            { "serverInfo", QJsonObject{ { "name", "charonos" }, { "title", "CHARONOS" }, { "version", QStringLiteral(FRAMEREVIEW_VERSION) } } },
            { "instructions",
              "CHARONOS is a frame-accurate reference viewer for animators. Frames are 0-based. `frame` arguments are "
              "positions on what the viewer is playing (the timeline, or one source when a single clip is viewed); "
              "`source_frame` is the frame inside a source file. Coordinates (x, y, w, h) are normalized 0..1 of the "
              "picture, origin top-left. Call get_state first, use get_frame_image to look at a frame, and "
              "motion_analysis after track_point to study spacing and timing." },
        };
        return rpcResult(id, result);
    }
    if (method == QLatin1String("ping")) return isRequest ? rpcResult(id, {}) : QJsonObject();
    if (method.startsWith(QLatin1String("notifications/"))) return {};
    if (method == QLatin1String("tools/list")) return rpcResult(id, QJsonObject{ { "tools", toolDefinitions() } });
    if (method == QLatin1String("tools/call")) {
        if (!params.value("name").isString()) return rpcError(id, -32602, QStringLiteral("Missing tool name"));
        return rpcResult(id, callTool(params));
    }
    if (method == QLatin1String("resources/list")) return rpcResult(id, QJsonObject{ { "resources", QJsonArray() } });
    if (method == QLatin1String("prompts/list")) return rpcResult(id, QJsonObject{ { "prompts", QJsonArray() } });
    return isRequest ? rpcError(id, -32601, QStringLiteral("Method not found: ") + method) : QJsonObject();
}

QJsonObject McpServer::callTool(const QJsonObject& params)
{
    const QString name = params.value("name").toString();
    const QJsonObject args = params.value("arguments").toObject();
    QString error;
    QJsonValue value;
    bool known = false;
    for (const QJsonValue& t : toolDefinitions())
        if (t.toObject().value("name").toString() == name) known = true;
    if (!known) error = QStringLiteral("Unknown tool: ") + name;
    else value = m_window->runTool(name, args, &error);

    QJsonArray content;
    if (!error.isEmpty()) {
        content.append(QJsonObject{ { "type", "text" }, { "text", error } });
        return QJsonObject{ { "content", content }, { "isError", true } };
    }
    QJsonObject obj = value.toObject();
    if (obj.contains("image_png_base64")) {
        content.append(QJsonObject{ { "type", "image" }, { "data", obj.take("image_png_base64").toString() }, { "mimeType", "image/png" } });
    }
    const QJsonDocument doc = value.isObject() ? QJsonDocument(obj) : value.isArray() ? QJsonDocument(value.toArray())
                                                                                       : QJsonDocument(QJsonObject{ { "result", value } });
    content.append(QJsonObject{ { "type", "text" }, { "text", QString::fromUtf8(doc.toJson(QJsonDocument::Indented)) } });
    QJsonObject result{ { "content", content } };
    if (value.isObject()) result["structuredContent"] = obj;
    return result;
}

QJsonArray McpServer::toolDefinitions()
{
    static const QJsonArray tools = [] {
        const QJsonObject frame = prop("integer", "Frame on what the viewer plays (0-based). Default: the current frame.");
        QJsonArray t;
        t.append(tool("get_state", "Project, current frame / clip / time, playback, compare, onion skin and view settings.", {}, {}, true));
        t.append(tool("list_sources", "Every source in the project with size, fps, frame count, decoder, annotation keys, bookmarks, notes and tracks.", {}, {}, true));
        t.append(tool("open_media", "Import video files, image sequences or folders and add them to the timeline.",
                      { { "paths", QJsonObject{ { "type", "array" }, { "items", QJsonObject{ { "type", "string" } } }, { "description", "Absolute file or folder paths" } } } },
                      { "paths" }));
        t.append(tool("go_to_frame", "Move the playhead. Give `frame`, or `source_id` + `source_frame` to land on a frame of one source.",
                      { { "frame", prop("integer", "Frame on the timeline / viewed clip") },
                        { "source_id", prop("string", "Source id from list_sources") },
                        { "source_frame", prop("integer", "Frame inside that source") } }));
        t.append(tool("step", "Step the playhead by a number of frames (negative = backwards).", { { "frames", prop("integer", "Frames to move") } }, { "frames" }));
        t.append(tool("playback", "Play, pause or toggle playback.", { { "action", enumProp({ "play", "pause", "toggle" }, "What to do") } }, { "action" }));
        t.append(tool("set_in_out", "Set the In / Out loop range, or clear it.",
                      { { "in", prop("integer", "In frame") }, { "out", prop("integer", "Out frame") },
                        { "loop", prop("boolean", "Loop playback inside the range (default true)") }, { "clear", prop("boolean", "Remove the range") } }));
        t.append(tool("get_frame_image", "Look at a frame: returns a PNG of the picture with annotations and tracking trails burned in.",
                      { { "frame", frame }, { "max_size", prop("integer", "Longest side in pixels (64-2048, default 1024)") },
                        { "annotations", prop("boolean", "Draw annotations and tracks (default true)") } }, {}, true));
        t.append(tool("list_annotations", "Annotation keys of a source: frame and the drawn objects (type, colour, text, position).",
                      { { "source_id", prop("string", "Default: the clip under the playhead") } }, {}, true));
        t.append(tool("add_annotation", "Draw on a frame (creates an annotation key there). Coordinates are normalized 0..1.",
                      { { "type", enumProp({ "text", "arrow", "line", "rect", "circle" }, "Shape") }, { "frame", frame },
                        { "x", prop("number", "text/rect: left; arrow/line: start x; circle: centre x") },
                        { "y", prop("number", "text/rect: top; arrow/line: start y; circle: centre y") },
                        { "x2", prop("number", "arrow/line end x") }, { "y2", prop("number", "arrow/line end y") },
                        { "w", prop("number", "rect/text width, circle radius x") }, { "h", prop("number", "rect height, circle radius y") },
                        { "text", prop("string", "Text for type=text") }, { "color", prop("string", "CSS colour, e.g. #e5483e") },
                        { "size", prop("number", "Line width in media pixels") }, { "font_size", prop("number", "Text size in media pixels") } },
                      { "type" }));
        t.append(tool("clear_frame", "Remove the annotations shown at a frame (adds an empty key there).", { { "frame", frame } }));
        t.append(tool("bookmark", "Add, remove or toggle a bookmark, or list all bookmarks.",
                      { { "action", enumProp({ "add", "remove", "toggle", "list" }, "What to do") }, { "frame", frame } }, { "action" }));
        t.append(tool("notes", "Frame notes (review comments) of a source: list, set or delete.",
                      { { "action", enumProp({ "list", "set", "delete" }, "What to do") }, { "frame", frame },
                        { "text", prop("string", "Note text for action=set") }, { "source_id", prop("string", "For list: default all sources") } },
                      { "action" }));
        t.append(tool("track_point", "Place a tracking marker at (x, y) on a frame and track it forward or backward. Runs in the background; poll list_tracks / motion_analysis.",
                      { { "x", prop("number", "0..1") }, { "y", prop("number", "0..1") }, { "frame", frame },
                        { "direction", enumProp({ "forward", "backward", "none" }, "Default forward") }, { "name", prop("string", "Track name") } },
                      { "x", "y" }));
        t.append(tool("list_tracks", "Tracks of a source with their tracked frame ranges and whether tracking is running.",
                      { { "source_id", prop("string", "Default: the clip under the playhead") } }, {}, true));
        t.append(tool("motion_analysis", "Spacing and timing of a tracked point: speed per frame, path length, suggested key poses (stops and direction changes).",
                      { { "track_id", prop("string", "Default: selected / first track") }, { "include_samples", prop("boolean", "Per-frame x, y, speed (default false)") },
                        { "bookmark_keys", prop("boolean", "Also bookmark the suggested keys") } }, {}, false));
        t.append(tool("set_compare", "A/B compare: show another source next to the current one.",
                      { { "mode", enumProp({ "off", "wipe", "side_by_side", "top_bottom", "blend" }, "Layout") }, { "source_id", prop("string", "B source") } },
                      { "mode" }));
        t.append(tool("set_view", "Viewer options.",
                      { { "flip", prop("boolean", "Mirror left/right") }, { "grid", prop("boolean", "Grid") }, { "hud", prop("boolean", "Frame counter overlay") },
                        { "annotations", prop("boolean", "Show annotations") }, { "channel", enumProp({ "rgb", "red", "green", "blue" }, "Colour channel") },
                        { "fit", prop("boolean", "Fit to view") } }));
        t.append(tool("set_onion", "Onion skin settings.",
                      { { "enabled", prop("boolean", "On / off") }, { "mode", enumProp({ "keys", "frames" }, "Drawing ghosts by key or by frame distance") },
                        { "before", prop("integer", "Drawing ghosts before (0-8)") }, { "after", prop("integer", "Drawing ghosts after (0-8)") },
                        { "video_before", prop("integer", "Video ghosts before (0-8)") }, { "video_after", prop("integer", "Video ghosts after (0-8)") },
                        { "step", prop("integer", "Frames between ghosts (1-12)") }, { "tint", prop("boolean", "Colour ghosts (before warm, after cool)") },
                        { "opacity", prop("number", "Nearest ghost opacity 0.05-0.9") } }));
        t.append(tool("timeline",
                      "Edit the multi-track timeline. Clips sit at explicit frames on video tracks (V1 = 0, higher tracks draw on top) "
                      "and audio tracks (A1 = 0); gaps are allowed. list: clips and tracks. add: a source (with its linked sound) at "
                      "`frame` on `track` (default: the end of the timeline). remove / split / trim / move / place / set / close_gap / add_track. "
                      "`ripple` pushes or pulls the later clips (default: the timeline's ripple setting).",
                      { { "action", enumProp({ "list", "add", "remove", "split", "trim", "move", "place", "set", "close_gap", "add_track" }, "What to do") },
                        { "clip_uid", prop("string", "Clip from action=list") }, { "source_id", prop("string", "For add") },
                        { "frame", prop("integer", "add / place: timeline frame of the clip's first frame; split / close_gap: timeline frame (default the playhead)") },
                        { "track", prop("integer", "add / place / close_gap: track number from 0 (V1 / A1)") },
                        { "kind", enumProp({ "video", "audio" }, "close_gap / add_track: track kind (default video)") },
                        { "in", prop("integer", "For trim: first source frame") }, { "out", prop("integer", "For trim: last source frame (-1 = end)") },
                        { "index", prop("integer", "For move: new position among the clips of its track (they are packed back to back)") },
                        { "ripple", prop("boolean", "remove / trim / add: ripple edit (later clips follow)") },
                        { "opacity", prop("number", "set: 0-1 (video)") }, { "scale", prop("number", "set: zoom, 1 = fit (video)") },
                        { "x", prop("number", "set: horizontal offset in canvas widths (video)") }, { "y", prop("number", "set: vertical offset in canvas heights (video)") },
                        { "blend", enumProp({ "normal", "add", "screen", "multiply", "difference" }, "set: composite mode (video)") },
                        { "gain_db", prop("number", "set: clip gain in dB (audio)") }, { "muted", prop("boolean", "set: mute the clip (audio)") },
                        { "enabled", prop("boolean", "set: clip on / off") } },
                      { "action" }));
        t.append(tool("annotation_clips",
                      "Annotations of a source seen as clips on annotation lanes (each drawing / text / shape group with a start, an end and a lane; "
                      "higher lanes draw on top). list, or move / trim / split / delete / duplicate / color one clip by `key`.",
                      { { "action", enumProp({ "list", "move", "trim", "split", "delete", "duplicate", "color" }, "What to do") },
                        { "source_id", prop("string", "Default: the clip under the playhead") }, { "key", prop("string", "Clip key from action=list") },
                        { "first", prop("integer", "move / trim / duplicate: first source frame") }, { "last", prop("integer", "trim: last source frame") },
                        { "lane", prop("integer", "move: annotation lane from 0") }, { "frame", prop("integer", "split: source frame where the second part starts") },
                        { "color", prop("string", "color: #RRGGBB label colour (empty = automatic)") } },
                      { "action" }));
        t.append(tool("set_filter",
                      "Analysis filter on the viewer (Filter menu): silhouette (by brightness), moving_silhouette (difference from the shot's "
                      "background plate), edges, value, notan, motion_highlight (change since the previous frame), or none.",
                      { { "mode", enumProp({ "none", "silhouette", "moving_silhouette", "edges", "value", "notan", "motion_highlight" }, "Filter") },
                        { "threshold", prop("number", "0-1 (turns automatic off for silhouette / notan)") }, { "auto", prop("boolean", "Automatic threshold (silhouette / notan)") },
                        { "invert", prop("boolean", "Bright subject on a dark background") }, { "show_original", prop("number", "0-1: picture behind the shape") },
                        { "levels", prop("integer", "Notan values: 2 or 3") } },
                      { "mode" }));
        t.append(tool("export_frame", "Save a frame with annotations as PNG / JPEG / TIFF.",
                      { { "path", prop("string", "Absolute output path (.png, .jpg or .tif)") }, { "frame", frame }, { "scale", prop("number", "0.1-4, default 1") },
                        { "overwrite", prop("boolean", "Replace an existing file (default false)") } },
                      { "path" }));
        t.append(tool("save_project", "Save the project (.frvm). Pass `path` the first time.",
                      { { "path", prop("string", "Absolute .frvm path") }, { "overwrite", prop("boolean", "Replace another existing file (default false)") } }));
        t.append(tool("open_project", "Open a .frvm project.",
                      { { "path", prop("string", "Absolute path") }, { "discard_changes", prop("boolean", "Drop unsaved changes in the current project") } },
                      { "path" }));
        t.append(tool("console", "Run a CHARONOS console command (help, status, frame N, play, pause, tool NAME, clear cache).",
                      { { "command", prop("string", "Command line") } }, { "command" }));
        return t;
    }();
    return tools;
}
