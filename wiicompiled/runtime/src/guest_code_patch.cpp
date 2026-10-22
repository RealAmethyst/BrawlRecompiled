// guest_code_patch.cpp - routes translated functions whose guest code a mod changed at run time to the interpreter.
// See include/guest_code_patch.h.

#include "guest_code_patch.h"

#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "memory.h"
#include "ppc_interpreter.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Generated with the translation (guest_code_guard table): every translated function's start, ascending, and its
// "patched" byte.
extern "C" const uint32_t g_mkwGuardedFunctionStarts[];
extern "C" const uint32_t g_mkwGuardedFunctionCount;

namespace {

constexpr uint32_t kPage = 4096;

std::atomic<bool> g_active{false};
std::atomic<bool> g_anyDirty{false};                                            // g_dirty is not empty
std::mutex g_mutex;
std::vector<std::pair<uint32_t, uint32_t>> g_ranges;                              // guarded code, [start, end)
std::unordered_map<uint32_t, std::unique_ptr<std::array<uint8_t, kPage>>> g_pristine;  // by page address
std::unordered_set<uint32_t> g_dirty;                                              // page addresses written since Sync
std::unordered_set<uint32_t> g_reported;                                           // functions logged as patched

// The cached (0x8/0x9) view: the trap also fires for the uncached alias of guarded code.
uint32_t Cached(uint32_t address) { return (address & 0xE0000000u) == 0xC0000000u ? address - 0x40000000u : address; }

bool IsGuardedLocked(uint32_t address) {
    return std::any_of(g_ranges.begin(), g_ranges.end(),
                       [&](const auto& range) { return address >= range.first && address < range.second; });
}

// The translated function holding `address`: the last start at or below it.
int64_t FunctionIndex(uint32_t address) {
    const uint32_t* begin = g_mkwGuardedFunctionStarts;
    const uint32_t* end = begin + g_mkwGuardedFunctionCount;
    const uint32_t* it = std::upper_bound(begin, end, address);
    return it == begin ? -1 : (it - begin) - 1;
}

void SnapshotLocked(uint32_t start, uint32_t end) {
    for (uint32_t page = start & ~(kPage - 1); page < end; page += kPage) {
        auto& slot = g_pristine[page];
        if (!slot) slot = std::make_unique<std::array<uint8_t, kPage>>();
        if (Memory::Contains(page, kPage)) std::memcpy(slot->data(), Memory::GetPointer(page, kPage), kPage);
    }
}

} // namespace

namespace GuestCodePatch {

bool Active() { return g_active.load(std::memory_order_acquire); }

void Protect(uint32_t start, uint32_t end, const char* name) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (std::find(g_ranges.begin(), g_ranges.end(), std::make_pair(start, end)) != g_ranges.end()) return;
        g_ranges.emplace_back(start, end);
        SnapshotLocked(start, end);
        g_active.store(true, std::memory_order_release);
    }
    RecompMod::RegisterExecutableRange(start, end, name);
}

bool NoteWrite(uint32_t address) {
    if (!Active()) return false;
    address = Cached(address);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!IsGuardedLocked(address)) return false;
    g_dirty.insert(address & ~(kPage - 1));
    g_anyDirty.store(true, std::memory_order_release);
    return true;
}

void Sync() {
    if (!g_anyDirty.load(std::memory_order_acquire)) return;
    std::vector<uint32_t> pages;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_dirty.empty()) return;
        pages.assign(g_dirty.begin(), g_dirty.end());
        g_dirty.clear();
        g_anyDirty.store(false, std::memory_order_release);
        for (const uint32_t page : pages) {
            const auto found = g_pristine.find(page);
            if (found == g_pristine.end() || !Memory::Contains(page, kPage)) continue;
            const uint8_t* now = Memory::GetPointer(page, kPage);
            const uint8_t* was = found->second->data();
            for (uint32_t offset = 0; offset < kPage; offset += 4) {
                if (std::memcmp(now + offset, was + offset, 4) == 0) continue;
                const uint32_t address = page + offset;
                if (!IsGuardedLocked(address)) continue;
                const int64_t index = FunctionIndex(address);
                if (index < 0 || g_mkwGuestCodePatched[index]) continue;
                g_mkwGuestCodePatched[index] = 1;
                const uint32_t function = g_mkwGuardedFunctionStarts[index];
                if (g_reported.insert(function).second) {
                    RT_LOGF(RT_TAG_MOD, "[mods] code at 0x%08X changed: function 0x%08X now runs in the interpreter\n",
                            address, function);
                }
            }
        }
    }
    for (const uint32_t page : pages) GuestFlat::ReprotectExecutablePage(page);
}

void ResetPristine(uint32_t start, uint32_t end) {
    std::lock_guard<std::mutex> lock(g_mutex);
    SnapshotLocked(start, end);
    for (int64_t index = FunctionIndex(start); index >= 0 && index < static_cast<int64_t>(g_mkwGuardedFunctionCount); ++index) {
        const uint32_t function = g_mkwGuardedFunctionStarts[index];
        if (function >= end) break;
        if (function >= start) g_mkwGuestCodePatched[index] = 0;
    }
    for (uint32_t page = start & ~(kPage - 1); page < end; page += kPage) g_dirty.erase(page);
}

void RunPatched(uint32_t address, CpuContext* ctx) {
    PpcInterpreter::Run(ctx, address);
    // A patched function is usually a mod hook, and what it called may have patched more code (a Gecko hook runs
    // the code handler): that takes effect now, as the handler's isync makes it on a Wii.
    Sync();
}

} // namespace GuestCodePatch
