#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Screen-reader speech for the rules screens (muRuleTask, sora_menu_rule, resident): Rules, More Rules, Item Switch and
// Random Stage Switch, opened from Group > Rules in the main menu and from the character select. The task owns one
// muProcMenu page per screen; event hooks wrap each page's enter (the page opened, or came back from a child page) and
// its moveCursor override, which holds all of the page's input: the cursor and every value are compared before and
// after it.
//
// Rules and More Rules show their title only in the main menu, whose description panel draws it (setSelect 14 / 15);
// the character select's panel draws none. Item Switch and Random Stage Switch carry their own title art everywhere.
// On / Off is read from the switch art the game shows (its colour-animation frame), so it is what the screen shows.

extern "C" void func_806A5BEC(CpuContext* ctx);  // muProcRule1 (Rules) enter, vtable +0xC
extern "C" void func_806A84F4(CpuContext* ctx);  // muProcRule2 (More Rules) enter
extern "C" void func_806AA5E8(CpuContext* ctx);  // muProcItemSwitch enter
extern "C" void func_806AB724(CpuContext* ctx);  // muProcStageSwitch enter
extern "C" void func_806A6F68(CpuContext* ctx);  // muProcRule1 moveCursor override, vtable +0x20
extern "C" void func_806A9498(CpuContext* ctx);  // muProcRule2 moveCursor override
extern "C" void func_806AAA6C(CpuContext* ctx);  // muProcItemSwitch moveCursor override
extern "C" void func_806AC16C(CpuContext* ctx);  // muProcStageSwitch moveCursor override

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace RulesSpeech {

// muProcMenu base (constructor 0x806A4CE8) and the page vtables the page constructors store at +0x668.
constexpr uint32_t kProcCursorOffset = 0x42;    // u16 (Rules, More Rules)
constexpr uint32_t kProcObjectsOffset = 0x64C;  // the task's MuObject* array (task +0x48), by object number
constexpr uint32_t kProcPanelOffset = 0x658;    // the description panel (vtable +0x10, 0x806A4654)
constexpr uint32_t kProcVtableOffset = 0x668;
constexpr uint32_t kRulesVtable = 0x806AE520, kMoreRulesVtable = 0x806AEC00;
constexpr uint32_t kItemSwitchVtable = 0x806AECC8, kStageSwitchVtable = 0x806AEE48;

// The description panel: the main menu's muMainPanelCtrlTask or the character select's muSelcharPanelCtrlTask. Both
// keep their vtable at +0x3C and a MuMsg at +0x48; the task installs the rules table in it (vtable +0x6C: setMsgData
// 0x800B8C7C stores the table at MuMsg +4), and the pages print with setMsg(entry, 1, null), i.e. from that table.
// setSelect (vtable +0x74) draws the title bar only in the main menu (0x9403A290; the character select's 0x8069F104
// is an empty function).
constexpr uint32_t kPanelVtableOffset = 0x3C;
constexpr uint32_t kPanelMuMsgOffset = 0x48;
constexpr uint32_t kPanelSetSelectSlot = 0x74;
constexpr uint32_t kMainMenuSetSelect = 0x9403A290;
constexpr uint32_t kMuMsgTableOffset = 0x4;

// Rules page (enter 0x806A5BEC): working copies of the rules, u8 each.
constexpr uint32_t kRulesValuesOffset = 0x66C;  // +0 style (0 time, 1 stock, 2 coin), +1 time (0 infinite),
                                                // +2 handicap (0 off, 1 on, 2 auto), +3 damage ratio x10 (5-20),
                                                // +4 stage choice (0-4), +7 stock
constexpr uint32_t kRulesValueBytes = 8;
constexpr uint32_t kRulesEnabledOffset = 0x6A0;    // u8 per cursor 0-6: 0 greyed out (navigation skips it)
constexpr uint32_t kRulesMoreRulesOffset = 0x6A7;  // u8: More Rules unlocked (fn_8004E810); else not drawn or reachable
constexpr uint16_t kRulesItems = 7;
enum : uint16_t { kRowStyle, kRowTime, kRowHandicap, kRowDamage, kRowStageChoice, kRowItemSwitch, kRowMoreRules };
enum : uint8_t { kStyleTime, kStyleStock, kStyleCoin };
constexpr uint32_t kRulesFirstOpening = 6;  // the enter's previous page when the task opens it (0x806A4590)

// More Rules page (enter 0x806A84F4): stock time limit (0 infinite), then team attack, pause, score display and damage
// gauge (0 off, 1 on), u8 each.
constexpr uint32_t kMoreRulesValuesOffset = 0x66C;
constexpr uint32_t kMoreRulesValueBytes = 5;
constexpr uint32_t kMoreRulesEnabledOffset = 0x694;     // u8 per cursor 0-5
constexpr uint32_t kMoreRulesStageSwitchOffset = 0x69A; // u8: Random Stage Switch unlocked (fn_8004E834)
constexpr uint16_t kMoreRulesItems = 6;
constexpr uint16_t kMoreRulesStageSwitchRow = 5;
constexpr uint32_t kMoreRulesFromRules = 0;  // the previous page when entered from Rules (from Stage Switch: 3)

// Item Switch page (enter 0x806AA5E8, text 0x806AB118).
constexpr uint32_t kItemMuMsgOffset = 0x66C;     // its own MuMsg, printing from the rules table
constexpr uint32_t kItemColumnOffset = 0x670;    // u32
constexpr uint32_t kItemRowOffset = 0x674;       // u32: 0 frequency, 1 Turn All and the first items, 2-8 items
constexpr uint32_t kItemFrequencyOffset = 0x678; // s8: 0 none, 1 low, 2 medium, 3 high
constexpr uint32_t kItemMaskHiOffset = 0x680;    // u32 pair: bit i = item i on
constexpr uint32_t kItemMaskLoOffset = 0x684;
constexpr uint32_t kItemColumns = 7;
constexpr uint32_t kItems = 49;
constexpr uint32_t kItemPlaces = kItems + 2;     // frequency (place 0), Turn All (1), items (2-50)
constexpr uint32_t kItemFrequencyPlace = 0, kItemTurnAllPlace = 1, kItemFirstPlace = 2;
// Per item, u32: its name and description number (rules table 27 + n and 80 + n), and its category's rules table entry.
constexpr uint32_t kItemNumbers = 0x806AD3C4;
constexpr uint32_t kItemCategories = 0x806AD300;
constexpr uint32_t kItemNameEntry = 27, kItemDescriptionEntry = 80;
constexpr uint32_t kFrequencyNameEntry = 76, kFrequencyDescriptionEntry = 129;
constexpr uint32_t kTurnAllOnEntry = 133, kTurnAllOffEntry = 134;
constexpr uint32_t kFirstCategoryEntry = 135, kLastCategoryEntry = 146;
constexpr uint8_t kFrequencies = 4;
constexpr uint32_t kItemFirstObject = 32;  // item i's switch art (MenMainSwitch0003)
constexpr uint32_t kItemTurnAllObject = 81;  // MenMainSwitch0004

// Random Stage Switch page (enter 0x806AB724, text 0x806ACD80). Only unlocked stages are listed (in slot order).
constexpr uint32_t kStageMuMsgOffset = 0x66C;      // its own MuMsg, printing from the stage name table
constexpr uint32_t kStageColumnOffset = 0x670;     // u32: 0-4 the Brawl grid, 5-6 the Melee column
constexpr uint32_t kStageRowOffset = 0x674;        // u32: 0 the Turn All buttons
constexpr uint32_t kStageMeleeRowsOffset = 0x67C;  // u32: rows of the Melee column; the row below is Custom Stages
constexpr uint32_t kStageBrawlSlotsOffset = 0x688; // u32 per listed Brawl stage: its slot
constexpr uint32_t kStageBrawlCountOffset = 0x704;
constexpr uint32_t kStageMeleeSlotsOffset = 0x708; // u32 per listed Melee stage: its slot
constexpr uint32_t kStageMeleeCountOffset = 0x730;
constexpr uint32_t kStageMaskHiOffset = 0x738;     // u32 pair: the stages random selection may pick
constexpr uint32_t kStageMaskLoOffset = 0x73C;
constexpr uint32_t kStageCustomShownOffset = 0x760;  // u8: the Custom Stages row exists
constexpr uint32_t kBrawlSlots = 31, kMeleeSlots = 10;
constexpr uint32_t kBrawlColumns = 5, kMeleeColumns = 2;
// u32 per Brawl slot: its stage name table entry (0x806ACF38; the same table gives the slot's bit).
constexpr uint32_t kBrawlStageEntries = 0x806AEDA0;
constexpr uint32_t kMeleeFirstEntry = 31;      // Melee slot s: entry 31 + s
constexpr uint32_t kCustomStagesEntry = 41;    // "Stages made using the Stage Builder."
// The game's place numbers (0x806AC16C): 0 Brawl Turn All, 1 Melee Turn All, 2-32 Brawl, 33-42 Melee, 43 custom.
constexpr uint32_t kStageBrawlTurnAll = 0, kStageMeleeTurnAll = 1, kStageFirstBrawl = 2, kStageFirstMelee = 33;
constexpr uint32_t kStageCustom = 43;
constexpr uint32_t kBrawlTurnAllObject = 85, kMeleeTurnAllObject = 86;  // MenMainSwitch0102, 0103
constexpr uint32_t kBrawlFirstObject = 87, kMeleeFirstObject = 118, kCustomObject = 128;  // 0104, 0105, 0106

// A switch's art shows OFF at colour-animation frame 0 and ON at frame 1 (CLR0 of MenMainSwitch0003/0004/0102-0106:
// the OFF texture's material is opaque only at frame 0, the ON one from frame 1). The game sets these frames through
// MuObject::setFrameMatCol 0x800B7A18: *(object+0x14) is the gfModelAnimation, whose +0x18 holds an nw4r
// AnmObjMatClrRes (vtable 0x80465D08, constructor 0x80197DA8); its GetFrame 0x80198058 returns the float at +0x18.
// The task sets every object's update rate to 0 (0x806A41B8), so a frame stays as set.
constexpr uint32_t kMuObjectAnimationOffset = 0x14;
constexpr uint32_t kAnimationMatClrOffset = 0x18;
constexpr uint32_t kAnmObjMatClrResVtable = 0x80465D08;
constexpr uint32_t kAnmObjFrameOffset = 0x18;

void Say(const std::string& text, bool interrupt) {
    if (!text.empty()) Speech::Say(text, interrupt);
}

bool IsPage(uint32_t proc, uint32_t vtable) {
    uint32_t actual = 0;
    return Memory::TryRead32(proc + kProcVtableOffset, actual) && actual == vtable;
}

std::string OnOff(bool on) { return AccessibilityText::String(on ? "on" : "off"); }

// Entry `index` of the table a MuMsg prints from.
std::string MuMsgEntry(uint32_t muMsg, uint32_t index) {
    if (muMsg == 0) return {};
    const uint32_t table = Memory::Read32(muMsg + kMuMsgTableOffset);
    if (table == 0) return {};
    return AccessibilityText::MessageEntry(table, index);
}

uint32_t PanelMuMsg(uint32_t proc) {
    const uint32_t panel = Memory::Read32(proc + kProcPanelOffset);
    return panel != 0 ? Memory::Read32(panel + kPanelMuMsgOffset) : 0;
}

// Whether the page's description panel is the main menu's, which draws the Rules and More Rules titles.
bool MainMenuPanel(uint32_t proc) {
    const uint32_t panel = Memory::Read32(proc + kProcPanelOffset);
    if (panel == 0) return false;
    const uint32_t vtable = Memory::Read32(panel + kPanelVtableOffset);
    return Memory::Read32(vtable + kPanelSetSelectSlot) == kMainMenuSetSelect;
}

// Whether a switch's art shows ON (true) or OFF (false); nullopt when its frame cannot be verified.
std::optional<bool> ShownOn(uint32_t proc, uint32_t object) {
    const uint32_t objects = Memory::Read32(proc + kProcObjectsOffset);
    const uint32_t muObject = objects != 0 ? Memory::Read32(objects + object * 4) : 0;
    const uint32_t animation = muObject != 0 ? Memory::Read32(muObject + kMuObjectAnimationOffset) : 0;
    const uint32_t matClr = animation != 0 ? Memory::Read32(animation + kAnimationMatClrOffset) : 0;
    if (matClr == 0 || Memory::Read32(matClr) != kAnmObjMatClrResVtable) {
        AccessibilityText::LogOnce("rules: a switch's colour animation is not the expected AnmObjMatClrRes");
        return std::nullopt;
    }
    const float frame = Memory::ReadFloat32(matClr + kAnmObjFrameOffset);
    if (frame == 0.0f) return false;
    if (frame == 1.0f) return true;
    AccessibilityText::LogOnce("rules: a switch shows an unexpected colour-animation frame");
    return std::nullopt;
}

std::string ShownValue(uint32_t proc, uint32_t object) {
    const auto on = ShownOn(proc, object);
    return on ? OnOff(*on) : std::string();
}

// A Turn All button as its caption reads: TURN ALL and the ON / OFF beside it, which is what pressing it does (frame
// 1.0 shows ON and the press turns the group on: 0x806AAE58-0x806AAED4, 0x806AC7AC-0x806AC834). "Turn All" alone when
// the ON / OFF cannot be verified.
std::string TurnAllCaption(const std::optional<bool>& on) {
    if (!on) return AccessibilityText::String("rules.turn_all");
    return AccessibilityText::String(*on ? "rules.turn_all_on" : "rules.turn_all_off");
}

// The 1-based place of `cursor` among the items `reachable` admits, and their count.
template <typename Reachable>
std::string Place(uint16_t cursor, uint16_t items, Reachable reachable) {
    size_t index = 0, total = 0;
    for (uint16_t item = 0; item < items; ++item) {
        if (!reachable(item)) continue;
        ++total;
        if (item <= cursor) ++index;
    }
    if (!reachable(cursor)) return {};
    return AccessibilityText::Position(index, total);
}

// "2" as the digits show it, 0 as the infinity sign (MenMainRuleInfini).
std::string Minutes(uint8_t value) {
    return value == 0 ? AccessibilityText::String("character_select.infinity") : std::to_string(value);
}

// A changed setting: its new value, then the description that came with it ("Turns, Choose stages in turns..."). A
// value with no description of its own (the row keeps the same description whatever its value) speaks alone.
std::string ChangedValue(const std::string& value, uint32_t muMsg, std::optional<uint32_t> entry,
                         std::optional<uint32_t> entryBefore) {
    if (value.empty()) return {};
    if (!entry || entry == entryBefore) return value;
    return AccessibilityText::Join({value, MuMsgEntry(muMsg, *entry)});
}

// ---- Rules ----

struct RulesValues {
    uint8_t bytes[kRulesValueBytes];
};

RulesValues ReadRulesValues(uint32_t proc) {
    RulesValues values{};
    for (uint32_t i = 0; i < kRulesValueBytes; ++i) values.bytes[i] = Memory::Read8(proc + kRulesValuesOffset + i);
    return values;
}

bool RulesReachable(uint32_t proc, uint16_t cursor) {
    if (cursor >= kRulesItems || Memory::Read8(proc + kRulesEnabledOffset + cursor) == 0) return false;
    return cursor != kRowMoreRules || Memory::Read8(proc + kRulesMoreRulesOffset) != 0;
}

std::string RulesName(const RulesValues& values, uint16_t cursor) {
    if (cursor == kRowTime && values.bytes[0] == kStyleStock) return AccessibilityText::String("rules.stock");
    return AccessibilityText::StringAt("rules.rules_items", cursor);
}

// The value a row shows (MenMainRule03Sel01-03 strip pieces, the digits); empty for the Item Switch / More Rules rows.
std::string RulesValue(const RulesValues& values, uint16_t cursor) {
    const uint8_t style = values.bytes[0];
    switch (cursor) {
    case kRowStyle: return AccessibilityText::StringAt("rules.styles", style);
    case kRowTime: return style == kStyleStock ? std::to_string(values.bytes[7]) : Minutes(values.bytes[1]);
    case kRowHandicap: return AccessibilityText::StringAt("rules.handicap", values.bytes[2]);
    case kRowDamage: {
        const uint8_t ratio = values.bytes[3];
        return std::to_string(ratio / 10) + "." + std::to_string(ratio % 10);
    }
    case kRowStageChoice: return AccessibilityText::StringAt("rules.stage_choice", values.bytes[4]);
    default: return {};
    }
}

// The description the page prints for a row (0x806A81E4, jump table 0x806AE500); nullopt when unknown.
std::optional<uint32_t> RulesDescription(const RulesValues& values, uint16_t cursor) {
    const uint8_t style = values.bytes[0];
    switch (cursor) {
    case kRowStyle:
        if (style == kStyleTime) return 0u;
        if (style == kStyleStock) return 2u;
        if (style == kStyleCoin) return 1u;
        return std::nullopt;
    case kRowTime:
        if (style == kStyleStock) return 5u;
        return values.bytes[1] == 0 ? 4u : 3u;
    case kRowHandicap: return 6u + values.bytes[2];
    case kRowDamage: return 9u;
    case kRowStageChoice: return 10u + values.bytes[4];
    case kRowItemSwitch: return 15u;
    case kRowMoreRules: return 16u;
    default: return std::nullopt;
    }
}

// "Damage Ratio, 1.0, The bigger the number..., 4 of 7".
std::string RulesItem(uint32_t proc) {
    const uint16_t cursor = Memory::Read16(proc + kProcCursorOffset);
    if (cursor >= kRulesItems) return {};
    const RulesValues values = ReadRulesValues(proc);
    const std::string name = RulesName(values, cursor);
    if (name.empty()) return {};
    std::string description;
    if (const auto entry = RulesDescription(values, cursor)) description = MuMsgEntry(PanelMuMsg(proc), *entry);
    const std::string place =
        Place(cursor, kRulesItems, [proc](uint16_t item) { return RulesReachable(proc, item); });
    return AccessibilityText::Join({name, RulesValue(values, cursor), description, place});
}

// ---- More Rules ----

struct MoreRulesValues {
    uint8_t bytes[kMoreRulesValueBytes];
};

MoreRulesValues ReadMoreRulesValues(uint32_t proc) {
    MoreRulesValues values{};
    for (uint32_t i = 0; i < kMoreRulesValueBytes; ++i)
        values.bytes[i] = Memory::Read8(proc + kMoreRulesValuesOffset + i);
    return values;
}

bool MoreRulesReachable(uint32_t proc, uint16_t cursor) {
    if (cursor >= kMoreRulesItems || Memory::Read8(proc + kMoreRulesEnabledOffset + cursor) == 0) return false;
    return cursor != kMoreRulesStageSwitchRow || Memory::Read8(proc + kMoreRulesStageSwitchOffset) != 0;
}

// The digits / infinity sign of Stock Time Limit, OFF / ON (MenMainAddRuleSel02) of the toggles.
std::string MoreRulesValue(const MoreRulesValues& values, uint16_t cursor) {
    if (cursor == 0) return Minutes(values.bytes[0]);
    if (cursor < kMoreRulesStageSwitchRow) {
        const uint8_t value = values.bytes[cursor];
        return value <= 1 ? OnOff(value != 0) : std::string();
    }
    return {};
}

// 17 Stock Time Limit, 18 + value Team Attack, 20 + Pause, 22 + Score Display, 24 + Damage Gauge, 26 Random Stage
// Switch (0x806A8C0C; moveCursor prints the same).
std::optional<uint32_t> MoreRulesDescription(const MoreRulesValues& values, uint16_t cursor) {
    if (cursor == 0) return 17u;
    if (cursor < kMoreRulesStageSwitchRow) {
        if (values.bytes[cursor] > 1) return std::nullopt;
        return 16u + 2u * cursor + values.bytes[cursor];
    }
    if (cursor == kMoreRulesStageSwitchRow) return 26u;
    return std::nullopt;
}

std::string MoreRulesItem(uint32_t proc) {
    const uint16_t cursor = Memory::Read16(proc + kProcCursorOffset);
    if (cursor >= kMoreRulesItems) return {};
    const MoreRulesValues values = ReadMoreRulesValues(proc);
    const std::string name = AccessibilityText::StringAt("rules.more_rules_items", cursor);
    if (name.empty()) return {};
    std::string description;
    if (const auto entry = MoreRulesDescription(values, cursor)) description = MuMsgEntry(PanelMuMsg(proc), *entry);
    const std::string place =
        Place(cursor, kMoreRulesItems, [proc](uint16_t item) { return MoreRulesReachable(proc, item); });
    return AccessibilityText::Join({name, MoreRulesValue(values, cursor), description, place});
}

// ---- Item Switch ----

// The selected place (0x806AB118): row 0 the frequency, row 1 columns 0-1 Turn All, then 7 items per row.
std::optional<uint32_t> ItemPlace(uint32_t proc) {
    const uint32_t row = Memory::Read32(proc + kItemRowOffset);
    const uint32_t column = Memory::Read32(proc + kItemColumnOffset);
    if (column >= kItemColumns) return std::nullopt;
    uint32_t place = 0;
    if (row == 0)
        place = kItemFrequencyPlace;
    else if (row == 1)
        place = column <= 1 ? kItemTurnAllPlace : column;
    else
        place = (row - 1) * kItemColumns + column;
    if (place >= kItemPlaces) return std::nullopt;
    return place;
}

int8_t ItemFrequency(uint32_t proc) { return static_cast<int8_t>(Memory::Read8(proc + kItemFrequencyOffset)); }

// The frequency the row shows, as the info text names it ("MEDIUM").
std::string FrequencyName(uint32_t proc, int8_t frequency) {
    if (frequency < 0 || frequency >= static_cast<int8_t>(kFrequencies)) return {};
    return MuMsgEntry(Memory::Read32(proc + kItemMuMsgOffset), kFrequencyNameEntry + frequency);
}

// "SMASH BALL, On, Final Smash, Allows you to use your Final Smash., 3 of 51"; the frequency row has no label, so it
// starts with its value; Turn All by its caption, "Turn All Off, Turn all items off., 2 of 51".
std::string ItemLine(uint32_t proc, uint32_t place) {
    const uint32_t muMsg = Memory::Read32(proc + kItemMuMsgOffset);
    const std::string position = AccessibilityText::Position(place + 1, kItemPlaces);
    if (place == kItemFrequencyPlace) {
        const int8_t frequency = ItemFrequency(proc);
        const std::string value = FrequencyName(proc, frequency);
        if (value.empty()) return {};
        return AccessibilityText::Join(
            {value, MuMsgEntry(muMsg, kFrequencyDescriptionEntry + frequency), position});
    }
    if (place == kItemTurnAllPlace) {
        const auto on = ShownOn(proc, kItemTurnAllObject);
        std::string description;
        if (on) description = MuMsgEntry(muMsg, *on ? kTurnAllOnEntry : kTurnAllOffEntry);
        return AccessibilityText::Join({TurnAllCaption(on), description, position});
    }
    const uint32_t item = place - kItemFirstPlace;
    const uint32_t number = Memory::Read32(kItemNumbers + item * 4);
    const uint32_t category = Memory::Read32(kItemCategories + item * 4);
    if (number >= kItems || category < kFirstCategoryEntry || category > kLastCategoryEntry) return {};
    const std::string name = MuMsgEntry(muMsg, kItemNameEntry + number);
    if (name.empty()) return {};
    return AccessibilityText::Join({name, ShownValue(proc, kItemFirstObject + item), MuMsgEntry(muMsg, category),
                                    MuMsgEntry(muMsg, kItemDescriptionEntry + number), position});
}

std::string ItemSwitchItem(uint32_t proc) {
    const auto place = ItemPlace(proc);
    return place ? ItemLine(proc, *place) : std::string();
}

// ---- Random Stage Switch ----

struct StageLists {
    uint32_t brawl = 0, melee = 0;
    bool custom = false;
};

std::optional<StageLists> ReadStageLists(uint32_t proc) {
    StageLists lists;
    lists.brawl = Memory::Read32(proc + kStageBrawlCountOffset);
    lists.melee = Memory::Read32(proc + kStageMeleeCountOffset);
    lists.custom = Memory::Read8(proc + kStageCustomShownOffset) != 0;
    if (lists.brawl > kBrawlSlots || lists.melee > kMeleeSlots) return std::nullopt;
    return lists;
}

// The selected place by the game's numbering (0x806AC16C).
std::optional<uint32_t> StagePlace(uint32_t proc) {
    const uint32_t column = Memory::Read32(proc + kStageColumnOffset);
    const uint32_t row = Memory::Read32(proc + kStageRowOffset);
    if (column < kBrawlColumns) return row == 0 ? kStageBrawlTurnAll : (row - 1) * kBrawlColumns + column + 2;
    if (column >= kBrawlColumns + kMeleeColumns) return std::nullopt;
    if (row == 0) return kStageMeleeTurnAll;
    if (row > Memory::Read32(proc + kStageMeleeRowsOffset)) return kStageCustom;
    return (row - 1) * kMeleeColumns + column + 28;
}

// Brawl stage `position` of the list (its slot's stage_name entry) or Melee stage `position`.
std::string StageName(uint32_t proc, bool melee, uint32_t position) {
    const uint32_t muMsg = Memory::Read32(proc + kStageMuMsgOffset);
    if (melee) {
        const uint32_t slot = Memory::Read32(proc + kStageMeleeSlotsOffset + position * 4);
        return slot < kMeleeSlots ? MuMsgEntry(muMsg, kMeleeFirstEntry + slot) : std::string();
    }
    const uint32_t slot = Memory::Read32(proc + kStageBrawlSlotsOffset + position * 4);
    if (slot >= kBrawlSlots) return {};
    return MuMsgEntry(muMsg, Memory::Read32(kBrawlStageEntries + slot * 4));
}

// The switch art object of a place; 0 when the place is not on the screen.
uint32_t StageObject(const StageLists& lists, uint32_t place) {
    if (place == kStageBrawlTurnAll) return kBrawlTurnAllObject;
    if (place == kStageMeleeTurnAll) return kMeleeTurnAllObject;
    if (place == kStageCustom) return lists.custom ? kCustomObject : 0;
    if (place >= kStageFirstBrawl && place < kStageFirstBrawl + lists.brawl)
        return kBrawlFirstObject + place - kStageFirstBrawl;
    if (place >= kStageFirstMelee && place < kStageFirstMelee + lists.melee)
        return kMeleeFirstObject + place - kStageFirstMelee;
    return 0;
}

// "Battlefield, On, 3 of 46". Places count in reading order: the Brawl Turn All and stages, the Melee Turn All and
// stages, then Custom Stages.
std::string StageLine(uint32_t proc, uint32_t place) {
    const auto lists = ReadStageLists(proc);
    if (!lists) return {};
    const uint32_t object = StageObject(*lists, place);
    if (object == 0) return {};
    const size_t total = 2 + lists->brawl + lists->melee + (lists->custom ? 1 : 0);
    std::string name, description;
    size_t index = 0;
    if (place == kStageBrawlTurnAll || place == kStageMeleeTurnAll) {
        // The caption carries the ON / OFF, so the line has no separate value.
        index = place == kStageBrawlTurnAll ? 1 : 2 + lists->brawl;
        return AccessibilityText::Join({TurnAllCaption(ShownOn(proc, object)), AccessibilityText::Position(index, total)});
    }
    if (place == kStageCustom) {
        name = AccessibilityText::String("rules.custom_stages");
        description = MuMsgEntry(Memory::Read32(proc + kStageMuMsgOffset), kCustomStagesEntry);
        index = total;
    } else if (place < kStageFirstMelee) {
        name = StageName(proc, false, place - kStageFirstBrawl);
        index = 2 + (place - kStageFirstBrawl);
    } else {
        name = StageName(proc, true, place - kStageFirstMelee);
        index = 3 + lists->brawl + (place - kStageFirstMelee);
    }
    if (name.empty()) return {};
    return AccessibilityText::Join(
        {name, ShownValue(proc, object), description, AccessibilityText::Position(index, total)});
}

std::string StageSwitchItem(uint32_t proc) {
    const auto place = StagePlace(proc);
    return place ? StageLine(proc, *place) : std::string();
}


// What a Turn All press left showing, from the switch art of its group: "All on", "All off", or, when the game kept a
// stage on (it never leaves no stage to pick), the stages still on.
enum class Group { Items, BrawlStages, MeleeStages };

std::string TurnAllEffect(uint32_t proc, Group group) {
    uint32_t first = kItemFirstObject, count = kItems;
    if (group != Group::Items) {
        const auto lists = ReadStageLists(proc);
        if (!lists) return {};
        const bool melee = group == Group::MeleeStages;
        first = melee ? kMeleeFirstObject : kBrawlFirstObject;
        count = melee ? lists->melee : lists->brawl;
    }
    std::vector<uint32_t> on;
    for (uint32_t i = 0; i < count; ++i) {
        const auto shown = ShownOn(proc, first + i);
        if (!shown) return {};
        if (*shown) on.push_back(i);
    }
    if (on.size() == count) return AccessibilityText::String("rules.all_on");
    if (on.empty()) return AccessibilityText::String("rules.all_off");
    if (group == Group::Items) {
        AccessibilityText::LogOnce("rules: Item Switch's Turn All left items both on and off");
        return {};
    }
    std::string text;
    for (const uint32_t position : on)
        text = AccessibilityText::Join({text, StageName(proc, group == Group::MeleeStages, position)});
    return AccessibilityText::Join({text, OnOff(true)});
}

// ---- Hooks ----

// A page opened: its title (when one is drawn) interrupts, the selected item follows; with no title the item
// interrupts.
void Open(uint32_t proc, uint32_t vtable, bool titled, const char* titleKey, std::string (*item)(uint32_t)) {
    if (AccessibilityText::SilencedByMods()) return;
    try {
        if (!IsPage(proc, vtable)) return;
        const std::string title = titled ? AccessibilityText::String(titleKey) : std::string();
        const std::string line = item(proc);
        Say(title, true);
        Say(line, title.empty());
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "rules page open", e);
    }
}

// Rules opened (previous page 6) or came back from Item Switch (2) or More Rules (1): only an opening has its title.
void RulesEnter(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3], from = ctx->gpr[5];
    func_806A5BEC(ctx);
    bool titled = false;
    try {
        titled = from == kRulesFirstOpening && MainMenuPanel(proc);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "rules title", e);
    }
    Open(proc, kRulesVtable, titled, "rules.titles.rules", RulesItem);
}

// More Rules opened from Rules (previous page 0) or came back from Random Stage Switch (3).
void MoreRulesEnter(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3], from = ctx->gpr[5];
    func_806A84F4(ctx);
    bool titled = false;
    try {
        titled = from == kMoreRulesFromRules && MainMenuPanel(proc);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "more rules title", e);
    }
    Open(proc, kMoreRulesVtable, titled, "rules.titles.more_rules", MoreRulesItem);
}

// Item Switch has no child page: every enter opens it, on the frequency row.
void ItemSwitchEnter(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    func_806AA5E8(ctx);
    Open(proc, kItemSwitchVtable, true, "rules.titles.item_switch", ItemSwitchItem);
}

// Random Stage Switch has no child page: every enter opens it, on the Brawl Turn All button.
void StageSwitchEnter(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    func_806AB724(ctx);
    Open(proc, kStageSwitchVtable, true, "rules.titles.stage_switch", StageSwitchItem);
}

// Runs every input frame of Rules: a new row speaks its item, a changed value its new value.
void RulesMoveCursor(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    uint16_t cursor = 0;
    RulesValues before{};
    bool known = false;
    try {
        if (IsPage(proc, kRulesVtable)) {
            cursor = Memory::Read16(proc + kProcCursorOffset);
            before = ReadRulesValues(proc);
            known = true;
        }
    } catch (const Memory::AccessViolation&) {
    }
    func_806A6F68(ctx);
    if (!known || AccessibilityText::SilencedByMods()) return;
    try {
        if (Memory::Read16(proc + kProcCursorOffset) != cursor) {
            Say(RulesItem(proc), true);
            return;
        }
        const RulesValues after = ReadRulesValues(proc);
        if (std::memcmp(after.bytes, before.bytes, sizeof after.bytes) == 0) return;
        const std::string value = RulesValue(after, cursor);
        if (value == RulesValue(before, cursor)) return;
        Say(ChangedValue(value, PanelMuMsg(proc), RulesDescription(after, cursor), RulesDescription(before, cursor)),
            true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "rules input", e);
    }
}

// Runs every input frame of More Rules.
void MoreRulesMoveCursor(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    uint16_t cursor = 0;
    MoreRulesValues before{};
    bool known = false;
    try {
        if (IsPage(proc, kMoreRulesVtable)) {
            cursor = Memory::Read16(proc + kProcCursorOffset);
            before = ReadMoreRulesValues(proc);
            known = true;
        }
    } catch (const Memory::AccessViolation&) {
    }
    func_806A9498(ctx);
    if (!known || AccessibilityText::SilencedByMods()) return;
    try {
        if (Memory::Read16(proc + kProcCursorOffset) != cursor) {
            Say(MoreRulesItem(proc), true);
            return;
        }
        const MoreRulesValues after = ReadMoreRulesValues(proc);
        if (std::memcmp(after.bytes, before.bytes, sizeof after.bytes) == 0) return;
        const std::string value = MoreRulesValue(after, cursor);
        if (value == MoreRulesValue(before, cursor)) return;
        Say(ChangedValue(value, PanelMuMsg(proc), MoreRulesDescription(after, cursor),
                         MoreRulesDescription(before, cursor)),
            true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "more rules input", e);
    }
}

// Runs every input frame of Item Switch: a new place speaks its line; a frequency change the new frequency; an item's
// A press its new On / Off; a Turn All press what it did.
void ItemSwitchMoveCursor(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    std::optional<uint32_t> place;
    int8_t frequency = 0;
    uint32_t maskHi = 0, maskLo = 0;
    bool known = false;
    try {
        if (IsPage(proc, kItemSwitchVtable)) {
            place = ItemPlace(proc);
            frequency = ItemFrequency(proc);
            maskHi = Memory::Read32(proc + kItemMaskHiOffset);
            maskLo = Memory::Read32(proc + kItemMaskLoOffset);
            known = true;
        }
    } catch (const Memory::AccessViolation&) {
    }
    func_806AAA6C(ctx);
    if (!known || AccessibilityText::SilencedByMods()) return;
    try {
        const auto now = ItemPlace(proc);
        if (!now) return;
        if (now != place) {
            Say(ItemLine(proc, *now), true);
            return;
        }
        if (const int8_t after = ItemFrequency(proc); after != frequency) {
            const uint32_t muMsg = Memory::Read32(proc + kItemMuMsgOffset);
            Say(ChangedValue(FrequencyName(proc, after), muMsg, kFrequencyDescriptionEntry + after,
                             kFrequencyDescriptionEntry + frequency),
                true);
            return;
        }
        if (Memory::Read32(proc + kItemMaskHiOffset) == maskHi && Memory::Read32(proc + kItemMaskLoOffset) == maskLo)
            return;
        if (*now == kItemTurnAllPlace)
            Say(TurnAllEffect(proc, Group::Items), true);
        else if (*now >= kItemFirstPlace)
            Say(ShownValue(proc, kItemFirstObject + *now - kItemFirstPlace), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "item switch input", e);
    }
}

// Runs every input frame of Random Stage Switch: a new place speaks its line; a stage's A press its new On / Off; a
// Turn All press what it did. A refused press (it would leave no stage on) changes nothing and stays silent; the game
// plays its error sound.
void StageSwitchMoveCursor(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    std::optional<uint32_t> place;
    uint32_t maskHi = 0, maskLo = 0;
    bool known = false;
    try {
        if (IsPage(proc, kStageSwitchVtable)) {
            place = StagePlace(proc);
            maskHi = Memory::Read32(proc + kStageMaskHiOffset);
            maskLo = Memory::Read32(proc + kStageMaskLoOffset);
            known = true;
        }
    } catch (const Memory::AccessViolation&) {
    }
    func_806AC16C(ctx);
    if (!known || AccessibilityText::SilencedByMods()) return;
    try {
        const auto now = StagePlace(proc);
        if (!now) return;
        if (now != place) {
            Say(StageLine(proc, *now), true);
            return;
        }
        if (Memory::Read32(proc + kStageMaskHiOffset) == maskHi && Memory::Read32(proc + kStageMaskLoOffset) == maskLo)
            return;
        const auto lists = ReadStageLists(proc);
        if (!lists) return;
        if (*now == kStageBrawlTurnAll)
            Say(TurnAllEffect(proc, Group::BrawlStages), true);
        else if (*now == kStageMeleeTurnAll)
            Say(TurnAllEffect(proc, Group::MeleeStages), true);
        else if (const uint32_t object = StageObject(*lists, *now); object != 0)
            Say(ShownValue(proc, object), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "stage switch input", e);
    }
}

} // namespace RulesSpeech

extern "C" void RulesEnter_806a5bec(CpuContext* ctx) { RulesSpeech::RulesEnter(ctx); }
extern "C" void MoreRulesEnter_806a84f4(CpuContext* ctx) { RulesSpeech::MoreRulesEnter(ctx); }
extern "C" void ItemSwitchEnter_806aa5e8(CpuContext* ctx) { RulesSpeech::ItemSwitchEnter(ctx); }
extern "C" void StageSwitchEnter_806ab724(CpuContext* ctx) { RulesSpeech::StageSwitchEnter(ctx); }
extern "C" void RulesMoveCursor_806a6f68(CpuContext* ctx) { RulesSpeech::RulesMoveCursor(ctx); }
extern "C" void MoreRulesMoveCursor_806a9498(CpuContext* ctx) { RulesSpeech::MoreRulesMoveCursor(ctx); }
extern "C" void ItemSwitchMoveCursor_806aaa6c(CpuContext* ctx) { RulesSpeech::ItemSwitchMoveCursor(ctx); }
extern "C" void StageSwitchMoveCursor_806ac16c(CpuContext* ctx) { RulesSpeech::StageSwitchMoveCursor(ctx); }

REGISTER_NATIVE_FUNCTION_AS(0x806A5BEC, RulesEnter_806a5bec, "RulesEnter_806a5bec");
REGISTER_NATIVE_FUNCTION_AS(0x806A84F4, MoreRulesEnter_806a84f4, "MoreRulesEnter_806a84f4");
REGISTER_NATIVE_FUNCTION_AS(0x806AA5E8, ItemSwitchEnter_806aa5e8, "ItemSwitchEnter_806aa5e8");
REGISTER_NATIVE_FUNCTION_AS(0x806AB724, StageSwitchEnter_806ab724, "StageSwitchEnter_806ab724");
REGISTER_NATIVE_FUNCTION_AS(0x806A6F68, RulesMoveCursor_806a6f68, "RulesMoveCursor_806a6f68");
REGISTER_NATIVE_FUNCTION_AS(0x806A9498, MoreRulesMoveCursor_806a9498, "MoreRulesMoveCursor_806a9498");
REGISTER_NATIVE_FUNCTION_AS(0x806AAA6C, ItemSwitchMoveCursor_806aaa6c, "ItemSwitchMoveCursor_806aaa6c");
REGISTER_NATIVE_FUNCTION_AS(0x806AC16C, StageSwitchMoveCursor_806ac16c, "StageSwitchMoveCursor_806ac16c");
