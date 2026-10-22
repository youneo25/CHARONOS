#pragma once

#include "analysis/AnalysisFilter.h"
#include "media/AudioClip.h"
#include "media/AudioMix.h"
#include "model/Project.h"
#include "model/Sequence.h"

#include <QSet>

#include <QImage>
#include <QRectF>

#include <atomic>
#include <map>
#include <memory>

// Draws a decoded frame fitted into `outSize`, optionally with the clip's annotations
// burned in and a small caption in the lower-left corner.
QImage composeFrame(const QImage& frame, const Source& source, int localFrame, QSize outSize, bool withNotes,
                    const QString& caption, bool withTracks = false, int trackRange = 0, double margin = 0);

struct StillFrameOptions {
    bool includeOutside = true, notes = true, tracks = true, video = true;
    double scale = 1.0;
    int padding = 16, trackRange = 0;
    QColor background = Qt::black;
    QString caption;
};
QRectF stillFrameBounds(QSize media, const Source& source, int frame, const StillFrameOptions& options);
QImage composeStillFrame(const QImage& frame, const Source& source, int localFrame, const StillFrameOptions& options);

struct ExportJob {
    enum class Format { Mp4H264, Mp4Nvenc, Mp4HevcNvenc, MovProRes, PngSequence, JpgSequence, TiffSequence };

    Format format = Format::Mp4H264;
    QString output;  // video file, or "<folder>/<base>" for sequences
    Project project;
    Sequence sequence;
    int64_t first = 0;
    int64_t last = 0;
    double fps = 24.0;
    double scale = 1.0;
    double margin = 0.0; // fractional border on every edge, includes off-video annotations
    bool notes = true;
    bool caption = false;
    bool tracks = false;
    int trackRange = 0;
    bool audio = true;  // video formats only
    std::map<QString, std::shared_ptr<AudioClip>> audioClips;  // already decoded by the app (null = no track)

    // A/B comparison, laid out like the viewer. The sequence is A on its own; B follows it.
    struct Compare {
        enum class Layout { Off, Wipe, SideBySide, TopBottom, Blend };
        Layout layout = Layout::Off;
        QString sourceId;              // B
        std::vector<int64_t> bFrame;   // B frame for each A clip-local frame
        std::vector<double> bTime;     // B clock (seconds) at the start of each A frame, one extra at the end
        double wipe = 0.5;
        bool audioFromB = false;
    } compare;

    // 1.1 multi-track timeline: each frame is composed from its layers (bottom track first) on
    // the timeline canvas, with the edited clip's annotations; the sound is the timeline's mix.
    struct Layer {
        QString sourceId;
        int64_t local = 0;
        QRectF rect;  // canvas pixels
        double opacity = 1.0;
        BlendMode blend = BlendMode::Normal;
        bool primary = false;  // its annotations and tracks are drawn
    };
    bool timeline = false;
    QSize canvas;
    std::vector<std::vector<Layer>> layers;  // per exported frame (first..last)
    std::shared_ptr<const AudioMix> mix;
    double timelineFps = 24.0;
    std::map<QString, QSet<QString>> hiddenNotes;  // objects on annotation lanes that are switched off

    // The viewer's analysis filter, burned in (Filter menu).
    AnalysisFilter filter;
    double filterThreshold = 0.5;
    std::map<QString, QImage> plates;  // background plates (moving silhouette)

    bool isSequence() const { return format >= Format::PngSequence; }
    int64_t frameCount() const { return last - first + 1; }
};

// Runs on a worker thread. Decodes each source independently of the viewer's cache.
bool runExport(const ExportJob& job, std::atomic<int64_t>& done, const std::atomic<bool>& cancel, QString* error);
