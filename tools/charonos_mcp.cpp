// charonos-mcp: stdio <-> named pipe bridge for the MCP server built into CHARONOS.
//
// AI clients (Claude Code, Claude Desktop, ...) start this program and talk MCP over its
// stdin / stdout, one JSON-RPC message per line. Every line is relayed unchanged to the
// running CHARONOS (the pipe only the current Windows user can open) and every reply is
// relayed back. When CHARONOS isn't running it is started (unless --no-launch) and requests
// wait until it is ready; if it can't be reached they get a JSON-RPC error instead of hanging.
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <string>

#include <windows.h>

namespace {

QString pipeName()
{
    // Must match McpServer::pipeName().
    QString user = qEnvironmentVariable("USERNAME", QStringLiteral("user")).toLower();
    user.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]")), QStringLiteral("_"));
    return QStringLiteral("charonos-mcp-") + user;
}

void writeLine(const QByteArray& line)
{
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

class Relay : public QObject {
public:
    explicit Relay(bool launch)
        : m_launch(launch)
    {
        connect(&m_socket, &QLocalSocket::readyRead, this, [this] {
            m_in.append(m_socket.readAll());
            for (qsizetype nl; (nl = m_in.indexOf('\n')) >= 0;) {
                const QByteArray line = m_in.left(nl).trimmed();
                m_in.remove(0, nl + 1);
                if (line.isEmpty()) continue;
                writeLine(line);
                const QJsonObject msg = QJsonDocument::fromJson(line).object();
                if (msg.contains("id") && !msg.contains("method") && m_outstanding > 0) --m_outstanding;
            }
            finishIfDone();
        });
        connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
            // CHARONOS closed: reconnect (and relaunch) on the next request.
            std::fprintf(stderr, "charonos-mcp: CHARONOS disconnected\n");
            m_outstanding = 0;
            finishIfDone();
        });
    }

    // stdin closed: leave once every request sent so far has its answer (or after 60 s).
    void clientClosed()
    {
        m_closing = true;
        QTimer::singleShot(60000, qApp, [] { QCoreApplication::quit(); });
        finishIfDone();
    }

    void fromClient(const QByteArray& line)
    {
        if (line.trimmed().isEmpty()) return;
        const QJsonObject parsed = QJsonDocument::fromJson(line).object();
        const bool request = parsed.contains("id") && parsed.contains("method");
        if (!ensureConnected()) {
            const QJsonObject msg = parsed;
            if (msg.contains("id"))
                writeLine(QJsonDocument(QJsonObject{
                                            { "jsonrpc", "2.0" },
                                            { "id", msg.value("id") },
                                            { "error", QJsonObject{ { "code", -32000 },
                                                                    { "message", "CHARONOS is not running or AI connections are turned off "
                                                                                 "(Tools > AI Assistant (MCP) > Allow AI Assistants to Connect)." } } } })
                              .toJson(QJsonDocument::Compact));
            return;
        }
        if (request) ++m_outstanding;
        m_socket.write(line);
        m_socket.write("\n");
        m_socket.flush();
    }

private:
    void finishIfDone()
    {
        if (m_closing && m_outstanding == 0) QCoreApplication::quit();
    }

    bool tryConnect(int ms)
    {
        if (m_socket.state() == QLocalSocket::ConnectedState) return true;
        m_socket.abort();
        m_socket.connectToServer(pipeName());
        return m_socket.waitForConnected(ms);
    }

    bool ensureConnected()
    {
        if (tryConnect(300)) return true;
        if (!m_launch || m_launched) return false;
        m_launched = true;
        const QString app = QCoreApplication::applicationDirPath() + QStringLiteral("/CHARONOS.exe");
        // The window must not inherit the client's stdio pipes: it would keep them open after
        // this bridge exits and the AI client would wait for an EOF that never comes.
        for (DWORD std : { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE })
            if (HANDLE h = GetStdHandle(std); h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
        QProcess launcher;
        launcher.setProgram(app);
        launcher.setWorkingDirectory(QCoreApplication::applicationDirPath());
        launcher.setStandardInputFile(QProcess::nullDevice());
        launcher.setStandardOutputFile(QProcess::nullDevice());
        launcher.setStandardErrorFile(QProcess::nullDevice());
        if (!launcher.startDetached()) {
            std::fprintf(stderr, "charonos-mcp: can't start %s\n", qPrintable(QDir::toNativeSeparators(app)));
            return false;
        }
        std::fprintf(stderr, "charonos-mcp: started CHARONOS, waiting for it\n");
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 30000) {
            if (tryConnect(500)) return true;
            QThread::msleep(250);
        }
        return false;
    }

    QLocalSocket m_socket;
    QByteArray m_in;
    bool m_launch = true;
    bool m_launched = false;
    bool m_closing = false;
    int m_outstanding = 0;
};

} // namespace

int main(int argc, char* argv[])
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains("--help") || args.contains("-h")) {
        std::fprintf(stderr, "charonos-mcp [--no-launch]\nMCP stdio bridge for CHARONOS (pipe %s).\n", qPrintable(pipeName()));
        return 0;
    }
    Relay relay(!args.contains("--no-launch"));

    // stdin is a pipe on Windows: read it on a plain thread and hand lines to the event loop.
    QThread* reader = QThread::create([&relay] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const QByteArray bytes = QByteArray::fromStdString(line);
            QMetaObject::invokeMethod(&relay, [&relay, bytes] { relay.fromClient(bytes); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&relay, [&relay] { relay.clientClosed(); }, Qt::QueuedConnection);
    });
    reader->start();
    const int code = app.exec();
    reader->wait(500);
    // The reader may still be blocked in getline; the process ends either way.
    std::fflush(stdout);
    std::_Exit(code);
}
