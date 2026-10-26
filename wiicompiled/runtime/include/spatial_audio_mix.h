#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// The signal path of spatial audio (src/accessibility/spatial_audio.cpp), kept free of guest memory and of Steam Audio
// itself so it can be checked offline (accessibility/tools/spatial_mix_check.cpp).
//
// The game's mix stays exactly as it is, guest memory included. For each positional voice the mixer reports what that
// voice added to the main left and right buses this AX frame; the renderer takes it back out and puts the same signal
// back through an HRTF from the sound's direction. The difference (HRTF minus original) is a host-only correction added
// to the frame the player hears.
//
// The original contribution carries the game's own loudness (distance, fades, ducking, the volume envelope) and its
// pan. The pan is undone by taking the level both channels share: the game pans with constant power, so a sample's
// pan-free value is sqrt(L^2 + R^2) with the sign of the sample.
//
// Steam Audio's built-in HRTF takes 24, 44.1 or 48 kHz only (measured with v4.8.1: 32 kHz fails with
// IPL_STATUS_INITIALIZATION), so each voice is resampled from the AX rate of 32 kHz to 48 kHz, rendered there in
// 144-sample frames, and the summed binaural bus is resampled back to 32 kHz.
namespace SpatialAudioMix {

constexpr uint32_t kMixRate = 32000;
constexpr uint32_t kMixFrame = 96;  // one AX frame, 3 ms
constexpr uint32_t kRenderRate = 48000;
constexpr uint32_t kRenderFrame = kMixFrame * kRenderRate / kMixRate;  // 144

// Listener space, as the game's Sound3DListener and Steam Audio both use it: +x right, +y up, -z ahead. Unit length.
struct Direction {
    float x = 0.0f;
    float y = 0.0f;
    float z = -1.0f;
};

// The front-centre level match: a sound straight ahead keeps the loudness the game's centre pan gives it (-3.01 dB per
// channel). Measured through this renderer with Steam Audio 4.8.1's default HRTF and white noise at 32 kHz
// (spatial_mix_check): the HRTF itself is asymmetric, about 1.3 dB louder in the right ear than the left.
constexpr float kFrontMatchGain = 0.7776f;

// One HRTF instance, owned by one voice slot and used only from the mix thread.
class Binaural {
public:
    virtual ~Binaural() = default;
    // Clears the filter state: the slot now belongs to another sound.
    virtual void Reset() = 0;
    // Renders kRenderFrame mono samples from `direction` into left and right (overwritten).
    virtual void Apply(const Direction& direction, const float* input, float* left, float* right) = 0;
};

// Kaiser-windowed sinc low-pass at the common rate of 96 kHz, cut off below 16 kHz (the AX Nyquist frequency), for both
// conversions. DC gain is the upsampling factor of the conversion that uses it.
class Prototype {
public:
    static constexpr uint32_t kRate = 96000;
    static constexpr uint32_t kTaps = 192;

    static const std::array<float, kTaps>& Taps() {
        static const std::array<float, kTaps> taps = Design();
        return taps;
    }

private:
    static double BesselI0(double x) {
        double sum = 1.0;
        double term = 1.0;
        for (int k = 1; k < 40; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    static std::array<float, kTaps> Design() {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kCutoff = 15400.0 / kRate;  // cycles per sample at 96 kHz
        constexpr double kBeta = 9.0;
        std::array<double, kTaps> h{};
        double sum = 0.0;
        const double centre = (kTaps - 1) / 2.0;
        for (uint32_t i = 0; i < kTaps; ++i) {
            const double t = i - centre;
            const double sinc = t == 0.0 ? 2.0 * kCutoff : std::sin(2.0 * kPi * kCutoff * t) / (kPi * t);
            const double r = t / centre;
            const double window = BesselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / BesselI0(kBeta);
            h[i] = sinc * window;
            sum += h[i];
        }
        std::array<float, kTaps> taps{};
        for (uint32_t i = 0; i < kTaps; ++i) taps[i] = static_cast<float>(h[i] / sum);
        return taps;
    }
};

// Rational resampler through the 96 kHz prototype: kUp input positions and kDown output positions per 96 kHz step, so
// 32 -> 48 kHz is <3, 2> and 48 -> 32 kHz is <2, 3>. Frames are whole numbers of both periods (96 <-> 144), so every
// frame uses the same phases. Causal: 1 ms of delay (half the filter at 96 kHz).
template <uint32_t kUp, uint32_t kDown, uint32_t kIn>
class Resampler {
public:
    static constexpr uint32_t kOut = kIn * kUp / kDown;
    static_assert(kIn * kUp % kDown == 0, "frames must hold whole resampling periods");
    static constexpr uint32_t kHistory = (Prototype::kTaps + kUp - 1) / kUp;

    void Reset() { m_buffer.fill(0.0f); }

    void Process(const float* input, float* output) {
        std::copy(input, input + kIn, m_buffer.begin() + kHistory);
        const auto& h = Prototype::Taps();
        for (uint32_t n = 0; n < kOut; ++n) {
            // y[n] = kUp * sum_k x[k] h[n*kDown - k*kUp], over the k that keep the tap index inside the filter.
            const int32_t position = static_cast<int32_t>(n * kDown);
            int32_t k = position / static_cast<int32_t>(kUp);
            float sum = 0.0f;
            for (int32_t tap = position - k * static_cast<int32_t>(kUp);
                 tap < static_cast<int32_t>(Prototype::kTaps); tap += kUp, --k) {
                sum += m_buffer[static_cast<size_t>(k + static_cast<int32_t>(kHistory))] * h[static_cast<size_t>(tap)];
            }
            output[n] = sum * static_cast<float>(kUp);
        }
        std::copy(m_buffer.begin() + kIn, m_buffer.begin() + kIn + kHistory, m_buffer.begin());
    }

private:
    std::array<float, kHistory + kIn> m_buffer{};
};

using Upsampler = Resampler<3, 2, kMixFrame>;
using Downsampler = Resampler<2, 3, kRenderFrame>;
static_assert(Upsampler::kOut == kRenderFrame && Downsampler::kOut == kMixFrame);

class Renderer {
public:
    // The pool fixes how many voices can be placed at once; a voice beyond it stays in the game's own mix.
    explicit Renderer(std::vector<std::unique_ptr<Binaural>> pool) {
        m_slots.resize(pool.size());
        for (size_t i = 0; i < pool.size(); ++i) m_slots[i].binaural = std::move(pool[i]);
    }

    size_t Capacity() const { return m_slots.size(); }

    // Forgets every voice, filter and tail: rendering resumes after a pause in which frames went unrendered.
    void Reset() {
        for (Slot& slot : m_slots) {
            slot.key = 0;
            slot.binaural->Reset();
            slot.upsampler.Reset();
        }
        m_down[0].Reset();
        m_down[1].Reset();
        m_downsamplerIdle = true;
    }

    void BeginFrame() {
        ++m_frame;
        m_delta[0].fill(0);
        m_delta[1].fill(0);
        m_bus[0].fill(0.0f);
        m_bus[1].fill(0.0f);
        m_used = false;
    }

    // A placed voice's main-bus contribution this frame, exactly what the game's mix added to main left and right.
    // `key` names the parameter block, `owner` the sound and channel playing on it (a new owner restarts the filter).
    // Returns false when no HRTF slot is free: the voice then stays as the game mixed it.
    bool AddVoice(uint32_t key, uint64_t owner, const Direction& target, const int32_t* mainL, const int32_t* mainR) {
        Slot* slot = Claim(key, owner, target);
        if (slot == nullptr || slot->fed == m_frame) return false;  // a block mixed twice in one frame stays vanilla
        std::array<float, kMixFrame> mono{};
        for (uint32_t i = 0; i < kMixFrame; ++i) {
            const float l = static_cast<float>(mainL[i]);
            const float r = static_cast<float>(mainR[i]);
            const float level = std::sqrt(l * l + r * r);
            mono[i] = (l + r < 0.0f ? -level : level) * kFrontMatchGain;
            m_delta[0][i] -= mainL[i];
            m_delta[1][i] -= mainR[i];
        }
        Render(*slot, mono.data());
        slot->fed = m_frame;
        return true;
    }

    // Ends the frame: voices that stopped finish their filter tails, the binaural bus returns to 32 kHz, and `deltaL`
    // and `deltaR` receive what must be added to the game's main buses (binaural minus the original contributions).
    // Returns false when nothing was placed and no tail is left, so the frame needs no correction.
    bool EndFrame(int32_t* deltaL, int32_t* deltaR) {
        static const std::array<float, kMixFrame> silence{};
        for (Slot& slot : m_slots) {
            if (slot.key == 0 || slot.fed == m_frame) continue;
            if (m_frame - slot.fed <= kTailFrames) Render(slot, silence.data());
            else if (m_frame - slot.fed > kReleaseFrames) slot.key = 0;
        }
        if (!m_used && m_downsamplerIdle) return false;
        std::array<float, kMixFrame> left{};
        std::array<float, kMixFrame> right{};
        m_down[0].Process(m_bus[0].data(), left.data());
        m_down[1].Process(m_bus[1].data(), right.data());
        // The downsampler keeps a few milliseconds of history; it is idle once a whole frame of silence has gone through.
        m_downsamplerIdle = !m_used;
        for (uint32_t i = 0; i < kMixFrame; ++i) {
            deltaL[i] = m_delta[0][i] + static_cast<int32_t>(std::lround(left[i]));
            deltaR[i] = m_delta[1][i] + static_cast<int32_t>(std::lround(right[i]));
        }
        return true;
    }

private:
    // A voice that stops is followed by silence for this many frames, so the resampler and HRTF filters ring out (the
    // default HRTF's response is gone within two 3 ms frames at 48 kHz, measured; Steam Audio 4.8.1 reports a binaural
    // tail as remaining on every frame, so its state cannot tell).
    static constexpr uint64_t kTailFrames = 3;
    // A slot whose voice has stopped keeps its key for a while so a restart on the same block finds it.
    static constexpr uint64_t kReleaseFrames = 32;
    // The direction moves toward the game's position (updated once per video frame, every 5 or 6 AX frames) by this
    // share per AX frame, so it glides instead of stepping.
    static constexpr float kGlide = 0.35f;

    struct Slot {
        std::unique_ptr<Binaural> binaural;
        Upsampler upsampler;
        uint32_t key = 0;  // 0: free
        uint64_t owner = 0;
        uint64_t fed = 0;  // the last frame with input
        Direction direction;
    };

    Slot* Claim(uint32_t key, uint64_t owner, const Direction& target) {
        Slot* free = nullptr;
        for (Slot& slot : m_slots) {
            if (slot.key == key) {
                if (slot.fed == m_frame) return &slot;
                // Another owner, or the same pooled sound object started again after a gap: a new sound.
                if (slot.owner != owner || m_frame - slot.fed > 1) {
                    Restart(slot, owner, target);
                } else {
                    slot.direction = Glide(slot.direction, target);
                }
                return &slot;
            }
            if (free == nullptr && slot.key == 0) free = &slot;
        }
        if (free == nullptr) return nullptr;
        free->key = key;
        Restart(*free, owner, target);
        return free;
    }

    static void Restart(Slot& slot, uint64_t owner, const Direction& target) {
        slot.owner = owner;
        slot.direction = target;
        slot.binaural->Reset();
        slot.upsampler.Reset();
    }

    static Direction Glide(const Direction& from, const Direction& to) {
        Direction d{from.x + (to.x - from.x) * kGlide, from.y + (to.y - from.y) * kGlide,
                    from.z + (to.z - from.z) * kGlide};
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (length < 1e-4f) return to;  // opposite directions: jump rather than pass through the head
        return {d.x / length, d.y / length, d.z / length};
    }

    void Render(Slot& slot, const float* mono) {
        std::array<float, kRenderFrame> input{};
        std::array<float, kRenderFrame> left{};
        std::array<float, kRenderFrame> right{};
        slot.upsampler.Process(mono, input.data());
        slot.binaural->Apply(slot.direction, input.data(), left.data(), right.data());
        for (uint32_t i = 0; i < kRenderFrame; ++i) {
            m_bus[0][i] += left[i];
            m_bus[1][i] += right[i];
        }
        m_used = true;
    }

    std::vector<Slot> m_slots;
    std::array<std::array<int32_t, kMixFrame>, 2> m_delta{};
    std::array<std::array<float, kRenderFrame>, 2> m_bus{};
    std::array<Downsampler, 2> m_down;
    uint64_t m_frame = 0;
    bool m_used = false;
    bool m_downsamplerIdle = true;
};

}  // namespace SpatialAudioMix
