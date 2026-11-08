#include "VideoDownloaderDialog.h"

#include "Lang.h"
#include "platform/Platform.h"

#include <QDir>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSettings>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

namespace {
QLabel* sectionTitle(const QString& text)
{
    auto* label = new QLabel(text.toUpper());
    label->setObjectName("SectionTitle");
    return label;
}

QWidget* section(const QString& title, QWidget* body)
{
    auto* box = new QWidget;
    box->setObjectName("Section");
    box->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);
    layout->addWidget(sectionTitle(title));
    layout->addWidget(body);
    return box;
}
}

VideoDownloaderDialog::VideoDownloaderDialog(QWidget* parent)
    : QDialog(parent), m_network(new QNetworkAccessManager(this))
{
    setObjectName("VideoDownloaderDialog");
    setWindowTitle(L("Video Downloader", "비디오 다운로더"));

    auto* intro = new QLabel(L("Paste a video link. The downloaded video will be added to Sources automatically.",
                                "영상 링크를 붙여넣으세요. 다운로드가 끝나면 영상이 Sources에 자동으로 추가됩니다."));
    intro->setWordWrap(true);
    intro->setObjectName("Muted");
    if (Platform::isAndroid())
        intro->setText(L("Android uses Termux. One-time setup: install Python and yt-dlp in Termux, enable allow-external-apps, and grant CHARONOS the Run Commands permission.",
                         "Android는 Termux를 사용합니다. 최초 1회 Termux에서 `termux-setup-storage`, `pkg install python python-pip ffmpeg`, `pip install -U \"yt-dlp[default]\"`를 실행하세요. `~/.termux/termux.properties`에 `allow-external-apps=true`를 추가하고 Termux를 다시 연 뒤, Android 설정에서 CHARONOS의 Run Commands 권한을 허용하세요."));

    m_url = new QLineEdit;
    m_url->setObjectName("DownloaderUrl");
    m_url->setPlaceholderText(L("YouTube, bilibili, or another yt-dlp link", "YouTube, bilibili 또는 yt-dlp 지원 링크"));
    m_url->setMinimumHeight(34);
    const QString clipboard = QApplication::clipboard()->text().trimmed();
    if (QUrl candidate(clipboard); candidate.isValid() && !candidate.host().isEmpty()) m_url->setText(clipboard);

    auto* urlBody = new QWidget;
    auto* urlLayout = new QFormLayout(urlBody);
    urlLayout->setContentsMargins(0, 0, 0, 0);
    urlLayout->addRow(L("Video link", "영상 링크"), m_url);
    auto* urlSection = section(L("Video", "비디오"), urlBody);

    m_folder = new QLineEdit;
    QString folder = QSettings().value("videoDownloader/dir").toString();
    if (folder.isEmpty()) folder = QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath("CHARONOS Downloads");
    m_folder->setText(QDir::toNativeSeparators(folder));
    auto* browse = new QPushButton(L("Browse...", "찾아보기..."));
    connect(browse, &QPushButton::clicked, this, &VideoDownloaderDialog::browseFolder);
    auto* destination = new QWidget;
    auto* destRow = new QHBoxLayout(destination);
    destRow->setContentsMargins(0, 0, 0, 0);
    destRow->addWidget(m_folder, 1);
    destRow->addWidget(browse);
    auto* destSection = section(L("Save to", "저장 위치"), destination);

    m_status = new QLabel(L("Ready", "준비됨"));
    m_status->setObjectName("Muted");
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(250);
    m_log->setMinimumHeight(90);
    m_log->setPlaceholderText(L("Download progress and messages will appear here.", "다운로드 진행 상황과 안내가 여기에 표시됩니다."));
    auto* progressBody = new QWidget;
    auto* progressLayout = new QVBoxLayout(progressBody);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(8);
    progressLayout->addWidget(m_status);
    progressLayout->addWidget(m_progress);
    progressLayout->addWidget(m_log);

    m_cancel = new QPushButton(L("Close", "닫기"));
    m_download = new QPushButton(L("Download", "다운로드"));
    m_download->setObjectName("Primary");
    m_download->setDefault(true);
    connect(m_cancel, &QPushButton::clicked, this, &VideoDownloaderDialog::cancelOrClose);
    connect(m_download, &QPushButton::clicked, this, &VideoDownloaderDialog::ensureDownloaderAndStart);
    connect(&m_androidPoll, &QTimer::timeout, this, &VideoDownloaderDialog::pollAndroidDownload);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] { appendOutput(QString::fromLocal8Bit(m_process.readAllStandardOutput())); });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { appendOutput(QString::fromLocal8Bit(m_process.readAllStandardError())); });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exitStatus) {
        const bool install = m_installing;
        m_installing = false;
        if (install) {
            setBusy(false);
            if (exitStatus != QProcess::NormalExit || code != 0 || !QFileInfo::exists(downloaderPath())) {
                m_status->setText(L("yt-dlp could not be installed.", "yt-dlp를 설치하지 못했습니다."));
                return;
            }
            m_url->setText(m_pendingUrl);
            m_folder->setText(QDir::toNativeSeparators(m_pendingFolder));
            startDownload();
            return;
        }

        setBusy(false);
        const QStringList lines = QString::fromLocal8Bit(m_process.readAllStandardOutput()).split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            if (line.startsWith("CHARONOS_FILE:")) m_outputFile = line.mid(QStringLiteral("CHARONOS_FILE:").size()).trimmed();
            else appendOutput(line);
        }
        if (exitStatus == QProcess::NormalExit && code == 0 && QFileInfo::exists(m_outputFile)) {
            m_status->setText(L("Download complete. Adding to Sources…", "다운로드 완료. Sources에 추가하는 중…"));
            m_progress->setValue(100);
            emit downloaded(m_outputFile);
            m_status->setText(L("Added to Sources: %1", "Sources에 추가됨: %1").arg(QFileInfo(m_outputFile).fileName()));
        } else {
            m_status->setText(L("Download failed. Check the message above.", "다운로드에 실패했습니다. 위 메시지를 확인하세요."));
        }
    });

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_download);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(14);
    layout->addWidget(intro);
    layout->addWidget(urlSection);
    layout->addWidget(destSection);
    layout->addWidget(section(L("Progress", "진행 상황"), progressBody), 1);
    layout->addLayout(buttons);
    resize(720, 490);
}

QString VideoDownloaderDialog::downloaderPath() const
{
#ifdef Q_OS_WIN
    const QString local = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("tools/yt-dlp.exe");
    if (QFileInfo::exists(local)) return local;
    const QString system = QStandardPaths::findExecutable("yt-dlp.exe");
    if (!system.isEmpty()) return system;
#else
    const QString system = QStandardPaths::findExecutable("yt-dlp");
    if (!system.isEmpty()) return system;
#endif
    return {};
}

void VideoDownloaderDialog::ensureDownloaderAndStart()
{
    const QUrl url(m_url->text().trimmed());
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()) {
        QMessageBox::warning(this, L("Video Downloader", "비디오 다운로더"), L("Paste a valid http or https video link.", "올바른 http 또는 https 영상 링크를 붙여넣으세요."));
        m_url->setFocus();
        return;
    }
    if (m_folder->text().trimmed().isEmpty() || !QDir().mkpath(m_folder->text().trimmed())) {
        QMessageBox::warning(this, L("Save location", "저장 위치"), L("Choose a folder that can be created.", "만들 수 있는 폴더를 선택하세요."));
        return;
    }
    QSettings().setValue("videoDownloader/dir", QDir::fromNativeSeparators(m_folder->text().trimmed()));
#ifndef Q_OS_ANDROID
    if (!downloaderPath().isEmpty()) {
        startDownload();
        return;
    }
#else
    startDownload();
    return;
#endif
#ifdef Q_OS_WIN
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("tools/yt-dlp.exe");
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_pendingUrl = url.toString();
    m_pendingFolder = m_folder->text().trimmed();
    m_installing = true;
    setBusy(true);
    m_status->setText(L("Preparing yt-dlp…", "yt-dlp 준비 중…"));
    m_log->appendPlainText(L("Downloading the official yt-dlp Windows release.", "공식 yt-dlp Windows 버전을 다운로드합니다."));
    QNetworkRequest request(QUrl("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) m_progress->setValue(int(received * 100 / total));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, path] {
        const QByteArray data = reply->readAll();
        const auto error = reply->error();
        const QString errorText = reply->errorString();
        reply->deleteLater();
        if (!m_installing) return;
        if (error != QNetworkReply::NoError || data.size() < 1024 * 1024) {
            m_installing = false;
            setBusy(false);
            m_status->setText(L("Could not download yt-dlp: %1", "yt-dlp를 다운로드하지 못했습니다: %1").arg(errorText));
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            m_installing = false;
            setBusy(false);
            m_status->setText(L("Could not save yt-dlp in the app data folder.", "앱 데이터 폴더에 yt-dlp를 저장하지 못했습니다."));
            return;
        }
        file.close();
        m_installing = false;
        m_progress->setValue(0);
        m_status->setText(L("yt-dlp is ready.", "yt-dlp 준비 완료."));
        startDownload();
    });
#else
    QMessageBox::information(this, L("yt-dlp required", "yt-dlp 필요"),
        L("Install yt-dlp and make it available on PATH, then try again.", "yt-dlp를 설치하고 PATH에 등록한 뒤 다시 시도하세요."));
#endif
}

void VideoDownloaderDialog::startDownload()
{
    m_outputFile.clear();
    m_consoleBuffer.clear();
    m_progress->setValue(0);
    m_log->clear();
    setBusy(true);
    m_status->setText(L("Downloading video…", "영상 다운로드 중…"));
#ifdef Q_OS_ANDROID
    m_androidToken = QUuid::createUuid().toString(QUuid::Id128);
    m_androidStatusPath = QDir(m_folder->text().trimmed()).filePath(".charonos-yt-dlp-" + m_androidToken + ".status");
    m_androidLogPath = QDir(m_folder->text().trimmed()).filePath(".charonos-yt-dlp-" + m_androidToken + ".log");
    m_androidLogOffset = 0;
    m_androidStarted.start();
    QString error;
    if (!Platform::startYtDlpDownload(m_url->text().trimmed(), m_folder->text().trimmed(), m_androidToken, &error)) {
        setBusy(false);
        m_status->setText(L("Android downloader setup is required.", "Android 다운로더 설정이 필요합니다."));
        QMessageBox::information(this, L("Termux setup", "Termux 설정"),
            L("%1\n\nInstall Termux from F-Droid, install yt-dlp in Termux, enable allow-external-apps, and grant CHARONOS the Run Commands permission. See the Termux RUN_COMMAND setup guide.",
              "%1\n\nF-Droid에서 Termux를 설치하고, Termux에서 yt-dlp를 설치한 뒤 allow-external-apps를 켜세요. Android 설정에서 CHARONOS에 Run Commands 권한도 허용해야 합니다. 자세한 내용은 Termux RUN_COMMAND 설정 안내를 참고하세요.").arg(error));
        return;
    }
    m_androidPoll.start(350);
    return;
#endif
    QStringList args{
        "--newline", "--no-warnings", "--no-playlist",
        "--progress-template", "download:CHARONOS_PROGRESS:%(progress._percent_str)s|%(progress._speed_str)s|%(progress._eta_str)s",
        "--print", "after_move:CHARONOS_FILE:%(filepath)s",
        "-f", "best[ext=mp4]/best",
        "-o", QDir(m_folder->text().trimmed()).filePath("%(title).200B [%(id)s].%(ext)s"),
        m_url->text().trimmed()
    };
    m_process.setProgram(downloaderPath());
    m_process.setArguments(args);
    m_process.setWorkingDirectory(m_folder->text().trimmed());
    m_process.start();
    if (!m_process.waitForStarted(3000)) {
        setBusy(false);
        m_status->setText(L("Could not start yt-dlp: %1", "yt-dlp를 실행하지 못했습니다: %1").arg(m_process.errorString()));
    }
}

void VideoDownloaderDialog::appendOutput(const QString& text)
{
    m_consoleBuffer += text.toLocal8Bit();
    while (true) {
        const int end = m_consoleBuffer.indexOf('\n');
        if (end < 0) break;
        const QString line = QString::fromLocal8Bit(m_consoleBuffer.left(end)).trimmed();
        m_consoleBuffer.remove(0, end + 1);
        if (line.startsWith("CHARONOS_PROGRESS:")) updateProgress(line.mid(18));
        else if (!line.isEmpty() && !line.startsWith("CHARONOS_FILE:")) m_log->appendPlainText(line);
        else if (line.startsWith("CHARONOS_FILE:")) m_outputFile = line.mid(QStringLiteral("CHARONOS_FILE:").size()).trimmed();
    }
    const int carriage = m_consoleBuffer.lastIndexOf('\r');
    if (carriage >= 0) {
        const QString line = QString::fromLocal8Bit(m_consoleBuffer.left(carriage)).trimmed();
        m_consoleBuffer.remove(0, carriage + 1);
        if (line.startsWith("CHARONOS_PROGRESS:")) updateProgress(line.mid(18));
    }
}

void VideoDownloaderDialog::updateProgress(const QString& text)
{
    const QStringList values = text.split('|');
    if (values.isEmpty()) return;
    bool ok = false;
    QString percentText = values[0];
    const double percent = percentText.remove('%').trimmed().toDouble(&ok);
    if (ok) m_progress->setValue(qBound(0, int(percent), 100));
    QString status = L("Downloading", "다운로드 중");
    if (ok) status += QStringLiteral(" · %1%").arg(int(percent));
    if (values.size() > 1 && !values[1].trimmed().isEmpty()) status += QStringLiteral(" · %1").arg(values[1].trimmed());
    if (values.size() > 2 && !values[2].trimmed().isEmpty()) status += QStringLiteral(" · ETA %1").arg(values[2].trimmed());
    m_status->setText(status);
}

void VideoDownloaderDialog::setBusy(bool busy)
{
    m_url->setEnabled(!busy);
    m_folder->setEnabled(!busy);
    m_download->setEnabled(!busy);
    m_cancel->setText(busy ? L("Cancel", "취소") : L("Close", "닫기"));
}

void VideoDownloaderDialog::cancelOrClose()
{
#ifdef Q_OS_ANDROID
    if (m_androidPoll.isActive()) {
        m_status->setText(L("The Termux download is running. Keep this window open until it finishes.",
                            "Termux에서 다운로드 중입니다. 완료될 때까지 이 창을 열어 두세요."));
        return;
    }
#endif
    if (m_installing) {
        m_installing = false;
        for (QNetworkReply* reply : m_network->findChildren<QNetworkReply*>()) reply->abort();
        setBusy(false);
        m_status->setText(L("Canceled", "취소됨"));
    } else if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
    } else {
        reject();
    }
}

void VideoDownloaderDialog::pollAndroidDownload()
{
#ifdef Q_OS_ANDROID
    QFile log(m_androidLogPath);
    if (log.open(QIODevice::ReadOnly) && log.size() > m_androidLogOffset) {
        log.seek(m_androidLogOffset);
        const QString chunk = QString::fromUtf8(log.readAll());
        m_androidLogOffset = log.size();
        const QStringList lines = chunk.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            if (line.startsWith("CHARONOS_PROGRESS:")) updateProgress(line.mid(18));
            else if (!line.isEmpty()) m_log->appendPlainText(line);
        }
    }

    QFile status(m_androidStatusPath);
    if (!status.open(QIODevice::ReadOnly)) {
        if (!QFileInfo::exists(m_androidLogPath) && m_androidStarted.elapsed() > 30000) {
            m_androidPoll.stop();
            setBusy(false);
            m_status->setText(L("Termux did not start the download. Check its setup and permissions.",
                                "Termux에서 다운로드를 시작하지 못했습니다. 설정과 권한을 확인하세요."));
        }
        return;
    }
    const QStringList lines = QString::fromUtf8(status.readAll()).split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
    QString filePath;
    int exitCode = -1;
    for (const QString& line : lines) {
        if (line.startsWith("CHARONOS_FILE:")) filePath = line.mid(QStringLiteral("CHARONOS_FILE:").size()).trimmed();
        else if (line.startsWith("CHARONOS_EXIT:")) exitCode = line.mid(QStringLiteral("CHARONOS_EXIT:").size()).toInt();
    }
    if (exitCode < 0) return;
    m_androidPoll.stop();
    setBusy(false);
    if (exitCode == 0 && QFileInfo::exists(filePath)) {
        m_outputFile = filePath;
        m_status->setText(L("Download complete. Adding to Sources…", "다운로드 완료. Sources에 추가하는 중…"));
        m_progress->setValue(100);
        emit downloaded(m_outputFile);
        m_status->setText(L("Added to Sources: %1", "Sources에 추가됨: %1").arg(QFileInfo(m_outputFile).fileName()));
        QFile::remove(m_androidStatusPath);
        QFile::remove(m_androidLogPath);
        QFile::remove(QDir(m_folder->text().trimmed()).filePath(".charonos-yt-dlp-" + m_androidToken + ".sh"));
    } else {
        m_status->setText(L("Download failed. Check the Termux setup and log.", "다운로드에 실패했습니다. Termux 설정과 로그를 확인하세요."));
    }
#endif
}

void VideoDownloaderDialog::reject()
{
#ifdef Q_OS_ANDROID
    if (m_androidPoll.isActive()) {
        cancelOrClose();
        return;
    }
#endif
    QDialog::reject();
}

void VideoDownloaderDialog::browseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, L("Save videos to", "영상 저장 위치"), m_folder->text());
    if (!folder.isEmpty()) m_folder->setText(QDir::toNativeSeparators(folder));
}
