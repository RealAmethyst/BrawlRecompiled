#pragma once

#include <cstddef>
#include <cstdint>

// Audio description for the game's movies (src/accessibility/audio_description.cpp): pre-recorded description clips
// from the separately installed description pack, played in step with the movie and mixed into the final output.
namespace AudioDescription {

// Mixes description into `frames` interleaved left, right S16 frames of final output at `sampleRate`, in place, and
// lowers the game's sound under it. The audio backend calls this for every output block on the main thread, including
// blocks it then drops, so the description keeps pace with the game's audio.
void ProcessOutput(int16_t* samples, size_t frames, uint32_t sampleRate);

} // namespace AudioDescription
