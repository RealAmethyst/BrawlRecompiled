#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

// The signal path of audio description (src/accessibility/audio_description.cpp), kept free of guest memory so it can
// be checked offline against an approved mix: description clips placed on the movie's timeline and mixed into the final
// stereo output, with the game's sound lowered under them.
namespace AudioDescriptionMix {

struct Cue {
    double start = 0.0;            // seconds on the movie's timeline
    std::vector<int16_t> samples;  // mono, at the output sample rate, already at mix level (PrepareClip)
};

using Cues = std::vector<Cue>;  // in start order

// Levels of the mix the designer approved by listening (OPMovie, 2026-10-26): game sound 6 dB down for the whole
// described movie, a further 2 dB down while a description plays (50 ms down, 600 ms back up), description clips at
// +2 dB per channel with their peaks limited at 98 % of full scale, and a limiter on the sum for the rare peak left.
struct Levels {
    float movieGain = 0.5012f;        // -6 dB
    float duckGain = 0.7943f;         // -2 dB
    float descriptionGain = 1.2589f;  // +2 dB
    float limit = 0.98f;
    float duckAttackSeconds = 0.05f;
    float duckReleaseSeconds = 0.6f;
    float movieRampSeconds = 0.3f;
    float fadeSeconds = 0.008f;       // a clip cut by pause, skip or loop fades instead of clicking
    float clipLookaheadSeconds = 0.005f;
    float clipReleaseSeconds = 0.05f;
    float limiterReleaseSeconds = 0.1f;
};

// A clip at mix level: the description gain, then a look-ahead peak limiter (the clips are prepared once when loaded,
// so the limiter can see ahead without delaying the game's sound).
inline std::vector<int16_t> PrepareClip(const std::vector<int16_t>& clip, uint32_t sampleRate, const Levels& levels = {}) {
    const size_t count = clip.size();
    const double limit = levels.limit * 32767.0;
    std::vector<double> gain(count);
    for (size_t i = 0; i < count; ++i) {
        const double level = std::fabs(clip[i] * static_cast<double>(levels.descriptionGain));
        gain[i] = level > limit ? limit / level : 1.0;
    }
    // Each sample takes the lowest gain needed within the look-ahead window after it (a sliding minimum), ...
    const size_t window = std::max<size_t>(1, static_cast<size_t>(levels.clipLookaheadSeconds * sampleRate));
    std::vector<double> needed(count);
    std::deque<size_t> lowest;
    for (size_t i = count; i-- > 0;) {
        while (!lowest.empty() && gain[lowest.back()] >= gain[i]) lowest.pop_back();
        lowest.push_back(i);
        if (lowest.front() > i + window) lowest.pop_front();
        needed[i] = gain[lowest.front()];
    }
    // ... reached by a linear ramp down across that window and released smoothly afterwards.
    const double step = 1.0 / static_cast<double>(window);
    for (size_t i = count - (count > 0 ? 1 : 0); i-- > 0;) needed[i] = std::min(needed[i], needed[i + 1] + step);
    const double release = 1.0 - std::exp(-1.0 / (levels.clipReleaseSeconds * sampleRate));
    std::vector<int16_t> out(count);
    double current = 1.0;
    for (size_t i = 0; i < count; ++i) {
        current = std::min(needed[i], current + (1.0 - current) * release);
        const double value = clip[i] * static_cast<double>(levels.descriptionGain) * current;
        out[i] = static_cast<int16_t>(std::lround(std::clamp(value, -limit, limit)));
    }
    return out;
}

class Mixer {
public:
    explicit Mixer(Levels levels = {}) : m_levels(levels) {}

    // A described movie starts playing from its beginning.
    void Start(std::shared_ptr<const Cues> cues) {
        m_cues = std::move(cues);
        m_next = 0;
        m_active = nullptr;
        m_cutting = false;
        m_fade = 1.0;
        m_lastSeconds = 0.0;
        m_described = m_cues != nullptr;
    }

    // The movie ends or is closed: a clip still speaking fades out, the game's sound returns to full level.
    void Stop() {
        m_described = false;
        if (m_active != nullptr) m_cutting = true;
        else m_cues.reset();
    }

    bool Idle() const {
        return !m_described && m_active == nullptr && m_movieGain == 1.0 && m_duck == 1.0 && m_limiter == 1.0;
    }

    // Processes `frames` interleaved stereo frames of final output in place. `seconds` is the movie's position at the
    // first frame; `playing` is false while the movie is paused, which holds the description where it is (after a
    // short fade) until the movie resumes.
    void Process(int16_t* samples, size_t frames, uint32_t sampleRate, double seconds, bool playing) {
        if (Idle() || frames == 0 || sampleRate == 0) return;
        if (m_described && playing) StartDueCue(seconds);

        // The ramps run in double: in float, a one-pole step this small stalls short of its target.
        const double rate = static_cast<double>(sampleRate);
        const double duckAttack = Coefficient(m_levels.duckAttackSeconds, rate);
        const double duckRelease = Coefficient(m_levels.duckReleaseSeconds, rate);
        const double movieRamp = Coefficient(m_levels.movieRampSeconds, rate);
        const double limiterRelease = Coefficient(m_levels.limiterReleaseSeconds, rate);
        const double fadeStep = 1.0 / (m_levels.fadeSeconds * rate);
        const double limit = m_levels.limit * 32767.0;

        for (size_t frame = 0; frame < frames; ++frame) {
            const bool speaking = m_active != nullptr && playing && !m_cutting;
            const double duckTarget = speaking ? m_levels.duckGain : 1.0;
            const double movieTarget = m_described ? m_levels.movieGain : 1.0;
            m_duck += (duckTarget - m_duck) * (duckTarget < m_duck ? duckAttack : duckRelease);
            m_movieGain += (movieTarget - m_movieGain) * movieRamp;

            double description = 0.0;
            if (m_active != nullptr) {
                m_fade = speaking ? std::min(1.0, m_fade + fadeStep) : std::max(0.0, m_fade - fadeStep);
                if (m_fade > 0.0) {
                    description = m_active->samples[m_position] * m_fade;
                    if (++m_position >= m_active->samples.size()) Release();
                } else if (m_cutting) {
                    Release();
                }
            }

            const double gain = m_movieGain * m_duck;
            double left = samples[frame * 2] * gain + description;
            double right = samples[frame * 2 + 1] * gain + description;
            const double peak = std::max(std::fabs(left), std::fabs(right)) * m_limiter;
            if (peak > limit) m_limiter *= limit / peak;
            else m_limiter += (1.0 - m_limiter) * limiterRelease;
            left *= m_limiter;
            right *= m_limiter;
            samples[frame * 2] = Clamp(left);
            samples[frame * 2 + 1] = Clamp(right);
        }
        Settle();
        if (playing) m_lastSeconds = seconds + static_cast<double>(frames) / sampleRate;
    }

private:
    // A clip starts at the first block that reaches its cue. A position that jumps backwards (a looping movie) fades
    // out the clip in progress and starts over from the first cue; a jump past several cues (a stall) starts the last
    // one due, and a cue more than kMaxLateness late is skipped.
    void StartDueCue(double seconds) {
        const Cues& cues = *m_cues;
        if (seconds + kLoopTolerance < m_lastSeconds) {
            m_next = 0;
            if (m_active != nullptr) m_cutting = true;
        }
        if (m_active != nullptr) return;
        while (m_next + 1 < cues.size() && cues[m_next + 1].start <= seconds) ++m_next;
        if (m_next >= cues.size() || cues[m_next].start > seconds) return;
        const Cue& cue = cues[m_next++];
        if (seconds - cue.start > kMaxLateness || cue.samples.empty()) return;
        m_active = &cue;
        m_position = 0;
        m_fade = 1.0;  // clips begin on silence; only cuts need the fade
    }

    void Release() {
        m_active = nullptr;
        m_cutting = false;
        if (!m_described) m_cues.reset();
    }

    // Ends the ramps exactly at full level, so the idle check can skip later blocks.
    void Settle() {
        if (!m_described && std::fabs(m_movieGain - 1.0) < 1e-4) m_movieGain = 1.0;
        if (m_active == nullptr && std::fabs(m_duck - 1.0) < 1e-4) m_duck = 1.0;
        if (std::fabs(m_limiter - 1.0) < 1e-4) m_limiter = 1.0;
    }

    static double Coefficient(float seconds, double rate) { return 1.0 - std::exp(-1.0 / (seconds * rate)); }

    static int16_t Clamp(double value) {
        return static_cast<int16_t>(std::lround(std::clamp(value, -32768.0, 32767.0)));
    }

    static constexpr double kLoopTolerance = 0.5;
    static constexpr double kMaxLateness = 1.0;

    Levels m_levels;
    std::shared_ptr<const Cues> m_cues;
    size_t m_next = 0;
    const Cue* m_active = nullptr;
    size_t m_position = 0;
    bool m_cutting = false;  // the active clip is fading out for good
    double m_fade = 1.0;
    double m_lastSeconds = 0.0;
    bool m_described = false;
    double m_movieGain = 1.0;
    double m_duck = 1.0;
    double m_limiter = 1.0;
};

} // namespace AudioDescriptionMix
