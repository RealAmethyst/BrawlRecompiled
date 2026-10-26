#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"
#include "stage_select_speech.h"

#include <cstdint>
#include <cstdio>
#include <string>

// Screen-reader speech for the stage select (muSelectStageTask, sora_menu_sel_stage, resident): the VS stage select
// (modes 0 and 1, offline and Wi-Fi) and the first step of Options > My Music (mode 2). The player moves a free
// pointer; the game hit-tests it every frame and acts only when the region under it changes, which is the event spoken
// here: a stage icon (its name and place on the page), a custom stage row, the Change, Random and Back buttons and the
// custom list's scroll controls. Other event hooks: the screen's init (the VS title), the input decision (Change
// switched the page), the page setter and the custom-list refresh (the Change caption), and the state that reveals a
// random stage.
//
// My Music has no title of its own here (the main menu speaks it, then queues the first stage via QueueNextHover).

extern "C" void func_806B0EB0(CpuContext* ctx);  // muSelectStageTask: init (fn_11_52C), once per opening, modes 0-2
extern "C" void func_806B626C(CpuContext* ctx);  // muSelectStageTask: hover update (fn_11_58E8)
extern "C" void func_806B5630(CpuContext* ctx);  // muSelectStageTask: input decision (fn_11_4CAC)
extern "C" void func_806B1F58(CpuContext* ctx);  // muSelectStageTask: show a page (fn_11_15D4)
extern "C" void func_806B36DC(CpuContext* ctx);  // muSelectStageTask: refresh the custom list (fn_11_2D58)
extern "C" void func_806B4D70(CpuContext* ctx);  // muSelectStageTask: state 4, reveal the random stage (fn_11_43EC)

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace StageSelectSpeech {

// muSelectStageTask layout (verified in sora_menu_sel_stage).
constexpr uint32_t kTaskModeOffset = 0x40;        // u32: 0 VS, 1 VS Wi-Fi, 2 My Music, 3-6 no visible screen
constexpr uint32_t kTaskBackObjectOffset = 0x5C;  // MenSelmapBack (title, BACK / LEAVE); loaded only in VS
constexpr uint32_t kTaskStateOffset = 0x224;      // u32: 0 input, 2 chosen, 4 revealing the random stage, ...
constexpr uint32_t kTaskPageOffset = 0x228;       // u32: 0 Brawl stages, 1 Melee stages, 2 custom stages
constexpr uint32_t kTaskPageListOffset = 0x22C;   // per page (8 bytes apart): pointer to the page's icon ids
constexpr uint32_t kTaskPageCountOffset = 0x230;  // per page: u8 icon count
constexpr uint32_t kTaskPageStride = 8;
constexpr uint32_t kTaskRegionOffset = 0x244;     // u32: region under the pointer
constexpr uint32_t kTaskCustomIndexOffset = 0x24C; // s32: the custom list index of the hovered row (fn_11_5F24)
constexpr uint32_t kTaskChoiceKindOffset = 0x254; // u32: 1 a stage, 3 a custom stage (set before state 4)
constexpr uint32_t kTaskCustomRecordsOffset = 0x28C;  // 12-byte records: u8 flags (0x80 source), +4 index, +8 slot
constexpr uint32_t kCustomRecordBytes = 12;
constexpr uint32_t kCustomRecordIndexOffset = 4;
constexpr uint32_t kCustomRecordSlotOffset = 8;
constexpr uint8_t kCustomRecordSourceFlag = 0x80;
constexpr uint32_t kTaskCustomCountOffset = 0x604C;  // s32: custom list length, at most 2000 (fn_11_2F04)
constexpr uint32_t kMaxCustomStages = 2000;
// The file task's per-source counts as the list was last built (fn_11_2D58 rebuilds when the file task's differ).
constexpr uint32_t kTaskBuiltCountAOffset = 0x605C;
constexpr uint32_t kTaskBuiltCountBOffset = 0x6060;
constexpr uint32_t kTaskBackShownOffset = 0x6150; // u8: the Back button exists (hit test 0x806B6F20)
constexpr uint32_t kTaskRandomPageOffset = 0x6154;   // u32: the random stage's page
constexpr uint32_t kTaskRandomIndexOffset = 0x6158;  // u32: its icon index in the page list, or custom source index
constexpr uint32_t kTaskPages = 3;
constexpr uint32_t kStagePages = 2;               // pages 0 and 1 hold stage icons, page 2 custom rows
constexpr uint32_t kCustomPage = 2;
constexpr uint32_t kModeVs = 0, kModeVsWifi = 1;
// Wi-Fi modes: bit (mode - 1) of 41, i.e. modes 1, 4 and 6 (0x806B1CBC; LEAVE instead of BACK).
constexpr uint32_t kWifiModeMask = 41;
constexpr uint32_t kStateRandomReveal = 4, kStateChosen = 2;
constexpr uint32_t kChoiceStage = 1, kChoiceCustom = 3;

// Hit-test regions (fn_11_6524 0x806B6EA8): stage icons 2 .. count+1 in the page's list order; custom rows 41-47 (only
// rows whose stage has loaded); 48 / 49 the up / down arrows (A: one row up / down, wrapping), 50 the scroll thumb (A:
// hold to scroll), 51 / 52 the track above / below it (A: six rows up / down); 53 Change, 54 Random, 55 Back.
constexpr uint32_t kFirstIconRegion = 2;
constexpr uint32_t kFirstRowRegion = 41, kLastRowRegion = 47;
constexpr uint32_t kUpArrowRegion = 48, kDownArrowRegion = 49, kThumbRegion = 50;
constexpr uint32_t kTrackAboveRegion = 51, kTrackBelowRegion = 52;
constexpr uint32_t kChangeRegion = 53, kRandomRegion = 54, kBackRegion = 55;
// Stored only by the custom-list refresh (0x806B386C) to make the next hover update fire; never a hit-test result.
constexpr uint32_t kRefreshRegion = 1;

// fn_11_85CC: a page's icon id -> MuStageKind, the u16 at (*(0x806B9298 + 8*page))[id*4].
constexpr uint32_t kStageKindTables = 0x806B9298;
// My Music appends id 255 to page 1 (0x806B1F04): the preview then shows frame 100, "MENU" (0x806B6B98).
constexpr uint8_t kMenuIcon = 255;
constexpr uint16_t kMaxStageKind = 64; // MuStageKind values stay below this; a larger one is not a stage
// muMenu::exchangeMuStageKindToMsgID 0x800AF6D8 (a leaf): byte 3 of the word at 0x80407AAC + 4*kind.
constexpr uint32_t kStageMessageIds = 0x80407AAC;
constexpr const char* kStageNameTable = "menu/msg/stage_name_en.msbin";

// muSelectStageFileTask (custom stages), its pointer at 0x806BB388: load slot s's info at +0x70 + 88*s (fn_11_8134),
// slots 0-8 (fn_11_7DE4 resets nine). Info: u32 status at +0 (3 loaded), the stage name at +32 (UTF-16, at most 16
// units: the preview prints it where a stage's name art goes, MuMsg slot 0 on bone textN0 under stnameN, and the row
// prints it on its wide lower line, textN3), the save time at +72 (OSTime; fn_11_7E7C).
constexpr uint32_t kFileTaskPointer = 0x806BB388;
constexpr uint32_t kSlotInfoOffset = 0x70;
constexpr uint32_t kSlotInfoBytes = 88;
constexpr uint32_t kLoadSlots = 9;
constexpr uint32_t kSlotLoaded = 3;
constexpr uint32_t kSlotNameOffset = 32;
constexpr uint32_t kSlotNameUnits = 16;
constexpr uint32_t kSlotTimeOffset = 72;

// The row's date order follows GameGlobal::getLanguage 0x8004D9DC: u8 at *(*0x805A00E0 + 0x24) + 0x82A; 0 prints
// "%4d.%02d.%02d" (year, month, day), anything else "%2d.%02d.%4d" (month, day, year); time "%2d:%02d" (0x806B29E4).
constexpr uint32_t kGameGlobalPointer = 0x805A00E0;
constexpr uint32_t kGameGlobalRecordOffset = 0x24;
constexpr uint32_t kRecordLanguageOffset = 0x82A;
// OSTicksToCalendarTime 0x801E1D80: ticks per second = bus clock (u32 at 0x800000F8) / 4, counted from 2000-01-01.
constexpr uint32_t kBusClockAddress = 0x800000F8;

// The screen's current task and the page its Change button names (-1 until a caption is known: the button shows a
// blank frame until the game sets one, and fn_11_15D4 sets none when there is no other page).
uint32_t g_task = 0;
int g_changeCaption = -1;
bool g_queueNextHover = false;
bool g_pageChanged = false;
std::string g_lastHover;

bool WifiMode(uint32_t mode) { return mode >= 1 && mode <= 6 && ((1u << (mode - 1)) & kWifiModeMask) != 0; }

// The page Change leads to from `page` (0x806B21FC): the next page that has icons, the custom page only offline with
// custom stages; `page` itself when no other qualifies, -1 when none does.
int NextPage(uint32_t task, uint32_t page) {
    uint32_t next = page;
    for (uint32_t step = 0; step < kTaskPages; ++step) {
        next = (next + 1) % kTaskPages;
        const bool shown =
            next < kStagePages
                ? Memory::Read8(task + kTaskPageCountOffset + next * kTaskPageStride) != 0
                : Memory::Read32(task + kTaskModeOffset) == kModeVs &&
                      static_cast<int32_t>(Memory::Read32(task + kTaskCustomCountOffset)) > 0;
        if (shown) return static_cast<int>(next);
    }
    return -1;
}

// A page by the words its Change caption shows (MenSelmapIcon_Change frames {3, 1, 2}[page], table 0x806B9140).
std::string PageName(int page) {
    if (page < 0 || page >= static_cast<int>(kTaskPages)) return {};
    return AccessibilityText::StringAt("stage_select.pages", static_cast<size_t>(page));
}

// The name of icon `icon` of stage page `page` (the preview's name art; the game's stage_name table holds the same
// names), and its place among the page's icons.
std::string StageIcon(uint32_t task, uint32_t page, uint32_t icon, size_t* place, size_t* total) {
    if (page >= kStagePages) return {};
    const uint8_t count = Memory::Read8(task + kTaskPageCountOffset + page * kTaskPageStride);
    if (icon >= count) return {};
    const uint32_t list = Memory::Read32(task + kTaskPageListOffset + page * kTaskPageStride);
    if (place) *place = icon + 1;
    if (total) *total = count;
    const uint8_t id = Memory::Read8(list + icon);
    if (id == kMenuIcon) return AccessibilityText::String("stage_select.menu");
    const uint16_t kind = Memory::Read16(Memory::Read32(kStageKindTables + page * 8) + id * 4u);
    if (kind >= kMaxStageKind) return {};
    const uint8_t message = Memory::Read8(kStageMessageIds + kind * 4u + 3);
    return AccessibilityText::DiscMessageEntry(kStageNameTable, message);
}

// Load slot info of custom list entry `index`, or 0 when its stage has not loaded.
uint32_t CustomSlotInfo(uint32_t task, int32_t index) {
    const int32_t count = static_cast<int32_t>(Memory::Read32(task + kTaskCustomCountOffset));
    if (index < 0 || count <= 0 || count > static_cast<int32_t>(kMaxCustomStages) || index >= count) return 0;
    const uint32_t record = task + kTaskCustomRecordsOffset + static_cast<uint32_t>(index) * kCustomRecordBytes;
    const int32_t slot = static_cast<int32_t>(Memory::Read32(record + kCustomRecordSlotOffset));
    if (slot < 0 || slot >= static_cast<int32_t>(kLoadSlots)) return 0;
    const uint32_t fileTask = Memory::Read32(kFileTaskPointer);
    if (fileTask == 0) return 0;
    const uint32_t info = fileTask + kSlotInfoOffset + static_cast<uint32_t>(slot) * kSlotInfoBytes;
    return Memory::Read32(info) == kSlotLoaded ? info : 0;
}

std::string CustomName(uint32_t info) { return AccessibilityText::GuestUtf16(info + kSlotNameOffset, kSlotNameUnits); }

std::string Trimmed(const char* text) {
    std::string out(text);
    out.erase(0, out.find_first_not_of(' '));
    return out;
}

// The row's date and time as fn_11_1F84 prints them from the save time.
void CustomDateTime(uint32_t info, std::string& date, std::string& time) {
    const int64_t tps = Memory::Read32(kBusClockAddress) / 4;
    if (tps <= 0) return;
    const int64_t ticks = static_cast<int64_t>(Memory::Read64(info + kSlotTimeOffset));
    int64_t seconds = ticks / tps;
    if (ticks % tps < 0) --seconds;
    int64_t days = seconds / 86400, ofDay = seconds % 86400;
    if (ofDay < 0) ofDay += 86400, --days;
    // Civil date from days since 2000-01-01 (proleptic Gregorian, as the OS library counts).
    const int64_t z = days + 10957 + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t dayOfEra = z - era * 146097;
    const int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const int64_t mp = (5 * dayOfYear + 2) / 153;
    const int day = static_cast<int>(dayOfYear - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int year = static_cast<int>(yearOfEra + era * 400 + (month <= 2 ? 1 : 0));
    const uint8_t language = Memory::Read8(Memory::Read32(Memory::Read32(kGameGlobalPointer) + kGameGlobalRecordOffset) +
                                           kRecordLanguageOffset);
    char buffer[32];
    if (language == 0)
        std::snprintf(buffer, sizeof buffer, "%4d.%02d.%02d", year, month, day);
    else
        std::snprintf(buffer, sizeof buffer, "%2d.%02d.%4d", month, day, year);
    date = Trimmed(buffer);
    std::snprintf(buffer, sizeof buffer, "%2d:%02d", static_cast<int>(ofDay / 3600), static_cast<int>(ofDay / 60 % 60));
    time = Trimmed(buffer);
}

// "name, date, time, n of N" for custom list entry `index`; empty when its stage has not loaded.
std::string CustomRow(uint32_t task, int32_t index) {
    const uint32_t info = CustomSlotInfo(task, index);
    if (info == 0) return {};
    std::string date, time;
    CustomDateTime(info, date, time);
    if (date.empty()) return {};
    const uint32_t count = Memory::Read32(task + kTaskCustomCountOffset);
    return AccessibilityText::Join(
        {CustomName(info), date, time, AccessibilityText::Position(static_cast<size_t>(index) + 1, count)});
}

std::string Item(uint32_t task, uint32_t region) {
    const uint32_t page = Memory::Read32(task + kTaskPageOffset);
    switch (region) {
    case kChangeRegion: return task == g_task ? PageName(g_changeCaption) : std::string();
    case kRandomRegion: return AccessibilityText::String("stage_select.random");
    case kBackRegion:
        if (Memory::Read8(task + kTaskBackShownOffset) == 0) return {};
        return AccessibilityText::String(WifiMode(Memory::Read32(task + kTaskModeOffset)) ? "stage_select.leave"
                                                                                          : "stage_select.back");
    default: break;
    }
    if (page == kCustomPage) {
        switch (region) {
        case kUpArrowRegion: return AccessibilityText::String("stage_select.up_arrow");
        case kDownArrowRegion: return AccessibilityText::String("stage_select.down_arrow");
        case kThumbRegion: return AccessibilityText::String("stage_select.scroll_bar");
        case kTrackAboveRegion: return AccessibilityText::String("stage_select.page_up");
        case kTrackBelowRegion: return AccessibilityText::String("stage_select.page_down");
        default: break;
        }
        if (region < kFirstRowRegion || region > kLastRowRegion) return {};
        return CustomRow(task, static_cast<int32_t>(Memory::Read32(task + kTaskCustomIndexOffset)));
    }
    if (region < kFirstIconRegion || region >= kFirstRowRegion) return {};
    size_t place = 0, total = 0;
    const std::string name = StageIcon(task, page, region - kFirstIconRegion, &place, &total);
    if (name.empty()) return {};
    return AccessibilityText::Join({name, AccessibilityText::Position(place, total)});
}

// What the pointer is on, or empty for nothing speakable.
std::string HoveredItem(uint32_t task, uint32_t region) {
    try {
        return Item(task, region);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stage select", e);
        return {};
    }
}

void SayHover(const std::string& text, bool interrupt) {
    if (text.empty()) return;
    Speech::Say(text, interrupt);
    g_lastHover = text;
}

void QueueNextHover() { g_queueNextHover = true; }

void SpeakHovered(uint32_t task, bool interrupt) {
    uint32_t region = 0;
    if (AccessibilityText::SilencedByMods() || !Memory::TryRead32(task + kTaskRegionOffset, region)) return;
    const std::string item = HoveredItem(task, region);
    if (item.empty()) return;
    SayHover(item, interrupt && !g_queueNextHover);
    g_queueNextHover = false;
}

// The screen opened (VS and My Music): the VS title, "STAGE SELECT" (MenSelmapBack07 on the Back model, shown for the
// whole screen), interrupts; the first item hovered queues after it.
void Init(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3];
    g_task = task;
    g_changeCaption = -1;
    g_queueNextHover = false;
    g_pageChanged = false;
    g_lastHover.clear();
    func_806B0EB0(ctx);
    if (AccessibilityText::SilencedByMods()) return;
    try {
        const uint32_t mode = Memory::Read32(task + kTaskModeOffset);
        if ((mode != kModeVs && mode != kModeVsWifi) || Memory::Read32(task + kTaskBackObjectOffset) == 0) return;
        const std::string title = AccessibilityText::String("stage_select.title");
        if (title.empty()) return;
        Speech::Say(title, true);
        g_queueNextHover = true;
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stage select open", e);
    }
}

// Runs every input frame (state 0, not while the scroll thumb is held). Speaks when the region under the pointer
// changes; after the page changed, what is now under the pointer queues after the page name.
void Hover(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(task + kTaskRegionOffset, before);
    const bool queue = g_queueNextHover, pageChanged = g_pageChanged;
    g_queueNextHover = g_pageChanged = false;  // they apply to the first update after they were set
    func_806B626C(ctx);
    uint32_t region = 0;
    if (!known || !Memory::TryRead32(task + kTaskRegionOffset, region) || AccessibilityText::SilencedByMods()) return;
    if (region == before) {
        // Change moved to the new page's last slot and is still under the pointer: its new caption.
        if (pageChanged && region == kChangeRegion) SayHover(HoveredItem(task, region), !queue);
        return;
    }
    const std::string item = HoveredItem(task, region);
    // The custom list was rebuilt: the buttons under the still pointer are not news, nor a row whose text is the same.
    // In practice the rebuild resets every row to loading (fn_11_7DE4), so the row under the pointer speaks again
    // through the normal path once it has loaded.
    if (before == kRefreshRegion && ((region < kFirstRowRegion || region > kLastRowRegion) || item == g_lastHover))
        return;
    SayHover(item, !queue);
}

// The input decision: a page change here is a Change press (the only page write in it, 0x806B5940). The page is
// named by the words the button showed; the next hover update queues what is now under the pointer.
void Decision(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(task + kTaskPageOffset, before);
    func_806B5630(ctx);
    uint32_t page = 0;
    if (!known || !Memory::TryRead32(task + kTaskPageOffset, page) || page == before ||
        AccessibilityText::SilencedByMods())
        return;
    const std::string name = PageName(static_cast<int>(page));
    if (name.empty()) return;
    Speech::Say(name, true);
    g_queueNextHover = g_pageChanged = true;
}

// Shows page r4 (init, Change, the random reveal) and sets the Change caption to the next page, unless there is none
// other than this one (0x806B2254).
void SetPage(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3], page = ctx->gpr[4];
    func_806B1F58(ctx);
    try {
        const int next = NextPage(task, page);
        if (task == g_task && next >= 0 && next != static_cast<int>(page)) g_changeCaption = next;
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stage select page", e);
    }
}

// Runs every frame offline (mode 0); when the file task's custom-stage counts changed it rebuilds the list and sets the
// Change caption again, this time without the one-page check (0x806B37D0..3850). A new caption is spoken only while
// the pointer is on Change.
void CustomRefresh(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3];
    uint32_t countA = 0, countB = 0;
    const bool known = Memory::TryRead32(task + kTaskBuiltCountAOffset, countA) &&
                       Memory::TryRead32(task + kTaskBuiltCountBOffset, countB);
    func_806B36DC(ctx);
    if (!known || task != g_task) return;
    try {
        if (Memory::Read32(task + kTaskBuiltCountAOffset) == countA &&
            Memory::Read32(task + kTaskBuiltCountBOffset) == countB)
            return;
        const int next = NextPage(task, Memory::Read32(task + kTaskPageOffset));
        if (next < 0 || next == g_changeCaption) return;
        g_changeCaption = next;
        if (AccessibilityText::SilencedByMods() || Memory::Read32(task + kTaskRegionOffset) != kChangeRegion) return;
        SayHover(PageName(next), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stage select custom list", e);
    }
}

// The custom list index of the random custom stage: the first source-0 entry with its source index (0x806B51FC).
int32_t RandomCustomIndex(uint32_t task) {
    const int32_t count = static_cast<int32_t>(Memory::Read32(task + kTaskCustomCountOffset));
    if (count <= 0 || count > static_cast<int32_t>(kMaxCustomStages)) return -1;
    const uint32_t wanted = Memory::Read32(task + kTaskRandomIndexOffset);
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t record = task + kTaskCustomRecordsOffset + static_cast<uint32_t>(i) * kCustomRecordBytes;
        if ((Memory::Read8(record) & kCustomRecordSourceFlag) == 0 &&
            Memory::Read32(record + kCustomRecordIndexOffset) == wanted)
            return i;
    }
    return -1;
}

// State 4 runs until the random stage is shown in the preview, then moves to state 2 (0x806B5338): its name.
void RandomReveal(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(task + kTaskStateOffset, before);
    func_806B4D70(ctx);
    uint32_t state = 0;
    if (!known || before != kStateRandomReveal || !Memory::TryRead32(task + kTaskStateOffset, state) ||
        state != kStateChosen || AccessibilityText::SilencedByMods())
        return;
    try {
        std::string name;
        const uint32_t kind = Memory::Read32(task + kTaskChoiceKindOffset);
        if (kind == kChoiceStage) {
            name = StageIcon(task, Memory::Read32(task + kTaskRandomPageOffset),
                             Memory::Read32(task + kTaskRandomIndexOffset), nullptr, nullptr);
        } else if (kind == kChoiceCustom) {
            if (const uint32_t info = CustomSlotInfo(task, RandomCustomIndex(task)); info != 0) name = CustomName(info);
        }
        if (!name.empty()) Speech::Say(name, true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stage select random", e);
    }
}

} // namespace StageSelectSpeech

extern "C" void StageSelectInit_806b0eb0(CpuContext* ctx) { StageSelectSpeech::Init(ctx); }
extern "C" void StageSelectHover_806b626c(CpuContext* ctx) { StageSelectSpeech::Hover(ctx); }
extern "C" void StageSelectDecision_806b5630(CpuContext* ctx) { StageSelectSpeech::Decision(ctx); }
extern "C" void StageSelectSetPage_806b1f58(CpuContext* ctx) { StageSelectSpeech::SetPage(ctx); }
extern "C" void StageSelectCustomRefresh_806b36dc(CpuContext* ctx) { StageSelectSpeech::CustomRefresh(ctx); }
extern "C" void StageSelectRandomReveal_806b4d70(CpuContext* ctx) { StageSelectSpeech::RandomReveal(ctx); }

REGISTER_NATIVE_FUNCTION_AS(0x806B0EB0, StageSelectInit_806b0eb0, "StageSelectInit_806b0eb0");
REGISTER_NATIVE_FUNCTION_AS(0x806B626C, StageSelectHover_806b626c, "StageSelectHover_806b626c");
REGISTER_NATIVE_FUNCTION_AS(0x806B5630, StageSelectDecision_806b5630, "StageSelectDecision_806b5630");
REGISTER_NATIVE_FUNCTION_AS(0x806B1F58, StageSelectSetPage_806b1f58, "StageSelectSetPage_806b1f58");
REGISTER_NATIVE_FUNCTION_AS(0x806B36DC, StageSelectCustomRefresh_806b36dc, "StageSelectCustomRefresh_806b36dc");
REGISTER_NATIVE_FUNCTION_AS(0x806B4D70, StageSelectRandomReveal_806b4d70, "StageSelectRandomReveal_806b4d70");
