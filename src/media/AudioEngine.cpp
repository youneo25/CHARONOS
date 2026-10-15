#include "AudioEngine.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
HWAVEOUT wave(void* p) { return static_cast<HWAVEOUT>(p); }
}

AudioEngine::AudioEngine()
{
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = AudioClip::kRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = AudioClip::kRate * 4;
    HWAVEOUT h = nullptr;
    if (waveOutOpen(&h, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;  // no output device
    m_wave = h;

    m_hdr.assign(sizeof(WAVEHDR) * kBlocks, 0);
    auto* hdr = reinterpret_cast<WAVEHDR*>(m_hdr.data());
    for (int i = 0; i < kBlocks; ++i) {
        m_buf[i].assign(size_t(kBlockFrames) * 2, 0);
        hdr[i].lpData = reinterpret_cast<LPSTR>(m_buf[i].data());
        hdr[i].dwBufferLength = DWORD(m_buf[i].size() * sizeof(int16_t));
        waveOutPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
        hdr[i].dwFlags |= WHDR_DONE;  // free to fill
    }
    m_thread = std::thread([this] { pump(); });
}

AudioEngine::~AudioEngine()
{
    if (!m_wave) return;
    m_quit = true;
    m_thread.join();
    waveOutReset(wave(m_wave));
    auto* hdr = reinterpret_cast<WAVEHDR*>(m_hdr.data());
    for (int i = 0; i < kBlocks; ++i) waveOutUnprepareHeader(wave(m_wave), &hdr[i], sizeof(WAVEHDR));
    waveOutClose(wave(m_wave));
}

void AudioEngine::start(std::shared_ptr<const AudioClip> clip, double videoSec, double speed, double seconds)
{
    if (!m_wave) return;
    std::lock_guard lock(m_mutex);
    m_active = false;
    m_continuous = false;
    if (waveOutReset(wave(m_wave)) != MMSYSERR_NOERROR) return;
    m_clip = std::move(clip);
    m_speed = std::clamp(speed, 0.05, 8.0);
    m_startVideoSec = videoSec;
    m_submittedFrames = 0;
    m_positionClock.reset();
    m_started = std::chrono::steady_clock::now();
    m_read = m_clip ? (videoSec - m_clip->startTime) * m_clip->rate : 0.0;
    m_end = seconds > 0 ? m_read + seconds * AudioClip::kRate * m_speed : -1.0;
    m_continuous = bool(m_clip) && seconds <= 0;
    m_active = bool(m_clip);
}

void AudioEngine::play(std::shared_ptr<const AudioClip> clip, double videoSec, double speed) { start(std::move(clip), videoSec, speed, 0); }
void AudioEngine::snippet(std::shared_ptr<const AudioClip> clip, double videoSec, double seconds) { start(std::move(clip), videoSec, 1.0, seconds); }

void AudioEngine::stop()
{
    if (!m_wave) return;
    std::lock_guard lock(m_mutex);
    m_active = false;
    m_continuous = false;
    waveOutReset(wave(m_wave));
}

double AudioEngine::position() const
{
    if (!m_wave) return 0.0;
    std::lock_guard lock(m_mutex);
    if (!m_continuous) return m_startVideoSec + m_positionClock.last() * m_speed;
    MMTIME t{};
    t.wType = TIME_SAMPLES;
    std::optional<double> device;
    if (waveOutGetPosition(wave(m_wave), &t, sizeof t) == MMSYSERR_NOERROR) {
        if (t.wType == TIME_SAMPLES) device = audioClockSeconds(AudioClockUnit::Samples, t.u.sample, AudioClip::kRate, 4);
        else if (t.wType == TIME_BYTES) device = audioClockSeconds(AudioClockUnit::Bytes, t.u.cb, AudioClip::kRate, 4);
        else if (t.wType == TIME_MS) device = audioClockSeconds(AudioClockUnit::Milliseconds, t.u.ms, AudioClip::kRate, 4);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count();
    const double elapsed = m_positionClock.observe(device, wall, double(m_submittedFrames) / AudioClip::kRate);
    return m_startVideoSec + elapsed * m_speed;
}

void AudioEngine::fill(std::vector<int16_t>& block)
{
    const double vol = m_muted ? 0.0 : std::clamp(m_volume.load(), 0.0, 1.0);
    const AudioClip* c = m_active ? m_clip.get() : nullptr;
    const int64_t frames = c ? c->frames() : 0;
    for (int i = 0; i < kBlockFrames; ++i) {
        int16_t l = 0, r = 0;
        if (c && (m_end < 0 || m_read < m_end) && m_read >= 0 && m_read < frames - 1) {
            const int64_t i0 = int64_t(m_read);
            const double f = m_read - double(i0);
            const int16_t* s = c->pcm.data() + i0 * 2;
            // linear interpolation keeps varispeed reasonably clean
            l = int16_t(std::lround((s[0] * (1 - f) + s[2] * f) * vol));
            r = int16_t(std::lround((s[1] * (1 - f) + s[3] * f) * vol));
        }
        block[size_t(i) * 2] = l;
        block[size_t(i) * 2 + 1] = r;
        if (c) m_read += m_speed;
    }
    if (c && m_end >= 0 && m_read >= m_end) m_active = false;  // snippet done
}

void AudioEngine::pump()
{
    auto* hdr = reinterpret_cast<WAVEHDR*>(m_hdr.data());
    while (!m_quit) {
        {
            std::lock_guard lock(m_mutex);
            for (int i = 0; i < kBlocks; ++i) {
                if (!m_active) break; // do not advance the device clock by pumping idle silence
                if (!(hdr[i].dwFlags & WHDR_DONE)) continue;  // still queued in the device
                fill(m_buf[i]);
                hdr[i].dwFlags &= ~WHDR_DONE;
                if (waveOutWrite(wave(m_wave), &hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
                    m_submittedFrames += kBlockFrames;
                else {
                    hdr[i].dwFlags |= WHDR_DONE;
                    m_active = m_continuous = false;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
