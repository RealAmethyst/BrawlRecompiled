#pragma once

#include <string>

// Screen-reader speech. Game events queue text here; a worker thread hands it to the player's running screen reader
// through Prism (https://github.com/ethindp/prism). Only screen-reader backends are used, never a stand-alone
// synthesiser, so a player without a screen reader running hears nothing extra.
namespace Speech {

// Queues UTF-8 text. interrupt drops anything still queued and cuts off the current speech first. Callable from any
// thread, guest hooks included: it never waits for the screen reader.
void Say(std::string text, bool interrupt);

// Drops queued speech and stops the current speech.
void Silence();

} // namespace Speech
