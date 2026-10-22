#include "ui/MainWindow.h"
#include "ui/Style.h"
#include "model/AnnotationPainter.h"
#include "export/Exporter.h"
#include "ui/ExportDialog.h"
#include "ui/FrameExportDialog.h"
#include "ui/HotkeyManager.h"
#include "ui/ConsoleDock.h"
#include "ui/Timeline.h"
#include "ui/Palette.h"
#include "media/VideoDecoder.h"
#include "ai/McpServer.h"
#include "ui/Icons.h"
#include "ui/MotionGraph.h"
#include "ui/TimelineEditor.h"
#include "model/NoteClips.h"
#include "model/TimelineModel.h"
#include "analysis/AnalysisFilter.h"
#include "media/AudioMix.h"
#include "media/AudioClip.h"
#include <QAction>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QSlider>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QApplication>
#include <QBuffer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QImageWriter>
#include <QListWidget>
#include <QMenu>
#include <QSplitter>
#include <QMimeData>
#include <QPainter>
#include <QPointingDevice>
#include <QSettings>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSurfaceFormat>
#include <QTabletEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QThread>
#include <memory>
#include <limits>

class CountingUhdSource final : public FrameSource {
    MediaInfo data;
    QImage image{3840,2160,QImage::Format_RGBA8888};
public:
    std::atomic<int> decoded{0};
    CountingUhdSource() { data.width=3840; data.height=2160; data.fps=60; data.frameCount=300; image.fill(Qt::black); }
    const MediaInfo& info() const override { return data; }
    bool decode(int64_t, QImage& out, QString*) override { ++decoded; out=image; return true; }
};

class SlowLoopSource final : public FrameSource {
    MediaInfo data;
    QImage image{64,48,QImage::Format_RGBA8888};
public:
    std::atomic<bool> delayFirst{false};
    SlowLoopSource() { data.width=64; data.height=48; data.fps=24; data.frameCount=24; image.fill(Qt::black); }
    const MediaInfo& info() const override { return data; }
    bool decode(int64_t index, QImage& out, QString*) override {
        if (index==0 && delayFirst) QThread::msleep(180);
        out=image; return true;
    }
};

class FeatureRegression : public QObject {
    Q_OBJECT
    QTemporaryDir temp{QCoreApplication::applicationDirPath()+"/qa-temp-XXXXXX"};
    std::unique_ptr<MainWindow> window;
    Annotation rect(QString id = "rect") {
        Annotation a;
        a.id = id; a.type = "rect"; a.x = .12; a.y = .2; a.w = .18; a.h = .24;
        a.color = Qt::red; a.size = 3;
        return a;
    }
    static void drag(Viewport& v, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}) {
        QMouseEvent press(QEvent::MouseButtonPress, from, v.mapToGlobal(from.toPoint()), Qt::LeftButton, Qt::LeftButton, mods);
        QApplication::sendEvent(&v, &press);
        QMouseEvent move(QEvent::MouseMove, to, v.mapToGlobal(to.toPoint()), Qt::NoButton, Qt::LeftButton, mods);
        QApplication::sendEvent(&v, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, v.mapToGlobal(to.toPoint()), Qt::LeftButton, Qt::NoButton, mods);
        QApplication::sendEvent(&v, &release);
    }
    void drop(QWidget* target, const QString& path) {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(QPoint(30,30), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QVERIFY(enter.isAccepted());
        QDragMoveEvent move(QPoint(30,30), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        QVERIFY(move.isAccepted());
        QDropEvent dropped(QPointF(30,30), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &dropped);
        QVERIFY(dropped.isAccepted());
    }
    // ===================================================================== 1.1
    // Window screenshot with the viewer's own rendering on top (a window grab reuses the GL
    // widget's last composed frame, without the annotation overlay).
    static bool windowShot(MainWindow& w, const QString& path) {
        QImage shot=w.grab().toImage();
        const QImage view=w.m_view->grabFramebuffer();
        QPainter p(&shot);
        p.drawImage(QRectF(w.m_view->mapTo(&w,QPoint(0,0)),QSizeF(w.m_view->size())),view);
        p.end();
        return shot.save(path);
    }
    static bool near(const QColor& a, const QColor& b, int tol) {
        return std::abs(a.red()-b.red())<=tol && std::abs(a.green()-b.green())<=tol && std::abs(a.blue()-b.blue())<=tol;
    }
    static QColor viewPixel(Viewport& v, QPointF media) {
        const QImage img=v.grabFramebuffer();
        const QPointF p=v.mediaToWidget().map(media)*v.devicePixelRatioF();
        return img.pixelColor(std::clamp(int(p.x()),0,img.width()-1),std::clamp(int(p.y()),0,img.height()-1));
    }
    static void press(QWidget* w, QPointF at, Qt::KeyboardModifiers mods={}) {
        QMouseEvent e(QEvent::MouseButtonPress,at,w->mapToGlobal(at.toPoint()),Qt::LeftButton,Qt::LeftButton,mods); QApplication::sendEvent(w,&e);
    }
    static void moveTo(QWidget* w, QPointF at, Qt::KeyboardModifiers mods={}) {
        QMouseEvent e(QEvent::MouseMove,at,w->mapToGlobal(at.toPoint()),Qt::NoButton,Qt::LeftButton,mods); QApplication::sendEvent(w,&e);
    }
    static void release(QWidget* w, QPointF at, Qt::KeyboardModifiers mods={}) {
        QMouseEvent e(QEvent::MouseButtonRelease,at,w->mapToGlobal(at.toPoint()),Qt::LeftButton,Qt::NoButton,mods); QApplication::sendEvent(w,&e);
    }
    static void dragOn(QWidget* w, QPointF from, QPointF to, Qt::KeyboardModifiers mods={}) {
        press(w,from,mods);
        for (int i=1;i<=6;++i) moveTo(w,from+(to-from)*(i/6.0),mods);
        release(w,to,mods);
    }
    static void key(QWidget* w, int k, Qt::KeyboardModifiers mods={}) {
        QKeyEvent over(QEvent::ShortcutOverride,k,mods); QApplication::sendEvent(w,&over);
        QKeyEvent down(QEvent::KeyPress,k,mods); QApplication::sendEvent(w,&down);
    }
    QString videoClipOf(MainWindow& w, const QString& sourceId) {
        for (const TimelineClip& c : w.m_project.timeline.clips) if (c.isVideo() && c.sourceId==sourceId) return c.uid;
        return {};
    }
    void showEditor(MainWindow& w) {
        QAction* show=w.findChild<QAction*>("Window/Timeline");
        if (w.m_editorDock->isHidden()) show->trigger();
        QTRY_VERIFY(w.m_editorDock->isVisible());
        w.m_editorDock->resize(1300,330); QTest::qWait(60);
        w.m_editor->zoomToFit(); QTest::qWait(60);
    }

    QStringList videoClipOrder(MainWindow& w) { return w.videoClipOrder(); }
private slots:
    void initTestCase() {
        QVERIFY(temp.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
        QApplication::setOrganizationName("CHARONOS-regression");
        QApplication::setApplicationName("features");
        window = std::make_unique<MainWindow>();
        window->resize(1200, 850);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
    void selectionOnStartup() { QCOMPARE(window->m_view->tool(), Tool::Select); }
    void externalVideoDrops() {
        const QString video = QApplication::applicationDirPath()+"/qa-media.mp4";
        QVERIFY(QFileInfo::exists(video));
        drop(window->m_tree->viewport(), video);
        QCOMPARE(window->m_project.sources.size(), 1);
        QCOMPARE(window->m_project.sources.front().media.kind, MediaRef::Kind::Video);
        QTRY_VERIFY_WITH_TIMEOUT(window->m_view->m_a.frame != nullptr, 10000);
        QImage image(240,320,QImage::Format_RGB32); image.fill(QColor(25,25,25));
        const QString still = temp.filePath("still.png"); QVERIFY(image.save(still));
        drop(window->m_clipList->viewport(), still);
        QCOMPARE(window->m_project.sources.size(), 2);
        // URL handling leaves internal QListWidget row moves alone.
        QMimeData internal;
        internal.setData("application/x-qabstractitemmodeldatalist", "test");
        QDragEnterEvent enter(QPoint(20,20),Qt::MoveAction,&internal,Qt::LeftButton,Qt::NoModifier);
        QVERIFY(!window->eventFilter(window->m_clipList->viewport(), &enter));
    }
    void resizeEveryAnnotationKind() {
        Viewport v; v.resize(800,600); v.setMediaSize(QSize(320,180));
        auto image=std::make_shared<QImage>(320,180,QImage::Format_RGB32); image->fill(Qt::black); v.setFrame(VideoFrame::share(*image));
        v.show(); QVERIFY(QTest::qWaitForWindowExposed(&v));
        for (const QString& type : {"stroke","line","arrow","rect","circle","text"}) {
            Annotation a=rect(); a.type=type; a.points={{.1,.2},{.3,.45}};
            a.x1=.1; a.y1=.2; a.x2=.3; a.y2=.45; a.cx=.2; a.cy=.3; a.rx=.1; a.ry=.1;
            a.text="CHARONOS"; a.fontSize=16; a.angle=.35;
            v.setAnnotations({{a,false}}); v.setSelection({a.id});
            const auto g=v.gizmo(); const auto xf=v.annoXf();
            const QPointF right=(g.box[1]+g.box[2])/2;
            QSignalSpy edits(&v,&Viewport::annotationsEdited);
            drag(v,xf.map(right),xf.map(g.center+(right-g.center)*1.6));
            QCOMPARE(edits.count(),1);
            const Annotation resized=v.items().front().annotation;
            QVERIFY(std::abs(resized.stretchX-1.6)<.02);
            QCOMPARE(resized.stretchY,1.0);
            const auto g2=v.gizmo(); const QPointF bottom=(g2.box[2]+g2.box[3])/2;
            drag(v,xf.map(bottom),xf.map(g2.center+(bottom-g2.center)*1.4));
            QVERIFY(std::abs(v.items().front().annotation.stretchY-1.4)<.02);
            // Rendered geometry and hit testing use the same stretched local axes.
            QVERIFY(AnnotationPainter::hit(v.items().front().annotation,QSizeF(320,180),g2.center,0));
        }
    }
    void pressureForAllStyles() {
        Viewport v; v.resize(800,500); v.setMediaSize(QSize(320,180));
        auto image=std::make_shared<QImage>(320,180,QImage::Format_RGB32); image->fill(Qt::black); v.setFrame(VideoFrame::share(*image));
        QPointingDevice pen("Regression pen", 11, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        for (Tool tool : {Tool::Pencil,Tool::Pen,Tool::Highlighter}) {
            v.setTool(tool); QSignalSpy created(&v,&Viewport::annotationCreated);
            auto send=[&](QEvent::Type type,QPointF point,qreal pressure) {
                QTabletEvent event(type,&pen,point,point,pressure,0,0,0,0,0,Qt::NoModifier,
                                   type==QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                                   type==QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
                QApplication::sendEvent(&v,&event); QVERIFY(event.isAccepted());
            };
            send(QEvent::TabletPress,{100,100},.1);
            send(QEvent::TabletMove,{300,100},.9);
            send(QEvent::TabletRelease,{300,100},0);
            QCOMPARE(created.count(),1);
            const Annotation a=qvariant_cast<Annotation>(created.front().front());
            QCOMPARE(a.points.size(),2); QCOMPARE(a.pressures.size(),2);
            QVERIFY(a.pressures[0]<a.pressures[1]);
            QCOMPARE(a.penStyle,tool==Tool::Pencil ? "pencil" : tool==Tool::Pen ? "pen" : "highlighter");
            QImage rendered(320,180,QImage::Format_ARGB32_Premultiplied); rendered.fill(Qt::transparent);
            QPainter painter(&rendered); AnnotationPainter::paint(painter,a,QSizeF(320,180),1,false); painter.end();
            QVERIFY(!rendered.isNull());
        }
    }
    void joinFollowEditUndoAndSave() {
        window->raise(); window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window.get()));
        window->setSolo(window->m_project.sources[0].id);
        Source& src=window->m_project.sources[0];
        src.annotations.clear(); src.annotations[0]={rect()};
        Track t; t.id="follow"; t.name="Follow"; t.color=Qt::green;
        t.points={{0,{.65,.6}},{4,{.75,.7}},{8,{.85,.8}}}; t.keys={0}; src.tracks={t};
        window->goToFrame(0); window->m_view->setSelection({"rect"});
        const auto xf=window->m_view->annoXf(); const QSize size=window->m_view->annoMedia();
        QTest::mouseClick(window->m_view,Qt::LeftButton,Qt::ShiftModifier,xf.map(QPointF(.65*size.width(),.6*size.height())).toPoint());
        QCOMPARE(window->m_view->selectedIds(),QStringList{"rect"});
        QCOMPARE(window->m_selectedTrackId,QString("follow"));
        QTest::keyClick(window->m_view,Qt::Key_J,Qt::ControlModifier);
        QCOMPARE(src.annotations[0][0].trackId,QString("follow"));
        window->goToFrame(4);
        QCOMPARE(window->m_view->items()[0].annotation.tx,.1);
        Annotation moved=window->m_view->items()[0].annotation; moved.tx+=.05;
        window->onAnnotationsEdited({moved});
        QCOMPARE(src.annotations.at(0)[0].tx,0.0);
        QVERIFY(std::abs(src.annotations.at(4)[0].tx-.05)<1e-9);
        window->goToFrame(8); QVERIFY(std::abs(window->m_view->items()[0].annotation.tx-.25)<1e-9);
        window->m_view->setSelection({"rect"}); QTest::keyClick(window->m_view,Qt::Key_J,Qt::ControlModifier|Qt::ShiftModifier);
        QVERIFY(src.annotations.at(8)[0].trackId.isEmpty());
        QVERIFY(std::abs(src.annotations.at(8)[0].tx-.25)<1e-9);
        QCOMPARE(src.annotations.at(0)[0].trackId,QString("follow"));
        window->undo(); QVERIFY(!src.explicitAt(8));
        window->redo(); QVERIFY(src.annotations.at(8)[0].trackId.isEmpty());
        window->undo();
        const QString file=temp.filePath("roundtrip.frvm"); QString error;
        QVERIFY2(window->m_project.save(file,&error),qPrintable(error));
        Project loaded; QVERIFY2(loaded.load(file,&error),qPrintable(error));
        QCOMPARE(loaded.sources[0].annotations[0][0].trackId,QString("follow"));
        const Annotation evaluated=loaded.sources[0].evaluated(*loaded.sources[0].renderable(8).front().annotation,8);
        QVERIFY(std::abs(evaluated.tx-.25)<1e-9);
        QImage black(320,180,QImage::Format_RGB32); black.fill(Qt::black);
        const QImage output=composeFrame(black,loaded.sources[0],8,black.size(),true,{},false,0);
        QVERIFY(output.save(QApplication::applicationDirPath()+"/qa-export-follow.png"));
        QVERIFY(output.pixelColor(118,72).red()>100); // both X and Y follow the tracked motion
        QCOMPARE(output.pixelColor(38,36),QColor(Qt::black)); // original position is empty
    }
    void onionKeysAndAsyncVideo() {
        Source& src=window->m_project.sources[0];
        src.annotations.clear(); src.tracks.clear();
        Annotation a=rect(); src.annotations[0]={a}; a.x=.4; src.annotations[4]={a}; a.x=.6; src.annotations[8]={a};
        window->m_onion=true; window->m_onionBefore=2; window->m_onionAfter=1; window->m_onionVideoBefore=1; window->m_onionVideoAfter=1;
        window->goToFrame(5);
        QCOMPARE(window->m_view->m_ghosts.size(),2);
        QCOMPARE(window->m_view->m_ghosts[0].annotation.x,.12);
        QCOMPARE(window->m_view->m_ghosts[1].annotation.x,.6);
        QTRY_VERIFY_WITH_TIMEOUT(window->m_view->m_ghostInfo.size()==2,10000);
        src.annotations[6]={}; window->goToFrame(6);
        QVERIFY(window->m_view->items().isEmpty());
        QCOMPARE(window->m_view->m_ghosts.size(),3);
        window->m_onion=false; window->refreshAnnotations();
        QVERIFY(window->m_view->m_ghosts.isEmpty()); QVERIFY(window->m_view->m_ghostInfo.isEmpty());
    }
    void compareGridAndVideoGhosts() {
        Viewport v; v.resize(1000,700); v.setMediaSize(QSize(320,180));
        // Stills reach the GL upload path as RGBA frames (video arrives as Y'CbCr planes).
        auto black=std::make_shared<QImage>(320,180,QImage::Format_RGBA8888); black->fill(Qt::black);
        auto b=std::make_shared<QImage>(240,320,QImage::Format_RGBA8888); b->fill(Qt::black);
        auto red=std::make_shared<QImage>(320,180,QImage::Format_RGBA8888); red->fill(Qt::red);
        auto blue=std::make_shared<QImage>(240,320,QImage::Format_RGBA8888); blue->fill(Qt::blue);
        v.setFrame(VideoFrame::share(*black)); v.setCompareFrame(VideoFrame::share(*b),b->size()); v.setCompareLabels("A","B");
        v.show(); QVERIFY(QTest::qWaitForWindowExposed(&v));
        auto sample=[&](const QImage& image,QPointF point) {
            const qreal dpr=image.devicePixelRatio(); return image.pixelColor(qRound(point.x()*dpr),qRound(point.y()*dpr));
        };
        for (CompareMode mode : {CompareMode::Wipe,CompareMode::SideBySide,CompareMode::TopBottom,CompareMode::Blend}) {
            v.setCompareMode(mode); v.setOnion({},{}); v.setShowGrid(false);
            QTest::qWait(25); const QImage plain=v.grabFramebuffer();
            v.setShowGrid(true); QTest::qWait(25); const QImage grid=v.grabFramebuffer();
            if (mode==CompareMode::SideBySide) QVERIFY(grid.save(QApplication::applicationDirPath()+"/qa-compare-grid.png"));
            // B's grid at 2/3 width, 1/4 height, away from labels and selection border.
            QPointF pt=v.annoToWidget(1).map(QPointF(240.*2/3,320.*.25));
            QVERIFY2(sample(grid,pt).red()>sample(plain,pt).red()+10,"Grid absent on B");
            v.setShowGrid(false); v.setOnion({},{{VideoFrame::share(*red),.28,0},{VideoFrame::share(*blue),.28,1}});
            QTest::qWait(25); const QImage ghosts=v.grabFramebuffer();
            const QPointF bp=v.annoToWidget(1).map(QPointF(240*.8,320*.7));
            QVERIFY(ghosts.save(QApplication::applicationDirPath()+QString("/qa-ghost-%1.png").arg(int(mode))));
            QVERIFY2(sample(ghosts,bp).blue()>40,"B onion frame hidden behind B base video");
            if (mode==CompareMode::SideBySide) QVERIFY(ghosts.save(QApplication::applicationDirPath()+"/qa-compare-onion.png"));
        }
    }
    void editingAndTrackingOnB() {
        QVERIFY(window->m_project.sources.size() >= 2);
        Source& a=window->m_project.sources[0]; Source& b=window->m_project.sources[1];
        b.annotations[0]={rect("b-rect")};
        Track t; t.id="b-follow"; t.color=Qt::green; t.points={{0,{.7,.7}}}; t.keys={0}; b.tracks={t};
        window->setCompareSource(b.id); window->setCompareMode(CompareMode::SideBySide); window->goToFrame(0);
        QTRY_VERIFY_WITH_TIMEOUT(window->m_view->m_b.frame!=nullptr,5000);
        window->m_view->setActiveSide(1); window->m_view->setSelection({"b-rect"});
        window->m_selectedTrackId=t.id;
        window->joinSelectionToTrack(); QCOMPARE(b.annotations[0][0].trackId,t.id);
        for (const auto& [frame,list] : a.annotations) for (const Annotation& note : list) QVERIFY(note.trackId!=t.id);
        Annotation changed=window->m_view->items()[0].annotation; changed.stretchX=1.8; changed.stretchY=.7;
        window->onAnnotationsEdited({changed});
        QCOMPARE(b.annotations[0][0].stretchX,1.8); QCOMPARE(b.annotations[0][0].stretchY,.7);
        window->m_onion=true; window->m_onionVideoBefore=1; window->m_onionVideoAfter=1; window->refreshAnnotations();
        window->m_view->setShowGrid(true);
        QTest::qWait(100);
        QCOMPARE(window->m_view->compareMode(),CompareMode::SideBySide);
        QCOMPARE(window->m_view->activeSide(),1);
        const QImage viewportImage=window->m_view->grabFramebuffer();
        QVERIFY(viewportImage.save(QApplication::applicationDirPath()+"/qa-application-viewport.png"));
        QVERIFY(window->grab().save(QApplication::applicationDirPath()+"/qa-application.png"));
        window->setCompareMode(CompareMode::Off);
    }
    void transformRoundtrip() {
        Annotation a=rect(); a.stretchX=2.3; a.stretchY=.6; a.angle=.4; a.penStyle="highlighter";
        a.trackId="follow"; a.trackAnchor={.2,.3};
        const Annotation loaded=Annotation::fromJson(a.toJson());
        QCOMPARE(loaded.stretchX,a.stretchX); QCOMPARE(loaded.stretchY,a.stretchY);
        QCOMPARE(loaded.penStyle,a.penStyle); QCOMPARE(loaded.trackAnchor,a.trackAnchor);
        Annotation legacy=Annotation::fromJson(QJsonObject{{"type","rect"},{"transform",QJsonObject{{"scale",2}}}});
        QCOMPARE(legacy.stretchX,1.0); QCOMPARE(legacy.stretchY,1.0); QCOMPARE(legacy.scale,2.0);
    }
    void inheritedTransformsHoldUntilNextKey() {
        MainWindow w; w.createCanvas("Hold",{320,180},80,24);
        Source& s=w.m_project.sources.front();
        AnnotationList original;
        for (const QString& type : {"stroke","line","arrow","rect","circle","text","image"}) {
            Annotation a=rect(type); a.type=type; a.points={{.1,.2},{.3,.4}};
            a.x1=.1; a.y1=.2; a.x2=.3; a.y2=.4; a.cx=.4; a.cy=.4; a.rx=.1; a.ry=.15;
            a.text="Hold"; original.append(a);
        }
        s.annotations[10]=original;
        Annotation future=rect("future"); s.annotations[40]={future};
        w.goToFrame(26);
        QVector<Annotation> changed;
        for (Annotation a : original) {
            AnnotationTransform::translate(a,{32,18},{320,180});
            AnnotationTransform::rotateAbout(a,.4,{160,90},{320,180});
            AnnotationTransform::scaleAbout(a,1.3,{160,90},{320,180});
            AnnotationTransform::stretchAbout(a,1.7,.7,{160,90},{320,180});
            changed.append(a);
        }
        w.onAnnotationsEdited(changed);
        QVERIFY(s.renderable(9).isEmpty());
        const auto baseJson=annotationsToJson(original), changedJson=annotationsToJson(changed);
        for (int f=10;f<26;++f) {
            QCOMPARE(s.keyAt(f),10);
            QCOMPARE(annotationsToJson(*s.explicitAt(s.keyAt(f))),baseJson);
        }
        QCOMPARE(annotationsToJson(s.annotations.at(26)),changedJson);
        QCOMPARE(s.keyAt(39),26); QCOMPARE(s.keyAt(40),40);
        QCOMPARE(s.annotations.at(40).front().id,QString("future"));
        w.undo(); QVERIFY(!s.explicitAt(26)); QCOMPARE(s.keyAt(26),10);
        w.redo(); QCOMPARE(annotationsToJson(s.annotations.at(26)),changedJson);
        // Editing an intervening earlier frame cannot overwrite the later state.
        w.goToFrame(17); w.m_view->setSelection({"circle","text"});
        QVERIFY(w.applyToSelection([](Annotation& a) { a.color=Qt::green; a.opacity=.6; }));
        w.goToFrame(18); // commits the grouped property session before navigation
        QVERIFY(!w.m_pending); QCOMPARE(s.annotations.at(17)[4].color,QColor(Qt::green));
        QCOMPARE(annotationsToJson(s.annotations.at(10)),baseJson);
        QCOMPARE(annotationsToJson(s.annotations.at(26)),changedJson);
        const QString path=temp.filePath("held-states.frvm"); QString error;
        QVERIFY2(w.m_project.save(path,&error),qPrintable(error));
        Project loaded; QVERIFY2(loaded.load(path,&error),qPrintable(error));
        QCOMPARE(annotationsToJson(loaded.sources.front().annotations.at(10)),baseJson);
        QCOMPARE(annotationsToJson(loaded.sources.front().annotations.at(26)),changedJson);
        w.m_dirty=false;
    }
    void unfinishedDragCommitsBeforeNavigation() {
        MainWindow w; w.resize(1200,850); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.createCanvas("Drag",{320,180},50,24); QTRY_VERIFY(w.m_view->m_a.frame);
        w.onAnnotationCreated(rect()); w.goToFrame(10); auto* v=w.m_view;
        v->setSelection({"rect"});
        const QPointF start=v->annoXf().map(AnnotationPainter::boundingBox(rect(),{320,180}).center());
        const QPointF end=start+QPointF(30,20);
        QMouseEvent press(QEvent::MouseButtonPress,start,v->mapToGlobal(start.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(v,&press);
        QMouseEvent move(QEvent::MouseMove,end,v->mapToGlobal(end.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(v,&move);
        QVERIFY(v->m_dragChanged);
        w.goToFrame(26);
        const Source& s=w.m_project.sources.front();
        QCOMPARE(s.annotations.at(0).front().tx,0.0);
        QVERIFY(s.annotations.at(10).front().tx>0); QVERIFY(!s.explicitAt(26));
        QCOMPARE(v->m_drag,Viewport::Drag::None);
        // A same-value edit on an inherited frame must not insert a redundant key.
        w.onAnnotationsEdited({v->items().front().annotation}); QVERIFY(!s.explicitAt(26));
        w.undo(); QVERIFY(!s.explicitAt(10)); w.redo(); QVERIFY(s.explicitAt(10));
        w.m_dirty=false;
    }
    void inheritedEraseDeleteAndCompareIsolation() {
        MainWindow w; w.createCanvas("A",{320,180},80,24);
        const QString aid=w.m_project.sources.front().id;
        w.createCanvas("B",{320,180},80,24);
        Source& a=*w.m_project.find(aid); Source& b=w.m_project.sources.back();
        a.annotations[9]={rect("a")}; b.annotations[9]={rect("b"),rect("keep")};
        w.setSolo(a.id); w.setCompareSource(b.id); w.setCompareMode(CompareMode::SideBySide);
        w.goToFrame(25); w.m_view->setActiveSide(1);
        const int bf=w.editTarget().frame;
        QCOMPARE(bf,25);
        Annotation moved=b.evaluated(*b.renderable(bf).front().annotation,bf); moved.tx=.2;
        w.onAnnotationsEdited({moved});
        QCOMPARE(b.annotations.at(9).front().tx,0.0); QCOMPARE(b.annotations.at(bf).front().tx,.2);
        QVERIFY(!a.explicitAt(25));
        w.goToFrame(30); w.removeAnnotations({"b"});
        QCOMPARE(b.annotations.at(30).size(),1); QCOMPARE(b.annotations.at(25).size(),2);
        w.goToFrame(35);
        w.onEraseAt({50,50},100); w.commitEdit();
        QVERIFY(b.explicitAt(35)); QVERIFY(b.annotations.at(35).isEmpty());
        QCOMPARE(b.annotations.at(30).size(),1); QCOMPARE(b.annotations.at(9).size(),2);
        w.undo(); QVERIFY(!b.explicitAt(35)); QCOMPARE(b.renderable(35).size(),1);
        w.redo(); QVERIFY(b.renderable(36).isEmpty());
        w.m_dirty=false;
    }
    void channelMenusAndRenderedPixels() {
        MainWindow menus;
        for (int i=0;i<4;++i) {
            auto* action=menus.findChild<QAction*>(QString("ColorChannel%1").arg(i)); QVERIFY(action);
            action->trigger(); QCOMPARE(menus.m_view->colorChannel(),ColorChannel(i)); QVERIFY(action->isChecked());
        }
        Viewport v; v.resize(900,650); v.setMediaSize({320,180});
        auto a=std::make_shared<QImage>(320,180,QImage::Format_RGBA8888); a->fill(QColor(64,128,192));
        auto b=std::make_shared<QImage>(320,180,QImage::Format_RGBA8888); b->fill(QColor(180,90,30));
        v.setFrame(VideoFrame::share(*a)); v.setCompareFrame(VideoFrame::share(*b),b->size()); v.show(); QVERIFY(QTest::qWaitForWindowExposed(&v));
        for (CompareMode compare : {CompareMode::Off,CompareMode::Wipe,CompareMode::SideBySide,CompareMode::TopBottom,CompareMode::Blend}) {
            v.setCompareMode(compare);
            for (int side : {0,1}) {
                if (side && compare==CompareMode::Off) continue;
                const QPointF pt=v.annoToWidget(side).map(QPointF(side ? 256 : 64, side ? 144 : 36));
                auto sample=[&] { QImage img=v.grabFramebuffer(); return img.pixelColor((pt*img.devicePixelRatio()).toPoint()); };
                v.setColorChannel(ColorChannel::RGB); const QColor full=sample();
                for (ColorChannel ch : {ColorChannel::Red,ColorChannel::Green,ColorChannel::Blue}) {
                    v.setColorChannel(ch); const QColor filtered=sample();
                    QVERIFY(std::abs(filtered.red()-(ch==ColorChannel::Red ? full.red() : 0))<=2);
                    QVERIFY(std::abs(filtered.green()-(ch==ColorChannel::Green ? full.green() : 0))<=2);
                    QVERIFY(std::abs(filtered.blue()-(ch==ColorChannel::Blue ? full.blue() : 0))<=2);
                }
            }
        }
        v.setCompareMode(CompareMode::Off); v.setColorChannel(ColorChannel::Green);
        v.setOnion({},{{VideoFrame::share(*b),.28,0}});
        const QImage ghost=v.grabFramebuffer();
        const QPoint pt=(v.annoToWidget(0).map(QPointF(160,90))*ghost.devicePixelRatio()).toPoint();
        const QColor pixel=ghost.pixelColor(pt); QCOMPARE(pixel.red(),0); QCOMPARE(pixel.blue(),0); QVERIFY(pixel.green()>90);
        QVERIFY(ghost.save(QApplication::applicationDirPath()+"/qa-channel-green.png"));
    }
    void trackingManagerVisibilityCreationUndoAndSave() {
        MainWindow w; w.resize(1400,850); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.createCanvas("Tracking A",{640,360},60,24); QTRY_VERIFY(w.m_view->m_a.frame);
        const QString aid=w.m_project.sources.front().id;
        w.goToFrame(12); w.placeTrack({.4,.5});
        Source& a=*w.m_project.find(aid);
        const QString id=a.tracks.front().id;
        QCOMPARE(a.tracks.front().createdFrame,12);
        // Backward tracking must not move the creation boundary.
        for (int f=4;f<=18;++f) a.tracks.front().points[f]={.3+f*.01,.5};
        auto render=[&](const Track& t,int frame) {
            QImage image(640,360,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image); TrackPainter::Style style;
            TrackPainter::paint(painter,{t},frame,{640,360},style); painter.end(); return image;
        };
        QImage empty(640,360,QImage::Format_ARGB32_Premultiplied); empty.fill(Qt::transparent);
        QCOMPARE(render(a.tracks.front(),11),empty);
        QVERIFY(render(a.tracks.front(),12)!=empty);
        w.goToFrame(8);
        auto marker=[&] { return w.m_view->annoXf().map(QPointF(.38*640,.5*360)); };
        QVERIFY(w.m_view->hitTrack(marker()).isEmpty());
        auto* action=w.findChild<QAction*>("ToggleTrackingManager"); QVERIFY(action); action->trigger();
        QTRY_VERIFY(w.m_trackingManager->isVisible());
        QCOMPARE(w.m_trackTree->topLevelItemCount(),1);
        auto* item=w.m_trackTree->topLevelItem(0);
        QCOMPARE(item->text(3),QString("12"));
        item->setCheckState(2,Qt::Unchecked);
        QVERIFY(!a.tracks.front().hideBeforeCreation);
        QVERIFY(render(a.tracks.front(),8)!=empty);
        QCOMPARE(w.m_view->hitTrack(marker()),id);
        w.undo(); QVERIFY(a.tracks.front().hideBeforeCreation); QVERIFY(w.m_view->hitTrack(marker()).isEmpty());
        w.redo(); QVERIFY(!a.tracks.front().hideBeforeCreation);
        item=w.m_trackTree->topLevelItem(0); item->setCheckState(1,Qt::Unchecked);
        QVERIFY(!a.tracks.front().visible); QCOMPARE(render(a.tracks.front(),12),empty);
        QImage footage(640,360,QImage::Format_RGB32); footage.fill(Qt::black);
        StillFrameOptions options; options.tracks=true; options.notes=false; options.includeOutside=true; options.padding=0;
        QCOMPARE(stillFrameBounds(footage.size(),a,12,options),QRectF(0,0,640,360));
        QCOMPARE(composeFrame(footage,a,12,footage.size(),false,{},true),footage);
        QVERIFY(w.m_view->hitTrack(marker()).isEmpty());
        w.undo(); QVERIFY(a.tracks.front().visible);
        auto* show=w.findChild<QCheckBox*>("TrackingShowOverlays"); QVERIFY(show);
        show->setChecked(false); QVERIFY(!w.m_view->m_showTracks); QVERIFY(!w.m_showTracksAction->isChecked());
        w.setTool(Tool::Track); QVERIFY(w.m_view->hitTrack(marker()).isEmpty());
        w.m_showTracksAction->setChecked(true); QVERIFY(show->isChecked()); QCOMPARE(w.m_view->hitTrack(marker()),id);
        const QImage shownOverlay=w.m_view->grabFramebuffer();
        w.m_showTracksAction->setChecked(false);
        const QImage hiddenOverlay=w.m_view->grabFramebuffer();
        QVERIFY(shownOverlay!=hiddenOverlay);
        w.m_showTracksAction->setChecked(true);
        const QString path=temp.filePath("track-manager.frvm"); QString error;
        QVERIFY2(w.m_project.save(path,&error),qPrintable(error));
        Project loaded; QVERIFY2(loaded.load(path,&error),qPrintable(error));
        const Track saved=loaded.sources.front().tracks.front();
        QCOMPARE(saved.createdFrame,12); QVERIFY(saved.visible); QVERIFY(!saved.hideBeforeCreation);
        Track legacy=Track::fromJson(QJsonObject{{"keys",QJsonArray{12}}, {"points",QJsonObject{{"4",QJsonArray{.3,.5}}}}});
        QCOMPARE(legacy.createdFrame,12); QVERIFY(!legacy.visibleAt(11)); QVERIFY(legacy.visibleAt(12));
        legacy.keys.insert(2); QCOMPARE(legacy.creationFrame(),12);
        QTest::qWait(30);
        QVERIFY(w.grab().save(QApplication::applicationDirPath()+"/qa-tracking-manager.png"));
        // The manager follows the editable compare side, including B at a different frame rate.
        w.createCanvas("Tracking B",{640,360},60,30);
        const QString bid=w.m_project.sources.back().id;
        w.setSolo(aid); w.setCompareSource(bid); w.setCompareMode(CompareMode::SideBySide);
        w.goToFrame(8); w.m_view->setActiveSide(1);
        const int bf=w.editTarget().frame; w.placeTrack({.5,.5});
        QCOMPARE(w.m_trackTree->property("sourceId").toString(),bid);
        QCOMPARE(w.m_project.find(bid)->tracks.front().createdFrame,bf);
        w.m_trackTree->topLevelItem(0)->setCheckState(1,Qt::Unchecked);
        QVERIFY(!w.m_project.find(bid)->tracks.front().visible); QVERIFY(w.m_project.find(aid)->tracks.front().visible);
        w.m_view->setActiveSide(0); QCOMPARE(w.m_trackTree->property("sourceId").toString(),aid);
        action->trigger(); QVERIFY(!w.m_trackingManager->isVisible());
        w.m_dirty=false;
    }
    void textOptionsFocusStyleHoldCancelAndSave() {
        MainWindow w; w.resize(1200,850); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.raise(); w.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&w));
        w.createCanvas("Text",{640,360},60,24); QTRY_VERIFY(w.m_view->m_a.frame);
        w.goToFrame(9); Annotation a=rect("styled-text"); a.type="text"; a.text="Frame 10"; a.x=.2; a.y=.5;
        w.onAnnotationCreated(a); w.goToFrame(25); auto* v=w.m_view;
        v->editText(a.id); QVERIFY(v->isEditingText()); QVERIFY(v->m_textOptions->isVisible());
        QVERIFY(v->m_textOptions->geometry().bottom()<v->m_textEdit->y());
        auto* size=v->findChild<QDoubleSpinBox*>("TextFontSize"); QVERIFY(size);
        QTest::mouseClick(size,Qt::LeftButton); QTest::qWait(20); QVERIFY(v->isEditingText());
        size->setValue(48);
        QTest::mouseClick(v->findChild<QPushButton*>("TextBold"),Qt::LeftButton);
        QTest::mouseClick(v->findChild<QPushButton*>("TextItalic"),Qt::LeftButton);
        v->findChild<QFontComboBox*>("TextFontFamily")->setCurrentFont(QFont("Arial"));
        auto* opacity=v->findChild<QSpinBox*>("TextOpacity");
        auto* slider=v->findChild<QSlider*>("TextOpacitySlider"); QVERIFY(slider);
        slider->setValue(0); QCOMPARE(opacity->value(),0); QCOMPARE(v->m_textAnn.opacity,0.0);
        opacity->setValue(100); QCOMPARE(slider->value(),100);
        slider->setValue(75); QCOMPARE(opacity->value(),75);
        v->findChild<QCheckBox*>("TextBackground")->setChecked(true);
        QTest::qWait(20); QVERIFY(v->isEditingText());
        QCOMPARE(v->m_textEdit->font().family(),QString("Arial")); QVERIFY(v->m_textEdit->font().bold()); QVERIFY(v->m_textEdit->font().italic());
        QVERIFY(w.grab().save(QApplication::applicationDirPath()+"/qa-text-options.png"));
        auto* family=v->findChild<QFontComboBox*>("TextFontFamily");
        QVERIFY(v->isEditingText());
        QTest::mouseClick(family,Qt::LeftButton,Qt::NoModifier,QPoint(family->width()-15,family->height()/2));
        QTRY_VERIFY_WITH_TIMEOUT(family->view()->isVisible(),2000);
        QVERIFY(v->isEditingText());
        QVERIFY(family->view()->window()->grab().save(QApplication::applicationDirPath()+"/qa-font-popup.png"));
        family->hidePopup();
        QTest::mouseClick(v->findChild<QPushButton*>("TextDone"),Qt::LeftButton);
        Source& s=w.m_project.sources.front(); QVERIFY(!v->isEditingText()); QVERIFY(!v->m_textOptions->isVisible());
        QCOMPARE(s.annotations.at(9).front().fontSize,28.0); QVERIFY(!s.annotations.at(9).front().bold);
        const Annotation styled=s.annotations.at(25).front(); QCOMPARE(styled.text,a.text); QCOMPARE(styled.fontSize,48.0);
        QCOMPARE(styled.fontFamily,QString("Arial")); QVERIFY(styled.bold); QVERIFY(styled.italic); QVERIFY(styled.useBg); QCOMPARE(styled.opacity,.75);
        w.goToFrame(30); v->editText(a.id); size->setValue(90);
        QTest::mouseClick(v->findChild<QPushButton*>("TextCancel"),Qt::LeftButton);
        QVERIFY(!s.explicitAt(30));
        // Navigation while a control has focus commits at the original frame.
        v->editText(a.id); size->setValue(54); size->setFocus(); w.goToFrame(35);
        QCOMPARE(s.annotations.at(30).front().fontSize,54.0); QVERIFY(!s.explicitAt(35));
        w.undo(); QVERIFY(!s.explicitAt(30)); w.redo(); QCOMPARE(s.annotations.at(30).front().fontSize,54.0);
        const QString path=temp.filePath("text-options.frvm"); w.m_projectPath=path;
        v->editText(a.id); v->m_textEdit->setPlainText("Saved while editing"); QVERIFY(w.saveProject(false));
        Project loaded; QString error; QVERIFY2(loaded.load(path,&error),qPrintable(error));
        const Annotation saved=*loaded.sources.front().renderable(35).front().annotation;
        QCOMPARE(saved.text,QString("Saved while editing")); QCOMPARE(saved.fontFamily,QString("Arial")); QVERIFY(saved.bold); QVERIFY(saved.italic);
        w.m_dirty=false;
    }
    void folderImportAndPersistence() {
        MainWindow w;
        const QString directory = temp.filePath("sources-bin");
        QVERIFY(QDir().mkpath(directory + "/nested"));
        QVERIFY(QFile::copy(QApplication::applicationDirPath()+"/qa-media.mp4", directory+"/clip.mp4"));
        QImage image(64,48,QImage::Format_RGB32); image.fill(Qt::green);
        QVERIFY(image.save(directory+"/anim_001.png")); QVERIFY(image.save(directory+"/anim_002.png"));
        QVERIFY(image.save(directory+"/unrelated.png"));
        QVERIFY(image.save(directory+"/nested/still.png"));
        QFile ignored(directory+"/readme.txt"); QVERIFY(ignored.open(QIODevice::WriteOnly)); ignored.write("ignore"); ignored.close();
        drop(w.m_clipList->viewport(), directory);
        QCOMPARE(w.m_project.sources.size(),4);
        QCOMPARE(w.m_project.folders.size(),2);
        QVERIFY(w.m_project.folders.contains("sources-bin"));
        int sequences=0;
        for (const auto& source : w.m_project.sources) {
            QVERIFY(!source.folder.isEmpty());
            if (source.media.frames.size()==2) ++sequences;
        }
        QCOMPARE(sequences,1);
        const QString path=temp.filePath("folders.frvm"); QString error;
        QVERIFY2(w.m_project.save(path,&error),qPrintable(error));
        Project loaded; QVERIFY2(loaded.load(path,&error),qPrintable(error));
        QCOMPARE(loaded.folders,w.m_project.folders);
        for (int i=0;i<loaded.sources.size();++i) QCOMPARE(loaded.sources[i].folder,w.m_project.sources[i].folder);
        QVERIFY(w.findChild<QToolButton*>("SourcesFolderButton"));
        w.m_dirty=false;
    }
    void unifiedPenAndStraightStrokes() {
        QVERIFY(window->m_penButton);
        QVERIFY(!window->m_toolButtons.contains(Tool::Pen));
        QVERIFY(!window->m_toolButtons.contains(Tool::Highlighter));
        for (Tool tool : {Tool::Pencil,Tool::Pen,Tool::Highlighter}) {
            window->setTool(tool); QVERIFY(window->m_penButton->isChecked());
            QCOMPARE(window->m_penStyle,tool);
        }
        Viewport v; v.resize(800,500); v.setMediaSize({320,180});
        auto image=std::make_shared<QImage>(320,180,QImage::Format_RGB32); image->fill(Qt::black); v.setFrame(VideoFrame::share(*image));
        v.show(); QVERIFY(QTest::qWaitForWindowExposed(&v));
        for (Tool tool : {Tool::Pencil,Tool::Pen,Tool::Highlighter}) {
            v.setTool(tool); QSignalSpy created(&v,&Viewport::annotationCreated);
            drag(v,{100,120},{350,230},Qt::ShiftModifier);
            QCOMPARE(created.count(),1);
            const Annotation a=qvariant_cast<Annotation>(created.front().front());
            QCOMPARE(a.points.size(),2);
            QVERIFY(QLineF(a.points[0],a.points[1]).length()>.1);
        }
        QPointingDevice pen("Straight pen", 12, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        v.setTool(Tool::Pen); QSignalSpy created(&v,&Viewport::annotationCreated);
        auto send=[&](QEvent::Type type,QPointF point,qreal pressure) {
            QTabletEvent event(type,&pen,point,point,pressure,0,0,0,0,0,Qt::ShiftModifier,
                               type==QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                               type==QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
            QApplication::sendEvent(&v,&event); QVERIFY(event.isAccepted());
        };
        send(QEvent::TabletPress,{100,100},.2); send(QEvent::TabletMove,{200,250},.5);
        send(QEvent::TabletMove,{300,140},.9); send(QEvent::TabletRelease,{300,140},0);
        QCOMPARE(created.count(),1);
        const Annotation a=qvariant_cast<Annotation>(created.front().front());
        QCOMPARE(a.points.size(),2); QCOMPARE(a.pressures.size(),2);
        QVERIFY(a.pressures.front()<a.pressures.back());
        window->setTool(Tool::Select);
    }
    void annotationsOutsideVideo() {
        Viewport v; v.resize(900,650); v.setMediaSize({320,180});
        auto image=std::make_shared<QImage>(320,180,QImage::Format_RGB32); image->fill(Qt::black); v.setFrame(VideoFrame::share(*image));
        v.setCompareFrame(VideoFrame::share(*image),{320,180}); v.setCompareMode(CompareMode::SideBySide);
        Annotation a=rect("outside"); a.x=.2; a.y=-.4; a.w=.2; a.h=.15; a.size=6;
        v.setAnnotations({{a,false}}); v.setCompareAnnotations({{a,false}});
        v.show(); QVERIFY(QTest::qWaitForWindowExposed(&v)); QTest::qWait(100);
        const QImage rendered=v.grabFramebuffer();
        for (int side : {0,1}) {
            const QPointF point=v.annoToWidget(side).map(QPointF(96,-72));
            const QPoint px=(point*v.devicePixelRatioF()).toPoint();
            QVERIFY(rendered.rect().contains(px));
            const QColor color=rendered.pixelColor(px);
            QVERIFY2(color.red()>180 && color.green()<100,qPrintable(QString("side %1 at %2,%3: %4").arg(side).arg(px.x()).arg(px.y()).arg(color.name())));
            QCOMPARE(v.sideAt(point),side);
            v.setActiveSide(side);
            QCOMPARE(v.hitItem({96,-72}),QString("outside"));
        }
        Source source; source.annotations[0]={a};
        const QImage padded=composeFrame(*image,source,0,{640,360},true,QString(),false,0,.5);
        QVERIFY(padded.pixelColor(256,18).red()>180);
        QVERIFY(rendered.save(QApplication::applicationDirPath()+"/qa-outside-compare.png"));
        QVERIFY(padded.save(QApplication::applicationDirPath()+"/qa-outside-export.png"));
    }
    void hotkeyConflictSaveAndReload() {
        QVERIFY(HotkeyManager::conflict({QKeySequence("F6"),QKeySequence("F6")})=="F6");
        QVERIFY(!HotkeyManager::conflict({QKeySequence("Ctrl+K"),QKeySequence("Ctrl+K, Ctrl+C")}).isEmpty());
        QVERIFY(HotkeyManager::conflict({QKeySequence(),QKeySequence("F6"),QKeySequence("F7")}).isEmpty());
        HotkeyManager manager(window->m_hotkeys,window.get());
        auto* editor=manager.findChild<QKeySequenceEdit*>("P"); QVERIFY(editor);
        editor->setKeySequence(QKeySequence("F6"));
        auto* buttons=manager.findChild<QDialogButtonBox*>(); QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Save)->click();
        QCOMPARE(manager.result(),int(QDialog::Accepted));
        QCOMPARE(window->m_hotkeys["P"]->shortcut(),QKeySequence("F6"));
        QSettings().setValue("hotkeys/Shift+Tab", "F7");
        MainWindow reloaded; QCOMPARE(reloaded.m_hotkeys["P"]->shortcut(),QKeySequence("F6"));
        QCOMPARE(reloaded.m_hotkeys["Shift+Tab"]->shortcut(),QKeySequence("F7"));
        reloaded.resize(1000,700); reloaded.show(); QVERIFY(QTest::qWaitForWindowExposed(&reloaded));
        reloaded.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&reloaded));
        QTest::keyClick(reloaded.m_view,Qt::Key_F6); QCOMPARE(reloaded.m_view->tool(),Tool::Pencil);
        window->m_hotkeys["P"]->setShortcut(QKeySequence("P")); QSettings().remove("hotkeys/P");
        QSettings().remove("hotkeys/Shift+Tab");
        reloaded.m_dirty=false;
    }
    void exportPresetSaveReloadDelete() {
        ExportDialog::Ranges ranges; ranges.total=24; ranges.fps=24; ranges.clipEnd=23; ranges.suggestedName="test";
        ExportDialog dialog(ranges);
        dialog.applyPreset({{"format",4},{"fps",30},{"scale",2},{"margin",3},{"notes",true},{"tracks",false},{"audio",false}});
        QCOMPARE(dialog.format(),ExportJob::Format::PngSequence); QCOMPARE(dialog.margin(),.5); QCOMPARE(dialog.fps(),30.0);
        auto* save=dialog.findChild<QPushButton*>("SaveExportPreset"); QVERIFY(save);
        QTimer::singleShot(20,[] {
            for (QWidget* widget : QApplication::topLevelWidgets()) if (auto* input=qobject_cast<QInputDialog*>(widget)) { input->setTextValue("Regression preset"); input->accept(); }
        });
        save->click();
        ExportDialog reloaded(ranges);
        int index=reloaded.m_presets->findText("Regression preset"); QVERIFY(index>=0);
        reloaded.m_presets->setCurrentIndex(index);
        reloaded.m_presets->activated(index);
        QCOMPARE(reloaded.presetValues(),dialog.presetValues());
        reloaded.show(); QTest::qWait(50);
        QVERIFY(reloaded.grab().save(QApplication::applicationDirPath()+"/qa-export-presets.png"));
        for (auto* button : reloaded.findChildren<QPushButton*>()) if (button->text()=="Delete") button->click();
        QVERIFY(reloaded.m_presets->findText("Regression preset")<0);
    }
    void canvasAnimationUndoSavePlaybackAndExport() {
        MainWindow w; w.resize(1000,750); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.createCanvas("Animation",{320,180},12,24);
        QCOMPARE(w.m_seq.total,int64_t(12));
        QCOMPARE(w.m_project.sources.front().media.kind,MediaRef::Kind::Canvas);
        QTRY_VERIFY(w.m_view->m_a.frame);
        const QString id=w.m_project.sources.front().id;
        w.onAnnotationCreated(rect("frame0"));
        w.goToFrame(3); w.onAnnotationCreated(rect("frame3"));
        auto* source=w.m_project.find(id); QVERIFY(source);
        QCOMPARE(source->annotations.at(3).size(),1); QCOMPARE(source->annotations.at(3).front().id,QString("frame3"));
        w.undo(); QVERIFY(!source->explicitAt(3)); w.redo(); QVERIFY(source->explicitAt(3));
        const QString path=temp.filePath("canvas.frvm"); QString error;
        QVERIFY2(w.m_project.save(path,&error),qPrintable(error)); w.m_dirty=false;
        QVERIFY2(w.loadProject(path),qPrintable(error));
        QCOMPARE(w.m_seq.total,int64_t(12));
        QVERIFY(w.m_project.sources.front().media.exists());
        w.goToFrame(0); w.togglePlay(); QTRY_VERIFY_WITH_TIMEOUT(w.m_current>0,3000); w.stopPlayback();
        w.setTool(Tool::Pen); w.goToFrame(4);
        QTRY_VERIFY(w.m_media.server(id)->frame(4));
        const auto xf=w.m_view->annoXf();
        drag(*w.m_view,xf.map(QPointF(20,30)),xf.map(QPointF(150,90)),Qt::ShiftModifier);
        QVERIFY(w.m_project.find(id)->explicitAt(4));
        ExportJob job; job.project=w.m_project; job.sequence=w.m_seq; job.first=0; job.last=5; job.fps=24;
        job.format=ExportJob::Format::PngSequence; job.output=temp.filePath("canvas/frame");
        std::atomic<int64_t> done{0}; std::atomic<bool> cancel{false};
        QVERIFY2(runExport(job,done,cancel,&error),qPrintable(error)); QCOMPARE(done.load(),int64_t(6));
        QImage rendered(temp.filePath("canvas/frame_0003.png")); QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.size(),QSize(320,180)); QVERIFY(rendered.pixelColor(39,45).red()>100);
        QVERIFY(rendered.save(QApplication::applicationDirPath()+"/qa-canvas-export.png"));
        job.format=ExportJob::Format::Mp4H264; job.output=temp.filePath("canvas.mp4"); done=0;
        QVERIFY2(runExport(job,done,cancel,&error),qPrintable(error));
        VideoDecoder decoder; QVERIFY2(decoder.open(job.output,&error),qPrintable(error));
        QCOMPARE(decoder.info().frameCount,int64_t(6)); QVERIFY(decoder.decode(3,rendered,&error));
        w.goToFrame(4); QTest::qWait(150);
        QVERIFY(!w.m_view->items().isEmpty());
        const QImage viewport=w.m_view->grabFramebuffer();
        QVERIFY(viewport.save(QApplication::applicationDirPath()+"/qa-canvas-viewport.png"));
        const QPoint point=(w.m_view->annoXf().map(QPointF(80,58))*w.m_view->devicePixelRatioF()).toPoint();
        QVERIFY(viewport.pixelColor(point).red()>100);
        QVERIFY(w.grab().save(QApplication::applicationDirPath()+"/qa-canvas-application.png"));
        w.m_dirty=false;
    }
    void endlessScrubbingAndConsole() {
        MainWindow w; w.createCanvas("Scrub",{64,48},24,24);
        w.m_endlessScrubbing=true;
        w.goToFrame(23); w.scrubRelative(2); QCOMPARE(w.m_current,int64_t(1));
        w.scrubRelative(-3); QCOMPARE(w.m_current,int64_t(22));
        w.m_soloLoop=true; w.m_soloIn=5; w.m_soloOut=9;
        w.goToFrame(9); w.scrubRelative(1); QCOMPARE(w.m_current,int64_t(5));
        w.scrubRelative(-1); QCOMPARE(w.m_current,int64_t(9));
        w.m_endlessScrubbing=false; w.goToFrame(23); w.scrubRelative(5); QCOMPARE(w.m_current,int64_t(23));
        QSignalSpy scrub(w.m_timeline,&Timeline::scrubBy);
        QMouseEvent press(QEvent::MouseButtonPress,{120,30},{120,30},Qt::MiddleButton,Qt::MiddleButton,Qt::NoModifier);
        QApplication::sendEvent(w.m_timeline,&press);
        QMouseEvent move(QEvent::MouseMove,{180,30},{180,30},Qt::NoButton,Qt::MiddleButton,Qt::NoModifier);
        QApplication::sendEvent(w.m_timeline,&move); QCOMPARE(scrub.count(),1); QCOMPARE(scrub.front().front().toInt(),10);
        QTest::mouseRelease(w.m_timeline,Qt::MiddleButton);
        w.runConsoleCommand("frame 7"); QCOMPARE(w.m_current,int64_t(7));
        w.runConsoleCommand("tool highlighter"); QCOMPARE(w.m_view->tool(),Tool::Highlighter);
        w.runConsoleCommand("status"); w.runConsoleCommand("help");
        auto* log=w.m_console->findChild<QPlainTextEdit*>("ConsoleLog"); QVERIFY(log);
        QVERIFY(log->toPlainText().contains("24")); QVERIFY(log->toPlainText().contains("help | status"));
        w.resize(1100,800); w.show(); w.m_console->show(); QTest::qWait(100);
        QVERIFY(w.grab().save(QApplication::applicationDirPath()+"/qa-console.png"));
        w.m_dirty=false;
    }
    void unifiedProjectDialog() {
        MainWindow w; w.createCanvas("Old",{64,48},6,24); w.m_dirty=false;
        const QString old=w.m_project.sources.front().id;
        QTimer::singleShot(30, [] {
            auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(d);
            QCOMPARE(d->objectName(),QString("NewProjectDialog"));
            auto* type=d->findChild<QComboBox*>("ProjectType"); QVERIFY(type); type->setCurrentIndex(1);
            d->findChild<QLineEdit*>("ProjectName")->setText("New animation");
            d->findChild<QSpinBox*>("CanvasWidth")->setValue(320);
            d->findChild<QSpinBox*>("CanvasHeight")->setValue(180);
            d->findChild<QSpinBox*>("CanvasFrames")->setValue(36);
            d->findChild<QDoubleSpinBox*>("CanvasFps")->setValue(30);
            QVERIFY(d->grab().save(QApplication::applicationDirPath()+"/qa-new-project.png")); d->accept();
        });
        w.newProject();
        QCOMPARE(w.m_project.sources.size(),1); QVERIFY(w.m_project.sources.front().id!=old);
        QCOMPARE(w.m_project.sources.front().media.width,320); QCOMPARE(w.m_seq.total,int64_t(36));
        QCOMPARE(w.viewFps(),30.0); QCOMPARE(w.m_project.timeline.name,QString("New animation"));
        const QString keep=w.m_project.sources.front().id;
        QTimer::singleShot(30, [] { auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(d); d->reject(); });
        w.newProject(); QCOMPARE(w.m_project.sources.front().id,keep); QVERIFY(w.m_dirty);
        w.m_dirty=false;
        QTimer::singleShot(30, [] { auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(d); d->accept(); });
        w.newProject(); QVERIFY(w.m_project.sources.isEmpty()); QCOMPARE(w.m_seq.total,int64_t(0));
    }
    void outsideStillExportAndOptions() {
        QImage frame(320,180,QImage::Format_RGBA8888); frame.fill(Qt::black);
        Source source; source.media.fileName="Still";
        Annotation a=rect("outside"); a.x=-.5; a.y=-.4; a.angle=.5; a.stretchX=1.7; a.size=18;
        source.annotations[0]={a};
        StillFrameOptions o; o.background=Qt::transparent; o.video=false; o.tracks=false;
        const QRectF bounds=stillFrameBounds(frame.size(),source,0,o);
        QVERIFY(bounds.left() < -100); QVERIFY(bounds.top()< -40);
        const QImage out=composeStillFrame(frame,source,0,o); QVERIFY(!out.isNull());
        QCOMPARE(out.size(),bounds.size().toSize()); QCOMPARE(out.pixelColor(0,0).alpha(),0);
        int red=0; for(int y=0;y<out.height();++y) for(int x=0;x<out.width();++x) if(out.pixelColor(x,y).red()>180) ++red;
        QVERIFY(red>400); QVERIFY(out.save(QApplication::applicationDirPath()+"/qa-still-auto-bounds.png"));
        o.includeOutside=false; QCOMPARE(composeStillFrame(frame,source,0,o).size(),frame.size());
        o.includeOutside=true; o.notes=false; o.padding=0;
        QCOMPARE(composeStillFrame(frame,source,0,o).size(),frame.size());
        o.scale=2; QCOMPARE(composeStillFrame(frame,source,0,o).size(),QSize(640,360));
        Annotation far=a; far.tx=1000000; source.annotations[0]={far}; o.notes=true;
        QVERIFY(composeStillFrame(frame,source,0,o).isNull());
        source.annotations[0]={a};
        FrameExportDialog dialog(frame,source,0);
        auto* bg=dialog.findChild<QComboBox*>("FrameBackground"); auto* format=dialog.findChild<QComboBox*>("FrameFormat");
        bg->setCurrentIndex(2); QCOMPARE(dialog.options().background.alpha(),0);
        format->setCurrentIndex(1); QCOMPARE(dialog.options().background.alpha(),255); QCOMPARE(dialog.format(),QByteArray("jpg"));
        dialog.findChild<QDoubleSpinBox*>("FrameScale")->setValue(50); QCOMPARE(dialog.options().scale,.5);
        dialog.show(); QTest::qWait(50); QVERIFY(dialog.grab().save(QApplication::applicationDirPath()+"/qa-frame-options.png"));
    }
    void stillExportUsesOptionsAndCommitsText() {
        MainWindow w; w.resize(1000,700); w.show(); w.createCanvas("Frame",{320,180},12,24);
        QTRY_VERIFY(w.m_view->m_a.frame);
        Annotation a=rect("outside"); a.x=-.6; a.y=.2; w.onAnnotationCreated(a);
        Annotation text=rect("text-commit"); text.type="text"; text.text="Before"; w.onAnnotationCreated(text);
        w.m_view->editText(text.id); QVERIFY(w.m_view->m_textEdit); w.m_view->m_textEdit->setPlainText("Committed current text");
        QString path=temp.filePath("export-with-outside.png");
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true);
        QTimer driver;
        connect(&driver,&QTimer::timeout,&w,[&] {
            auto* modal=QApplication::activeModalWidget();
            if(auto* options=dynamic_cast<FrameExportDialog*>(modal)) {
                options->findChild<QComboBox*>("FrameFormat")->setCurrentIndex(0);
                options->findChild<QCheckBox*>("FrameIncludeOutside")->setChecked(true); options->accept();
            } else if(auto* file=qobject_cast<QFileDialog*>(modal)) {
                file->setDirectory(temp.path());
                if (auto* edit=file->findChild<QLineEdit*>("fileNameEdit")) edit->setText(QFileInfo(path).fileName());
                QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);
            }
        });
        driver.start(30); w.exportCurrentFrame(); driver.stop();
        QImage out(path); QVERIFY(!out.isNull()); QVERIFY(out.width()>320);
        QVERIFY(!w.m_view->isEditingText());
        QCOMPARE(w.m_project.sources.front().annotations.at(0).back().text,QString("Committed current text"));
        const QString aId=w.m_project.sources.front().id;
        w.createCanvas("B",{160,120},12,24);
        const QString bId=w.m_project.sources.back().id;
        w.chooseA(aId); w.chooseB(bId);
        QTRY_VERIFY(w.m_view->m_b.frame); w.m_view->setActiveSide(1);
        Annotation b=rect("outside-b"); b.x=-.7; w.onAnnotationCreated(b);
        path=temp.filePath("export-active-b.png");
        driver.start(30); w.exportCurrentFrame(); driver.stop();
        const QImage bOut(path); QVERIFY(!bOut.isNull()); QVERIFY(bOut.width()>160); QCOMPARE(bOut.height(),152);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,false);
        w.m_dirty=false;
    }
    void stillExportTransformedImagesTextThickPensAndFormats() {
        QImage frame(320,180,QImage::Format_RGBA8888); frame.fill(Qt::black);
        QImage blue(12,12,QImage::Format_RGBA8888); blue.fill(Qt::blue);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(blue.save(&buffer,"PNG"));
        Annotation picture=rect("picture"); picture.type="image"; picture.x=1.2; picture.y=.3; picture.w=.4; picture.h=.3;
        picture.angle=.4; picture.stretchX=1.7; picture.stretchY=1.4;
        picture.src="data:image/png;base64,"+QString::fromLatin1(bytes.toBase64());
        Annotation text=rect("text"); text.type="text"; text.x=.5; text.y=1.6; text.text="Outside text"; text.fontSize=30;
        Annotation pen=rect("pen"); pen.type="stroke"; pen.points={{-.5,-.8},{-.2,-.5}}; pen.size=120; pen.penStyle="pen"; pen.stretchX=1.4;
        Source source; source.annotations[0]={picture,text,pen};
        StillFrameOptions o; o.background=Qt::transparent; o.video=false; o.tracks=false;
        const QImage out=composeStillFrame(frame,source,0,o); QVERIFY(!out.isNull());
        int bluePixels=0,redPixels=0;
        for(int y=0;y<out.height();++y) for(int x=0;x<out.width();++x) {
            const auto c=out.pixelColor(x,y); if(c.blue()>200 && c.red()<40) ++bluePixels; if(c.red()>200) ++redPixels;
            if(x==0 || y==0 || x==out.width()-1 || y==out.height()-1) QCOMPARE(c.alpha(),0);
        }
        QVERIFY(bluePixels>1000); QVERIFY(redPixels>2000);
        QVERIFY(out.save(QApplication::applicationDirPath()+"/qa-still-outside-content.png"));
        for(const QByteArray format : {QByteArray("png"),QByteArray("jpg"),QByteArray("tif")}) {
            const QString path=temp.filePath("still."+QString::fromLatin1(format));
            QImageWriter writer(path,format); writer.setQuality(90); QVERIFY2(writer.write(out),qPrintable(writer.errorString()));
            const QImage loaded(path); QCOMPARE(loaded.size(),out.size());
            if(format!="jpg") QCOMPARE(loaded.pixelColor(0,0).alpha(),0);
        }
    }
    void playbackPrefetchFitsCacheAndStopsWhenIdle() {
        auto cache=std::make_shared<FrameCache>(); cache->setBudget(qint64(256)<<20);
        auto source=std::make_unique<CountingUhdSource>(); auto* counter=source.get();
        FrameServer server(std::move(source),cache,0);
        server.request(0,true,0,299);
        QTRY_VERIFY_WITH_TIMEOUT(server.isCached(0),3000);
        QTest::qWait(150);
        const int settled=counter->decoded.load();
        QTest::qWait(150);
        QCOMPARE(counter->decoded.load(),settled); QVERIFY(settled<=8); QVERIFY(server.isCached(0));
        for (int i=1;i<20;++i) {
            server.request(i,true,0,299); QTRY_VERIFY_WITH_TIMEOUT(server.isCached(i),3000);
        }
        server.setIdle(); QTest::qWait(50); cache->clear();
        const int idle=counter->decoded.load(); QTest::qWait(100); QCOMPARE(counter->decoded.load(),idle);
    }
    void reviewRangeSemantics() {
        Source s; s.annotations[10]={rect()}; s.annotations[20]={}; s.annotations[30]={rect()};
        const auto ranges=s.reviewRanges(40);
        QCOMPARE(ranges.size(),2); QCOMPARE(ranges[0].first,10); QCOMPARE(ranges[0].last,20);
        QVERIFY(!inReviewRange(ranges,9)); QVERIFY(inReviewRange(ranges,19)); QVERIFY(inReviewRange(ranges,20));
        QVERIFY(!inReviewRange(ranges,21)); QVERIFY(inReviewRange(ranges,39));
        QCOMPARE(s.reviewRanges(0).size(),0);
        Track t; t.points[2]={.1,.1}; t.points[6]={.2,.2}; t.keys={2}; s.tracks={t};
        const auto tracked=s.reviewRanges(40); QVERIFY(inReviewRange(tracked,4)); QVERIFY(!inReviewRange(tracked,7));
        QCOMPARE(s.reviewRanges(5).back().last,4);
    }
    void reviewBarsRenderedColorsAndBoundaries() {
        ClipBar bar; bar.resize(800,48); bar.setClip("A very long source name for layout verification.mp4",100,10,100);
        bar.setReviewMarks({199,110,120,110}, {115});
        const auto colorCount=[](const QImage& image, QColor wanted, QRect area) {
            int count=0; for(int y=area.top();y<=area.bottom();++y) for(int x=area.left();x<=area.right();++x)
                if(image.pixelColor(x,y)==wanted) ++count;
            return count;
        };
        QImage image=bar.grab().toImage();
        QVERIFY(colorCount(image,Pal::review,QRect(119,30,6,10))>0);
        QCOMPARE(colorCount(image,Pal::review,QRect(130,30,50,10)),0); // no fill between keys
        QVERIFY(colorCount(image,Pal::reviewActive,QRect(52,0,696,16))>0);
        QVERIFY(colorCount(image,Pal::review,QRect(744,30,8,10))>0); // final single-frame edit remains visible
        bar.setClip("clip",100,15,100); image=bar.grab().toImage();
        QCOMPARE(colorCount(image,Pal::reviewActive,QRect(52,0,696,16)),0);
        bar.setClip("clip",100,20,100); image=bar.grab().toImage();
        QVERIFY(colorCount(image,Pal::reviewActive,QRect(52,0,696,16))>0);
        bar.setClip("one",1,0); bar.setReviewMarks({0},{}); image=bar.grab().toImage();
        QVERIFY(colorCount(image,Pal::reviewActive,QRect(40,0,100,16))>0);
        QVERIFY(image.save(QApplication::applicationDirPath()+"/qa-review-single-frame.png"));
        Timeline bottom; bottom.resize(800,60); bottom.setFrameCount(100); bottom.setMarks({20,10,10},{}); bottom.setCurrent(10);
        image=bottom.grab().toImage(); QVERIFY(colorCount(image,Pal::reviewActive,QRect(90,0,620,16))>0);
        QCOMPARE(colorCount(image,Pal::review,QRect(164,35,40,4)),0);
        bottom.setCurrent(15); image=bottom.grab().toImage(); QCOMPARE(colorCount(image,Pal::reviewActive,QRect(90,0,620,16)),0);
        bar.setClip("empty",0,0); image=bar.grab().toImage(); QCOMPARE(colorCount(image,Pal::review,QRect(0,0,800,48)),0);
    }
    void reviewNavigationLoopUndoAndClipChanges() {
        MainWindow w; w.resize(1200,850); w.show(); w.createCanvas("Review A",QSize(320,180),60,24);
        auto* a=&w.m_project.sources.front(); a->annotations[5]={rect()}; a->annotations[15]={}; a->annotations[30]={rect()};
        w.refreshMarks(); w.goToFrame(5); w.jumpReviewKey(1); QCOMPARE(w.m_current,15); w.jumpReviewKey(-1); QCOMPARE(w.m_current,5);
        w.loopReviewRange(8); QCOMPARE(w.playRange().first,5); QCOMPARE(w.playRange().second,14);
        w.loopReviewRange(15); QCOMPARE(w.playRange().first,15); QCOMPARE(w.playRange().second,15);
        w.clearLoop(); w.goToFrame(35); w.clearCurrentFrame(); QVERIFY(inReviewRange(w.m_reviewRanges,35)); QVERIFY(!inReviewRange(w.m_reviewRanges,36));
        w.undo(); QVERIFY(inReviewRange(w.m_reviewRanges,36)); w.redo(); QVERIFY(!inReviewRange(w.m_reviewRanges,36));
        w.createCanvas("Review B",QSize(320,180),40,24); w.m_project.sources.back().annotations[3]={rect()};
        w.setSolo(QString()); w.rebuildSequence(); w.goToFrame(40); w.jumpReviewKey(1); QCOMPARE(w.m_current,63);
        w.loopReviewRange(65); QCOMPARE(w.playRange().first,63); QCOMPARE(w.playRange().second,99);
        w.clearLoop(); w.goToFrame(5); QTest::qWait(80);
        QVERIFY(w.grab().save(QApplication::applicationDirPath()+"/qa-review-ranges.png"));
        QVERIFY(w.m_project.save(QApplication::applicationDirPath()+"/qa-review-demo.frvm",nullptr));
        w.m_dirty=false;
    }
    void compareReviewRangesRespectDifferentFps() {
        MainWindow w; w.createCanvas("A",QSize(320,180),24,24); const QString a=w.m_project.sources.front().id;
        w.createCanvas("B",QSize(320,180),96,48); const QString b=w.m_project.sources.back().id;
        w.m_project.find(b)->annotations[24]={rect()}; w.m_project.find(b)->annotations[48]={};
        w.setSolo(a); w.setCompareSource(b); w.m_compareByTime=true; w.rebuildSequence(); w.refreshMarks();
        QVERIFY(!inReviewRange(w.m_reviewRanges,11)); QVERIFY(inReviewRange(w.m_reviewRanges,12));
        QVERIFY(inReviewRange(w.m_reviewRanges,24)); QVERIFY(!inReviewRange(w.m_reviewRanges,25));
        w.goToFrame(30); QSignalSpy seek(w.m_clipBar,&ClipBar::scrubbed);
        QTest::mouseClick(w.m_clipBar,Qt::LeftButton,Qt::NoModifier,QPoint(w.m_clipBar->width()-52,30));
        QCOMPARE(w.m_current,47); // top bar still moves past the end of the shorter A
        w.setCompareMode(CompareMode::Off); QVERIFY(w.m_reviewRanges.isEmpty()); w.m_dirty=false;
    }
    void steppedPlaybackHoldsTimingSeeksAndLoops() {
        MainWindow w; w.createCanvas("Stepped",QSize(320,180),24,24); auto& s=w.m_project.sources.front();
        w.togglePlay(true); QVERIFY(!w.m_playing); // requires a bookmark in range
        s.bookmarks[2]={}; s.bookmarks[7]={}; s.bookmarks[15]={}; w.refreshMarks();
        auto* srv=w.m_media.server(s.id); srv->request(0,true,0,23);
        QTRY_VERIFY_WITH_TIMEOUT(srv->isCached(15),3000);
        w.goToFrame(5); w.togglePlay(true); w.m_playTimer.stop();
        QVERIFY(w.m_playing); QVERIFY(w.m_stepped); QCOMPARE(w.m_current,2); QVERIFY(!w.m_audio.isPlaying());
        const auto tick=[&] { w.m_nextDueNs=0; w.onPlayTick(); };
        for(int i=0;i<4;++i) tick(); QCOMPARE(w.m_current,2); tick(); QCOMPARE(w.m_current,7);
        w.goToFrame(12); QCOMPARE(w.m_current,7); QCOMPARE(w.m_steppedPosition,7);
        for(int i=0;i<8;++i) tick(); QCOMPARE(w.m_current,15);
        for(int i=0;i<9;++i) tick(); QCOMPARE(w.m_current,2);
        w.stopPlayback(); QVERIFY(!w.m_stepped);
        w.m_soloLoop=true; w.m_soloIn=4; w.m_soloOut=12;
        QCOMPARE(w.playbackBookmarks(),QVector<qint64>{7}); w.togglePlay(true); w.m_playTimer.stop(); QCOMPARE(w.m_current,7);
        for(int i=0;i<20;++i) tick(); QCOMPARE(w.m_current,7);
        s.bookmarks.clear(); tick(); QVERIFY(!w.m_playing);
        w.m_soloLoop=false; s.bookmarks[2]={}; w.goToFrame(0); w.togglePlay(); w.m_playTimer.stop(); QVERIFY(!w.m_stepped); w.stopPlayback();
        w.m_dirty=false;
    }
    void audioWaitsForLoopPictureAndStopsAfterPause() {
        MainWindow w; w.createCanvas("Audio loop",QSize(64,48),24,24);
        if (!w.m_audio.available()) QSKIP("No Windows audio output device");
        const QString id=w.m_project.sources.front().id;
        auto slow=std::make_unique<SlowLoopSource>(); auto* delay=slow.get(); w.m_media.add(id,std::move(slow));
        auto pcm=std::make_shared<AudioClip>(); pcm->pcm.resize(AudioClip::kRate*4);
        for(size_t i=0;i<pcm->pcm.size();++i) pcm->pcm[i]=int16_t(1200*std::sin(i*.03));
        w.m_audioClips[id]=pcm; w.m_audio.setMuted(true);
        auto* srv=w.m_media.server(id); srv->request(0,true,0,23);
        QTRY_VERIFY_WITH_TIMEOUT(srv->isCached(23),3000);
        w.goToFrame(23); w.togglePlay(); w.m_playTimer.stop(); QVERIFY(w.m_audio.isPlaying());
        // Make the first image unavailable on wrap, as with an evicted/long-GOP frame.
        srv->setIdle(); delay->delayFirst=true; w.m_media.clearCache(); w.m_shownKey=id+":23"; w.m_current=23;
        w.m_audio.play(pcm,1.1,1.0); w.onPlayTick();
        QCOMPARE(w.m_current,0); QVERIFY(!w.m_audio.isPlaying());
        QTRY_VERIFY_WITH_TIMEOUT(w.m_shownKey==id+":0",3000);
        QVERIFY(w.m_audio.isPlaying());
        w.stopPlayback(); QVERIFY(!w.m_audio.isPlaying());
        w.onFrameReady(id,0); w.syncAudio(true); QTest::qWait(80); QVERIFY(!w.m_audio.isPlaying());
        // Stop is idempotent and also shuts down an orphaned output stream.
        w.m_audio.play(pcm,0,1); QVERIFY(w.m_audio.isPlaying()); w.m_scrubSoundPending=true;
        w.stopPlayback(); QVERIFY(!w.m_audio.isPlaying()); QVERIFY(!w.m_scrubSoundPending);
        w.m_dirty=false;
    }
    void realVideoAudioWrapAndHome() {
        MainWindow w; w.resize(1100,800); w.show();
        if (!w.m_audio.available()) QSKIP("No Windows audio output device");
        const QString video=QApplication::applicationDirPath()+"/qa-audio-loop.mp4";
        QVERIFY(QFileInfo::exists(video)); w.openPaths({video});
        QCOMPARE(w.m_project.sources.size(),1); const QString id=w.m_project.sources.front().id;
        QTRY_VERIFY_WITH_TIMEOUT(w.m_audioClips.count(id) && w.m_audioClips.at(id),5000);
        auto* srv=w.m_media.server(id); srv->request(0,true,0,47);
        QTRY_VERIFY_WITH_TIMEOUT(srv->isCached(47),5000);
        w.m_audio.setMuted(true); w.togglePlay();
        QTRY_VERIFY_WITH_TIMEOUT(w.m_current>36,3000);
        QTRY_VERIFY_WITH_TIMEOUT(w.m_current<12,1500);
        QVERIFY(w.m_audio.isPlaying());
        QAction* home=nullptr;
        for(auto* action:w.findChildren<QAction*>()) if(action->shortcut()==QKeySequence(Qt::Key_Home)) home=action;
        QVERIFY(home); home->trigger(); QCOMPARE(w.m_current,0); QVERIFY(!w.m_playing); QVERIFY(!w.m_audio.isPlaying());
        QTest::qWait(120); QCOMPARE(w.m_current,0); QVERIFY(!w.m_audio.isPlaying());
        w.m_dirty=false;
    }
    void playbackStartupMustNotSkipWhenDisabled() {
        MainWindow w; w.createCanvas("Startup clock",QSize(64,48),480,240);
        if (!w.m_audio.available()) QSKIP("No Windows audio output device");
        const QString id=w.m_project.sources.front().id;
        auto pcm=std::make_shared<AudioClip>(); pcm->pcm.resize(AudioClip::kRate*4,0);
        w.m_audioClips[id]=pcm; w.m_audio.setMuted(true); w.m_realtime=false;
        auto* srv=w.m_media.server(id); srv->request(0,true,0,479);
        QTRY_VERIFY_WITH_TIMEOUT(srv->isCached(120),3000);
        w.goToFrame(0); w.togglePlay(); w.m_playTimer.stop(); QCOMPARE(w.m_current,0);
        QTest::qSleep(350); // simulate an expensive UI task before the first timer event
        const double heard=w.m_audio.position(); w.onPlayTick();
        qInfo("startup: audio=%.3fs, displayed frame=%lld, skip disabled",heard,(long long)w.m_current);
        QVERIFY2(w.m_current<=1,"Playback skipped initial frames even though Skip Frames to Keep Up is disabled");
        QVERIFY(w.m_audio.position()<.08); // large clock debt was re-anchored to the next displayed frame
        for(int i=0;i<20;++i) {
            const auto previous=w.m_current; QTest::qSleep(8); w.onPlayTick();
            QVERIFY(w.m_current>=previous && w.m_current<=previous+1);
        }
        // Opting in to real-time playback still permits frame skipping.
        w.stopPlayback(); w.goToFrame(0); w.m_realtime=true; w.togglePlay(); w.m_playTimer.stop();
        QTest::qSleep(160); w.onPlayTick(); QVERIFY(w.m_current>1);
        w.stopPlayback(); w.m_dirty=false;
    }
    void audioClockRestartsAtRequestedPosition() {
        AudioEngine engine; if (!engine.available()) QSKIP("No Windows audio output device"); engine.setMuted(true);
        auto pcm=std::make_shared<AudioClip>(); pcm->pcm.resize(AudioClip::kRate*6,0);
        for(int i=0;i<12;++i) {
            engine.snippet(pcm,.5,.045); QTest::qSleep(60); engine.stop(); QTest::qSleep(50);
            engine.play(pcm,0,1); const double initial=engine.position();
            QVERIFY2(initial>=0 && initial<.08,qPrintable(QString("restart %1 started at %2 seconds").arg(i).arg(initial)));
            QTest::qSleep(35); QVERIFY(engine.position()<.18); engine.stop();
        }
    }
    void audioClockRejectsInvalidAndStaleReports() {
        QCOMPARE(audioClockSeconds(AudioClockUnit::Samples,48000,48000,4).value(),1.0);
        QCOMPARE(audioClockSeconds(AudioClockUnit::Bytes,192000,48000,4).value(),1.0);
        QCOMPARE(audioClockSeconds(AudioClockUnit::Milliseconds,1000,48000,4).value(),1.0);
        QVERIFY(!audioClockSeconds(AudioClockUnit::Unsupported,1,48000,4));
        AudioClock clock;
        QVERIFY(clock.observe(3.333,.005,.06)<=.005); // an old 80-frame report cannot jump a fresh session
        const double before=clock.observe(.04,.05,.1);
        QVERIFY(clock.observe(.02,.06,.1)>=before); // device jitter never rewinds video
        clock.reset(); QCOMPARE(clock.last(),0.0);
        QCOMPARE(clock.observe(std::nullopt,.04,.02),.02); // fallback is bounded by submitted audio
        clock.reset(); QVERIFY(clock.observe(std::numeric_limits<double>::quiet_NaN(),.01,.06)<=.01);
    }
    void repeatedSpaceStartsFromDisplayedFrame() {
        MainWindow w; w.resize(1000,720); w.show(); w.createCanvas("Space startup",QSize(64,48),480,240);
        if (!w.m_audio.available()) QSKIP("No Windows audio output device");
        const QString id=w.m_project.sources.front().id;
        auto pcm=std::make_shared<AudioClip>(); pcm->pcm.resize(AudioClip::kRate*4,0);
        w.m_audioClips[id]=pcm; w.m_audio.setMuted(true); w.m_realtime=false;
        auto* srv=w.m_media.server(id); srv->request(0,true,0,479);
        QTRY_VERIFY_WITH_TIMEOUT(srv->isCached(300),3000);
        QVERIFY(QTest::qWaitForWindowActive(&w));
        for(int i=0;i<8;++i) {
            const qint64 start=i%2 ? 110 : 0; w.goToFrame(start); w.m_view->setFocus();
            QTest::keyClick(&w,Qt::Key_Space); QVERIFY(w.m_playing); w.m_playTimer.stop(); QCOMPARE(w.m_current,start);
            QTest::qSleep(120); w.onPlayTick(); QVERIFY(w.m_current>=start && w.m_current<=start+1);
            QTest::keyClick(&w,Qt::Key_Space); QVERIFY(!w.m_playing); QVERIFY(!w.m_audio.isPlaying());
        }
        w.m_soloLoop=true; w.m_soloIn=80; w.m_soloOut=120; w.goToFrame(0); w.togglePlay(); w.m_playTimer.stop();
        QCOMPARE(w.m_current,80); // an explicitly enabled In/Out range is still respected
        w.stopPlayback(); w.m_dirty=false;
    }
    // ======================================================================== 1.0
    void yuvFramesRenderOnGpuLikeCpu() {
        const QString video=QApplication::applicationDirPath()+"/qa-yuv-quads.mp4";
        QVERIFY2(QFileInfo::exists(video),"run tools/test-features.bat to create the test clips");
        VideoDecoder dec; QString error; QVERIFY2(dec.open(video,&error),qPrintable(error));
        VideoFrame f; QVERIFY2(dec.decodeFrame(6,f,&error),qPrintable(error));
        QVERIFY(f.isYuv());
        QCOMPARE(f.sizeInBytes(),qint64(320*180*3/2));                    // 4:2:0 = 1.5 bytes per pixel
        QVERIFY(f.sizeInBytes()*100<=qint64(320)*180*4*40);               // >= 60% less than RGBA
        const QImage cpu=f.toImage(); QCOMPARE(cpu.size(),QSize(320,180));
        Viewport v; v.resize(760,480); v.setMediaSize({320,180});
        v.setFrame(std::make_shared<const VideoFrame>(std::move(f)));
        v.show(); QVERIFY(QTest::qWaitForWindowExposed(&v)); v.setZoom(2.0); QTest::qWait(60);
        const QImage gpu=v.grabFramebuffer();
        QVERIFY(gpu.save(QApplication::applicationDirPath()+"/qa-gpu-yuv.png"));
        const QPointF centres[]={{80,45},{240,45},{80,135},{240,135}};
        const QColor expected[]={QColor(255,0,0),QColor(0,255,0),QColor(0,0,255),QColor(128,128,128)};
        int worstCpu=0, worstTruth=0;
        for (int i=0;i<4;++i) {
            const QColor g=gpu.pixelColor((v.annoXf().map(centres[i])*gpu.devicePixelRatio()).toPoint());
            const QColor c=cpu.pixelColor(centres[i].toPoint());
            worstCpu=std::max({worstCpu,std::abs(g.red()-c.red()),std::abs(g.green()-c.green()),std::abs(g.blue()-c.blue())});
            worstTruth=std::max({worstTruth,std::abs(g.red()-expected[i].red()),std::abs(g.green()-expected[i].green()),std::abs(g.blue()-expected[i].blue())});
        }
        QVERIFY2(worstCpu<=4,qPrintable(QString("GPU vs CPU conversion differs by %1").arg(worstCpu)));
        QVERIFY2(worstTruth<=10,qPrintable(QString("GPU colours off by %1").arg(worstTruth)));
        // Mirrored view: the picture flips with the overlay transform.
        v.setFlipHorizontal(true); QTest::qWait(40);
        const QImage flipped=v.grabFramebuffer();
        const QColor redSide=flipped.pixelColor((v.annoXf().map(centres[0])*flipped.devicePixelRatio()).toPoint());
        QVERIFY(redSide.red()>200 && redSide.green()<60);
        QVERIFY(v.annoXf().map(centres[0]).x()>v.annoXf().map(centres[1]).x());
    }
    void nvdecAndCpuDecodeTheSameFrames() {
        const QString video=QApplication::applicationDirPath()+"/qa-audio-loop.mp4";
        QString error;
        VideoDecoder::setHardwareDecodingEnabled(true);
        VideoDecoder hw; QVERIFY2(hw.open(video,&error),qPrintable(error));
        VideoDecoder::setHardwareDecodingEnabled(false);
        VideoDecoder sw; QVERIFY2(sw.open(video,&error),qPrintable(error));
        VideoDecoder::setHardwareDecodingEnabled(true);
        QVERIFY(!sw.hardwareActive());
        qInfo().noquote() << "hardware decoder:" << hw.info().decoderName;
        for (int64_t i : {int64_t(0),int64_t(17),int64_t(5),int64_t(47),int64_t(30)}) {   // random order forces seeks
            VideoFrame a, b; QVERIFY(hw.decodeFrame(i,a,&error)); QVERIFY(sw.decodeFrame(i,b,&error));
            QVERIFY2(std::abs(a.averageLuma()-b.averageLuma())<0.75,qPrintable(QString("frame %1 differs").arg(i)));
        }
    }
    void timelineTrimSplitMoveUndoAndSave() {
        MainWindow w; w.resize(1400,900); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.m_editorDock->isHidden());                                // hidden until Window > Timeline
        w.createCanvas("Shot A",{160,90},48,24);
        w.createCanvas("Shot B",{160,90},24,24);
        w.setSolo(QString());
        QCOMPARE(w.m_project.timeline.clips.size(),2); QCOMPARE(w.m_seq.total,int64_t(72));
        const QString idA=w.m_project.timeline.clips[0].sourceId;
        w.goToFrame(20); w.splitAtPlayhead();
        QCOMPARE(w.m_project.timeline.clips.size(),3);
        QCOMPARE(w.m_project.timeline.clips[0].out,19); QCOMPARE(w.m_project.timeline.clips[1].in,20);
        QCOMPARE(w.m_seq.total,int64_t(72)); QCOMPARE(w.locate(w.m_current).rawLocal,int64_t(20));
        const QString tail=w.m_project.timeline.clips[1].uid, shotB=w.m_project.timeline.clips[2].uid;
        // 1.1: without ripple a trim leaves a gap; with ripple the clips close up like 1.0's magnetic timeline.
        w.m_editor->setRipple(false);
        w.trimClip(tail,30,39);
        QCOMPARE(w.m_seq.total,int64_t(72)); QVERIFY(w.m_seq.at(25)->isGap()); QCOMPARE(w.m_project.timeline.clip(tail)->start,qint64(30));
        w.undo();
        w.m_editor->setRipple(true);
        w.trimClip(tail,30,39);
        QCOMPARE(w.m_seq.total,int64_t(20+10+24)); QCOMPARE(w.locate(25).rawLocal,int64_t(35));
        w.goToFrame(25); w.onAnnotationCreated(rect("on-tail"));
        QVERIFY(w.m_project.find(idA)->explicitAt(35));                     // keyed by source frame
        QVERIFY(w.m_reviewKeys.contains(25));
        w.moveClip(shotB,0); QCOMPARE(w.m_project.timeline.clips[0].uid,shotB); QCOMPARE(w.m_seq.segments[0].count,int64_t(24));
        w.undo(); QCOMPARE(w.m_project.timeline.clips[2].uid,shotB);           // move
        w.undo(); QVERIFY(!w.m_project.find(idA)->explicitAt(35));             // annotation
        w.undo(); QCOMPARE(w.m_project.timeline.clips[1].in,20); QCOMPARE(w.m_project.timeline.clips[1].out,-1);  // trim
        w.undo(); QCOMPARE(w.m_project.timeline.clips.size(),2); QCOMPARE(w.m_seq.total,int64_t(72));            // split
        w.redo(); w.redo(); QCOMPARE(w.m_project.timeline.clips.size(),3); QCOMPARE(w.m_project.timeline.clips[1].in,30);
        // Export follows the trims: timeline frame 25 is source frame 35 of Shot A.
        w.goToFrame(25); w.onAnnotationCreated(rect("export-check"));
        {
            ExportJob job; job.project=w.m_project; job.sequence=w.m_seq; job.first=24; job.last=26; job.fps=24;
            job.format=ExportJob::Format::PngSequence; job.output=temp.filePath("trim/frame");
            std::atomic<int64_t> done{0}; std::atomic<bool> cancel{false}; QString exportError;
            QVERIFY2(runExport(job,done,cancel,&exportError),qPrintable(exportError));
            auto reds=[](const QImage& img) { int n=0; for (int y=0;y<img.height();++y) for (int x=0;x<img.width();++x) { const QColor c=img.pixelColor(x,y); if (c.red()>150 && c.green()<90) ++n; } return n; };
            QCOMPARE(reds(QImage(temp.filePath("trim/frame_0000.png"))),0);    // source frame 34: nothing drawn yet
            QVERIFY(reds(QImage(temp.filePath("trim/frame_0001.png")))>20);    // source frame 35: the rectangle
        }
        const QString path=temp.filePath("trims.frvm"); QString error; QVERIFY2(w.m_project.save(path,&error),qPrintable(error));
        Project loaded; QVERIFY(loaded.load(path,&error));
        QCOMPARE(loaded.timeline.clips.size(),3); QCOMPARE(loaded.timeline.clips[1].in,30); QCOMPARE(loaded.timeline.clips[1].out,39);
        QCOMPARE(loaded.timeline.clips[1].uid,tail);
        QAction* show=w.findChild<QAction*>("Window/Timeline"); QVERIFY(show); show->trigger();
        QTRY_VERIFY(w.m_editorDock->isVisible());
        QCOMPARE(w.m_editor->clipCount(),3); QCOMPARE(w.m_editor->currentFrame(),w.m_current);
        w.m_editor->zoomToFit(); QTest::qWait(250);
        QVERIFY(w.grab().save(QApplication::applicationDirPath()+"/qa-timeline-editor.png"));
        // Trimmed-away frames: next/previous annotated frame skips them.
        w.goToFrame(0); QCOMPARE(w.annotatedGlobals(),QVector<qint64>({25}));
        w.m_editor->setRipple(false);
        w.m_dirty=false;
    }
    void timelineVisibilityPersistsOnlyAfterClosing() {
        QSettings settings;
        QMap<QString,QVariant> saved;
        settings.beginGroup("layout");
        for (const QString& key : settings.allKeys()) saved[key]=settings.value(key);
        settings.endGroup();
        const QVariant geometry=settings.value("geometry");
        const auto restore=qScopeGuard([&] {
            settings.remove("layout"); settings.beginGroup("layout");
            for (auto it=saved.begin();it!=saved.end();++it) settings.setValue(it.key(),it.value());
            settings.endGroup();
            if (geometry.isValid()) settings.setValue("geometry",geometry); else settings.remove("geometry");
        });
        settings.remove("layout");
        {
            MainWindow first; first.show(); QVERIFY(QTest::qWaitForWindowExposed(&first));
            QVERIFY(first.m_editorDock->isHidden());
            first.findChild<QAction*>("Window/Timeline")->trigger(); QTRY_VERIFY(first.m_editorDock->isVisible());
            QVERIFY(!settings.contains("layout/timelineVisible")); // toggling alone is not a completed exit
            first.m_dirty=false; QVERIFY(first.close());
        }
        QVERIFY(settings.value("layout/timelineVisible").toBool());
        const QByteArray visibleState=settings.value("layout/state").toByteArray();
        {
            MainWindow reopened; reopened.show(); QVERIFY(QTest::qWaitForWindowExposed(&reopened));
            QTRY_VERIFY(reopened.m_editorDock->isVisible());
            reopened.setViewerOnly(true); QVERIFY(!reopened.m_editorDock->isVisible());
            reopened.m_dirty=false; QVERIFY(reopened.close()); // temporary focus view must not erase the choice
        }
        QVERIFY(settings.value("layout/timelineVisible").toBool());
        {
            MainWindow third; third.show(); QVERIFY(QTest::qWaitForWindowExposed(&third));
            QTRY_VERIFY(third.m_editorDock->isVisible());
            third.findChild<QAction*>("Window/Timeline")->trigger(); QVERIFY(third.m_editorDock->isHidden());
            third.m_dirty=false; QVERIFY(third.close());
        }
        QVERIFY(!settings.value("layout/timelineVisible").toBool());
        {
            MainWindow fourth; fourth.show(); QVERIFY(QTest::qWaitForWindowExposed(&fourth));
            QVERIFY(fourth.m_editorDock->isHidden()); fourth.m_dirty=false;
        }
        settings.remove("layout/timelineVisible"); settings.setValue("layout/state",visibleState);
        {
            MainWindow legacy; legacy.show(); QVERIFY(QTest::qWaitForWindowExposed(&legacy));
            QTRY_VERIFY(legacy.m_editorDock->isVisible()); legacy.m_dirty=false;
        }
    }

    void windowMenuRestoresClosedPanels() {
        MainWindow w; w.resize(1200,800); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* menu=w.findChild<QMenu*>("WindowMenu"); QVERIFY(menu);
        for (const char* name : {"Window/Project","Window/Sources","Window/Timeline","Window/Notes","Window/MotionGraph",
                                 "Window/Tracking","Window/Console","Window/Tools","Window/ClipBar","Window/ScrubBar","Window/Transport"})
            QVERIFY2(w.findChild<QAction*>(name),name);
        w.m_projectPanel->hide(); w.m_timelinePanel->hide(); w.m_leftSplit->hide(); w.m_toolStrip->hide();
        w.findChild<QAction*>("Window/Project")->trigger();
        QVERIFY(!w.m_projectPanel->isHidden()); QVERIFY(!w.m_leftSplit->isHidden());
        w.findChild<QAction*>("Window/Tools")->trigger(); QVERIFY(!w.m_toolStrip->isHidden());
        w.findChild<QAction*>("Window/Notes")->trigger(); QTRY_VERIFY(w.m_notesDock->isVisible());
        w.setViewerOnly(true); QVERIFY(w.m_projectPanel->isHidden()); QVERIFY(!w.m_notesDock->isVisible());
        w.setViewerOnly(false); QVERIFY(!w.m_projectPanel->isHidden()); QTRY_VERIFY(w.m_notesDock->isVisible());
        w.resetLayout();
        QVERIFY(!w.m_notesDock->isVisible()); QVERIFY(w.m_editorDock->isHidden()); QVERIFY(!w.m_timelinePanel->isHidden());
        w.m_dirty=false;
    }
    void onionSkinBehavesLikeAnimationTools() {
        MainWindow w; w.resize(1000,700); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.createCanvas("Onion",{160,90},40,24);
        Source& s=w.m_project.sources.front();
        Annotation a=rect(); for (int k : {0,4,8,12,16}) { a.x=.01*k; a.id=QString("k%1").arg(k); s.annotations[k]={a}; }
        w.m_onion=true; w.m_onionMode=1; w.m_onionBefore=2; w.m_onionAfter=2; w.m_onionStep=4; w.m_onionTint=true;
        w.m_onionVideoBefore=0; w.m_onionVideoAfter=0;
        w.goToFrame(8);
        QCOMPARE(w.m_view->m_ghosts.size(),4);                                 // keys 4, 0 before; 12, 16 after
        QCOMPARE(w.m_view->m_ghosts[0].annotation.id,QString("k4"));
        QCOMPARE(w.m_view->m_ghosts[0].tint,w.m_onionBeforeColor);
        QVERIFY(w.m_view->m_ghosts[0].opacity>w.m_view->m_ghosts[1].opacity);  // nearest is strongest
        QCOMPARE(w.m_view->m_ghosts[2].tint,w.m_onionAfterColor);
        w.m_onionStep=1; w.goToFrame(9);                                       // held drawing is never ghosted over itself
        QCOMPARE(w.m_view->m_ghosts.size(),1);                                 // frame 7 shows key 4; 8, 10, 11 hold the shown key
        w.m_onionMode=0; w.m_onionBefore=1; w.m_onionAfter=1; w.m_onionVideoBefore=1; w.m_onionVideoAfter=1; w.m_onionStep=2;
        w.goToFrame(20);
        QCOMPARE(w.m_onionVideoFrames,QVector<int64_t>({18,22}));
        QTRY_VERIFY_WITH_TIMEOUT(w.m_view->m_ghostInfo.size()==2,5000);
        QCOMPARE(w.m_view->m_ghostInfo[0].tint,w.m_onionBeforeColor);
        QCOMPARE(w.m_view->m_ghostInfo[1].tint,w.m_onionAfterColor);
        w.m_onionTint=false; w.refreshAnnotations();
        QVERIFY(!w.m_view->m_ghostInfo[0].tint.isValid()); QVERIFY(!w.m_view->m_ghosts.front().tint.isValid());
        w.m_playing=true; w.refreshAnnotations(); QVERIFY(w.m_view->m_ghosts.isEmpty());   // hidden while playing by default
        w.m_onionWhilePlaying=true; w.refreshAnnotations(); QVERIFY(!w.m_view->m_ghosts.isEmpty());
        w.m_playing=false; w.m_onionTint=true; w.refreshAnnotations();
        QTimer::singleShot(80,[&] { QVERIFY(w.m_onionMenu->grab().save(QApplication::applicationDirPath()+"/qa-onion-settings.png")); w.m_onionMenu->close(); });
        w.m_onionMenu->exec(w.mapToGlobal(QPoint(80,80)));
        w.m_dirty=false;
    }
    void playOnTwosAndThrees() {
        MainWindow w; w.createCanvas("Twos",{64,48},48,24);
        w.m_stepEvery=2; QCOMPARE(w.steppedFrameFor(7),int64_t(6)); QCOMPARE(w.steppedFrameFor(8),int64_t(8));
        w.m_stepEvery=3; QCOMPARE(w.steppedFrameFor(8),int64_t(6));
        w.m_stepEvery=0;
        w.playOnNs(2); QVERIFY(w.m_playing); QVERIFY(w.m_stepped); QCOMPARE(w.m_stepEvery,2);
        QTest::qWait(400); QCOMPARE(w.m_current%2,int64_t(0));
        w.stopPlayback(); QCOMPARE(w.m_stepEvery,0); QVERIFY(!w.m_stepped);
        w.m_dirty=false;
    }
    void iconsShareOneSize() {
        QImage sheet(16*56,int((Icons::names().size()+15)/16)*56,QImage::Format_ARGB32_Premultiplied); sheet.fill(QColor(0x26,0x26,0x24));
        QPainter p(&sheet); int i=0; QStringList report;
        for (const QString& name : Icons::names()) {
            const QPixmap pm=Icons::pixmap(name,Qt::white,48,1.0);
            const QImage img=pm.toImage();
            int l=48,t=48,r=-1,b=-1;
            for (int y=0;y<48;++y) for (int x=0;x<48;++x) if (qAlpha(img.pixel(x,y))>24) { l=std::min(l,x); r=std::max(r,x); t=std::min(t,y); b=std::max(b,y); }
            const double side=std::max(r-l+1,b-t+1)*24.0/48.0, cx=(l+r+1)/2.0*24/48, cy=(t+b+1)/2.0*24/48;
            report << QString("%1 %2").arg(name).arg(side,0,'f',1);
            QVERIFY2(side>=12.5 && side<=18.5,qPrintable(QString("%1 is %2 units").arg(name).arg(side)));
            QVERIFY2(std::abs(cx-12)<=0.75 && std::abs(cy-12)<=0.75,qPrintable(QString("%1 is off-centre").arg(name)));
            p.drawPixmap((i%16)*56+4,(i/16)*56+4,pm); p.setPen(QColor(255,255,255,40)); p.drawRect((i%16)*56+4,(i/16)*56+4,47,47); ++i;
        }
        p.end(); QVERIFY(sheet.save(QApplication::applicationDirPath()+"/qa-icons.png"));
        qInfo().noquote() << "icon sizes:" << report.join(", ");
    }
    void motionAnalysisFindsStopsAndExtremes() {
        std::map<int,QPointF> points;
        for (int f=0; f<=32; ++f) {                       // a ball thrown up, landing at 24, resting after
            const double t=std::min(f,24);
            points[f]=QPointF(.1+.02*t,.8-.0055*t*(24-t));
        }
        const auto a=MotionGraph::analyze(points,QSizeF(1000,1000));
        auto near=[&](int f,int tolerance) { for (int k : a.keys) if (std::abs(k-f)<=tolerance) return true; return false; };
        QVERIFY(near(0,0)); QVERIFY2(near(12,1),"apex"); QVERIFY2(near(24,2),"landing");
        QVERIFY(a.maxSpeed>0); QVERIFY(a.distance>0);
        MainWindow w; w.resize(1300,860); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.createCanvas("Bounce",{640,360},40,24);
        Track t; t.id="ball"; t.name="Ball"; t.color=Track::colorFor(0); t.createdFrame=0; t.points=points; t.keys={0};
        w.m_project.sources.front().tracks={t}; w.m_selectedTrackId="ball";
        w.findChild<QAction*>("Window/MotionGraph")->trigger(); QTRY_VERIFY(w.m_graphDock->isVisible());
        w.goToFrame(12); QTest::qWait(150);
        const auto inWindow=MotionGraph::analyze(points,QSizeF(640,360));
        QCOMPARE(w.m_graph->analysis().keys,inWindow.keys);
        QVERIFY(w.grab().save(QApplication::applicationDirPath()+"/qa-motion-graph.png"));
        emit w.m_graph->bookmarkRequested(inWindow.keys);
        QCOMPARE(int(w.m_project.sources.front().bookmarks.size()),int(inWindow.keys.size()));
        w.m_dirty=false;
    }
    void frameNotesUndoAndExport() {
        MainWindow w; w.resize(1200,800); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.createCanvas("Notes",{160,90},30,24);
        const QString id=w.m_project.sources.front().id;
        w.findChild<QAction*>("Window/Notes")->trigger(); QTRY_VERIFY(w.m_notesDock->isVisible());
        w.goToFrame(7); w.m_noteEdit->setPlainText("Anticipation needs 2 more frames"); w.goToFrame(9);   // moving commits the typed note
        QCOMPARE(w.m_project.find(id)->notes.at(7),QString("Anticipation needs 2 more frames"));
        QCOMPARE(w.m_notesTree->topLevelItemCount(),1);
        w.undo(); QVERIFY(!w.m_project.find(id)->notes.count(7)); w.redo(); QVERIFY(w.m_project.find(id)->notes.count(7));
        QCOMPARE(w.m_noteEdit->toPlainText(),QString("Anticipation needs 2 more frames"));   // editor follows undo / redo
        const QString path=temp.filePath("notes.frvm"); QString error; QVERIFY(w.m_project.save(path,&error));
        Project loaded; QVERIFY(loaded.load(path,&error)); QCOMPARE(loaded.sources.front().notes.at(7),QString("Anticipation needs 2 more frames"));
        w.goToFrame(7); QTest::qWait(60);
        QVERIFY(w.grab().save(QApplication::applicationDirPath()+"/qa-frame-notes.png"));
        w.m_dirty=false;
    }
    void mcpServerProtocolAndTools() {
        MainWindow w; w.resize(1000,700); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.m_mcp);
        auto call=[&](const QString& method,const QJsonObject& params) {
            return w.m_mcp->handle(QJsonObject{{"jsonrpc","2.0"},{"id",7},{"method",method},{"params",params}});
        };
        const QJsonObject init=call("initialize",{{"protocolVersion","2025-06-18"},{"capabilities",QJsonObject()},{"clientInfo",QJsonObject{{"name","qa"},{"version","1"}}}});
        QCOMPARE(init["result"].toObject()["protocolVersion"].toString(),QString("2025-06-18"));
        QCOMPARE(init["id"].toInt(),7);
        QVERIFY(w.m_mcp->handle(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}}).isEmpty());
        const QJsonArray tools=call("tools/list",{})["result"].toObject()["tools"].toArray();
        QVERIFY(tools.size()>=20);
        for (const QJsonValue& t : tools) QCOMPARE(t.toObject()["inputSchema"].toObject()["type"].toString(),QString("object"));
        auto tool=[&](const QString& name,const QJsonObject& args) { return call("tools/call",{{"name",name},{"arguments",args}})["result"].toObject(); };
        QVERIFY(tool("get_frame_image",{})["isError"].toBool());               // no media yet
        const QJsonObject opened=tool("open_media",{{"paths",QJsonArray{QApplication::applicationDirPath()+"/qa-yuv-quads.mp4"}}});
        QVERIFY2(!opened["isError"].toBool(),qPrintable(QJsonDocument(opened).toJson()));
        const QJsonObject image=tool("get_frame_image",{{"frame",3},{"max_size",160}});
        QVERIFY2(!image["isError"].toBool(),qPrintable(QJsonDocument(image).toJson()));
        const QJsonObject part=image["content"].toArray()[0].toObject(); QCOMPARE(part["type"].toString(),QString("image"));
        QImage png; QVERIFY(png.loadFromData(QByteArray::fromBase64(part["data"].toString().toLatin1()),"PNG"));
        QCOMPARE(png.width(),160); QVERIFY(png.pixelColor(40,22).red()>200);
        QVERIFY(!tool("add_annotation",{{"type","arrow"},{"frame",5},{"x",.1},{"y",.1},{"x2",.4},{"y2",.4},{"color","#00ff00"}})["isError"].toBool());
        QCOMPARE(int(w.m_project.sources.front().annotations.count(5)),1);
        QVERIFY(!tool("notes",{{"action","set"},{"frame",5},{"text","Hold this pose 2 more frames"}})["isError"].toBool());
        QCOMPARE(w.m_project.sources.front().notes.at(5),QString("Hold this pose 2 more frames"));
        const QJsonObject state=tool("get_state",{})["structuredContent"].toObject();
        QCOMPARE(state["frame"].toInt(),5); QCOMPARE(state["clip"].toObject()["note"].toString(),QString("Hold this pose 2 more frames"));
        QVERIFY(!tool("timeline",{{"action","list"}})["isError"].toBool());
        QVERIFY(!tool("set_onion",{{"enabled",true},{"video_before",2},{"step",2}})["isError"].toBool()); QVERIFY(w.m_onion); QCOMPARE(w.m_onionStep,2);
        const QString png2=temp.filePath("ai-frame.png");
        QVERIFY(!tool("export_frame",{{"path",png2},{"frame",5}})["isError"].toBool()); QVERIFY(QImage(png2).width()==320);
        QVERIFY(tool("export_frame",{{"path",png2},{"frame",6}})["isError"].toBool());                     // never overwrites silently
        QVERIFY(!tool("export_frame",{{"path",png2},{"frame",6},{"overwrite",true}})["isError"].toBool());
        QVERIFY(tool("unknown_tool",{})["isError"].toBool());
        QCOMPARE(call("bogus/method",{})["error"].toObject()["code"].toInt(),-32601);
        w.undo(); QVERIFY(!w.m_project.sources.front().notes.count(5));     // AI edits are undoable
        QVERIFY(McpServer::configJson().contains("charonos-mcp.exe"));
        w.m_dirty=false;
    }
    void shortcutsWindowAndPropertiesAreSafe() {
        MainWindow w; w.resize(1100,800); w.createCanvas("Keys",{64,48},24,24);
        bool seen=false;
        QTimer::singleShot(120,[&] {
            auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(d);
            QCOMPARE(d->objectName(),QString("ShortcutsDialog"));
            seen=true;
            QVERIFY(d->grab().save(QApplication::applicationDirPath()+"/qa-shortcuts.png"));
            auto* search=d->findChild<QLineEdit*>("ShortcutSearch"); QVERIFY(search); search->setText("split");
            QTest::qWait(30); QVERIFY(d->grab().save(QApplication::applicationDirPath()+"/qa-shortcuts-search.png"));
            d->accept();
        });
        w.showShortcuts(); QVERIFY(seen);
        // Editing the FPS in Properties rebuilds the panel from that box's own signal (crashed before 1.0).
        w.m_tabs->setCurrentIndex(1);
        auto* spin=w.m_props->findChild<QDoubleSpinBox*>(); QVERIFY(spin);
        spin->setValue(12); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QCOMPARE(w.viewFps(),12.0); QVERIFY(w.m_props->findChild<QDoubleSpinBox*>());
        w.m_dirty=false;
    }
    void timelineEngineOverwriteRippleAndLayers() {
        const QHash<QString,int64_t> lengths{{"a",48},{"b",24},{"c",30}};
        const tl::LengthFn len=[&](const QString& id) { return lengths.value(id); };
        TimelineDoc doc; doc.clips<<TimelineClip::of("a")<<TimelineClip::of("b");
        tl::normalize(doc,len);
        const QString a=doc.clips[0].uid, b=doc.clips[1].uid;
        QCOMPARE(tl::span(*doc.clip(b),len).start,int64_t(48));                      // 1.0 clips land back to back
        QCOMPARE(tl::duration(doc,len),int64_t(72));
        // gap-preserving move: an empty stretch opens and stays
        QVERIFY(tl::moveOverwrite(doc,{b},10,0,0,false,len));
        QCOMPARE(tl::span(*doc.clip(b),len).start,int64_t(58));
        const auto gap=tl::gapAt(doc,tl::Kind::Video,0,50,len); QCOMPARE(gap.first,int64_t(48)); QCOMPARE(gap.second,int64_t(58));
        const Sequence seq=tl::primary(doc,len,{});
        QCOMPARE(seq.total,int64_t(82)); QCOMPARE(seq.segments.size(),3); QVERIFY(seq.segments[1].isGap()); QVERIFY(!seq.at(50)->clipId.size());
        TimelineDoc closed=doc; QVERIFY(tl::closeGap(closed,tl::Kind::Video,0,50,len)); QCOMPARE(tl::span(*closed.clip(b),len).start,int64_t(48));
        // overwrite: dropping b back over a trims a's tail
        QVERIFY(tl::moveOverwrite(doc,{b},-20,0,0,false,len));
        QCOMPARE(tl::span(*doc.clip(a),len).count,int64_t(38)); QCOMPARE(doc.clip(a)->out,37);
        // ripple insert pushes and cuts the clip under the insertion point
        TimelineDoc ins=doc; QVERIFY(tl::insertClips(ins,{TimelineClip::of("c")},10,0,0,true,len));
        QCOMPARE(tl::duration(ins,len),int64_t(92)); QCOMPARE(ins.videoClipCount(),4);
        // lift leaves the gap, ripple delete closes it
        TimelineDoc lift=doc; QVERIFY(tl::remove(lift,{a},false,len)); QCOMPARE(tl::span(*lift.clip(b),len).start,int64_t(38));
        TimelineDoc rip=doc; QVERIFY(tl::remove(rip,{a},true,len)); QCOMPARE(tl::span(*rip.clip(b),len).start,int64_t(0));
        // trims: normal trims stop at the neighbour, ripple trims push it
        TimelineDoc t=doc;
        QCOMPARE(tl::clampTrim(t,{a},1,5,false,len),int64_t(0));
        QVERIFY(tl::trim(t,{a},1,5,true,len)); QCOMPARE(tl::span(*t.clip(b),len).start,int64_t(43));
        QVERIFY(tl::trim(t,{b},-1,4,false,len)); QCOMPARE(tl::span(*t.clip(b),len).start,int64_t(47)); QCOMPARE(t.clip(b)->in,4);
        // cuts keep picture and sound linked, head with head and tail with tail
        TimelineDoc linked;
        { TimelineClip v=TimelineClip::of("a"), s=TimelineClip::of("a",TimelineClip::Kind::Audio); v.link=s.uid; s.link=v.uid; linked.clips<<v<<s; }
        tl::normalize(linked,len);
        QStringList tails; QVERIFY(tl::split(linked,tl::withLinks(linked,{linked.clips[0].uid}),20,len,&tails)); QCOMPARE(tails.size(),2);
        const TimelineClip* vt=linked.clip(tails[0]); QVERIFY(vt && linked.clip(vt->link)); QCOMPARE(linked.clip(vt->link)->link,vt->uid);
        // tracks: a picture-in-picture inset on V2 leaves V1 editable; layers come bottom first
        TimelineDoc mt; TimelineClip base=TimelineClip::of("a"), pip=TimelineClip::of("b");
        base.start=0; pip.track=1; pip.start=10; pip.scale=0.3; mt.clips<<base<<pip; tl::normalize(mt,len);
        QCOMPARE(tl::primary(mt,len,{}).at(15)->clipId,base.uid);
        const auto layers=tl::layersAt(mt,15,len,{}); QCOMPARE(layers.size(),2); QCOMPARE(layers[0].clip->uid,base.uid); QCOMPARE(layers[1].local,int64_t(5));
        mt.clip(pip.uid)->scale=1.0; QCOMPARE(tl::primary(mt,len,{}).at(15)->clipId,pip.uid);     // full frame on top wins
        mt.videoTracks[1].visible=false; QCOMPARE(tl::primary(mt,len,{}).at(15)->clipId,base.uid); // hidden track
        // ripple move = reorder at a clip boundary
        TimelineDoc ro; ro.clips<<TimelineClip::of("a")<<TimelineClip::of("b")<<TimelineClip::of("c"); tl::normalize(ro,len);
        const QString rc=ro.clips[2].uid;
        QCOMPARE(tl::insertionPoint(ro,{rc},tl::Kind::Video,0,5,len),int64_t(0));
        QVERIFY(tl::moveInsert(ro,{rc},0,0,0,len)); QCOMPARE(tl::span(*ro.clip(rc),len).start,int64_t(0)); QCOMPARE(tl::duration(ro,len),int64_t(102));
        QVERIFY(tl::swapNeighbour(ro,rc,1,len)); QCOMPARE(tl::span(*ro.clip(rc),len).start,int64_t(48));
        QCOMPARE(tl::editPoints(ro,len),QVector<int64_t>({0,48,78,102}));
        // duplicate with Alt-drag semantics: originals stay, copies get new uids
        TimelineDoc dup=ro; QStringList made; QVERIFY(tl::moveOverwrite(dup,{rc},100,0,0,true,len,&made));
        QCOMPARE(made.size(),1); QVERIFY(dup.clip(rc)); QCOMPARE(tl::span(*dup.clip(made[0]),len).start,int64_t(148));
        // locked tracks don't move
        TimelineDoc lk=ro; lk.videoTracks[0].locked=true; QVERIFY(!tl::moveOverwrite(lk,{rc},5,0,0,false,len));
    }

    void projectV7RoundTripAndLegacyTimeline() {
        Project p; Source s; s.id="s1"; s.media.kind=MediaRef::Kind::Canvas; s.media.width=64; s.media.height=48; s.media.fileName="Canvas"; s.frameCount=30; s.fps=24;
        p.sources<<s;
        TimelineClip v=TimelineClip::of("s1"); v.start=12; v.track=1; v.opacity=0.5; v.blend=BlendMode::Difference; v.scale=0.4; v.posX=0.2; v.posY=-0.1;
        v.color=QColor("#5cae63"); v.name="Pose"; v.enabled=false;
        TimelineClip a=TimelineClip::of("s1",TimelineClip::Kind::Audio); a.start=12; a.gainDb=-6; a.muted=true; a.fadeIn=3; a.fadeOut=4; a.track=0;
        a.link=v.uid; v.link=a.uid;
        p.timeline.clips<<v<<a;
        p.timeline.videoTracks<<TrackState{"Overlay",true,true};
        p.timeline.audioTracks[0].solo=true; p.timeline.audioTracks[0].gainDb=-3;
        p.timeline.noteTracks<<TrackState{}; p.timeline.noteTracks[1].visible=false;
        p.timeline.width=1280; p.timeline.height=720;
        const QString path=temp.filePath("v7.frvm"); QString error; QVERIFY2(p.save(path,&error),qPrintable(error));
        QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root=QJsonDocument::fromJson(f.readAll()).object(); f.close();
        QCOMPARE(root["version"].toInt(),7);
        const QJsonObject tl0=root["timelines"].toArray()[0].toObject();
        QCOMPARE(tl0["clips"].toArray().size(),1); QCOMPARE(tl0["audioClips"].toArray().size(),1);   // builds before 1.1 only see the picture
        Project q; QVERIFY(q.load(path,&error));
        QVERIFY(!q.timeline.linkAudioOnOpen);
        QCOMPARE(q.timeline.clips.size(),2);
        const TimelineClip* qv=q.timeline.clip(v.uid); const TimelineClip* qa=q.timeline.clip(a.uid); QVERIFY(qv && qa);
        QCOMPARE(*qv,v); QCOMPARE(*qa,a);
        QCOMPARE(q.timeline.videoTracks.size(),2); QCOMPARE(q.timeline.videoTracks[1].name,QString("Overlay")); QVERIFY(q.timeline.videoTracks[1].locked);
        QVERIFY(q.timeline.audioTracks[0].solo); QCOMPARE(q.timeline.audioTracks[0].gainDb,-3.0); QVERIFY(!q.timeline.noteTracks[1].visible);
        QCOMPARE(q.timeline.width,1280); QCOMPARE(q.timeline.height,720);
        // A 1.0 (v6) project: clips without positions play back to back, sound becomes linked clips on open.
        QJsonObject legacy{{"format","FrameReview"},{"version",6},
            {"sources",QJsonArray{QJsonObject{{"id","s1"},{"media",QJsonObject{{"type","canvas"},{"width",64},{"height",48},{"fileName","Old"}}},{"fps",24},{"frameCount",30}}}},
            {"timelines",QJsonArray{QJsonObject{{"name","Old"},{"sources",QJsonArray{"s1","s1"}},
                {"clips",QJsonArray{QJsonObject{{"uid","c1"},{"source","s1"},{"in",0},{"out",9}},QJsonObject{{"uid","c2"},{"source","s1"},{"in",20},{"out",-1}}}}}}}};
        const QString old=temp.filePath("old.frvm"); QFile o(old); QVERIFY(o.open(QIODevice::WriteOnly)); o.write(QJsonDocument(legacy).toJson()); o.close();
        Project r; QVERIFY(r.load(old,&error));
        QVERIFY(r.timeline.linkAudioOnOpen); QCOMPARE(r.timeline.clips.size(),2); QCOMPARE(r.timeline.clips[0].start,qint64(-1));
        tl::normalize(r.timeline,[](const QString&) { return int64_t(30); });
        QCOMPARE(r.timeline.clip("c2")->start,qint64(10));                                    // right after c1 (10 frames)
        QCOMPARE(tl::duration(r.timeline,[](const QString&) { return int64_t(30); }),int64_t(20));
        // The window opens it the same way.
        MainWindow w; QVERIFY(w.loadProject(old));
        QCOMPARE(w.m_seq.total,int64_t(20)); QCOMPARE(w.m_project.timeline.clip("c2")->start,qint64(10));
        w.m_dirty=false;
    }

    void annotationClipsMoveTrimSplitDuplicateAndLanes() {
        auto stroke=[](const QString& id) { Annotation a; a.id=id; a.type="stroke"; a.points={{.1,.1},{.2,.2}}; a.color=Qt::red; return a; };
        auto text=[](const QString& id) { Annotation a; a.id=id; a.type="text"; a.text="Arc!"; a.x=.5; a.y=.5; a.color=Qt::yellow; return a; };
        Source s; s.id="n"; s.frameCount=40;
        s.annotations[0]={stroke("s1"),stroke("s2")};
        s.annotations[10]={text("t1")};
        s.annotations[20]={};
        QVector<NoteClip> cl=notes::clips(s,40);
        QCOMPARE(cl.size(),2);
        const NoteClip draw=*std::find_if(cl.begin(),cl.end(),[](const NoteClip& n) { return n.kind=="draw"; });
        const NoteClip txt=*std::find_if(cl.begin(),cl.end(),[](const NoteClip& n) { return n.kind=="text"; });
        QCOMPARE(draw.first,0); QCOMPARE(draw.last,9); QCOMPARE(draw.ids.size(),2); QCOMPARE(draw.icon,QString("pencil")); QCOMPARE(draw.lane,0);
        QCOMPARE(txt.first,10); QCOMPARE(txt.last,19); QCOMPARE(txt.icon,QString("text")); QCOMPARE(txt.label,QString("Arc!"));
        // move the text later: it shows only there now
        QVERIFY(notes::move(s,40,txt.key,25,0));
        QVERIFY(s.renderable(15).isEmpty()); QCOMPARE(s.renderable(30).size(),1); QCOMPARE(s.renderable(30)[0].annotation->id,QString("t1"));
        QVERIFY(s.renderable(35).isEmpty()); QCOMPARE(s.renderable(5).size(),2);
        // trim the drawing to five frames
        cl=notes::clips(s,40);
        const QString drawKey=std::find_if(cl.begin(),cl.end(),[](const NoteClip& n) { return n.kind=="draw"; })->key;
        QVERIFY(notes::trim(s,40,drawKey,0,4)); QVERIFY(s.renderable(7).isEmpty()); QCOMPARE(s.renderable(4).size(),2);
        // cut the text clip: two clips, the second one with its own objects
        cl=notes::clips(s,40);
        const QString textKey=std::find_if(cl.begin(),cl.end(),[](const NoteClip& n) { return n.kind=="text"; })->key;
        QString second; QVERIFY(notes::split(s,40,textKey,30,&second));
        cl=notes::clips(s,40); QCOMPARE(cl.size(),3);
        QCOMPARE(s.renderable(31)[0].annotation->text,QString("Arc!")); QVERIFY(s.renderable(31)[0].annotation->id!="t1");
        // a copy over the original is stacked on the next lane and draws on top
        cl=notes::clips(s,40);
        const QString d2=std::find_if(cl.begin(),cl.end(),[](const NoteClip& n) { return n.kind=="draw"; })->key;
        QString copy; QVERIFY(notes::duplicate(s,40,d2,2,&copy));
        cl=notes::clips(s,40);
        const NoteClip dup=*std::find_if(cl.begin(),cl.end(),[&](const NoteClip& n) { return n.key==copy; });
        QCOMPARE(dup.lane,1); QCOMPARE(dup.first,2); QCOMPARE(dup.last,6);
        const auto at3=s.renderable(3); QCOMPARE(at3.size(),4);
        QVERIFY(dup.ids.contains(at3.back().annotation->id));                                     // higher lane = drawn last
        // the lane can be chosen; the original keeps its own
        QVERIFY(notes::move(s,40,copy,2,3)); cl=notes::clips(s,40);
        QCOMPARE(std::find_if(cl.begin(),cl.end(),[&](const NoteClip& n) { return n.key==copy; })->lane,3);
        // colour label, delete
        QVERIFY(notes::setColor(s,40,copy,QColor("#4cb05c"),false));
        cl=notes::clips(s,40); QCOMPARE(std::find_if(cl.begin(),cl.end(),[&](const NoteClip& n) { return n.key==copy; })->color,QColor("#4cb05c"));
        QVERIFY(notes::erase(s,40,{copy})); QCOMPARE(s.renderable(3).size(),2);
        // a new stroke at an existing key joins the clip drawn there
        Annotation more=stroke("s3"); notes::placeNew(s,40,0,more,true); s.annotations[0].append(more);
        cl=notes::clips(s,40); const NoteClip joined=*std::find_if(cl.begin(),cl.end(),[](const NoteClip& n) { return n.kind=="draw" && n.first==0; });
        QCOMPARE(joined.ids.size(),3);
        // 1.0 compatibility: the result is still plain keys with lanes kept in unknown-to-1.0 fields
        const QJsonArray json=annotationsToJson(s.annotations.begin()->second);
        QVERIFY(json[0].toObject().contains("clip")); QVERIFY(json[0].toObject().contains("lane"));
    }

    void multiTrackViewerCompositesAndGaps() {
        MainWindow w; w.resize(1200,820); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setOnion(false);  // an earlier test leaves onion skin on (saved setting); ghosts would tint the pixels
        const QString dir=QApplication::applicationDirPath();
        w.importPaths({dir+"/qa-silhouette.mp4"}); w.importPaths({dir+"/qa-blue-tone.mp4"});
        QCOMPARE(w.m_project.sources.size(),2);
        const QString sil=w.m_project.sources[0].id, blue=w.m_project.sources[1].id;
        w.setSolo(QString());
        QCOMPARE(w.m_project.timeline.videoClipCount(),2);
        const QString blueClip=videoClipOf(w,blue);
        const TimelineClip* bc=w.m_project.timeline.clip(blueClip); QVERIFY(bc && !bc->link.isEmpty());       // its sound came along, linked
        QVERIFY(w.m_project.timeline.clip(bc->link)->isAudio()); QCOMPARE(w.m_project.timeline.clip(bc->link)->start,bc->start);
        // V2 picture-in-picture over V1 (top right, 35%)
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::moveOverwrite(d,tl::withLinks(d,{blueClip}),10-48,1,0,false,w.lengthFn()); }));
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { TimelineClip* c=d.clip(blueClip); c->scale=0.35; c->posX=0.3; c->posY=-0.3; return true; }));
        QCOMPARE(w.m_seq.total,int64_t(48));
        w.goToFrame(20);
        QTRY_VERIFY_WITH_TIMEOUT(w.m_view->m_a.frame && w.m_view->m_above.size()==1 && w.m_view->m_above[0].tex.frame,10000);
        QCOMPARE(w.locate(20).source->id,sil);                                                   // the full frame stays the edited picture
        QCOMPARE(w.m_view->mediaSize(),QSize(320,180));
        QTest::qWait(80);
        QVERIFY(w.m_view->grab().save(dir+"/qa-multitrack-pip.png"));
        QVERIFY2(near(viewPixel(*w.m_view,{256,36}),QColor(0x20,0x60,0xC0),40),qPrintable(viewPixel(*w.m_view,{256,36}).name()));  // V2 on top
        QVERIFY(near(viewPixel(*w.m_view,{40,160}),QColor(0xD8,0xD0,0xC0),30));                 // V1 elsewhere
        QVERIFY(w.m_view->grab().save(dir+"/qa-multitrack-pip.png"));
        // Difference blend, full frame: |blue - beige| everywhere outside the moving block
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { TimelineClip* c=d.clip(blueClip); c->scale=1; c->posX=c->posY=0; c->blend=BlendMode::Difference; return true; }));
        w.goToFrame(21);
        QCOMPARE(w.locate(21).source->id,blue);                                                  // full frame on top is edited now
        QTRY_VERIFY_WITH_TIMEOUT(w.m_view->m_a.frame && w.m_view->m_below.size()==1 && w.m_view->m_below[0].tex.frame,10000);
        QTest::qWait(80);
        QVERIFY2(near(viewPixel(*w.m_view,{300,170}),QColor(0xB8,0x70,0x00),40),qPrintable(viewPixel(*w.m_view,{300,170}).name()));
        QVERIFY(w.m_view->grab().save(dir+"/qa-multitrack-difference.png"));
        // Hide V2: V1 alone again
        w.setTrackState(0,1,TrackState{QString(),false});
        QCOMPARE(w.locate(21).source->id,sil);
        w.setTrackState(0,1,TrackState{});
        // Gaps: lift V1, the first ten frames are empty and black
        const QString silClip=videoClipOf(w,sil);
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::remove(d,{silClip},false,w.lengthFn()); }));
        QCOMPARE(w.m_seq.total,int64_t(34)); QVERIFY(w.m_seq.at(5)->isGap());
        w.goToFrame(5);
        QCOMPARE(w.m_current,int64_t(5)); QVERIFY(!w.m_view->m_a.frame); QCOMPARE(w.m_timeline->currentFrame(),qint64(5));
        w.goToFrame(12); QTRY_VERIFY_WITH_TIMEOUT(w.m_view->m_a.frame!=nullptr,10000);
        // Only the inset is left: it is the edited picture, and its annotations follow its transform.
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { TimelineClip* c=d.clip(blueClip); c->scale=0.35; c->posX=0.3; c->posY=-0.3; c->blend=BlendMode::Normal; return true; }));
        w.goToFrame(12);
        QCOMPARE(w.m_view->primaryRect(),QRectF(200,4.5,112,63));
        const QPointF a0=w.m_view->annoToWidget(0).map(QPointF(0,0)), c0=w.m_view->mediaToWidget().map(QPointF(200,4.5));
        QVERIFY(std::abs(a0.x()-c0.x())<0.5 && std::abs(a0.y()-c0.y())<0.5);
        QCOMPARE(w.m_view->layerMedia(0),QSize(320,180));
        w.undo();
        w.undo(); QCOMPARE(w.m_seq.total,int64_t(48)); QVERIFY(!w.m_seq.at(5)->isGap());
        w.m_dirty=false;
    }

    void sourceTreeDragCopiesToTimelineAndUndo() {
        MainWindow w; w.resize(1400,900); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        const QString dir=QApplication::applicationDirPath();
        w.importPaths({dir+"/qa-media.mp4"});
        const auto ids=w.importPaths({dir+"/qa-blue-tone.mp4",dir+"/qa-audio-loop.mp4"},false);
        QCOMPARE(ids.size(),2);
        w.setSolo(QString()); showEditor(w); w.m_editor->setSnap(true); w.m_editor->setRipple(false);
        QVERIFY(w.m_tree->dragEnabled()); QCOMPARE(w.m_tree->dragDropMode(),QAbstractItemView::DragDrop);
        QCOMPARE(w.m_tree->defaultDropAction(),Qt::CopyAction);
        QModelIndexList indices;
        for (const QString& id : ids)
            for (QTreeWidgetItemIterator it(w.m_tree); *it; ++it)
                if ((*it)->data(0,RoleKind).toString()=="source" && (*it)->data(0,RoleId).toString()==id)
                    indices << w.m_tree->indexFromItem(*it);
        QCOMPARE(indices.size(),2);
        std::unique_ptr<QMimeData> mime(w.m_tree->model()->mimeData(indices));
        QWidget* canvas=w.m_editor->canvas(); const QPoint pt=w.m_editor->trackPoint(0,0,24).toPoint();
        auto sendDrop=[&] {
            QDragEnterEvent enter(pt,Qt::CopyAction|Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);
            QApplication::sendEvent(canvas,&enter); QVERIFY(enter.isAccepted());
            QDragMoveEvent move(pt,Qt::CopyAction|Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);
            QApplication::sendEvent(canvas,&move); QVERIFY(move.isAccepted()); QCOMPARE(move.dropAction(),Qt::CopyAction);
            QDropEvent drop(pt,Qt::CopyAction|Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);
            QApplication::sendEvent(canvas,&drop); QVERIFY(drop.isAccepted()); QCOMPARE(drop.dropAction(),Qt::CopyAction);
        };
        sendDrop(); QCOMPARE(w.m_project.timeline.videoClipCount(),3); QCOMPARE(w.m_project.sources.size(),3);
        QCOMPARE(w.m_project.timeline.clip(videoClipOf(w,ids[0]))->start,qint64(24));
        QCOMPARE(w.m_project.timeline.clip(videoClipOf(w,ids[1]))->start,qint64(48));
        QCOMPARE(w.m_seq.total,int64_t(96));
        w.undo(); QCOMPARE(w.m_project.timeline.videoClipCount(),1); QCOMPARE(w.m_project.sources.size(),3);
        w.redo(); QCOMPARE(w.m_project.timeline.videoClipCount(),3);
        TrackState locked; locked.locked=true; w.setTrackState(0,0,locked);
        QDragEnterEvent enter(pt,Qt::CopyAction,mime.get(),Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(canvas,&enter);
        QDragMoveEvent move(pt,Qt::CopyAction,mime.get(),Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(canvas,&move);
        QVERIFY(!move.isAccepted()); QCOMPARE(w.m_project.timeline.videoClipCount(),3);
        // The lower Sources tab uses QListWidget's payload and supports copying out too.
        w.setTrackState(0,0,TrackState{});
        QCOMPARE(w.m_clipList->dragDropMode(),QAbstractItemView::DragDrop);
        mime.reset(w.m_clipList->model()->mimeData({w.m_clipList->model()->index(0,0)}));
        const QPoint next=w.m_editor->trackPoint(0,0,96).toPoint();
        QDragEnterEvent listEnter(next,Qt::CopyAction|Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(canvas,&listEnter); QVERIFY(listEnter.isAccepted());
        QDragMoveEvent listMove(next,Qt::CopyAction|Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(canvas,&listMove); QVERIFY(listMove.isAccepted());
        w.m_view->setFocus(); QTRY_VERIFY(w.m_view->hasFocus());
        QDropEvent listDrop(next,Qt::CopyAction|Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(canvas,&listDrop); QVERIFY(listDrop.isAccepted()); QCOMPARE(listDrop.dropAction(),Qt::CopyAction);
        QCOMPARE(w.m_project.timeline.videoClipCount(),4); QCOMPARE(w.m_project.sources.size(),3);
        QTRY_VERIFY(canvas->hasFocus());
        QTest::keyClick(QApplication::focusWidget(),Qt::Key_Delete);
        QCOMPARE(w.m_project.timeline.videoClipCount(),3);
        w.undo(); QCOMPARE(w.m_project.timeline.videoClipCount(),4);
        w.undo(); QCOMPARE(w.m_project.timeline.videoClipCount(),3);
        w.m_editor->zoomToFit(); QTest::qWait(60);
        QVERIFY(windowShot(w,dir+"/qa-source-drag-drop.png"));
        w.m_dirty=false;
    }

    void mixedFpsTimelineKeepsDurationPitchAndEdits() {
        MainWindow w;
        const QString dir=QApplication::applicationDirPath();
        w.importPaths({dir+"/qa-audio-loop.mp4"});
        w.importPaths({dir+"/qa-60-tone.mp4"});
        w.importPaths({dir+"/qa-30-tone.mp4"});
        w.importPaths({dir+"/qa-2997-tone.mp4"});
        w.setSolo(QString());
        const QString id=w.m_project.sources[1].id, uid=videoClipOf(w,id);
        QTRY_VERIFY_WITH_TIMEOUT(w.m_audioClips.count(id),15000);
        const auto len=w.lengthFn(); const auto s=tl::span(*w.m_project.timeline.clip(uid),len);
        QCOMPARE(s.start,int64_t(48)); QCOMPARE(s.count,int64_t(24)); QCOMPARE(s.last(),int64_t(59));
        QCOMPARE(w.locate(60).local,int64_t(30)); QCOMPARE(w.visibleLayers(60).front().local,int64_t(30));
        QCOMPARE(w.m_seq.total,int64_t(121)); // 2 s + 1 s + 1 s + 30/29.97 s (rounded up to 25 frames)
        const auto seq=w.timelineSequence(); QCOMPARE(Sequence::globalIn(*seq.at(60),30),int64_t(60));
        auto pitch=[&](const AudioMix& mix,double time) {
            const int n=12000; std::vector<float> buf(n*2,0); mix.render(time,1.0/48000,n,buf.data());
            int rises=0; for (int i=1;i<n;++i) if (buf[(i-1)*2]<=0 && buf[i*2]>0) ++rises;
            return rises*48000.0/n;
        };
        QVERIFY(std::abs(pitch(*w.timelineMix(),2.2)-880)<8); // previously 352 Hz (24/60 speed)
        QTRY_VERIFY_WITH_TIMEOUT(w.m_audioClips.count(w.m_project.sources[2].id) && w.m_audioClips.count(w.m_project.sources[3].id),15000);
        QVERIFY(std::abs(pitch(*w.timelineMix(),3.2)-660)<8);
        QVERIFY(std::abs(pitch(*w.timelineMix(),4.2)-550)<8);
        const auto pair=tl::withLinks(w.m_project.timeline,{uid});
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::split(d,pair,55,w.lengthFn()); }));
        const auto head=tl::span(*w.m_project.timeline.clip(uid),w.lengthFn()); QCOMPARE(head.count,int64_t(7));
        QCOMPARE(w.locate(60).local,int64_t(30));
        QVERIFY(std::abs(pitch(*w.timelineMix(),2.2)-880)<8); // crosses the fractional source-frame split
        const QString saved=temp.filePath("mixed-rate.frvm"); QString error;
        QVERIFY2(w.m_project.save(saved,&error),qPrintable(error)); Project loaded; QVERIFY(loaded.load(saved,&error));
        QCOMPARE(loaded.timeline.clips,w.m_project.timeline.clips);
        w.undo();
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::trim(d,pair,-1,5,false,w.lengthFn()); }));
        const auto trimmed=tl::span(*w.m_project.timeline.clip(uid),w.lengthFn());
        QCOMPARE(trimmed.count,int64_t(19)); QCOMPARE(trimmed.in,int64_t(12));
        QVERIFY(std::abs(trimmed.timeStart-5.0/24)<1e-6);
        QCOMPARE(w.locate(60).local,int64_t(30));
        w.undo();
        w.setViewFps(30); QCOMPARE(w.m_seq.total,int64_t(151));
        QCOMPARE(w.m_project.timeline.clip(uid)->start,qint64(60)); QCOMPARE(w.locate(75).local,int64_t(30));
        QVERIFY(std::abs(pitch(*w.timelineMix(),2.2)-880)<8);
        w.undo(); QCOMPARE(w.m_project.timeline.fps,24.0); QCOMPARE(w.locate(60).local,int64_t(30));
        w.m_dirty=false;
    }

    void mixedFpsExportPreservesToneAfterCut() {
        MainWindow w; const QString dir=QApplication::applicationDirPath();
        w.importPaths({dir+"/qa-audio-loop.mp4"}); w.importPaths({dir+"/qa-60-tone.mp4"}); w.setSolo(QString());
        const QString first=w.m_project.sources[0].id, second=w.m_project.sources[1].id;
        QTRY_VERIFY_WITH_TIMEOUT(w.m_audioClips.count(first) && w.m_audioClips.count(second),15000);
        const QString uid=videoClipOf(w,second);
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::split(d,tl::withLinks(d,{uid}),55,w.lengthFn()); }));
        ExportJob job; job.project=w.m_project; job.sequence=w.m_seq; job.first=0; job.last=71; job.fps=24;
        job.format=ExportJob::Format::Mp4H264; job.output=dir+"/qa-mixed-fps-export.mp4";
        job.timeline=true; job.canvas=w.canvasSize(); job.timelineFps=24; job.mix=w.timelineMix();
        for (int64_t g=0;g<=job.last;++g) {
            std::vector<ExportJob::Layer> frame;
            for (const auto& p : w.visibleLayers(g)) frame.push_back({p.clip->sourceId,p.local,p.rect,p.clip->opacity,p.clip->blend,p.clip->uid==w.m_seq.at(g)->clipId});
            job.layers.push_back(frame);
        }
        std::atomic<int64_t> done{0}; std::atomic<bool> cancel{false}; QString error;
        QVERIFY2(runExport(job,done,cancel,&error),qPrintable(error)); QCOMPARE(done.load(),int64_t(72));
        const auto out=AudioClip::decode(job.output,&error); QVERIFY2(out!=nullptr,qPrintable(error));
        auto pitch=[&](double time) {
            const int begin=int((time-out->startTime)*out->rate), n=12000; int rises=0;
            for (int i=begin+1;i<begin+n;++i) if (out->pcm[size_t(i-1)*2]<=0 && out->pcm[size_t(i)*2]>0) ++rises;
            return rises*out->rate/double(n);
        };
        QVERIFY(std::abs(pitch(.2)-440)<8); QVERIFY(std::abs(pitch(2.2)-880)<8);
        QVERIFY(std::abs(out->duration()-3.0)<.06);
        w.m_dirty=false;
    }

    void variableFpsUsesElapsedTimeAndExactSplit() {
        auto info=std::make_shared<MediaInfo>(); info->frameCount=6; info->fps=6; info->variableFrameRate=true;
        info->frameTimes={1.0,1.05,1.2,1.3,1.65,1.8};
        SourceTiming t(6); t.info=info; t.fps=24;
        const tl::LengthFn len=[t](const QString&) { return t; };
        TimelineDoc d; TimelineClip c=TimelineClip::of("vfr"); c.start=0; d.clips<<c;
        const auto s=tl::span(c,len); QCOMPARE(s.count,int64_t(24)); // 0.9667 s rounds up
        const auto seq=tl::primary(d,len,{}); QCOMPARE(seq.at(7)->sourceFrame(7),int64_t(2));
        QCOMPARE(tl::layersAt(d,7,len,{}).front().local,int64_t(2));
        QVERIFY(tl::split(d,{c.uid},7,len)); QCOMPARE(tl::duration(d,len),int64_t(24));
        QCOMPARE(tl::layersAt(d,8,len,{}).front().local,int64_t(3));
        QVERIFY(std::abs(d.clips[0].sourceEnd-(1+7.0/24))<1e-6);
    }

    void timelineSoundMixGainMuteSoloFades() {
        // The mixer on its own: constant 0.5 in, gain and fades out.
        auto pcm=std::make_shared<AudioClip>(); pcm->pcm.assign(size_t(AudioClip::kRate)*2*2,16384); pcm->startTime=0;
        AudioMix mix; mix.fps=24;
        AudioMix::Item it; it.pcm=pcm; it.start=0; it.count=24; for (int k=0;k<=24;++k) it.times.push_back(k/24.0); it.gain=1.0;
        mix.items.push_back(it);
        std::vector<float> buf(4800*2,0.f); mix.render(0.5,1.0/48000,4800,buf.data()); QVERIFY(std::abs(buf[100]-0.5f)<0.01f);
        mix.items[0].gain=dbToGain(-6.0206); std::fill(buf.begin(),buf.end(),0.f); mix.render(0.5,1.0/48000,4800,buf.data()); QVERIFY(std::abs(buf[100]-0.25f)<0.01f);
        mix.items[0].gain=1.0; mix.items[0].fadeIn=12; std::fill(buf.begin(),buf.end(),0.f); mix.render(0.25,1.0/48000,1,buf.data()); QVERIFY(std::abs(buf[0]-0.25f)<0.02f);
        std::fill(buf.begin(),buf.end(),0.f); mix.render(1.5,1.0/48000,10,buf.data()); QCOMPARE(buf[0],0.f);  // after the clip: silence
        // In the window: sound clips, track mute / solo, clip gain, waveform peaks.
        MainWindow w;
        const QString dir=QApplication::applicationDirPath();
        w.importPaths({dir+"/qa-audio-loop.mp4"}); w.importPaths({dir+"/qa-blue-tone.mp4"});
        const QString loop=w.m_project.sources[0].id, tone=w.m_project.sources[1].id;
        w.setSolo(QString());
        QTRY_VERIFY_WITH_TIMEOUT(w.m_audioClips.count(loop) && w.m_audioClips.count(tone),15000);
        QVERIFY(w.m_peaks.count(loop)); QCOMPARE(w.m_peaks.at(loop).size(),qsizetype(48*TimelineEditor::kPeakBins));
        QVERIFY(*std::max_element(w.m_peaks.at(loop).begin(),w.m_peaks.at(loop).end())>0.2f);       // 440 Hz at 1/8 amplitude
        QCOMPARE(int(w.timelineMix()->items.size()),2);
        // the tone's sound on A2, independent of its picture
        QString toneSound; for (const TimelineClip& c : w.m_project.timeline.clips) if (c.isAudio() && c.sourceId==tone) toneSound=c.uid;
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::moveOverwrite(d,{toneSound},-48,0,1,false,w.lengthFn()); }));
        QCOMPARE(w.m_project.timeline.clip(toneSound)->track,1); QCOMPARE(w.m_project.timeline.clip(toneSound)->start,qint64(0));
        QCOMPARE(w.m_project.timeline.clip(videoClipOf(w,tone))->start,qint64(48));            // the picture stayed
        w.setTrackState(1,0,TrackState{QString(),false});                                       // mute A1
        QCOMPARE(int(w.timelineMix()->items.size()),1);
        w.setTrackState(1,0,TrackState{});
        TrackState solo; solo.solo=true; w.setTrackState(1,1,solo);                             // solo A2
        QCOMPARE(int(w.timelineMix()->items.size()),1); QCOMPARE(w.timelineMix()->items[0].pcm,std::shared_ptr<const AudioClip>(w.m_audioClips.at(tone)));
        w.setTrackState(1,1,TrackState{});
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { d.clip(toneSound)->gainDb=-6; d.clip(toneSound)->muted=false; return true; }));
        bool found=false; for (const auto& i : w.timelineMix()->items) if (i.pcm==w.m_audioClips.at(tone)) { found=true; QVERIFY(std::abs(i.gain-0.501)<0.01); }
        QVERIFY(found);
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { d.clip(toneSound)->muted=true; return true; }));
        QCOMPARE(int(w.timelineMix()->items.size()),1);
        // Export: the mix is what is written.
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { d.clip(toneSound)->muted=false; return true; }));
        ExportJob job; job.project=w.m_project; job.sequence=w.m_seq; job.first=0; job.last=w.m_seq.total-1; job.fps=24;
        job.format=ExportJob::Format::Mp4H264; job.output=temp.filePath("mix.mp4"); job.audioClips=w.m_audioClips;
        job.timeline=true; job.canvas=w.canvasSize(); job.timelineFps=24; job.mix=w.timelineMix();
        for (int64_t g=job.first; g<=job.last; ++g) {
            std::vector<ExportJob::Layer> frame;
            for (const auto& p : w.visibleLayers(g)) frame.push_back({p.clip->sourceId,p.local,p.rect,p.clip->opacity,p.clip->blend,w.m_seq.at(g) && p.clip->uid==w.m_seq.at(g)->clipId});
            job.layers.push_back(frame);
        }
        std::atomic<int64_t> done{0}; std::atomic<bool> cancel{false}; QString error;
        QVERIFY2(runExport(job,done,cancel,&error),qPrintable(error));
        const auto out=AudioClip::decode(job.output,&error); QVERIFY(out);
        auto rms=[&](double t0,double t1) { double sum=0; int64_t n=0; for (int64_t i=int64_t(t0*48000); i<int64_t(t1*48000) && i<out->frames(); ++i,++n) sum+=std::pow(out->pcm[size_t(i)*2]/32768.0,2); return n? std::sqrt(sum/n):0.0; };
        QVERIFY2(rms(0.1,0.9)>0.05,qPrintable(QString::number(rms(0.1,0.9))));                  // 440 Hz + 880 Hz under the first second
        QVERIFY(rms(2.2,2.9)<0.2);                                                               // only the loop's tail remains (it ends at 2 s)
        w.m_dirty=false;
    }

    void silhouetteFiltersCpuAndGpu() {
        // CPU (export / verification): a dark block on a light background.
        QImage frame(320,180,QImage::Format_RGB32); frame.fill(QColor(0xD8,0xD0,0xC0));
        QPainter(&frame).fillRect(QRect(100,60,40,60),QColor(0x30,0x28,0x20));
        AnalysisFilter f; f.mode=AnalysisFilter::Mode::Silhouette;
        const double thr=analysis::otsu(frame); QVERIFY(thr>0.2 && thr<0.75);
        QImage out=analysis::apply(frame,f,thr);
        QVERIFY(near(out.pixelColor(120,90),f.ink,8)); QVERIFY(near(out.pixelColor(10,10),f.paper,8));
        QVector<QImage> shots;
        for (int x : {20,80,140,200,260}) { QImage s=frame.copy(); s.fill(QColor(0xD8,0xD0,0xC0)); QPainter(&s).fillRect(QRect(x,60,40,60),QColor(0x30,0x28,0x20)); shots<<s; }
        const QImage plate=analysis::backgroundPlate(shots); QVERIFY(near(plate.pixelColor(110,90),QColor(0xD8,0xD0,0xC0),4));  // the walker is gone
        f.mode=AnalysisFilter::Mode::MovingSilhouette; out=analysis::apply(frame,f,f.threshold(),plate);
        QVERIFY(near(out.pixelColor(120,90),f.ink,8)); QVERIFY(near(out.pixelColor(250,30),f.paper,8));
        f.mode=AnalysisFilter::Mode::Edges; out=analysis::apply(frame,f,f.threshold());
        QVERIFY(near(out.pixelColor(100,90),f.ink,60)); QVERIFY(near(out.pixelColor(120,90),f.paper,8));
        QImage prev=frame.copy(); prev.fill(QColor(0xD8,0xD0,0xC0)); QPainter(&prev).fillRect(QRect(96,60,40,60),QColor(0x30,0x28,0x20));
        f.mode=AnalysisFilter::Mode::MotionHighlight; out=analysis::apply(frame,f,f.threshold(),prev);
        QVERIFY(near(out.pixelColor(138,90),f.accent,40)); QVERIFY(!near(out.pixelColor(120,90),f.accent,60));
        f.mode=AnalysisFilter::Mode::Notan; out=analysis::apply(frame,f,thr); QVERIFY(near(out.pixelColor(120,90),f.ink,4));
        // GPU (viewer): the same picture through the shader.
        MainWindow w; w.resize(1100,760); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setOnion(false);
        const QString dir=QApplication::applicationDirPath();
        w.importPaths({dir+"/qa-silhouette.mp4"});
        const QString id=w.m_project.sources[0].id;
        w.goToFrame(24); QTRY_VERIFY_WITH_TIMEOUT(w.m_view->m_a.frame!=nullptr,10000);
        w.setFilterMode(AnalysisFilter::Mode::Silhouette);
        QTRY_VERIFY(w.m_otsu.contains(id));
        QTest::qWait(60);
        QVERIFY(w.m_view->grab().save(dir+"/qa-filter-silhouette.png"));
        // block at frame 24: x = 20 + 100 * 1 s
        QVERIFY2(near(viewPixel(*w.m_view,{140,90}),w.m_filter.ink,30),qPrintable(viewPixel(*w.m_view,{140,90}).name()));
        QVERIFY2(near(viewPixel(*w.m_view,{30,20}),w.m_filter.paper,30),qPrintable(viewPixel(*w.m_view,{30,20}).name()));
        QVERIFY(w.m_view->grab().save(dir+"/qa-filter-silhouette.png"));
        w.setFilterMode(AnalysisFilter::Mode::MovingSilhouette);
        QTRY_VERIFY_WITH_TIMEOUT(w.m_plates.contains(id),20000);
        QTest::qWait(80);
        QVERIFY2(near(viewPixel(*w.m_view,{140,90}),w.m_filter.ink,40),qPrintable(viewPixel(*w.m_view,{140,90}).name()));
        QVERIFY2(near(viewPixel(*w.m_view,{280,160}),w.m_filter.paper,30),qPrintable(viewPixel(*w.m_view,{280,160}).name()));
        QVERIFY(w.m_view->grab().save(dir+"/qa-filter-moving.png"));
        w.setFilterMode(AnalysisFilter::Mode::MotionHighlight);
        QTRY_VERIFY_WITH_TIMEOUT(w.m_view->m_aRef.frame!=nullptr,10000);
        QTest::qWait(60);
        QVERIFY2(near(viewPixel(*w.m_view,{158,90}),w.m_filter.accent,70),qPrintable(viewPixel(*w.m_view,{158,90}).name()));  // leading edge moved
        QVERIFY(w.m_view->grab().save(dir+"/qa-filter-motion.png"));
        w.setFilterMode(AnalysisFilter::Mode::Edges); QTest::qWait(60);
        QVERIFY(w.m_view->grab().save(dir+"/qa-filter-edges.png"));
        // Export with the filter burned in matches the viewer.
        ExportJob job; job.project=w.m_project; job.sequence=w.m_seq; job.first=24; job.last=24; job.fps=24;
        job.format=ExportJob::Format::PngSequence; job.output=temp.filePath("filter/frame");
        w.setFilterMode(AnalysisFilter::Mode::Silhouette); job.filter=w.m_filter; job.filterThreshold=w.effectiveThreshold();
        std::atomic<int64_t> done{0}; std::atomic<bool> cancel{false}; QString error;
        QVERIFY2(runExport(job,done,cancel,&error),qPrintable(error));
        const QImage png(temp.filePath("filter/frame_0000.png"));
        QVERIFY(near(png.pixelColor(140,90),w.m_filter.ink,30)); QVERIFY(near(png.pixelColor(30,20),w.m_filter.paper,30));
        w.setFilterMode(AnalysisFilter::Mode::None);
        w.m_dirty=false;
    }

    void timelineDeleteUsesRealFocusAndRespectsNoteLocks() {
        MainWindow w; w.resize(1400,950); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateWindow(); QTRY_VERIFY(w.isActiveWindow());
        w.createCanvas("Delete A",{320,180},24,24); w.createCanvas("Delete B",{320,180},24,24); w.setSolo(QString());
        const QString first=w.m_project.sources[0].id;
        w.goToFrame(0); w.onAnnotationCreated(rect("delete-note"));
        showEditor(w); w.m_editor->setRipple(false); w.m_editor->setSnap(false);
        QWidget* canvas=w.m_editor->canvas();
        const QString uid=videoClipOrder(w)[0], next=videoClipOrder(w)[1];
        QTest::mouseClick(canvas,Qt::LeftButton,{},w.m_editor->clipPoint(uid,.5).toPoint());
        QTRY_VERIFY(canvas->hasFocus()); QCOMPARE(w.m_editor->selectedClip(),uid);
        QTest::keyClick(QApplication::focusWidget(),Qt::Key_Delete,Qt::KeypadModifier);
        QVERIFY(!w.m_project.timeline.clip(uid)); QCOMPARE(w.m_project.timeline.clip(next)->start,qint64(24));
        w.undo();
        QTest::mouseClick(canvas,Qt::LeftButton,{},w.m_editor->clipPoint(uid,.5).toPoint());
        // Menu/global Delete must use the same timeline context, even when invoked directly.
        w.deleteContextual(); QVERIFY(!w.m_project.timeline.clip(uid)); w.undo();
        QTest::mouseClick(canvas,Qt::LeftButton,{},w.m_editor->clipPoint(uid,.5).toPoint());
        QTest::keyClick(QApplication::focusWidget(),Qt::Key_Delete,Qt::ShiftModifier);
        QCOMPARE(w.m_project.timeline.clip(next)->start,qint64(0)); w.undo();
        const auto note=notes::clips(*w.m_project.find(first),24).front();
        auto clickNote=[&] { QTest::mouseClick(canvas,Qt::LeftButton,{},w.m_editor->notePoint(first,note.key,.5).toPoint()); };
        clickNote(); QTRY_VERIFY(canvas->hasFocus()); QCOMPARE(w.m_editor->selectedNoteKey(),note.key);
        TrackState locked; locked.locked=true; w.setTrackState(2,note.lane,locked);
        QTest::keyClick(QApplication::focusWidget(),Qt::Key_Delete); QVERIFY(!notes::clips(*w.m_project.find(first),24).isEmpty());
        w.setTrackState(2,note.lane,TrackState{}); clickNote();
        QTest::keyClick(QApplication::focusWidget(),Qt::Key_Delete);
        QVERIFY(notes::clips(*w.m_project.find(first),24).isEmpty()); QCOMPARE(w.m_project.timeline.videoClipCount(),2);
        w.undo(); QVERIFY(!notes::clips(*w.m_project.find(first),24).isEmpty());
        // Viewer Delete continues to delete annotations, leaving timeline media alone.
        w.m_view->setFocus(); w.m_view->setSelection({"delete-note"}); w.deleteContextual();
        QCOMPARE(w.m_project.timeline.videoClipCount(),2);
        w.m_dirty=false;
    }

    void annotationLaneMouseResizePersistsAndResets() {
        QSettings settings; const QVariant before=settings.value("timeline/noteLaneHeights");
        const auto restore=qScopeGuard([&] { if (before.isValid()) settings.setValue("timeline/noteLaneHeights",before); else settings.remove("timeline/noteLaneHeights"); });
        settings.remove("timeline/noteLaneHeights");
        TimelineEditor editor; editor.resize(1100,850);
        TimelineDoc doc; doc.noteTracks << TrackState{};
        editor.setModel(doc,{},"Annotation lane resize"); editor.show(); QVERIFY(QTest::qWaitForWindowExposed(&editor));
        QWidget* canvas=editor.canvas();
        auto boundary=[&](int lane) { const auto p=editor.trackPoint(2,lane,0); return QPointF(45,p.y()+editor.annotationLaneHeight(lane)/2.0); };
        QCOMPARE(editor.annotationLaneHeight(0),40); QCOMPARE(editor.annotationLaneHeight(1),40);
        const double videoBefore=editor.trackPoint(0,0,0).y();
        QTest::mouseMove(canvas,boundary(1).toPoint()); QTRY_COMPARE(canvas->cursor().shape(),Qt::SizeVerCursor);
        dragOn(canvas,boundary(1),boundary(1)+QPointF(0,60));
        QCOMPARE(editor.annotationLaneHeight(1),100); QCOMPARE(editor.annotationLaneHeight(0),40);
        QCOMPARE(editor.trackPoint(0,0,0).y(),videoBefore+60); QVERIFY(editor.doc().sameLayout(doc));
        { TimelineEditor reopened; QCOMPARE(reopened.annotationLaneHeight(1),100); QCOMPARE(reopened.annotationLaneHeight(0),40); }
        dragOn(canvas,boundary(0),boundary(0)-QPointF(0,200)); QCOMPARE(editor.annotationLaneHeight(0),24);
        dragOn(canvas,boundary(0),boundary(0)+QPointF(0,500)); QCOMPARE(editor.annotationLaneHeight(0),240);
        QTest::mouseDClick(canvas,Qt::LeftButton,{},boundary(0).toPoint()); QCOMPARE(editor.annotationLaneHeight(0),40);
        { TimelineEditor reopened; QCOMPARE(reopened.annotationLaneHeight(0),40); QCOMPARE(reopened.annotationLaneHeight(1),100); }
        editor.resize(1100,320); QTest::qWait(40);
        dragOn(canvas,boundary(1),boundary(1)+QPointF(0,100));
        QVERIFY(editor.m_vscroll->maximum()>0); // growing lanes updates the scrollable content
        QVERIFY(editor.grab().save(QApplication::applicationDirPath()+"/qa-resizable-note-lanes.png"));
    }

    void timelineEditorMouseKeysAndUndo() {
        MainWindow w; w.resize(1500,950); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.createCanvas("Shot A",{160,90},48,24); w.createCanvas("Shot B",{160,90},24,24); w.setSolo(QString());
        showEditor(w);
        w.m_editor->setSnap(false); w.m_editor->setRipple(false); w.m_editor->setTool(TimelineEditor::Tool::Select);
        QWidget* c=w.m_editor->canvas(); QVERIFY(c);
        const QString b=videoClipOrder(w)[1];
        // drag B twelve frames later: a gap opens before it
        const double fx=w.m_editor->trackPoint(0,0,60).x()-w.m_editor->trackPoint(0,0,48).x();
        const QPointF grab=w.m_editor->clipPoint(b,0.5);
        dragOn(c,grab,grab+QPointF(fx,0));
        QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(60)); QCOMPARE(w.m_seq.total,int64_t(84)); QVERIFY(w.m_seq.at(50)->isGap());
        w.undo(); QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(48));
        // drag B up onto a new V2 (above the top video track)
        const QPointF up=w.m_editor->clipPoint(b,0.5);
        dragOn(c,up,QPointF(up.x(),w.m_editor->trackPoint(2,0,0).y()));
        QCOMPARE(w.m_project.timeline.clip(b)->track,1); QCOMPARE(w.m_project.timeline.videoTracks.size(),2);
        w.undo(); QCOMPARE(w.m_project.timeline.clip(b)->track,0);
        // trim B's head by 4 frames (no ripple): its In moves, it stays put otherwise
        const double fx4=w.m_editor->trackPoint(0,0,52).x()-w.m_editor->trackPoint(0,0,48).x();
        const QPointF head=w.m_editor->clipPoint(b,0);
        dragOn(c,head,head+QPointF(fx4,0));
        QCOMPARE(w.m_project.timeline.clip(b)->in,4); QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(52));
        w.undo();
        // blade: click A at frame 20
        const QString a=videoClipOrder(w)[0];
        w.m_editor->setTool(TimelineEditor::Tool::Blade);
        const QPointF cut=w.m_editor->trackPoint(0,0,20)-QPointF((w.m_editor->trackPoint(0,0,21).x()-w.m_editor->trackPoint(0,0,20).x())/2,0);
        press(c,cut); release(c,cut);
        QCOMPARE(w.m_project.timeline.videoClipCount(),3); QCOMPARE(w.m_project.timeline.clip(a)->out,19);
        w.m_editor->setTool(TimelineEditor::Tool::Select);
        // Delete lifts (gap stays), Shift+Delete ripples
        const QString tail=videoClipOrder(w)[1];
        const QPointF t=w.m_editor->clipPoint(tail,0.5); press(c,t); release(c,t);
        QCOMPARE(w.m_editor->selectedClips(),QStringList{tail});
        key(c,Qt::Key_Delete);
        QCOMPARE(w.m_project.timeline.videoClipCount(),2); QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(48)); QVERIFY(w.m_seq.at(30)->isGap());
        w.undo();
        const QPointF t2=w.m_editor->clipPoint(tail,0.5); press(c,t2); release(c,t2);
        key(c,Qt::Key_Delete,Qt::ShiftModifier);
        QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(20)); QCOMPARE(w.m_seq.total,int64_t(44));
        w.undo(); QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(48));
        // keys while the timeline has focus
        key(c,Qt::Key_S); QVERIFY(w.m_editor->snap()); key(c,Qt::Key_S); QVERIFY(!w.m_editor->snap());
        key(c,Qt::Key_B); QCOMPARE(w.m_editor->tool(),TimelineEditor::Tool::Blade); key(c,Qt::Key_V); QCOMPARE(w.m_editor->tool(),TimelineEditor::Tool::Select);
        // nudge with Alt+arrow, disable with D
        const QPointF nb=w.m_editor->clipPoint(b,0.5); press(c,nb); release(c,nb);
        key(c,Qt::Key_Right,Qt::AltModifier); QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(49));
        key(c,Qt::Key_D); QVERIFY(!w.m_project.timeline.clip(b)->enabled); QVERIFY(w.m_seq.at(55)->isGap());
        w.undo(); w.undo(); QVERIFY(w.m_project.timeline.clip(b)->enabled); QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(48));
        // copy / paste at the playhead
        press(c,w.m_editor->clipPoint(b,0.5)); release(c,w.m_editor->clipPoint(b,0.5)); w.m_editor->setCurrent(72);
        key(c,Qt::Key_C,Qt::ControlModifier); key(c,Qt::Key_V,Qt::ControlModifier);
        QCOMPARE(w.m_project.timeline.videoClipCount(),4); QCOMPARE(w.m_seq.total,int64_t(96));
        // ripple mode: dragging a clip earlier reorders
        w.undo(); w.m_editor->setRipple(true);
        const QPointF rb=w.m_editor->clipPoint(b,0.5);
        dragOn(c,rb,QPointF(w.m_editor->trackPoint(0,0,1).x(),rb.y()));
        QCOMPARE(w.m_project.timeline.clip(b)->start,qint64(0)); QCOMPARE(w.m_seq.total,int64_t(72));
        w.m_editor->setRipple(false);
        QVERIFY(windowShot(w,QApplication::applicationDirPath()+"/qa-timeline-multitrack.png"));
        w.m_dirty=false;
    }

    void annotationLanesInEditorAndViewer() {
        MainWindow w; w.resize(1400,900); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.createCanvas("Notes",{320,180},40,24); w.setSolo(QString());
        const QString id=w.m_project.sources[0].id;
        w.goToFrame(0); w.onAnnotationCreated(rect("r1"));
        w.m_continueDrawing=true; w.goToFrame(10); w.onAnnotationCreated(rect("r2")); w.m_continueDrawing=false;
        QVector<NoteClip> cl=notes::clips(*w.m_project.find(id),40);
        QCOMPARE(cl.size(),2);
        const NoteClip first=cl[0].first==0 ? cl[0] : cl[1], second=cl[0].first==0 ? cl[1] : cl[0];
        QCOMPARE(first.lane,0); QCOMPARE(second.lane,1); QCOMPARE(first.icon,QString("rect"));
        QVERIFY(w.m_reviewKeys.contains(0) && w.m_reviewKeys.contains(10));                      // red markers on the scrub bar stay
        showEditor(w); w.m_editor->setSnap(false);
        const QPointF block=w.m_editor->notePoint(id,second.key,0.5); QVERIFY(!block.isNull());
        // hide lane N2: r2 disappears from the viewer, r1 stays
        w.goToFrame(15);
        // drawn on the picture: the rectangle's left edge (x = 0.12) is red
        w.m_view->setShowAnnotations(true); QTest::qWait(40);
        const QColor edge=viewPixel(*w.m_view,{0.12*320,0.32*180});
        QVERIFY2(edge.red()>150 && edge.green()<90,qPrintable(edge.name()));
        auto ids=[&] { QStringList out; for (const auto& i : w.m_view->m_layers[0]) out<<i.annotation.id; return out; };
        QCOMPARE(ids(),QStringList({"r1","r2"}));
        w.setTrackState(2,1,TrackState{QString(),false}); QCOMPARE(ids(),QStringList({"r1"}));
        w.setTrackState(2,1,TrackState{});
        // drag the r2 block five frames later in the timeline
        QWidget* c=w.m_editor->canvas();
        const double fx=w.m_editor->trackPoint(0,0,15).x()-w.m_editor->trackPoint(0,0,10).x();
        dragOn(c,block,block+QPointF(fx,0));
        QVERIFY(w.m_project.find(id)->renderable(12).size()==1); QCOMPARE(w.m_project.find(id)->renderable(16).size(),2);
        w.undo(); QCOMPARE(w.m_project.find(id)->renderable(12).size(),2);
        // trim r1 with its tail handle to end at frame 5
        cl=notes::clips(*w.m_project.find(id),40);
        const NoteClip r1=*std::find_if(cl.begin(),cl.end(),[](const NoteClip& n) { return n.first==0; });
        const QPointF tailPt=w.m_editor->notePoint(id,r1.key,1);
        const QPointF to(w.m_editor->trackPoint(0,0,6).x()-(w.m_editor->trackPoint(0,0,6).x()-w.m_editor->trackPoint(0,0,5).x())/2,tailPt.y());
        dragOn(c,tailPt,to);
        QVERIFY(w.m_project.find(id)->renderable(8).size()<=1); QCOMPARE(w.m_project.find(id)->renderable(3).size(),1);
        // clicking a block selects its objects in the viewer
        w.undo();
        const QPointF sel=w.m_editor->notePoint(id,r1.key,0.2); press(c,sel); release(c,sel);
        QCOMPARE(w.m_view->selectedIds(),QStringList({"r1"}));
        QVERIFY(windowShot(w,QApplication::applicationDirPath()+"/qa-annotation-lanes.png"));
        w.m_dirty=false;
    }

    void editPointsShuttleMarkAndGapPlayback() {
        MainWindow w; w.createCanvas("A",{64,48},48,24); w.createCanvas("B",{64,48},24,24); w.setSolo(QString());
        w.goToFrame(0); w.jumpEditPoint(1); QCOMPARE(w.m_current,int64_t(48)); w.jumpEditPoint(-1); QCOMPARE(w.m_current,int64_t(0));
        w.goToFrame(50); w.markClip(); QCOMPARE(w.m_project.timeline.loopIn,48); QCOMPARE(w.m_project.timeline.loopOut,71); QVERIFY(w.m_project.timeline.loopEnabled);
        w.clearLoop();
        w.goToFrame(60); w.shuttle(-1); QTRY_VERIFY_WITH_TIMEOUT(w.m_current<56,3000); w.shuttle(0); QVERIFY(!w.m_shuttleTimer.isActive());
        w.shuttle(-1); w.shuttle(-1); QCOMPARE(w.m_shuttle,-2); w.togglePlay(); QCOMPARE(w.m_shuttle,0);
        // playback runs through an empty stretch
        const QString a=videoClipOrder(w)[0];
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::remove(d,{a},false,w.lengthFn()); }));
        w.goToFrame(0); w.togglePlay(); QVERIFY(w.m_playing);
        QTRY_VERIFY_WITH_TIMEOUT(w.m_current>48,5000);
        w.stopPlayback();
        w.m_dirty=false;
    }

    void mcpTimelineAnnotationClipsAndFilterTools() {
        MainWindow w; w.createCanvas("AI",{320,180},30,24); w.createCanvas("AI2",{320,180},20,24); w.setSolo(QString());
        QString error;
        QJsonObject list=w.runTool("timeline",{{"action","list"}},&error).toObject(); QVERIFY(error.isEmpty());
        QCOMPARE(list["clips"].toArray().size(),2); QCOMPARE(list["video_tracks"].toArray().size(),1);
        const QString second=list["clips"].toArray()[1].toObject()["clip_uid"].toString();
        list=w.runTool("timeline",{{"action","place"},{"clip_uid",second},{"frame",40},{"track",1}},&error).toObject(); QVERIFY2(error.isEmpty(),qPrintable(error));
        QCOMPARE(w.m_project.timeline.clip(second)->start,qint64(40)); QCOMPARE(w.m_project.timeline.clip(second)->track,1);
        w.runTool("timeline",{{"action","set"},{"clip_uid",second},{"opacity",0.5},{"blend","difference"}},&error); QVERIFY(error.isEmpty());
        QCOMPARE(w.m_project.timeline.clip(second)->opacity,0.5); QCOMPARE(w.m_project.timeline.clip(second)->blend,BlendMode::Difference);
        w.runTool("timeline",{{"action","place"},{"clip_uid",second},{"frame",40},{"track",0}},&error);
        w.runTool("timeline",{{"action","close_gap"},{"frame",35},{"track",0}},&error); QVERIFY2(error.isEmpty(),qPrintable(error));
        QCOMPARE(w.m_project.timeline.clip(second)->start,qint64(30));
        w.undo(); QCOMPARE(w.m_project.timeline.clip(second)->start,qint64(40));                 // AI edits are undoable
        w.goToFrame(2); w.onAnnotationCreated(rect("ai-note"));
        const QJsonObject notesList=w.runTool("annotation_clips",{{"action","list"}},&error).toObject(); QVERIFY(error.isEmpty());
        QCOMPARE(notesList["clips"].toArray().size(),1);
        const QString key=notesList["clips"].toArray()[0].toObject()["key"].toString();
        w.runTool("annotation_clips",{{"action","trim"},{"key",key},{"first",2},{"last",6}},&error); QVERIFY2(error.isEmpty(),qPrintable(error));
        QVERIFY(w.m_project.sources[0].renderable(8).isEmpty()); QCOMPARE(w.m_project.sources[0].renderable(5).size(),1);
        const QJsonObject filter=w.runTool("set_filter",{{"mode","silhouette"},{"threshold",0.4}},&error).toObject(); QVERIFY(error.isEmpty());
        QCOMPARE(w.m_filter.mode,AnalysisFilter::Mode::Silhouette); QCOMPARE(filter["threshold"].toDouble(),0.4);
        w.runTool("set_filter",{{"mode","none"}},&error); QVERIFY(!w.m_filter.enabled());
        QVERIFY(w.runTool("timeline",{{"action","bogus"}},&error).isNull());
        w.m_dirty=false;
    }

    void timelineExportComposesTracks() {
        MainWindow w; w.resize(1000,700);
        const QString dir=QApplication::applicationDirPath();
        w.importPaths({dir+"/qa-silhouette.mp4"}); w.importPaths({dir+"/qa-blue-tone.mp4"});
        const QString blue=w.m_project.sources[1].id; w.setSolo(QString());
        const QString blueClip=videoClipOf(w,blue);
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::moveOverwrite(d,tl::withLinks(d,{blueClip}),-48,1,0,false,w.lengthFn()); }));
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { TimelineClip* c=d.clip(blueClip); c->scale=0.35; c->posX=0.3; c->posY=-0.3; return true; }));
        ExportJob job; job.project=w.m_project; job.sequence=w.m_seq; job.first=5; job.last=6; job.fps=24;
        job.format=ExportJob::Format::PngSequence; job.output=temp.filePath("layers/frame");
        job.timeline=true; job.canvas=w.canvasSize(); job.timelineFps=24;
        for (int64_t g=job.first; g<=job.last; ++g) {
            std::vector<ExportJob::Layer> frame;
            for (const auto& p : w.visibleLayers(g)) frame.push_back({p.clip->sourceId,p.local,p.rect,p.clip->opacity,p.clip->blend,p.clip->uid==w.m_seq.at(g)->clipId});
            job.layers.push_back(frame);
        }
        std::atomic<int64_t> done{0}; std::atomic<bool> cancel{false}; QString error;
        QVERIFY2(runExport(job,done,cancel,&error),qPrintable(error));
        const QImage png(temp.filePath("layers/frame_0000.png")); QCOMPARE(png.size(),QSize(320,180));
        QVERIFY(near(png.pixelColor(256,36),QColor(0x20,0x60,0xC0),30));                         // V2 inset on top
        QVERIFY(near(png.pixelColor(40,160),QColor(0xD8,0xD0,0xC0),30));                          // V1 behind it
        w.m_dirty=false;
    }

    void qaShowcaseTimeline() {
        // A realistic edit for the QA sheet: picture + sound, an inset on V2 with its sound on A2,
        // annotation clips of every kind on their lanes, a gap, and the silhouette filter.
        MainWindow w; w.resize(1600,1000); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setOnion(false);
        const QString dir=QApplication::applicationDirPath();
        w.importPaths({dir+"/qa-audio-loop.mp4"}); w.importPaths({dir+"/qa-silhouette.mp4"}); w.importPaths({dir+"/qa-blue-tone.mp4"});
        const QString loop=w.m_project.sources[0].id, sil=w.m_project.sources[1].id, blue=w.m_project.sources[2].id;
        w.setSolo(QString());
        QTRY_VERIFY_WITH_TIMEOUT(w.m_audioClips.count(loop) && w.m_audioClips.count(blue),15000);
        const QString blueClip=videoClipOf(w,blue), silClip=videoClipOf(w,sil);
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::moveOverwrite(d,tl::withLinks(d,{blueClip}),30-96,1,1,false,w.lengthFn()); }));
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { TimelineClip* c=d.clip(blueClip); c->scale=0.35; c->posX=0.3; c->posY=-0.3; c->name="Ref inset"; return true; }));
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { return tl::moveOverwrite(d,{silClip},12,0,0,false,w.lengthFn()); }));  // a gap before it
        QString blueSound; for (const TimelineClip& c : w.m_project.timeline.clips) if (c.isAudio() && c.sourceId==blue) blueSound=c.uid;
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { d.clip(blueSound)->gainDb=-6; d.clip(blueSound)->fadeIn=6; d.clip(blueSound)->fadeOut=6; return true; }));
        // annotations on the first clip: a drawing, a text, then shapes drawn over them
        Annotation stroke; stroke.id="qa-stroke"; stroke.type="stroke"; stroke.color=QColor(0xe5,0x48,0x3e); stroke.size=4;
        for (int i=0;i<20;++i) stroke.points<<QPointF(.2+i*.02,.6-std::sin(i/19.0*3.14159)*.3);
        w.goToFrame(4); w.onAnnotationCreated(stroke);
        Annotation text; text.id="qa-text"; text.type="text"; text.text="Arc peak"; text.x=.55; text.y=.15; text.fontSize=22; text.color=Qt::white; text.useBg=true; text.bgColor="#000000";
        w.goToFrame(14); w.onAnnotationCreated(text);
        w.m_continueDrawing=true; w.goToFrame(20);
        Annotation box=rect("qa-rect"); box.color=QColor(0x4a,0x86,0xe0); w.onAnnotationCreated(box);
        w.m_continueDrawing=false;
        showEditor(w);
        w.resizeDocks({w.m_editorDock},{430},Qt::Vertical); QTest::qWait(60); w.m_editor->zoomToFit();
        w.m_editor->setSelectedClips({blueClip});
        w.goToFrame(36);
        QTRY_VERIFY_WITH_TIMEOUT(w.m_view->m_a.frame && w.m_view->layerCount()==1 && w.m_view->m_above[0].tex.frame,10000);
        QTest::qWait(150);
        QVERIFY(windowShot(w,dir+"/qa-timeline-1.1.png"));
        QVERIFY(!w.m_peaks.at(loop).isEmpty());
        w.setFilterMode(AnalysisFilter::Mode::Silhouette); QTest::qWait(150);
        QVERIFY(windowShot(w,dir+"/qa-silhouette-app.png"));
        w.setFilterMode(AnalysisFilter::Mode::None);
        w.m_dirty=false;
    }
    void perfTwoLayerPlayback() {
        // UHD long-GOP on V1 with an HD B-frame inset on V2: both decode while the timeline plays.
        const QString uhd="C:/dev/testmedia/uhd_h264_longgop.mp4", hd="C:/dev/testmedia/hd_h264_bframes.mp4";
        if (!QFileInfo::exists(uhd) || !QFileInfo::exists(hd)) QSKIP("local test media not present");
        MainWindow w; w.resize(1400,900); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setOnion(false);
        w.importPaths({uhd}); w.importPaths({hd});
        const QString inset=videoClipOf(w,w.m_project.sources[1].id);
        w.setSolo(QString());
        QVERIFY(w.editTimeline([&](TimelineDoc& d) { TimelineClip* c=d.clip(inset); c->track=1; c->start=0; c->scale=0.35; c->posX=0.3; c->posY=-0.3; return true; }));
        w.m_realtime=false;
        w.goToFrame(0); QTRY_VERIFY_WITH_TIMEOUT(w.layersReady(0),15000);
        w.togglePlay();
        QTest::qWait(1500);                                 // warm-up: decoders reach the playhead
        const int64_t f0=w.m_current; QElapsedTimer t; t.start();
        QTest::qWait(4000);
        const double fps=(w.m_current-f0)*1000.0/t.elapsed();
        w.stopPlayback();
        qInfo().noquote()<<QString("Two-layer playback (UHD V1 + HD V2 inset): %1 fps of %2").arg(fps,0,'f',1).arg(w.viewFps());
        QVERIFY2(fps>w.viewFps()*0.8,qPrintable(QString::number(fps)));
        w.m_dirty=false;
    }
    void realProjectsFromEarlierVersionsOpen() {
        // Projects saved by earlier builds on this machine (read only; re-saved into the temp folder).
        const QFileInfoList files=QDir("C:/dev/testmedia").entryInfoList({"*.frvm"},QDir::Files);
        if (files.isEmpty()) QSKIP("no earlier projects on this machine");
        for (const QFileInfo& fi : files) {
            Project p; QString error; QVERIFY2(p.load(fi.absoluteFilePath(),&error),qPrintable(fi.fileName()+": "+error));
            int keys=0; for (const Source& s : p.sources) keys+=int(s.annotations.size());
            const QString copy=temp.filePath("again-"+fi.fileName()); QVERIFY(p.save(copy,&error));
            Project q; QVERIFY(q.load(copy,&error));
            int keys2=0; for (const Source& s : q.sources) keys2+=int(s.annotations.size());
            QCOMPARE(keys2,keys); QCOMPARE(q.timeline.videoClipCount(),p.timeline.videoClipCount());
            bool allMedia=true; for (const Source& s : p.sources) allMedia=allMedia && s.media.exists();
            if (!allMedia) continue;
            MainWindow w; QVERIFY(w.loadProject(fi.absoluteFilePath()));
            QVERIFY(w.m_seq.total>0);
            int open=0; for (const Source& s : w.m_project.sources) open+=int(s.annotations.size());
            QCOMPARE(open,keys);
            qInfo().noquote()<<QString("%1: %2 sources, %3 clips, %4 annotation keys, %5 frames").arg(fi.fileName()).arg(p.sources.size())
                                .arg(w.m_project.timeline.clips.size()).arg(keys).arg(w.m_seq.total);
            w.m_dirty=false;
        }
    }
    void cleanupTestCase() { window->m_dirty=false; window.reset(); }
};

int main(int argc,char** argv) {
    QSurfaceFormat format; format.setStencilBufferSize(8); format.setSamples(8); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv); applyDarkTheme(app);
    FeatureRegression test; return QTest::qExec(&test,argc,argv);
}
#include "feature_regression.moc"

