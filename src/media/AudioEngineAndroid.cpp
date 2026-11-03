#include "AudioEngine.h"

// Android: the same engine as the Windows one, playing through AAudio (API 26+). A pump thread
// writes 10 ms blocks with a blocking write, so the queue stays short and seeks are quick; the
// position comes from the stream's presentation timestamps, bounded like on Windows.

#include <aaudio/AAudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

namespace {
AAudioStream* stream(void* p) { return static_cast<AAudioStream*>(p); }

AAudioStream* openStream()
{
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return nullptr;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, AudioClip::kRate);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStream* s = nullptr;
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    return r == AAUDIO_OK ? s : nullptr;
}

int64_t nowNanos()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
} // namespace

AudioEngine::AudioEngine()
{
    AAudioStream* s = openStream();
    if (!s) return;  // no output device
    m_wave = s;
    for (int i = 0; i < kBlocks; ++i) m_buf[i].assign(size_t(kBlockFrames) * 2, 0);
    // A short device queue (a few bursts) keeps seeks and scrub snippets responsive.
    AAudioStream_setBufferSizeInFrames(s, std::max(AAudioStream_getFramesPerBurst(s) * 4, kBlockFrames * 3));
    AAudioStream_requestStart(s);
    m_thread = std::thread([this] { pump(); });
}

AudioEngine::~AudioEngine()
{
    if (!m_wave) return;
    m_quit = true;
    m_thread.join();
    AAudioStream_requestStop(stream(m_wave));
    AAudioStream_close(stream(m_wave));
}

void AudioEngine::start(std::shared_ptr<const AudioClip> clip, std::shared_ptr<const AudioMix> mix, double videoSec, double speed,
                        double seconds)
{
    if (!m_wave) return;
    std::lock_guard lock(m_mutex);
    m_active = false;
    m_continuous = false;
    // Drop what is queued (like waveOutReset): pause, flush, start again.
    AAudioStream* s = stream(m_wave);
    AAudioStream_requestPause(s);
    AAudioStream_waitForStateChange(s, AAUDIO_STREAM_STATE_PAUSING, nullptr, 20 * 1000000LL);
    AAudioStream_requestFlush(s);
    AAudioStream_requestStart(s);
    m_clip = std::move(clip);
    m_mix = std::move(mix);
    m_speed = std::clamp(speed, 0.05, 8.0);
    m_startVideoSec = videoSec;
    m_submittedFrames = 0;
    m_hdr.assign(sizeof(int64_t), 0);
    *reinterpret_cast<int64_t*>(m_hdr.data()) = AAudioStream_getFramesWritten(s);  // device frames before this session
    m_positionClock.reset();
    m_started = std::chrono::steady_clock::now();
    const bool any = m_clip || m_mix;
    if (m_mix) {
        m_read = videoSec;  // timeline seconds
        m_end = seconds > 0 ? m_read + seconds * m_speed : -1.0;
    } else {
        m_read = m_clip ? (videoSec - m_clip->startTime) * m_clip->rate : 0.0;
        m_end = seconds > 0 ? m_read + seconds * AudioClip::kRate * m_speed : -1.0;
    }
    m_continuous = any && seconds <= 0;
    m_active = any;
}

void AudioEngine::play(std::shared_ptr<const AudioClip> clip, double videoSec, double speed) { start(std::move(clip), nullptr, videoSec, speed, 0); }
void AudioEngine::snippet(std::shared_ptr<const AudioClip> clip, double videoSec, double seconds) { start(std::move(clip), nullptr, videoSec, 1.0, seconds); }
void AudioEngine::playMix(std::shared_ptr<const AudioMix> mix, double timelineSec, double speed) { start(nullptr, std::move(mix), timelineSec, speed, 0); }
void AudioEngine::snippetMix(std::shared_ptr<const AudioMix> mix, double timelineSec, double seconds) { start(nullptr, std::move(mix), timelineSec, 1.0, seconds); }

void AudioEngine::updateMix(std::shared_ptr<const AudioMix> mix)
{
    std::lock_guard lock(m_mutex);
    if (m_mix && mix) m_mix = std::move(mix);
}

void AudioEngine::stop()
{
    if (!m_wave) return;
    std::lock_guard lock(m_mutex);
    m_active = false;
    m_continuous = false;
    m_mix.reset();
    AAudioStream* s = stream(m_wave);
    AAudioStream_requestPause(s);
    AAudioStream_waitForStateChange(s, AAUDIO_STREAM_STATE_PAUSING, nullptr, 20 * 1000000LL);
    AAudioStream_requestFlush(s);
    AAudioStream_requestStart(s);
}

double AudioEngine::position() const
{
    if (!m_wave) return 0.0;
    std::lock_guard lock(m_mutex);
    if (!m_continuous) return m_startVideoSec + m_positionClock.last() * m_speed;
    std::optional<double> device;
    int64_t framePos = 0, timeNs = 0;
    if (AAudioStream_getTimestamp(stream(m_wave), CLOCK_MONOTONIC, &framePos, &timeNs) == AAUDIO_OK && m_hdr.size() >= sizeof(int64_t)) {
        const int64_t base = *reinterpret_cast<const int64_t*>(m_hdr.data());
        // Frames presented in this session, extrapolated to now.
        const double presented = double(framePos - base) / AudioClip::kRate + double(nowNanos() - timeNs) * 1e-9;
        device = presented;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count();
    const double elapsed = m_positionClock.observe(device, wall, double(m_submittedFrames) / AudioClip::kRate);
    return m_startVideoSec + elapsed * m_speed;
}

void AudioEngine::fill(std::vector<int16_t>& block)
{
    const double vol = m_muted ? 0.0 : std::clamp(m_volume.load(), 0.0, 1.0);
    if (m_mix && m_active) {
        m_mixBuf.assign(size_t(kBlockFrames) * 2, 0.f);
        const double step = m_speed / AudioClip::kRate;
        int frames = kBlockFrames;
        if (m_end >= 0) frames = int(std::clamp((m_end - m_read) / step, 0.0, double(kBlockFrames)));
        m_mix->render(m_read, step, frames, m_mixBuf.data());
        for (size_t i = 0; i < block.size(); ++i)
            block[i] = int16_t(std::lround(std::clamp(m_mixBuf[i] * vol, -1.0, 1.0) * 32767.0));
        m_read += step * kBlockFrames;
        if (m_end >= 0 && m_read >= m_end) m_active = false;
        return;
    }
    const AudioClip* c = m_active ? m_clip.get() : nullptr;
    const int64_t frames = c ? c->frames() : 0;
    for (int i = 0; i < kBlockFrames; ++i) {
        int16_t l = 0, r = 0;
        if (c && (m_end < 0 || m_read < m_end) && m_read >= 0 && m_read < frames - 1) {
            const int64_t i0 = int64_t(m_read);
            const double f = m_read - double(i0);
            const int16_t* s = c->pcm.data() + i0 * 2;
            l = int16_t(std::lround((s[0] * (1 - f) + s[2] * f) * vol));
            r = int16_t(std::lround((s[1] * (1 - f) + s[3] * f) * vol));
        }
        block[size_t(i) * 2] = l;
        block[size_t(i) * 2 + 1] = r;
        if (c) m_read += m_speed;
    }
    if (c && m_end >= 0 && m_read >= m_end) m_active = false;
}

void AudioEngine::pump()
{
    std::vector<int16_t>& block = m_buf[0];
    while (!m_quit) {
        bool wrote = false;
        {
            std::unique_lock lock(m_mutex);
            if (m_active) {
                fill(block);
                lock.unlock();
                // Blocks while the device queue is full (about kBlocks x 10 ms), which paces the pump.
                const aaudio_result_t r = AAudioStream_write(stream(m_wave), block.data(), kBlockFrames, 100 * 1000000LL);
                lock.lock();
                if (r > 0) m_submittedFrames += r;
                else if (r < 0) m_active = m_continuous = false;
                wrote = true;
            }
        }
        if (!wrote) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
