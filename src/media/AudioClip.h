#pragma once

#include <QString>

#include <atomic>
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

    // Returns null and leaves `error` empty when the file simply has no audio track. `cancel` stops a
    // long decode early (app closing); soundtracks longer than kMaxSeconds are refused instead of
    // exhausting memory.
    static constexpr double kMaxSeconds = 4.0 * 3600.0;
    static std::shared_ptr<AudioClip> decode(const QString& path, QString* error, const std::atomic<bool>* cancel = nullptr);
};
