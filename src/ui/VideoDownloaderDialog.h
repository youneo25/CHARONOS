#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;

class VideoDownloaderDialog : public QDialog {
    Q_OBJECT
public:
    explicit VideoDownloaderDialog(QWidget* parent = nullptr);

public slots:
    void reject() override;

signals:
    void downloaded(const QString& filePath);

private:
    QString downloaderPath() const;
    void ensureDownloaderAndStart();
    void startDownload();
    void appendOutput(const QString& text);
    void updateProgress(const QString& text);
    void setBusy(bool busy);
    void cancelOrClose();
    void pollAndroidDownload();
    void browseFolder();

    QLineEdit* m_url = nullptr;
    QLineEdit* m_folder = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QPushButton* m_download = nullptr;
    QPushButton* m_cancel = nullptr;
    QProcess m_process;
    QNetworkAccessManager* m_network = nullptr;
    QString m_outputFile;
    QString m_pendingUrl;
    QString m_pendingFolder;
    bool m_installing = false;
    QByteArray m_consoleBuffer;
    QTimer m_androidPoll;
    QString m_androidStatusPath;
    QString m_androidLogPath;
    QString m_androidToken;
    qint64 m_androidLogOffset = 0;
    QElapsedTimer m_androidStarted;
};
