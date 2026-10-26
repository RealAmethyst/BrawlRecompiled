// Offline check of the audio description mixer (wiicompiled/runtime/include/audio_description_mix.h).
//
//   ad_mix_check selftest
//       Pause, resume and loop behaviour on synthetic signals; exits non-zero on a failure.
//   ad_mix_check mix <movie.s16> <manifest> <fps> <out.s16>
//       Mixes a movie's sound (raw S16LE stereo, 32 kHz) with the clips in <manifest> (lines "start path", each clip
//       raw S16LE mono at 32 kHz) the way the runtime does: 96-frame blocks, the clock from whole movie frames plus
//       the samples output since the last frame, as THPPlayer's audio frame count gives it.
//
// Build: g++ -std=c++20 -O2 -I wiicompiled/runtime/include accessibility/tools/ad_mix_check.cpp -o ad_mix_check

#include "audio_description_mix.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRate = 32000;
constexpr size_t kBlock = 96;

std::vector<int16_t> ReadRaw(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<int16_t> samples(bytes.size() / 2);
    std::memcpy(samples.data(), bytes.data(), samples.size() * 2);
    return samples;
}

// Runs `seconds(frame index)` and `playing(frame index)` through the mixer over `movie` (stereo), as the runtime does.
template <typename Clock, typename Playing>
std::vector<int16_t> Run(std::vector<int16_t> movie, std::shared_ptr<const AudioDescriptionMix::Cues> cues,
                         Clock clock, Playing playing) {
    AudioDescriptionMix::Mixer mixer;
    mixer.Start(std::move(cues));
    const size_t frames = movie.size() / 2;
    for (size_t at = 0; at < frames; at += kBlock) {
        const size_t count = std::min(kBlock, frames - at);
        mixer.Process(movie.data() + at * 2, count, kRate, clock(at), playing(at));
    }
    return movie;
}

int Fail(const char* what) {
    std::cerr << "FAIL: " << what << std::endl;
    return 1;
}

int SelfTest() {
    // One 1-second clip of a counting signal at 2.0 s, over silence.
    auto cues = std::make_shared<AudioDescriptionMix::Cues>();
    AudioDescriptionMix::Cue cue;
    cue.start = 2.0;
    for (int i = 0; i < static_cast<int>(kRate); ++i) cue.samples.push_back(static_cast<int16_t>(i % 2000 - 1000));
    cues->push_back(cue);
    const std::vector<int16_t> silence(kRate * 6 * 2, 0);

    const auto steady = [](size_t at) { return static_cast<double>(at) / kRate; };
    const auto always = [](size_t) { return true; };
    const std::vector<int16_t> plain = Run(silence, cues, steady, always);

    // The clip lands at 2.0 s, within one block.
    size_t first = 0;
    while (first < plain.size() / 2 && plain[first * 2] == 0) ++first;
    if (first < 2 * kRate || first > 2 * kRate + kBlock) return Fail("clip start");

    // A 1.5 s pause during the clip, on block boundaries as the runtime sees it: the clip fades out over 8 ms while
    // still advancing, holds, fades back in on resume and continues where it stopped.
    const size_t fadeFrames = kRate * 8 / 1000;  // the 8 ms fade, stated here rather than read from the mixer
    const size_t pauseFrame = 834 * kBlock, pauseFrames = 500 * kBlock;
    const double pauseAt = static_cast<double>(pauseFrame) / kRate, pauseFor = static_cast<double>(pauseFrames) / kRate;
    const auto paused = [&](size_t at) {
        const double t = static_cast<double>(at) / kRate;
        return !(t >= pauseAt && t < pauseAt + pauseFor);
    };
    const auto pausedClock = [&](size_t at) {
        const double t = static_cast<double>(at) / kRate;
        return t < pauseAt ? t : t < pauseAt + pauseFor ? pauseAt : t - pauseFor;
    };
    const std::vector<int16_t> withPause = Run(silence, cues, pausedClock, paused);
    for (size_t f = 0; f < pauseFrame; ++f) {
        if (withPause[f * 2] != plain[f * 2]) return Fail("before the pause");
    }
    for (size_t f = pauseFrame; f < pauseFrame + fadeFrames; ++f) {
        if (std::abs(withPause[f * 2]) > std::abs(plain[f * 2])) return Fail("fade out on pause");
    }
    for (size_t f = pauseFrame + fadeFrames; f < pauseFrame + pauseFrames; ++f) {
        if (withPause[f * 2] != 0) return Fail("description during pause");
    }
    // After the fade-in, the paused run is the plain one delayed by the pause less the fade-out it used.
    const size_t resume = pauseFrame + pauseFrames + fadeFrames, shift = pauseFrames - fadeFrames;
    for (size_t f = resume; f < plain.size() / 2; ++f) {
        if (std::abs(withPause[f * 2] - plain[(f - shift) * 2]) > 1) return Fail("resume continues where it stopped");
    }

    // A looping movie (3 s long): the clip plays again on the second pass.
    const auto looped = [](size_t at) { return std::fmod(static_cast<double>(at) / kRate, 3.0); };
    const std::vector<int16_t> loop = Run(silence, cues, looped, always);
    size_t second = 4 * kRate;
    while (second < loop.size() / 2 && loop[second * 2] == 0) ++second;
    if (second < 5 * kRate || second > 5 * kRate + kBlock) return Fail("clip again after a loop");

    // Stop: the game's sound ramps back to full level and the mixer goes idle.
    AudioDescriptionMix::Mixer mixer;
    mixer.Start(cues);
    // (Before the first cue: 3 s is ten time constants of the 300 ms ramp.)
    std::vector<int16_t> tone(kRate * 6 * 2, 10000);
    mixer.Process(tone.data(), kRate * 3 / 2, kRate, 0.0, true);
    mixer.Process(tone.data() + kRate * 3, kRate * 3 / 2, kRate, 1.5, true);
    if (std::abs(tone[(kRate * 3 - 1) * 2] - 5012) > 2) return Fail("movie level -6 dB");
    mixer.Stop();
    mixer.Process(tone.data() + kRate * 6, kRate * 3, kRate, 0.0, false);
    if (!mixer.Idle() || tone.back() != 10000) return Fail("full level after stop");

    // Stop in the middle of a clip: it fades out within 8 ms instead of cutting, and the cues stay alive until then.
    {
        AudioDescriptionMix::Mixer cut;
        cut.Start(cues);
        const size_t stopAt = 2 * kRate + 4800;
        std::vector<int16_t> block(kBlock * 2, 0);
        for (size_t at = 0; at < stopAt; at += kBlock) {
            std::fill(block.begin(), block.end(), 0);
            cut.Process(block.data(), kBlock, kRate, static_cast<double>(at) / kRate, true);
        }
        const int16_t before = block[(kBlock - 1) * 2];
        cut.Stop();
        std::vector<int16_t> tail(kRate / 10 * 2, 0);
        cut.Process(tail.data(), kRate / 10, kRate, 0.0, false);
        int previous = before;
        for (size_t f = 0; f < fadeFrames; ++f) {
            if (std::abs(tail[f * 2] - previous) > 40) return Fail("fade out on stop");
            previous = tail[f * 2];
        }
        for (size_t f = fadeFrames + 1; f < tail.size() / 2; ++f) {
            if (tail[f * 2] != 0) return Fail("silence after the stop fade");
        }
    }

    // Clip preparation: +2 dB, peaks held at 98 % of full scale by a look-ahead limiter, quiet passages untouched.
    {
        std::vector<int16_t> clip(kRate, 1000);  // 0.75 s after the peak: the 50 ms release has fully recovered
        for (size_t i = 8000; i < 8100; ++i) clip[i] = static_cast<int16_t>(i % 2 ? 32767 : -32767);
        const std::vector<int16_t> prepared = AudioDescriptionMix::PrepareClip(clip, kRate);
        const int limit = static_cast<int>(0.98 * 32767.0 + 0.5);
        for (const int16_t sample : prepared) {
            if (std::abs(sample) > limit) return Fail("prepared clip peak");
        }
        if (prepared[100] != 1259 || prepared[clip.size() - 1] != 1259) return Fail("prepared clip gain");
        for (size_t i = 7700; i < 8000; ++i) {
            if (std::abs(prepared[i] - prepared[i - 1]) > 20) return Fail("limiter ramps before a peak");
        }
    }

    std::cout << "selftest passed" << std::endl;
    return 0;
}

int Mix(const std::string& moviePath, const std::string& manifest, double fps, const std::string& outPath) {
    std::vector<int16_t> movie = ReadRaw(moviePath);
    auto cues = std::make_shared<AudioDescriptionMix::Cues>();
    std::ifstream list(manifest);
    std::string line;
    while (std::getline(list, line)) {
        std::istringstream fields(line);
        AudioDescriptionMix::Cue cue;
        std::string path;
        if (!(fields >> cue.start >> path)) continue;
        cue.samples = AudioDescriptionMix::PrepareClip(ReadRaw(path), kRate);
        cues->push_back(std::move(cue));
    }
    // Whole movie frames from the audio frame count, plus the samples since the count last changed (capped at one
    // frame), exactly as AudioDescriptionPlayback::MovieSeconds computes it.
    const auto clock = [fps](size_t at) {
        const double t = static_cast<double>(at) / kRate;
        const double frame = std::floor(t * fps);
        return frame / fps + std::min(t - frame / fps, 1.0 / fps);
    };
    const std::vector<int16_t> out = Run(std::move(movie), cues, clock, [](size_t) { return true; });
    std::ofstream file(outPath, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size() * 2));
    std::cout << cues->size() << " cues mixed" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "selftest") return SelfTest();
    if (args.size() == 5 && args[0] == "mix") return Mix(args[1], args[2], std::stod(args[3]), args[4]);
    std::cerr << "usage: ad_mix_check selftest | mix <movie.s16> <manifest> <fps> <out.s16>" << std::endl;
    return 2;
}
