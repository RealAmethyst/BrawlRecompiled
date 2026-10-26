#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// Screen-reader speech for the character select (scene scSelChar, muSelCharTask in sora_menu_sel_char, resident).
// Each human player moves a hand and carries a token; the token's position picks the fighter, and the hand points at
// the top bar and the player cards. Event hooks on the game's own functions: the screen's init (it opens), the
// fighter setter (the preview under a carried token changed), the grid's A press (a token was dropped), the setters
// for player kind, costume, team, Zelda/Sheik and the Pokémon choice, team battle, the rule value and the ready
// banner, and the card lists (CPU level, handicap, names). What a hand points at is stored by one small function every
// frame; it is wrapped and the stored target compared.
//
// Lines name the acting player ("P2, ...") only while two or more humans are on the screen, and another player's card
// by its place ("Card 3, ..."). The top bar speaks in the Group modes (selection modes 0-3: mode name, rules line,
// Back, Rules, the arrows and the ready banner) and in the Solo modes (4-11: the mode's title, Back, the ready banner,
// and the difficulty or level, stock, Home-Run and Multi-Man options with their setters). The Wi-Fi variants show
// other titles and buttons, not mapped yet, and stay silent there.

extern "C" void func_80682EB4(CpuContext* ctx);  // muSelCharTask: init, once per opening
extern "C" void func_8069DCD4(CpuContext* ctx);  // muSelCharHand: store the hit test (what the hand points at)
extern "C" void func_80696F60(CpuContext* ctx);  // muSelCharPlayerArea::setCharKind
extern "C" void func_8068ADC4(CpuContext* ctx);  // muSelCharTask: A in the fighter grid (pick up or drop a token)
extern "C" void func_8068BE7C(CpuContext* ctx);  // muSelCharTask: set a card's player kind
extern "C" void func_8069A22C(CpuContext* ctx);  // muSelCharPlayerArea: next costume
extern "C" void func_8069A340(CpuContext* ctx);  // muSelCharPlayerArea: previous costume
extern "C" void func_8069987C(CpuContext* ctx);  // muSelCharPlayerArea: next team
extern "C" void func_80699BC4(CpuContext* ctx);  // muSelCharPlayerArea: previous team
extern "C" void func_80694934(CpuContext* ctx);  // muSelCharPlayerArea: choose Zelda or Sheik
extern "C" void func_80694A04(CpuContext* ctx);  // muSelCharPlayerArea: choose the Pokémon Trainer's Pokémon
extern "C" void func_8068ED8C(CpuContext* ctx);  // muSelCharTask: set team battle (and the mode name)
extern "C" void func_8068F99C(CpuContext* ctx);  // muSelCharTask: rule value up
extern "C" void func_8068FBD0(CpuContext* ctx);  // muSelCharTask: rule value down
extern "C" void func_8069186C(CpuContext* ctx);  // muSelCharTask: show or hide the ready banner
extern "C" void func_8069F240(CpuContext* ctx);  // MuSelctChrList: open
extern "C" void func_8069FE88(CpuContext* ctx);  // MuSelctChrList: process (input while open)
extern "C" void func_8069301C(CpuContext* ctx);  // muSelCharTask: run the Rules screen opened from here
extern "C" void func_8068A358(CpuContext* ctx);  // muSelCharTask: a hand's top-bar input (task, card, pad)
extern "C" void func_80692298(CpuContext* ctx);  // muSelCharTask: show the difficulty or level (Solo)
extern "C" void func_806923A8(CpuContext* ctx);  // muSelCharTask: show the stock (Classic)
extern "C" void func_80691DA4(CpuContext* ctx);  // muSelCharTask: show the Home-Run play order
extern "C" void func_80692048(CpuContext* ctx);  // muSelCharTask: show the Multi-Man type

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace CharacterSelectSpeech {

// muSelCharTask (verified in sora_menu_sel_char).
constexpr uint32_t kTaskAreasOffset = 0x44;        // muSelCharPlayerArea* per card
constexpr uint32_t kTaskAreaCountOffset = 0x648;   // u32
constexpr uint32_t kMaxAreas = 4;
constexpr uint32_t kTaskIconKindsOffset = 0x70;    // u32 MuSelchkind per grid icon, in layout order
constexpr uint32_t kTaskIconCountOffset = 0x138;   // u32; fighters (at most 49) then Random
constexpr uint32_t kMaxIcons = 50;
constexpr uint32_t kTaskTeamBattleOffset = 0x5C8;  // u8
constexpr uint32_t kTaskStaminaOffset = 0x5D0;     // u8: Special Brawl stamina (handicap list shows HP)
constexpr uint32_t kTaskRuleKindOffset = 0x5D4;    // u32: 0 time, 1 stock, 2 coin
constexpr uint32_t kTaskTimeOffset = 0x5D8;        // u32 minutes, 0 infinite
constexpr uint32_t kTaskStockOffset = 0x5DC;       // u32
constexpr uint32_t kTaskLoserOutOffset = 0x620;    // u32: Rotation's winner out (0) / loser out
// The Rules screen opened from here (muRuleTask at +0x62C): its state, 1 open, 2 closing, 3 waiting, 0 closed
// (0x80692FB4 opens; 0x8069301C runs it from processDefault while non-zero and stores 0 after gfTask::exit).
constexpr uint32_t kTaskRulesStateOffset = 0x630;
// The selection mode: u8 at *(GameGlobal+0x10)+4 (getter 0x80683B28). Written by sora_scene's sequences: 0 Brawl
// (sqVsMelee), 1 Rotation (sqQuMelee, inferred), 2 Special Brawl (sqSpMelee), 3 Tourney (sqToMelee); the Solo modes
// 4 Classic (sqSingleSimple), 5 All-Star, 6 Boss Battles, 7 Target Smash, 8 Home-Run Contest, 9 Multi-Man Brawl,
// 10 Training, 11 Events. The module groups 4-11 together everywhere (0x80691DD8..0x80691E58).
constexpr uint32_t kGameGlobalPointer = 0x805A00E0;
constexpr uint32_t kGameGlobalSelCharDataOffset = 0x10;
constexpr uint32_t kSelCharDataModeOffset = 4;
enum : uint8_t {
    kModeBrawl = 0, kModeRotation = 1, kModeSpecialBrawl = 2, kModeTourney = 3,
    kModeClassic = 4, kModeTargetSmash = 7, kModeEvents = 11,
};

// Solo top bar (verified in sora_menu_sel_char).
// The title (0x8068ED8C): the per-mode table 0x806A0B20 (12 bytes per mode) gives the model, u16 at +0 (180,
// MenSelchrTitleS_TopN, for modes 4-11), and its PAT0 frame, byte +2 alone or byte +3 once a second player has come in
// (u8 task+0x5FC, set at init or when P2 joins, 0x80689954, before the title is redrawn; never cleared).
constexpr uint32_t kModeTable = 0x806A0B20;
constexpr uint32_t kModeRecordBytes = 12;
constexpr uint32_t kModeTitleModelOffset = 0, kModeSoloFrameOffset = 2, kModeCoopFrameOffset = 3;
constexpr uint16_t kSoloTitleModel = 180;
constexpr uint32_t kTaskCoopOffset = 0x5FC;
// Difficulty or level (targets 9-13 set it to target - 9; 14 / 15 step it +1 / -1, wrapping; 0x8068A87C..0x8068A938):
// u32 task+0x5E0, 0-4; the icons shown, u32 task+0x5E4 (5, Target Smash: levels unlocked). Stock (targets 16-20 set
// it to target - 15; 21 / 22 step it, 1-5 wrapping; 0x8068A9B0..0x8068AA3C): u32 task+0x5DC (kTaskStockOffset).
// Home-Run play order (target 23 toggles it, 0x8068A620): u32 task+0x600. Multi-Man type (target 24 steps it,
// wrapping 6 to 0, 0x8068A780): u32 task+0x604.
constexpr uint32_t kTaskDifficultyOffset = 0x5E0;
constexpr uint32_t kTaskDifficultyCountOffset = 0x5E4;
constexpr uint32_t kTaskHomeRunOffset = 0x600;
constexpr uint32_t kTaskMultiManOffset = 0x604;
constexpr uint32_t kDifficulties = 5;
constexpr uint32_t kMinStock = 1, kMaxStock = 5;
// The play order's CLR animation by value (0x80691E78..0x80691EB8; Solo uses the first pair): byte 0 for value 0,
// byte 1 otherwise. Animation 0 shows "Cooperatively" (MenSelchrSngl1003), 1 "Alternately" (Sngl1002), each after
// "Play" (Sngl1004).
constexpr uint32_t kHomeRunAnimTable = 0x806A0CE8;
constexpr uint32_t kHomeRunAnims = 2;
// The Multi-Man type's sel_char entry, u32 per type (0x80692048 prints it in slot 1 of the task's MuMsg).
constexpr uint32_t kMultiManEntryTable = 0x806A0CF0;
constexpr uint32_t kMultiManTypes = 6;
// GameGlobal+0x28: the 120 player-name records, 0x124 bytes each, the name (6 UTF-16 units, none when the first is
// 0) at +0xE0.
constexpr uint32_t kGameGlobalNamesOffset = 0x28;
constexpr uint32_t kNameRecords = 120;
constexpr uint32_t kNameRecordBytes = 0x124;
constexpr uint32_t kNameTextOffset = 0xE0;
constexpr uint32_t kNameTextUnits = 6;

// muSelCharPlayerArea (verified against its init 0x806939C4).
constexpr uint32_t kAreaHandOffset = 0x1A8;
constexpr uint32_t kAreaCoinOffset = 0x1AC;
constexpr uint32_t kAreaIndexOffset = 0x1B0;      // u32: the card's place, 0-3
constexpr uint32_t kAreaPlayerKindOffset = 0x1B4; // u32: 0 none, 1 human, 2 CPU
constexpr uint32_t kAreaCharKindOffset = 0x1B8;   // u32 MuSelchkind
constexpr uint32_t kAreaCostumeOffset = 0x1BC;    // u32: index into the fighter's costume list
constexpr uint32_t kAreaTeamOffset = 0x1C0;       // u32: 0 red, 1 blue, 2 green
constexpr uint32_t kAreaNameOffset = 0x1C8;       // s32: name record, or none
constexpr uint32_t kAreaNumberOffset = 0x1CC;     // u32: when above 0 the default label is "NO." and this number
constexpr uint32_t kAreaHandicapOffset = 0x1D0;   // u32: handicap / 10, or stamina HP / 10
constexpr uint32_t kAreaCpuLevelOffset = 0x1D4;   // u32: 0-8, shown as 1-9
constexpr uint32_t kAreaControllerOffset = 0x1DC; // s32: -1 none (the card has no hand)
constexpr uint32_t kAreaTaskOffset = 0x1F4;
constexpr uint32_t kAreaListOffset = 0x1FC;       // MuSelctChrList
enum : uint32_t { kPlayerNone = 0, kPlayerHuman = 1, kPlayerCpu = 2 };
// exchangeCharKindDetail 0x806948D4: Zelda (14), Samus (3) and the Pokémon Trainer (27) show the kind their byte
// tables give for the card's choice at +0x1E8, +0x1EC, +0x1F0.
constexpr uint32_t kKindZelda = 14, kKindSamus = 3, kKindTrainer = 27;
constexpr uint32_t kZeldaChoiceOffset = 0x1E8, kSamusChoiceOffset = 0x1EC, kTrainerChoiceOffset = 0x1F0;
constexpr uint32_t kZeldaKinds = 0x806A0D78, kSamusKinds = 0x806A0D7C, kTrainerKinds = 0x806A0D80;
constexpr uint32_t kZeldaChoices = 2, kTrainerChoices = 4;
constexpr uint32_t kKindNone = 40, kKindRandom = 41, kFighterKinds = 42;
// muMenu::getNumCharColor 0x800AF8D0 (a leaf): 16-byte records from 0x80455458, a pointer at +8 to 2-byte entries
// ending at one whose first byte is 12.
constexpr uint32_t kCostumeTable = 0x80455458;
constexpr uint32_t kCostumeRecordBytes = 16;
constexpr uint32_t kCostumeListOffset = 8;
constexpr uint8_t kCostumeListEnd = 12;
constexpr uint32_t kMaxCostumes = 16;

// muSelCharHand (constructor 0x8069C0B8).
constexpr uint32_t kHandAreaOffset = 0x9C;
constexpr uint32_t kHandCoinOffset = 0xA0;   // the token it carries, or 0
constexpr uint32_t kHandModeOffset = 0xA4;   // 0 free, 1-3 token animations, 4 rule value, 5-7 card lists, 8 keyboard
constexpr uint32_t kHandModeArgOffset = 0xA8;
constexpr uint32_t kHandTargetOffset = 0xAC; // what it points at (hit test 0x8069DB68)
constexpr uint32_t kHandTargetIndexOffset = 0xB0;  // the card, for tokens and card parts (stale otherwise)
enum : uint32_t { kHandFree = 0, kHandHandicapList = 5, kHandCpuLevelList = 6, kHandNameList = 7 };
// Token (coin): the hand carrying it at +0x10, its card at +0x14.
constexpr uint32_t kCoinHolderOffset = 0x10;
constexpr uint32_t kCoinAreaOffset = 0x14;

// Hit-test targets (tables 0x806A0C70 top bar, 0x806A0EF8 card parts).
enum : uint32_t {
    kTargetToken = 1,
    kTargetBack = 2,
    kTargetModeName = 3,  // pressing it toggles team battle (Rotation: winner / loser out)
    kTargetRules = 4,
    kTargetRuleRight = 5, // raises the value
    kTargetRuleLeft = 6,  // lowers it
    kTargetRuleValue = 7, // hold A, then left / right
    kTargetReady = 8,
    // Solo (visibility switch 0x806A16B8 in 0x8069053C): 9-13 the difficulty or level icons, left to right, on
    // Option0000 (modes 4-7, icon i while i < task+0x5E4, none when it is 1); 14 the right arrow (+1), 15 the left
    // arrow (-1) (same modes and count; hung on Option0000's bones pos01 and pos00, which faces left); 16-20 stock 1-5,
    // 21 its right arrow (+1), 22 its left arrow (-1) (mode 4); 23 the Home-Run play order (mode 8 once co-op); 24 the
    // Multi-Man type (mode 9). The mode name (3), Rules (4) and the rule arrows (5-7) need the Group styles.
    kTargetFirstLevel = 9,
    kTargetLastLevel = 13,
    kTargetLevelRight = 14,
    kTargetLevelLeft = 15,
    kTargetFirstStock = 16,
    kTargetLastStock = 20,
    kTargetStockRight = 21,
    kTargetStockLeft = 22,
    kTargetHomeRun = 23,
    kTargetMultiMan = 24,
    kTargetCard = 27,     // the card: next costume (next team in team battle)
    kTargetNameEntry = 28,
    kTargetPlayerKind = 29,
    kTargetHandicap = 30,
    kTargetCpuLevel = 31,
    kTargetZelda = 32,    // the right half of Zelda's / Sheik's portrait
    kTargetSheik = 33,
    kTargetTrainer = 34,  // 34-37: Trainer, Charizard, Squirtle, Ivysaur quarters of the portrait (table 0x806A0D80)
    kTargetIvysaur = 37,
};

// MuSelctChrList (open 0x8069F240, process 0x8069FE88): type at +0 (0 names, 1 CPU level, 2 handicap, 3 stamina),
// open flag +4, cursor +0x44 (names: -1 and count+1 are the "New entry" rows, 0 the default label, then the names;
// numbers: 0..rows-1), name count +0x6C, name records u16 from +0x70. With 120 names saved the "New entry" rows
// switch to "List Full" only for a while after a refused press (0x8069FF60), so they are spoken as "New entry".
constexpr uint32_t kListTypeOffset = 0x0;
constexpr uint32_t kListCursorOffset = 0x44;
constexpr uint32_t kListNameCountOffset = 0x6C;
constexpr uint32_t kListNamesOffset = 0x70;
enum : uint32_t { kListNames = 0, kListCpuLevel = 1, kListHandicap = 2, kListStamina = 3 };
constexpr uint32_t kCpuLevels = 9, kHandicapRows = 31;

// sel_char entries: 0 "PLAYER", 1 "NO.", 2 "CPU", 3 "  NONE", 4-7 the rules line without its number, 15-23 the CPU
// level names. The task's and cards' MuMsgs print from this table (embedded copy in sc_selcharacter_en.pac, identical).
constexpr const char* kSelCharTable = "menu/msg/sel_char_en.msbin";
constexpr uint32_t kPlayerEntry = 0, kNumberEntry = 1, kCpuEntry = 2, kNoneEntry = 3;
constexpr uint32_t kTimeRuleEntry = 4, kStaminaRuleEntry = 7, kCpuLevelEntry = 15;

// Set while init runs: the setters it calls to build the screen are not player actions.
bool g_initializing = false;
// The hands of a screen that has just opened: what each points at on its first frame queues after the opening.
uint32_t g_openingHands[kMaxAreas] = {};
// The Solo title frame last spoken on this screen (-1: none), so that only a real change (P2 joining) speaks again.
int g_soloTitleFrame = -1;
// The card whose hand's top-bar input is running (0x8068A358), or -1: the actor of the top-bar setters it calls.
int g_topBarActor = -1;

uint8_t SelectionMode() {
    return Memory::Read8(Memory::Read32(Memory::Read32(kGameGlobalPointer) + kGameGlobalSelCharDataOffset) +
                         kSelCharDataModeOffset);
}

bool GroupMode() { return SelectionMode() <= kModeTourney; }

bool SoloMode() {
    const uint8_t mode = SelectionMode();
    return mode >= kModeClassic && mode <= kModeEvents;
}

uint32_t AreaCount(uint32_t task) {
    const uint32_t count = Memory::Read32(task + kTaskAreaCountOffset);
    return count <= kMaxAreas ? count : 0;
}

uint32_t Area(uint32_t task, uint32_t index) { return Memory::Read32(task + kTaskAreasOffset + index * 4); }

// The card's place, or -1 when `area` is not one of its task's cards: the check before trusting a hook argument.
int AreaIndex(uint32_t area) {
    const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
    const uint32_t index = Memory::Read32(area + kAreaIndexOffset);
    if (index >= AreaCount(task) || Area(task, index) != area) return -1;
    return static_cast<int>(index);
}

bool HandActive(uint32_t area) { return static_cast<int32_t>(Memory::Read32(area + kAreaControllerOffset)) >= 0; }

uint32_t Humans(uint32_t task) {
    uint32_t humans = 0;
    for (uint32_t i = 0; i < AreaCount(task); ++i)
        if (Memory::Read32(Area(task, i) + kAreaPlayerKindOffset) == kPlayerHuman) ++humans;
    return humans;
}

std::string Numbered(std::string_view key, uint32_t number) {
    std::string text = AccessibilityText::String(key);
    if (const size_t at = text.find("{number}"); at != std::string::npos) text.replace(at, 8, std::to_string(number));
    return text;
}

// The start of a line: the acting player while two or more humans are on the screen, then the card the line is about
// when it is not the actor's own. actor or card -1: unknown / none.
std::string Lead(uint32_t task, int actor, int card) {
    std::string player, place;
    if (actor >= 0 && Humans(task) >= 2) player = Numbered("character_select.player", actor + 1);
    if (card >= 0 && card != actor) place = Numbered("character_select.card", card + 1);
    return AccessibilityText::Join({player, place});
}

// The kind the card shows: Zelda / Sheik, Samus / Zero Suit Samus and the Pokémon Trainer's choice resolved.
uint32_t ShownKind(uint32_t area) {
    const uint32_t kind = Memory::Read32(area + kAreaCharKindOffset);
    uint32_t table = 0, offset = 0;
    if (kind == kKindZelda) table = kZeldaKinds, offset = kZeldaChoiceOffset;
    if (kind == kKindSamus) table = kSamusKinds, offset = kSamusChoiceOffset;
    if (kind == kKindTrainer) table = kTrainerKinds, offset = kTrainerChoiceOffset;
    if (table == 0) return kind;
    const uint32_t choice = Memory::Read32(area + offset);
    if (choice >= (kind == kKindTrainer ? kTrainerChoices : kZeldaChoices)) return kKindNone;
    return Memory::Read8(table + choice);
}

// The name plate's caption (art, MenSelchrChrNm); empty for no fighter.
std::string FighterName(uint32_t kind) {
    if (kind >= kFighterKinds || kind == kKindNone) return {};
    return AccessibilityText::StringAt("character_select.fighters", kind);
}

// Row lengths of the grid by icon count, read from the layout animation MenSelchrPos_TopN__0 (frame = icon count):
// icons fill rows left to right, top to bottom, and a short last row is centred.
uint32_t RowLength(uint32_t icons) {
    if (icons <= 5) return icons;
    if (icons <= 10) return 5;
    if (icons <= 18) return 6;
    if (icons <= 21) return 7;
    if (icons <= 24) return 8;
    if (icons <= 27) return 7;
    if (icons <= 32) return 8;
    if (icons <= 36) return 9;
    if (icons <= 40) return 10;
    if (icons <= 45) return 9;
    return 10;
}

// "row 2, column 5" for a kind in the grid; a centred short row counts the screen columns it sits under, and where it
// sits half a column over, the column to the left.
std::string GridPlace(uint32_t task, uint32_t kind) {
    const uint32_t icons = Memory::Read32(task + kTaskIconCountOffset);
    if (icons == 0 || icons > kMaxIcons) return {};
    for (uint32_t i = 0; i < icons; ++i) {
        if (Memory::Read32(task + kTaskIconKindsOffset + i * 4) != kind) continue;
        const uint32_t length = RowLength(icons);
        const uint32_t row = i / length, rows = (icons + length - 1) / length;
        const uint32_t inLastRow = icons - (rows - 1) * length;
        const uint32_t shift = row == rows - 1 ? (length - inLastRow) / 2 : 0;
        std::string text = AccessibilityText::String("character_select.grid_place");
        for (const auto& [field, value] : {std::pair<std::string_view, uint32_t>{"{row}", row + 1},
                                           {"{column}", i % length + 1 + shift}})
            if (const size_t at = text.find(field); at != std::string::npos)
                text.replace(at, field.size(), std::to_string(value));
        return text;
    }
    return {};
}

std::string Costume(uint32_t area) {
    const uint32_t kind = Memory::Read32(area + kAreaCharKindOffset);
    if (kind >= kKindNone) return {};
    uint32_t entry = Memory::Read32(kCostumeTable + kind * kCostumeRecordBytes + kCostumeListOffset);
    uint32_t costumes = 0;
    while (costumes < kMaxCostumes && Memory::Read8(entry) != kCostumeListEnd) ++costumes, entry += 2;
    const uint32_t costume = Memory::Read32(area + kAreaCostumeOffset);
    if (costumes == kMaxCostumes || costume >= costumes) return {};
    std::string text = AccessibilityText::String("character_select.costume");
    for (const auto& [field, value] : {std::pair<std::string_view, uint32_t>{"{index}", costume + 1},
                                       {"{total}", costumes}})
        if (const size_t at = text.find(field); at != std::string::npos)
            text.replace(at, field.size(), std::to_string(value));
    return text;
}

std::string Team(uint32_t area) {
    return AccessibilityText::StringAt("character_select.teams", Memory::Read32(area + kAreaTeamOffset));
}

// The card's costume, or its team in team battle.
std::string CostumeOrTeam(uint32_t area) {
    const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
    return Memory::Read8(task + kTaskTeamBattleOffset) != 0 ? Team(area) : Costume(area);
}

std::string NameRecord(uint32_t record) {
    if (record >= kNameRecords) return {};
    const uint32_t records = Memory::Read32(Memory::Read32(kGameGlobalPointer) + kGameGlobalNamesOffset);
    const uint32_t name = records + record * kNameRecordBytes + kNameTextOffset;
    if (Memory::Read16(name) == 0) return {};
    return AccessibilityText::GuestUtf16(name, kNameTextUnits);
}

// The default name label (fn 0x80693FE4): "PLAYER" and the card's number, or "NO." and the record's number.
std::string DefaultName(uint32_t area) {
    const uint32_t number = Memory::Read32(area + kAreaNumberOffset);
    if (static_cast<int32_t>(number) > 0) {
        const std::string label = AccessibilityText::DiscMessageEntry(kSelCharTable, kNumberEntry);
        if (label.empty()) return {};
        return label + (number < 10 ? "0" : "") + std::to_string(number);
    }
    const std::string label = AccessibilityText::DiscMessageEntry(kSelCharTable, kPlayerEntry);
    if (label.empty()) return {};
    return label + " " + std::to_string(Memory::Read32(area + kAreaIndexOffset) + 1);
}

// The card's name tab (printer 0x8069B1D8), which is also its human / CPU / none switch.
std::string NameTab(uint32_t area) {
    switch (Memory::Read32(area + kAreaPlayerKindOffset)) {
    case kPlayerNone: return AccessibilityText::DiscMessageEntry(kSelCharTable, kNoneEntry);
    case kPlayerCpu: return AccessibilityText::DiscMessageEntry(kSelCharTable, kCpuEntry);
    case kPlayerHuman: {
        const std::string name = NameRecord(Memory::Read32(area + kAreaNameOffset));
        return name.empty() ? DefaultName(area) : name;
    }
    default: return {};
    }
}

std::string Valued(std::string_view key, uint32_t value) {
    std::string text = AccessibilityText::String(key);
    if (const size_t at = text.find("{value}"); at != std::string::npos) text.replace(at, 7, std::to_string(value));
    return text;
}

// The card's handicap or, in stamina Special Brawl, its HP, as the card shows it.
std::string Handicap(uint32_t area) {
    const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
    const uint32_t value = Memory::Read32(area + kAreaHandicapOffset);
    if (Memory::Read8(task + kTaskStaminaOffset) != 0)
        return Valued("character_select.stamina", value == 0 ? 1 : value * 10);
    return Valued("character_select.handicap", value * 10);
}

// The mode name in the top bar (MenSelchrRfont, set by 0x8068ED8C): Group modes only.
std::string ModeName(uint32_t task) {
    const uint8_t mode = SelectionMode();
    if (mode == kModeRotation)
        return AccessibilityText::String(Memory::Read32(task + kTaskLoserOutOffset) != 0
                                             ? "character_select.modes.loser_out"
                                             : "character_select.modes.winner_out");
    if (mode > kModeTourney) return {};
    if (Memory::Read8(task + kTaskTeamBattleOffset) != 0)
        return AccessibilityText::String("character_select.modes.team_battle");
    return AccessibilityText::String(mode == kModeSpecialBrawl ? "character_select.modes.special_brawl"
                                                               : "character_select.modes.brawl");
}

// The rules line (0x8068F0A4): the value drawn as digits (0 as the infinity sign) before the sel_char entry, which
// starts with "-": "2-minute KO fest!". Stamina Special Brawl shows its entry alone.
std::string RulesLine(uint32_t task) {
    if (Memory::Read8(task + kTaskStaminaOffset) != 0)
        return AccessibilityText::DiscMessageEntry(kSelCharTable, kStaminaRuleEntry);
    const uint32_t kind = Memory::Read32(task + kTaskRuleKindOffset);
    if (kind > 2) return {};
    const std::string text = AccessibilityText::DiscMessageEntry(kSelCharTable, kTimeRuleEntry + kind);
    if (text.empty()) return {};
    const uint32_t value = Memory::Read32(task + (kind == 1 ? kTaskStockOffset : kTaskTimeOffset));
    return (value == 0 ? AccessibilityText::String("character_select.infinity") : std::to_string(value)) + text;
}

std::string PortraitChoice(uint32_t table, uint32_t choice) { return FighterName(Memory::Read8(table + choice)); }

// The Solo title's PAT0 frame (0x8068EE38..0x8068EE88), or -1 outside the Solo modes or when the table does not name
// the Solo title model.
int SoloTitleFrame(uint32_t task) {
    if (!SoloMode()) return -1;
    const uint32_t record = kModeTable + SelectionMode() * kModeRecordBytes;
    if (Memory::Read16(record + kModeTitleModelOffset) != kSoloTitleModel) return -1;
    const bool coop = Memory::Read8(task + kTaskCoopOffset) != 0;
    return Memory::Read8(record + (coop ? kModeCoopFrameOffset : kModeSoloFrameOffset));
}

// The title art at that frame (MenSelchrTitleS_TopN__0: the mode name, and "Co-op" on the co-op frames).
std::string SoloTitle(int frame) {
    if (frame < 0) return {};
    return AccessibilityText::StringAt("character_select.solo_titles", static_cast<size_t>(frame));
}

// A difficulty or, in Target Smash, a level, as its caption reads under the enlarged icon (Option0000: frame 0 of
// its PAT0 shows the difficulty words, frame 1, set in Target Smash by 0x806921F8, "LEVEL" and the number).
std::string Difficulty(uint32_t index) {
    if (index >= kDifficulties) return {};
    if (SelectionMode() == kModeTargetSmash) return Valued("character_select.level", index + 1);
    return AccessibilityText::StringAt("character_select.difficulties", index);
}

// The Home-Run play order as the button shows it for `value`.
std::string HomeRunOrder(uint32_t value) {
    const uint8_t anim = Memory::Read8(kHomeRunAnimTable + (value != 0 ? 1 : 0));
    if (anim >= kHomeRunAnims) return {};
    return AccessibilityText::StringAt("character_select.home_run", anim);
}

// The Multi-Man type's caption (game text).
std::string MultiManType(uint32_t type) {
    if (type >= kMultiManTypes) return {};
    return AccessibilityText::DiscMessageEntry(kSelCharTable, Memory::Read32(kMultiManEntryTable + type * 4));
}

// A Solo top-bar option the hand points at. The difficulty or level icons and the stock digits are a list: "name,
// selected, n of N", N the icons shown (u32 task+0x5E4; the hit test offers icon i only while i < N and none when N is
// 1, 0x80690BA8..0x80690C38) or the five digits. The current difficulty or level is enlarged and captioned (its bone
// group level1-5 moves in from y -200, CHR0 MenSelchrOption0000_TopN__0 at frame 5 × (value + 1)); the current stock
// shows that many stock icons beside the digits (0x806923A8): both are marked "selected".
std::string SoloTarget(uint32_t task, uint32_t target) {
    std::string text;
    bool selected = false;
    uint32_t place = 0, total = 0;
    if (target >= kTargetFirstLevel && target <= kTargetLastLevel) {
        const uint32_t index = target - kTargetFirstLevel;
        total = Memory::Read32(task + kTaskDifficultyCountOffset);
        if (total > kDifficulties || index >= total) return {};
        text = Difficulty(index);
        selected = Memory::Read32(task + kTaskDifficultyOffset) == index;
        place = index + 1;
    } else if (target >= kTargetFirstStock && target <= kTargetLastStock) {
        const uint32_t stock = target - kTargetFirstStock + kMinStock;
        text = Valued("character_select.stock", stock);
        selected = Memory::Read32(task + kTaskStockOffset) == stock;
        place = stock - kMinStock + 1;
        total = kMaxStock - kMinStock + 1;
    } else {
        switch (target) {
        case kTargetLevelRight:
        case kTargetStockRight: return AccessibilityText::String("character_select.right_arrow");
        case kTargetLevelLeft:
        case kTargetStockLeft: return AccessibilityText::String("character_select.left_arrow");
        case kTargetHomeRun: return HomeRunOrder(Memory::Read32(task + kTaskHomeRunOffset));
        case kTargetMultiMan: return MultiManType(Memory::Read32(task + kTaskMultiManOffset));
        default: return {};
        }
    }
    if (text.empty()) return {};
    return AccessibilityText::Join({text, selected ? AccessibilityText::String("character_select.selected") : std::string(),
                                    AccessibilityText::Position(place, total)});
}

// A top-bar item (targets 2-26) as the screen shows it in this mode, or nothing.
std::string TopBarTarget(uint32_t task, uint32_t target) {
    const bool group = GroupMode();
    const bool solo = !group && SoloMode();
    // Of the Group top bar, the Solo modes show Back and the ready banner (visibility types 0 and the banner state).
    if (target >= kTargetBack && target <= kTargetReady && !group &&
        !(solo && (target == kTargetBack || target == kTargetReady)))
        return {};
    if (target >= kTargetFirstLevel) return solo ? SoloTarget(task, target) : std::string{};
    switch (target) {
    case kTargetBack: return AccessibilityText::String("character_select.back");
    case kTargetModeName: return ModeName(task);
    // The Rules button's hit box frames the rules line as well (MenSelchrRule colL..colR); pressing it opens the rules.
    case kTargetRules: return AccessibilityText::Join({AccessibilityText::String("character_select.rules"), RulesLine(task)});
    case kTargetRuleRight: return AccessibilityText::String("character_select.right_arrow");
    case kTargetRuleLeft: return AccessibilityText::String("character_select.left_arrow");
    case kTargetRuleValue: return RulesLine(task);
    case kTargetReady: return AccessibilityText::String("character_select.ready");
    default: return {};
    }
}

// What a hand points at, for its owner's card `own`.
std::string Target(uint32_t task, int own, uint32_t target, uint32_t index) {
    // The top bar belongs to no card: the line starts with the pointing player while two or more humans play.
    if (target >= kTargetBack && target < kTargetCard) {
        const std::string text = TopBarTarget(task, target);
        if (text.empty()) return {};
        return AccessibilityText::Join({Lead(task, own, -1), text});
    }
    if (target == kTargetToken) {
        if (index >= AreaCount(task)) return {};
        const uint32_t area = Area(task, index);
        return AccessibilityText::Join({Lead(task, own, static_cast<int>(index)),
                                        AccessibilityText::String("character_select.token"),
                                        FighterName(ShownKind(area))});
    }
    if (target < kTargetCard || target > kTargetIvysaur || index >= AreaCount(task)) return {};
    const uint32_t area = Area(task, index);
    const std::string lead = Lead(task, own, static_cast<int>(index));
    std::string part;
    switch (target) {
    case kTargetCard: part = AccessibilityText::Join({FighterName(ShownKind(area)), CostumeOrTeam(area)}); break;
    case kTargetNameEntry: part = AccessibilityText::String("character_select.name_entry"); break;
    case kTargetPlayerKind: part = NameTab(area); break;
    case kTargetHandicap: part = Handicap(area); break;
    case kTargetCpuLevel:
        part = Valued("character_select.cpu_level", Memory::Read32(area + kAreaCpuLevelOffset) + 1);
        break;
    case kTargetZelda:
    case kTargetSheik: part = PortraitChoice(kZeldaKinds, target - kTargetZelda); break;
    default: part = PortraitChoice(kTrainerKinds, target - kTargetTrainer); break;
    }
    if (part.empty()) return {};
    return AccessibilityText::Join({lead, part});
}

// A top-bar setter's new value, after the player whose top-bar input made the change (two or more humans only).
std::string TopBarChange(uint32_t task, const std::string& text) {
    if (text.empty()) return {};
    return AccessibilityText::Join({Lead(task, g_topBarActor, -1), text});
}

// The card of the hand that acts on `area`: one carrying its token, else the card's own hand, else one pointing at one
// of its parts in [first, last]. -1 when none. A human's card parts react to its own hand only (X / Y act on the card
// of the token a hand carries, else its own), so another hand can be the actor only on a CPU or empty card.
int Actor(uint32_t area, uint32_t first, uint32_t last) {
    const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
    const int index = AreaIndex(area);
    if (index < 0) return -1;
    const uint32_t holder = Memory::Read32(Memory::Read32(area + kAreaCoinOffset) + kCoinHolderOffset);
    if (holder != 0) return AreaIndex(Memory::Read32(holder + kHandAreaOffset));
    if (HandActive(area)) return index;
    for (uint32_t i = 0; i < AreaCount(task); ++i) {
        const uint32_t other = Area(task, i);
        if (!HandActive(other)) continue;
        const uint32_t hand = Memory::Read32(other + kAreaHandOffset);
        const uint32_t target = Memory::Read32(hand + kHandTargetOffset);
        if (target >= first && target <= last &&
            Memory::Read32(hand + kHandTargetIndexOffset) == static_cast<uint32_t>(index))
            return static_cast<int>(i);
    }
    return -1;
}

// The card whose hand has a list of `area` open (hand modes 5-7 take the card as their argument).
int ListActor(uint32_t area, int index) {
    const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
    for (uint32_t i = 0; i < AreaCount(task); ++i) {
        const uint32_t other = Area(task, i);
        if (!HandActive(other)) continue;
        const uint32_t hand = Memory::Read32(other + kAreaHandOffset);
        const uint32_t mode = Memory::Read32(hand + kHandModeOffset);
        if (mode >= kHandHandicapList && mode <= kHandNameList &&
            Memory::Read32(hand + kHandModeArgOffset) == static_cast<uint32_t>(index))
            return static_cast<int>(i);
    }
    return -1;
}

// A list row as the list shows it, then its place: "3 WEAK, 3 of 9", "30%, 4 of 31", a name, "New entry".
std::string ListRow(uint32_t area, uint32_t list) {
    const uint32_t type = Memory::Read32(list + kListTypeOffset);
    const int32_t cursor = static_cast<int32_t>(Memory::Read32(list + kListCursorOffset));
    switch (type) {
    case kListCpuLevel: {
        if (cursor < 0 || static_cast<uint32_t>(cursor) >= kCpuLevels) return {};
        const std::string level = AccessibilityText::DiscMessageEntry(kSelCharTable, kCpuLevelEntry + cursor);
        if (level.empty()) return {};
        return AccessibilityText::Join({std::to_string(cursor + 1) + " " + level,
                                        AccessibilityText::Position(cursor + 1, kCpuLevels)});
    }
    case kListHandicap:
    case kListStamina: {
        if (cursor < 0 || static_cast<uint32_t>(cursor) >= kHandicapRows) return {};
        const std::string value = type == kListHandicap ? std::to_string(cursor * 10) + "%"
                                                        : std::to_string(cursor == 0 ? 1 : cursor * 10);
        return AccessibilityText::Join({value, AccessibilityText::Position(cursor + 1, kHandicapRows)});
    }
    case kListNames: {
        const uint32_t names = Memory::Read32(list + kListNameCountOffset);
        if (names > kNameRecords || cursor < -1 || cursor > static_cast<int32_t>(names) + 1) return {};
        std::string row;
        if (cursor == -1 || cursor == static_cast<int32_t>(names) + 1)
            row = AccessibilityText::String("character_select.new_entry");
        else if (cursor == 0)
            row = DefaultName(area);
        else
            row = NameRecord(Memory::Read16(list + kListNamesOffset + (cursor - 1) * 2));
        if (row.empty()) return {};
        return AccessibilityText::Join({row, AccessibilityText::Position(cursor + 2, names + 3)});
    }
    default: return {};
    }
}

void Say(const std::string& text, bool interrupt) {
    if (!text.empty()) Speech::Say(text, interrupt);
}

} // namespace CharacterSelectSpeech

// The screen has opened and drawn everything: the mode name interrupts, the rules line follows. The Solo modes show
// their title instead, and no rules line.
extern "C" void CharacterSelectInit_80682eb4(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t task = ctx->gpr[3];
    {
        struct InitializingScope {
            InitializingScope() { g_initializing = true; }
            ~InitializingScope() { g_initializing = false; }
        } initializing;
        func_80682EB4(ctx);
    }
    if (AccessibilityText::SilencedByMods()) return;
    try {
        for (uint32_t i = 0; i < kMaxAreas; ++i)
            g_openingHands[i] = i < AreaCount(task) ? Memory::Read32(Area(task, i) + kAreaHandOffset) : 0;
        g_soloTitleFrame = SoloTitleFrame(task);
        if (g_soloTitleFrame >= 0) {
            Say(SoloTitle(g_soloTitleFrame), true);
            return;
        }
        if (!GroupMode()) return;
        const std::string mode = ModeName(task);
        const std::string rules = RulesLine(task);
        Say(mode, true);
        Say(rules, mode.empty());
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select open", e);
    }
}

// Runs every frame for each hand; speaks when what the hand points at changes while the hand is free (not while a
// dropped token settles under it: the drop itself is silent).
extern "C" void CharacterSelectHover_8069dcd4(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t hand = ctx->gpr[3];
    bool opening = false;
    for (uint32_t& openingHand : g_openingHands)
        if (openingHand == hand) opening = true, openingHand = 0;
    uint32_t before = 0, beforeIndex = 0;
    const bool known = Memory::TryRead32(hand + kHandTargetOffset, before) &&
                       Memory::TryRead32(hand + kHandTargetIndexOffset, beforeIndex);
    func_8069DCD4(ctx);
    if (!known || AccessibilityText::SilencedByMods()) return;
    try {
        const uint32_t target = Memory::Read32(hand + kHandTargetOffset);
        const uint32_t index = Memory::Read32(hand + kHandTargetIndexOffset);
        // The index is stale for targets that do not set it, so it counts only for tokens and card parts.
        const bool indexed = target == kTargetToken || (target >= kTargetCard && target <= kTargetIvysaur);
        if (target == before && (!indexed || index == beforeIndex)) return;
        const uint32_t mode = Memory::Read32(hand + kHandModeOffset);
        if (mode != kHandFree) return;
        // A hand carrying a token cannot take another; the preview under its token speaks instead.
        if (target == kTargetToken && Memory::Read32(hand + kHandCoinOffset) != 0) return;
        const uint32_t area = Memory::Read32(hand + kHandAreaOffset);
        const int own = AreaIndex(area);
        if (own < 0) return;
        const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
        Say(Target(task, own, target, index), !opening);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select hover", e);
    }
}

// The fighter under a carried token changed: its name and grid place. Leaving a fighter (kind 40) is silent.
extern "C" void CharacterSelectSetFighter_80696f60(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t area = ctx->gpr[3], kind = ctx->gpr[4];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(area + kAreaCharKindOffset, before);
    func_80696F60(ctx);
    if (!known || before == kind || kind == kKindNone || g_initializing || AccessibilityText::SilencedByMods()) return;
    try {
        const int index = AreaIndex(area);
        if (index < 0) return;
        const uint32_t holder = Memory::Read32(Memory::Read32(area + kAreaCoinOffset) + kCoinHolderOffset);
        if (holder == 0) return;
        const int actor = AreaIndex(Memory::Read32(holder + kHandAreaOffset));
        const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
        const std::string name = FighterName(ShownKind(area));
        if (name.empty()) return;
        Say(AccessibilityText::Join({Lead(task, actor, index), name, GridPlace(task, kind)}), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select fighter", e);
    }
}

// A press in the grid (task, card, pad): when the hand's token left it, the token was dropped. The announcer calls
// every fighter but Random, which is spoken.
extern "C" void CharacterSelectGridPress_8068adc4(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t task = ctx->gpr[3], index = ctx->gpr[4];
    uint32_t hand = 0, coin = 0;
    bool known = false;
    try {
        if (index < AreaCount(task)) {
            hand = Memory::Read32(Area(task, index) + kAreaHandOffset);
            coin = Memory::Read32(hand + kHandCoinOffset);
            known = true;
        }
    } catch (const Memory::AccessViolation&) {
    }
    func_8068ADC4(ctx);
    if (!known || coin == 0 || AccessibilityText::SilencedByMods()) return;
    try {
        if (Memory::Read32(hand + kHandCoinOffset) != 0) return;
        const uint32_t area = Memory::Read32(coin + kCoinAreaOffset);
        const int card = AreaIndex(area);
        if (card < 0 || Memory::Read32(area + kAreaCharKindOffset) != kKindRandom) return;
        Say(AccessibilityText::Join({Lead(task, static_cast<int>(index), card), FighterName(kKindRandom)}), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select drop", e);
    }
}

// A card's player kind changed (its switch, a controller joining or leaving): the name tab it now shows.
extern "C" void CharacterSelectPlayerKind_8068be7c(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t task = ctx->gpr[3], index = ctx->gpr[4];
    uint32_t before = 0;
    bool known = false;
    try {
        if (index < AreaCount(task)) {
            before = Memory::Read32(Area(task, index) + kAreaPlayerKindOffset);
            known = true;
        }
    } catch (const Memory::AccessViolation&) {
    }
    func_8068BE7C(ctx);
    if (!known || g_initializing || AccessibilityText::SilencedByMods()) return;
    try {
        const uint32_t area = Area(task, index);
        if (Memory::Read32(area + kAreaPlayerKindOffset) == before) return;
        const int actor = Actor(area, kTargetPlayerKind, kTargetPlayerKind);
        Say(AccessibilityText::Join({Lead(task, actor, static_cast<int>(index)), NameTab(area)}), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select player kind", e);
    }
}

namespace CharacterSelectSpeech {

// Wraps a card setter (area in r3): after `original`, when `field` changed, speaks `describe(area)` for the acting
// hand, found among hands pointing at parts in [first, last].
template <typename Describe>
void CardChange(CpuContext* ctx, void (*original)(CpuContext*), uint32_t field, uint32_t first, uint32_t last,
                const char* who, Describe describe) {
    const uint32_t area = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(area + field, before);
    original(ctx);
    if (!known || g_initializing || AccessibilityText::SilencedByMods()) return;
    try {
        if (Memory::Read32(area + field) == before) return;
        const int index = AreaIndex(area);
        if (index < 0) return;
        const std::string text = describe(area);
        if (text.empty()) return;
        const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
        Say(AccessibilityText::Join({Lead(task, Actor(area, first, last), index), text}), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, who, e);
    }
}

} // namespace CharacterSelectSpeech

extern "C" void CharacterSelectNextCostume_8069a22c(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    CardChange(ctx, func_8069A22C, kAreaCostumeOffset, kTargetCard, kTargetCard, "character select costume", Costume);
}

extern "C" void CharacterSelectPreviousCostume_8069a340(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    CardChange(ctx, func_8069A340, kAreaCostumeOffset, kTargetCard, kTargetCard, "character select costume", Costume);
}

extern "C" void CharacterSelectNextTeam_8069987c(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    CardChange(ctx, func_8069987C, kAreaTeamOffset, kTargetCard, kTargetCard, "character select team", Team);
}

extern "C" void CharacterSelectPreviousTeam_80699bc4(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    CardChange(ctx, func_80699BC4, kAreaTeamOffset, kTargetCard, kTargetCard, "character select team", Team);
}

extern "C" void CharacterSelectZelda_80694934(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    CardChange(ctx, func_80694934, kZeldaChoiceOffset, kTargetZelda, kTargetSheik, "character select Zelda",
               [](uint32_t area) { return FighterName(ShownKind(area)); });
}

extern "C" void CharacterSelectTrainer_80694a04(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    CardChange(ctx, func_80694A04, kTrainerChoiceOffset, kTargetTrainer, kTargetIvysaur,
               "character select Pokemon Trainer", [](uint32_t area) { return FighterName(ShownKind(area)); });
}

// Team battle switched, or Rotation's winner / loser out: the mode name it now shows. Rotation's press stores its flag
// before calling this (0x8068A44C), and outside init only that press calls it in Rotation, so there every call speaks.
// In the Solo modes a second player joining calls it (0x8068995C, after setting the co-op flag): the title, now with
// "Co-op", interrupts, and what the hands point at next queues after it (the new player's hand starts the same frame).
extern "C" void CharacterSelectTeamBattle_8068ed8c(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t task = ctx->gpr[3];
    uint8_t teamBefore = 0;
    bool known = false;
    try {
        teamBefore = Memory::Read8(task + kTaskTeamBattleOffset);
        known = true;
    } catch (const Memory::AccessViolation&) {
    }
    func_8068ED8C(ctx);
    if (!known || g_initializing || AccessibilityText::SilencedByMods()) return;
    try {
        if (SoloMode()) {
            const int frame = SoloTitleFrame(task);
            if (frame < 0 || frame == g_soloTitleFrame) return;
            g_soloTitleFrame = frame;
            for (uint32_t i = 0; i < kMaxAreas; ++i)
                g_openingHands[i] = i < AreaCount(task) ? Memory::Read32(Area(task, i) + kAreaHandOffset) : 0;
            Say(SoloTitle(frame), true);
            return;
        }
        if (SelectionMode() != kModeRotation && Memory::Read8(task + kTaskTeamBattleOffset) == teamBefore) return;
        Say(TopBarChange(task, ModeName(task)), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select team battle", e);
    }
}

namespace CharacterSelectSpeech {

void RuleValue(CpuContext* ctx, void (*original)(CpuContext*)) {
    const uint32_t task = ctx->gpr[3];
    original(ctx);
    if (g_initializing || AccessibilityText::SilencedByMods()) return;
    try {
        if (GroupMode()) Say(TopBarChange(task, RulesLine(task)), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select rules", e);
    }
}

} // namespace CharacterSelectSpeech

extern "C" void CharacterSelectRuleUp_8068f99c(CpuContext* ctx) { CharacterSelectSpeech::RuleValue(ctx, func_8068F99C); }

extern "C" void CharacterSelectRuleDown_8068fbd0(CpuContext* ctx) { CharacterSelectSpeech::RuleValue(ctx, func_8068FBD0); }

namespace CharacterSelectSpeech {

// Wraps a Solo option's display setter (task, new value): init draws through it (silent) and the Wi-Fi sync calls it
// outside the Solo modes; otherwise only the option's press does, so each call is a change. The value comes in r4
// because the press stores it after the call (0x8068A938, 0x8068AA3C).
template <typename Describe>
void SoloValue(CpuContext* ctx, void (*original)(CpuContext*), const char* who, Describe describe) {
    const uint32_t task = ctx->gpr[3], value = ctx->gpr[4];
    original(ctx);
    if (g_initializing || AccessibilityText::SilencedByMods()) return;
    try {
        if (SoloMode()) Say(TopBarChange(task, describe(task, value)), true);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, who, e);
    }
}

} // namespace CharacterSelectSpeech

extern "C" void CharacterSelectDifficulty_80692298(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    SoloValue(ctx, func_80692298, "character select difficulty",
              [](uint32_t, uint32_t value) { return Difficulty(value); });
}

extern "C" void CharacterSelectStock_806923a8(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    SoloValue(ctx, func_806923A8, "character select stock", [](uint32_t, uint32_t value) {
        if (value < kMinStock || value > kMaxStock) return std::string{};
        return Valued("character_select.stock", value);
    });
}

extern "C" void CharacterSelectHomeRun_80691da4(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    SoloValue(ctx, func_80691DA4, "character select Home-Run",
              [](uint32_t, uint32_t value) { return HomeRunOrder(value); });
}

extern "C" void CharacterSelectMultiMan_80692048(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    SoloValue(ctx, func_80692048, "character select Multi-Man",
              [](uint32_t, uint32_t value) { return MultiManType(value); });
}

// A hand's top-bar input (task, card, pad; called per hand from 0x80689E98): every top-bar setter a press or a held
// rule value runs is called from inside it (team battle 0x8068A49C, Winner / Loser Out 0x8068A458, the rule value
// 0x8068AAFC..0x8068ABD8, the Solo options 0x8068A644..0x8068AA30), so the card it names is their actor.
extern "C" void CharacterSelectTopBarInput_8068a358(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t task = ctx->gpr[3], index = ctx->gpr[4];
    int actor = -1;
    try {
        if (index < AreaCount(task)) actor = static_cast<int>(index);
    } catch (const Memory::AccessViolation&) {
    }
    struct ActorScope {
        int saved;
        explicit ActorScope(int actor) : saved(g_topBarActor) { g_topBarActor = actor; }
        ~ActorScope() { g_topBarActor = saved; }
    } scope(actor);
    func_8068A358(ctx);
}

// The ready banner appeared: queued after whatever the last action said (the announcer has just called a name).
extern "C" void CharacterSelectReady_8069186c(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const bool show = ctx->gpr[4] != 0;
    func_8069186C(ctx);
    if (!show || g_initializing || AccessibilityText::SilencedByMods()) return;
    try {
        if (GroupMode() || SoloMode()) Say(AccessibilityText::String("character_select.ready"), false);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select ready", e);
    }
}

namespace CharacterSelectSpeech {

// The list's card, from the list's place inside it; -1 when it is not a card's list.
int ListArea(uint32_t list, uint32_t& area) {
    area = list - kAreaListOffset;
    return AreaIndex(area);
}

void SpeakListRow(uint32_t list) {
    uint32_t area = 0;
    const int index = ListArea(list, area);
    if (index < 0) return;
    const std::string row = ListRow(area, list);
    if (row.empty()) return;
    const uint32_t task = Memory::Read32(area + kAreaTaskOffset);
    Say(AccessibilityText::Join({Lead(task, ListActor(area, index), index), row}), true);
}

} // namespace CharacterSelectSpeech

// A card list opened (CPU level, handicap or stamina, names): its selected row.
extern "C" void CharacterSelectListOpen_8069f240(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t list = ctx->gpr[3];
    func_8069F240(ctx);
    if (g_initializing || AccessibilityText::SilencedByMods()) return;
    try {
        SpeakListRow(list);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select list", e);
    }
}

// Runs the Rules screen opened from here each frame. When it closes, the character select is back: what the hand
// that opened it points at speaks again, without the screen's title.
extern "C" void CharacterSelectRules_8069301c(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t task = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(task + kTaskRulesStateOffset, before);
    func_8069301C(ctx);
    if (!known || before == 0 || AccessibilityText::SilencedByMods()) return;
    try {
        if (Memory::Read32(task + kTaskRulesStateOffset) != 0) return;
        for (uint32_t i = 0; i < AreaCount(task); ++i) {
            const uint32_t area = Area(task, i);
            if (!HandActive(area)) continue;
            const uint32_t hand = Memory::Read32(area + kAreaHandOffset);
            const uint32_t target = Memory::Read32(hand + kHandTargetOffset);
            if (target != kTargetRules) continue;
            Say(Target(task, static_cast<int>(i), target, Memory::Read32(hand + kHandTargetIndexOffset)), true);
            return;
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select rules return", e);
    }
}

// Runs every frame while a list is open; speaks when its cursor moves.
extern "C" void CharacterSelectListInput_8069fe88(CpuContext* ctx) {
    using namespace CharacterSelectSpeech;
    const uint32_t list = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(list + kListCursorOffset, before);
    func_8069FE88(ctx);
    if (!known || AccessibilityText::SilencedByMods()) return;
    try {
        if (Memory::Read32(list + kListCursorOffset) != before) SpeakListRow(list);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "character select list", e);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x80682EB4, CharacterSelectInit_80682eb4, "CharacterSelectInit_80682eb4");
REGISTER_NATIVE_FUNCTION_AS(0x8069DCD4, CharacterSelectHover_8069dcd4, "CharacterSelectHover_8069dcd4");
REGISTER_NATIVE_FUNCTION_AS(0x80696F60, CharacterSelectSetFighter_80696f60, "CharacterSelectSetFighter_80696f60");
REGISTER_NATIVE_FUNCTION_AS(0x8068ADC4, CharacterSelectGridPress_8068adc4, "CharacterSelectGridPress_8068adc4");
REGISTER_NATIVE_FUNCTION_AS(0x8068BE7C, CharacterSelectPlayerKind_8068be7c, "CharacterSelectPlayerKind_8068be7c");
REGISTER_NATIVE_FUNCTION_AS(0x8069A22C, CharacterSelectNextCostume_8069a22c, "CharacterSelectNextCostume_8069a22c");
REGISTER_NATIVE_FUNCTION_AS(0x8069A340, CharacterSelectPreviousCostume_8069a340,
                            "CharacterSelectPreviousCostume_8069a340");
REGISTER_NATIVE_FUNCTION_AS(0x8069987C, CharacterSelectNextTeam_8069987c, "CharacterSelectNextTeam_8069987c");
REGISTER_NATIVE_FUNCTION_AS(0x80699BC4, CharacterSelectPreviousTeam_80699bc4, "CharacterSelectPreviousTeam_80699bc4");
REGISTER_NATIVE_FUNCTION_AS(0x80694934, CharacterSelectZelda_80694934, "CharacterSelectZelda_80694934");
REGISTER_NATIVE_FUNCTION_AS(0x80694A04, CharacterSelectTrainer_80694a04, "CharacterSelectTrainer_80694a04");
REGISTER_NATIVE_FUNCTION_AS(0x8068ED8C, CharacterSelectTeamBattle_8068ed8c, "CharacterSelectTeamBattle_8068ed8c");
REGISTER_NATIVE_FUNCTION_AS(0x8068F99C, CharacterSelectRuleUp_8068f99c, "CharacterSelectRuleUp_8068f99c");
REGISTER_NATIVE_FUNCTION_AS(0x8068FBD0, CharacterSelectRuleDown_8068fbd0, "CharacterSelectRuleDown_8068fbd0");
REGISTER_NATIVE_FUNCTION_AS(0x8069186C, CharacterSelectReady_8069186c, "CharacterSelectReady_8069186c");
REGISTER_NATIVE_FUNCTION_AS(0x8069F240, CharacterSelectListOpen_8069f240, "CharacterSelectListOpen_8069f240");
REGISTER_NATIVE_FUNCTION_AS(0x8069FE88, CharacterSelectListInput_8069fe88, "CharacterSelectListInput_8069fe88");
REGISTER_NATIVE_FUNCTION_AS(0x8069301C, CharacterSelectRules_8069301c, "CharacterSelectRules_8069301c");
REGISTER_NATIVE_FUNCTION_AS(0x80692298, CharacterSelectDifficulty_80692298, "CharacterSelectDifficulty_80692298");
REGISTER_NATIVE_FUNCTION_AS(0x806923A8, CharacterSelectStock_806923a8, "CharacterSelectStock_806923a8");
REGISTER_NATIVE_FUNCTION_AS(0x80691DA4, CharacterSelectHomeRun_80691da4, "CharacterSelectHomeRun_80691da4");
REGISTER_NATIVE_FUNCTION_AS(0x80692048, CharacterSelectMultiMan_80692048, "CharacterSelectMultiMan_80692048");
REGISTER_NATIVE_FUNCTION_AS(0x8068A358, CharacterSelectTopBarInput_8068a358, "CharacterSelectTopBarInput_8068a358");
