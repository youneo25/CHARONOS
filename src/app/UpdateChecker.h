#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QWidget;

// Asks GitHub for the latest release of this program. On startup it prompts once per new
// version: "Later" remembers that version and stays quiet until a newer one is published.
// Help > Check for Updates always reports the result.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    struct Release {
        QString tag;        // "v1.2.0"
        QString version;    // "1.2.0"
        QString name;
        QString notes;      // Markdown
        QString page;       // release page
        QString assetName;  // the download for this edition and platform (may be empty)
        QString assetUrl;
        qint64 assetSize = 0;
    };

    explicit UpdateChecker(QWidget* window);

    void check(bool manual);
    bool busy() const { return m_busy; }

    static QString currentVersion();
    // -1 / 0 / 1 like strcmp; "v" prefixes and pre-release suffixes ("-beta") are handled.
    static int compareVersions(const QString& a, const QString& b);
    // Picks this edition's download from a release's assets (installer, or the APK on Android).
    static int pickAsset(const QStringList& names, bool lite, bool android);
    static Release parseRelease(const QByteArray& json, bool lite, bool android, QString* error);

private:
    void handle(const QByteArray& json, const QString& error, bool manual);
    void prompt(const Release& release, bool manual);
    void install(const Release& release);

    QWidget* m_window;
    QNetworkAccessManager* m_net = nullptr;
    bool m_busy = false;
};
