#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

// Screen-reader speech for the Events screen (scene scSelEvent, muEventTask in sora_menu_event, a module loaded on
// demand). The task runs a state machine (table 0x940A6950, {enter, process} pairs): 0 builds the screen once, 1 plays
// the intro, 2 is the event list, 3 the difficulty choice, 4 starts the event. Event hooks: state 0's process (the
// screen has opened), the detail panel's redraw (the selected event changed, or the list was entered or switched),
// the co-op redraw (a second player joined), and state 3's enter and process (the difficulty choice).
//
// With Gecko codes active (Project+ and other SD mods) on-demand modules are linked in the heap instead of at their
// translated home, so these hooks never fire.

extern "C" void func_940A5740(CpuContext* ctx);  // muEventTask: state 0 process (build, then 1 once done)
extern "C" void func_940A5298(CpuContext* ctx);  // muEventTask: draw the detail panel (task, cursor, table)
extern "C" void func_940A4DB4(CpuContext* ctx);  // muEventTask: draw the co-op parts
extern "C" void func_940A5E54(CpuContext* ctx);  // muEventTask: state 3 enter (the difficulty choice opens)
extern "C" void func_940A5FB8(CpuContext* ctx);  // muEventTask: state 3 process (input while it is open)

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace EventsSpeech {

// muEventTask (verified in sora_menu_event).
// The state machine (processDefault 0x940A4490): the state u32 at +0x1A0 and the one whose enter last ran at +0x1A4
// (stored after the enter, 0x940A4574). The co-op check (0x940A45C0..0x940A45D8) runs after the process in the frame a
// process returns 2, before state 2's enter has run (it runs next frame).
constexpr uint32_t kTaskEnteredStateOffset = 0x1A4;
constexpr uint32_t kStateList = 2;
constexpr uint32_t kTaskCursorOffset = 0x17B8;      // u32: the selected event (state 2's process writes it)
constexpr uint32_t kTaskTableOffset = 0x17BC;       // u32: 0 the solo events, 1 the co-op events
constexpr uint32_t kTaskMuMsgOffset = 0x17C0;       // MuMsg of the detail panel (event_en, archive entry 30)
constexpr uint32_t kTaskDifficultyOffset = 0x17C8;  // u32: 0-2, the difficulty choice's cursor
constexpr uint32_t kTables = 2;
// The unlocked events, copied by 0x940A4C88 from muEventTbl_1P / muEventTbl_2P: 16-byte records from task+0xFB0, the
// count u8 at task+0x13B0, each table 0x404 bytes further on (room for 64 records).
constexpr uint32_t kTaskEventsOffset = 0xFB0;
constexpr uint32_t kTaskEventCountOffset = 0x13B0;
constexpr uint32_t kTableStride = 0x404;
constexpr uint32_t kEventBytes = 16;
constexpr uint32_t kMaxEvents = 64;
constexpr uint32_t kEventNameOffset = 0;         // u32 event_en entry
constexpr uint32_t kEventDescriptionOffset = 4;  // u32 event_en entry
constexpr uint32_t kEventFighterOffset = 8;      // u8 MuSelchkind, 40: the player chooses
constexpr uint32_t kEventPartnerOffset = 10;     // u8 MuSelchkind of the co-op events' second fighter
constexpr uint32_t kEventFlagsOffset = 11;       // u8: 0x40 the record is a score, else a time
constexpr uint8_t kEventScoreFlag = 0x40;
// The records by place in the list (0x940A5390..0x940A541C): from task+0x3B0, 0x600 bytes per table, 24 per event,
// 8 per difficulty; the u8 at +0 has 0x80 set once cleared, the u32 at +4 is the score or the time in frames.
constexpr uint32_t kTaskRecordsOffset = 0x3B0;
constexpr uint32_t kRecordTableStride = 0x600;
constexpr uint32_t kRecordEventBytes = 24;
constexpr uint32_t kRecordDifficultyBytes = 8;
constexpr uint32_t kRecordValueOffset = 4;
constexpr uint8_t kRecordClearedFlag = 0x80;
constexpr uint32_t kDifficulties = 3;
constexpr uint32_t kMaxScore = 99999;
constexpr uint32_t kFramesPerMinute = 3600, kFramesPerSecond = 60, kMaxMinutes = 100;
// MuMsg: its message table at +4 (getMsgData 0x800B9348).
constexpr uint32_t kMuMsgTableOffset = 4;
// event_en entry 2 "Choose": the fighter line of a solo event the player picks a fighter for (0x940A551C).
constexpr uint32_t kChooseEntry = 2;
constexpr uint32_t kChooseKind = 40;
constexpr uint32_t kFighterKinds = 40;  // MuSelchkind 0-39, the roster

// Set while state 0 builds the screen: the panel and co-op draws it makes are not changes.
bool g_building = false;
// The task whose next detail panel queues after a title just spoken (the opening, or the co-op switch).
uint32_t g_queueRowTask = 0;
// A co-op switch that came before the list was entered: its own panel draw is skipped (the list's enter draws it
// next frame), and "Co-op" is spoken with that row instead.
uint32_t g_skipPanelTask = 0;
uint32_t g_pendingCoopTask = 0;

void Say(const std::string& text, bool interrupt) {
    if (!text.empty()) Speech::Say(text, interrupt);
}

std::string Trimmed(std::string text) {
    const size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    text.erase(0, first);
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

uint32_t Table(uint32_t task) { return Memory::Read32(task + kTaskTableOffset); }

// The fighter's name as its name plate on the character select reads (strings_en.toml: the panel prints the same
// words from the internal table 0x800AFA30, which stores the é of Pokémon as U+2208).
std::string FighterName(uint32_t kind) {
    if (kind >= kFighterKinds) return {};
    return AccessibilityText::StringAt("character_select.fighters", kind);
}

// The record line as 0x940A55DC prints it: a score ("%d", at most 99999, "-" when not cleared) or a time ("%2d:%02d
// %02d", minutes, seconds, hundredths, at most 99:59 99; "--:-- --").
std::string RecordText(bool cleared, uint32_t value, bool score) {
    if (!cleared) return score ? "-" : "--:-- --";
    if (score) return std::to_string(std::min(value, kMaxScore));
    uint32_t minutes = value / kFramesPerMinute, seconds = 59, hundredths = 99;
    if (minutes >= kMaxMinutes) {
        minutes = 99;
    } else {
        const uint32_t rest = value - minutes * kFramesPerMinute;
        seconds = rest / kFramesPerSecond;
        hundredths = (rest - seconds * kFramesPerSecond) * 100 / kFramesPerSecond;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%2u:%02u %02u", minutes, seconds, hundredths);
    return Trimmed(text);
}

// An event's record for one difficulty: the u8 at +0 has 0x80 set once cleared, the u32 at +4 is the value.
uint32_t RecordAddress(uint32_t task, uint32_t cursor, uint32_t table, uint32_t difficulty) {
    return task + kTaskRecordsOffset + table * kRecordTableStride + cursor * kRecordEventBytes +
           difficulty * kRecordDifficultyBytes;
}

bool Cleared(uint32_t record) { return (Memory::Read8(record) & kRecordClearedFlag) != 0; }

// The panel's record: the hardest difficulty cleared, then its icon (AdvMenuLvl.1-3 on MenEvent0012_TopN at
// task+0x190, VIS0 frame hardest + 1, 0x940A5460..0x940A5494), spoken as the difficulty choice names that icon:
// "1:23 45, Hard". Not cleared: the dashes alone (frame 0 shows no icon).
std::string Record(uint32_t task, uint32_t cursor, uint32_t table, bool score) {
    int hardest = -1;
    for (uint32_t difficulty = 0; difficulty < kDifficulties; ++difficulty)
        if (Cleared(RecordAddress(task, cursor, table, difficulty))) hardest = static_cast<int>(difficulty);
    if (hardest < 0) return RecordText(false, 0, score);
    const uint32_t record = RecordAddress(task, cursor, table, static_cast<uint32_t>(hardest));
    return AccessibilityText::Join({RecordText(true, Memory::Read32(record + kRecordValueOffset), score),
                                    AccessibilityText::StringAt("events.difficulties", static_cast<size_t>(hardest))});
}

// An event row: "name, fighter, description, record, n of N". The panel prints "#n " before the name; n is the
// place in the list, which the position already gives. The solo panel prints the fighter's name (or "Choose"); the
// co-op panel shows both fighters' portraits only, which speak as their names. A player-chosen fighter's portrait
// (frame 0 of MenEvent0000_TopN__0 / MenEvent0040_TopN__0, texture MenReplayChr.600) is an arrow with no text, and the
// co-op panel clears the name line (0x940A4C48), so it is left out.
std::string Row(uint32_t task, uint32_t cursor, uint32_t table) {
    if (table >= kTables) return {};
    const uint32_t count = Memory::Read8(task + kTaskEventCountOffset + table * kTableStride);
    if (count == 0 || count > kMaxEvents || cursor >= count) return {};
    const uint32_t event = task + kTaskEventsOffset + table * kTableStride + cursor * kEventBytes;
    const uint32_t msbin = Memory::Read32(Memory::Read32(task + kTaskMuMsgOffset) + kMuMsgTableOffset);
    if (msbin == 0) return {};
    const std::string name = Trimmed(AccessibilityText::MessageEntry(msbin, Memory::Read32(event + kEventNameOffset)));
    if (name.empty()) return {};
    const uint32_t fighter = Memory::Read8(event + kEventFighterOffset);
    std::string fighters;
    if (table == 0)
        fighters = fighter == kChooseKind ? AccessibilityText::MessageEntry(msbin, kChooseEntry) : FighterName(fighter);
    else
        fighters = AccessibilityText::Join({FighterName(fighter), FighterName(Memory::Read8(event + kEventPartnerOffset))});
    const std::string description =
        Trimmed(AccessibilityText::MessageEntry(msbin, Memory::Read32(event + kEventDescriptionOffset)));
    const bool score = (Memory::Read8(event + kEventFlagsOffset) & kEventScoreFlag) != 0;
    return AccessibilityText::Join(
        {name, fighters, description, Record(task, cursor, table, score), AccessibilityText::Position(cursor + 1, count)});
}

// The difficulty choice's selected item (MenEvent07Lvsel02 "EASY NORMAL HARD", left to right; the cursor's CHR0
// MenEvent0004_TopN__0, task+0x80, sits at x -13.2, 0, 13.2 for 0, 1, 2), then the record line the choice redraws for
// that difficulty (0x940A5ECC..0x940A5F5C on opening, 0x940A6064..0x940A60F4 on each move): "Normal, 2 of 3, 1:23 45".
std::string Difficulty(uint32_t task) {
    const uint32_t difficulty = Memory::Read32(task + kTaskDifficultyOffset);
    if (difficulty >= kDifficulties) return {};
    const std::string choice = AccessibilityText::Join({AccessibilityText::StringAt("events.difficulties", difficulty),
                                                        AccessibilityText::Position(difficulty + 1, kDifficulties)});
    const uint32_t cursor = Memory::Read32(task + kTaskCursorOffset), table = Table(task);
    if (table >= kTables) return choice;
    const uint32_t count = Memory::Read8(task + kTaskEventCountOffset + table * kTableStride);
    if (count > kMaxEvents || cursor >= count) return choice;
    const uint32_t event = task + kTaskEventsOffset + table * kTableStride + cursor * kEventBytes;
    const bool score = (Memory::Read8(event + kEventFlagsOffset) & kEventScoreFlag) != 0;
    const uint32_t record = RecordAddress(task, cursor, table, difficulty);
    return AccessibilityText::Join(
        {choice, RecordText(Cleared(record), Memory::Read32(record + kRecordValueOffset), score)});
}

} // namespace EventsSpeech

// State 0 waits for the menu sounds, then builds everything and returns 1 once: the screen has opened. The title
// interrupts ("Co-op" follows it when the co-op events are shown, MenEvent08Hutariicon moving in with
// MenEvent0021_TopN__1); the selected event queues after it once the list is entered.
extern "C" void EventsBuild_940a5740(CpuContext* ctx) {
    using namespace EventsSpeech;
    const uint32_t task = ctx->gpr[3];
    {
        struct BuildingScope {
            BuildingScope() { g_building = true; }
            ~BuildingScope() { g_building = false; }
        } building;
        func_940A5740(ctx);
    }
    if (ctx->gpr[3] != 1 || AccessibilityText::SilencedByMods()) return;
    g_skipPanelTask = g_pendingCoopTask = 0;
    try {
        const uint32_t table = Table(task);
        const std::string coop = table == 1 ? AccessibilityText::String("events.co_op") : std::string();
        Say(AccessibilityText::Join({AccessibilityText::String("events.title"), coop}), true);
        g_queueRowTask = task;
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "events open", e);
    }
}

// The detail panel is redrawn for the selected event: on entering the list (after the intro, or back from the
// difficulty choice), on every cursor move and on the co-op switch.
extern "C" void EventsPanel_940a5298(CpuContext* ctx) {
    using namespace EventsSpeech;
    const uint32_t task = ctx->gpr[3], cursor = ctx->gpr[4], table = ctx->gpr[5];
    func_940A5298(ctx);
    if (g_building || AccessibilityText::SilencedByMods()) return;
    try {
        if (g_skipPanelTask == task) {
            g_skipPanelTask = 0;
            return;
        }
        bool queued = g_queueRowTask == task;
        g_queueRowTask = 0;
        if (g_pendingCoopTask == task) {
            g_pendingCoopTask = 0;
            Say(AccessibilityText::String("events.co_op"), !queued);
            queued = true;
        }
        Say(Row(task, cursor, table), !queued);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "events row", e);
    }
}

// Another controller pressed Start on the list (0x940A4A3C): the co-op events replace the solo ones and the "Co-op"
// badge moves in. It interrupts; the panel redrawn next (the co-op list's first event) queues after it. When the switch
// comes in the frame the intro (or the difficulty choice) hands over to the list, before the list's enter, "Co-op"
// waits for that enter's row instead, so the opening title is not cut and the row is not spoken twice.
extern "C" void EventsCoop_940a4db4(CpuContext* ctx) {
    using namespace EventsSpeech;
    const uint32_t task = ctx->gpr[3];
    func_940A4DB4(ctx);
    if (g_building || AccessibilityText::SilencedByMods()) return;
    try {
        if (Table(task) != 1) return;
        if (Memory::Read32(task + kTaskEnteredStateOffset) != kStateList) {
            g_skipPanelTask = task;
            g_pendingCoopTask = task;
            return;
        }
        Say(AccessibilityText::String("events.co_op"), true);
        g_queueRowTask = task;
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "events co-op", e);
    }
}

// The difficulty choice opened (once per opening): its selected difficulty. It has no title.
extern "C" void EventsDifficultyOpen_940a5e54(CpuContext* ctx) {
    using namespace EventsSpeech;
    const uint32_t task = ctx->gpr[3];
    func_940A5E54(ctx);
    if (AccessibilityText::SilencedByMods()) return;
    try {
        Say(Difficulty(task), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "events difficulty", e);
    }
}

// Runs every frame while the difficulty choice is open; speaks when its cursor moves.
extern "C" void EventsDifficultyInput_940a5fb8(CpuContext* ctx) {
    using namespace EventsSpeech;
    const uint32_t task = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(task + kTaskDifficultyOffset, before);
    func_940A5FB8(ctx);
    if (!known || AccessibilityText::SilencedByMods()) return;
    try {
        if (Memory::Read32(task + kTaskDifficultyOffset) != before) Say(Difficulty(task), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "events difficulty", e);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x940A5740, EventsBuild_940a5740, "EventsBuild_940a5740");
REGISTER_NATIVE_FUNCTION_AS(0x940A5298, EventsPanel_940a5298, "EventsPanel_940a5298");
REGISTER_NATIVE_FUNCTION_AS(0x940A4DB4, EventsCoop_940a4db4, "EventsCoop_940a4db4");
REGISTER_NATIVE_FUNCTION_AS(0x940A5E54, EventsDifficultyOpen_940a5e54, "EventsDifficultyOpen_940a5e54");
REGISTER_NATIVE_FUNCTION_AS(0x940A5FB8, EventsDifficultyInput_940a5fb8, "EventsDifficultyInput_940a5fb8");
