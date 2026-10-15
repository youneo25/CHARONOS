#pragma once

#include "AudioClip.h"
#include "AudioClock.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Plays AudioClips through the Windows wave mapper (no extra runtime needed).
// A pump thread keeps a few 10 ms blocks queued. Times are container seconds (the clock of
// MediaInfo::frameTimes and AudioClip::startTime).
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool available() const { return m_wave != nullptr; }

    // Continuous playback from `videoSec`. speed != 1 plays faster / slower (varispeed, like tape).
    void play(std::shared_ptr<const AudioClip> clip, double videoSec, double speed);
    // A short burst (e.g. one frame while stepping).
    void snippet(std::shared_ptr<const AudioClip> clip, double videoSec, double seconds);
    void stop();

    bool isPlaying() const { return m_continuous; }
    const AudioClip* clip() const { return m_clip.get(); }
    double position() const;  // video seconds currently heard

    void setVolume(double v) { m_volume = v; }
    void setMuted(bool m) { m_muted = m; }

private:
    void start(std::shared_ptr<const AudioClip> clip, double videoSec, double speed, double seconds);
    void pump();
    void fill(std::vector<int16_t>& block);

    void* m_wave = nullptr;  // HWAVEOUT
    static constexpr int kBlocks = 6;
    static constexpr int kBlockFrames = 480;  // 10 ms at 48 kHz
    std::vector<int16_t> m_buf[kBlocks];
    std::vector<unsigned char> m_hdr;  // WAVEHDR[kBlocks], kept opaque to avoid windows.h here

    mutable std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_quit{ false };

    std::shared_ptr<const AudioClip> m_clip;
    double m_read = 0.0;     // position in clip frames
    double m_speed = 1.0;
    double m_end = -1.0;     // snippet end in clip frames, -1 = continuous
    double m_startVideoSec = 0.0;
    int64_t m_submittedFrames = 0;
    std::chrono::steady_clock::time_point m_started;
    mutable AudioClock m_positionClock;
    std::atomic<bool> m_active{ false };
    std::atomic<bool> m_continuous{ false };
    std::atomic<double> m_volume{ 0.8 };
    std::atomic<bool> m_muted{ false };
};
