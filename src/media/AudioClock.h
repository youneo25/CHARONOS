#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

enum class AudioClockUnit { Samples, Bytes, Milliseconds, Unsupported };

inline std::optional<double> audioClockSeconds(AudioClockUnit unit, unsigned value, int sampleRate, int blockAlign)
{
    switch (unit) {
    case AudioClockUnit::Samples: return double(value) / sampleRate;
    case AudioClockUnit::Bytes: return double(value) / (sampleRate * blockAlign);
    case AudioClockUnit::Milliseconds: return double(value) / 1000;
    default: return std::nullopt;
    }
}

// Bound a device clock by audio actually submitted in this playback session.
// A stale counter, an unsupported unit or a driver error cannot seek the video.
class AudioClock {
public:
    void reset() { m_last = 0; }
    double observe(std::optional<double> device, double wall, double submitted)
    {
        const double bound = std::min(std::max(0.0, submitted), std::max(0.0, wall) + .02);
        double elapsed = device.value_or(wall);
        if (!std::isfinite(elapsed) || elapsed < 0 || elapsed > wall + .02 || elapsed + .1 < m_last)
            elapsed = wall;
        m_last = std::max(m_last, std::clamp(elapsed, 0.0, bound));
        return m_last;
    }
    double last() const { return m_last; }
private:
    double m_last = 0;
};
