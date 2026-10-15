#include "FileAssociation.h"
#include "Lang.h"
#include "ui/MainWindow.h"
#include "ui/Style.h"

#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QSettings>
#include <QTranslator>
#include <QSurfaceFormat>

int main(int argc, char* argv[])
{
    QSurfaceFormat format;
    format.setSwapInterval(1);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(8);  // QPainter on QOpenGLWidget uses stencil for clipping
    format.setSamples(8);            // MSAA: without it QPainter's antialiasing hint does nothing on GL
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    QApplication::setOrganizationName("CHRONOS");
    QApplication::setApplicationName("CHRONOS");
    QApplication::setApplicationDisplayName("CHARONOS");
    {
        // Carry the settings over from the FrameReview days (language, brush, audio, recent files...).
        QSettings now;
        if (now.allKeys().isEmpty()) {
            QSettings old(QStringLiteral("FrameReview"), QStringLiteral("FrameReview"));
            for (const QString& key : old.allKeys()) now.setValue(key, old.value(key));
        }
    }
    QApplication::setApplicationVersion(FRAMEREVIEW_VERSION);
    applyDarkTheme(app);
    QApplication::setWindowIcon(QIcon(QApplication::applicationDirPath() + QStringLiteral("/app.png")));
    registerFrvmAssociation();  // double-clicking a .frvm opens this build

    // UI language: English unless Korean was chosen in Tools > Language.
    Lang::set(QSettings().value("ui/language", "en").toString() == "ko" ? Lang::Korean : Lang::English);

    // Korean labels for Qt's own dialogs (Yes/No, Save/Discard, file dialogs).
    QTranslator qtKo;
    if (Lang::current() == Lang::Korean &&
        qtKo.load(QStringLiteral("qtbase_ko"), QApplication::applicationDirPath() + QStringLiteral("/translations")))
        QApplication::installTranslator(&qtKo);

    MainWindow window;
    window.show();

    const QStringList args = app.arguments().mid(1);
    if (!args.isEmpty()) window.openPaths(args);

    return app.exec();
}
