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
#include <QAction>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
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
#include <QMimeData>
#include <QPainter>
#include <QPointingDevice>
#include <QSettings>
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
    bool decode(int64_t, QImage& out, QString*, const FrameSink*) override { ++decoded; out=image; return true; }
};

class SlowLoopSource final : public FrameSource {
    MediaInfo data;
    QImage image{64,48,QImage::Format_RGBA8888};
public:
    std::atomic<bool> delayFirst{false};
    SlowLoopSource() { data.width=64; data.height=48; data.fps=24; data.frameCount=24; image.fill(Qt::black); }
    const MediaInfo& info() const override { return data; }
    bool decode(int64_t index, QImage& out, QString*, const FrameSink*) override {
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
        auto image=std::make_shared<QImage>(320,180,QImage::Format_RGB32); image->fill(Qt::black); v.setFrame(image);
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
        auto image=std::make_shared<QImage>(320,180,QImage::Format_RGB32); image->fill(Qt::black); v.setFrame(image);
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
        window->m_onion=true; window->m_onionBefore=2; window->m_onionAfter=1; window->m_onionVideo=1;
        window->goToFrame(5);
        QCOMPARE(window->m_view->m_ghosts.size(),2);
        QCOMPARE(window->m_view->m_ghosts[0].annotation.x,.12);
        QCOMPARE(window->m_view->m_ghosts[1].annotation.x,.6);
        QTRY_VERIFY_WITH_TIMEOUT(window->m_view->m_ghostOpacity.size()==2,10000);
        src.annotations[6]={}; window->goToFrame(6);
        QVERIFY(window->m_view->items().isEmpty());
        QCOMPARE(window->m_view->m_ghosts.size(),3);
        window->m_onion=false; window->refreshAnnotations();
        QVERIFY(window->m_view->m_ghosts.isEmpty()); QVERIFY(window->m_view->m_ghostOpacity.isEmpty());
    }
    void compareGridAndVideoGhosts() {
        Viewport v; v.resize(1000,700); v.setMediaSize(QSize(320,180));
        // FrameServer delivers RGBA8888 images to the GL texture upload path.
        auto black=std::make_shared<QImage>(320,180,QImage::Format_RGBA8888); black->fill(Qt::black);
        auto b=std::make_shared<QImage>(240,320,QImage::Format_RGBA8888); b->fill(Qt::black);
        auto red=std::make_shared<QImage>(320,180,QImage::Format_RGBA8888); red->fill(Qt::red);
        auto blue=std::make_shared<QImage>(240,320,QImage::Format_RGBA8888); blue->fill(Qt::blue);
        v.setFrame(black); v.setCompareFrame(b,b->size()); v.setCompareLabels("A","B");
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
            v.setShowGrid(false); v.setOnion({},{{red,.28,0},{blue,.28,1}});
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
        window->m_onion=true; window->m_onionVideo=1; window->refreshAnnotations();
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
        v.setFrame(a); v.setCompareFrame(b,b->size()); v.show(); QVERIFY(QTest::qWaitForWindowExposed(&v));
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
        v.setOnion({},{{b,.28,0}});
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
        auto image=std::make_shared<QImage>(320,180,QImage::Format_RGB32); image->fill(Qt::black); v.setFrame(image);
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
        auto image=std::make_shared<QImage>(320,180,QImage::Format_RGB32); image->fill(Qt::black); v.setFrame(image);
        v.setCompareFrame(image,{320,180}); v.setCompareMode(CompareMode::SideBySide);
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
    void cleanupTestCase() { window->m_dirty=false; window.reset(); }
};

int main(int argc,char** argv) {
    QSurfaceFormat format; format.setStencilBufferSize(8); format.setSamples(8); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv); applyDarkTheme(app);
    FeatureRegression test; return QTest::qExec(&test,argc,argv);
}
#include "feature_regression.moc"

