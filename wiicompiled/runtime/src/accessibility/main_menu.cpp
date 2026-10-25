#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"
#include "stage_select_speech.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

// Screen-reader speech for the main menu (scene muMenuMain): Select Mode, Group, Solo, Options and its screens. Event
// hooks only, each wrapping the game's own function: a screen's enter (it opens), the shared cursor move and the
// description panel's setMsg (the selection changed), and for Erase Data its message prints and per-frame process
// (its yes/no dialog has no toggle function of its own).
//
// The screens are muProc objects in sora_menu_main, a module loaded on demand. With Gecko codes active (Project+ and
// other SD mods) on-demand modules are linked in the heap instead of at their translated home, so these enter hooks
// never fire and the other hooks, finding no open screen, stay silent.

extern "C" void func_94018F18(CpuContext* ctx);  // muProcTop enter (vtable +0xC)
extern "C" void func_94019708(CpuContext* ctx);  // muProcVs enter
extern "C" void func_9401716C(CpuContext* ctx);  // muProc1p enter
extern "C" void func_940187C0(CpuContext* ctx);  // muProcOption enter
extern "C" void func_9401AB98(CpuContext* ctx);  // muProcOptScreen enter
extern "C" void func_9401B0E8(CpuContext* ctx);  // muProcOptDeflick enter
extern "C" void func_9401CDEC(CpuContext* ctx);  // muProcOptVib enter
extern "C" void func_9401E068(CpuContext* ctx);  // muProcOptBtn enter (Options > Controls)
extern "C" void func_9402EF70(CpuContext* ctx);  // muProcName enter (Group > Names)
extern "C" void func_80677354(CpuContext* ctx);  // muNameTask: redraw the name list (sora_menu_name, resident)
extern "C" void func_9401B4A8(CpuContext* ctx);  // muProcOptSound enter
extern "C" void func_9401E5B8(CpuContext* ctx);  // muProcOptSong enter
extern "C" void func_9401C0A0(CpuContext* ctx);  // muProcOptDel enter
extern "C" void func_9401C170(CpuContext* ctx);  // muProcOptDel process (vtable +0x14)
extern "C" void func_9401D3A8(CpuContext* ctx);  // muProcOptVib moveCursor override (vtable +0x20)
extern "C" void func_9401E6D4(CpuContext* ctx);  // muProcOptSong process (vtable +0x14)
extern "C" void func_9401EF18(CpuContext* ctx);  // muProcOptSong moveCursor override: the song list's input
extern "C" void func_806A50F4(CpuContext* ctx);  // muProcMenu::moveCursor (sora_menu_rule, resident)
extern "C" void func_9403A048(CpuContext* ctx);  // muMainPanelCtrlTask::setMsg (vtable +0x64)
extern "C" void func_9403A290(CpuContext* ctx);  // muMainPanelCtrlTask::setSelect (vtable +0x74)
extern "C" void func_800B91B8(CpuContext* ctx);  // MuMsg::printIndex
extern "C" void func_80076F30(CpuContext* ctx);  // sndSystem: apply the music / sound-effects balance
extern "C" void func_80078CD8(CpuContext* ctx);  // sndSystem: set a song's My Music frequency

namespace {

// muProc layout (sora_menu_rule fn_18_11C4 builds the shared base).
constexpr uint32_t kProcCursorOffset = 0x42;     // u16: the selected item (on Screen and Deflicker, the value)
constexpr uint32_t kProcNavTableOffset = 0x63C;  // pointer to 16-byte entries, one per item
constexpr uint32_t kProcVtableOffset = 0x668;
constexpr uint32_t kNavEntryBytes = 16;          // u16 neighbour per input direction at +0, +4, +8, +12
constexpr uint32_t kNavDirections = 4;
// muMainPanelCtrlTask (vtable at +0x3C): setMsg 0x9403A048 prints with MuMsg::printIndex(*(panel+0x48), 0, entry,
// bin) and keeps the entry at +0x40 and bin at +0x80; printIndex 0x800B91B8 uses the MuMsg's own table at +4 when bin
// is null. The menu's procs pass a null bin (descriptions were silent until the fallback was followed).
constexpr uint32_t kPanelVtableOffset = 0x3C;
constexpr uint32_t kPanelMuMsgOffset = 0x48;
constexpr uint32_t kPanelMessageTableOffset = 0x80;
constexpr uint32_t kMuMsgMessageTableOffset = 0x4;
// muProcOptDel's yes/no dialog (process 0x9401C170): its MuMsg prints the question in slot 0 (entries 0-2, then 3
// while erasing) and YES / NO (entries 4 and 5) in slots 1 and 2; the byte cursor is 0 for YES, 1 for NO.
constexpr uint32_t kEraseDialogMuMsgOffset = 0x66C;
constexpr uint32_t kEraseDialogCursorOffset = 0x670;
constexpr uint32_t kEraseDialogStateOffset = 0x664;
constexpr uint32_t kEraseQuestionSlot = 0;
constexpr uint32_t kEraseErasingEntry = 3;
constexpr uint32_t kEraseYesEntry = 4;
constexpr uint8_t kEraseChoiceCount = 2;

// Globals the screens read (main.dol): GameGlobal and the sound system.
constexpr uint32_t kGameGlobalPointer = 0x805A00E0;
constexpr uint32_t kSoundSystemPointer = 0x805A01D8;
// GameGlobal: +0x24 the settings record, whose rumble bytes (one per pad) are at +0x820; +0x28 the 120 player-name
// records, 0x124 bytes each, the name (6 UTF-16 units, empty when the first is 0) at +0xE0 and its rumble flag at +0xEC
// (getters fn_8004DA94, fn_8004D6C4; inlined in the Rumble screen).
constexpr uint32_t kGameGlobalRecordOffset = 0x24;
constexpr uint32_t kRecordRumbleOffset = 0x820;
constexpr uint32_t kGameGlobalNamesOffset = 0x28;
constexpr uint32_t kNameRecords = 120;
constexpr uint32_t kNameRecordBytes = 0x124;
constexpr uint32_t kNameTextOffset = 0xE0;
constexpr uint32_t kNameTextUnits = 6;
constexpr uint32_t kNameRumbleOffset = 0xEC;
constexpr uint32_t kPads = 8;  // pads 0-7 (GameCube and Wii); the screen draws pad % 4 as its row

// muProcOptSound: the balance, u16 0-100 at +0x66C. Left raises it (towards sound effects only), right lowers it
// (towards music only): fn_80076F30 gives music min(1, 2(100-v)/100) and effects min(1, 2v/100).
constexpr uint32_t kSoundBalanceOffset = 0x66C;
constexpr uint16_t kSoundBalanceRow = 0;
// muProcOptVib: per-pad display state at +0x66C (-1 not connected, else the controller type: 0 GameCube, 1 Classic,
// 2 Wii Remote, 3 Wii Remote and Nunchuk); the Names tab's selected name at +0x68C (u16).
constexpr uint32_t kRumblePadStateOffset = 0x66C;
constexpr uint32_t kRumbleSelectedNameOffset = 0x68C;
constexpr int32_t kClassicControllerType = 1;
constexpr uint16_t kRumbleControllerTab = 0;
constexpr uint16_t kRumbleNamesTab = 1;
// muProcOptSong: its state (+0x664: 0 while the stage picker is up), the music group (+0x670), group 4's song count
// within the merged groups 4 and 5 (+0x688), the listed song ids (u32 from +0x698) and their count (+0x6D4), and the
// MuMsg printing titles from snd_title (*(+0x66C)).
constexpr uint32_t kMyMusicStateOffset = 0x664;
constexpr uint32_t kMyMusicPickerOffset = 0x694;  // the muSelectStageTask of the stage picker
constexpr uint32_t kMyMusicGroupOffset = 0x670;
constexpr uint32_t kMyMusicFirstGroupCountOffset = 0x688;
constexpr uint32_t kMyMusicSongIdsOffset = 0x698;
constexpr uint32_t kMyMusicSongCountOffset = 0x6D4;
constexpr uint32_t kMyMusicTitleMuMsgOffset = 0x66C;
constexpr uint32_t kMyMusicMergedGroup = 4;
constexpr uint32_t kMyMusicMaxSongs = 15;  // the id array runs from +0x698 to the count at +0x6D4
// sndSystem: the song info records (48 bytes, id at +0, snd_title entry at +0x10) at *(+0x194), their count at
// +0x198 (fn_800791FC); the My Music records (8 bytes: id, group at +6, frequency at +7) at *(+0), each group's first
// record index at +184 + 4*group (fn_80078D30).
constexpr uint32_t kSoundInfoRecordsOffset = 0x194;
constexpr uint32_t kSoundInfoCountOffset = 0x198;
constexpr uint32_t kSoundInfoRecordBytes = 48;
constexpr uint32_t kSoundInfoTitleOffset = 0x10;
constexpr uint32_t kSoundMusicRecordsOffset = 0x0;
constexpr uint32_t kSoundGroupFirstRecordOffset = 184;
constexpr uint32_t kMaxSoundRecords = 1024;

// Controls and Names keep a muNameTask (sora_menu_name fn_16_1EA0) at +0x71C: its list of names, 4 bytes each from
// +0x1F0 (s16 at +2: the name record, or -1 / -2 for "New entry"), count u16 at +0x3D8, state at +0x9C (2: the list
// has focus). The "New entry" label is main.dol text (fn_800B96B0; English pointer at 0x8059E09C, "@New entry").
constexpr uint32_t kProcNameTaskOffset = 0x71C;
constexpr uint32_t kNameTaskStateOffset = 0x9C;
constexpr uint32_t kNameTaskListOffset = 0x1F0;
constexpr uint32_t kNameTaskCountOffset = 0x3D8;
constexpr uint32_t kNameTaskRowOffset = 0x1EC;
constexpr uint32_t kNameTaskScrollOffset = 0x1EE;
constexpr uint32_t kNameTaskListState = 2;
constexpr uint32_t kNameTaskMaxEntries = 122;
constexpr uint32_t kNewEntryLabelPointer = 0x8059E09C;

// How a screen's selection changes: through the shared moveCursor, or only through the description panel's setMsg
// (screens with their own input handling call it on every change).
enum class Movement { SharedCursor, DescriptionMessage };
// Where an item's place comes from: the navigation table (items the player can reach), the cursor itself (a fixed
// list), or a single setting row.
enum class Placement { NavigationTable, Sequential, Single, RumbleTabs };

struct Screen {
    const char* titleKey;
    const char* itemsKey;  // nullptr: only the title is known so far
    uint16_t itemCount;
    Movement movement;
    Placement placement;
    uint32_t (*description)(uint16_t cursor);  // main_en.msbin entry the screen prints for the selected item
    // A setting's current value for the selected row (empty when the row has none).
    std::string (*value)(uint32_t proc, uint16_t cursor) = nullptr;
    // Lines queued after the selected item: what the selected tab shows.
    void (*details)(uint32_t proc, uint16_t cursor) = nullptr;
};

std::string SoundValue(uint32_t proc, uint16_t cursor);
void RumbleDetails(uint32_t proc, uint16_t tab);
void NameListDetails(uint32_t proc, uint16_t cursor);

// main_en.msbin entries, from each proc's setSelect / setMsg calls.
constexpr uint32_t kOptionDescriptions[] = {29, 30, 31, 32, 34, 33, 36};  // table at 0x94041890

constexpr Screen kTopScreen{"main_menu.titles.select_mode", "main_menu.items.top", 6, Movement::SharedCursor,
                            Placement::NavigationTable, [](uint16_t cursor) { return 0u + cursor; }};
constexpr Screen kGroupScreen{"main_menu.titles.group", "main_menu.items.group", 6, Movement::SharedCursor,
                              Placement::NavigationTable, [](uint16_t cursor) { return 6u + cursor; }};
constexpr Screen kSoloScreen{"main_menu.titles.solo", "main_menu.items.solo", 6, Movement::SharedCursor,
                             Placement::NavigationTable, [](uint16_t cursor) { return 12u + cursor; }};
constexpr Screen kOptionsScreen{"main_menu.titles.options", "main_menu.items.options", 7, Movement::SharedCursor,
                                Placement::NavigationTable,
                                [](uint16_t cursor) { return kOptionDescriptions[cursor]; }};
constexpr Screen kScreenScreen{"main_menu.titles.screen", "main_menu.items.screen", 2, Movement::DescriptionMessage,
                               Placement::Sequential, [](uint16_t cursor) { return 60u + cursor; }};
constexpr Screen kDeflickerScreen{"main_menu.titles.deflicker", "main_menu.items.deflicker", 2,
                                  Movement::DescriptionMessage, Placement::Single,
                                  [](uint16_t cursor) { return 63u - cursor; }};
constexpr Screen kEraseDataScreen{"main_menu.titles.erase_data", "main_menu.items.erase_data", 5,
                                  Movement::SharedCursor, Placement::NavigationTable,
                                  [](uint16_t cursor) { return 73u + cursor; }};
// Rumble's tabs; each tab's rows are spoken by RumbleDetails and the Rumble input hook.
constexpr Screen kRumbleScreen{"main_menu.titles.rumble", "main_menu.items.rumble_tabs", 2,
                               Movement::DescriptionMessage, Placement::RumbleTabs,
                               [](uint16_t tab) { return 64u + tab; }, nullptr, RumbleDetails};
constexpr Screen kSoundScreen{"main_menu.titles.sound", "main_menu.items.sound", 2, Movement::DescriptionMessage,
                              Placement::Sequential, [](uint16_t row) { return 68u + row; }, SoundValue};
// My Music opens on the stage picker (stage_select.cpp speaks it); its song list is spoken by the song-list hooks.
constexpr Screen kMyMusicScreen{"main_menu.titles.my_music", nullptr, 0, Movement::DescriptionMessage,
                                Placement::Single, nullptr};
// Controls and Names open on the name list (NameListDetails and the list's redraw hook speak it). Controls' button
// setup (scene scButton) and the name keyboard are not covered yet.
constexpr Screen kControlsScreen{"main_menu.titles.controls", nullptr, 0, Movement::SharedCursor, Placement::Single,
                                 nullptr, nullptr, NameListDetails};
constexpr Screen kNamesScreen{"main_menu.titles.names", nullptr, 0, Movement::SharedCursor, Placement::Single,
                              nullptr, nullptr, NameListDetails};

// The screen that opened last. muMenuMain runs one proc at a time; the vtable tells a later object at the same
// address apart.
struct OpenScreen {
    uint32_t proc;
    uint32_t vtable;
    const Screen* screen;
};
std::optional<OpenScreen> g_openScreen;
// The description panel, with its vtable to reject a stale pointer after the menu scene has gone.
uint32_t g_descriptionPanel = 0;
uint32_t g_descriptionPanelVtable = 0;
bool g_screenEntering = false;  // a screen's enter is running: its own setup calls are not selection changes

// Erase Data's dialog, followed between the message print that shows a question and the next frames.
struct EraseDialog {
    std::optional<uint32_t> pendingQuestion;  // printed this frame, not yet spoken
    bool open = false;
    uint8_t cursor = 0;
    uint32_t listState = 0;  // the screen's state in the frame the dialog opened from: its list has focus
};
EraseDialog g_eraseDialog;

// Whether `proc` is muMenuMain's current proc: the scene manager's current scene (*(0x805A0060)+4) is muMenuMain, and
// its proc array (+0x35C) holds `proc` at its current id (+0x3D0). A proc that handed over to one not hooked here
// (Sound Test, the button setup scene) is no longer current.
bool IsCurrentProc(uint32_t proc) {
    constexpr uint32_t kSceneManagerPointer = 0x805A0060;
    constexpr uint32_t kManagerCurrentSceneOffset = 0x4;
    constexpr uint32_t kMenuMainProcsOffset = 0x35C;
    constexpr uint32_t kMenuMainCurrentProcOffset = 0x3D0;
    constexpr uint32_t kMenuMainProcCount = 29;
    try {
        const uint32_t scene = Memory::Read32(Memory::Read32(kSceneManagerPointer) + kManagerCurrentSceneOffset);
        if (scene == 0 || AccessibilityText::GuestCString(Memory::Read32(scene), 16) != "muMenuMain") return false;
        const uint32_t id = Memory::Read32(scene + kMenuMainCurrentProcOffset);
        return id < kMenuMainProcCount && Memory::Read32(scene + kMenuMainProcsOffset + 4 * id) == proc;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

bool IsOpen(uint32_t proc, const Screen& screen) {
    if (!g_openScreen || g_openScreen->proc != proc || g_openScreen->screen != &screen) return false;
    uint32_t vtable = 0;
    return Memory::TryRead32(proc + kProcVtableOffset, vtable) && vtable == g_openScreen->vtable &&
           IsCurrentProc(proc);
}

void CaptureDescriptionPanel(uint32_t panel) {
    uint32_t vtable = 0;
    if (!Memory::TryRead32(panel + kPanelVtableOffset, vtable)) return;
    g_descriptionPanel = panel;
    g_descriptionPanelVtable = vtable;
}

// The message table the description panel prints from, by printIndex's rule; 0 when unknown.
uint32_t DescriptionTable() {
    uint32_t vtable = 0, table = 0, muMsg = 0;
    if (g_descriptionPanel == 0 || !Memory::TryRead32(g_descriptionPanel + kPanelVtableOffset, vtable) ||
        vtable != g_descriptionPanelVtable || !Memory::TryRead32(g_descriptionPanel + kPanelMessageTableOffset, table))
        return 0;
    if (table == 0 && Memory::TryRead32(g_descriptionPanel + kPanelMuMsgOffset, muMsg) && muMsg != 0)
        Memory::TryRead32(muMsg + kMuMsgMessageTableOffset, table);
    return table;
}

// The cursor's 1-based place among the items reachable from it, and their count. A locked item (Solo's All-Star
// before it is unlocked) is cut out of the navigation table by the screen's enter, so it is neither counted nor
// numbered.
std::optional<std::pair<size_t, size_t>> NavigationPosition(uint32_t proc, uint16_t cursor, uint16_t itemCount) {
    constexpr uint16_t kMaxItems = 32;
    if (itemCount > kMaxItems) return std::nullopt;
    try {
        const uint32_t table = Memory::Read32(proc + kProcNavTableOffset);
        bool reachable[kMaxItems] = {};
        uint16_t pending[kMaxItems];
        size_t pendingCount = 0;
        reachable[cursor] = true;
        pending[pendingCount++] = cursor;
        while (pendingCount > 0) {
            const uint16_t item = pending[--pendingCount];
            for (uint32_t direction = 0; direction < kNavDirections; ++direction) {
                const uint16_t neighbour = Memory::Read16(table + item * kNavEntryBytes + direction * 4);
                if (neighbour < itemCount && !reachable[neighbour]) {
                    reachable[neighbour] = true;
                    pending[pendingCount++] = neighbour;
                }
            }
        }
        size_t index = 0, total = 0;
        for (uint16_t item = 0; item < itemCount; ++item) {
            if (!reachable[item]) continue;
            ++total;
            if (item <= cursor) ++index;
        }
        return std::pair{index, total};
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "main menu navigation table", e);
        return std::nullopt;
    }
}

uint32_t GameGlobal() {
    uint32_t gameGlobal = 0;
    return Memory::TryRead32(kGameGlobalPointer, gameGlobal) ? gameGlobal : 0;
}

uint32_t SoundSystem() {
    uint32_t sound = 0;
    return Memory::TryRead32(kSoundSystemPointer, sound) ? sound : 0;
}

std::string OnOff(bool on) { return AccessibilityText::String(on ? "on" : "off"); }

// "40 percent": the balance knob's distance from the left (sound effects) end.
std::string SoundValue(uint32_t proc, uint16_t row) {
    if (row != kSoundBalanceRow) return {};
    uint16_t balance = 0;
    try {
        balance = Memory::Read16(proc + kSoundBalanceOffset);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "sound balance", e);
        return {};
    }
    if (balance > 100) return {};
    return AccessibilityText::Percent(100u - balance);
}

// The registered player names, as fn_8004D148 counts them (non-empty records).
std::optional<uint32_t> NameCount() {
    try {
        const uint32_t records = Memory::Read32(GameGlobal() + kGameGlobalNamesOffset);
        uint32_t count = 0;
        for (uint32_t i = 0; i < kNameRecords; ++i)
            if (Memory::Read16(records + i * kNameRecordBytes + kNameTextOffset) != 0) ++count;
        return count;
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "player names", e);
        return std::nullopt;
    }
}

// "Name, On, 2 of 5": a name on Rumble's Names tab with its rumble setting.
std::string RumbleNameLine(uint32_t proc) {
    const auto count = NameCount();
    if (!count || *count == 0) return {};
    try {
        const uint16_t index = Memory::Read16(proc + kRumbleSelectedNameOffset);
        if (index >= *count) return {};
        const uint32_t record = Memory::Read32(GameGlobal() + kGameGlobalNamesOffset) + index * kNameRecordBytes;
        const std::string name = AccessibilityText::GuestUtf16(record + kNameTextOffset, kNameTextUnits);
        if (name.empty()) return {};
        const bool on = Memory::Read8(record + kNameRumbleOffset) != 0;
        return AccessibilityText::Join({name, OnOff(on), AccessibilityText::Position(index + 1u, *count)});
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "rumble name", e);
        return {};
    }
}

// "Controller 1, GameCube Controller, On": a connected pad on Rumble's Controller tab; empty when not connected.
std::string RumblePadLine(uint32_t proc, uint32_t pad) {
    try {
        const int32_t state = static_cast<int32_t>(Memory::Read32(proc + kRumblePadStateOffset + 4 * pad));
        if (state < 0) return {};
        const uint32_t record = Memory::Read32(GameGlobal() + kGameGlobalRecordOffset);
        // As the screen draws it (fn_2_19BCC): on only when set and the pad can rumble (a Classic Controller cannot).
        const bool on = Memory::Read8(record + kRecordRumbleOffset + pad) == 1 && state != kClassicControllerType;
        std::string label = AccessibilityText::String("rumble.controller");
        if (const size_t at = label.find("{number}"); at != std::string::npos)
            label.replace(at, 8, std::to_string(pad % 4 + 1));
        const std::string type = AccessibilityText::StringAt("rumble.controller_types", static_cast<size_t>(state));
        if (type.empty()) return {};
        return AccessibilityText::Join({label, type, OnOff(on)});
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "rumble pad", e);
        return {};
    }
}

// What a Rumble tab shows: every connected controller, or the selected name.
void RumbleDetails(uint32_t proc, uint16_t tab) {
    if (tab == kRumbleControllerTab) {
        for (uint32_t pad = 0; pad < kPads; ++pad)
            if (const std::string line = RumblePadLine(proc, pad); !line.empty()) Speech::Say(line, false);
    } else if (tab == kRumbleNamesTab) {
        if (const std::string line = RumbleNameLine(proc); !line.empty()) Speech::Say(line, false);
    }
}

// "Group, Play a variety of Smash modes..., 1 of 6": the selected item's name, its value when it is a setting,
// description and place. Empty when the name cannot be established.
std::string SelectedItem(const OpenScreen& open) {
    const Screen& screen = *open.screen;
    if (screen.itemsKey == nullptr) return {};
    uint16_t cursor = 0;
    try {
        cursor = Memory::Read16(open.proc + kProcCursorOffset);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "main menu cursor", e);
        return {};
    }
    if (cursor >= screen.itemCount) {
        AccessibilityText::LogOnce(std::string("main menu cursor out of range on ") + screen.titleKey);
        return {};
    }
    const std::string name = AccessibilityText::StringAt(screen.itemsKey, cursor);
    if (name.empty()) return {};
    const std::string value = screen.value != nullptr ? screen.value(open.proc, cursor) : std::string();
    std::string description;
    if (const uint32_t table = DescriptionTable(); table != 0)
        description = AccessibilityText::MessageEntry(table, screen.description(cursor));
    else
        AccessibilityText::LogOnce("main menu description table unknown");
    std::string position;
    switch (screen.placement) {
    case Placement::NavigationTable:
        if (const auto place = NavigationPosition(open.proc, cursor, screen.itemCount))
            position = AccessibilityText::Position(place->first, place->second);
        break;
    case Placement::Sequential: position = AccessibilityText::Position(cursor + 1u, screen.itemCount); break;
    case Placement::Single: position = AccessibilityText::Position(1, 1); break;
    case Placement::RumbleTabs:
        // The Names tab can only be reached while a name is registered.
        if (const auto names = NameCount())
            position = AccessibilityText::Position(cursor + 1u, *names > 0 ? 2 : 1);
        break;
    }
    return AccessibilityText::Join({name, value, description, position});
}

// The selected item, then what it shows, after `title` (when there is one), which interrupts.
void SpeakSelection(const OpenScreen& open, const std::string& title) {
    const std::string item = SelectedItem(open);
    if (!title.empty()) Speech::Say(title, true);
    if (!item.empty()) Speech::Say(item, title.empty());
    if (open.screen->details == nullptr) return;
    uint16_t cursor = 0;
    try {
        cursor = Memory::Read16(open.proc + kProcCursorOffset);
    } catch (const Memory::AccessViolation&) {
        return;
    }
    open.screen->details(open.proc, cursor);
}

// Runs a screen's enter, then announces it: its title interrupts whatever was being said, its selected item follows.
void EnterScreen(CpuContext* ctx, void (*enter)(CpuContext*), const Screen& screen) {
    const uint32_t proc = ctx->gpr[3];
    {
        struct EnteringScope {
            EnteringScope() { g_screenEntering = true; }
            ~EnteringScope() { g_screenEntering = false; }
        } entering;
        enter(ctx);
    }
    uint32_t vtable = 0;
    if (!Memory::TryRead32(proc + kProcVtableOffset, vtable)) {
        g_openScreen.reset();
        RT_LOG(RT_TAG_ACCESSIBILITY) << "main menu screen at 0x" << std::hex << proc << std::dec << " unreadable"
                                     << std::endl;
        return;
    }
    g_openScreen = OpenScreen{proc, vtable, &screen};
    if (AccessibilityText::SilencedByMods()) return;
    SpeakSelection(*g_openScreen, AccessibilityText::String(screen.titleKey));
}

void AnnounceSelection() {
    if (!AccessibilityText::SilencedByMods()) SpeakSelection(*g_openScreen, {});
}

// A message table entry the Erase Data dialog prints, from its MuMsg's own table.
std::string EraseDialogEntry(uint32_t proc, uint32_t entry) {
    uint32_t muMsg = 0, table = 0;
    if (!Memory::TryRead32(proc + kEraseDialogMuMsgOffset, muMsg) || muMsg == 0 ||
        !Memory::TryRead32(muMsg + kMuMsgMessageTableOffset, table) || table == 0)
        return {};
    return AccessibilityText::MessageEntry(table, entry);
}

// "NO, 2 of 2": the dialog's selected answer. YES and NO are entries 4 and 5, in cursor order.
std::string EraseDialogChoice(uint32_t proc, uint8_t cursor) {
    if (cursor >= kEraseChoiceCount) return {};
    return AccessibilityText::Join({EraseDialogEntry(proc, kEraseYesEntry + cursor),
                                    AccessibilityText::Position(cursor + 1u, kEraseChoiceCount)});
}

// The My Music group a listed song belongs to: group 4's list merges groups 4 and 5 (the override asks
// fn_80078D30 for group 4 below +0x688 and 5 from there).
uint32_t MyMusicSongGroup(uint32_t proc, uint32_t index) {
    const uint32_t group = Memory::Read32(proc + kMyMusicGroupOffset);
    if (group != kMyMusicMergedGroup) return group;
    return index < Memory::Read32(proc + kMyMusicFirstGroupCountOffset) ? kMyMusicMergedGroup : kMyMusicMergedGroup + 1;
}

// fn_80078D30: a song's frequency within its group, 0-100; nullopt when the song is not in the group.
std::optional<uint32_t> MyMusicFrequency(uint32_t group, uint32_t songId) {
    const uint32_t sound = SoundSystem();
    const uint32_t records = Memory::Read32(sound + kSoundMusicRecordsOffset);
    uint32_t record = Memory::Read32(sound + kSoundGroupFirstRecordOffset + 4 * group);
    for (uint32_t steps = 0; steps < kMaxSoundRecords; ++steps, ++record) {
        const uint32_t address = records + record * 8;
        if (Memory::Read8(address + 6) != group) return std::nullopt;
        if (Memory::Read32(address) == songId) return Memory::Read8(address + 7);
    }
    return std::nullopt;
}

// The song's title: snd_title entry *(info+0x10) of its info record (fn_800791FC), from the title MuMsg's table.
std::string MyMusicTitle(uint32_t proc, uint32_t songId) {
    const uint32_t sound = SoundSystem();
    const uint32_t infos = Memory::Read32(sound + kSoundInfoRecordsOffset);
    const uint32_t count = Memory::Read32(sound + kSoundInfoCountOffset);
    for (uint32_t i = 0; i < count && i < kMaxSoundRecords; ++i) {
        const uint32_t info = infos + i * kSoundInfoRecordBytes;
        if (Memory::Read32(info) != songId) continue;
        const uint32_t muMsg = Memory::Read32(proc + kMyMusicTitleMuMsgOffset);
        const uint32_t table = muMsg != 0 ? Memory::Read32(muMsg + kMuMsgMessageTableOffset) : 0;
        return table != 0 ? AccessibilityText::MessageEntry(table, Memory::Read32(info + kSoundInfoTitleOffset))
                          : std::string();
    }
    return {};
}

// "Song title, 50 percent, 3 of 12": the selected song in My Music's list and how likely it is to play.
std::string MyMusicSong(uint32_t proc) {
    try {
        const uint32_t count = Memory::Read32(proc + kMyMusicSongCountOffset);
        const uint16_t index = Memory::Read16(proc + kProcCursorOffset);
        if (count > kMyMusicMaxSongs || index >= count) return {};
        const uint32_t songId = Memory::Read32(proc + kMyMusicSongIdsOffset + 4u * index);
        const std::string title = MyMusicTitle(proc, songId);
        if (title.empty()) return {};
        const auto frequency = MyMusicFrequency(MyMusicSongGroup(proc, index), songId);
        if (!frequency || *frequency > 100) return {};
        return AccessibilityText::Join({title, AccessibilityText::Percent(*frequency),
                                        AccessibilityText::Position(index + 1u, count)});
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "my music song", e);
        return {};
    }
}

// "New entry" without its leading '@' (a marker the label printer handles; inferred, it is not drawn).
std::string NewEntryLabel() {
    uint32_t label = 0;
    if (!Memory::TryRead32(kNewEntryLabelPointer, label) || label == 0) return {};
    std::string text = AccessibilityText::GuestCString(label, 64);
    if (!text.empty() && text.front() == '@') text.erase(0, 1);
    return text;
}

// "Name, description, 2 of 6": entry `index` of the name list, with the description the screen shows for it
// (Controls: main_en 66; Names: 41 on a name, 40 on "New entry").
std::string NameListLine(const OpenScreen& open, uint32_t task, uint32_t index) {
    try {
        const uint16_t count = Memory::Read16(task + kNameTaskCountOffset);
        if (count > kNameTaskMaxEntries || index >= count) return {};
        const int16_t record = static_cast<int16_t>(Memory::Read16(task + kNameTaskListOffset + 4 * index + 2));
        const bool newEntry = record < 0;
        std::string name;
        if (newEntry) {
            name = NewEntryLabel();
        } else if (static_cast<uint32_t>(record) < kNameRecords) {
            const uint32_t records = Memory::Read32(GameGlobal() + kGameGlobalNamesOffset);
            name = AccessibilityText::GuestUtf16(records + record * kNameRecordBytes + kNameTextOffset, kNameTextUnits);
        }
        if (name.empty()) return {};
        const uint32_t entry = open.screen == &kNamesScreen ? (newEntry ? 40u : 41u) : 66u;
        std::string description;
        if (const uint32_t table = DescriptionTable(); table != 0)
            description = AccessibilityText::MessageEntry(table, entry);
        return AccessibilityText::Join({name, description, AccessibilityText::Position(index + 1, count)});
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "name list", e);
        return {};
    }
}

// The name list's last spoken entry, so a redraw that changes nothing (opening, a name added) is not spoken twice.
struct NameListSpoken {
    uint32_t task = 0;
    std::string line;
};
NameListSpoken g_nameListSpoken;

void SpeakNameListEntry(const OpenScreen& open, uint32_t task, uint32_t index, bool interrupt) {
    const std::string line = NameListLine(open, task, index);
    if (line.empty() || (task == g_nameListSpoken.task && line == g_nameListSpoken.line)) return;
    g_nameListSpoken = {task, line};
    Speech::Say(line, interrupt);
}

// The name selected when Controls or Names opens, queued after the title.
void NameListDetails(uint32_t proc, uint16_t) {
    uint32_t task = 0;
    uint16_t row = 0, scroll = 0;
    try {
        task = Memory::Read32(proc + kProcNameTaskOffset);
        if (task == 0) return;
        row = Memory::Read16(task + kNameTaskRowOffset);
        scroll = Memory::Read16(task + kNameTaskScrollOffset);
    } catch (const Memory::AccessViolation&) {
        return;
    }
    g_nameListSpoken = {};
    SpeakNameListEntry(*g_openScreen, task, static_cast<uint32_t>(row) + scroll, false);
}

// My Music's song list: shown once its input starts running after a stage is picked; reset while the picker is up.
struct MyMusicList {
    bool shown = false;
    uint16_t cursor = 0;
};
MyMusicList g_myMusicList;

} // namespace

#define MAIN_MENU_ENTER_HOOK(name, address, screen) \
    extern "C" void name(CpuContext* ctx) { EnterScreen(ctx, &func_##address, screen); }

MAIN_MENU_ENTER_HOOK(MainMenuTopEnter_94018f18, 94018F18, kTopScreen)
MAIN_MENU_ENTER_HOOK(MainMenuGroupEnter_94019708, 94019708, kGroupScreen)
MAIN_MENU_ENTER_HOOK(MainMenuSoloEnter_9401716c, 9401716C, kSoloScreen)
MAIN_MENU_ENTER_HOOK(MainMenuOptionsEnter_940187c0, 940187C0, kOptionsScreen)
MAIN_MENU_ENTER_HOOK(MainMenuScreenEnter_9401ab98, 9401AB98, kScreenScreen)
MAIN_MENU_ENTER_HOOK(MainMenuDeflickerEnter_9401b0e8, 9401B0E8, kDeflickerScreen)
MAIN_MENU_ENTER_HOOK(MainMenuRumbleEnter_9401cdec, 9401CDEC, kRumbleScreen)
MAIN_MENU_ENTER_HOOK(MainMenuControlsEnter_9401e068, 9401E068, kControlsScreen)
MAIN_MENU_ENTER_HOOK(MainMenuNamesEnter_9402ef70, 9402EF70, kNamesScreen)
MAIN_MENU_ENTER_HOOK(MainMenuSoundEnter_9401b4a8, 9401B4A8, kSoundScreen)

extern "C" void MainMenuMyMusicEnter_9401e5b8(CpuContext* ctx) {
    EnterScreen(ctx, &func_9401E5B8, kMyMusicScreen);
    g_myMusicList = {};
    StageSelectSpeech::QueueNextHover();  // the first stage follows the title
}

extern "C" void MainMenuEraseDataEnter_9401c0a0(CpuContext* ctx) {
    EnterScreen(ctx, &func_9401C0A0, kEraseDataScreen);
    g_eraseDialog = {};
}

// Returns 1 in r3 only when the cursor moved; every muProc built on the shared base comes through here, so only the
// screen that opened last, still the same object, speaks.
extern "C" void MainMenuMoveCursor_806a50f4(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    func_806A50F4(ctx);
    if (ctx->gpr[3] != 1 || !g_openScreen || g_openScreen->screen->movement != Movement::SharedCursor) return;
    if (IsOpen(proc, *g_openScreen->screen)) AnnounceSelection();
}

// Screens with their own input handling (Screen, Deflicker) show a new description exactly when their selection
// changes.
extern "C" void MainMenuDescriptionMessage_9403a048(CpuContext* ctx) {
    CaptureDescriptionPanel(ctx->gpr[3]);
    func_9403A048(ctx);
    if (g_screenEntering || !g_openScreen || g_openScreen->screen->movement != Movement::DescriptionMessage) return;
    if (IsOpen(g_openScreen->proc, *g_openScreen->screen)) AnnounceSelection();
}

extern "C" void MainMenuDescriptionSelect_9403a290(CpuContext* ctx) {
    CaptureDescriptionPanel(ctx->gpr[3]);
    func_9403A290(ctx);
}

extern "C" void MainMenuMessagePrint_800b91b8(CpuContext* ctx) {
    const uint32_t muMsg = ctx->gpr[3], slot = ctx->gpr[4], entry = ctx->gpr[5];
    func_800B91B8(ctx);
    if (slot != kEraseQuestionSlot || !g_openScreen || g_openScreen->screen != &kEraseDataScreen) return;
    uint32_t dialogMuMsg = 0;
    if (Memory::TryRead32(g_openScreen->proc + kEraseDialogMuMsgOffset, dialogMuMsg) && dialogMuMsg == muMsg)
        g_eraseDialog.pendingQuestion = entry;
}

// After each frame of Erase Data: a question printed during it is spoken with the answer selected after it (the
// cursor resets to NO with each question); a changed answer is spoken alone; back on the list, its item again.
extern "C" void MainMenuEraseDataProcess_9401c170(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    uint32_t stateBefore = 0;
    const bool haveStateBefore = Memory::TryRead32(proc + kEraseDialogStateOffset, stateBefore);
    func_9401C170(ctx);
    if (!IsOpen(proc, kEraseDataScreen) || AccessibilityText::SilencedByMods()) return;
    uint8_t cursor = 0;
    uint32_t state = 0;
    try {
        cursor = Memory::Read8(proc + kEraseDialogCursorOffset);
        state = Memory::Read32(proc + kEraseDialogStateOffset);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "erase data dialog", e);
        return;
    }
    if (const auto question = std::exchange(g_eraseDialog.pendingQuestion, std::nullopt)) {
        const std::string text = EraseDialogEntry(proc, *question);
        if (!text.empty()) Speech::Say(text, true);
        if (*question != kEraseErasingEntry) {
            const std::string choice = EraseDialogChoice(proc, cursor);
            if (!choice.empty()) Speech::Say(choice, text.empty());
        }
        if (!g_eraseDialog.open) {
            if (!haveStateBefore) return;
            g_eraseDialog.listState = stateBefore;
        }
        g_eraseDialog.open = true;
        g_eraseDialog.cursor = cursor;
        return;
    }
    if (!g_eraseDialog.open) return;
    if (state == g_eraseDialog.listState) {
        g_eraseDialog.open = false;
        AnnounceSelection();
    } else if (cursor != g_eraseDialog.cursor) {
        g_eraseDialog.cursor = cursor;
        const std::string choice = EraseDialogChoice(proc, cursor);
        if (!choice.empty()) Speech::Say(choice, true);
    }
}

// The balance moved (fn_80076F30 applies it live on every step): the new position, alone. Its other callers apply the
// saved balance at load, when no Sound screen is open.
extern "C" void MainMenuSoundBalance_80076f30(CpuContext* ctx) {
    func_80076F30(ctx);
    if (g_screenEntering || !g_openScreen || !IsOpen(g_openScreen->proc, kSoundScreen) ||
        AccessibilityText::SilencedByMods())
        return;
    const std::string value = SoundValue(g_openScreen->proc, kSoundBalanceRow);
    if (!value.empty()) Speech::Say(value, true);
}

// Rumble's input (each pad toggles its own rumble with A; the Names tab moves and toggles the selected name). The
// toggles are inlined, so the frame's changes are found by comparing what they write.
extern "C" void MainMenuRumbleInput_9401d3a8(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    if (!IsOpen(proc, kRumbleScreen) || AccessibilityText::SilencedByMods()) {
        func_9401D3A8(ctx);
        return;
    }
    uint8_t padsBefore[kPads] = {};
    uint16_t tabBefore = 0, nameBefore = 0;
    std::string nameLineBefore;
    uint32_t record = 0;
    try {
        record = Memory::Read32(GameGlobal() + kGameGlobalRecordOffset);
        for (uint32_t pad = 0; pad < kPads; ++pad) padsBefore[pad] = Memory::Read8(record + kRecordRumbleOffset + pad);
        tabBefore = Memory::Read16(proc + kProcCursorOffset);
        nameBefore = Memory::Read16(proc + kRumbleSelectedNameOffset);
    } catch (const Memory::AccessViolation&) {
        func_9401D3A8(ctx);
        return;
    }
    if (tabBefore == kRumbleNamesTab) nameLineBefore = RumbleNameLine(proc);
    func_9401D3A8(ctx);
    try {
        if (Memory::Read16(proc + kProcCursorOffset) != tabBefore) return;  // setMsg announced the new tab
        if (tabBefore == kRumbleControllerTab) {
            for (uint32_t pad = 0; pad < kPads; ++pad) {
                if (Memory::Read8(record + kRecordRumbleOffset + pad) == padsBefore[pad]) continue;
                if (const std::string line = RumblePadLine(proc, pad); !line.empty()) Speech::Say(line, true);
            }
        } else if (tabBefore == kRumbleNamesTab) {
            const std::string line = RumbleNameLine(proc);
            if (!line.empty() &&
                (Memory::Read16(proc + kRumbleSelectedNameOffset) != nameBefore || line != nameLineBefore))
                Speech::Say(line, true);
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "rumble input", e);
    }
}

// My Music's frame: while its stage picker is up (state 0) the song list is not showing; coming back to the picker
// from the list, the stage under the pointer is spoken again (the pointer has not moved, so no hover event comes).
extern "C" void MainMenuMyMusicProcess_9401e6d4(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    func_9401E6D4(ctx);
    uint32_t state = 0, picker = 0;
    if (!IsOpen(proc, kMyMusicScreen) || !Memory::TryRead32(proc + kMyMusicStateOffset, state) || state != 0) return;
    if (g_myMusicList.shown && Memory::TryRead32(proc + kMyMusicPickerOffset, picker) && picker != 0)
        StageSelectSpeech::SpeakHovered(picker, true);
    g_myMusicList.shown = false;
}

// My Music's song-list input: its first frame shows the list (the selected song, then the A hint); later frames speak
// a newly selected song.
extern "C" void MainMenuMyMusicInput_9401ef18(CpuContext* ctx) {
    const uint32_t proc = ctx->gpr[3];
    func_9401EF18(ctx);
    if (!IsOpen(proc, kMyMusicScreen) || AccessibilityText::SilencedByMods()) return;
    uint16_t cursor = 0;
    try {
        cursor = Memory::Read16(proc + kProcCursorOffset);
    } catch (const Memory::AccessViolation&) {
        return;
    }
    if (g_myMusicList.shown && cursor == g_myMusicList.cursor) return;
    const bool opening = !g_myMusicList.shown;
    g_myMusicList = {true, cursor};
    const std::string song = MyMusicSong(proc);
    if (!song.empty()) Speech::Say(song, true);
    if (opening) {
        const std::string hint = AccessibilityText::String("my_music.play_hint");
        if (!hint.empty()) Speech::Say(hint, song.empty());
    }
}

// A song's frequency changed (its only caller is the song list's input): the new value, alone.
extern "C" void MainMenuMyMusicFrequency_80078cd8(CpuContext* ctx) {
    const uint32_t value = ctx->gpr[6];
    func_80078CD8(ctx);
    if (!g_openScreen || !IsOpen(g_openScreen->proc, kMyMusicScreen) || value > 100 ||
        AccessibilityText::SilencedByMods())
        return;
    Speech::Say(AccessibilityText::Percent(value), true);
}

// The name list redraws after every cursor move (fn_16_32E8: this, row, scroll): the selected name, alone.
extern "C" void MainMenuNameListRedraw_80677354(CpuContext* ctx) {
    const uint32_t task = ctx->gpr[3], row = ctx->gpr[4], scroll = ctx->gpr[5];
    func_80677354(ctx);
    if (g_screenEntering || !g_openScreen || AccessibilityText::SilencedByMods()) return;
    const Screen* screen = g_openScreen->screen;
    if ((screen != &kControlsScreen && screen != &kNamesScreen) || !IsOpen(g_openScreen->proc, *screen)) return;
    uint32_t ownTask = 0, state = 0;
    if (!Memory::TryRead32(g_openScreen->proc + kProcNameTaskOffset, ownTask) || ownTask != task ||
        !Memory::TryRead32(task + kNameTaskStateOffset, state) || state != kNameTaskListState)
        return;
    SpeakNameListEntry(*g_openScreen, task, row + scroll, true);
}

REGISTER_NATIVE_FUNCTION_AS(0x94018F18, MainMenuTopEnter_94018f18, "MainMenuTopEnter_94018f18");
REGISTER_NATIVE_FUNCTION_AS(0x94019708, MainMenuGroupEnter_94019708, "MainMenuGroupEnter_94019708");
REGISTER_NATIVE_FUNCTION_AS(0x9401716C, MainMenuSoloEnter_9401716c, "MainMenuSoloEnter_9401716c");
REGISTER_NATIVE_FUNCTION_AS(0x940187C0, MainMenuOptionsEnter_940187c0, "MainMenuOptionsEnter_940187c0");
REGISTER_NATIVE_FUNCTION_AS(0x9401AB98, MainMenuScreenEnter_9401ab98, "MainMenuScreenEnter_9401ab98");
REGISTER_NATIVE_FUNCTION_AS(0x9401B0E8, MainMenuDeflickerEnter_9401b0e8, "MainMenuDeflickerEnter_9401b0e8");
REGISTER_NATIVE_FUNCTION_AS(0x9401CDEC, MainMenuRumbleEnter_9401cdec, "MainMenuRumbleEnter_9401cdec");
REGISTER_NATIVE_FUNCTION_AS(0x9401E068, MainMenuControlsEnter_9401e068, "MainMenuControlsEnter_9401e068");
REGISTER_NATIVE_FUNCTION_AS(0x9401B4A8, MainMenuSoundEnter_9401b4a8, "MainMenuSoundEnter_9401b4a8");
REGISTER_NATIVE_FUNCTION_AS(0x9401E5B8, MainMenuMyMusicEnter_9401e5b8, "MainMenuMyMusicEnter_9401e5b8");
REGISTER_NATIVE_FUNCTION_AS(0x9401C0A0, MainMenuEraseDataEnter_9401c0a0, "MainMenuEraseDataEnter_9401c0a0");
REGISTER_NATIVE_FUNCTION_AS(0x806A50F4, MainMenuMoveCursor_806a50f4, "MainMenuMoveCursor_806a50f4");
REGISTER_NATIVE_FUNCTION_AS(0x9403A048, MainMenuDescriptionMessage_9403a048, "MainMenuDescriptionMessage_9403a048");
REGISTER_NATIVE_FUNCTION_AS(0x9403A290, MainMenuDescriptionSelect_9403a290, "MainMenuDescriptionSelect_9403a290");
REGISTER_NATIVE_FUNCTION_AS(0x800B91B8, MainMenuMessagePrint_800b91b8, "MainMenuMessagePrint_800b91b8");
REGISTER_NATIVE_FUNCTION_AS(0x9401C170, MainMenuEraseDataProcess_9401c170, "MainMenuEraseDataProcess_9401c170");
REGISTER_NATIVE_FUNCTION_AS(0x80076F30, MainMenuSoundBalance_80076f30, "MainMenuSoundBalance_80076f30");
REGISTER_NATIVE_FUNCTION_AS(0x9401D3A8, MainMenuRumbleInput_9401d3a8, "MainMenuRumbleInput_9401d3a8");
REGISTER_NATIVE_FUNCTION_AS(0x9401E6D4, MainMenuMyMusicProcess_9401e6d4, "MainMenuMyMusicProcess_9401e6d4");
REGISTER_NATIVE_FUNCTION_AS(0x9401EF18, MainMenuMyMusicInput_9401ef18, "MainMenuMyMusicInput_9401ef18");
REGISTER_NATIVE_FUNCTION_AS(0x80078CD8, MainMenuMyMusicFrequency_80078cd8, "MainMenuMyMusicFrequency_80078cd8");
REGISTER_NATIVE_FUNCTION_AS(0x9402EF70, MainMenuNamesEnter_9402ef70, "MainMenuNamesEnter_9402ef70");
REGISTER_NATIVE_FUNCTION_AS(0x80677354, MainMenuNameListRedraw_80677354, "MainMenuNameListRedraw_80677354");
