#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

class QLabel;
class QLineEdit;
class QComboBox;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;

class VideoDownloaderDialog : public QDialog {
    Q_OBJECT
public:
    explicit VideoDownloaderDialog(QWidget* parent = nullptr);

public slots:
    void reject() override;
    void setSourceImportResult(bool added);

signals:
    void downloaded(const QString& filePath);

private:
    QString downloaderPath() const;
    void installDownloader(bool forInfo);
    void handleUrlChanged();
    void lookupVideoInfo();
    void showVideoInfo(const QByteArray& json);
    void showVideoInfoError(const QString& message);
    void loadThumbnail(const QString& url, quint64 generation);
    void ensureDownloaderAndStart();
    void startDownload(bool retry = false);
    void updateAndRetry();
    void appendOutput(const QString& text);
    void updateProgress(const QString& text);
    void setBusy(bool busy);
    void cancelOrClose();
    void pollAndroidDownload();
    void browseFolder();

    QLineEdit* m_url = nullptr;
    QLineEdit* m_folder = nullptr;
    QLineEdit* m_cookies = nullptr;
    QComboBox* m_format = nullptr;
    QComboBox* m_resolution = nullptr;
    QPushButton* m_native = nullptr;
    QLabel* m_thumbnail = nullptr;
    QLabel* m_infoTitle = nullptr;
    QLabel* m_infoDetails = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QPushButton* m_download = nullptr;
    QPushButton* m_cancel = nullptr;
    QProcess m_process;
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_thumbnailReply = nullptr;
    QString m_outputFile;
    QString m_pendingUrl;
    QString m_pendingFolder;
    bool m_installing = false;
    bool m_updating = false;
    bool m_retriedAfterUpdate = false;
    QByteArray m_processOutput;
    QByteArray m_consoleBuffer;
    QByteArray m_infoOutput;
    QByteArray m_infoErrors;
    QString m_infoUrl;
    QTimer m_infoDebounce;
    quint64 m_infoGeneration = 0;
    bool m_fetchingInfo = false;
    bool m_infoLoading = false;
    bool m_videoInfoReady = false;
    bool m_pendingInfoLookup = false;
    bool m_installForInfo = false;
    int m_nativeHeight = 0;
    QTimer m_androidPoll;
    QString m_androidStatusPath;
    QString m_androidLogPath;
    QString m_androidToken;
    qint64 m_androidLogOffset = 0;
    QElapsedTimer m_androidStarted;
};
