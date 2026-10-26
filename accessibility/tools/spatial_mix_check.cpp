// Offline checks of the spatial audio signal path (wiicompiled/runtime/include/spatial_audio_mix.h) with the real Steam
// Audio library, without the game.
//
// Build (from the repository root; STEAMAUDIO is the extracted SDK, steamaudio_4.8.1.zip):
//   g++ -std=c++20 -O2 -I wiicompiled/runtime/include -I $STEAMAUDIO/include accessibility/tools/spatial_mix_check.cpp
//       -L $STEAMAUDIO/lib/linux-x64 -lphonon -Wl,-rpath,$STEAMAUDIO/lib/linux-x64 -o spatial_mix_check
// Run: ./spatial_mix_check   (exit status 0 when every check passes)
#include "spatial_audio_steam.h"

#include <cmath>
#include <cstdio>
#include <random>

namespace {

using namespace SpatialAudioMix;

int g_failures = 0;

void Check(bool ok, const char* what, double value) {
    std::printf("%s %-58s %.3f\n", ok ? "pass" : "FAIL", what, value);
    if (!ok) ++g_failures;
}

double Db(double ratio) { return 10.0 * std::log10(ratio); }

const SpatialAudioSteam::Api& LinkedApi() {
    static const SpatialAudioSteam::Api api{&iplContextCreate,        &iplContextRelease,      &iplHRTFCreate,
                                            &iplHRTFRelease,          &iplBinauralEffectCreate, &iplBinauralEffectRelease,
                                            &iplBinauralEffectReset,  &iplBinauralEffectApply};
    return api;
}

// A sine through 32 -> 48 -> 32 kHz comes back at the same level, and an image above 16 kHz is rejected.
void CheckResamplers() {
    for (double hz : {100.0, 1000.0, 8000.0, 14000.0}) {
        Upsampler up;
        Downsampler down;
        double in = 0.0, out = 0.0;
        for (int frame = 0; frame < 400; ++frame) {
            std::array<float, kMixFrame> x{};
            std::array<float, kRenderFrame> mid{};
            std::array<float, kMixFrame> y{};
            for (uint32_t i = 0; i < kMixFrame; ++i) {
                x[i] = static_cast<float>(std::sin(2.0 * 3.141592653589793 * hz * (frame * kMixFrame + i) / kMixRate));
            }
            up.Process(x.data(), mid.data());
            down.Process(mid.data(), y.data());
            if (frame < 10) continue;
            for (uint32_t i = 0; i < kMixFrame; ++i) {
                in += x[i] * x[i];
                out += y[i] * y[i];
            }
        }
        char label[64];
        std::snprintf(label, sizeof label, "round trip at %.0f Hz keeps its level (dB)", hz);
        Check(std::fabs(Db(out / in)) < (hz <= 8000.0 ? 0.1 : 1.0), label, Db(out / in));
    }
    // 20 kHz at 48 kHz cannot be represented at 32 kHz: the downsampler must remove it.
    Downsampler down;
    double in = 0.0, out = 0.0;
    for (int frame = 0; frame < 200; ++frame) {
        std::array<float, kRenderFrame> x{};
        std::array<float, kMixFrame> y{};
        for (uint32_t i = 0; i < kRenderFrame; ++i) {
            x[i] = static_cast<float>(std::sin(2.0 * 3.141592653589793 * 20000.0 * (frame * kRenderFrame + i) / kRenderRate));
        }
        down.Process(x.data(), y.data());
        if (frame < 10) continue;
        for (float v : x) in += v * v;
        for (float v : y) out += v * v;
    }
    Check(Db(out / in) < -40.0, "20 kHz at 48 kHz rejected on the way to 32 kHz (dB)", Db(out / in));
}

struct Result {
    double vanillaL = 0, vanillaR = 0, heardL = 0, heardR = 0;
};

// One voice of noise, panned by the game with `panL`/`panR` (constant power), placed at `direction`. Returns the
// energy of the game's own mix and of what the player hears (game mix + correction).
Result RunVoice(Renderer& renderer, const Direction& direction, float panL, float panR, int frames, uint32_t key = 0x100,
                uint64_t owner = 1) {
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 3000.0f);
    Result result;
    for (int frame = 0; frame < frames; ++frame) {
        std::array<int32_t, kMixFrame> l{}, r{}, dl{}, dr{};
        for (uint32_t i = 0; i < kMixFrame; ++i) {
            const float s = noise(rng);
            l[i] = static_cast<int32_t>(s * panL);
            r[i] = static_cast<int32_t>(s * panR);
        }
        renderer.BeginFrame();
        renderer.AddVoice(key, owner, direction, l.data(), r.data());
        if (!renderer.EndFrame(dl.data(), dr.data())) dl.fill(0), dr.fill(0);
        if (frame < 20) continue;
        for (uint32_t i = 0; i < kMixFrame; ++i) {
            result.vanillaL += double(l[i]) * l[i];
            result.vanillaR += double(r[i]) * r[i];
            result.heardL += double(l[i] + dl[i]) * (l[i] + dl[i]);
            result.heardR += double(r[i] + dr[i]) * (r[i] + dr[i]);
        }
    }
    return result;
}

void CheckRenderer() {
    SpatialAudioSteam::Engine engine(LinkedApi());
    const std::string error = engine.Create(4);
    Check(error.empty(), "Steam Audio context, HRTF and 4 effects at 48 kHz / 144", 0);
    if (!error.empty()) return;
    Renderer renderer(engine.TakePool());

    // Centre pan, straight ahead: about the game's loudness in each ear (the HRTF is slightly asymmetric).
    const float centre = 0.70710678f;
    Result front = RunVoice(renderer, {0, 0, -1}, centre, centre, 600);
    const double frontL = Db(front.heardL / front.vanillaL), frontR = Db(front.heardR / front.vanillaR);
    Check(std::fabs(Db((front.heardL + front.heardR) / (front.vanillaL + front.vanillaR))) < 0.5,
          "straight ahead keeps the game's loudness (both ears, dB)",
          Db((front.heardL + front.heardR) / (front.vanillaL + front.vanillaR)));
    Check(std::fabs(frontL - frontR) < 3.0, "straight ahead stays near the middle (L-R, dB)", frontL - frontR);

    // Hard right: the right ear clearly louder, and the game's own pan (here: panned left) is not heard.
    Renderer right(engine.TakePool());  // empty pool: every voice must stay vanilla
    Result vanilla = RunVoice(right, {1, 0, 0}, 1.0f, 0.0f, 100);
    Check(vanilla.heardL == vanilla.vanillaL && vanilla.heardR == vanilla.vanillaR,
          "no free slot: the voice stays exactly as the game mixed it", vanilla.heardL - vanilla.vanillaL);

    Result side = RunVoice(renderer, {1, 0, 0}, 1.0f, 0.0f, 600, 0x100, 2);
    Check(Db(side.heardR / side.heardL) > 10.0, "a sound to the right (panned left by the game) is heard right (dB)",
          Db(side.heardR / side.heardL));

    // After the voice stops, the tail ends and the correction stops.
    int framesUntilIdle = -1;
    for (int frame = 0; frame < 50; ++frame) {
        std::array<int32_t, kMixFrame> dl{}, dr{};
        renderer.BeginFrame();
        if (!renderer.EndFrame(dl.data(), dr.data())) {
            framesUntilIdle = frame;
            break;
        }
    }
    Check(framesUntilIdle >= 1 && framesUntilIdle <= 6, "after a stop, the tail ends within a few frames", framesUntilIdle);

    // Turned off mid-sound and on again: nothing from before plays.
    RunVoice(renderer, {1, 0, 0}, 1.0f, 0.0f, 50, 0x100, 3);
    renderer.Reset();
    std::array<int32_t, kMixFrame> dl{}, dr{};
    renderer.BeginFrame();
    Check(!renderer.EndFrame(dl.data(), dr.data()), "after a reset, no tail of the old sound plays", 0);
}

}  // namespace

int main() {
    CheckResamplers();
    CheckRenderer();
    std::printf("%s\n", g_failures == 0 ? "all checks passed" : "checks FAILED");
    return g_failures == 0 ? 0 : 1;
}
