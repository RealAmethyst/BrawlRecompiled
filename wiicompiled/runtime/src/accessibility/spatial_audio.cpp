#include "spatial_audio.h"

#if defined(MKW_SPATIAL_AUDIO)

#include "accessibility_text.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "spatial_audio_mix.h"
#include "spatial_audio_steam.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_set>

#include <SDL3/SDL_loadso.h>

// Where each positional voice is, read from the game's NW4R sound system once per AX frame, with no hooks: the voice
// list, each voice's owner sound and its 3D parameters, the listener matrix and the parameter block (PB) the DSP mixes.
// Every hop below was traced in main.dol (USA RSBE01 rev 1) at the code that creates or uses it (accessibility/
// RESEARCH.md, "Audio pipeline and positional sound"), and each hop with a back-pointer is checked before it is used.
// A voice that fails any check stays in the game's own mix, counted by a named reason.

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace SpatialAudioRuntime {

using SpatialAudioMix::Direction;

// nw4r::snd::detail::VoiceManager (GetInstance 0x801D2AD0): active list count +4, sentinel node +8; node {next +0,
// prev +4}; Voice = node - 0x124 (the list walk at 0x801D2FC8).
constexpr uint32_t kVoiceListSentinel = 0x804DD0EC + 8;
constexpr uint32_t kVoiceNode = 0x124;
// Voice: AxVoice table +0xC + channel*0x10 + output*4, channel count +0x9C (1-2), output count +0xA0 (1-4), callback
// +0xA4 and its data +0xA8, active u8 +0xAC (Voice::Initialize 0x801D049C, Acquire 0x801D0B68).
constexpr uint32_t kVoiceAxVoices = 0x0C;
constexpr uint32_t kVoiceChannels = 0x9C;
constexpr uint32_t kVoiceOutputs = 0xA0;
constexpr uint32_t kVoiceCallback = 0xA4;
constexpr uint32_t kVoiceCallbackData = 0xA8;
constexpr uint32_t kVoiceActive = 0xAC;
// Channel: the voice callback is Channel's own with the Channel as data (AllocChannel 0x801BE6CC-E0); owner callback
// +0xD4 and data +0xD8 (Channel init 0x801BDA20), Voice back-pointer +0xDC (0x801BE774).
constexpr uint32_t kChannelVoiceCallback = 0x801BE47C;
constexpr uint32_t kChannelOwnerCallback = 0xD4;
constexpr uint32_t kChannelOwnerData = 0xD8;
constexpr uint32_t kChannelVoice = 0xDC;
// Owners. Wave: callback 0x801D4EE8 with the WsdPlayer, which is sound+0xD8 (StartChannel 0x801D49F4, WaveSound ctor
// 0x801D3920). Sequence: callback 0x801C5744 with the track; track+0xB8 is its SeqPlayer (AllocTrack 0x801C2110),
// which is sound+0xD8 (SeqSound ctor 0x801C42A0).
constexpr uint32_t kWaveOwnerCallback = 0x801D4EE8;
constexpr uint32_t kSeqOwnerCallback = 0x801C5744;
constexpr uint32_t kSoundPlayer = 0xD8;
constexpr uint32_t kTrackPlayer = 0xB8;
constexpr uint32_t kWaveSoundVtable = 0x80466D60;
constexpr uint32_t kSeqSoundVtable = 0x80466A58;
// BasicSound 3D state (SetAmbientInfo 0x801BD5C0): +0x18 the Sound3DManager for a 3D sound, +0x24 its own copy of the
// Sound3DActor parameters (zeroed by the constructor and at shutdown; check both). Parameters: 3D flags +4 (bit 0
// volume, 1 pan, 2 surround pan, 3 priority), position +0xC (x, y, z, f32), refreshed each frame by 0x801C606C.
constexpr uint32_t kSoundAmbientUpdate = 0x18;
constexpr uint32_t kSoundAmbientArg = 0x24;
constexpr uint32_t kSoundId = 0x78;
constexpr uint32_t kParamFlags = 0x04;
constexpr uint32_t kParamPosition = 0x0C;
// sndSystem (pointer at 0x805A01D0): Sound3DManager at +0x26C (actor ctor call 0x80079590); the manager's listener
// pointer at +0xC (Sound3DManager::Update 0x801C6238 reads it there; Brawl stores sndSystem+0x294, 0x800732E8); the
// listener's MTX34 at +0, row-major, translation in column 3 (PSMTXMultVec 0x801ECD70).
constexpr uint32_t kSndSystemPointer = 0x805A01D0;
constexpr uint32_t kSndSystem3DManager = 0x26C;
constexpr uint32_t kManagerListener = 0x0C;
// AxVoice +0 its AXVPB, +0x3C its Voice (0x801BAAA0, 0x801BB8BC); AXVPB +0x14 user context = the AxVoice (0x801FF69C),
// +0x18 index (0x8020211C). __AXSyncPBs copies each into the PB array at *(0x805A0B28) + index*0x140, and the command
// list names those PBs by that same address (getter 0x80201EF0, command 4 at 0x80200158; next = PB + 0x140).
constexpr uint32_t kAxVoiceVpb = 0x00;
constexpr uint32_t kAxVoiceVoice = 0x3C;
constexpr uint32_t kVpbUserContext = 0x14;
constexpr uint32_t kVpbIndex = 0x18;
constexpr uint32_t kPbArrayPointer = 0x805A0B28;
constexpr uint32_t kPbCountPointer = 0x805A0B34;
constexpr uint32_t kPbStride = 0x140;
constexpr uint32_t kMaxPbs = 96;
// The gameplay camera (gfCameraManager camera 0, pointer at 0x8059FFC0): the listener is built from it, and its
// effective vertical field of view in radians is at +0xD8 (gfCamera::calc 0x80018C30).
constexpr uint32_t kCameraPointer = 0x8059FFC0;
constexpr uint32_t kCameraFovY = 0xD8;

// The instructions and data that create each hop above, as on the disc (main.dol, RSBE01 rev 1). A mod or code patch
// that changes any of them turns spatial audio off rather than placing sounds from a layout it no longer has.
constexpr std::pair<uint32_t, uint32_t> kAnchors[] = {
    {0x801D49F4, 0x3CC0801D}, {0x801D4A00, 0x38C64EE8}, {0x801D49FC, 0x7FE7FB78},  // wave owner callback and data
    {0x801D3920, 0x387E00D8}, {0x801D3C28, 0x386300D8}, {0x80466DC8, 0x801D3C28},  // WsdPlayer at sound+0xD8
    {0x8040AF9C, 0x801C5744}, {0x801C5AEC, 0x93610028}, {0x801C2110, 0x93E300B8},  // sequence owner, track player
    {0x801C42A0, 0x387E00D8}, {0x801C4994, 0x386300D8}, {0x80466AC0, 0x801C4994},  // SeqPlayer at sound+0xD8
    {0x801BE6CC, 0x3CE0801C}, {0x801BE6E0, 0x38E7E47C}, {0x801BE6DC, 0x7FC8F378},  // channel's voice callback
    {0x801BDA20, 0x908300D4}, {0x801BDA24, 0x90A300D8}, {0x801BE774, 0x907E00DC},  // owner fields, voice back-pointer
    {0x801D0D40, 0x9019000C}, {0x801D2FE8, 0x83E30008}, {0x801D0D80, 0x981400AC},  // voice table, list, active flag
    {0x801FF69C, 0x93BE0014}, {0x801BAAA0, 0x90830000}, {0x801BB8BC, 0x9312003C},  // AxVoice and AXVPB links
    {0x80201EF0, 0x806DC708}, {0x80201EF4, 0x4E800020}, {0x80200158, 0x38000004},  // PB array in the command list
    {0x80201C7C, 0x806DC708}, {0x80201C80, 0x1C000140}, {0x8020217C, 0x38050140},  // PB index * 0x140
    {0x80079590, 0x38BF026C}, {0x800732E8, 0x381C0294}, {0x800732F8, 0x901C0278},  // 3D manager and listener
};

// The Height setting: how far up the top edge of the screen sounds. True angles keep the camera's geometry, where the
// top of the screen is only half the field of view up (about 15 degrees with the default 30-degree view).
using SpatialAudio::Height;
constexpr float kHeightTopDegrees[] = {0.0f, 30.0f, 45.0f, 60.0f};

enum Reason {
    kInactive,
    kForeignVoiceCallback,
    kChannelMismatch,
    kForeignOwner,
    kBadVtable,
    kNot3D,
    kNo3DEffectOff,
    kBadVoiceCounts,
    kAxVoiceMismatch,
    kBadPbIndex,
    kUnreadable,
    kReasonCount,
};
constexpr const char* kReasonNames[kReasonCount] = {
    "inactive voice",       "voice callback not a channel's", "channel's voice back-pointer differs",
    "owner neither wave nor sequence", "owner sound vtable differs",  "sound not 3D",
    "no 3D effect (setting off)", "voice channel or output count out of range", "AxVoice or AXVPB back-pointer differs",
    "PB index out of range", "unreadable guest memory",
};

struct Placement {
    uint32_t pb = 0;
    uint32_t voice = 0;
    uint64_t owner = 0;
    Direction direction;
    bool mixed = false;
};

struct Frame {
    uint32_t addr = 0;
    uint64_t mix = 0;
    std::array<uint8_t, SpatialAudioMix::kMixFrame * 4> game{};
    std::array<uint8_t, SpatialAudioMix::kMixFrame * 4> heard{};
};

enum class State { Untried, Ready, Failed };

// Settings, read from Config.toml when the feature starts and changed by the Accessibility settings screen.
std::atomic<bool> g_enabled{false};
std::atomic<int> g_height{static_cast<int>(Height::Medium)};
std::atomic<bool> g_no3dEffect{false};
bool g_diagnostics = false;
bool g_settingsLoaded = false;

State g_state = State::Untried;
SpatialAudioSteam::Api g_api;
SDL_SharedObject* g_library = nullptr;
std::unique_ptr<SpatialAudioSteam::Engine> g_engine;  // outlives the renderer, whose effects use its HRTF
std::unique_ptr<SpatialAudioMix::Renderer> g_renderer;

// Written on the guest thread at dispatch (the mix worker is idle), read by that command list's mix.
std::array<Placement, kMaxPbs> g_map;
size_t g_mapCount = 0;
uint64_t g_mixCount = 0;
bool g_rendering = false;  // this frame's placements are rendered (not only the diagnostics)
bool g_renderedLastFrame = false;

std::mutex g_framesMutex;
std::array<Frame, 4> g_frames;
std::vector<uint8_t> g_heardBlock;

// Diagnostics: failures per reason, counts, and the first placement of each sound id.
std::array<uint64_t, kReasonCount> g_reasons{};
uint64_t g_voicesSeen = 0;
uint64_t g_placed = 0;
uint64_t g_mappedNotMixed = 0;
uint64_t g_lostWhileActive = 0;  // a placement dropped while its voice still plays: an audible handover
uint64_t g_blocksReplaced = 0;
uint64_t g_blocksStale = 0;      // a stored frame for the block's address, too old to use
uint64_t g_blocksDiffer = 0;     // a stored frame for the block's address whose game bytes differ from the guest's
uint64_t g_blocksUnmatched = 0;  // no stored frame for the block's address
uint64_t g_framesSinceReport = 0;
std::unordered_set<uint32_t> g_loggedSounds;
constexpr uint64_t kReportFrames = 3334;  // about 10 s of AX frames
constexpr size_t kMaxLoggedSounds = 400;

Height ParseHeight(const std::string& value) {
    if (value == "true") return Height::True;
    if (value == "low") return Height::Low;
    if (value == "high") return Height::High;
    return Height::Medium;
}

void LoadSettings() {
    if (g_settingsLoaded) return;
    g_settingsLoaded = true;
    g_enabled = RuntimeConfigFile::SpatialAudioEnabled(false);
    g_height = static_cast<int>(ParseHeight(RuntimeConfigFile::SpatialAudioHeight("medium")));
    g_no3dEffect = RuntimeConfigFile::SpatialAudioNo3dEffect(false);
    g_diagnostics = RuntimeConfigFile::SpatialAudioDiagnostics(false);
}

bool ReadU32(uint32_t addr, uint32_t& value) { return Memory::TryRead32(addr, value); }

bool ReadF32(uint32_t addr, float& value) {
    uint32_t bits = 0;
    if (!Memory::TryRead32(addr, bits)) return false;
    value = std::bit_cast<float>(bits);
    return std::isfinite(value);
}

bool ReadU8(uint32_t addr, uint8_t& value) {
    uint32_t word = 0;
    if (!Memory::TryRead32(addr & ~3u, word)) return false;
    value = static_cast<uint8_t>(word >> (24 - 8 * (addr & 3u)));
    return true;
}

bool AnchorsMatch() {
    for (const auto& [address, expected] : kAnchors) {
        uint32_t word = 0;
        if (!ReadU32(address, word) || word != expected) {
            std::ostringstream message;
            message.imbue(std::locale::classic());  // no digit grouping from the user's locale
            message << "spatial audio off: the game's code differs from the disc at 0x" << std::hex << address;
            RT_LOG(RT_TAG_ACCESSIBILITY) << message.str() << std::endl;
            return false;
        }
    }
    return true;
}

template <typename Fn>
bool Bind(Fn& fn, const char* symbol) {
    fn = reinterpret_cast<Fn>(SDL_LoadFunction(g_library, symbol));
    return fn != nullptr;
}

bool LoadSteamAudio() {
    const auto directory = RuntimeConfigFile::ExecutableDirectory();
    if (!directory) {
        RT_LOG(RT_TAG_ACCESSIBILITY) << "spatial audio off: the executable's folder is unknown" << std::endl;
        return false;
    }
    const std::string path = RuntimeConfigFile::PathToUtf8(*directory / MKW_STEAM_AUDIO_LIBRARY_NAME);
    g_library = SDL_LoadObject(path.c_str());
    if (g_library == nullptr) {
        RT_LOG(RT_TAG_ACCESSIBILITY) << "spatial audio off: " << SDL_GetError() << std::endl;
        return false;
    }
    const bool bound = Bind(g_api.contextCreate, "iplContextCreate") &&
                       Bind(g_api.contextRelease, "iplContextRelease") && Bind(g_api.hrtfCreate, "iplHRTFCreate") &&
                       Bind(g_api.hrtfRelease, "iplHRTFRelease") &&
                       Bind(g_api.binauralCreate, "iplBinauralEffectCreate") &&
                       Bind(g_api.binauralRelease, "iplBinauralEffectRelease") &&
                       Bind(g_api.binauralReset, "iplBinauralEffectReset") &&
                       Bind(g_api.binauralApply, "iplBinauralEffectApply");
    if (!bound) {
        RT_LOG(RT_TAG_ACCESSIBILITY) << "spatial audio off: " << path << " lacks a Steam Audio function" << std::endl;
        return false;
    }
    g_engine = std::make_unique<SpatialAudioSteam::Engine>(g_api);
    const std::string error = g_engine->Create(kMaxPbs);
    if (!error.empty()) {
        RT_LOG(RT_TAG_ACCESSIBILITY) << "spatial audio off: " << error << std::endl;
        g_engine.reset();
        return false;
    }
    g_renderer = std::make_unique<SpatialAudioMix::Renderer>(g_engine->TakePool());
    RT_LOG(RT_TAG_ACCESSIBILITY) << "spatial audio: Steam Audio " << STEAMAUDIO_VERSION_MAJOR << '.'
                                 << STEAMAUDIO_VERSION_MINOR << '.' << STEAMAUDIO_VERSION_PATCH << " ready, "
                                 << g_renderer->Capacity() << " voices" << std::endl;
    return true;
}

// Logs what the walk depends on once: the PB array (MEM1 or MEM2), the listener, the camera's field of view.
void LogEnvironment(uint32_t sndSystem, uint32_t manager, uint32_t listener) {
    uint32_t pbArray = 0, pbCount = 0, camera = 0, outputMode = 0;
    float fov = 0.0f;
    ReadU32(kPbArrayPointer, pbArray);
    ReadU32(kPbCountPointer, pbCount);
    ReadU32(kCameraPointer, camera);
    if (camera != 0) ReadF32(camera + kCameraFovY, fov);
    ReadU32(0x804C1B8C, outputMode);  // AxManager output mode: 0 stereo, 2 DPL2, 3 mono (SCGetSoundMode 0x8020FD44)
    std::ostringstream message;
    message.imbue(std::locale::classic());  // no digit grouping from the user's locale
    message << std::hex << "spatial audio: sndSystem 0x" << sndSystem << ", 3D manager 0x" << manager
            << ", listener 0x" << listener << " (sndSystem+0x294 expected), PB array 0x" << pbArray << std::dec
            << ", voices " << pbCount << ", output mode " << outputMode << ", camera field of view " << fov * 57.29578f
            << " degrees";
    RT_LOG(RT_TAG_ACCESSIBILITY) << message.str() << std::endl;
}

// The listener-space vector of a world position, then the Height setting applied to its vertical angle.
Direction Place(const std::array<float, 12>& m, const float* position, float topScale) {
    const float x = m[0] * position[0] + m[1] * position[1] + m[2] * position[2] + m[3];
    const float y = m[4] * position[0] + m[5] * position[1] + m[6] * position[2] + m[7];
    const float z = m[8] * position[0] + m[9] * position[1] + m[10] * position[2] + m[11];
    const float horizontal = std::sqrt(x * x + z * z);
    if (horizontal < 1e-3f && std::fabs(y) < 1e-3f) return {};  // at the listener: straight ahead
    constexpr float kHalfPi = 1.5707963f;
    const float elevation = std::clamp(std::atan2(y, horizontal) * topScale, -kHalfPi, kHalfPi);
    const float azimuthX = horizontal > 1e-6f ? x / horizontal : 0.0f;
    const float azimuthZ = horizontal > 1e-6f ? z / horizontal : -1.0f;
    return {azimuthX * std::cos(elevation), std::sin(elevation), azimuthZ * std::cos(elevation)};
}

// How much the vertical angle is scaled so the top edge of the screen sounds at the chosen height. 1 for true angles,
// and also when the camera's field of view cannot be read (counted and logged once).
float TopScale() {
    const auto height = static_cast<Height>(g_height.load());
    if (height == Height::True) return 1.0f;
    uint32_t camera = 0;
    float fov = 0.0f;
    if (!ReadU32(kCameraPointer, camera) || camera == 0 || !ReadF32(camera + kCameraFovY, fov) || fov < 0.05f ||
        fov > 3.0f) {
        AccessibilityText::LogOnce("spatial audio: the camera's field of view is unreadable; height uses true angles");
        return 1.0f;
    }
    return kHeightTopDegrees[static_cast<int>(height)] * 0.017453293f / (fov * 0.5f);
}

void Fail(Reason reason) { ++g_reasons[reason]; }

// Rendering stops: frames stored for the output must not replace blocks mixed without it.
void StopRendering() {
    g_rendering = false;
    g_renderedLastFrame = false;
    std::lock_guard<std::mutex> lock(g_framesMutex);
    for (Frame& frame : g_frames) frame.addr = 0;
}

void LogPlacement(uint32_t sound, bool wave, uint32_t flags, uint32_t channel, uint32_t channels, uint32_t outputs,
                  uint32_t pb, const Direction& d, const float* position) {
    uint32_t id = 0;
    if (!ReadU32(sound + kSoundId, id) || g_loggedSounds.size() >= kMaxLoggedSounds || !g_loggedSounds.insert(id).second) {
        return;
    }
    std::ostringstream message;
    message.imbue(std::locale::classic());  // no digit grouping from the user's locale
    message << "spatial audio: sound " << id << (wave ? " wave" : " sequence") << ", 3D flags 0x" << std::hex << flags
            << ", PB 0x" << pb << std::dec << ", channel " << channel << " of " << channels << ", outputs " << outputs
            << ", position " << position[0] << ' ' << position[1] << ' ' << position[2] << ", direction " << d.x << ' '
            << d.y << ' ' << d.z;
    RT_LOG(RT_TAG_ACCESSIBILITY) << message.str() << std::endl;
}

void Report() {
    std::ostringstream message;
    message.imbue(std::locale::classic());  // no digit grouping from the user's locale
    {
        std::lock_guard<std::mutex> lock(g_framesMutex);
        message << "spatial audio: last ~10 s: voices " << g_voicesSeen << ", PBs placed " << g_placed
                << ", placed but not mixed " << g_mappedNotMixed << ", lost while playing " << g_lostWhileActive
                << "; output blocks replaced " << g_blocksReplaced << ", stale " << g_blocksStale << ", differing "
                << g_blocksDiffer << ", unmatched " << g_blocksUnmatched;
        g_blocksReplaced = g_blocksStale = g_blocksDiffer = g_blocksUnmatched = 0;
    }
    for (int i = 0; i < kReasonCount; ++i) {
        if (g_reasons[i] != 0) message << "; " << kReasonNames[i] << ' ' << g_reasons[i];
    }
    RT_LOG(RT_TAG_ACCESSIBILITY) << message.str() << std::endl;
    g_reasons.fill(0);
    g_voicesSeen = g_placed = g_mappedNotMixed = g_lostWhileActive = 0;
}

// One voice: owner sound, 3D check, direction, then each of its AxVoices to its PB.
void WalkVoice(uint32_t voice, const std::array<float, 12>& listener, uint32_t manager, uint32_t pbArray,
               uint32_t pbCount, float topScale) {
    ++g_voicesSeen;
    uint8_t active = 0;
    uint32_t callback = 0, channel = 0, channelVoice = 0, ownerCallback = 0, ownerData = 0;
    if (!ReadU8(voice + kVoiceActive, active) || !ReadU32(voice + kVoiceCallback, callback)) return Fail(kUnreadable);
    if (active == 0) return Fail(kInactive);
    if (callback != kChannelVoiceCallback) return Fail(kForeignVoiceCallback);
    if (!ReadU32(voice + kVoiceCallbackData, channel) || !ReadU32(channel + kChannelVoice, channelVoice) ||
        !ReadU32(channel + kChannelOwnerCallback, ownerCallback) || !ReadU32(channel + kChannelOwnerData, ownerData)) {
        return Fail(kUnreadable);
    }
    if (channelVoice != voice) return Fail(kChannelMismatch);

    uint32_t sound = 0, vtable = 0;
    const bool wave = ownerCallback == kWaveOwnerCallback;
    if (wave) {
        sound = ownerData - kSoundPlayer;
    } else if (ownerCallback == kSeqOwnerCallback) {
        uint32_t player = 0;
        if (!ReadU32(ownerData + kTrackPlayer, player)) return Fail(kUnreadable);
        sound = player - kSoundPlayer;
    } else {
        return Fail(kForeignOwner);
    }
    if (!ReadU32(sound, vtable)) return Fail(kUnreadable);
    if (vtable != (wave ? kWaveSoundVtable : kSeqSoundVtable)) return Fail(kBadVtable);

    uint32_t ambientUpdate = 0, arg = 0, flags = 0;
    if (!ReadU32(sound + kSoundAmbientUpdate, ambientUpdate) || !ReadU32(sound + kSoundAmbientArg, arg)) {
        return Fail(kUnreadable);
    }
    if (ambientUpdate != manager || arg == 0) return Fail(kNot3D);
    std::array<float, 3> position{};
    if (!ReadU32(arg + kParamFlags, flags) || !ReadF32(arg + kParamPosition, position[0]) ||
        !ReadF32(arg + kParamPosition + 4, position[1]) || !ReadF32(arg + kParamPosition + 8, position[2])) {
        return Fail(kUnreadable);
    }
    if (flags == 0 && !g_no3dEffect) return Fail(kNo3DEffectOff);
    const Direction direction = Place(listener, position.data(), topScale);

    uint32_t channels = 0, outputs = 0;
    if (!ReadU32(voice + kVoiceChannels, channels) || !ReadU32(voice + kVoiceOutputs, outputs)) return Fail(kUnreadable);
    if (channels < 1 || channels > 2 || outputs < 1 || outputs > 4) return Fail(kBadVoiceCounts);
    for (uint32_t ch = 0; ch < channels; ++ch) {
        for (uint32_t out = 0; out < outputs; ++out) {
            uint32_t axVoice = 0, vpb = 0, context = 0, owner = 0, index = 0;
            if (!ReadU32(voice + kVoiceAxVoices + ch * 0x10 + out * 4, axVoice)) return Fail(kUnreadable);
            if (axVoice == 0) continue;
            if (!ReadU32(axVoice + kAxVoiceVpb, vpb) || vpb == 0 || !ReadU32(vpb + kVpbUserContext, context) ||
                !ReadU32(axVoice + kAxVoiceVoice, owner) || !ReadU32(vpb + kVpbIndex, index)) {
                Fail(kUnreadable);
                continue;
            }
            if (context != axVoice || owner != voice) {
                Fail(kAxVoiceMismatch);
                continue;
            }
            if (index >= pbCount || g_mapCount >= g_map.size()) {
                Fail(kBadPbIndex);
                continue;
            }
            const uint32_t pb = pbArray + index * kPbStride;
            g_map[g_mapCount++] = {pb, voice, (static_cast<uint64_t>(sound) << 32) | (voice + ch), direction, false};
            ++g_placed;
            if (g_diagnostics) LogPlacement(sound, wave, flags, ch, channels, outputs, pb, direction, position.data());
        }
    }
}

void BuildMap() {
    g_mapCount = 0;
    uint32_t sndSystem = 0, listenerAddr = 0, pbArray = 0, pbCount = 0;
    if (!ReadU32(kSndSystemPointer, sndSystem) || sndSystem == 0) return;  // the sound system is not up yet
    const uint32_t manager = sndSystem + kSndSystem3DManager;
    if (!ReadU32(manager + kManagerListener, listenerAddr) || listenerAddr == 0 || !ReadU32(kPbArrayPointer, pbArray) ||
        pbArray == 0 || !ReadU32(kPbCountPointer, pbCount)) {
        return Fail(kUnreadable);
    }
    pbCount = std::min(pbCount, kMaxPbs);
    std::array<float, 12> listener{};
    for (uint32_t i = 0; i < listener.size(); ++i) {
        if (!ReadF32(listenerAddr + i * 4, listener[i])) return Fail(kUnreadable);
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        LogEnvironment(sndSystem, manager, listenerAddr);
    }
    const float topScale = TopScale();
    uint32_t node = 0;
    if (!ReadU32(kVoiceListSentinel, node)) return Fail(kUnreadable);
    for (uint32_t guard = 0; node != kVoiceListSentinel && node != 0 && guard < 256; ++guard) {
        WalkVoice(node - kVoiceNode, listener, manager, pbArray, pbCount, topScale);
        if (!ReadU32(node, node)) return Fail(kUnreadable);
    }
}

const Placement* Find(uint32_t pb) {
    for (size_t i = 0; i < g_mapCount; ++i) {
        if (g_map[i].pb == pb) return &g_map[i];
    }
    return nullptr;
}

void ToBigEndian(const int16_t* samples, uint8_t* bytes) {
    for (uint32_t i = 0; i < SpatialAudioMix::kMixFrame * 2; ++i) {
        const auto value = static_cast<uint16_t>(samples[i]);
        bytes[i * 2] = static_cast<uint8_t>(value >> 8);
        bytes[i * 2 + 1] = static_cast<uint8_t>(value);
    }
}

// The mixer's calls (spatial_audio.h), in the order it makes them.
bool PrepareFrame() {
    LoadSettings();
    if (!g_enabled && !g_diagnostics) return false;
    if (g_state == State::Untried) {
        g_state = !AccessibilityText::SilencedByMods() && AnchorsMatch() && LoadSteamAudio() ? State::Ready
                                                                                            : State::Failed;
    }
    if (g_state != State::Ready || AccessibilityText::SilencedByMods()) {
        StopRendering();
        return false;
    }

    // Placements the last mix never reached: the PB address formula or the walk is wrong for them.
    std::array<Placement, kMaxPbs> previous;
    const size_t previousCount = g_mapCount;
    std::copy_n(g_map.begin(), g_mapCount, previous.begin());
    for (size_t i = 0; i < g_mapCount; ++i) {
        if (!g_map[i].mixed) ++g_mappedNotMixed;
    }
    BuildMap();
    for (size_t i = 0; i < previousCount; ++i) {
        uint8_t active = 0;
        if (Find(previous[i].pb) == nullptr && ReadU8(previous[i].voice + kVoiceActive, active) && active != 0) {
            ++g_lostWhileActive;
        }
    }
    if (g_diagnostics && ++g_framesSinceReport >= kReportFrames) {
        g_framesSinceReport = 0;
        Report();
    }
    // With diagnostics alone the mix still reports which placed PBs it reached, but the sound stays the game's.
    g_rendering = g_enabled;
    if (!g_rendering) {
        StopRendering();
    } else if (!g_renderedLastFrame) {
        g_renderer->Reset();  // nothing from before the pause may play again
    }
    g_renderedLastFrame = g_rendering;
    return true;
}

void BeginMix() {
    ++g_mixCount;
    if (g_rendering) g_renderer->BeginFrame();
}

bool Placed(uint32_t pbAddr) { return Find(pbAddr) != nullptr; }

void AddVoice(uint32_t pbAddr, const int32_t* mainL, const int32_t* mainR) {
    for (size_t i = 0; i < g_mapCount; ++i) {
        Placement& placement = g_map[i];
        if (placement.pb != pbAddr) continue;
        placement.mixed = true;
        if (g_rendering) g_renderer->AddVoice(pbAddr, placement.owner, placement.direction, mainL, mainR);
        return;
    }
}

bool EndVoices(int32_t* deltaL, int32_t* deltaR) { return g_rendering && g_renderer->EndFrame(deltaL, deltaR); }

void StoreOutput(uint32_t lrAddr, const int16_t* gameRL, const int16_t* heardRL) {
    std::lock_guard<std::mutex> lock(g_framesMutex);
    Frame* slot = &g_frames[0];
    for (Frame& frame : g_frames) {
        if (frame.addr == lrAddr) {
            slot = &frame;
            break;
        }
        if (frame.mix < slot->mix) slot = &frame;
    }
    slot->addr = lrAddr;
    slot->mix = g_mixCount;
    ToBigEndian(gameRL, slot->game.data());
    ToBigEndian(heardRL, slot->heard.data());
}

const uint8_t* HeardBlock(uint32_t addr, const uint8_t* guest, size_t bytes) {
    constexpr size_t kFrameBytes = SpatialAudioMix::kMixFrame * 4;
    // A frame mixed this long ago is stale: the game has moved on to other buffers.
    constexpr uint64_t kMaxAge = 4;
    if (!g_renderedLastFrame) return nullptr;  // nothing rendered, nothing stored
    std::lock_guard<std::mutex> lock(g_framesMutex);
    if (bytes == 0 || bytes % kFrameBytes != 0) return nullptr;
    bool replaced = false;
    for (size_t offset = 0; offset < bytes; offset += kFrameBytes) {
        Frame* match = nullptr;
        for (Frame& frame : g_frames) {
            if (frame.addr != 0 && frame.addr == addr + offset) match = &frame;
        }
        if (match == nullptr) {
            ++g_blocksUnmatched;
        } else if (g_mixCount - match->mix > kMaxAge) {
            ++g_blocksStale;
            match->addr = 0;
        } else if (std::memcmp(match->game.data(), guest + offset, kFrameBytes) != 0) {
            ++g_blocksDiffer;
            match->addr = 0;
        } else {
            if (!replaced) g_heardBlock.assign(guest, guest + bytes);
            std::memcpy(g_heardBlock.data() + offset, match->heard.data(), kFrameBytes);
            match->addr = 0;  // used once
            replaced = true;
            ++g_blocksReplaced;
        }
    }
    return replaced ? g_heardBlock.data() : nullptr;
}

} // namespace SpatialAudioRuntime

namespace SpatialAudio {

bool Supported() { return true; }
bool Enabled() {
    SpatialAudioRuntime::LoadSettings();
    return SpatialAudioRuntime::g_enabled;
}
void SetEnabled(bool enabled) {
    SpatialAudioRuntime::LoadSettings();
    SpatialAudioRuntime::g_enabled = enabled;
}
Height HeightSetting() {
    SpatialAudioRuntime::LoadSettings();
    return static_cast<Height>(SpatialAudioRuntime::g_height.load());
}
void SetHeight(Height height) {
    SpatialAudioRuntime::LoadSettings();
    SpatialAudioRuntime::g_height = static_cast<int>(height);
}
bool No3dEffect() {
    SpatialAudioRuntime::LoadSettings();
    return SpatialAudioRuntime::g_no3dEffect;
}
void SetNo3dEffect(bool placed) {
    SpatialAudioRuntime::LoadSettings();
    SpatialAudioRuntime::g_no3dEffect = placed;
}

bool PrepareFrame() { return SpatialAudioRuntime::PrepareFrame(); }
void BeginMix() { SpatialAudioRuntime::BeginMix(); }
bool Placed(uint32_t pbAddr) { return SpatialAudioRuntime::Placed(pbAddr); }
void AddVoice(uint32_t pbAddr, const int32_t* mainL, const int32_t* mainR) {
    SpatialAudioRuntime::AddVoice(pbAddr, mainL, mainR);
}
bool EndVoices(int32_t* deltaL, int32_t* deltaR) { return SpatialAudioRuntime::EndVoices(deltaL, deltaR); }
void StoreOutput(uint32_t lrAddr, const int16_t* gameRL, const int16_t* heardRL) {
    SpatialAudioRuntime::StoreOutput(lrAddr, gameRL, heardRL);
}
const uint8_t* HeardBlock(uint32_t addr, const uint8_t* guest, size_t bytes) {
    return SpatialAudioRuntime::HeardBlock(addr, guest, bytes);
}

} // namespace SpatialAudio

#else

namespace SpatialAudio {

bool Supported() { return false; }
bool Enabled() { return false; }
void SetEnabled(bool) {}
Height HeightSetting() { return Height::Medium; }
void SetHeight(Height) {}
bool No3dEffect() { return false; }
void SetNo3dEffect(bool) {}
bool PrepareFrame() { return false; }
void BeginMix() {}
bool Placed(uint32_t) { return false; }
void AddVoice(uint32_t, const int32_t*, const int32_t*) {}
bool EndVoices(int32_t*, int32_t*) { return false; }
void StoreOutput(uint32_t, const int16_t*, const int16_t*) {}
const uint8_t* HeardBlock(uint32_t, const uint8_t*, size_t) { return nullptr; }

} // namespace SpatialAudio

#endif
