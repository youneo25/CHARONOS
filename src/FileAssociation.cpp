#include "FileAssociation.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <windows.h>
#include <shlobj.h>

bool registerFrvmAssociation()
{
    // test builds must not steal .frvm
    if (qEnvironmentVariableIsSet("CHRONOS_NO_ASSOC") || qEnvironmentVariableIsSet("FRAMEREVIEW_NO_ASSOC")) return false;
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString command = QStringLiteral("\"%1\" \"%2\"").arg(exe, QStringLiteral("%1"));
    const QString progId = QStringLiteral("CHRONOS.Project");

    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    const bool current = classes.value(QStringLiteral(".frvm/."))                        == progId &&
                         classes.value(progId + QStringLiteral("/shell/open/command/.")) == command;
    if (current) return false;  // already points at this exe (re-registers if the exe was moved)

    classes.setValue(QStringLiteral(".frvm/."), progId);
    classes.setValue(QStringLiteral(".frvm/OpenWithProgids/") + progId, QString());
    classes.setValue(progId + QStringLiteral("/."), QStringLiteral("CHARONOS Project"));
    classes.setValue(progId + QStringLiteral("/DefaultIcon/."), QStringLiteral("\"%1\",0").arg(exe));
    classes.setValue(progId + QStringLiteral("/shell/open/command/."), command);
    // The class registered before the rename (FrameReview.Project) goes away.
    classes.remove(QStringLiteral("FrameReview.Project"));
    classes.remove(QStringLiteral(".frvm/OpenWithProgids/FrameReview.Project"));
    classes.sync();

    // An "Always use this app" choice made in Explorer would win over the class above.
    QSettings fileExts(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.frvm"),
                       QSettings::NativeFormat);
    fileExts.remove(QStringLiteral("UserChoice"));

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}
