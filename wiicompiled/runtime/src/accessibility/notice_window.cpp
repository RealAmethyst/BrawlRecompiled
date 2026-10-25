#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

// Screen-reader speech for MuNoticeWnd, the game's shared message window (main.dol): save-data prompts at boot, Vault,
// stage and character select, results, Wi-Fi and Subspace notices. Every text it shows, body and button labels alike,
// passes through one print function; the window's opening, its input frames and its destruction are the events.
// main.dol is resident, so these hooks fire whatever loads the modules, and stay silent once mods patch the game.

extern "C" void func_800BF850(CpuContext* ctx);  // MuNoticeWnd: print a text into a slot
extern "C" void func_800BFB14(CpuContext* ctx);  // MuNoticeWnd: state 0 (open) enter
extern "C" void func_800BFCF4(CpuContext* ctx);  // MuNoticeWnd: state 1 (input) process
extern "C" void func_800BF204(CpuContext* ctx);  // MuNoticeWnd: constructor
extern "C" void func_800BF2CC(CpuContext* ctx);  // MuNoticeWnd: destructor

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace NoticeWindowSpeech {

// MuNoticeWnd layout (constructor 0x800BF204; verified).
constexpr uint32_t kWindowTypeOffset = 0xFC;       // u32: 0 no buttons, 1 two buttons, 2 one button, 3 busy
constexpr uint32_t kWindowSelectionOffset = 0x100; // u8: 1 = the first label (slot 2), 0 = the second (slot 3)
constexpr uint32_t kSlots = 6;
// Which slot holds what, by window type (table 0x805A0BC8).
constexpr uint32_t kBodySlotNoButtons = 0;
constexpr uint32_t kBodySlotButtons = 1;
constexpr uint32_t kFirstLabelSlot = 2;
constexpr uint32_t kSecondLabelSlot = 3;
constexpr uint32_t kSingleLabelSlot = 4;
enum WindowType : uint32_t { kNoButtons = 0, kTwoButtons = 1, kOneButton = 2, kBusy = 3 };

// The speakable text last printed into each slot of each live window.
std::unordered_map<uint32_t, std::array<std::string, kSlots>> g_windowTexts;

std::string SlotText(uint32_t window, uint32_t slot) {
    const auto texts = g_windowTexts.find(window);
    return texts == g_windowTexts.end() ? std::string() : texts->second[slot];
}

// "YES, 1 of 2": a two-button window's selected answer, in on-screen order (first label first).
std::string Choice(uint32_t window, uint8_t selection) {
    const bool first = selection == 1;
    const std::string label = SlotText(window, first ? kFirstLabelSlot : kSecondLabelSlot);
    if (label.empty()) return {};
    return AccessibilityText::Join({label, AccessibilityText::Position(first ? 1 : 2, 2)});
}

} // namespace NoticeWindowSpeech

extern "C" void NoticeWindowPrint_800bf850(CpuContext* ctx) {
    const uint32_t window = ctx->gpr[3], slot = ctx->gpr[4], text = ctx->gpr[5], length = ctx->gpr[6];
    func_800BF850(ctx);
    if (slot < NoticeWindowSpeech::kSlots)
        NoticeWindowSpeech::g_windowTexts[window][slot] = AccessibilityText::GuestText(text, length);
}

// The window has printed its body and set its first selection: the body interrupts, the selected button follows.
extern "C" void NoticeWindowOpen_800bfb14(CpuContext* ctx) {
    using namespace NoticeWindowSpeech;
    const uint32_t window = ctx->gpr[3];
    func_800BFB14(ctx);
    if (AccessibilityText::SilencedByMods()) return;
    uint32_t type = 0;
    uint8_t selection = 0;
    try {
        type = Memory::Read32(window + kWindowTypeOffset);
        selection = Memory::Read8(window + kWindowSelectionOffset);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "notice window", e);
        return;
    }
    if (type > kBusy) return;
    const bool buttons = type == kTwoButtons || type == kOneButton;
    const std::string body = SlotText(window, buttons ? kBodySlotButtons : kBodySlotNoButtons);
    std::string choice;
    if (type == kTwoButtons) choice = Choice(window, selection);
    if (type == kOneButton) choice = SlotText(window, kSingleLabelSlot);  // a lone button has no index
    if (!body.empty()) Speech::Say(body, true);
    if (!choice.empty()) Speech::Say(choice, body.empty());
}

// The only frames where a two-button window's selection can change.
extern "C" void NoticeWindowInput_800bfcf4(CpuContext* ctx) {
    using namespace NoticeWindowSpeech;
    const uint32_t window = ctx->gpr[3];
    uint8_t before = 0;
    uint32_t type = 0;
    try {
        before = Memory::Read8(window + kWindowSelectionOffset);
        type = Memory::Read32(window + kWindowTypeOffset);
    } catch (const Memory::AccessViolation&) {
        func_800BFCF4(ctx);
        return;
    }
    func_800BFCF4(ctx);
    if (type != kTwoButtons || AccessibilityText::SilencedByMods()) return;
    uint8_t after = 0;
    try {
        after = Memory::Read8(window + kWindowSelectionOffset);
    } catch (const Memory::AccessViolation&) {
        return;
    }
    if (after == before) return;
    const std::string choice = Choice(window, after);
    if (!choice.empty()) Speech::Say(choice, true);
}

// A new window at an address an earlier one used starts with no texts of its own.
extern "C" void NoticeWindowCreate_800bf204(CpuContext* ctx) {
    NoticeWindowSpeech::g_windowTexts.erase(ctx->gpr[3]);
    func_800BF204(ctx);
}

extern "C" void NoticeWindowDestroy_800bf2cc(CpuContext* ctx) {
    NoticeWindowSpeech::g_windowTexts.erase(ctx->gpr[3]);
    func_800BF2CC(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x800BF850, NoticeWindowPrint_800bf850, "NoticeWindowPrint_800bf850");
REGISTER_NATIVE_FUNCTION_AS(0x800BFB14, NoticeWindowOpen_800bfb14, "NoticeWindowOpen_800bfb14");
REGISTER_NATIVE_FUNCTION_AS(0x800BFCF4, NoticeWindowInput_800bfcf4, "NoticeWindowInput_800bfcf4");
REGISTER_NATIVE_FUNCTION_AS(0x800BF204, NoticeWindowCreate_800bf204, "NoticeWindowCreate_800bf204");
REGISTER_NATIVE_FUNCTION_AS(0x800BF2CC, NoticeWindowDestroy_800bf2cc, "NoticeWindowDestroy_800bf2cc");
