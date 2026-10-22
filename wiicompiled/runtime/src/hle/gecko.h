// Gecko/Ocarina codes (.gct), installed the way Project+'s Brawl Mod Loader (USB Loader GX) installs them.
#pragma once

#include <cstdint>

struct CpuContext;

namespace Gecko {

// Before the game's entry point runs: the code list, gc.txt's pokes and the handler hook. No-op without codes.
void Install();
// VISetNextFrameBuffer_HLE stands in for the guest function whose blr the VBI hook replaces.
void RunVbiHook(CpuContext* ctx);

} // namespace Gecko
