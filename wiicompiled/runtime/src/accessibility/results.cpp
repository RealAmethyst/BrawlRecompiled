#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Screen-reader speech for the results screen after a Group match (scene scVsResult, task ifVsResultTask in main.dol,
// resident). Two event hooks: the winner line's printer (once per showing) and the task's per-frame processAnim,
// wrapped to compare the task state (the A press that shows the table) and each player's page and top row before and
// after the frame (page changes, ready, back, and scrolling the statistics list).
//
// Numbers are text the game prints from the match result record; place numerals, column labels, the WINNER banner and
// the ready caption are art (mod wording); fighters are portraits, named from the character select's plate list
// through the game's own kind table. A player's own pages are spoken for the humans who hold a controller on this
// screen; with two or more of them, a page line starts with the player's name tag.

extern "C" void func_800E7BD0(CpuContext* ctx);  // ifVsResultTask: print the winner's name
extern "C" void func_800E67A4(CpuContext* ctx);  // ifVsResultTask::processAnim (vtable 0x8045AF08), every frame

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace ResultsSpeech {

// GameGlobal (r13-17216): +0x18 the match result (gmResultInfo), +0x8 the melee mode settings (gmGlobalModeMelee).
constexpr uint32_t kGameGlobalPointer = 0x805A00E0;
constexpr uint32_t kGameGlobalResultOffset = 0x18;
constexpr uint32_t kGameGlobalModeMeleeOffset = 0x8;

// gmResultInfo, as the task reads it (constructor 0x800E5858, build 0x800E9878, printers 0x800E60E4, 0x800E7BD0).
constexpr uint32_t kResultRuleOffset = 0x01;          // u8: 0 time, 1 stock, 2 coin
constexpr uint32_t kResultTeamsOffset = 0x02;         // u8: 1 team battle
constexpr uint32_t kResultSdPenaltyOffset = 0x03;     // s8: time-mode points per self-destruct
constexpr uint32_t kResultPlayersOffset = 0x24;       // gmPlayerResultInfo per player
constexpr uint32_t kResultPlayerBytes = 0x2AC;
constexpr uint32_t kResultTeamPlacesOffset = 0x1304;  // u8 0-based place per team, 0x30 apart (0x800E98E0)
constexpr uint32_t kResultTeamPlaceStride = 0x30;
constexpr uint32_t kResultDecisionOffset = 0x1378;    // u8
constexpr uint8_t kDecisionNoContest = 9;
enum : uint8_t { kRuleTime = 0, kRuleStock = 1, kRuleCoin = 2 };

// gmPlayerResultInfo (the record of player p; after the constructor's 0x800EB314 its statistics hold display units).
constexpr uint32_t kPlayerKindOffset = 0x00;         // u8 gmCharacterKind
constexpr uint32_t kPlayerTeamOffset = 0x06;         // u8: 0 red, 1 blue, 2 green (label colours 0x80408A38)
constexpr uint32_t kPlayerKoTimeHiddenOffset = 0x0A; // s8: above 0 shows the KO'd column as "--:--" (0x800E6318)
constexpr uint32_t kPlayerPlaceOffset = 0x0E;        // u8 0-based place
constexpr uint32_t kPlayerKosOffset = 0x10;          // u32
constexpr uint32_t kPlayerFallsOffset = 0x14;        // u32
constexpr uint32_t kPlayerSdsOffset = 0x18;          // u16
constexpr uint32_t kPlayerFinalCoinsOffset = 0x28;   // u32
constexpr uint32_t kPlayerCoinsOffset = 0x2C;        // u32: coins collected
constexpr uint32_t kPlayerLostCoinsOffset = 0x30;    // u32
constexpr uint32_t kPlayerKoKindsOffset = 0x34;      // u8 gmCharacterKind per fighter this player KO'd
constexpr uint32_t kPlayerFallKindsOffset = 0xFC;    // u8 gmCharacterKind per fighter that KO'd this player
constexpr uint32_t kPlayerStatsOffset = 0x1C0;       // u32 statistic k at +4k (k 3-10, 12-24)
constexpr uint32_t kPlayerKoCountOffset = 0x1C4;     // u32: statistic 0, and the KO list's length
constexpr uint32_t kPlayerFallCountOffset = 0x1C8;   // u32: statistic 1, and the falls list's length
constexpr uint32_t kPlayerHitRateOffset = 0x1EC;     // u8: statistic 11
constexpr uint32_t kPlayerTeamAttacksOffset = 0x224; // u32
constexpr uint32_t kPlayerDroppedCoinsOffset = 0x228; // u32
constexpr uint32_t kPlayerKoFrameOffset = 0x254;     // u32: the frame the player was KO'd (stock)
constexpr uint32_t kMaxListed = 100;                 // the KO page draws at most 100 icons per list (0x800E88CC)

// ifVsResultTask (constructor 0x800E5858, processAnim 0x800E67A4, per-player handler 0x800EB684).
constexpr uint32_t kTaskStateOffset = 0x40;        // u32: 3 waiting for A, 4 the table and the players' pages
constexpr uint32_t kTaskPageOffset = 0x48;         // u32 per player: 0 summary, 1 KOs, 2 statistics, 3 ready
constexpr uint32_t kTaskTopRowOffset = 0x98;       // u32 per player: the statistics list's top line
constexpr uint32_t kTaskPlayerStateOffset = 0xA8;  // u32 per player: 0 human, 1 CPU, 3 empty
constexpr uint32_t kTaskWinnerOffset = 0xB8;       // u32: the winning player
constexpr uint32_t kTaskWinningTeamOffset = 0xBC;  // u32
constexpr uint32_t kTaskOwnPadOffset = 0xC1;       // u8 mask: humans who read their own controller here
constexpr uint32_t kTaskStaminaOffset = 0xD4;      // u8: Special Brawl stamina
constexpr uint32_t kTaskRowCountOffset = 0x4438;   // u32 per player: lines of the statistics list (0x800E8F1C)
constexpr uint32_t kTaskMessagesOffset = 0x4448;   // the loaded /menu/msg/vsres.msbin (0x800E68F8, 0x800EA92C)
constexpr uint32_t kPlayers = 4;
enum : uint32_t { kStateWaiting = 3, kStateTable = 4 };
enum : uint32_t { kPageSummary = 0, kPageKos = 1, kPageStatistics = 2, kPageReady = 3 };
enum : uint32_t { kPlayerHuman = 0, kPlayerEmpty = 3 };
constexpr uint32_t kMaxRows = 58;  // 50, coin 56, team battle +2
constexpr uint32_t kVisibleAtEnd = 9;  // lines from the last top line to the list's end

// gmGlobalModeMelee: 0x5C-byte player entries from +0x98; the result's label uses the first whose byte +1 is not 3
// and byte +3 is the player, and its name (5 UTF-16 units and a NUL) at +0xC when not empty (0x800EA794).
constexpr uint32_t kModeMeleePlayersOffset = 0x98;
constexpr uint32_t kModeMeleePlayerBytes = 0x5C;
constexpr uint32_t kModeMeleeUnusedOffset = 0x1;
constexpr uint8_t kModeMeleeUnused = 3;
constexpr uint32_t kModeMeleeSlotOffset = 0x3;
constexpr uint32_t kModeMeleeNameOffset = 0xC;
constexpr uint32_t kNameUnits = 6;

// main.dol data. The glyph table's strings are what the numbers and labels are built from: 0-9 the full-width digits,
// 10 plus, 11 minus, 13 "P", 14 "CPU", 17 colon, 18 the ideographic space (0x800E636C, 0x800E6608, 0x800EA88C).
constexpr uint32_t kGlyphTable = 0x8045B674;
enum : uint32_t { kGlyphPlus = 10, kGlyphMinus = 11, kGlyphP = 13, kGlyphCpu = 14, kGlyphColon = 17, kGlyphSpace = 18 };
constexpr uint32_t kNoContestPointer = 0x8059E348;  // r13-24792: "NO CONTEST"
constexpr uint32_t kTeamNames = 0x8045B650;         // English and Japanese name per team: "RED TEAM", ... (8 bytes)
constexpr uint32_t kTeamNameStride = 8;
constexpr uint32_t kTeams = 3;
// exchangeGmCharacterKind2MuSelchkind 0x800AF708: the first of 40 16-byte records (MuSelchkind order) whose byte 0 or
// 1 is the gmCharacterKind; 40 when none. For every playable kind its name matches the one the winner line prints.
constexpr uint32_t kSelchkindTable = 0x80455458;
constexpr uint32_t kSelchkindRecordBytes = 16;
constexpr uint32_t kSelchkinds = 40;
constexpr uint8_t kNoKind = 0xFF;  // the table's filler byte, never a fighter
// The statistics' unit entries (s32 vsres entry, -1 none), by statistic (0x800E8F98, read at 0x800E94CC).
constexpr uint32_t kUnitTable = 0x8040898C;
constexpr uint32_t kUnitEntries = 29;
constexpr int32_t kNoUnit = -1;
constexpr int32_t kPercentUnit = 38;
constexpr uint32_t kMaxUnitBytes = 16;

// vsres entries: 9 + k statistic k's label; 50 and 51 the stamina labels of statistics 0 and 1; 35-37 the coin
// labels, which replace statistics 25-27 outside team battle (0x800E9050-0x800E93D4).
constexpr uint32_t kStatLabelEntry = 9;
constexpr uint32_t kStaminaKosEntry = 50;
constexpr uint32_t kStaminaFallsEntry = 51;
constexpr uint32_t kCoinLabelEntry = 35;
constexpr uint32_t kCoinStatistic = 25;
constexpr uint32_t kSelfDestructStatistic = 2;
constexpr uint32_t kStaminaBareStatistics = 7;  // statistics below 7 drop their "%" in stamina mode

// Numbers are capped at 99999 (0x800E636C) and KO frames at 99:59 (0x800E6608).
constexpr uint32_t kMaxNumber = 99999;
constexpr uint32_t kMaxKoFrame = 359999;

uint32_t ResultInfo() {
    return Memory::Read32(Memory::Read32(kGameGlobalPointer) + kGameGlobalResultOffset);
}

uint32_t Record(uint32_t result, uint32_t player) {
    return result + kResultPlayersOffset + player * kResultPlayerBytes;
}

uint32_t PlayerState(uint32_t task, uint32_t player) {
    return Memory::Read32(task + kTaskPlayerStateOffset + player * 4);
}

bool NoContest(uint32_t result) { return Memory::Read8(result + kResultDecisionOffset) == kDecisionNoContest; }

bool Stamina(uint32_t task) { return Memory::Read8(task + kTaskStaminaOffset) != 0; }

std::string Glyph(uint32_t index) {
    return AccessibilityText::GuestCString(Memory::Read32(kGlyphTable + index * 4), 16);
}

std::string Fill(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
    std::string text = AccessibilityText::String(key);
    for (const auto& [field, value] : fields)
        if (const size_t at = text.find(field); at != std::string::npos) text.replace(at, field.size(), value);
    return text;
}

// "KOs +3": a column or statistic and its value.
std::string Entry(const std::string& label, const std::string& value) {
    if (label.empty() || value.empty()) return {};
    return Fill("results.entry", {{"{label}", label}, {"{value}", value}});
}

// The game's all-capitals names ("NO CONTEST", "RED TEAM") in the title case the layer speaks for all-capitals text.
std::string TitleCase(std::string text) {
    bool wordStart = true;
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z' && !wordStart) c = static_cast<char>(c - 'A' + 'a');
        wordStart = c == ' ';
    }
    return text;
}

// The plate name (character_select.fighters) of a gmCharacterKind; empty when the kind has no plate.
std::string FighterName(uint8_t kind) {
    if (kind == kNoKind) return {};
    for (uint32_t i = 0; i < kSelchkinds; ++i) {
        const uint32_t record = kSelchkindTable + i * kSelchkindRecordBytes;
        if (Memory::Read8(record) == kind || Memory::Read8(record + 1) == kind)
            return AccessibilityText::StringAt("character_select.fighters", i);
    }
    AccessibilityText::LogOnce("results: fighter kind without a name " + std::to_string(kind));
    return {};
}

std::string TeamName(uint32_t team) {
    if (team >= kTeams) return {};
    return TitleCase(AccessibilityText::GuestCString(Memory::Read32(kTeamNames + team * kTeamNameStride), 32));
}

// The player's name tag as printed (0x800EA690-0x800EA8E8): the entered name, else "P" and the player's number; "CPU".
std::string Label(uint32_t task, uint32_t player) {
    const uint32_t state = PlayerState(task, player);
    if (state == kPlayerEmpty) return {};
    if (state != kPlayerHuman) return Glyph(kGlyphCpu);
    const uint32_t mode = Memory::Read32(Memory::Read32(kGameGlobalPointer) + kGameGlobalModeMeleeOffset);
    for (uint32_t i = 0; i < kPlayers; ++i) {
        const uint32_t entry = mode + kModeMeleePlayersOffset + i * kModeMeleePlayerBytes;
        if (Memory::Read8(entry + kModeMeleeUnusedOffset) == kModeMeleeUnused ||
            Memory::Read8(entry + kModeMeleeSlotOffset) != player)
            continue;
        if (Memory::Read16(entry + kModeMeleeNameOffset) != 0)
            return AccessibilityText::GuestUtf16(entry + kModeMeleeNameOffset, kNameUnits);
        break;
    }
    const std::string p = Glyph(kGlyphP), digit = Glyph(player + 1);
    if (p.empty() || digit.empty()) return {};
    return p + digit;
}

// The place card's numeral (model InfResultRank{place}, 0x800E9B10-0x800E9B54): the team's place in team battle.
// 0 when it is not one of the four numerals.
uint32_t Place(uint32_t result, uint32_t record) {
    uint32_t place = 0;
    const uint8_t teams = Memory::Read8(result + kResultTeamsOffset);
    if (teams == 0) {
        place = Memory::Read8(record + kPlayerPlaceOffset) + 1u;
    } else if (teams == 1) {
        const uint8_t team = Memory::Read8(record + kPlayerTeamOffset);
        if (team < kTeams)
            place = Memory::Read8(result + kResultTeamPlacesOffset + team * kResultTeamPlaceStride) + 1u;
    }
    return place >= 1 && place <= kPlayers ? place : 0;
}

// A number as 0x800E636C prints it: capped, then a plus or minus sign unless it is 0.
std::string Number(uint32_t value, bool plus) {
    value = std::min(value, kMaxNumber);
    if (value == 0) return "0";
    const std::string sign = Glyph(plus ? kGlyphPlus : kGlyphMinus);
    if (sign.empty()) return {};
    return sign + std::to_string(value);
}

// The KO'd column (0x800E6608): minutes and seconds of the KO frame, the minutes' tens blank when 0; "--:--" when the
// game shows no time.
std::string KoTime(uint32_t frames, bool shown) {
    const std::string colon = Glyph(kGlyphColon);
    if (colon.empty()) return {};
    if (!shown) {
        const std::string dash = Glyph(kGlyphMinus);
        if (dash.empty()) return {};
        return dash + dash + colon + dash + dash;
    }
    frames = std::min(frames, kMaxKoFrame);
    const uint32_t minutes = frames / 3600, seconds = frames / 60 % 60;
    return (minutes >= 10 ? std::to_string(minutes / 10) : Glyph(kGlyphSpace)) + std::to_string(minutes % 10) + colon +
           std::to_string(seconds / 10) + std::to_string(seconds % 10);
}

// The numbers under the column labels (0x800E60E4) with the labels the rule shows (InfResultHitnum frame,
// 0x800EA1EC): "KOs +3", "Falls -1", ... Empty when any part cannot be read.
std::vector<std::string> Columns(uint32_t task, uint32_t result, uint32_t record) {
    const auto label = [](std::string_view key) {
        return AccessibilityText::String("results.columns." + std::string(key));
    };
    const uint32_t kos = Memory::Read32(record + kPlayerKosOffset), falls = Memory::Read32(record + kPlayerFallsOffset);
    std::vector<std::string> columns;
    if (Stamina(task)) {
        columns = {Entry(label("finishes"), Number(kos, true)), Entry(label("misses"), Number(falls, false)),
                   Entry(label("total"), kos < falls ? Number(falls - kos, false) : Number(kos - falls, true))};
    } else {
        const uint8_t rule = Memory::Read8(result + kResultRuleOffset);
        if (rule == kRuleCoin) {
            const uint32_t coins = Memory::Read32(record + kPlayerCoinsOffset);
            const uint32_t total = Memory::Read32(record + kPlayerFinalCoinsOffset);
            columns = {Entry(label("coins_collected"), Number(coins, true)),
                       Entry(label("coins_lost"), Number(coins - total, false)), Entry(label("total"), Number(total, true))};
        } else if (rule == kRuleTime || rule == kRuleStock) {
            const uint32_t sds = Memory::Read16(record + kPlayerSdsOffset);
            const auto penalty = static_cast<int8_t>(Memory::Read8(result + kResultSdPenaltyOffset));
            const uint32_t netFalls = falls - sds;
            const uint32_t sdPoints = 0u - sds * static_cast<uint32_t>(static_cast<int32_t>(penalty));
            columns = {Entry(label("kos"), Number(kos, true)), Entry(label("falls"), Number(netFalls, false)),
                       Entry(label("self_destructs"), Number(sdPoints, false))};
            if (rule == kRuleTime) {
                const uint32_t lost = netFalls + sdPoints;
                columns.push_back(Entry(label("total"), kos < lost ? Number(lost - kos, false)
                                                                   : Number(kos - netFalls - sdPoints, true)));
            } else {
                const auto hidden = static_cast<int8_t>(Memory::Read8(record + kPlayerKoTimeHiddenOffset));
                const bool shown = hidden <= 0 && !NoContest(result);
                const uint32_t koFrame = Memory::Read32(record + kPlayerKoFrameOffset);
                columns.push_back(Entry(label("knocked_out_at"), KoTime(koFrame, shown)));
            }
        }
    }
    for (const std::string& column : columns)
        if (column.empty()) return {};
    return columns;
}

// A player's line of the table: "1 P1, Red Team, Mario, KOs +3, Falls -1, SDs 0, Total +2". No Contest has no places.
std::string Row(uint32_t task, uint32_t player) {
    if (PlayerState(task, player) == kPlayerEmpty) return {};
    const uint32_t result = ResultInfo(), record = Record(result, player);
    const std::string label = Label(task, player);
    const std::string fighter = FighterName(Memory::Read8(record + kPlayerKindOffset));
    if (label.empty() || fighter.empty()) return {};
    std::string lead = label;
    if (!NoContest(result)) {
        const uint32_t place = Place(result, record);
        if (place == 0) return {};
        lead = Fill("results.place", {{"{place}", std::to_string(place)}, {"{label}", label}});
    }
    std::string team;
    if (Memory::Read8(result + kResultTeamsOffset) != 0) {
        team = TeamName(Memory::Read8(record + kPlayerTeamOffset));
        if (team.empty()) return {};
    }
    const std::vector<std::string> columns = Columns(task, result, record);
    if (columns.empty()) return {};
    std::string text = AccessibilityText::Join({lead, team, fighter});
    for (const std::string& column : columns) text = AccessibilityText::Join({text, column});
    return text;
}

uint32_t Messages(uint32_t task) { return Memory::Read32(task + kTaskMessagesOffset); }

// A vsres entry exactly as stored, for the units the game appends to a number (" sec.", "%"): the text pipeline would
// drop the leading space that separates them. Units hold no markup; any control byte fails closed.
std::optional<std::string> RawUnit(uint32_t messages, uint32_t index) {
    const uint32_t tableBytes = Memory::Read32(messages);
    if (tableBytes < 4 || tableBytes > 0x10000 || tableBytes % 4 != 0 || index >= tableBytes / 4) return std::nullopt;
    const uint32_t start = Memory::Read32(messages + index * 4);
    const uint32_t end = index + 1 < tableBytes / 4 ? Memory::Read32(messages + (index + 1) * 4) : start + kMaxUnitBytes;
    if (start < tableBytes || end < start || end - start > kMaxUnitBytes) return std::nullopt;
    std::string raw;
    for (uint32_t offset = start; offset < end; ++offset) {
        const uint8_t c = Memory::Read8(messages + offset);
        if (c == 0) break;
        if (c < 0x20) return std::nullopt;
        raw += static_cast<char>(c);
    }
    return raw;
}

// Statistic k's label as the list prints it (the label line 2k).
std::string StatLabel(uint32_t task, uint32_t result, uint32_t k) {
    const bool teams = Memory::Read8(result + kResultTeamsOffset) == 1;
    uint32_t entry = kStatLabelEntry + k;
    if (k >= kCoinStatistic && !teams)
        entry = kCoinLabelEntry + (k - kCoinStatistic);
    else if (Stamina(task) && k == 0)
        entry = kStaminaKosEntry;
    else if (Stamina(task) && k == 1)
        entry = kStaminaFallsEntry;
    return AccessibilityText::MessageEntry(Messages(task), entry);
}

// Statistic k's value line (0x800E93DC-0x800E9604): "%d" and its unit.
std::string StatValue(uint32_t task, uint32_t result, uint32_t record, uint32_t k) {
    int32_t value = 0;
    if (k == 0) {
        value = static_cast<int32_t>(Memory::Read32(record + kPlayerKoCountOffset));
    } else if (k == 1) {
        value = static_cast<int32_t>(Memory::Read32(record + kPlayerFallCountOffset));
    } else if (k == kSelfDestructStatistic) {
        value = Memory::Read16(record + kPlayerSdsOffset);
    } else if (k == 11) {
        value = Memory::Read8(record + kPlayerHitRateOffset);
    } else if (k < kCoinStatistic) {
        value = static_cast<int32_t>(Memory::Read32(record + kPlayerStatsOffset + k * 4));
    } else {
        // Team battle puts Team Attacks at 25 and moves the coin statistics one down.
        uint32_t coin = k;
        const bool teams = Memory::Read8(result + kResultTeamsOffset) == 1;
        if (teams && k == kCoinStatistic) {
            value = static_cast<int32_t>(Memory::Read32(record + kPlayerTeamAttacksOffset));
        } else {
            if (teams) --coin;
            uint32_t offset = 0;
            if (coin == kCoinStatistic) offset = kPlayerCoinsOffset;
            else if (coin == kCoinStatistic + 1) offset = kPlayerDroppedCoinsOffset;
            else if (coin == kCoinStatistic + 2) offset = kPlayerLostCoinsOffset;
            else return {};
            value = static_cast<int32_t>(Memory::Read32(record + offset));
        }
    }
    if (k >= kUnitEntries) return {};
    const auto unit = static_cast<int32_t>(Memory::Read32(kUnitTable + k * 4));
    std::string raw = std::to_string(value);
    if (unit != kNoUnit && !(unit == kPercentUnit && k < kStaminaBareStatistics && Stamina(task))) {
        const auto text = RawUnit(Messages(task), static_cast<uint32_t>(unit));
        if (!text) {
            AccessibilityText::LogOnce("results: unit entry unreadable");
            return {};
        }
        raw += *text;
    }
    return AccessibilityText::Prepare(raw);
}

// Statistic k of the player's list: "KOs 3", "Ground Time 30 sec.". Empty past the list's end.
std::string Statistic(uint32_t task, uint32_t player, uint32_t k) {
    const uint32_t rows = Memory::Read32(task + kTaskRowCountOffset + player * 4);
    if (rows > kMaxRows || rows % 2 != 0 || 2 * k + 1 >= rows) return {};
    const uint32_t result = ResultInfo();
    return Entry(StatLabel(task, result, k), StatValue(task, result, Record(result, player), k));
}

// The fighters in one of the KO page's icon lists, or its value line ("0") when the list is empty. A kind without a
// plate name is left out: the icon the game draws for it is Mario's (0x800AF5B4 falls back to stock kind 0), which
// would name the wrong fighter.
std::string KoList(uint32_t task, uint32_t result, uint32_t record, uint32_t kindsOffset, uint32_t countOffset,
                   uint32_t statistic) {
    const uint32_t count = std::min(Memory::Read32(record + countOffset), kMaxListed);
    if (count == 0) return StatValue(task, result, record, statistic);
    std::string names;
    for (uint32_t i = 0; i < count; ++i)
        names = AccessibilityText::Join({names, FighterName(Memory::Read8(record + kindsOffset + i))});
    return names;
}

// The KO page (0x800E88CC): the KOs label and whom the player KO'd, the Falls label and who KO'd them, then the
// Self-Destructs line.
std::string KoPage(uint32_t task, uint32_t player) {
    const uint32_t result = ResultInfo(), record = Record(result, player);
    const std::string kosLabel = StatLabel(task, result, 0), fallsLabel = StatLabel(task, result, 1);
    const std::string kos = KoList(task, result, record, kPlayerKoKindsOffset, kPlayerKoCountOffset, 0);
    const std::string falls = KoList(task, result, record, kPlayerFallKindsOffset, kPlayerFallCountOffset, 1);
    const std::string selfDestructs = Statistic(task, player, kSelfDestructStatistic);
    if (kosLabel.empty() || fallsLabel.empty() || kos.empty() || falls.empty() || selfDestructs.empty()) return {};
    return Fill("results.ko_page", {{"{kos_label}", kosLabel}, {"{kos}", kos}, {"{falls_label}", fallsLabel},
                                    {"{falls}", falls}, {"{self_destructs}", selfDestructs}});
}

// The first line the statistics list can show at the top once scrolled to its end (0x800ECBC4 caps the scroll at
// rows - 9 lines), or none for an unreadable count.
std::optional<uint32_t> LastTop(uint32_t task, uint32_t player) {
    const uint32_t rows = Memory::Read32(task + kTaskRowCountOffset + player * 4);
    if (rows > kMaxRows || rows % 2 != 0 || rows < kVisibleAtEnd) return std::nullopt;
    return rows - kVisibleAtEnd;
}

// The statistics below the top line that stay on screen at the list's end: their label lines follow the top line.
std::vector<std::string> RestOfList(uint32_t task, uint32_t player, uint32_t top) {
    std::vector<std::string> rest;
    const uint32_t rows = Memory::Read32(task + kTaskRowCountOffset + player * 4);
    for (uint32_t k = top / 2 + 1; 2 * k + 1 < rows && 2 * k + 1 < kMaxRows; ++k) {
        const std::string statistic = Statistic(task, player, k);
        if (statistic.empty()) return {};
        rest.push_back(statistic);
    }
    return rest;
}

// What a player's page shows: the summary row on returning to it, the KO page, the statistic at the list's top, the
// ready caption. With two or more players on their own controllers, the player's tag leads (the row has it already).
std::string Page(uint32_t task, uint32_t player, uint32_t page, uint32_t top) {
    std::string text;
    switch (page) {
    case kPageSummary: return Row(task, player);
    case kPageKos: text = KoPage(task, player); break;
    case kPageStatistics: text = Statistic(task, player, top / 2); break;
    case kPageReady: text = AccessibilityText::String("results.ready"); break;
    default: return {};
    }
    if (text.empty()) return {};
    if (__builtin_popcount(Memory::Read8(task + kTaskOwnPadOffset) & 0xFu) < 2) return text;
    const std::string label = Label(task, player);
    if (label.empty()) return {};
    return AccessibilityText::Join({label, text});
}

// The winner line (0x800E7BD0): No Contest, the winning team, or the winner's fighter.
std::string WinnerName(uint32_t task) {
    const uint32_t result = ResultInfo();
    if (NoContest(result))
        return TitleCase(AccessibilityText::GuestCString(Memory::Read32(kNoContestPointer), 32));
    if (Memory::Read8(result + kResultTeamsOffset) == 0) {
        const uint32_t winner = Memory::Read32(task + kTaskWinnerOffset);
        if (winner >= kPlayers) return {};
        return FighterName(Memory::Read8(Record(result, winner) + kPlayerKindOffset));
    }
    return TeamName(Memory::Read32(task + kTaskWinningTeamOffset));
}

// The table appeared: every player's row in place order (players in order on a tie, and for No Contest), the first
// interrupting.
void SpeakTable(uint32_t task) {
    const uint32_t result = ResultInfo();
    const bool noContest = NoContest(result);
    std::vector<std::pair<uint32_t, uint32_t>> order;  // place, player
    for (uint32_t player = 0; player < kPlayers; ++player) {
        if (PlayerState(task, player) == kPlayerEmpty) continue;
        order.emplace_back(noContest ? 0 : Place(result, Record(result, player)), player);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    bool first = true;
    for (const auto& [place, player] : order) {
        const std::string row = Row(task, player);
        if (row.empty()) {
            AccessibilityText::LogOnce("results: a table row could not be read");
            continue;
        }
        Speech::Say(row, first);
        first = false;
    }
}

struct TaskFrame {
    uint32_t state = 0;
    uint32_t pages[kPlayers] = {};
    uint32_t tops[kPlayers] = {};
};

bool ReadFrame(uint32_t task, TaskFrame& frame) {
    if (!Memory::TryRead32(task + kTaskStateOffset, frame.state)) return false;
    for (uint32_t i = 0; i < kPlayers; ++i)
        if (!Memory::TryRead32(task + kTaskPageOffset + i * 4, frame.pages[i]) ||
            !Memory::TryRead32(task + kTaskTopRowOffset + i * 4, frame.tops[i]))
            return false;
    return true;
}

} // namespace ResultsSpeech

// The winner's name was printed (once per showing): "Winner, Mario". On No Contest the game switches the panel to
// InfResultPanel_TopN__0 (0x800E6CA4-0x800E6CB8), whose key 1 gives every WINNER letter the blank InfResultNameW.0, so
// only "No Contest" is spoken.
extern "C" void ResultsWinner_800e7bd0(CpuContext* ctx) {
    using namespace ResultsSpeech;
    const uint32_t task = ctx->gpr[3];
    func_800E7BD0(ctx);
    if (AccessibilityText::SilencedByMods()) return;
    try {
        const std::string name = WinnerName(task);
        if (name.empty()) return;
        if (NoContest(ResultInfo())) {
            Speech::Say(name, true);
            return;
        }
        const std::string winner = AccessibilityText::String("results.winner");
        if (!winner.empty()) Speech::Say(AccessibilityText::Join({winner, name}), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "results winner", e);
    }
}

// Every frame: the A press that shows the table (state 3 to 4), then each controller-holding player's page changes,
// the statistics list's top statistic, and on reaching the list's end the rest of it. Other players' pages follow
// theirs and stay silent.
extern "C" void ResultsFrame_800e67a4(CpuContext* ctx) {
    using namespace ResultsSpeech;
    const uint32_t task = ctx->gpr[3];
    TaskFrame before;
    const bool known = ReadFrame(task, before);
    func_800E67A4(ctx);
    if (!known || AccessibilityText::SilencedByMods()) return;
    try {
        TaskFrame after;
        if (!ReadFrame(task, after)) return;
        if (before.state == kStateWaiting && after.state == kStateTable) {
            SpeakTable(task);
            return;
        }
        if (before.state != kStateTable) return;
        const uint8_t own = Memory::Read8(task + kTaskOwnPadOffset);
        bool first = true;
        const auto say = [&first](const std::string& text) {
            if (text.empty()) return;
            Speech::Say(text, first);
            first = false;
        };
        for (uint32_t player = 0; player < kPlayers; ++player) {
            if ((own & (1u << player)) == 0) continue;
            const uint32_t page = after.pages[player], top = after.tops[player];
            const uint32_t topBefore = before.tops[player];
            const bool turned = page != before.pages[player];
            if (!turned && (page != kPageStatistics || top == topBefore)) continue;
            // The top-row rule: a new page, or a new statistic at the list's top.
            if (turned || top / 2 != topBefore / 2) say(Page(task, player, page, top));
            if (page != kPageStatistics) continue;
            // Reaching the list's end (once, not on every frame there): the statistics still on screen below the top.
            const auto lastTop = LastTop(task, player);
            if (!lastTop || top != *lastTop || (!turned && topBefore == *lastTop)) continue;
            for (const std::string& statistic : RestOfList(task, player, top)) say(statistic);
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "results frame", e);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x800E7BD0, ResultsWinner_800e7bd0, "ResultsWinner_800e7bd0");
REGISTER_NATIVE_FUNCTION_AS(0x800E67A4, ResultsFrame_800e67a4, "ResultsFrame_800e67a4");
