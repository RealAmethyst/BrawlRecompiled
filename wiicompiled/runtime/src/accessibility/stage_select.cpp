#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"
#include "stage_select_speech.h"

#include <cstdint>
#include <string>

// Screen-reader speech for the stage select (muSelectStageTask, sora_menu_sel_stage, resident): the VS stage select
// and the first step of Options > My Music. The player moves a free pointer; the game hit-tests it every frame and
// acts only when the region under it changes, which is the event spoken here: the hovered stage's name (game text)
// and its place among the page's stages.
//
// Not covered yet: the Random, Change and Back buttons (art whose captions are not verified) and the custom-stage page.

extern "C" void func_806B626C(CpuContext* ctx);  // muSelectStageTask: hover update (fn_11_58E8)

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace StageSelectSpeech {

// muSelectStageTask layout (verified in sora_menu_sel_stage).
constexpr uint32_t kTaskPageOffset = 0x228;      // u32: 0 Brawl stages, 1 Melee stages, 2 custom stages
constexpr uint32_t kTaskPageListOffset = 0x22C;  // per page (8 bytes apart): pointer to the page's icon ids
constexpr uint32_t kTaskPageCountOffset = 0x230; // per page: u8 icon count
constexpr uint32_t kTaskPageStride = 8;
constexpr uint32_t kTaskRegionOffset = 0x244;    // u32: region under the pointer
// Regions 2 .. count+1 are the page's stage icons, in its list order.
constexpr uint32_t kFirstIconRegion = 2;
constexpr uint32_t kStagePages = 2;
// fn_11_85CC: a page's icon id -> MuStageKind, the u16 at (*(0x806B9298 + 8*page))[id*4].
constexpr uint32_t kStageKindTables = 0x806B9298;
constexpr uint8_t kNoStageIcon = 255;  // My Music's extra entry on page 1 (not a stage; silent, so not counted)
constexpr uint16_t kMaxStageKind = 64; // MuStageKind values stay below this; a larger one is not a stage
// muMenu::exchangeMuStageKindToMsgID 0x800AF6D8 (a leaf): byte 3 of the word at 0x80407AAC + 4*kind.
constexpr uint32_t kStageMessageIds = 0x80407AAC;
constexpr const char* kStageNameTable = "menu/msg/stage_name_en.msbin";

bool g_queueNextHover = false;

// "Battlefield, 1 of 31" for a stage icon region; empty for anything else. The stage-select preview draws the name as
// art (MenSelmapFrontStname); the game's own stage_name table holds the same names.
std::string HoveredStage(uint32_t task, uint32_t region) {
    try {
        const uint32_t page = Memory::Read32(task + kTaskPageOffset);
        if (page >= kStagePages || region < kFirstIconRegion) return {};
        const uint32_t icon = region - kFirstIconRegion;
        const uint8_t count = Memory::Read8(task + kTaskPageCountOffset + page * kTaskPageStride);
        if (icon >= count) return {};
        const uint32_t list = Memory::Read32(task + kTaskPageListOffset + page * kTaskPageStride);
        const uint8_t id = Memory::Read8(list + icon);
        if (id == kNoStageIcon) return {};
        size_t place = 0, stages = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (Memory::Read8(list + i) == kNoStageIcon) continue;
            ++stages;
            if (i <= icon) ++place;
        }
        const uint16_t kind = Memory::Read16(Memory::Read32(kStageKindTables + page * 8) + id * 4u);
        if (kind >= kMaxStageKind) return {};
        const uint8_t message = Memory::Read8(kStageMessageIds + kind * 4u + 3);
        const std::string name = AccessibilityText::DiscMessageEntry(kStageNameTable, message);
        if (name.empty()) return {};
        return AccessibilityText::Join({name, AccessibilityText::Position(place, stages)});
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stage select", e);
        return {};
    }
}

void QueueNextHover() { g_queueNextHover = true; }

void SpeakHovered(uint32_t task, bool interrupt) {
    uint32_t region = 0;
    if (AccessibilityText::SilencedByMods() || !Memory::TryRead32(task + kTaskRegionOffset, region)) return;
    const std::string stage = HoveredStage(task, region);
    if (stage.empty()) return;
    Speech::Say(stage, interrupt && !g_queueNextHover);
    g_queueNextHover = false;
}

} // namespace StageSelectSpeech

extern "C" void StageSelectHover_806b626c(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(task + StageSelectSpeech::kTaskRegionOffset, before);
    func_806B626C(ctx);
    uint32_t region = 0;
    if (!known || !Memory::TryRead32(task + StageSelectSpeech::kTaskRegionOffset, region) || region == before ||
        AccessibilityText::SilencedByMods())
        return;
    StageSelectSpeech::SpeakHovered(task, true);
}

REGISTER_NATIVE_FUNCTION_AS(0x806B626C, StageSelectHover_806b626c, "StageSelectHover_806b626c");
