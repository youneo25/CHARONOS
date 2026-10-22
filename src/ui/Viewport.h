#pragma once

#include "analysis/AnalysisFilter.h"
#include "media/FrameCache.h"
#include "model/Annotation.h"
#include "model/Project.h"
#include "model/Track.h"

#include <QHash>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QSet>

#include <optional>

class QOpenGLFramebufferObject;
class QPlainTextEdit;

enum class Tool { Select, Pencil, Pen, Highlighter, Line, Arrow, Rect, Circle, Text, Eraser, Track };
enum class CompareMode { Off, Wipe, SideBySide, TopBottom, Blend };
enum class ColorChannel { RGB, Red, Green, Blue };

// GPU frame display (texture upload once per frame, zoom/pan on the quad) with a
// QPainter overlay for grid, onion skin, annotations, selection gizmo, motion trails
// and compare chrome. Text is edited in place with a child editor.
class Viewport : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    // The canvas: on a timeline its size, otherwise the size of the source being shown.
    void setMediaSize(QSize size);
    QSize mediaSize() const { return m_media; }
    void setFrame(FrameCache::Ptr image);
    void clearFrame();

    // Where the edited picture (A) sits on the canvas, its own pixel size and how it is
    // composited. Its annotations and tracks live in its own pixels and follow it. An empty
    // rect = the whole canvas (a source shown on its own).
    void setPrimaryPlacement(QSize size, QRectF rect, float opacity = 1.f, BlendMode blend = BlendMode::Normal);
    QRectF primaryRect() const;
    // Other video tracks at this frame, bottom first: `below` are drawn under A, `above` over it
    // (the higher track always wins, like an editing program).
    struct Layer {
        FrameCache::Ptr frame;
        QRectF rect;  // canvas pixels
        float opacity = 1.f;
        BlendMode blend = BlendMode::Normal;
        FrameCache::Ptr reference;  // filter reference (background plate / previous frame)
    };
    void setLayers(QVector<Layer> below, QVector<Layer> above);
    int layerCount() const { return int(m_below.size() + m_above.size()); }

    // Analysis filter (Filter menu) on every video picture; `threshold` is the effective one.
    void setFilter(const AnalysisFilter& filter, double threshold);
    const AnalysisFilter& filter() const { return m_filter; }
    double filterThreshold() const { return m_filterThreshold; }
    // Background plate (moving silhouette) or previous frame (motion highlight) of A and B.
    void setFilterReference(FrameCache::Ptr a, FrameCache::Ptr b = nullptr);

    void setCompareMode(CompareMode mode);
    CompareMode compareMode() const { return m_compare; }
    double wipe() const { return m_wipe; }
    void setCompareFrame(FrameCache::Ptr image, QSize size);
    void setCompareLabels(const QString& a, const QString& b);

    struct Item {
        Annotation annotation;
        bool inherited;
    };
    void setAnnotations(QVector<Item> items);         // A (main clip)
    void setCompareAnnotations(QVector<Item> items);  // B, editable while comparing
    int activeSide() const { return m_side; }         // 0 = A, 1 = B: where edits go
    // Picks the side explicitly (Blend has both on top of each other, so a click can't tell).
    void setActiveSide(int side);

    struct Ghost {
        Annotation annotation;
        QColor tint;
        double opacity;
        int side = 0;  // 0 = A, 1 = B
    };
    struct FrameGhost {
        FrameCache::Ptr frame;
        double opacity;
        int side = 0;
        QColor tint;                // onion colour for this direction
        double tintStrength = 0.0;  // 0 = true colours
    };
    void setOnion(QVector<Ghost> notes, QVector<FrameGhost> frames);

    // Tracks of the active side (editable) and of the other side (shown only).
    void setTracks(QVector<Track> tracks, int frame, const QString& selectedId, int range, bool frameNumbers);
    void setPassiveTracks(QVector<Track> tracks, int frame);
    void setShowTracks(bool on);

    // selection
    QStringList selectedIds() const { return QStringList(m_sel.begin(), m_sel.end()); }
    void selectAll();
    void clearSelection();
    void setSelection(const QStringList& ids);
    bool isEditingText() const;
    void editText(const QString& id);
    void commitTextEdit();
    void commitEdits(); // settle in-progress edits before changing frame/source

    void setTool(Tool tool);
    Tool tool() const { return m_tool; }
    void setBrush(const QColor& color, double size, double opacity);
    void setTextSize(double px) { m_textSize = px; }
    void setVideoOpacity(double v);
    void setColorChannel(ColorChannel channel);
    ColorChannel colorChannel() const { return m_colorChannel; }
    void setShowAnnotations(bool on);
    void setShowGrid(bool on);
    bool showGrid() const { return m_showGrid; }
    void setPlaceholder(const QString& text);
    void setEndlessScrubbing(bool enabled) { m_endlessScrubbing = enabled; }
    void fitToView();
    void setZoom(double zoom);
    double zoom() const { return m_zoom; }
    // Mirror the view left/right (animators check silhouettes and drawings this way).
    void setFlipHorizontal(bool on);
    bool flipHorizontal() const { return m_flipH; }
    // Frame counter overlay in the lower-left corner.
    void setHud(const QString& text);
    void setShowHud(bool on);
    bool showHud() const { return m_showHud; }
    double displayScale() const;  // screen pixels per media pixel

signals:
    void editStarted();
    void annotationCreated(Annotation a);
    void annotationsEdited(QVector<Annotation> modified);
    void annotationsRemoved(QStringList ids);
    void selectionChanged(int count);
    void eraseBegin();
    void eraseAt(QPointF mediaPoint, double radius);
    void eraseEnd();
    void trackPlaced(QPointF normalized);
    void trackMoved(QString id, QPointF normalized);
    void trackSelected(QString id);
    void filesDropped(QStringList paths);
    void scrubBy(int frames);  // Alt + left drag
    void contextMenuAt(QPoint globalPos, QString annotationId, QString trackId);
    void zoomChanged(double displayScale);
    void activeSideChanged(int side);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void tabletEvent(QTabletEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void leaveEvent(QEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    friend class FeatureRegression;
    struct Tex {
        GLuint id[3] = { 0, 0, 0 };  // RGBA, or Y' / Cb / Cr planes
        QSize size[3];
        int layout = -1;             // 0 = RGBA, 1 = planar Y'CbCr
        FrameCache::Ptr frame;
        bool dirty = false;
    };
    struct GhostInfo {
        double opacity = 0;
        int side = 0;
        QColor tint;
        double tintStrength = 0;
    };
    enum class Drag { None, Pan, Draw, Erase, Move, Scale, Rotate, Marquee, Wipe, TrackMove, Scrub };
    struct Gizmo {
        bool valid = false;
        QPolygonF box;  // media space, 4 corners
        QPointF center;
        QPointF rotateHandle;
    };

    QSizeF virtualSize() const;
    QRectF compareRect() const;
    double fitScale() const;
    QTransform mediaToWidget() const;
    QPointF widgetToMedia(const QPointF& p) const;
    QPointF toNormalized(const QPointF& mediaPt) const;
    // Annotation layers. Everything that edits notes works on the active side (m_side).
    QSize layerMedia(int side) const;
    QTransform annoToWidget(int side) const;
    QTransform annoXf() const { return annoToWidget(m_side); }
    QPointF widgetToAnno(const QPointF& p) const { return annoXf().inverted().map(p); }
    QSize annoMedia() const { return layerMedia(m_side); }
    double annoScale() const { return annoXf().m11(); }
    bool sideEditable(int side) const;
    bool pickSide(const QPointF& widgetPos);
    int sideAt(const QPointF& widgetPos) const;
    QRectF layerClip(int side) const;  // where a side's image is visible, in A pixels
    QVector<Item>& items() { return m_layers[m_side]; }
    const QVector<Item>& items() const { return m_layers[m_side]; }
    struct LayerTex {
        Tex tex;
        Tex ref;
        QRectF rect;
        float opacity = 1.f;
        BlendMode blend = BlendMode::Normal;
    };
    void upload(Tex& t);
    void release(Tex& t);
    void setTexFrame(Tex& t, FrameCache::Ptr frame);
    void drawTexture(Tex& t, const QRectF& mediaRect, float opacity, const QRectF* clip,
                     const QColor& tint = QColor(), float tintStrength = 0.f, Tex* ref = nullptr,
                     BlendMode blend = BlendMode::Normal, bool filtered = true);
    void drawVideoStack();
    void drawGhosts(int side);
    void drawFramebuffer(GLuint texture);
    QTransform primaryToCanvas() const;
    void updateDraft(const QPointF& mediaPt, Qt::KeyboardModifiers mods = {});
    QRectF annotationClip(int side) const;
    bool nearWipe(const QPointF& widgetPos) const;
    void wrapCursor(const QPointF& localPos);

    Item* findItem(const QString& id);
    QString hitItem(const QPointF& mediaPt, bool includeLocked = false) const;
    Gizmo gizmo() const;
    void applyDrag(const QPointF& mediaPt, Qt::KeyboardModifiers mods);
    void updateHoverCursor(const QPointF& widgetPos);
    QString hitTrack(const QPointF& widgetPos) const;
    void beginTextEdit(const Annotation& a, bool isNew);
    void positionTextEditor();
    void finishTextEdit(bool commit);
    void createTextOptions();
    void syncTextOptions();
    void checkTextFocus();

    QSize m_media{ 1920, 1080 };
    Tex m_a;
    Tex m_b;
    Tex m_aRef;
    Tex m_bRef;
    QSize m_aSize;      // A's own pixels (invalid = the canvas)
    QRectF m_aRect;     // where A sits on the canvas (empty = all of it)
    float m_aOpacity = 1.f;
    BlendMode m_aBlend = BlendMode::Normal;
    QVector<LayerTex> m_below;
    QVector<LayerTex> m_above;
    AnalysisFilter m_filter;
    double m_filterThreshold = 0.5;
    QOpenGLFramebufferObject* m_stackFbo = nullptr;  // for Difference: the stack drawn off screen
    GLuint m_dstTex = 0;                             // copy of what is under a Difference layer
    QSize m_dstSize;
    QVector<Tex> m_ghostTex;
    QVector<GhostInfo> m_ghostInfo;
    GLenum m_planeFormat = GL_LUMINANCE;       // one 8-bit channel (Y', Cb, Cr)
    GLenum m_pairFormat = GL_LUMINANCE_ALPHA;  // two channels (NV12 Cb/Cr)
    bool m_glReady = false;
    bool m_flipH = false;
    bool m_showHud = false;
    QString m_hud;
    QSize m_bSize;
    CompareMode m_compare = CompareMode::Off;
    double m_wipe = 0.5;
    QString m_labelA, m_labelB;

    QOpenGLShaderProgram m_program;
    QOpenGLBuffer m_vbo{ QOpenGLBuffer::VertexBuffer };

    QVector<Item> m_layers[2];
    int m_side = 0;
    int m_blendSide = 0;
    QVector<Ghost> m_ghosts;
    std::optional<Annotation> m_draft;
    QPointF m_draftStart;
    bool m_straightStroke = false;

    QSet<QString> m_sel;
    QHash<QString, Annotation> m_dragOrig;
    QPointF m_pressMedia;
    QPointF m_gizmoCenter;
    QPointF m_scaleAxisX{1, 0}, m_scaleAxisY{0, 1};
    int m_resizeHandle = -1; // corners 0..3, edge midpoints 4..7
    QRectF m_marquee;
    bool m_marqueeAdd = false;
    bool m_dragChanged = false;

    QVector<Track> m_tracks;
    QVector<Track> m_passiveTracks;
    int m_passiveFrame = 0;
    int m_trackFrame = 0;
    QString m_selectedTrack;
    int m_trackRange = 0;
    bool m_trackNumbers = false;
    bool m_showTracks = true;
    QString m_dragTrack;

    QPlainTextEdit* m_textEdit = nullptr;
    QWidget* m_textOptions = nullptr;
    Annotation m_textAnn;
    Annotation m_textOriginal;
    bool m_textIsNew = false;
    bool m_textOptionDialog = false;

    Tool m_tool = Tool::Select;
    QColor m_color{ 0xe5, 0x48, 0x3e };
    double m_brushSize = 3.0;
    double m_brushOpacity = 1.0;
    double m_textSize = 36.0;
    double m_eraserSize = 22.0;
    double m_videoOpacity = 1.0;
    ColorChannel m_colorChannel = ColorChannel::RGB;
    bool m_showAnnotations = true;
    bool m_showGrid = false;
    QString m_placeholder;

    double m_zoom = 1.0;
    QPointF m_pan;
    Drag m_drag = Drag::None;
    QPointF m_lastMouse;
    QPointF m_hover{ -1, -1 };
    bool m_tabletStroke = false;
    double m_scrubAcc = 0.0;  // current draft comes from a pen (records pressure)
    bool m_endlessScrubbing = true;
};
