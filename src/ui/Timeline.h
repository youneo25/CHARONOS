#pragma once

#include <QBitArray>
#include <QWidget>
#include <QVector>

// Bottom scrub bar: frame count on the left, playback rate on the right, a slim track
// showing cached frames, clip boundaries, loop range, annotation and bookmark marks.
class Timeline : public QWidget {
    Q_OBJECT
public:
    explicit Timeline(QWidget* parent = nullptr);

    void setFrameCount(qint64 count);
    qint64 currentFrame() const { return m_current; }
    void setCurrent(qint64 frame);
    void setMarks(QVector<qint64> annotated, QVector<qint64> bookmarks);
    void setStepped(bool on, const QString& label = QString()) { m_stepped = on; m_stepLabel = label; update(); }
    void setSegments(QVector<qint64> starts);
    void setLoop(bool enabled, qint64 in, qint64 out);
    void setCacheMask(QBitArray mask);
    void setRates(double target, double measured);
    void setWaveform(QVector<float> peaksPerFrame);  // 0..1, one per frame; empty = no audio
    void setEndlessScrubbing(bool enabled) { m_endlessScrubbing = enabled; }

    QSize sizeHint() const override { return { 800, 60 }; }

signals:
    void scrubbed(qint64 frame);
    void scrubBy(int frames);
    void rateClicked();
    void contextMenuAt(qint64 frame, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QRectF trackRect() const;
    double xFor(double frame) const;
    qint64 frameAt(double x) const;
    QRectF rateRect() const;

    qint64 m_count = 0;
    qint64 m_current = 0;
    QVector<qint64> m_annotated;
    QVector<qint64> m_bookmarks;
    QVector<qint64> m_segments;
    bool m_stepped = false;
    QString m_stepLabel;
    bool m_loop = false;
    qint64 m_in = 0;
    qint64 m_out = 0;
    QBitArray m_cache;
    QVector<float> m_wave;
    double m_target = 24.0;
    double m_measured = 0.0;
    bool m_dragging = false;
    bool m_relative = false;
    QPointF m_lastMouse;
    double m_scrubAcc = 0;
    bool m_endlessScrubbing = true;
};

// Top bar: the current clip's own frame range with a marker (Keyframe Pro's source bar).
class ClipBar : public QWidget {
    Q_OBJECT
public:
    explicit ClipBar(QWidget* parent = nullptr);
    // local / count are positions inside the segment; offset = its first global frame and
    // firstLabel = the source frame number shown at its start (a trimmed clip doesn't start at 0).
    void setClip(const QString& name, qint64 count, qint64 local, qint64 offset = 0, qint64 firstLabel = 0);
    void setReviewMarks(QVector<qint64> keys, QVector<qint64> bookmarks);

    QSize sizeHint() const override { return { 800, 48 }; }

signals:
    void scrubbed(qint64 localFrame);
    void contextMenuAt(qint64 localFrame, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    QRectF lineRect() const;
    qint64 frameAt(double x) const;

    QString m_name;
    qint64 m_count = 0;
    qint64 m_local = 0;
    qint64 m_offset = 0;
    qint64 m_firstLabel = 0;
    QVector<qint64> m_keys, m_bookmarks;
};
