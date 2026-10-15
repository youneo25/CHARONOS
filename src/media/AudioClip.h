#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

// A clip's whole soundtrack, decoded once: interleaved stereo int16 at `rate`.
struct AudioClip {
    static constexpr int kRate = 48000;
    std::vector<int16_t> pcm;
    int rate = kRate;
    double startTime = 0.0;  // container time (seconds) of the first sample, same clock as MediaInfo::frameTimes

    int64_t frames() const { return int64_t(pcm.size() / 2); }
    double duration() const { return double(frames()) / rate; }

    // Returns null and leaves `error` empty when the file simply has no audio track.
    static std::shared_ptr<AudioClip> decode(const QString& path, QString* error);
};
