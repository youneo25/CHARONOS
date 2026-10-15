#include "Viewport.h"

#include "Palette.h"
#include "Icons.h"
#include "Lang.h"
#include "model/AnnotationPainter.h"

#include <QApplication>
#include <QAbstractItemDelegate>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QSlider>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QTabletEvent>
#include <QTextDocument>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

// Keep Qt's native combo interaction, with an explicit arrow on its styled button.
class InspectorFontComboBox final : public QFontComboBox {
public:
    using QFontComboBox::QFontComboBox;
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QFontComboBox::paintEvent(event);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const QRect button = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
        QPainter painter(this);
        Icons::icon("chevron-down").paint(&painter,
            QStyle::alignedRect(layoutDirection(), Qt::AlignCenter, QSize(18, 18), button));
    }
};

// Keep Qt's font-name/sample previews, replacing only its coloured font badge.
class NeutralFontIconDelegate final : public QAbstractItemDelegate {
public:
    NeutralFontIconDelegate(QAbstractItemDelegate* preview, QObject* parent)
        : QAbstractItemDelegate(parent), m_preview(preview) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        return m_preview->sizeHint(option, index);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        constexpr int badgeSize = 16;
        QRect gutter = option.rect;
        if (option.direction == Qt::RightToLeft) gutter.setLeft(gutter.right() - badgeSize - 3);
        else gutter.setWidth(badgeSize + 4);

        painter->save();
        painter->setClipRegion(QRegion(option.rect).subtracted(QRegion(gutter)), Qt::IntersectClip);
        m_preview->paint(painter, option, index);
        painter->restore();

        painter->save();
        painter->fillRect(gutter, option.state & QStyle::State_Selected ? option.palette.highlight() : option.palette.base());
        const QRect badge = QStyle::alignedRect(option.direction, Qt::AlignLeft | Qt::AlignVCenter,
                                                QSize(badgeSize, badgeSize), option.rect);
        const QIcon::Mode mode = !(option.state & QStyle::State_Enabled) ? QIcon::Disabled :
                                option.state & QStyle::State_Selected ? QIcon::Active : QIcon::Normal;
        Icons::icon("text").paint(painter, badge, Qt::AlignCenter, mode);
        painter->restore();
    }

private:
    QAbstractItemDelegate* m_preview; // owned by the same font combo
};

const char* kVertexShader = R"(
attribute vec2 aPos;
attribute vec2 aUv;
varying vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(
uniform sampler2D uTex;
uniform float uOpacity;
uniform int uChannel;
varying vec2 vUv;
void main() {
    vec4 c = texture2D(uTex, vUv);
    if (uChannel == 1) c.rgb = vec3(c.r, 0.0, 0.0);
    else if (uChannel == 2) c.rgb = vec3(0.0, c.g, 0.0);
    else if (uChannel == 3) c.rgb = vec3(0.0, 0.0, c.b);
    gl_FragColor = vec4(c.rgb, uOpacity);
}
)";

QRectF fitInto(const QSizeF& src, const QRectF& box)
{
    if (src.isEmpty()) return box;
    const double s = std::min(box.width() / src.width(), box.height() / src.height());
    const QSizeF sz = src * s;
    return QRectF(box.center() - QPointF(sz.width() / 2, sz.height() / 2), sz);
}

double dist(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

constexpr double kHandle = 7.0;         // screen px
constexpr double kRotateOffset = 26.0;  // screen px above the box

} // namespace

Viewport::Viewport(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setAcceptDrops(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 200);
}

Viewport::~Viewport()
{
    makeCurrent();
    for (Tex* t : { &m_a, &m_b })
        if (t->id) glDeleteTextures(1, &t->id);
    for (Tex& t : m_ghostTex)
        if (t.id) glDeleteTextures(1, &t.id);
    m_vbo.destroy();
    doneCurrent();
}

// ================================================================== state setters

void Viewport::setMediaSize(QSize size)
{
    if (size.isValid() && size != m_media) {
        m_media = size;
        fitToView();
    }
}

void Viewport::setFrame(FrameCache::Ptr image)
{
    if (image == m_a.frame) return;
    m_a.frame = std::move(image);
    m_a.dirty = true;
    update();
}

void Viewport::clearFrame()
{
    m_a.frame.reset();
    m_a.dirty = false;
    m_b.frame.reset();
    for (Tex& t : m_ghostTex) t.frame.reset();
    update();
}

void Viewport::setCompareMode(CompareMode mode)
{
    if (mode == m_compare) return;
    m_compare = mode;
    const int side = !sideEditable(1) ? 0 : mode == CompareMode::Blend ? m_blendSide : m_side;
    if (side != m_side) {
        m_sel.clear();
        m_side = side;
        emit activeSideChanged(side);
    }
    fitToView();
}

void Viewport::setCompareFrame(FrameCache::Ptr image, QSize size)
{
    m_bSize = size;
    if (image == m_b.frame) return;
    m_b.frame = std::move(image);
    m_b.dirty = true;
    update();
}

void Viewport::setCompareLabels(const QString& a, const QString& b)
{
    m_labelA = a;
    m_labelB = b;
    update();
}

void Viewport::setAnnotations(QVector<Item> items)
{
    if (m_drag == Drag::Move || m_drag == Drag::Scale || m_drag == Drag::Rotate) return;  // don't fight an active drag
    m_layers[0] = std::move(items);
    QSet<QString> present;
    for (const Item& i : this->items()) present.insert(i.annotation.id);
    const int before = m_sel.size();
    m_sel.intersect(present);
    if (m_sel.size() != before) emit selectionChanged(m_sel.size());
    update();
}

void Viewport::setCompareAnnotations(QVector<Item> items)
{
    if (m_drag == Drag::Move || m_drag == Drag::Scale || m_drag == Drag::Rotate) return;
    m_layers[1] = std::move(items);
    if (m_side == 1) {
        QSet<QString> present;
        for (const Item& i : m_layers[1]) present.insert(i.annotation.id);
        m_sel.intersect(present);
    }
    update();
}

void Viewport::setOnion(QVector<Ghost> notes, QVector<FrameGhost> frames)
{
    m_ghosts = std::move(notes);
    if (m_ghostTex.size() < frames.size()) m_ghostTex.resize(frames.size());
    m_ghostSide.clear();
    for (const FrameGhost& g : frames) m_ghostSide << g.side;
    m_ghostOpacity.resize(frames.size());
    for (qsizetype i = 0; i < m_ghostTex.size(); ++i) {
        FrameCache::Ptr f = i < frames.size() ? frames[i].frame : nullptr;
        if (f != m_ghostTex[i].frame) {
            m_ghostTex[i].frame = f;
            m_ghostTex[i].dirty = bool(f);
        }
        if (i < frames.size()) m_ghostOpacity[i] = frames[i].opacity;
    }
    update();
}

void Viewport::setPassiveTracks(QVector<Track> tracks, int frame)
{
    m_passiveTracks = std::move(tracks);
    m_passiveFrame = frame;
    update();
}

void Viewport::setTracks(QVector<Track> tracks, int frame, const QString& selectedId, int range, bool frameNumbers)
{
    if (m_drag == Drag::TrackMove) return;
    m_tracks = std::move(tracks);
    m_trackFrame = frame;
    m_selectedTrack = selectedId;
    m_trackRange = range;
    m_trackNumbers = frameNumbers;
    update();
}

void Viewport::setShowTracks(bool on) { m_showTracks = on; update(); }

void Viewport::selectAll()
{
    m_sel.clear();
    for (const Item& i : items())
        if (!i.annotation.locked) m_sel.insert(i.annotation.id);
    emit selectionChanged(m_sel.size());
    update();
}

void Viewport::clearSelection()
{
    if (m_sel.isEmpty()) return;
    m_sel.clear();
    emit selectionChanged(0);
    update();
}

void Viewport::setSelection(const QStringList& ids)
{
    m_sel = QSet<QString>(ids.begin(), ids.end());
    emit selectionChanged(m_sel.size());
    update();
}

void Viewport::setTool(Tool tool)
{
    if (isEditingText()) commitTextEdit();
    m_tool = tool;
    m_draft.reset();
    if (tool != Tool::Select && tool != Tool::Text) clearSelection();
    setCursor(tool == Tool::Select ? Qt::ArrowCursor : tool == Tool::Text ? Qt::IBeamCursor : Qt::CrossCursor);
    update();
}

void Viewport::setBrush(const QColor& color, double size, double opacity)
{
    m_color = color;
    m_brushSize = size;
    m_brushOpacity = opacity;
    m_eraserSize = std::max(14.0, size * 4);
}

void Viewport::setVideoOpacity(double v) { m_videoOpacity = v; update(); }
void Viewport::setColorChannel(ColorChannel channel) { m_colorChannel = channel; update(); }
void Viewport::setShowAnnotations(bool on) { m_showAnnotations = on; update(); }
void Viewport::setShowGrid(bool on) { m_showGrid = on; update(); }
void Viewport::setPlaceholder(const QString& text) { m_placeholder = text; update(); }

void Viewport::fitToView()
{
    m_zoom = 1.0;
    m_pan = {};
    positionTextEditor();
    emit zoomChanged(displayScale());
    update();
}

void Viewport::setZoom(double displayScaleTarget)
{
    m_zoom = std::clamp(displayScaleTarget / fitScale(), 0.02, 64.0);
    m_pan = {};
    positionTextEditor();
    emit zoomChanged(displayScale());
    update();
}

// ================================================================== geometry

QSizeF Viewport::virtualSize() const
{
    const QSizeF a(m_media);
    switch (m_compare) {
    case CompareMode::SideBySide: return { a.width() * 2, a.height() };
    case CompareMode::TopBottom: return { a.width(), a.height() * 2 };
    default: return a;
    }
}

QRectF Viewport::compareRect() const
{
    const QSizeF a(m_media);
    QRectF box(QPointF(0, 0), a);
    if (m_compare == CompareMode::SideBySide) box.translate(a.width(), 0);
    if (m_compare == CompareMode::TopBottom) box.translate(0, a.height());
    return fitInto(QSizeF(m_bSize.isValid() ? m_bSize : m_media), box);
}

double Viewport::fitScale() const
{
    const QSizeF v = virtualSize();
    return std::min(width() / v.width(), height() / v.height()) * 0.97;
}

double Viewport::displayScale() const { return fitScale() * m_zoom; }

QTransform Viewport::mediaToWidget() const
{
    const QSizeF v = virtualSize();
    const double s = displayScale();
    QTransform t;
    t.translate(width() / 2.0 + m_pan.x(), height() / 2.0 + m_pan.y());
    t.scale(s, s);
    t.translate(-v.width() / 2.0, -v.height() / 2.0);
    return t;
}

QPointF Viewport::widgetToMedia(const QPointF& p) const { return mediaToWidget().inverted().map(p); }

QPointF Viewport::toNormalized(const QPointF& m) const
{
    const QSize am = annoMedia();
    return QPointF(m.x() / am.width(), m.y() / am.height());
}

QSize Viewport::layerMedia(int side) const { return side == 1 && m_bSize.isValid() ? m_bSize : m_media; }

QTransform Viewport::annoToWidget(int side) const
{
    if (side == 0) return mediaToWidget();
    // B is drawn fitted into its half: B pixels -> that rect -> widget
    const QRectF r = compareRect();
    const QSizeF m(layerMedia(1));
    QTransform t;
    t.translate(r.x(), r.y());
    t.scale(r.width() / m.width(), r.height() / m.height());
    return t * mediaToWidget();
}

bool Viewport::sideEditable(int side) const
{
    return side == 0 || (m_compare != CompareMode::Off && m_b.frame);
}

QRectF Viewport::layerClip(int side) const
{
    QRectF r = side == 1 ? compareRect() : QRectF(QPointF(0, 0), QSizeF(m_media));
    if (m_compare == CompareMode::Wipe) {
        // A on the left of the wipe line, B on the right
        const double x = m_media.width() * m_wipe;
        if (side == 0) r.setRight(std::min(r.right(), x));
        else r.setLeft(std::max(r.left(), x));
    }
    return r;
}

QRectF Viewport::annotationClip(int side) const
{
    QRectF area = mediaToWidget().inverted().mapRect(QRectF(rect()));
    if (m_compare == CompareMode::Wipe || m_compare == CompareMode::SideBySide) {
        const double split = m_compare == CompareMode::Wipe ? m_media.width() * m_wipe : m_media.width();
        if (side == 0) area.setRight(split); else area.setLeft(split);
    } else if (m_compare == CompareMode::TopBottom) {
        if (side == 0) area.setBottom(m_media.height()); else area.setTop(m_media.height());
    }
    return area;
}

int Viewport::sideAt(const QPointF& pos) const
{
    if (!sideEditable(1)) return 0;
    if (m_compare == CompareMode::Blend) return m_blendSide;
    return annotationClip(1).contains(widgetToMedia(pos)) ? 1 : 0;
}

void Viewport::setActiveSide(int side)
{
    m_blendSide = side;
    side = sideEditable(1) ? side : 0;
    if (side == m_side) return;
    if (isEditingText()) finishTextEdit(true);
    m_draft.reset();
    m_sel.clear();
    emit selectionChanged(0);
    m_side = side;
    emit activeSideChanged(side);
    update();
}

bool Viewport::pickSide(const QPointF& pos)
{
    const int side = sideAt(pos);
    if (side == m_side) return false;
    if (isEditingText()) finishTextEdit(true);
    m_draft.reset();
    m_sel.clear();
    emit selectionChanged(0);
    m_side = side;
    emit activeSideChanged(side);
    update();
    return true;
}

// ================================================================== GL

void Viewport::initializeGL()
{
    initializeOpenGLFunctions();
    m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    m_program.bindAttributeLocation("aPos", 0);
    m_program.bindAttributeLocation("aUv", 1);
    m_program.link();
    m_vbo.create();
}

void Viewport::resizeGL(int, int)
{
    positionTextEditor();
    emit zoomChanged(displayScale());
}

void Viewport::upload(Tex& t)
{
    if (!t.frame || t.frame->isNull()) return;
    const QImage& img = *t.frame;
    if (!t.id) glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (t.size != img.size()) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
        t.size = img.size();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width(), img.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    }
    t.dirty = false;
}

void Viewport::drawTexture(Tex& t, const QRectF& mediaRect, float opacity, const QRectF* clip)
{
    if (!t.frame) return;
    if (t.dirty) upload(t);
    const QTransform xf = mediaToWidget();
    const QRectF r = xf.mapRect(mediaRect);
    const float W = float(width()), H = float(height());
    auto nx = [&](qreal x) { return float(2.0 * x / W - 1.0); };
    auto ny = [&](qreal y) { return float(1.0 - 2.0 * y / H); };
    const float l = nx(r.left()), rt = nx(r.right()), tp = ny(r.top()), b = ny(r.bottom());
    const float verts[] = {
        l, tp, 0, 0,   l, b, 0, 1,   rt, b, 1, 1,
        l, tp, 0, 0,   rt, b, 1, 1,  rt, tp, 1, 0,
    };

    const qreal dpr = devicePixelRatioF();
    if (clip) {
        const QRectF c = xf.mapRect(*clip);
        glEnable(GL_SCISSOR_TEST);
        glScissor(int(c.left() * dpr), int((H - c.bottom()) * dpr), int(std::ceil(c.width() * dpr)),
                  int(std::ceil(c.height() * dpr)));
    }

    m_program.bind();
    m_vbo.bind();
    m_vbo.allocate(verts, sizeof verts);
    m_program.enableAttributeArray(0);
    m_program.enableAttributeArray(1);
    m_program.setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * sizeof(float));
    m_program.setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));
    m_program.setUniformValue("uTex", 0);
    m_program.setUniformValue("uOpacity", opacity);
    m_program.setUniformValue("uChannel", int(m_colorChannel));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t.id);
    const bool pixelPeep = (r.width() / std::max(1, t.size.width())) * dpr > 1.5;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, pixelPeep ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    if (clip) glDisable(GL_SCISSOR_TEST);

    m_program.disableAttributeArray(0);
    m_program.disableAttributeArray(1);
    m_vbo.release();
    m_program.release();
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ================================================================== painting

void Viewport::paintGL()
{
    const QTransform xf = mediaToWidget();
    const QSizeF msize(m_media);
    const QRectF mediaRect(QPointF(0, 0), msize);
    const bool comparing = m_compare != CompareMode::Off;
    const double s = displayScale();

    QPainter p(this);
    p.beginNativePainting();
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, int(width() * dpr), int(height() * dpr));
    glClearColor(Pal::viewer.redF(), Pal::viewer.greenF(), Pal::viewer.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    drawTexture(m_a, mediaRect, float(m_videoOpacity), nullptr);
    if (comparing && m_b.frame) {
        const QRectF br = compareRect();
        if (m_compare == CompareMode::Wipe) {
            const QRectF clip(QPointF(msize.width() * m_wipe, 0), QPointF(msize.width(), msize.height()));
            drawTexture(m_b, br, float(m_videoOpacity), &clip);
        } else {
            drawTexture(m_b, br, m_compare == CompareMode::Blend ? 0.5f : float(m_videoOpacity), nullptr);
        }
    }
    // Ghosts must be composited after both base images, clipped to their own side.
    for (qsizetype i = 0; i < m_ghostTex.size() && i < m_ghostOpacity.size(); ++i) {
        const int side = i < m_ghostSide.size() ? m_ghostSide[i] : 0;
        if (!m_ghostTex[i].frame || !sideEditable(side)) continue;
        const QRectF clip = layerClip(side);
        drawTexture(m_ghostTex[i], side ? compareRect() : mediaRect,
                    float(m_ghostOpacity[i]), comparing ? &clip : nullptr);
    }
    p.endNativePainting();

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setTransform(xf);

    if (!m_a.frame && !m_placeholder.isEmpty()) {
        p.resetTransform();
        p.setPen(Pal::textFaint);
        QFont f = font();
        f.setPixelSize(13);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, m_placeholder);
        return;
    }

    if (m_showGrid) {
      for (int side = 0; side < 2; ++side) {
        if (!sideEditable(side)) continue;
        p.save();
        if (comparing) p.setClipRect(layerClip(side));
        p.setTransform(annoToWidget(side));
        const QSizeF msize(layerMedia(side));
        QPen grid(QColor(255, 255, 255, 34), 0);
        grid.setCosmetic(true);
        p.setPen(grid);
        for (int i = 1; i < 12; ++i) {
            p.drawLine(QPointF(msize.width() * i / 12.0, 0), QPointF(msize.width() * i / 12.0, msize.height()));
            p.drawLine(QPointF(0, msize.height() * i / 12.0), QPointF(msize.width(), msize.height() * i / 12.0));
        }
        QPen thirds(QColor(255, 255, 255, 80), 0);
        thirds.setCosmetic(true);
        p.setPen(thirds);
        for (int i = 1; i < 3; ++i) {
            p.drawLine(QPointF(msize.width() * i / 3.0, 0), QPointF(msize.width() * i / 3.0, msize.height()));
            p.drawLine(QPointF(0, msize.height() * i / 3.0), QPointF(msize.width(), msize.height() * i / 3.0));
        }
        p.restore();
      }
    }

    if (m_showAnnotations) {
        const bool drawing = m_draft.has_value();
        const QString editing = m_textEdit && m_textEdit->isVisible() ? m_textAnn.id : QString();
        for (int side = 0; side < 2; ++side) {
            if (!sideEditable(side)) continue;
            const QSizeF lm(layerMedia(side));
            p.save();
            if (comparing) p.setClipRect(annotationClip(side));  // partition only, including the space outside the image
            p.setTransform(annoToWidget(side));
            for (const Ghost& g : m_ghosts)
                if (g.side == side) AnnotationPainter::paint(p, g.annotation, lm, g.opacity, false, g.tint);
            for (const Item& item : m_layers[side])
                if (item.annotation.id != editing)
                    AnnotationPainter::paint(p, item.annotation, lm, 1.0, drawing && side == m_side && item.inherited);
            if (m_draft && side == m_side) AnnotationPainter::paint(p, *m_draft, lm, 1.0, false);
            p.restore();
        }
        p.setTransform(xf);
    }
    if (sideEditable(1) && m_compare != CompareMode::Blend) {
        // thin outline on the side that receives edits
        const QRectF r = layerClip(m_side);
        p.save();
        p.resetTransform();
        QPen pen(QColor(217, 119, 87, 210), 1.5);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(xf.mapRect(r).adjusted(-1.5, -1.5, 1.5, 1.5));
        p.restore();
    }

    if (m_showTracks) {
        // active side's tracks on top; the other side's are drawn too but can't be picked
        for (int pass = 0; pass < 2; ++pass) {
            const int side = pass == 0 ? 1 - m_side : m_side;
            const QVector<Track>& tracks = pass == 0 ? m_passiveTracks : m_tracks;
            if (tracks.isEmpty() || !sideEditable(side)) continue;
            const QTransform lx = annoToWidget(side);
            TrackPainter::Style st;
            st.range = m_trackRange;
            st.frameNumbers = m_trackNumbers;
            st.unit = 1.0 / lx.m11();  // one screen pixel in that layer's pixels
            st.selectedId = pass == 1 ? m_selectedTrack : QString();
            p.save();
            if (comparing) p.setClipRect(layerClip(side));
            p.setTransform(lx);
            TrackPainter::paint(p, tracks, pass == 0 ? m_passiveFrame : m_trackFrame, QSizeF(layerMedia(side)), st);
            p.restore();
        }
        p.setTransform(xf);
    }

    // selection gizmo
    p.resetTransform();
    const Gizmo g = gizmo();
    if (g.valid && m_showAnnotations) {
        const QTransform ax = annoXf();
        const QPolygonF box = ax.map(g.box);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 120), 3));
        p.drawPolygon(box);
        QPen dash(Pal::textStrong, 1, Qt::DashLine);
        dash.setDashPattern({ 4, 3 });
        p.setPen(dash);
        p.drawPolygon(box);
        if (m_sel.size() > 1)
            for (const Item& i : items())
                if (m_sel.contains(i.annotation.id)) {
                    p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::DotLine));
                    p.drawPolygon(ax.map(AnnotationPainter::outline(i.annotation, QSizeF(annoMedia()))));
                }
        const QPointF top = (box[0] + box[1]) / 2;
        const QPointF rot = ax.map(g.rotateHandle);
        p.setPen(QPen(Pal::textStrong, 1));
        p.drawLine(top, rot);
        p.setBrush(Pal::panel);
        p.drawEllipse(rot, 5, 5);
        for (const QPointF& c : box) p.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
        for (int i = 0; i < 4; ++i) {
            const QPointF c = (box[i] + box[(i+1)%4]) / 2;
            p.drawRect(QRectF(c.x()-4, c.y()-4, 8, 8));
        }
    }
    if (m_drag == Drag::Marquee) {
        const QRectF r = annoXf().mapRect(m_marquee);
        p.setPen(QPen(QColor(255, 255, 255, 170), 1, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 18));
        p.drawRect(r);
    }

    if (comparing) {
        QFont f = font();
        f.setPixelSize(11);
        p.setFont(f);
        auto label = [&](const QRectF& mediaBox, const QString& tag, const QString& name, bool right, bool active) {
            const QRectF w = xf.mapRect(mediaBox);
            const QString text = tag + QStringLiteral("  ") + name;
            const QFontMetrics fm(f);
            const int tw = fm.horizontalAdvance(text) + 14;
            const QRectF pill(right ? w.right() - tw - 8 : w.left() + 8, w.top() + 8, tw, 20);
            p.setPen(active ? QPen(QColor(217, 119, 87), 1.2) : QPen(Qt::NoPen));
            p.setBrush(QColor(20, 20, 19, 190));
            p.drawRoundedRect(pill, 6, 6);
            p.setPen(Pal::text);
            p.drawText(pill, Qt::AlignCenter, text);
        };
        if (m_compare == CompareMode::Wipe) {
            const double x = xf.map(QPointF(msize.width() * m_wipe, 0)).x();
            const QRectF w = xf.mapRect(mediaRect);
            p.setPen(QPen(QColor(0, 0, 0, 120), 3));
            p.drawLine(QPointF(x, w.top()), QPointF(x, w.bottom()));
            p.setPen(QPen(QColor(255, 255, 255, 220), 1));
            p.drawLine(QPointF(x, w.top()), QPointF(x, w.bottom()));
            p.setBrush(Pal::text);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(x, w.center().y()), 5, 5);
            label(mediaRect, "A", m_labelA, false, m_b.frame && m_side == 0);
            label(mediaRect, "B", m_labelB, true, m_side == 1);
        } else {
            label(mediaRect, "A", m_labelA, false, m_b.frame && m_side == 0);
            label(compareRect(), m_compare == CompareMode::Blend ? "B 50%" : "B", m_labelB, m_compare == CompareMode::Blend, m_side == 1);
        }
    }

    if (m_tool == Tool::Eraser && m_hover.x() >= 0) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 170), 1));
        p.drawEllipse(m_hover, m_eraserSize / 2, m_eraserSize / 2);
    }
}

// ================================================================== selection helpers

Viewport::Item* Viewport::findItem(const QString& id)
{
    for (Item& i : items())
        if (i.annotation.id == id) return &i;
    return nullptr;
}

QString Viewport::hitItem(const QPointF& mediaPt, bool includeLocked) const
{
    const double tol = 5.0 / annoScale();
    for (auto it = items().rbegin(); it != items().rend(); ++it)
        if ((includeLocked || !it->annotation.locked) && AnnotationPainter::hitShape(it->annotation, QSizeF(annoMedia()), mediaPt, tol))
            return it->annotation.id;
    return {};
}

Viewport::Gizmo Viewport::gizmo() const
{
    Gizmo g;
    QVector<const Annotation*> sel;
    for (const Item& i : items())
        if (m_sel.contains(i.annotation.id)) sel << &i.annotation;
    if (sel.isEmpty()) return g;
    const QSizeF m(annoMedia());
    if (sel.size() == 1) {
        g.box = AnnotationPainter::outline(*sel[0], m);
        g.center = AnnotationPainter::boundingBox(*sel[0], m).center();
    } else {
        QRectF r;
        for (const Annotation* a : sel) r = r.united(AnnotationPainter::outline(*a, m).boundingRect());
        g.box = QPolygonF(r);
        g.box.removeLast();
        g.center = r.center();
    }
    if (g.box.size() != 4) return g;
    const QPointF top = (g.box[0] + g.box[1]) / 2;
    QPointF dir = top - g.center;
    const double len = std::hypot(dir.x(), dir.y());
    dir = len > 1e-6 ? dir / len : QPointF(0, -1);
    g.rotateHandle = top + dir * (kRotateOffset / annoScale());
    g.valid = true;
    return g;
}

void Viewport::applyDrag(const QPointF& mp, Qt::KeyboardModifiers mods)
{
    const QSizeF m(annoMedia());
    for (auto it = m_dragOrig.constBegin(); it != m_dragOrig.constEnd(); ++it) {
        Item* item = findItem(it.key());
        if (!item) continue;
        Annotation a = it.value();
        if (m_drag == Drag::Move) {
            QPointF d = mp - m_pressMedia;
            if (mods & Qt::ShiftModifier) (std::abs(d.x()) > std::abs(d.y()) ? d.ry() : d.rx()) = 0;  // axis lock
            AnnotationTransform::translate(a, d, m);
        } else if (m_drag == Drag::Scale) {
            const QPointF start = m_pressMedia - m_gizmoCenter, now = mp - m_gizmoCenter;
            auto dot = [](QPointF a, QPointF b) { return a.x()*b.x() + a.y()*b.y(); };
            auto ratio = [&](QPointF axis) {
                const double d = dot(start, axis);
                return std::abs(d) < 1e-6 ? 1.0 : std::clamp(dot(now, axis)/d, 0.02, 100.0);
            };
            double x = ratio(m_scaleAxisX), y = ratio(m_scaleAxisY);
            if (m_resizeHandle == 4 || m_resizeHandle == 6) x = 1;
            if (m_resizeHandle == 5 || m_resizeHandle == 7) y = 1;
            if (mods & Qt::ShiftModifier) {
                const double k = m_resizeHandle == 4 || m_resizeHandle == 6 ? y : x;
                AnnotationTransform::scaleAbout(a, k, m_gizmoCenter, m);
            } else {
              if (m_sel.size() == 1) {
                AnnotationTransform::stretchAbout(a, x, y, m_gizmoCenter, m);
              } else {
                // A group uses the gizmo's axes for its positions and each item's local shape.
                const QPointF c = AnnotationPainter::boundingBox(a, m).center();
                const QPointF d = c - m_gizmoCenter;
                AnnotationTransform::translate(a, m_scaleAxisX * (dot(d,m_scaleAxisX)*(x-1)) +
                                                   m_scaleAxisY * (dot(d,m_scaleAxisY)*(y-1)), m);
                a.stretchX *= x;
                a.stretchY *= y;
              }
            }
        } else if (m_drag == Drag::Rotate) {
            double th = std::atan2(mp.y() - m_gizmoCenter.y(), mp.x() - m_gizmoCenter.x()) -
                        std::atan2(m_pressMedia.y() - m_gizmoCenter.y(), m_pressMedia.x() - m_gizmoCenter.x());
            if (mods & Qt::ShiftModifier) th = std::round(th / (M_PI / 12)) * (M_PI / 12);  // 15° steps
            AnnotationTransform::rotateAbout(a, th, m_gizmoCenter, m);
        }
        item->annotation = a;
    }
    m_dragChanged = true;
}

void Viewport::updateHoverCursor(const QPointF& pos)
{
    if (m_tool != Tool::Select || m_drag != Drag::None) return;
    if (nearWipe(pos)) return setCursor(Qt::SplitHCursor);
    const Gizmo g = gizmo();
    const QTransform xf = annoXf();
    if (g.valid) {
        if (dist(xf.map(g.rotateHandle), pos) < kHandle + 2) return setCursor(Qt::CrossCursor);
        for (const QPointF& c : g.box)
            if (dist(xf.map(c), pos) < kHandle + 2) return setCursor(Qt::SizeFDiagCursor);
        for (int i = 0; i < 4; ++i)
            if (dist(xf.map((g.box[i]+g.box[(i+1)%4])/2), pos) < kHandle+2)
                return setCursor(i%2 ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        if (xf.map(g.box).containsPoint(pos, Qt::OddEvenFill)) return setCursor(Qt::SizeAllCursor);
    }
    setCursor(hitItem(widgetToAnno(pos)).isEmpty() ? Qt::ArrowCursor : Qt::SizeAllCursor);
}

QString Viewport::hitTrack(const QPointF& pos) const
{
    if (!m_showTracks) return {};
    const QTransform xf = annoXf();
    const QSize am = annoMedia();
    QString best;
    double bestD = 14.0;
    for (const Track& t : m_tracks)
        if (const auto p = t.visibleAt(m_trackFrame) ? t.at(m_trackFrame) : std::nullopt) {
            const double d = dist(xf.map(QPointF(p->x() * am.width(), p->y() * am.height())), pos);
            if (d < bestD) {
                bestD = d;
                best = t.id;
            }
        }
    return best;
}

// ================================================================== text editing

bool Viewport::isEditingText() const { return m_textEdit && m_textEdit->isVisible(); }

void Viewport::createTextOptions()
{
    m_textOptions = new QWidget(this);
    m_textOptions->setObjectName("TextOptions");
    m_textOptions->setAttribute(Qt::WA_StyledBackground);
    m_textOptions->setAttribute(Qt::WA_NoMousePropagation);
    m_textOptions->setStyleSheet("QWidget#TextOptions { background: #30302e; border: 1px solid #6e6c66; border-radius: 6px; }"
                                 "QPushButton { padding: 5px 8px; } QDoubleSpinBox, QSpinBox { min-width: 52px; }"
                                 "QLabel { color: #faf9f5; }"
                                 "QFontComboBox { padding-right: 36px; }"
                                 "QFontComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 30px;"
                                 " background: #484742; border-left: 1px solid #858279; border-top-right-radius: 5px; border-bottom-right-radius: 5px; }"
                                 "QFontComboBox::drop-down:hover { background: #5c5a53; }"
                                 "QFontComboBox::down-arrow { image: none; }"
                                 "QSpinBox#TextOpacity { background: #1f1e1d; color: #faf9f5; border: 1px solid #858279;"
                                 " font-size: 14px; font-weight: 600; padding: 5px 8px; min-width: 64px; }"
                                 "QSlider#TextOpacitySlider::sub-page:horizontal { background: #e5e3db; }"
                                 "QSlider#TextOpacitySlider::groove:horizontal { height: 5px; background: #6e6c66; }");
    auto* layout = new QGridLayout(m_textOptions);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(7);
    auto* family = new InspectorFontComboBox(m_textOptions);
    family->setItemDelegate(new NeutralFontIconDelegate(family->itemDelegate(), family));
    family->setObjectName("TextFontFamily");
    family->setMinimumWidth(70);
    family->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    family->setToolTip(L("Font", "글꼴"));
    family->setMinimumHeight(30);
    auto* size = new QDoubleSpinBox(m_textOptions);
    size->setObjectName("TextFontSize");
    size->setRange(1, 2000); size->setDecimals(1); size->setSuffix(" px");
    size->setKeyboardTracking(false);
    size->setToolTip(L("Font size (media pixels)", "글자 크기 (미디어 픽셀)"));
    layout->addWidget(family, 0, 0, 1, 4);
    layout->addWidget(size, 0, 4, 1, 2);
    auto button = [&](const char* name, const QString& label, int row, int col, bool checkable = false) {
        auto* b = new QPushButton(label, m_textOptions);
        b->setObjectName(name); b->setCheckable(checkable);
        layout->addWidget(b, row, col);
        return b;
    };
    auto* bold = button("TextBold", "B", 1, 0, true);
    QFont bf = bold->font(); bf.setBold(true); bold->setFont(bf);
    bold->setToolTip(L("Bold", "굵게"));
    auto* italic = button("TextItalic", "I", 1, 1, true);
    QFont it = italic->font(); it.setItalic(true); italic->setFont(it);
    italic->setToolTip(L("Italic", "기울임"));
    auto* color = button("TextColor", L("Color", "글자색"), 1, 2);
    color->setToolTip(L("Text Color", "글자색"));
    auto* bg = new QCheckBox(L("Background", "배경"), m_textOptions);
    bg->setObjectName("TextBackground");
    layout->addWidget(bg, 1, 3, 1, 2);
    auto* bgColor = button("TextBackgroundColor", L("Color", "배경색"), 1, 5);
    bgColor->setToolTip(L("Background Color", "배경색"));
    layout->addWidget(new QLabel(L("Opacity", "불투명도"), m_textOptions), 2, 0, 1, 2);
    auto* opacity = new QSpinBox(m_textOptions);
    opacity->setObjectName("TextOpacity"); opacity->setRange(0, 100); opacity->setSuffix(" %");
    opacity->setAlignment(Qt::AlignCenter);
    opacity->setMinimumHeight(32);
    opacity->setToolTip(L("Text opacity: 0% transparent, 100% opaque", "글자 불투명도: 0% 투명, 100% 불투명"));
    opacity->setAccessibleName(L("Text opacity (%)", "글자 불투명도 (%)"));
    opacity->setKeyboardTracking(false);
    auto* opacitySlider = new QSlider(Qt::Horizontal, m_textOptions);
    opacitySlider->setObjectName("TextOpacitySlider");
    opacitySlider->setRange(0, 100);
    opacitySlider->setMinimumWidth(90);
    opacitySlider->setToolTip(opacity->toolTip());
    opacitySlider->setAccessibleName(opacity->accessibleName());
    layout->addWidget(opacitySlider, 2, 2, 1, 3);
    layout->addWidget(opacity, 2, 5);
    auto* done = button("TextDone", L("Apply", "적용"), 3, 4);
    auto* cancel = button("TextCancel", L("Cancel", "취소"), 3, 5);
    connect(opacitySlider, &QSlider::valueChanged, opacity, &QSpinBox::setValue);
    connect(opacity, &QSpinBox::valueChanged, opacitySlider, &QSlider::setValue);
    connect(family, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        m_textAnn.fontFamily = f.family(); positionTextEditor();
    });
    connect(size, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_textAnn.fontSize = v; positionTextEditor();
    });
    connect(bold, &QPushButton::toggled, this, [this](bool on) { m_textAnn.bold = on; positionTextEditor(); });
    connect(italic, &QPushButton::toggled, this, [this](bool on) { m_textAnn.italic = on; positionTextEditor(); });
    connect(opacity, &QSpinBox::valueChanged, this, [this](int v) { m_textAnn.opacity = v / 100.0; positionTextEditor(); });
    connect(bg, &QCheckBox::toggled, this, [this](bool on) {
        m_textAnn.useBg = on;
        if (m_textAnn.bgColor.isEmpty()) m_textAnn.bgColor = "#202020";
        positionTextEditor();
    });
    auto chooseColor = [this](bool background) {
        m_textOptionDialog = true;
        const QColor initial = background ? QColor(m_textAnn.bgColor.isEmpty() ? "#202020" : m_textAnn.bgColor) : m_textAnn.color;
        const QColor chosen = QColorDialog::getColor(initial, m_textOptions, background ? L("Background Color", "배경색") : L("Text Color", "글자색"));
        m_textOptionDialog = false;
        if (chosen.isValid()) {
            if (background) { m_textAnn.bgColor = chosen.name(); m_textAnn.useBg = true; }
            else m_textAnn.color = chosen;
            syncTextOptions(); positionTextEditor();
        }
        if (isEditingText()) m_textEdit->setFocus();
    };
    connect(color, &QPushButton::clicked, this, [chooseColor] { chooseColor(false); });
    connect(bgColor, &QPushButton::clicked, this, [chooseColor] { chooseColor(true); });
    connect(done, &QPushButton::clicked, this, [this] { finishTextEdit(true); });
    connect(cancel, &QPushButton::clicked, this, [this] { finishTextEdit(false); });
    for (QWidget* w : m_textOptions->findChildren<QWidget*>()) w->installEventFilter(this);
    m_textOptions->hide();
}

void Viewport::syncTextOptions()
{
    const auto widgets = m_textOptions->findChildren<QWidget*>();
    // Block setters so opening an editor never changes the annotation.
    for (QWidget* w : widgets) w->blockSignals(true);
    m_textOptions->findChild<QFontComboBox*>("TextFontFamily")->setCurrentFont(QFont(m_textAnn.fontFamily));
    m_textOptions->findChild<QDoubleSpinBox*>("TextFontSize")->setValue(m_textAnn.fontSize);
    m_textOptions->findChild<QPushButton*>("TextBold")->setChecked(m_textAnn.bold);
    m_textOptions->findChild<QPushButton*>("TextItalic")->setChecked(m_textAnn.italic);
    m_textOptions->findChild<QSpinBox*>("TextOpacity")->setValue(qRound(m_textAnn.opacity * 100));
    m_textOptions->findChild<QSlider*>("TextOpacitySlider")->setValue(qRound(m_textAnn.opacity * 100));
    m_textOptions->findChild<QCheckBox*>("TextBackground")->setChecked(m_textAnn.useBg);
    for (QWidget* w : widgets) w->blockSignals(false);
}

void Viewport::checkTextFocus()
{
    if (!isEditingText() || m_textOptionDialog) return;
    QWidget* focus = QApplication::focusWidget();
    if (focus && (focus == m_textEdit || m_textEdit->isAncestorOf(focus) ||
                  focus == m_textOptions || m_textOptions->isAncestorOf(focus))) return;
    if (QApplication::activePopupWidget()) return;
    finishTextEdit(true);
}

void Viewport::beginTextEdit(const Annotation& a, bool isNew)
{
    // The argument may point into a layer replaced by finishTextEdit's signal.
    const Annotation next = a;
    if (isEditingText()) finishTextEdit(true);
    emit editStarted();
    if (!m_textEdit) {
        m_textEdit = new QPlainTextEdit(this);
        m_textEdit->setFrameStyle(QFrame::NoFrame);
        m_textEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_textEdit->document()->setDocumentMargin(0);
        m_textEdit->installEventFilter(this);
        connect(m_textEdit, &QPlainTextEdit::textChanged, this, &Viewport::positionTextEditor);
        createTextOptions();
    }
    m_textAnn = next;
    m_textOriginal = next;
    m_textIsNew = isNew;
    syncTextOptions();
    m_textEdit->setPlainText(next.text);
    positionTextEditor();
    m_textEdit->show();
    m_textOptions->show();
    m_textOptions->raise();
    m_textEdit->setFocus();
    m_textEdit->moveCursor(QTextCursor::End);
    update();
}

void Viewport::positionTextEditor()
{
    if (!m_textEdit) return;
    const Annotation& a = m_textAnn;
    QFont f(a.fontFamily);
    f.setBold(a.bold); f.setItalic(a.italic);
    const double px = std::max(6.0, a.fontSize * a.scale * a.stretchY * annoScale());
    f.setPixelSize(int(std::round(px)));
    // The app style sheet sets a global font size, so the size has to go into this widget's own sheet.
    const QColor bg = a.useBg ? QColor(a.bgColor) : QColor(0, 0, 0, 60);
    QString familyCss = a.fontFamily;
    familyCss.replace('\\', "\\\\"); familyCss.replace('"', "\\\"");
    m_textEdit->setStyleSheet(QStringLiteral("QPlainTextEdit { font-family: \"%4\"; font-weight: %5; font-style: %6; font-size: %2px; background: %3;"
                                             " color: %1; border: 1px dashed rgba(255,255,255,150); border-radius: 0; padding: 0;"
                                             " selection-background-color: rgba(255,255,255,70); }")
                                  .arg(QColor(a.color.red(), a.color.green(), a.color.blue(), qRound(a.opacity * 255)).name(QColor::HexArgb))
                                  .arg(f.pixelSize()).arg(bg.name(QColor::HexArgb)).arg(familyCss)
                                  .arg(a.bold ? "bold" : "normal").arg(a.italic ? "italic" : "normal"));
    m_textEdit->setFont(f);
    const QFontMetricsF fm(f);
    double w = fm.horizontalAdvance(QStringLiteral("M")) * 2;
    const QStringList lines = m_textEdit->toPlainText().split('\n');
    for (const QString& l : lines) w = std::max(w, fm.horizontalAdvance(l) + fm.horizontalAdvance(QStringLiteral("M")));
    const double h = fm.lineSpacing() * std::max<qsizetype>(1, lines.size()) + 4;
    const QSize am = annoMedia();
    const QPointF baseOrigin((a.x * a.scale + a.tx) * am.width(), (a.y * a.scale + a.ty) * am.height());
    const QPointF c = AnnotationPainter::boundingBox(a, QSizeF(am)).center();
    const QPointF d = baseOrigin - c;
    const QPointF local(d.x()*a.stretchX, d.y()*a.stretchY);
    const QPointF origin = annoXf().map(c + QPointF(local.x()*std::cos(a.angle)-local.y()*std::sin(a.angle),
                                                 local.x()*std::sin(a.angle)+local.y()*std::cos(a.angle)));
    w *= a.stretchX / a.stretchY;
    m_textEdit->setGeometry(QRect(QPoint(int(origin.x()) - 2, int(origin.y()) - 2), QSize(int(w) + 6, int(h) + 2)));
    if (m_textOptions) {
        const int panelWidth = std::min(width(), 470);
        m_textOptions->setFixedWidth(panelWidth);
        m_textOptions->adjustSize();
        const int panelX = std::clamp(m_textEdit->x(), 0, std::max(0, width() - panelWidth));
        const int panelY = std::clamp(m_textEdit->y() - m_textOptions->height() - 5, 0, std::max(0, height() - m_textOptions->height()));
        m_textOptions->move(panelX, panelY);
        // Keep the controls above the editable box even at the top edge.
        if (m_textEdit->y() < panelY + m_textOptions->height() + 5)
            m_textEdit->move(m_textEdit->x(), panelY + m_textOptions->height() + 5);
    }
}

void Viewport::finishTextEdit(bool commit)
{
    if (!isEditingText()) return;
    QString text = m_textEdit->toPlainText();
    while (text.endsWith('\n') || text.endsWith(' ')) text.chop(1);
    m_textEdit->hide();
    m_textOptions->hide();
    setFocus();
    if (commit) {
        Annotation a = m_textAnn;
        a.text = text;
        if (m_textIsNew) {
            if (!text.isEmpty()) emit annotationCreated(a);
        } else if (text.isEmpty()) {
            emit annotationsRemoved({ a.id });
        } else if (a.toJson() != m_textOriginal.toJson()) {
            if (Item* i = findItem(a.id)) i->annotation = a;
            emit annotationsEdited({ a });
        }
    }
    update();
}

void Viewport::commitTextEdit() { finishTextEdit(true); }

void Viewport::commitEdits()
{
    finishTextEdit(true);
    switch (m_drag) {
    case Drag::Draw: {
        m_drag = Drag::None;
        m_tabletStroke = false;
        if (m_draft) {
            const Annotation a = *m_draft;
            m_draft.reset();
            const QRectF box = AnnotationPainter::boundingBox(a, QSizeF(annoMedia()));
            if (a.type == "stroke" || box.width() > 12 || box.height() > 12) emit annotationCreated(a);
        }
        break;
    }
    case Drag::Erase: case Drag::Move: case Drag::Scale: case Drag::Rotate: case Drag::TrackMove: {
        const QPointF pos = m_hover.x() >= 0 ? m_hover : m_lastMouse;
        QMouseEvent release(QEvent::MouseButtonRelease, pos, mapToGlobal(pos.toPoint()),
                            Qt::LeftButton, Qt::NoButton, QApplication::keyboardModifiers());
        mouseReleaseEvent(&release);
        m_tabletStroke = false;
        break;
    }
    default: break;
    }
}

void Viewport::editText(const QString& id)
{
    if (const Item* i = findItem(id); i && !i->annotation.locked && i->annotation.type == "text") beginTextEdit(i->annotation, false);
}

bool Viewport::eventFilter(QObject* obj, QEvent* e)
{
    auto* widget = qobject_cast<QWidget*>(obj);
    if (m_textOptions && widget && m_textOptions->isAncestorOf(widget)) {
        if (e->type() == QEvent::FocusOut) QTimer::singleShot(0, this, &Viewport::checkTextFocus);
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
            finishTextEdit(false); return true;
        }
        if (e->type() == QEvent::ShortcutOverride) e->accept();
    }
    if (obj == m_textEdit) {
        if (e->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Escape) {
                finishTextEdit(false);
                return true;
            }
            if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && !(k->modifiers() & Qt::ShiftModifier)) {
                finishTextEdit(true);
                return true;
            }
            k->accept();  // keep single-letter tool shortcuts from firing while typing
        } else if (e->type() == QEvent::ShortcutOverride) {
            e->accept();
            return false;
        } else if (e->type() == QEvent::FocusOut) {
            QTimer::singleShot(0, this, &Viewport::checkTextFocus);
        }
    }
    return QOpenGLWidget::eventFilter(obj, e);
}

// ================================================================== mouse

void Viewport::updateDraft(const QPointF& mediaPt, Qt::KeyboardModifiers mods)
{
    if (!m_draft) return;
    const QPointF n = toNormalized(mediaPt);
    const QPointF s = toNormalized(m_draftStart);
    Annotation& a = *m_draft;
    if (a.type == "stroke") {
        m_straightStroke = m_straightStroke || bool(mods & Qt::ShiftModifier);
        if (m_straightStroke) {
            a.points = {s, n};
            if (!a.pressures.isEmpty()) a.pressures = {a.pressures.first(), a.pressures.last()};
            return;
        }
        const QPointF last = a.points.isEmpty() ? QPointF(-1, -1) : a.points.last();
        const QPointF d((n.x() - last.x()) * annoMedia().width(), (n.y() - last.y()) * annoMedia().height());
        if (std::hypot(d.x(), d.y()) >= 0.75) a.points.append(n);
    } else if (a.type == "line" || a.type == "arrow") {
        a.x2 = n.x();
        a.y2 = n.y();
    } else if (a.type == "rect") {
        a.x = std::min(s.x(), n.x());
        a.y = std::min(s.y(), n.y());
        a.w = std::abs(n.x() - s.x());
        a.h = std::abs(n.y() - s.y());
    } else if (a.type == "circle") {
        a.cx = (s.x() + n.x()) / 2;
        a.cy = (s.y() + n.y()) / 2;
        a.rx = std::abs(n.x() - s.x()) / 2;
        a.ry = std::abs(n.y() - s.y()) / 2;
    }
}

bool Viewport::nearWipe(const QPointF& pos) const
{
    if (m_compare != CompareMode::Wipe) return false;
    const double x = mediaToWidget().map(QPointF(m_media.width() * m_wipe, 0)).x();
    return std::abs(pos.x() - x) < 8;
}

void Viewport::mousePressEvent(QMouseEvent* e)
{
    if (m_tabletStroke) return;
    if (isEditingText()) finishTextEdit(true);
    setFocus();
    m_lastMouse = e->position();
    m_hover = e->position();
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier) && !nearWipe(e->position())) pickSide(e->position());
    const QPointF mp = widgetToAnno(e->position());
    m_pressMedia = mp;
    m_dragChanged = false;

    if (e->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier)) {
        // Alt + drag anywhere: scrub the playhead (right = forward), like most review players.
        m_drag = Drag::Scrub;
        m_scrubAcc = 0.0;
        setCursor(Qt::SizeHorCursor);
        return;
    }
    if (e->button() != Qt::LeftButton || !m_a.frame) return;
    if (nearWipe(e->position())) {
        m_drag = Drag::Wipe;
        return;
    }

    emit editStarted();

    const QPointF n = toNormalized(mp);
    switch (m_tool) {
    case Tool::Pencil:
    case Tool::Pen:
    case Tool::Highlighter:
    case Tool::Line:
    case Tool::Arrow:
    case Tool::Rect:
    case Tool::Circle: {
        Annotation a;
        a.id = Annotation::newId();
        a.color = m_color;
        a.size = m_brushSize;
        a.opacity = m_brushOpacity;
        m_straightStroke = bool(e->modifiers() & Qt::ShiftModifier);
        m_draftStart = mp;
        switch (m_tool) {
        case Tool::Pencil:
        case Tool::Pen:
        case Tool::Highlighter:
            a.type = "stroke"; a.points = { n };
            a.penStyle = m_tool == Tool::Highlighter ? "highlighter" : m_tool == Tool::Pen ? "pen" : "pencil";
            break;
        case Tool::Line: a.type = "line"; break;
        case Tool::Arrow: a.type = "arrow"; break;
        case Tool::Rect: a.type = "rect"; break;
        default: a.type = "circle"; break;
        }
        a.x1 = a.x2 = n.x();
        a.y1 = a.y2 = n.y();
        m_draft = a;
        m_drag = Drag::Draw;
        update();
        break;
    }
    case Tool::Text: {
        const QString hit = hitItem(mp);
        if (Item* i = hit.isEmpty() ? nullptr : findItem(hit); i && i->annotation.type == "text") {
            beginTextEdit(i->annotation, false);
            break;
        }
        Annotation a;
        a.id = Annotation::newId();
        a.type = "text";
        a.color = m_color;
        a.opacity = m_brushOpacity;
        a.fontSize = m_textSize;
        a.x = n.x();
        a.y = n.y();
        beginTextEdit(a, true);
        break;
    }
    case Tool::Eraser:
        m_drag = Drag::Erase;
        emit eraseBegin();
        emit eraseAt(mp, (m_eraserSize / 2) / annoScale());
        break;
    case Tool::Track: {
        const QString id = hitTrack(e->position());
        if (!id.isEmpty()) {
            m_dragTrack = id;
            m_drag = Drag::TrackMove;
            m_selectedTrack = id;
            emit trackSelected(id);
        } else {
            emit trackPlaced(n);
        }
        update();
        break;
    }
    case Tool::Select: {
        const Gizmo g = gizmo();
        const QTransform xf = annoXf();
        Drag mode = Drag::None;
        m_resizeHandle = -1;
        if (g.valid) {
            if (dist(xf.map(g.rotateHandle), e->position()) < kHandle + 2) mode = Drag::Rotate;
            else
                for (int i = 0; i < 8; ++i) {
                    const QPointF c = i < 4 ? g.box[i] : (g.box[i-4] + g.box[(i-3)%4])/2;
                    if (dist(xf.map(c), e->position()) < kHandle + 2) {
                        mode = Drag::Scale;
                        m_resizeHandle = i;
                        break;
                    }
                }
        }
        if (mode == Drag::None && m_showTracks) {
            const QString trackId = hitTrack(e->position());
            if (!trackId.isEmpty()) {
                if (!(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) clearSelection();
                m_selectedTrack = trackId;
                m_dragTrack = trackId;
                m_drag = Drag::TrackMove;
                emit trackSelected(trackId);
                update();
                break;
            }
        }
        if (mode == Drag::None) {
            const QString hit = hitItem(mp);
            const bool shift = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
            if (!hit.isEmpty()) {
                if (shift) {
                    if (m_sel.contains(hit)) m_sel.remove(hit); else m_sel.insert(hit);
                } else if (!m_sel.contains(hit)) {
                    m_sel = { hit };
                }
                emit selectionChanged(m_sel.size());
                mode = m_sel.contains(hit) ? Drag::Move : Drag::None;
                if (!shift && !m_selectedTrack.isEmpty()) {
                    m_selectedTrack.clear();
                    emit trackSelected(QString());
                }
            } else if (g.valid && m_sel.size() > 1 && xf.map(g.box).containsPoint(e->position(), Qt::OddEvenFill) && !shift) {
                mode = Drag::Move;
            } else {
                if (!shift) clearSelection();
                if (!m_selectedTrack.isEmpty()) {
                    m_selectedTrack.clear();
                    emit trackSelected(QString());
                }
                m_marqueeAdd = shift;
                m_marquee = QRectF(mp, mp);
                mode = Drag::Marquee;
            }
        }
        if (mode == Drag::Move || mode == Drag::Scale || mode == Drag::Rotate) {
            m_dragOrig.clear();
            for (const Item& i : items())
                if (m_sel.contains(i.annotation.id) && !i.annotation.locked) m_dragOrig.insert(i.annotation.id, i.annotation);
            m_gizmoCenter = g.valid ? g.center : mp;
            if (g.valid) {
                const QPointF dx = g.box[1] - g.box[0], dy = g.box[3] - g.box[0];
                m_scaleAxisX = dx / std::max(1e-6, std::hypot(dx.x(), dx.y()));
                m_scaleAxisY = dy / std::max(1e-6, std::hypot(dy.x(), dy.y()));
            }
            if (mode != Drag::Move && !g.valid) mode = Drag::None;
        }
        m_drag = mode;
        update();
        break;
    }
    }
}

void Viewport::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    m_hover = pos;
    const QPointF mp = widgetToAnno(pos);
    switch (m_drag) {
    case Drag::Pan:
        m_pan += pos - m_lastMouse;
        m_lastMouse = pos;
        wrapCursor(pos);
        update();
        return;
    case Drag::Scrub: {
        // 6 px per frame; Shift for 1 frame per 18 px when lining up exact frames.
        const double pxPerFrame = (e->modifiers() & Qt::ShiftModifier) ? 18.0 : 6.0;
        m_scrubAcc += (pos.x() - m_lastMouse.x()) / pxPerFrame;
        m_lastMouse = pos;
        wrapCursor(pos);
        const int frames = int(m_scrubAcc);
        if (frames != 0) {
            m_scrubAcc -= frames;
            emit scrubBy(frames);
        }
        return;
    }
    case Drag::Wipe:
        m_wipe = std::clamp(widgetToMedia(pos).x() / m_media.width(), 0.0, 1.0);
        update();
        return;
    case Drag::Draw:
        updateDraft(mp, e->modifiers());
        update();
        return;
    case Drag::Erase:
        emit eraseAt(mp, (m_eraserSize / 2) / annoScale());
        update();
        return;
    case Drag::Move:
    case Drag::Scale:
    case Drag::Rotate:
        applyDrag(mp, e->modifiers());
        update();
        return;
    case Drag::Marquee:
        m_marquee = QRectF(m_pressMedia, mp).normalized();
        update();
        return;
    case Drag::TrackMove:
        for (Track& t : m_tracks)
            if (t.id == m_dragTrack) t.points[m_trackFrame] = toNormalized(mp);
        m_dragChanged = true;
        update();
        return;
    case Drag::None: break;
    }
    m_lastMouse = pos;
    if (nearWipe(pos))
        setCursor(Qt::SplitHCursor);
    else if (m_tool == Tool::Select)
        updateHoverCursor(pos);
    else if (cursor().shape() == Qt::SplitHCursor)
        setTool(m_tool);
    if (m_tool == Tool::Eraser) update();
}

// Endless drag: when the cursor reaches the edge of its screen, jump it to the opposite edge
// and continue measuring from there (like Maya / Blender value drags).
void Viewport::wrapCursor(const QPointF& localPos)
{
    if (m_drag == Drag::Scrub && !m_endlessScrubbing) return;
    const QPoint g = mapToGlobal(localPos.toPoint());
    QScreen* s = QGuiApplication::screenAt(g);
    if (!s) return;
    const QRect r = s->geometry();
    constexpr int edge = 3, inset = 40;
    QPoint ng = g;
    if (g.x() <= r.left() + edge) ng.setX(r.right() - inset);
    else if (g.x() >= r.right() - edge) ng.setX(r.left() + inset);
    if (g.y() <= r.top() + edge) ng.setY(r.bottom() - inset);
    else if (g.y() >= r.bottom() - edge) ng.setY(r.top() + inset);
    if (ng == g) return;
    QCursor::setPos(ng);
    m_lastMouse = mapFromGlobal(ng);
}

void Viewport::mouseReleaseEvent(QMouseEvent* e)
{
    const Drag drag = m_drag;
    m_drag = Drag::None;
    const QPointF mp = widgetToAnno(e->position());
    switch (drag) {
    case Drag::Pan:
    case Drag::Scrub:
        setTool(m_tool);
        break;
    case Drag::Erase:
        emit eraseEnd();
        break;
    case Drag::Draw:
        if (m_draft) {
            updateDraft(mp, e->modifiers());
            Annotation a = *m_draft;
            m_draft.reset();
            const QRectF box = AnnotationPainter::boundingBox(a, QSizeF(annoMedia()));
            const bool meaningful = a.type == "stroke" || box.width() > 12 || box.height() > 12;
            if (meaningful) emit annotationCreated(a);
        }
        break;
    case Drag::Move:
    case Drag::Scale:
    case Drag::Rotate:
        if (m_dragChanged) {
            QVector<Annotation> changed;
            for (auto it = m_dragOrig.constBegin(); it != m_dragOrig.constEnd(); ++it)
                if (Item* i = findItem(it.key())) changed << i->annotation;
            if (!changed.isEmpty()) emit annotationsEdited(changed);
        }
        m_dragOrig.clear();
        break;
    case Drag::Marquee: {
        const QSizeF m(annoMedia());
        if (!m_marqueeAdd) m_sel.clear();
        if (m_marquee.width() * annoScale() > 3 || m_marquee.height() * annoScale() > 3)
            for (const Item& i : items())
                if (!i.annotation.locked && AnnotationPainter::outline(i.annotation, m).boundingRect().intersects(m_marquee))
                    m_sel.insert(i.annotation.id);
        emit selectionChanged(m_sel.size());
        break;
    }
    case Drag::TrackMove:
        if (m_dragChanged) emit trackMoved(m_dragTrack, toNormalized(mp));
        m_dragTrack.clear();
        break;
    default: break;
    }
    updateHoverCursor(e->position());
    update();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || (m_tool != Tool::Select && m_tool != Tool::Text)) return;
    pickSide(e->position());
    const QString hit = hitItem(widgetToAnno(e->position()));
    if (Item* i = hit.isEmpty() ? nullptr : findItem(hit); i && i->annotation.type == "text") {
        m_drag = Drag::None;
        beginTextEdit(i->annotation, false);
    }
}

void Viewport::tabletEvent(QTabletEvent* e)
{
    // All three freehand styles use pressure. Everything else falls through to the synthesized mouse events.
    if ((m_tool != Tool::Pencil && m_tool != Tool::Pen && m_tool != Tool::Highlighter) || !m_a.frame || e->pointerType() == QPointingDevice::PointerType::Eraser) {
        e->ignore();
        return;
    }
    const QPointF pos = e->position();
    if (e->type() == QEvent::TabletPress) pickSide(pos);
    const QPointF mp = widgetToAnno(pos);
    const float pressure = float(std::clamp(e->pressure(), 0.0, 1.0));
    m_hover = pos;
    switch (e->type()) {
    case QEvent::TabletPress: {
        if (e->button() != Qt::LeftButton) {
            e->ignore();
            return;
        }
        emit editStarted();
        if (isEditingText()) finishTextEdit(true);
        setFocus();
        Annotation a;
        a.id = Annotation::newId();
        a.type = "stroke";
        a.penStyle = m_tool == Tool::Highlighter ? "highlighter" : m_tool == Tool::Pen ? "pen" : "pencil";
        a.color = m_color;
        a.size = m_brushSize;
        a.opacity = m_brushOpacity;
        a.points = { toNormalized(mp) };
        a.pressures = { pressure };
        m_straightStroke = bool(e->modifiers() & Qt::ShiftModifier);
        m_draftStart = mp;
        m_draft = a;
        m_drag = Drag::Draw;
        m_tabletStroke = true;
        update();
        break;
    }
    case QEvent::TabletMove:
        if (m_drag == Drag::Draw && m_tabletStroke && m_draft) {
            m_straightStroke = m_straightStroke || bool(e->modifiers() & Qt::ShiftModifier);
            const QPointF n = toNormalized(mp);
            if (m_straightStroke) {
                m_draft->points = {toNormalized(m_draftStart), n};
                m_draft->pressures = {m_draft->pressures.first(), pressure};
                update(); break;
            }
            const QPointF last = m_draft->points.last();
            const double d = std::hypot((n.x() - last.x()) * annoMedia().width(), (n.y() - last.y()) * annoMedia().height());
            if (d >= 0.6) {
                m_draft->points.append(n);
                m_draft->pressures.append(pressure);
            } else {
                m_draft->pressures.last() = std::max(m_draft->pressures.last(), pressure);
            }
            update();
        }
        break;
    case QEvent::TabletRelease:
        if (m_drag == Drag::Draw && m_tabletStroke && m_draft) {
            if (m_straightStroke || (e->modifiers() & Qt::ShiftModifier)) {
                m_draft->points = {toNormalized(m_draftStart), toNormalized(mp)};
                m_draft->pressures = {m_draft->pressures.first(), m_draft->pressures.last()};
            }
            Annotation a = *m_draft;
            m_draft.reset();
            m_drag = Drag::None;
            m_tabletStroke = false;
            emit annotationCreated(a);
            update();
        }
        break;
    default: break;
    }
    e->accept();
}

void Viewport::contextMenuEvent(QContextMenuEvent* e)
{
    if (m_drag != Drag::None || !m_a.frame) return;
    if (isEditingText()) finishTextEdit(true);
    const QPointF pos = e->pos();
    pickSide(pos);
    QString track = m_showTracks ? hitTrack(pos) : QString();
    QString note;
    if (track.isEmpty() && m_showAnnotations) {
        note = hitItem(widgetToAnno(pos), true);
        // Right-clicking an unselected note selects just that note, as in most editors.
        if (!note.isEmpty() && !m_sel.contains(note)) {
            const Item* i = findItem(note);
            if (i && !i->annotation.locked) setSelection({ note });
        }
    }
    if (!track.isEmpty()) {
        m_selectedTrack = track;
        emit trackSelected(track);
        update();
    }
    emit contextMenuAt(e->globalPos(), note, track);
}

void Viewport::wheelEvent(QWheelEvent* e)
{
    const QPointF pos = e->position();
    const QPointF anchor = widgetToMedia(pos);
    const double factor = std::pow(1.0015, e->angleDelta().y());
    m_zoom = std::clamp(m_zoom * factor, 0.02, 64.0);
    m_pan += pos - mediaToWidget().map(anchor);
    positionTextEditor();
    emit zoomChanged(displayScale());
    update();
}

void Viewport::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        m_draft.reset();
        clearSelection();
        update();
        return;
    }
    QOpenGLWidget::keyPressEvent(e);
}

void Viewport::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void Viewport::dropEvent(QDropEvent* e)
{
    QStringList paths;
    for (const QUrl& url : e->mimeData()->urls())
        if (url.isLocalFile()) paths << url.toLocalFile();
    if (!paths.isEmpty()) { e->acceptProposedAction(); emit filesDropped(paths); }
}

void Viewport::leaveEvent(QEvent*)
{
    m_hover = { -1, -1 };
    update();
}
