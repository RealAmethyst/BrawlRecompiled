#include "audio_description.h"

#include "accessibility_text.h"
#include "audio_description_mix.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_stdinc.h>
#include <toml.hpp>

// Audio description for the game's movies. Every movie (the opening, the Subspace Emissary cutscenes, the endings, How
// to Play, the Vault) plays through main.dol's THPPlayer behind the mvMoviePlayer wrapper, so four event hooks there
// follow each movie: open (which file), play (start or resume), pause and stop. The description pack is installed
// separately beside the executable (accessibility/audio-description/en/<movie>/cues.toml and its WAV clips); a movie
// without one plays as before.
//
// Timing follows what the player hears: the movie's clock is THPPlayer's count of audio frames played, advanced by its AX
// callback, read once per output block and refined by the samples output since it last changed. Mixing happens on the
// host side of the final output (AudioDescription::ProcessOutput), so guest memory is only read, never written.

extern "C" void func_8007EC08(CpuContext* ctx);  // mvMoviePlayer: open r4 = "/movie/<name>.thp" (every movie)
extern "C" void func_8007C944(CpuContext* ctx);  // THPPlayerPlay: prepared or paused -> playing; returns 1 on success
extern "C" void func_8007CB08(CpuContext* ctx);  // THPPlayerPause: playing -> paused; returns 1 on success
extern "C" void func_8007C9DC(CpuContext* ctx);  // THPPlayerStop: from the wrapper's close, re-open and error paths

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace AudioDescriptionPlayback {

// THPPlayer ("ActivePlayer", 0x1B0 bytes) at a fixed address. THPPlayerOpen copies the file header to +0x3C
// (0x8007BEE8), so the header's frame rate and frame count land at +0x4C and +0x50 (the draw's modulo, 0x8007D0C4).
constexpr uint32_t kPlayer = 0x8049DC00;
constexpr uint32_t kFrameRate = kPlayer + 0x4C;    // f32
constexpr uint32_t kFrameCount = kPlayer + 0x50;   // u32
constexpr uint32_t kState = kPlayer + 0xA4;        // u8: 0 stop, 1 prepared, 2 playing, 3 played, 4 paused, 5 error
constexpr uint32_t kFlags = kPlayer + 0xA6;        // u8: bit 0 loops (end check 0x8007CE4C)
constexpr uint32_t kHasAudio = kPlayer + 0xA7;     // u8 (THPPlayerPlay 0x8007C988)
constexpr uint32_t kAudioFrames = kPlayer + 0xE4;  // u32: audio frames played (AX callback 0x8007DF00; 0 at prepare)
constexpr uint8_t kStatePrepared = 1;

constexpr uint32_t kOutputRate = 32000;  // the runtime's output rate (src/hle/audio/audio.cpp, kDefaultSampleRate)
constexpr size_t kMaxPathBytes = 64;
constexpr char kLanguage[] = "en";       // the supported disc (RSBE01) is English

struct Load {
    std::mutex mutex;
    bool done = false;
    std::shared_ptr<const AudioDescriptionMix::Cues> cues;  // null: the movie has no usable description
};

std::mutex g_mutex;
AudioDescriptionMix::Mixer g_mixer;
std::shared_ptr<Load> g_load;  // the open movie's description, loading or loaded
std::string g_movie;
bool g_started = false;        // the movie has started playing
bool g_mixing = false;         // its description has been handed to the mixer
bool g_playing = false;
bool g_loops = false;
double g_frameRate = 0.0;
uint32_t g_frameCount = 0;
uint32_t g_baseFrames = 0;
uint32_t g_lastFrames = 0;
uint64_t g_samplesSinceFrame = 0;
std::atomic<bool> g_engaged{false};  // anything to do in ProcessOutput

void Log(const std::string& message) {
    RT_LOG(RT_TAG_ACCESSIBILITY) << "audio description: " << message << std::endl;
}

// "OPMovie" from "/movie/OPMovie.thp"; empty unless the path has exactly that shape.
std::string MovieName(uint32_t path) {
    std::string text;
    try {
        for (size_t i = 0; i < kMaxPathBytes; ++i) {
            const char c = static_cast<char>(Memory::Read8(path + static_cast<uint32_t>(i)));
            if (c == '\0') break;
            text.push_back(c);
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "movie path", e);
        return {};
    }
    constexpr std::string_view kPrefix = "/movie/", kSuffix = ".thp";
    if (text.size() <= kPrefix.size() + kSuffix.size() || text.compare(0, kPrefix.size(), kPrefix) != 0 ||
        text.compare(text.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        Log("unexpected movie path \"" + text + "\"");
        return {};
    }
    std::string name = text.substr(kPrefix.size(), text.size() - kPrefix.size() - kSuffix.size());
    for (const char c : name) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                           c == '-';
        if (!plain) {
            Log("unexpected movie path \"" + text + "\"");
            return {};
        }
    }
    return name;
}

std::string Utf8(const std::filesystem::path& path) {
    return RuntimeConfigFile::PathToUtf8(path);
}

// One clip as mono S16 at the output rate and mix level (the player's description volume in percent of the approved
// level), or empty with the reason logged.
std::vector<int16_t> LoadClip(const std::filesystem::path& path, int volume) {
    SDL_AudioSpec spec{};
    Uint8* data = nullptr;
    Uint32 length = 0;
    if (!SDL_LoadWAV(Utf8(path).c_str(), &spec, &data, &length)) {
        Log("clip unreadable: " + Utf8(path) + ": " + SDL_GetError());
        return {};
    }
    const SDL_AudioSpec output{SDL_AUDIO_S16, 1, static_cast<int>(kOutputRate)};
    Uint8* converted = nullptr;
    int convertedLength = 0;
    const bool ok = SDL_ConvertAudioSamples(&spec, data, static_cast<int>(length), &output, &converted,
                                            &convertedLength);
    SDL_free(data);
    if (!ok) {
        Log("clip not convertible: " + Utf8(path) + ": " + SDL_GetError());
        return {};
    }
    std::vector<int16_t> samples(static_cast<size_t>(convertedLength) / sizeof(int16_t));
    std::memcpy(samples.data(), converted, samples.size() * sizeof(int16_t));
    SDL_free(converted);
    if (samples.empty()) Log("clip empty: " + Utf8(path));
    AudioDescriptionMix::Levels levels;
    levels.descriptionGain *= static_cast<float>(volume) / 100.0f;
    return AudioDescriptionMix::PrepareClip(samples, kOutputRate, levels);
}

// cues.toml as a document; a UTF-8 byte order mark (some editors add one) is skipped, as for Config.toml.
toml::value ParseCueFile(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open");
    if (input.peek() == 0xEF) {
        char bom[3] = {};
        input.read(bom, 3);
        if (static_cast<unsigned char>(bom[1]) != 0xBB || static_cast<unsigned char>(bom[2]) != 0xBF) {
            input.clear();
            input.seekg(0);
        }
    }
    return toml::parse(input, Utf8(file));
}

// The movie's description from the pack, or null with the reason logged. Any unusable cue disables the whole
// description: a partial one would leave gaps the listener cannot tell from silence by design. Cues may not overlap
// (a clip must end by the next cue, within kOverlapTolerance), since the next one would wait behind it.
std::shared_ptr<const AudioDescriptionMix::Cues> LoadCues(const std::string& movie, int volume) {
    constexpr double kOverlapTolerance = 0.005;
    const auto directory = RuntimeConfigFile::ExecutableDirectory();
    if (!directory) {
        Log("off: the executable's folder is unknown");
        return nullptr;
    }
    const std::filesystem::path folder = *directory / "accessibility" / "audio-description" / kLanguage / movie;
    const std::filesystem::path file = folder / "cues.toml";
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        Log("none installed for " + movie);
        return nullptr;
    }
    const std::string name = Utf8(file);
    auto cues = std::make_shared<AudioDescriptionMix::Cues>();
    try {
        const toml::value document = ParseCueFile(file);
        for (const toml::value& entry : toml::find<toml::array>(document, "cue")) {
            const toml::value& startValue = toml::find(entry, "start");
            const double start = startValue.is_integer() ? static_cast<double>(startValue.as_integer())
                                                         : toml::get<double>(startValue);
            const std::string clip = toml::find<std::string>(entry, "clip");
            if (clip.empty() || clip.find_first_of("/\\") != std::string::npos || clip == "." || clip == "..") {
                Log(name + ": clip \"" + clip + "\" must be a file name in the same folder");
                return nullptr;
            }
            if (start < 0.0) {
                Log(name + ": cue at " + std::to_string(start) + " s is before the movie starts");
                return nullptr;
            }
            if (!cues->empty()) {
                const AudioDescriptionMix::Cue& previous = cues->back();
                const double end = previous.start + static_cast<double>(previous.samples.size()) / kOutputRate;
                if (start <= previous.start || end > start + kOverlapTolerance) {
                    Log(name + ": cue at " + std::to_string(start) + " s overlaps the one before or is out of order");
                    return nullptr;
                }
            }
            AudioDescriptionMix::Cue cue;
            cue.start = start;
            cue.samples = LoadClip(folder / clip, volume);
            if (cue.samples.empty()) return nullptr;
            cues->push_back(std::move(cue));
        }
    } catch (const std::exception& e) {
        Log(name + ": " + e.what());
        return nullptr;
    }
    if (cues->empty()) {
        Log(name + ": no cues");
        return nullptr;
    }
    Log(movie + ": " + std::to_string(cues->size()) + " cues loaded");
    return cues;
}

void Stop() {
    g_mixer.Stop();
    g_load.reset();
    g_movie.clear();
    g_started = false;
    g_mixing = false;
    g_playing = false;
}

void Open(const std::string& movie) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Stop();
    if (movie.empty()) return;
    if (!RuntimeConfigFile::AudioDescriptionEnabled(true)) return;
    if (AccessibilityText::SilencedByMods()) {
        AccessibilityText::LogOnce("audio description off: a mod has changed the game's code");
        return;
    }
    auto load = std::make_shared<Load>();
    try {
        const int volume = std::clamp(RuntimeConfigFile::AudioDescriptionVolume(100), 0, 200);
        std::thread([load, movie, volume] {
            std::shared_ptr<const AudioDescriptionMix::Cues> cues;
            try {
                cues = LoadCues(movie, volume);
            } catch (const std::exception& e) {
                Log(movie + ": " + e.what());
            } catch (...) {
                Log(movie + ": loading failed");
            }
            std::lock_guard<std::mutex> loadLock(load->mutex);
            load->cues = std::move(cues);
            load->done = true;
        }).detach();
    } catch (const std::system_error& e) {
        Log(std::string("off: cannot start the loader: ") + e.what());
        return;
    }
    g_load = load;
    g_movie = movie;
    g_engaged = true;
}

// The player has just entered the playing state, from prepared (a start) or paused (a resume).
void Play(uint8_t previousState) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_load) return;
    if (previousState != kStatePrepared) {
        g_playing = g_started;
        return;
    }
    float frameRate = 0.0f;
    uint8_t hasAudio = 0;
    try {
        const uint32_t bits = Memory::Read32(kFrameRate);
        std::memcpy(&frameRate, &bits, sizeof(frameRate));
        g_frameCount = Memory::Read32(kFrameCount);
        g_loops = (Memory::Read8(kFlags) & 1) != 0;
        hasAudio = Memory::Read8(kHasAudio);
        g_baseFrames = Memory::Read32(kAudioFrames);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "movie player", e);
        Stop();
        return;
    }
    if (!(frameRate > 1.0f && frameRate < 121.0f) || g_frameCount == 0 || g_frameCount > 1000000) {
        Log(g_movie + ": implausible movie header (" + std::to_string(frameRate) + " fps, " +
            std::to_string(g_frameCount) + " frames)");
        Stop();
        return;
    }
    if (hasAudio == 0) {
        // The clock is the movie's own audio; a silent movie has none.
        Log(g_movie + ": the movie has no sound track to follow");
        Stop();
        return;
    }
    g_frameRate = frameRate;
    g_lastFrames = g_baseFrames;
    g_samplesSinceFrame = 0;
    g_started = true;
    g_playing = true;
}

void Pause() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_playing = false;
}

void Close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Stop();
}

// The movie's position in seconds at the start of the current output block, or false when the clock is unreadable. A
// looping movie wraps; any other stops at its end, where the count equals the frame count (state 3, 0x8007CE88).
bool MovieSeconds(double& seconds) {
    uint32_t frames = 0;
    try {
        frames = Memory::Read32(kAudioFrames);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "movie clock", e);
        return false;
    }
    if (frames != g_lastFrames) {
        g_lastFrames = frames;
        g_samplesSinceFrame = 0;
    }
    const uint32_t played = frames - g_baseFrames;
    const uint32_t frame = g_loops ? played % g_frameCount : std::min(played, g_frameCount);
    const double within = std::min(static_cast<double>(g_samplesSinceFrame) / kOutputRate, 1.0 / g_frameRate);
    seconds = frame / g_frameRate + within;
    return true;
}

void ProcessOutput(int16_t* samples, size_t frames, uint32_t sampleRate) {
    if (!g_engaged.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (sampleRate != kOutputRate) {
        AccessibilityText::LogOnce("audio description off: output rate " + std::to_string(sampleRate) + " Hz");
        return;
    }
    if (g_load && !g_mixing) {
        std::shared_ptr<const AudioDescriptionMix::Cues> cues;
        bool done = false;
        {
            std::lock_guard<std::mutex> loadLock(g_load->mutex);
            done = g_load->done;
            cues = g_load->cues;
        }
        if (done && !cues) {
            Stop();  // no description for this movie: nothing more to do until the next one opens
        } else if (done && g_started) {
            g_mixer.Start(std::move(cues));
            g_mixing = true;
        }
    }
    const bool playing = g_mixing && g_playing;
    double seconds = 0.0;
    if (playing && !MovieSeconds(seconds)) {
        Stop();
        return;
    }
    g_mixer.Process(samples, frames, sampleRate, seconds, playing);
    if (playing) g_samplesSinceFrame += frames;
    if (!g_load && g_mixer.Idle()) g_engaged = false;
}

} // namespace AudioDescriptionPlayback

void AudioDescription::ProcessOutput(int16_t* samples, size_t frames, uint32_t sampleRate) {
    AudioDescriptionPlayback::ProcessOutput(samples, frames, sampleRate);
}

extern "C" void MovieOpen_8007ec08(CpuContext* ctx) {
    const std::string movie = AudioDescriptionPlayback::MovieName(ctx->gpr[4]);
    func_8007EC08(ctx);
    AudioDescriptionPlayback::Open(movie);
}

extern "C" void MoviePlay_8007c944(CpuContext* ctx) {
    uint8_t previousState = 0;
    try {
        previousState = Memory::Read8(AudioDescriptionPlayback::kState);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "movie player state", e);
    }
    func_8007C944(ctx);
    if (ctx->gpr[3] == 1) AudioDescriptionPlayback::Play(previousState);
}

extern "C" void MoviePause_8007cb08(CpuContext* ctx) {
    func_8007CB08(ctx);
    if (ctx->gpr[3] == 1) AudioDescriptionPlayback::Pause();
}

extern "C" void MovieStop_8007c9dc(CpuContext* ctx) {
    func_8007C9DC(ctx);
    AudioDescriptionPlayback::Close();
}

REGISTER_NATIVE_FUNCTION_AS(0x8007EC08, MovieOpen_8007ec08, "MovieOpen_8007ec08");
REGISTER_NATIVE_FUNCTION_AS(0x8007C944, MoviePlay_8007c944, "MoviePlay_8007c944");
REGISTER_NATIVE_FUNCTION_AS(0x8007CB08, MoviePause_8007cb08, "MoviePause_8007cb08");
REGISTER_NATIVE_FUNCTION_AS(0x8007C9DC, MovieStop_8007c9dc, "MovieStop_8007c9dc");
