#pragma once

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <array>

class VideoFrame;

// Viewer filters for animation and motion analysis (Filter menu). The viewer runs them in its
// shader; apply() is the same maths on the CPU for export, still frames and verification.
struct AnalysisFilter {
    enum class Mode {
        None,
        Silhouette,        // dark (or bright) subject as a solid shape on paper, threshold on brightness
        MovingSilhouette,  // whatever differs from the shot's background plate (fixed camera)
        Edges,             // outline drawing of the picture's contours
        Value,             // brightness only (value study)
        Notan,             // two or three flat values
        MotionHighlight,   // pixels that changed since the previous frame, over a dimmed picture
    };
    static constexpr int kModes = 7;

    Mode mode = Mode::None;
    std::array<double, kModes> thresholds{ 0.5, 0.5, 0.12, 0.22, 0.5, 0.5, 0.08 };
    bool autoThreshold = true;  // Silhouette / Notan: split brightness automatically (Otsu)
    double softness = 0.03;
    bool invert = false;        // Silhouette / Notan: the subject is brighter than the background
    double showOriginal = 0.0;  // 0 = paper behind the shape, 1 = the picture itself
    int levels = 2;             // Notan: 2 or 3 values
    QColor ink{ 0x17, 0x17, 0x19 };
    QColor paper{ 0xF1, 0xEE, 0xE6 };
    QColor accent{ 0xFF, 0x4B, 0x3A };

    bool enabled() const { return mode != Mode::None; }
    double threshold() const { return thresholds[size_t(mode)]; }
    void setThreshold(double v) { thresholds[size_t(mode)] = v; }
    bool usesAutoThreshold() const { return autoThreshold && (mode == Mode::Silhouette || mode == Mode::Notan); }
    bool needsReference() const { return mode == Mode::MovingSilhouette || mode == Mode::MotionHighlight; }

    QJsonObject toJson() const;
    static AnalysisFilter fromJson(const QJsonObject& o);
    static QString name(Mode mode);
    static QString description(Mode mode);
};

namespace analysis {

double luma(const QColor& c);
// Brightness that best splits the picture in two (Otsu), 0..1.
double otsu(const QImage& image);
double otsu(const VideoFrame& frame);
// Per-pixel median of several frames of one shot: its static background (people moving
// through it vanish). Pictures are scaled to `maxWidth` first.
QImage backgroundPlate(const QVector<QImage>& frames, int maxWidth = 640);
// The filter applied to a picture. `threshold` is the effective one (automatic or set);
// `reference` is the background plate (moving silhouette) or the previous frame (motion).
QImage apply(const QImage& picture, const AnalysisFilter& filter, double threshold, const QImage& reference = {});

} // namespace analysis
