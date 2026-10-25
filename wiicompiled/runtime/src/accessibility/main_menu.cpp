#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

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
extern "C" void func_9401E068(CpuContext* ctx);  // muProcOptBtn enter
extern "C" void func_9401B4A8(CpuContext* ctx);  // muProcOptSound enter
extern "C" void func_9401E5B8(CpuContext* ctx);  // muProcOptSong enter
extern "C" void func_9401C0A0(CpuContext* ctx);  // muProcOptDel enter
extern "C" void func_9401C170(CpuContext* ctx);  // muProcOptDel process (vtable +0x14)
extern "C" void func_806A50F4(CpuContext* ctx);  // muProcMenu::moveCursor (sora_menu_rule, resident)
extern "C" void func_9403A048(CpuContext* ctx);  // muMainPanelCtrlTask::setMsg (vtable +0x64)
extern "C" void func_9403A290(CpuContext* ctx);  // muMainPanelCtrlTask::setSelect (vtable +0x74)
extern "C" void func_800B91B8(CpuContext* ctx);  // MuMsg::printIndex

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

// How a screen's selection changes: through the shared moveCursor, or only through the description panel's setMsg
// (screens with their own input handling call it on every change).
enum class Movement { SharedCursor, DescriptionMessage };
// Where an item's place comes from: the navigation table (items the player can reach), the cursor itself (a fixed
// list), or a single setting row.
enum class Placement { NavigationTable, Sequential, Single };

struct Screen {
    const char* titleKey;
    const char* itemsKey;  // nullptr: only the title is known so far
    uint16_t itemCount;
    Movement movement;
    Placement placement;
    uint32_t (*description)(uint16_t cursor);  // main_en.msbin entry the screen prints for the selected item
};

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
// Not mapped beyond their titles yet: Rumble's per-controller toggles and name list, the Controls flow
// (sora_menu_name), the Sound balance slider and My Music's song list.
constexpr Screen kRumbleScreen{"main_menu.titles.rumble", nullptr, 0, Movement::DescriptionMessage,
                               Placement::Single, nullptr};
constexpr Screen kControlsScreen{"main_menu.titles.controls", nullptr, 0, Movement::DescriptionMessage,
                                 Placement::Single, nullptr};
constexpr Screen kSoundScreen{"main_menu.titles.sound", nullptr, 0, Movement::DescriptionMessage, Placement::Single,
                              nullptr};
constexpr Screen kMyMusicScreen{"main_menu.titles.my_music", nullptr, 0, Movement::DescriptionMessage,
                                Placement::Single, nullptr};

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

bool IsOpen(uint32_t proc, const Screen& screen) {
    if (!g_openScreen || g_openScreen->proc != proc || g_openScreen->screen != &screen) return false;
    uint32_t vtable = 0;
    return Memory::TryRead32(proc + kProcVtableOffset, vtable) && vtable == g_openScreen->vtable;
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

// "Group, Play a variety of Smash modes..., 1 of 6": the selected item's name (or a setting's value), description
// and place. Empty when the name cannot be established.
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
    }
    return AccessibilityText::Join({name, description, position});
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
    const std::string title = AccessibilityText::String(screen.titleKey);
    const std::string item = SelectedItem(*g_openScreen);
    if (!title.empty()) Speech::Say(title, true);
    if (!item.empty()) Speech::Say(item, title.empty());
}

void AnnounceSelection() {
    const std::string item = SelectedItem(*g_openScreen);
    if (!item.empty()) Speech::Say(item, true);
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
MAIN_MENU_ENTER_HOOK(MainMenuSoundEnter_9401b4a8, 9401B4A8, kSoundScreen)
MAIN_MENU_ENTER_HOOK(MainMenuMyMusicEnter_9401e5b8, 9401E5B8, kMyMusicScreen)

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
    if (!IsOpen(proc, kEraseDataScreen)) return;
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
