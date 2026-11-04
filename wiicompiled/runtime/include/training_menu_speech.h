#pragma once

#include <cstdint>
#include <string>

// Screen-reader speech for the Training pause menu (src/accessibility/training_menu.cpp). The menu prints its values
// and the highlighted row's description through MuMsg, whose functions other speech modules already hook; those hooks
// pass each print on here while a hooked Training menu function is running.
namespace TrainingMenuSpeech {

// True while one of the Training menu's own functions is running: only then are MuMsg prints of interest.
bool Capturing();

// MuMsg::printIndex(muMsg, slot, entry, bin) was called (bin 0: the MuMsg's own message table).
void OnPrintIndex(uint32_t muMsg, uint32_t slot, uint32_t entry, uint32_t bin);

// MuMsg::printf(muMsg, slot, ...) was called; `text` is the formatted result.
void OnPrintf(uint32_t muMsg, uint32_t slot, std::string text);

} // namespace TrainingMenuSpeech
