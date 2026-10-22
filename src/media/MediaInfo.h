#pragma once

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct MediaInfo {
    QString path;
    int width = 0;
    int height = 0;
    double fps = 24.0;
    int64_t frameCount = 0;
    double duration = 0.0;
    QString codec;
    QString pixelFormat;
    bool variableFrameRate = false;
    bool isImageSequence = false;
    bool hasAudio = false;  // the file also has a sound stream (it becomes a linked sound clip)
    QString decoderName;  // "NVDEC (CUDA)", "CPU · 16 threads", "Image reader"...
    // Presentation time (seconds, container clock) of each frame. Empty for image sequences.
    // Phone and screen recordings are usually VFR, so frame N is NOT at N / fps.
    std::vector<double> frameTimes;

    // Time of a frame; beyond the last frame (compare mode holds A) it continues at the average rate.
    double timeOf(int64_t frame) const
    {
        const double rate = fps > 0 ? fps : 24.0;
        if (frameTimes.empty()) return frame / rate;
        const int64_t last = int64_t(frameTimes.size()) - 1;
        if (frame < 0) return frameTimes.front() + frame / rate;
        if (frame > last) return frameTimes.back() + (frame - last) / rate;
        return frameTimes[size_t(frame)];
    }

    // The frame on screen at time t (the last frame that started at or before t).
    int64_t frameAt(double t) const
    {
        const double rate = fps > 0 ? fps : 24.0;
        if (frameTimes.empty()) return std::max<int64_t>(0, int64_t(std::floor(t * rate + 1e-6)));
        const int64_t last = int64_t(frameTimes.size()) - 1;
        if (t >= frameTimes.back()) return last + int64_t(std::floor((t - frameTimes.back()) * rate + 1e-6));
        const auto it = std::upper_bound(frameTimes.begin(), frameTimes.end(), t + 1e-6);
        return std::max<int64_t>(0, int64_t(it - frameTimes.begin()) - 1);
    }

    double startTime() const { return frameTimes.empty() ? 0.0 : frameTimes.front(); }
};
