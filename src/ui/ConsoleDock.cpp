#include "ConsoleDock.h"
#include "Lang.h"
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>
extern "C" {
#include <libavutil/log.h>
}

namespace {
QMutex logMutex;
ConsoleDock* logSink = nullptr;
QtMessageHandler previousHandler = nullptr;
void ffmpegMessage(void* context, int level, const char* format, va_list args) {
    if (level > av_log_get_level()) return;
    thread_local int prefix = 1;
    thread_local QString pending;
    char line[4096];
    av_log_format_line2(context, level, format, args, line, sizeof(line), &prefix);
    pending += QString::fromUtf8(line);
    while (pending.contains('\n')) {
        const int end = pending.indexOf('\n');
        const QString message = "FFmpeg: " + pending.left(end).trimmed();
        pending.remove(0, end + 1);
        if (level <= AV_LOG_WARNING) qWarning().noquote() << message;
        else qInfo().noquote() << message;
    }
    if (pending.size() > 8192) pending.clear();
}
void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    QtMessageHandler previous;
    {
        QMutexLocker guard(&logMutex);
        previous = previousHandler;
        if (logSink) {
            const QString level = type == QtWarningMsg ? "WARN" : type == QtCriticalMsg || type == QtFatalMsg ? "ERROR" : type == QtDebugMsg ? "DEBUG" : "INFO";
            auto* sink = logSink;
            const QString line = QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + " [" + level + "] " + message;
            QMetaObject::invokeMethod(sink, [sink, line] { sink->append(line); }, Qt::QueuedConnection);
        }
    }
    if (previous) previous(type, context, message);
}
}

ConsoleDock::ConsoleDock(QWidget* parent) : QDockWidget(L("Console", "콘솔"), parent) {
    setObjectName("ConsoleDock");
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    m_log = new QPlainTextEdit;
    m_log->setObjectName("ConsoleLog");
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setMinimumHeight(100);
    auto* row = new QHBoxLayout;
    auto* input = new QLineEdit;
    input->setObjectName("ConsoleCommand");
    input->setPlaceholderText(L("Command: help, status, frame 12, play, pause, tool pen", "명령: help, status, frame 12, play, pause, tool pen"));
    connect(input, &QLineEdit::returnPressed, this, [this, input] {
        const QString command = input->text().trimmed();
        if (command.isEmpty()) return;
        append("> " + command);
        input->clear();
        emit commandEntered(command);
    });
    auto* clear = new QPushButton(L("Clear", "비우기"));
    connect(clear, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    auto* save = new QPushButton(L("Save Log...", "로그 저장..."));
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, L("Save Log", "로그 저장"), "F:/Codex/outputs/charonos-console.txt", "Text (*.txt)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(m_log->toPlainText().toUtf8());
        else append(file.errorString());
    });
    row->addWidget(input, 1); row->addWidget(clear); row->addWidget(save);
    layout->addWidget(m_log); layout->addLayout(row);
    setWidget(body);
    QMutexLocker guard(&logMutex);
    // Only the primary dock consumes process logging (tests may create extra windows).
    if (!logSink) { logSink = this; previousHandler = qInstallMessageHandler(messageHandler); av_log_set_callback(ffmpegMessage); }
    append(L("CHARONOS console ready. Type help for commands.", "CHARONOS 콘솔 준비 완료. help로 명령을 확인하세요."));
}
ConsoleDock::~ConsoleDock() {
    QMutexLocker guard(&logMutex);
    if (logSink == this) { av_log_set_callback(av_log_default_callback); qInstallMessageHandler(previousHandler); logSink = nullptr; previousHandler = nullptr; }
}
void ConsoleDock::append(const QString& text) { m_log->appendPlainText(text); }
