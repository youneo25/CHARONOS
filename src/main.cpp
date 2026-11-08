#include "FileAssociation.h"
#include "Lang.h"
#include "app/Edition.h"
#include "platform/Platform.h"
#include "ui/MainWindow.h"
#include "ui/SetupWizard.h"
#include "ui/Style.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLibraryInfo>
#include <QSettings>
#include <QTranslator>
#include <QSurfaceFormat>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
extern "C" {
#include <libavcodec/jni.h>
}
#endif

int main(int argc, char* argv[])
{
    QSurfaceFormat format;
    format.setSwapInterval(1);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(8);  // QPainter on QOpenGLWidget uses stencil for clipping
    // MSAA: without it QPainter's antialiasing hint does nothing on GL (4x is plenty on a phone GPU)
    format.setSamples(Platform::isAndroid() ? 4 : 8);
#ifdef Q_OS_ANDROID
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);  // GL_RG / unpack row length for the planar video textures
#endif
    QSurfaceFormat::setDefaultFormat(format);
#ifdef Q_OS_ANDROID
    QApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);  // the menu bar is drawn by the app
#endif

    QApplication app(argc, argv);
#ifdef Q_OS_ANDROID
    // FFmpeg's MediaCodec encoder (H.264 export) talks to Java through the app's VM.
    av_jni_set_java_vm(QJniEnvironment::javaVM(), nullptr);
    // Qt's own file dialog works on plain paths (the system picker hands out content:// URIs that
    // FFmpeg and a project file opened on Windows could not use); it starts in shared storage.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QDir::setCurrent(QStringLiteral("/storage/emulated/0"));
#endif
    // Settings in a folder instead of the registry: a portable install ("portable.ini" next to the
    // exe) or a test run (CHARONOS_SETTINGS_DIR) leaves the user's own settings alone.
    const QString settingsDir = qEnvironmentVariable("CHARONOS_SETTINGS_DIR");
    const bool portable = !Platform::isAndroid() && QFileInfo::exists(QApplication::applicationDirPath() + QStringLiteral("/portable.ini"));
    if (!settingsDir.isEmpty() || portable) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           settingsDir.isEmpty() ? QApplication::applicationDirPath() + QStringLiteral("/settings") : settingsDir);
    }
    QApplication::setOrganizationName("CHRONOS");
    QApplication::setApplicationName(Edition::settingsName());
    QApplication::setApplicationDisplayName(Edition::productName());
    if (!Edition::lite) {
        // Carry the settings over from the FrameReview days (language, brush, audio, recent files...).
        QSettings now;
        if (now.allKeys().isEmpty()) {
            QSettings old(QStringLiteral("FrameReview"), QStringLiteral("FrameReview"));
            for (const QString& key : old.allKeys()) now.setValue(key, old.value(key));
        }
    }
    QApplication::setApplicationVersion(FRAMEREVIEW_VERSION);
    applyDarkTheme(app);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app.png")));
#ifdef Q_OS_WIN
    registerFrvmAssociation(!Edition::lite);  // double-clicking a .frvm opens the full build; Lite is in "Open with"
#endif

    // First run: language, start layout, memory, folders, updates. Closing it keeps the defaults.
    if (SetupWizard::needed()) {
        SetupWizard wizard;
        if (Platform::isAndroid()) wizard.setWindowState(Qt::WindowMaximized);
        if (wizard.exec() != QDialog::Accepted) SetupWizard::applyDefaults();
    }

    // UI language: English unless Korean was chosen in setup or Edit > Settings > Language.
    Lang::set(QSettings().value("ui/language", "en").toString() == "ko" ? Lang::Korean : Lang::English);

    // Korean labels for Qt's own dialogs (Yes/No, Save/Discard, file dialogs).
    QTranslator qtKo;
    if (Lang::current() == Lang::Korean &&
        (qtKo.load(QStringLiteral("qtbase_ko"), QStringLiteral(":/translations")) ||
         qtKo.load(QStringLiteral("qtbase_ko"), QApplication::applicationDirPath() + QStringLiteral("/translations"))))
        QApplication::installTranslator(&qtKo);

    MainWindow window;
    if (Platform::isAndroid()) window.showMaximized();
    else window.show();

    const QStringList args = app.arguments().mid(1);
    if (!args.isEmpty()) window.openPaths(args);

    return app.exec();
}
