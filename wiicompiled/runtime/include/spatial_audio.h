#pragma once

#include <cstddef>
#include <cstdint>

// Spatial audio (src/accessibility/spatial_audio.cpp): the game's positional sounds through an HRTF, from where they are
// relative to the gameplay camera, including height. Off unless the player turns it on (Options, Accessibility).
//
// Guest memory is never written: the game's own mix, and the audio frame it leaves in guest memory, stay exactly as
// they are; the correction reaches only what the player hears. The AX mixer (src/hle/audio/ax_mix.cpp) calls these in
// order for each command list.
namespace SpatialAudio {

// The player's settings (Options, Accessibility, which also saves them to Config.toml). Main thread.
enum class Height { True, Low, Medium, High };
bool Supported();  // built with Steam Audio for this platform
bool Enabled();
void SetEnabled(bool enabled);
Height HeightSetting();
void SetHeight(Height height);
bool No3dEffect();  // sounds the game plays as positional but without any 3D effect are placed too
void SetNo3dEffect(bool placed);

// Guest thread, when the game hands the DSP a command list (the mix worker is idle): reads where each positional voice
// is this frame. Returns false when spatial audio is off or cannot run; the mix then takes no other call below.
bool PrepareFrame();

// The mix of that command list (mix worker or guest thread).
void BeginMix();
// True when the voice on this parameter block (guest address as the command list gives it) is placed this frame.
bool Placed(uint32_t pbAddr);
// What a placed voice added to the main left and right buses this frame (96 samples each).
void AddVoice(uint32_t pbAddr, const int32_t* mainL, const int32_t* mainR);
// After the last voice: the correction to add to the main buses (96 samples each). False when there is none.
bool EndVoices(int32_t* deltaL, int32_t* deltaR);
// The OUTPUT command: the frame written to guest memory at `lrAddr` and the frame the player should hear instead, both
// 96 right-left sample pairs in guest order.
void StoreOutput(uint32_t lrAddr, const int16_t* gameRL, const int16_t* heardRL);

// Guest thread, when the audio interface plays `bytes` of guest memory from `addr` (`guest`, big-endian right-left
// pairs): the bytes to play instead, or null. A replacement is only used when every frame of the block still holds
// exactly what the mix wrote there.
const uint8_t* HeardBlock(uint32_t addr, const uint8_t* guest, size_t bytes);

} // namespace SpatialAudio
