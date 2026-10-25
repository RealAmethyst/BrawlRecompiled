#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <cstdint>
#include <optional>
#include <string>

// Screen-reader speech for the controller setup (scene scButton, task muButtonTask in sora_menu_main), reached from
// Options > Controls by picking a name. Its states switch inside its own per-frame process, and the controller choice,
// list cursor and yes/no answer change inline there, so the process is wrapped and those fields compared. The action
// a button is being given is printed through its own function, wrapped as the event for that step.

extern "C" void func_9403573C(CpuContext* ctx);  // muButtonTask: processDefault
extern "C" void func_940359F8(CpuContext* ctx);  // muButtonTask: print a mapping row's action (fn_2_3192C)

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace ButtonSetupSpeech {

// muButtonTask: its data block at *(+0x40) (verified in sora_menu_main).
constexpr uint32_t kTaskDataOffset = 0x40;
constexpr uint32_t kStateOffset = 0xB0;        // u32
constexpr uint32_t kControllerOffset = 0xB4;   // u32: 0 GameCube, 1 Wii Remote with Nunchuk, 2 Wii Remote, 3 Classic
constexpr uint32_t kMuMsgOffset = 0x164;       // MuMsg over the `button` message table
constexpr uint32_t kMappingOffset = 0x19C;     // u8 action per row
constexpr uint32_t kTogglesOffset = 0x1A8;     // u8: 0x80 Tap Jump on, 0x40 Shake Smash on
constexpr uint32_t kCursorOffset = 0x1AC;      // u32: the list item
constexpr uint32_t kYesNoOffset = 0x1BC;       // u8: 1 YES (left), 0 NO
constexpr uint32_t kControllers = 4;
// States (process table 0x94042740): 1-2 choose the controller, 4 the mapping list, 5 choose a button's action, 7
// "restore defaults?", 8 "cannot test", 9 "save?", 12 saved.
enum : uint32_t { kChooseA = 1, kChooseB = 2, kList = 4, kAction = 5, kDefaults = 7, kNoTest = 8, kSave = 9, kSaved = 12 };
// `button` entries: the controller prompt, and per state the prompt or message it shows.
constexpr uint32_t kChoosePrompt = 0;
uint32_t StateMessage(uint32_t state) {
    switch (state) {
    case kDefaults: return 30;
    case kNoTest: return 31;
    case kSave: return 32;
    case kSaved: return 33;
    default: return UINT32_MAX;
    }
}
// Mapping-list items: rows 0..n-1, then 20 Tap Jump, 21 Shake Smash, 22 Default, 23 Test, 24 Save. Per controller the
// list's records (12 bytes, item id first) at *(0x940425F0 + 8*type), their count at +4, in screen order.
constexpr uint32_t kListTables = 0x940425F0;
constexpr uint32_t kListRecordBytes = 12;
constexpr uint32_t kMaxListItems = 32;
constexpr uint32_t kTapJumpItem = 20, kShakeSmashItem = 21, kDefaultItem = 22, kTestItem = 23, kSaveItem = 24;
// Action -> (name entry, description entry) in `button`: 2 bytes each from 0x94042658.
constexpr uint32_t kActionMessages = 0x94042658;
constexpr uint32_t kActions = 13;
const char* const kControllerRows[kControllers] = {"button_setup.gamecube", "button_setup.nunchuk",
                                                     "button_setup.wii_remote", "button_setup.classic"};

std::string Message(uint32_t data, uint32_t entry) {
    uint32_t muMsg = 0, table = 0;
    if (!Memory::TryRead32(data + kMuMsgOffset, muMsg) || muMsg == 0 || !Memory::TryRead32(muMsg + 4, table) ||
        table == 0)
        return {};
    return AccessibilityText::MessageEntry(table, entry);
}

// An action's name or description (`part` 0 or 1) from the `button` table.
std::string ActionText(uint32_t data, uint8_t action, uint32_t part) {
    if (action >= kActions) return {};
    try {
        return Message(data, Memory::Read8(kActionMessages + action * 2u + part));
    } catch (const Memory::AccessViolation&) {
        return {};
    }
}

// "Wii Remote, 3 of 4".
std::string ControllerLine(uint32_t controller) {
    if (controller >= kControllers) return {};
    const std::string name = AccessibilityText::StringAt("button_setup.controllers", controller);
    if (name.empty()) return {};
    return AccessibilityText::Join({name, AccessibilityText::Position(controller + 1, kControllers)});
}

// "A, Attack, <description>, 6 of 16": the mapping list's selected item.
std::string ListLine(uint32_t data) {
    try {
        const uint32_t controller = Memory::Read32(data + kControllerOffset);
        const uint32_t item = Memory::Read32(data + kCursorOffset);
        if (controller >= kControllers) return {};
        const uint32_t records = Memory::Read32(kListTables + 8 * controller);
        const uint32_t count = Memory::Read32(kListTables + 8 * controller + 4);
        if (count > kMaxListItems) return {};
        std::optional<uint32_t> place;
        uint32_t rows = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t id = Memory::Read8(records + i * kListRecordBytes);
            if (id < kTapJumpItem) ++rows;
            if (id == item) place = i + 1;
        }
        if (!place) return {};
        const std::string position = AccessibilityText::Position(*place, count);
        const uint8_t toggles = Memory::Read8(data + kTogglesOffset);
        const auto onOff = [](bool on) { return AccessibilityText::String(on ? "on" : "off"); };
        if (item < rows) {
            const std::string name = AccessibilityText::StringAt(kControllerRows[controller], item);
            const uint8_t action = Memory::Read8(data + kMappingOffset + item);
            if (name.empty()) return {};
            return AccessibilityText::Join(
                {name, ActionText(data, action, 0), ActionText(data, action, 1), position});
        }
        switch (item) {
        case kTapJumpItem:
            return AccessibilityText::Join({Message(data, 27), onOff(toggles & 0x80), Message(data, 9), position});
        case kShakeSmashItem:
            return AccessibilityText::Join({Message(data, 28), onOff(toggles & 0x40), Message(data, 10), position});
        case kDefaultItem:
            return AccessibilityText::Join({AccessibilityText::String("button_setup.default"), Message(data, 11),
                                            position});
        case kTestItem:
            return AccessibilityText::Join({AccessibilityText::String("button_setup.test"), Message(data, 12),
                                            position});
        case kSaveItem:
            return AccessibilityText::Join({AccessibilityText::String("button_setup.save"), Message(data, 13),
                                            position});
        default: return {};
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "button setup list", e);
        return {};
    }
}

// "NO, 2 of 2": a prompt's selected answer.
std::string YesNoLine(uint8_t yes) {
    const std::string label = AccessibilityText::String(yes ? "button_setup.yes" : "button_setup.no");
    if (label.empty()) return {};
    return AccessibilityText::Join({label, AccessibilityText::Position(yes ? 1 : 2, 2)});
}

struct Fields {
    uint32_t state = 0, controller = 0, cursor = 0;
    uint8_t yes = 0;
};

std::optional<Fields> ReadFields(uint32_t data) {
    try {
        return Fields{Memory::Read32(data + kStateOffset), Memory::Read32(data + kControllerOffset),
                      Memory::Read32(data + kCursorOffset), Memory::Read8(data + kYesNoOffset)};
    } catch (const Memory::AccessViolation&) {
        return std::nullopt;
    }
}

bool Choosing(uint32_t state) { return state == kChooseA || state == kChooseB; }

void Say(const std::string& text, bool interrupt) {
    if (!text.empty()) Speech::Say(text, interrupt);
}

// A state just began: its prompt or message, then what is selected in it.
void AnnounceState(uint32_t data, const Fields& now, bool interrupt) {
    if (Choosing(now.state)) {
        const std::string prompt = Message(data, kChoosePrompt);
        Say(prompt, interrupt);
        Say(ControllerLine(now.controller), interrupt && prompt.empty());
    } else if (now.state == kList) {
        Say(ListLine(data), interrupt);
    } else if (now.state == kAction) {
        try {
            const uint8_t action = Memory::Read8(data + kMappingOffset + now.cursor);
            Say(ActionText(data, action, 0), interrupt);
        } catch (const Memory::AccessViolation&) {
        }
    } else if (const uint32_t entry = StateMessage(now.state); entry != UINT32_MAX) {
        const std::string message = Message(data, entry);
        Say(message, interrupt);
        if (now.state == kDefaults || now.state == kSave) Say(YesNoLine(now.yes), interrupt && message.empty());
    }
}

uint32_t g_actionTask = 0;  // the task whose action step is open, so its first print (the current action) is not repeated

} // namespace ButtonSetupSpeech

extern "C" void ButtonSetupProcess_9403573c(CpuContext* ctx) {
    using namespace ButtonSetupSpeech;
    const uint32_t task = ctx->gpr[3];
    uint32_t data = 0;
    const bool known = Memory::TryRead32(task + kTaskDataOffset, data) && data != 0;
    const auto before = known ? ReadFields(data) : std::nullopt;
    func_9403573C(ctx);
    if (!before || AccessibilityText::SilencedByMods()) return;
    const auto now = ReadFields(data);
    if (!now) return;
    if (now->state != before->state) {
        // The two "choose a controller" states are one screen.
        if (!(Choosing(now->state) && Choosing(before->state))) AnnounceState(data, *now, true);
        g_actionTask = now->state == kAction ? task : 0;
    } else if (Choosing(now->state) && now->controller != before->controller) {
        Say(ControllerLine(now->controller), true);
    } else if (now->state == kList && now->cursor != before->cursor) {
        Say(ListLine(data), true);
    } else if ((now->state == kDefaults || now->state == kSave) && now->yes != before->yes) {
        Say(YesNoLine(now->yes), true);
    }
}

// While a button's action is being chosen, each candidate is printed into its row: the new action's name.
extern "C" void ButtonSetupActionPrint_940359f8(CpuContext* ctx) {
    using namespace ButtonSetupSpeech;
    const uint32_t task = ctx->gpr[3], action = ctx->gpr[7];
    func_940359F8(ctx);
    if (task != g_actionTask || action >= kActions || AccessibilityText::SilencedByMods()) return;
    uint32_t data = 0, state = 0;
    if (!Memory::TryRead32(task + kTaskDataOffset, data) || !Memory::TryRead32(data + kStateOffset, state) ||
        state != kAction)
        return;
    Say(ActionText(data, static_cast<uint8_t>(action), 0), true);
}

REGISTER_NATIVE_FUNCTION_AS(0x9403573C, ButtonSetupProcess_9403573c, "ButtonSetupProcess_9403573c");
REGISTER_NATIVE_FUNCTION_AS(0x940359F8, ButtonSetupActionPrint_940359f8, "ButtonSetupActionPrint_940359f8");
