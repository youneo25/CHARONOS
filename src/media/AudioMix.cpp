#include "AudioMix.h"

#include <algorithm>
#include <cmath>

double AudioMix::duration() const
{
    int64_t end = 0;
    for (const Item& it : items) end = std::max(end, it.start + it.count);
    return fps > 0 ? end / fps : 0.0;
}

void AudioMix::render(double t0, double step, int frames, float* out) const
{
    if (frames <= 0 || fps <= 0) return;
    const double t1 = t0 + step * frames;
    for (const Item& it : items) {
        if (!it.pcm || it.count <= 0 || it.times.size() < size_t(it.count) + 1 || it.gain <= 0) continue;
        const double a = it.start / fps, b = (it.start + it.count) / fps;
        if (b <= t0 || a >= t1) continue;
        const int64_t n = it.pcm->frames();
        const int16_t* pcm = it.pcm->pcm.data();
        const double rate = it.pcm->rate;
        const double first = it.pcm->startTime;
        for (int i = 0; i < frames; ++i) {
            // Position inside the clip in frames: each frame's own time span maps onto the source.
            const double p = (t0 + step * i) * fps - double(it.start);
            if (p < 0 || p >= double(it.count)) continue;
            const int64_t k = int64_t(p);
            const double frac = p - double(k);
            const double src = it.times[size_t(k)] + frac * (it.times[size_t(k) + 1] - it.times[size_t(k)]);
            if (src >= it.sourceEnd) continue;
            const double pos = (src - first) * rate;
            if (pos < 0 || pos >= double(n - 1)) continue;
            const int64_t i0 = int64_t(pos);
            const double f = pos - double(i0);
            double g = it.gain;
            if (it.fadeIn > 0 && p < it.fadeIn) g *= p / it.fadeIn;
            if (it.fadeOut > 0 && p > double(it.count - it.fadeOut)) g *= std::max(0.0, (double(it.count) - p) / it.fadeOut);
            const int16_t* s = pcm + i0 * 2;
            out[size_t(i) * 2] += float((s[0] * (1 - f) + s[2] * f) * g / 32768.0);
            out[size_t(i) * 2 + 1] += float((s[1] * (1 - f) + s[3] * f) * g / 32768.0);
        }
    }
}
