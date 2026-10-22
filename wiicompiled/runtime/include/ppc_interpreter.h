#pragma once

#include <cstdint>

struct CpuContext;

// Guest code without a translation (mods' code, untranslated addresses) runs in src/ppc_interpreter.cpp.
namespace PpcInterpreter {

bool Enabled();
void SetEnabled(bool enabled);
// True when address holds guest memory an interpreter run can start at.
bool CanRun(uint32_t address);
// Runs the guest function at entry until it returns to ctx->lr as it was on entry.
void Run(CpuContext* ctx, uint32_t entry);
uint64_t InstructionCount();

}  // namespace PpcInterpreter
