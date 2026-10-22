#pragma once

#include <QColor>
#include <QPointF>
#include <QVector>
#include <QWidget>

#include <map>

class QComboBox;
class QLabel;
class QToolButton;
class MotionPlot;

// Motion analysis of one tracked point (Window > Motion Graph): X / Y position and speed per
// frame, an animator's spacing chart (ticks spaced by the distance travelled each frame, so
// eases bunch up), and suggested key poses: where the point stops or changes direction.
class MotionGraph : public QWidget {
    Q_OBJECT
public:
    struct Sample {
        int frame = 0;
        QPointF pos;          // media pixels
        double speed = 0;     // px / frame from the previous sample (0 for the first)
        bool hasSpeed = false;
    };
    struct Analysis {
        QVector<Sample> samples;
        QVector<int> keys;      // suggested key poses (stops / extremes)
        QVector<int> fastest;   // local speed peaks
        double maxSpeed = 0;
        double meanSpeed = 0;
        double distance = 0;    // path length, px
    };

    explicit MotionGraph(QWidget* parent = nullptr);

    // Tracks of the clip being edited: (id, name, colour). Empty = nothing tracked yet.
    void setTracks(const QVector<std::tuple<QString, QString, QColor>>& tracks, const QString& selectedId);
    void setData(const std::map<int, QPointF>& normalizedPoints, QSizeF mediaSize, double fps, const QString& clipName);
    void setCurrentFrame(int frame);
    QString trackId() const;
    const Analysis& analysis() const { return m_analysis; }

    static Analysis analyze(const std::map<int, QPointF>& normalizedPoints, QSizeF mediaSize);
    QString csv() const;

signals:
    void trackChosen(QString id);
    void frameClicked(int sourceFrame);
    void bookmarkRequested(QVector<int> sourceFrames);

private:
    friend class MotionPlot;
    void updateReadout();

    MotionPlot* m_plot = nullptr;
    QComboBox* m_track = nullptr;
    QLabel* m_readout = nullptr;
    QLabel* m_summary = nullptr;
    QToolButton* m_showX = nullptr;
    QToolButton* m_showY = nullptr;
    QToolButton* m_showSpeed = nullptr;
    Analysis m_analysis;
    double m_fps = 24.0;
    int m_current = -1;
    QString m_clip;
    bool m_filling = false;
};
