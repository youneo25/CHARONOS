#include "Platform.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_ANDROID)
#include <QJniEnvironment>
#include <QJniObject>
#include <QCoreApplication>
#include <unistd.h>
#endif

namespace Platform {

namespace {
constexpr qint64 MB = qint64(1) << 20;
constexpr qint64 GB = qint64(1) << 30;

#ifdef Q_OS_ANDROID
// "MemTotal:  7812345 kB" -> bytes
qint64 meminfo(const char* key)
{
    QFile f(QStringLiteral("/proc/meminfo"));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QByteArray k(key);
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (!line.startsWith(k)) continue;
        const QList<QByteArray> parts = line.mid(k.size()).simplified().split(' ');
        return parts.isEmpty() ? -1 : parts.first().toLongLong() * 1024;
    }
    return -1;
}

const char* kHelper = "com/charonos/lite/CharonosHelper";
#endif
} // namespace

qint64 totalPhysicalMemory()
{
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) return qint64(status.ullTotalPhys);
#elif defined(Q_OS_ANDROID)
    if (const qint64 v = meminfo("MemTotal:"); v > 0) return v;
#endif
    return 8 * GB;
}

qint64 availablePhysicalMemory()
{
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) return qint64(status.ullAvailPhys);
#elif defined(Q_OS_ANDROID)
    return meminfo("MemAvailable:");
#endif
    return -1;
}

qint64 processMemory()
{
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return qint64(pmc.WorkingSetSize);
#elif defined(Q_OS_ANDROID)
    QFile f(QStringLiteral("/proc/self/statm"));
    if (f.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> parts = f.readAll().simplified().split(' ');
        if (parts.size() > 1) return parts[1].toLongLong() * sysconf(_SC_PAGESIZE);
    }
#endif
    return -1;
}

qint64 cacheBudgetForPreset(int preset)
{
    const qint64 ram = totalPhysicalMemory();
    if (isAndroid()) {
        switch (preset) {
        case 0: return std::clamp<qint64>(ram / 16, 192 * MB, 768 * MB);
        case 2: return std::clamp<qint64>(ram / 4, 512 * MB, 3 * GB);
        default: return std::clamp<qint64>(ram / 8, 256 * MB, 1536 * MB);
        }
    }
    switch (preset) {
    case 0: return std::clamp<qint64>(ram / 8, 512 * MB, 3 * GB);
    case 2: return std::clamp<qint64>(ram / 2, 2 * GB, std::max<qint64>(2 * GB, ram - 4 * GB));
    default: return std::clamp<qint64>(ram / 4, 1 * GB, 8 * GB);
    }
}

qint64 memoryReserve()
{
    const qint64 ram = totalPhysicalMemory();
    return isAndroid() ? std::max<qint64>(384 * MB, ram / 10) : std::max<qint64>(1536 * MB, ram * 12 / 100);
}

QString documentsDir()
{
#ifdef Q_OS_ANDROID
    // Shared storage, so the files are visible to other apps and can be copied to a PC.
    return QStringLiteral("/storage/emulated/0/Documents/CHARONOS");
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("CHARONOS"));
#endif
}

QString exportDir()
{
#ifdef Q_OS_ANDROID
    return QStringLiteral("/storage/emulated/0/Movies/CHARONOS");
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath(QStringLiteral("CHARONOS Exports"));
#endif
}

bool hasStorageAccess()
{
#ifdef Q_OS_ANDROID
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    return QJniObject::callStaticMethod<jboolean>(kHelper, "hasStorageAccess", "(Landroid/content/Context;)Z", activity.object());
#else
    return true;
#endif
}

void requestStorageAccess()
{
#ifdef Q_OS_ANDROID
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    QJniObject::callStaticMethod<void>(kHelper, "requestStorageAccess", "(Landroid/content/Context;)V", activity.object());
#endif
}

QByteArray httpGet(const QString& url, int timeoutMs, QString* error)
{
#ifdef Q_OS_ANDROID
    QJniEnvironment env;
    const QJniObject result = QJniObject::callStaticObjectMethod(kHelper, "httpGet", "(Ljava/lang/String;I)[B",
                                                                 QJniObject::fromString(url).object<jstring>(), jint(timeoutMs));
    if (env.checkAndClearExceptions() || !result.isValid()) {
        if (error) *error = QStringLiteral("Network request failed");
        return {};
    }
    auto bytes = result.object<jbyteArray>();
    const jsize n = env->GetArrayLength(bytes);
    QByteArray out(n, Qt::Uninitialized);
    env->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(out.data()));
    return out;
#else
    Q_UNUSED(url);
    Q_UNUSED(timeoutMs);
    if (error) *error = QStringLiteral("Not available on this platform");
    return {};
#endif
}

bool openExternal(const QString& urlOrPath)
{
#ifdef Q_OS_ANDROID
    if (!urlOrPath.startsWith(QLatin1String("http"))) {
        // Hand an APK (or any file) to the system installer / viewer through a content URI.
        QJniObject activity = QNativeInterface::QAndroidApplication::context();
        return QJniObject::callStaticMethod<jboolean>(kHelper, "openFile", "(Landroid/content/Context;Ljava/lang/String;)Z",
                                                      activity.object(), QJniObject::fromString(urlOrPath).object<jstring>());
    }
#endif
    const QUrl url = urlOrPath.contains(QLatin1String("://")) ? QUrl(urlOrPath) : QUrl::fromLocalFile(urlOrPath);
    return QDesktopServices::openUrl(url);
}

} // namespace Platform
