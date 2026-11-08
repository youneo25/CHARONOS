#pragma once

#include <QByteArray>
#include <QString>

// Small OS layer: memory figures, default folders and the few things Android does differently
// (all-files storage access, HTTPS without OpenSSL, opening links).
namespace Platform {

constexpr bool isAndroid()
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return false;
#endif
}

qint64 totalPhysicalMemory();
qint64 availablePhysicalMemory();  // -1 when unknown
qint64 processMemory();            // resident memory of this process, -1 when unknown

// Frame cache presets (Tools > Cache Memory, first-run setup): 0 = light, 1 = balanced, 2 = high.
qint64 cacheBudgetForPreset(int preset);
// Below this much free RAM the frame cache gives memory back (adaptive memory).
qint64 memoryReserve();

QString documentsDir();  // where projects are saved by default
QString exportDir();     // where exports go by default

// Android 11+: reading media from shared storage needs "All files access" so projects can keep
// plain paths (and open the same way on Windows). Always true elsewhere.
bool hasStorageAccess();
void requestStorageAccess();

// Blocking HTTPS GET (call it off the UI thread). Used on Android, where Qt has no TLS backend
// without bundled OpenSSL; the desktop builds use QNetworkAccessManager instead.
QByteArray httpGet(const QString& url, int timeoutMs, QString* error);

// Opens a web page or a downloaded file with the system.
bool openExternal(const QString& urlOrPath);

} // namespace Platform
