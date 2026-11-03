#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

class MainWindow;
class QLocalServer;
class QLocalSocket;

// Built-in Model Context Protocol server, so AI assistants (Claude Code, Claude Desktop,
// Codex, ...) can look at and drive CHARONOS: read project state, grab the current frame as an
// image, annotate, bookmark, write frame notes, track points and read motion analysis.
//
// The app listens on a local named pipe that only the current Windows user can open and speaks
// MCP (JSON-RPC 2.0, one message per line) on it. AI clients start the small stdio bridge
// `charonos-mcp.exe` next to CHARONOS.exe, which relays stdin/stdout to that pipe and launches
// CHARONOS when it isn't running. Tools run on the UI thread, exactly like menu commands.
class McpServer : public QObject {
    Q_OBJECT
public:
    explicit McpServer(MainWindow* window);
    ~McpServer() override;

    bool start();
    void stop();
    bool isListening() const;
    int clientCount() const { return int(m_buffers.size()); }

    static QString pipeName();
    static QString bridgePath();
    static QString claudeCodeCommand();
    static QString configJson();
    static QString aboutHtml(bool listening);
    static QJsonArray toolDefinitions();
    static constexpr const char* kLatestProtocol = "2025-06-18";

    // One JSON-RPC message in, the response out (an empty object for notifications).
    QJsonObject handle(const QJsonObject& message);

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket* socket);
    QJsonObject callTool(const QJsonObject& params);

    MainWindow* m_window;
    QLocalServer* m_server = nullptr;
    QHash<QLocalSocket*, QByteArray> m_buffers;
};
