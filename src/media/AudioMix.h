#pragma once

#include "AudioClip.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <limits>
#include <vector>

// The timeline's sound: every audible sound clip with its gain and fades, mixed on the fly by
// the playback engine and by the exporter, so what is heard is what is exported.
struct AudioMix {
    struct Item {
        std::shared_ptr<const AudioClip> pcm;
        int64_t start = 0;          // first timeline frame
        int64_t count = 0;          // frames
        std::vector<double> times;  // source time (container seconds) at each frame boundary: count + 1 (VFR-safe)
        double gain = 1.0;          // linear, clip x track
        int fadeIn = 0, fadeOut = 0;  // frames
        double sourceEnd = std::numeric_limits<double>::infinity(); // silence in the rounded final timeline frame
    };
    std::vector<Item> items;
    double fps = 24.0;

    bool empty() const { return items.empty(); }
    double duration() const;  // seconds of timeline that have sound

    // Adds `frames` interleaved stereo float frames to `out`, the first at timeline time `t0`
    // (seconds) and each next one `step` seconds later (1/48000 at normal speed).
    void render(double t0, double step, int frames, float* out) const;
};

// Gain in decibels to a linear factor (-60 dB and below = silence).
inline double dbToGain(double db) { return db <= -60.0 ? 0.0 : std::pow(10.0, db / 20.0); }
