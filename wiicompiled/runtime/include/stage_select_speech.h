#pragma once

#include <cstdint>

// The stage select's speech (src/accessibility/stage_select.cpp), for screens that host the stage picker.
namespace StageSelectSpeech {

// The next stage hovered is queued after what is being said (a title just announced) instead of interrupting it.
void QueueNextHover();

// Speaks the stage under the pointer of the stage-select task at `task` now, e.g. when its picker regains focus.
void SpeakHovered(uint32_t task, bool interrupt);

} // namespace StageSelectSpeech
