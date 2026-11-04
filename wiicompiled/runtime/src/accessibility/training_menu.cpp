#include "training_menu_speech.h"

#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Screen-reader speech for the Training pause menu (main.dol, found by a play-session trace on 2026-11-04).
//
// One MuMsg draws the whole menu, by slot: 0 speed, 1 item, 2 number of CPUs, 3 CPU behaviour, 4 CPU damage,
// 5 camera, 6 information display, 7 the highlighted row's description, 8-12 the information panel on the right.
// The row labels are art; their wording is in strings_en.toml [training_menu]. The description in slot 7 is its own
// message table's entry 17 + row (17 Speed ... 24 Exit), which tells the highlighted row.
//
// Each hook wraps one of the menu's functions. While one runs, the MuMsg prints it makes are collected (the MuMsg
// hooks in main_menu.cpp and data_screens.cpp forward them here); when the outermost one returns, the event is spoken.

extern "C" void func_80104F28(CpuContext* ctx);  // set up the menu: prints every row's value and the first description
extern "C" void func_801055FC(CpuContext* ctx);  // open (plays sound 5)
extern "C" void func_801056E0(CpuContext* ctx);  // move the cursor (plays sound 0, prints the new row's description)
extern "C" void func_80105894(CpuContext* ctx);  // change: speed
extern "C" void func_80105974(CpuContext* ctx);  // change: item
extern "C" void func_80105A90(CpuContext* ctx);  // change: number of CPUs
extern "C" void func_80105B3C(CpuContext* ctx);  // change: CPU behaviour
extern "C" void func_80105C1C(CpuContext* ctx);  // change: CPU damage
extern "C" void func_80105CC4(CpuContext* ctx);  // change: camera
extern "C" void func_80105D8C(CpuContext* ctx);  // change: information display (also redraws the panel)

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace TrainingMenuSpeech {

constexpr uint32_t kRows = 8;            // Speed, Item, CPUs, Behaviour, Damage, Camera, Information, Exit
constexpr uint32_t kValueSlots = 7;      // slots 0-6: the value shown on rows 0-6 (Exit has none)
constexpr uint32_t kDescriptionSlot = 7;
constexpr uint32_t kFirstDescriptionEntry = 17;
constexpr uint32_t kMuMsgMessageTableOffset = 0x4;

enum class Event { Setup, Open, Move, Change };

struct State {
    int depth = 0;          // nesting of the hooked functions (one may call another)
    uint32_t muMsg = 0;     // the menu's MuMsg, learned from its description prints
    uint32_t row = 0;       // the highlighted row
    std::array<std::string, kValueSlots> values{};
    std::string description;
    int changedSlot = -1;   // the lowest value slot printed during the current call
    // Value prints of the current call, kept until it returns: setup prints the values before the description that
    // identifies the menu's MuMsg.
    struct Print {
        uint32_t muMsg;
        uint32_t slot;
        std::string text;
    };
    std::vector<Print> pending;
};

State& Get() {
    static State state;
    return state;
}

bool Capturing() { return Get().depth > 0; }

void Store(uint32_t muMsg, uint32_t slot, std::string text) {
    if (muMsg != 0 && slot < kValueSlots) Get().pending.push_back({muMsg, slot, std::move(text)});
}

// The call has returned: its prints on the menu's MuMsg become the shown values.
void ApplyPending() {
    State& s = Get();
    for (State::Print& print : s.pending) {
        if (print.muMsg != s.muMsg) continue;
        s.values[print.slot] = std::move(print.text);
        if (s.changedSlot < 0 || static_cast<int>(print.slot) < s.changedSlot)
            s.changedSlot = static_cast<int>(print.slot);
    }
    s.pending.clear();
}

void OnPrintIndex(uint32_t muMsg, uint32_t slot, uint32_t entry, uint32_t bin) {
    if (!Capturing()) return;
    State& s = Get();
    uint32_t table = bin;
    if (table == 0 && !Memory::TryRead32(muMsg + kMuMsgMessageTableOffset, table)) return;
    if (table == 0) return;
    std::string text;
    try {
        text = AccessibilityText::MessageEntry(table, entry);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "training menu text", e);
        return;
    }
    if (slot == kDescriptionSlot && entry >= kFirstDescriptionEntry && entry < kFirstDescriptionEntry + kRows) {
        s.muMsg = muMsg;
        s.row = entry - kFirstDescriptionEntry;
        s.description = std::move(text);
        return;
    }
    Store(muMsg, slot, std::move(text));
}

void OnPrintf(uint32_t muMsg, uint32_t slot, std::string text) {
    if (Capturing()) Store(muMsg, slot, std::move(text));
}

std::string RowLabel(uint32_t row) { return AccessibilityText::StringAt("training_menu.rows", row); }

// "CPU behavior, Walk": the row's label and, except on Exit, its value.
std::string Row(uint32_t row) {
    const State& s = Get();
    const std::string label = RowLabel(row);
    if (row >= kValueSlots) return label;
    return AccessibilityText::Join({label, s.values[row]});
}

void Speak(Event event) {
    State& s = Get();
    if (AccessibilityText::SilencedByMods() || s.muMsg == 0) return;
    switch (event) {
    case Event::Setup:
        return;  // the menu is built hidden when Training starts; it speaks when opened
    case Event::Open:
        Speech::Say(AccessibilityText::String("training_menu.title"), true);
        Speech::Say(Row(s.row), false);
        return;
    case Event::Move:
        Speech::Say(AccessibilityText::Join({Row(s.row), AccessibilityText::Position(s.row + 1, kRows)}), true);
        if (!s.description.empty()) Speech::Say(s.description, false);
        return;
    case Event::Change:
        if (s.changedSlot >= 0) Speech::Say(s.values[static_cast<size_t>(s.changedSlot)], true);
        return;
    }
}

// Runs one of the menu's functions with its prints collected, then speaks the event if it is the outermost.
template <void (*Original)(CpuContext*)>
void Run(CpuContext* ctx, Event event) {
    State& s = Get();
    if (s.depth == 0) {
        s.changedSlot = -1;
        s.pending.clear();
    }
    ++s.depth;
    try {
        Original(ctx);
    } catch (...) {
        --s.depth;
        throw;
    }
    if (--s.depth == 0) {
        ApplyPending();
        Speak(event);
    }
}

} // namespace TrainingMenuSpeech

using TrainingMenuSpeech::Event;
using TrainingMenuSpeech::Run;

extern "C" void TrainingMenuSetup_80104f28(CpuContext* ctx) { Run<func_80104F28>(ctx, Event::Setup); }
extern "C" void TrainingMenuOpen_801055fc(CpuContext* ctx) { Run<func_801055FC>(ctx, Event::Open); }
extern "C" void TrainingMenuMove_801056e0(CpuContext* ctx) { Run<func_801056E0>(ctx, Event::Move); }
extern "C" void TrainingMenuSpeed_80105894(CpuContext* ctx) { Run<func_80105894>(ctx, Event::Change); }
extern "C" void TrainingMenuItem_80105974(CpuContext* ctx) { Run<func_80105974>(ctx, Event::Change); }
extern "C" void TrainingMenuCpus_80105a90(CpuContext* ctx) { Run<func_80105A90>(ctx, Event::Change); }
extern "C" void TrainingMenuBehavior_80105b3c(CpuContext* ctx) { Run<func_80105B3C>(ctx, Event::Change); }
extern "C" void TrainingMenuDamage_80105c1c(CpuContext* ctx) { Run<func_80105C1C>(ctx, Event::Change); }
extern "C" void TrainingMenuCamera_80105cc4(CpuContext* ctx) { Run<func_80105CC4>(ctx, Event::Change); }
extern "C" void TrainingMenuInformation_80105d8c(CpuContext* ctx) { Run<func_80105D8C>(ctx, Event::Change); }

REGISTER_NATIVE_FUNCTION_AS(0x80104F28, TrainingMenuSetup_80104f28, "TrainingMenuSetup_80104f28");
REGISTER_NATIVE_FUNCTION_AS(0x801055FC, TrainingMenuOpen_801055fc, "TrainingMenuOpen_801055fc");
REGISTER_NATIVE_FUNCTION_AS(0x801056E0, TrainingMenuMove_801056e0, "TrainingMenuMove_801056e0");
REGISTER_NATIVE_FUNCTION_AS(0x80105894, TrainingMenuSpeed_80105894, "TrainingMenuSpeed_80105894");
REGISTER_NATIVE_FUNCTION_AS(0x80105974, TrainingMenuItem_80105974, "TrainingMenuItem_80105974");
REGISTER_NATIVE_FUNCTION_AS(0x80105A90, TrainingMenuCpus_80105a90, "TrainingMenuCpus_80105a90");
REGISTER_NATIVE_FUNCTION_AS(0x80105B3C, TrainingMenuBehavior_80105b3c, "TrainingMenuBehavior_80105b3c");
REGISTER_NATIVE_FUNCTION_AS(0x80105C1C, TrainingMenuDamage_80105c1c, "TrainingMenuDamage_80105c1c");
REGISTER_NATIVE_FUNCTION_AS(0x80105CC4, TrainingMenuCamera_80105cc4, "TrainingMenuCamera_80105cc4");
REGISTER_NATIVE_FUNCTION_AS(0x80105D8C, TrainingMenuInformation_80105d8c, "TrainingMenuInformation_80105d8c");
