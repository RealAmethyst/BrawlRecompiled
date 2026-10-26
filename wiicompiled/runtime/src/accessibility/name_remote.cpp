#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <cstdint>
#include <optional>
#include <string>

// Screen-reader speech for the Wii Remote name transfer and the Names erase dialog. The transfer runs inside the name
// list's task (muNameTask, sora_menu_name, resident; Controls and Names): a state machine whose enter functions
// (table 0x8067B7E0, called once when the state changes, 0x806772E4) show its messages, and whose To Wii / To Wii
// Remote / Back choice redraws its highlight through one function on opening and on every move. Its prompts in the
// shared message window (overwrite, list full, errors) are spoken by notice_window.cpp.
//
// The erase dialog belongs to the Names screen (muProcName, sora_menu_main): the list handler opens it, its own input
// function runs each frame while it is up. Its question is printed when the screen opens, so its opening is the event.

extern "C" void func_80678DF8(CpuContext* ctx);  // muNameTask state 9 enter: "press 1 and 2"
extern "C" void func_806791F0(CpuContext* ctx);  // muNameTask state 12 enter: loading
extern "C" void func_80679544(CpuContext* ctx);  // muNameTask state 14 enter: no name data
extern "C" void func_80678C44(CpuContext* ctx);  // muNameTask: highlight the transfer choice (this, cursor)
extern "C" void func_8067A18C(CpuContext* ctx);  // muNameTask state 19 enter: the name was loaded
extern "C" void func_8067A530(CpuContext* ctx);  // muNameTask state 21 enter: saving
extern "C" void func_8067A88C(CpuContext* ctx);  // muNameTask state 23 enter: the name was saved
extern "C" void func_9402F264(CpuContext* ctx);  // muProcName: the name list's frame (opens the erase dialog)
extern "C" void func_9402F4C0(CpuContext* ctx);  // muProcName: the erase dialog's input

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace NameRemoteSpeech {

// muNameTask (verified in sora_menu_name): the MuMsg its messages print through at +0x154 (slot 6 the message, slots
// 7-9 the choice captions), and the message table it passes them at +0x458 (name_remote; printIndex 0x800B91B8 falls
// back to the MuMsg's own table at +4 when it is null). The Wii Remote's name, UTF-16, at +0x40E (copied from the
// remote's data, 0x80679298); the selected list record at +0x404 (s32); the choice's enabled flags, one byte per
// button from +0x450 (0 disabled: drawn dimmed by 0x80678C70 and skipped by the moves at 0x80679AD4, 0x80679B14).
constexpr uint32_t kTaskMuMsgOffset = 0x154;
constexpr uint32_t kTaskMessageTableOffset = 0x458;
constexpr uint32_t kMuMsgMessageTableOffset = 0x4;
constexpr uint32_t kTaskRemoteNameOffset = 0x40E;
constexpr uint32_t kTaskRecordOffset = 0x404;
constexpr uint32_t kTaskChoiceEnabledOffset = 0x450;
constexpr uint32_t kChoiceButtons = 3;

// name_remote entries, as each state enter passes them (li 5, n before printIndex): 0 "press 1 and 2" (0x80678E74),
// 8 loading (0x806791FC), 11 no name data (0x80679550), 3 saving (0x8067A53C). The loaded and saved lines are composed
// by fn_16_4ABC 0x80678B28: entry 9, the name, entry 10 (0x8067A1D4) / entry 4, the name, entry 5 (0x8067A8EC).
constexpr uint32_t kPressButtonsEntry = 0;
constexpr uint32_t kLoadingEntry = 8;
constexpr uint32_t kNoDataEntry = 11;
constexpr uint32_t kSavingEntry = 3;
constexpr uint32_t kLoadedPrefixEntry = 9, kLoadedSuffixEntry = 10;
constexpr uint32_t kSavedPrefixEntry = 4, kSavedSuffixEntry = 5;
// The choice captions, from the byte pairs at 0x8067B9F8 printed by state 15's enter (0x8067991C): 15 "To Wii",
// 16 "To Wii Remote", 17 "Back".
constexpr uint32_t kChoiceEntries[kChoiceButtons] = {15, 16, 17};
constexpr uint32_t kMaxTableBytes = 0x1000;  // offset table bound for the raw reader (name_remote has 20 entries)
constexpr uint32_t kMaxRawEntryBytes = 512;

// GameGlobal (main.dol 0x805A00E0) +0x28: the 120 player-name records, 0x124 bytes each, the name (6 UTF-16 units)
// at +0xE0; fn_8004D674 copies it for state 23.
constexpr uint32_t kGameGlobalPointer = 0x805A00E0;
constexpr uint32_t kGameGlobalNamesOffset = 0x28;
constexpr uint32_t kNameRecords = 120;
constexpr uint32_t kNameRecordBytes = 0x124;
constexpr uint32_t kNameTextOffset = 0xE0;
constexpr uint32_t kNameTextUnits = 6;

// muProcName (verified in sora_menu_main): its state at +0x664 (0 the list, 1 the erase dialog; set by 0x9402F300),
// flags at +0x66C whose bit 0x40 is the dialog's answer (set: YES; left sets it at 0x9402F61C, right clears it at
// 0x9402F5C8, opening clears it at 0x9402F2F4; A erases only while it is set, 0x9402F528), and the question's MuMsg at
// +0x674 (slot 0 = main entry 43, "Erase this name?", printed by the enter at 0x9402F04C from the MuMsg's own table).
constexpr uint32_t kProcStateOffset = 0x664;
constexpr uint32_t kProcFlagsOffset = 0x66C;
constexpr uint32_t kProcQuestionMuMsgOffset = 0x674;
constexpr uint32_t kProcEraseDialogState = 1;
constexpr uint8_t kEraseYesFlag = 0x40;
constexpr uint32_t kEraseQuestionEntry = 43;

// The table the task's messages print from, by printIndex's rule; 0 when unknown.
uint32_t MessageTable(uint32_t task) {
    uint32_t table = 0, muMsg = 0;
    if (!Memory::TryRead32(task + kTaskMessageTableOffset, table)) return 0;
    if (table == 0 && Memory::TryRead32(task + kTaskMuMsgOffset, muMsg) && muMsg != 0)
        Memory::TryRead32(muMsg + kMuMsgMessageTableOffset, table);
    return table;
}

// Entry `index` of a loaded message table as raw bytes, markup included, spanning off[index]..off[index + 1] as
// Message::getPrintIndexData 0x8006B134 copies it for a composed line; nullopt when the table looks wrong.
std::optional<std::string> RawEntry(uint32_t table, uint32_t index) {
    try {
        const uint32_t tableBytes = Memory::Read32(table);
        if (tableBytes < 8 || tableBytes > kMaxTableBytes || tableBytes % 4 != 0 || index + 1 >= tableBytes / 4)
            return std::nullopt;
        const uint32_t start = Memory::Read32(table + 4 * index);
        const uint32_t end = Memory::Read32(table + 4 * (index + 1));
        if (start < tableBytes || end < start || end - start > kMaxRawEntryBytes) return std::nullopt;
        std::string raw;
        for (uint32_t offset = start; offset < end; ++offset) raw += static_cast<char>(Memory::Read8(table + offset));
        return raw;
    } catch (const Memory::AccessViolation&) {
        return std::nullopt;
    }
}

// A line as fn_16_4ABC composes it: entry `prefix`, the name, entry `suffix`; empty when a part is missing.
std::string ComposedLine(uint32_t task, uint32_t prefix, const std::string& name, uint32_t suffix) {
    if (name.empty()) return {};
    const uint32_t table = MessageTable(task);
    if (table == 0) return {};
    const auto before = RawEntry(table, prefix);
    const auto after = RawEntry(table, suffix);
    if (!before || !after) {
        AccessibilityText::LogOnce("name transfer message table rejected");
        return {};
    }
    return AccessibilityText::Prepare(*before + name + *after);
}

// The name of list record `record`, empty when it is out of range or unused (fn_8004D674's rule).
std::string RecordName(int32_t record) {
    if (record < 0 || static_cast<uint32_t>(record) >= kNameRecords) return {};
    uint32_t gameGlobal = 0, records = 0;
    if (!Memory::TryRead32(kGameGlobalPointer, gameGlobal) ||
        !Memory::TryRead32(gameGlobal + kGameGlobalNamesOffset, records))
        return {};
    return AccessibilityText::GuestUtf16(records + static_cast<uint32_t>(record) * kNameRecordBytes + kNameTextOffset,
                                         kNameTextUnits);
}

// "Name was loaded from the Wii Remote.": the Wii Remote's name, as the screen composes it.
std::string LoadedLine(uint32_t task) {
    const std::string name = AccessibilityText::GuestUtf16(task + kTaskRemoteNameOffset, kNameTextUnits);
    return ComposedLine(task, kLoadedPrefixEntry, name, kLoadedSuffixEntry);
}

// "Name was saved to the Wii Remote.": the selected Wii name that was written.
std::string SavedLine(uint32_t task) {
    uint32_t record = 0;
    if (!Memory::TryRead32(task + kTaskRecordOffset, record)) return {};
    return ComposedLine(task, kSavedPrefixEntry, RecordName(static_cast<int32_t>(record)), kSavedSuffixEntry);
}

// Runs a state's enter, then speaks the line it shows (interrupting: each state replaces the last one's message).
template <typename Line>
void EnterState(CpuContext* ctx, void (*enter)(CpuContext*), Line line) {
    const uint32_t task = ctx->gpr[3];
    enter(ctx);
    if (AccessibilityText::SilencedByMods()) return;
    if (const std::string text = line(task); !text.empty()) Speech::Say(text, true);
}

void EnterMessageState(CpuContext* ctx, void (*enter)(CpuContext*), uint32_t entry) {
    EnterState(ctx, enter, [entry](uint32_t task) {
        const uint32_t table = MessageTable(task);
        return table != 0 ? AccessibilityText::MessageEntry(table, entry) : std::string();
    });
}

// "To Wii Remote, 2 of 3": the highlighted button, placed among the buttons the cursor can reach.
std::string ChoiceLine(uint32_t task, uint32_t cursor) {
    if (cursor >= kChoiceButtons) return {};
    bool enabled[kChoiceButtons] = {};
    try {
        for (uint32_t i = 0; i < kChoiceButtons; ++i) enabled[i] = Memory::Read8(task + kTaskChoiceEnabledOffset + i) != 0;
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "name transfer choice", e);
        return {};
    }
    if (!enabled[cursor]) return {};
    size_t index = 0, total = 0;
    for (uint32_t i = 0; i < kChoiceButtons; ++i) {
        if (!enabled[i]) continue;
        ++total;
        if (i <= cursor) ++index;
    }
    const uint32_t table = MessageTable(task);
    if (table == 0) return {};
    const std::string caption = AccessibilityText::MessageEntry(table, kChoiceEntries[cursor]);
    if (caption.empty()) return {};
    return AccessibilityText::Join({caption, AccessibilityText::Position(index, total)});
}

// "NO, 2 of 2": the erase dialog's selected answer, YES on the left, NO on the right.
std::string EraseAnswer(bool yes) {
    const std::string label = AccessibilityText::String(yes ? "names.yes" : "names.no");
    if (label.empty()) return {};
    return AccessibilityText::Join({label, AccessibilityText::Position(yes ? 1 : 2, 2)});
}

// "Erase this name?" from the question's MuMsg.
std::string EraseQuestion(uint32_t proc) {
    uint32_t muMsg = 0, table = 0;
    if (!Memory::TryRead32(proc + kProcQuestionMuMsgOffset, muMsg) || muMsg == 0 ||
        !Memory::TryRead32(muMsg + kMuMsgMessageTableOffset, table) || table == 0)
        return {};
    return AccessibilityText::MessageEntry(table, kEraseQuestionEntry);
}

} // namespace NameRemoteSpeech

extern "C" void NameRemotePressButtons_80678df8(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    EnterMessageState(ctx, func_80678DF8, kPressButtonsEntry);
}

extern "C" void NameRemoteLoading_806791f0(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    EnterMessageState(ctx, func_806791F0, kLoadingEntry);
}

extern "C" void NameRemoteNoData_80679544(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    EnterMessageState(ctx, func_80679544, kNoDataEntry);
}

extern "C" void NameRemoteSaving_8067a530(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    EnterMessageState(ctx, func_8067A530, kSavingEntry);
}

extern "C" void NameRemoteLoaded_8067a18c(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    EnterState(ctx, func_8067A18C, LoadedLine);
}

extern "C" void NameRemoteSaved_8067a88c(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    EnterState(ctx, func_8067A88C, SavedLine);
}

// The choice's highlight is drawn by state 16's enter (the choice opens) and by its process when the cursor moves:
// either way, the highlighted button alone, interrupting.
extern "C" void NameRemoteChoice_80678c44(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    const uint32_t task = ctx->gpr[3], cursor = ctx->gpr[4];
    func_80678C44(ctx);
    if (AccessibilityText::SilencedByMods()) return;
    if (const std::string line = ChoiceLine(task, cursor); !line.empty()) Speech::Say(line, true);
}

// The name list's frame: when it has opened the erase dialog (A on a name), the question, then the answer selected
// (NO), queued after it.
extern "C" void NamesEraseDialogOpen_9402f264(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    const uint32_t proc = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(proc + kProcStateOffset, before);
    func_9402F264(ctx);
    if (!known || before == kProcEraseDialogState || AccessibilityText::SilencedByMods()) return;
    uint32_t after = 0;
    uint8_t flags = 0;
    try {
        after = Memory::Read32(proc + kProcStateOffset);
        if (after != kProcEraseDialogState) return;
        flags = Memory::Read8(proc + kProcFlagsOffset);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_ACCESSIBILITY, "names erase dialog", e);
        return;
    }
    const std::string question = EraseQuestion(proc);
    const std::string answer = EraseAnswer((flags & kEraseYesFlag) != 0);
    if (!question.empty()) Speech::Say(question, true);
    if (!answer.empty()) Speech::Say(answer, question.empty());
}

// Each frame of the erase dialog: a changed answer, alone. Confirming or cancelling closes it without speech here.
extern "C" void NamesEraseDialogInput_9402f4c0(CpuContext* ctx) {
    using namespace NameRemoteSpeech;
    const uint32_t proc = ctx->gpr[3];
    uint8_t flagsBefore = 0;
    try {
        flagsBefore = Memory::Read8(proc + kProcFlagsOffset);
    } catch (const Memory::AccessViolation&) {
        func_9402F4C0(ctx);
        return;
    }
    func_9402F4C0(ctx);
    if (AccessibilityText::SilencedByMods()) return;
    uint8_t flagsAfter = 0;
    try {
        flagsAfter = Memory::Read8(proc + kProcFlagsOffset);
    } catch (const Memory::AccessViolation&) {
        return;
    }
    if (((flagsBefore ^ flagsAfter) & kEraseYesFlag) == 0) return;
    if (const std::string answer = EraseAnswer((flagsAfter & kEraseYesFlag) != 0); !answer.empty())
        Speech::Say(answer, true);
}

REGISTER_NATIVE_FUNCTION_AS(0x80678DF8, NameRemotePressButtons_80678df8, "NameRemotePressButtons_80678df8");
REGISTER_NATIVE_FUNCTION_AS(0x806791F0, NameRemoteLoading_806791f0, "NameRemoteLoading_806791f0");
REGISTER_NATIVE_FUNCTION_AS(0x80679544, NameRemoteNoData_80679544, "NameRemoteNoData_80679544");
REGISTER_NATIVE_FUNCTION_AS(0x8067A530, NameRemoteSaving_8067a530, "NameRemoteSaving_8067a530");
REGISTER_NATIVE_FUNCTION_AS(0x8067A18C, NameRemoteLoaded_8067a18c, "NameRemoteLoaded_8067a18c");
REGISTER_NATIVE_FUNCTION_AS(0x8067A88C, NameRemoteSaved_8067a88c, "NameRemoteSaved_8067a88c");
REGISTER_NATIVE_FUNCTION_AS(0x80678C44, NameRemoteChoice_80678c44, "NameRemoteChoice_80678c44");
REGISTER_NATIVE_FUNCTION_AS(0x9402F264, NamesEraseDialogOpen_9402f264, "NamesEraseDialogOpen_9402f264");
REGISTER_NATIVE_FUNCTION_AS(0x9402F4C0, NamesEraseDialogInput_9402f4c0, "NamesEraseDialogInput_9402f4c0");
