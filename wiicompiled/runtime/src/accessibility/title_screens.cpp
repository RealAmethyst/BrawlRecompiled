#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <cstdint>
#include <string>

// Screen-reader speech from boot to the title screen: the wrist-strap reminder, the ESRB online notice and
// NOW LOADING (scene scStrap at boot), "4 WAYS TO PLAY!" (scStrap in the title's idle loop) and the title screen
// (scTitle). Each hook wraps the function that shows that screen; all live in sora_menu_title, a module loaded on
// demand, so with Gecko codes active they never fire.
//
// Everything here except the ESRB notice is art, spoken from the accessibility strings.

extern "C" void func_940E2398(CpuContext* ctx);  // muStrapTask: show the wrist-strap reminder
extern "C" void func_940E2DEC(CpuContext* ctx);  // muESRBTask: show the online notice
extern "C" void func_940E31BC(CpuContext* ctx);  // muNowLoadingTask: show
extern "C" void func_940E27A4(CpuContext* ctx);  // controller screen ("4 WAYS TO PLAY!"): show
extern "C" void func_940E3E90(CpuContext* ctx);  // muTitleTask: show the title (once per scTitle)

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace TitleScreenSpeech {

// muESRBTask: its init 0x940E2BE8 creates the MuMsg at +0x50 over the notice's message table and prints entry 0
// (English), 1 (language byte 3, French) or 2 (language byte 4, Spanish).
constexpr uint32_t kEsrbMuMsgOffset = 0x50;
constexpr uint32_t kEsrbLanguageOffset = 0x54;
constexpr uint32_t kMuMsgMessageTableOffset = 0x4;
constexpr size_t kStrapLines = 3;

uint32_t EsrbEntry(uint8_t language) {
    switch (language) {
    case 3: return 1;
    case 4: return 2;
    default: return 0;
    }
}

void SayScreen(const std::string& text) {
    if (!text.empty()) Speech::Say(text, true);
}

} // namespace TitleScreenSpeech

extern "C" void BootStrapShow_940e2398(CpuContext* ctx) {
    func_940E2398(ctx);
    for (size_t line = 0; line < TitleScreenSpeech::kStrapLines; ++line) {
        const std::string text = AccessibilityText::StringAt("boot.strap", line);
        if (!text.empty()) Speech::Say(text, line == 0);
    }
}

extern "C" void BootEsrbShow_940e2dec(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3];
    func_940E2DEC(ctx);
    uint32_t muMsg = 0, table = 0;
    uint8_t language = 0;
    try {
        muMsg = Memory::Read32(task + TitleScreenSpeech::kEsrbMuMsgOffset);
        language = Memory::Read8(task + TitleScreenSpeech::kEsrbLanguageOffset);
        if (muMsg != 0) table = Memory::Read32(muMsg + TitleScreenSpeech::kMuMsgMessageTableOffset);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "ESRB notice", e);
        return;
    }
    if (table != 0)
        TitleScreenSpeech::SayScreen(AccessibilityText::MessageEntry(table, TitleScreenSpeech::EsrbEntry(language)));
}

extern "C" void BootNowLoadingShow_940e31bc(CpuContext* ctx) {
    func_940E31BC(ctx);
    TitleScreenSpeech::SayScreen(AccessibilityText::String("boot.now_loading"));
}

extern "C" void BootControllerScreenShow_940e27a4(CpuContext* ctx) {
    func_940E27A4(ctx);
    TitleScreenSpeech::SayScreen(AccessibilityText::String("boot.four_ways_to_play"));
}

// The logo interrupts as the screen's title; the prompt and the copyright lines follow it.
extern "C" void TitleShow_940e3e90(CpuContext* ctx) {
    func_940E3E90(ctx);
    TitleScreenSpeech::SayScreen(AccessibilityText::String("title.logo"));
    for (const char* key : {"title.press_any_button", "title.copyright"}) {
        const std::string text = AccessibilityText::String(key);
        if (!text.empty()) Speech::Say(text, false);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x940E2398, BootStrapShow_940e2398, "BootStrapShow_940e2398");
REGISTER_NATIVE_FUNCTION_AS(0x940E2DEC, BootEsrbShow_940e2dec, "BootEsrbShow_940e2dec");
REGISTER_NATIVE_FUNCTION_AS(0x940E31BC, BootNowLoadingShow_940e31bc, "BootNowLoadingShow_940e31bc");
REGISTER_NATIVE_FUNCTION_AS(0x940E27A4, BootControllerScreenShow_940e27a4, "BootControllerScreenShow_940e27a4");
REGISTER_NATIVE_FUNCTION_AS(0x940E3E90, TitleShow_940e3e90, "TitleShow_940e3e90");
