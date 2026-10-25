#include "accessibility_text.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "speech.h"

#include <cstdint>
#include <optional>
#include <string>

// Screen-reader speech for the name keyboard (MuName, sora_menu_name, resident), shared by the name screens, character
// select, Vault and Wi-Fi friend entry. Its opening, the key cursor's move (written only on a change) and its action
// function (the only place the typed text changes) are the events.
//
// The keyboard is a phone-style keypad: pressing a key again cycles its characters, replacing the last one typed. Each
// press speaks the character now typed; a deletion speaks what was removed; the OK key reads the name typed so far.

extern "C" void func_80674614(CpuContext* ctx);  // MuName: open
extern "C" void func_80674C28(CpuContext* ctx);  // MuName: move the key cursor
extern "C" void func_80674E84(CpuContext* ctx);  // MuName: act on the pressed key or button

// Named, not anonymous: the accessibility sources share one unity translation unit (cmake/PublicProducts.cmake).
namespace KeyboardSpeech {

// MuName layout (verified in sora_menu_name): +4 pointer to the typed UTF-8 text, +8 its maximum length in characters,
// +0x24 the character-set ids (u32 each), +0x38 their count, +0x3C the current set's index, +0x40 the key under the
// cursor.
constexpr uint32_t kTextPointerOffset = 0x4;
constexpr uint32_t kSetIdsOffset = 0x24;
constexpr uint32_t kSetCountOffset = 0x38;
constexpr uint32_t kSetIndexOffset = 0x3C;
constexpr uint32_t kKeyOffset = 0x40;
constexpr uint32_t kMaxSets = 5;
constexpr uint32_t kMaxTextBytes = 64;
// Keys: 0 Delete (top), 1-12 a 4 x 3 grid (12 switches the character set), 13 OK (bottom).
constexpr uint32_t kDeleteKey = 0;
constexpr uint32_t kSwitchKey = 12;
constexpr uint32_t kOkKey = 13;
constexpr uint32_t kKeyCount = 14;
// Character-set ids: 0 hiragana, 1 katakana, 2 letters, 3 numbers, 4 accented letters.
const char* SetKey(uint32_t id) {
    switch (id) {
    case 2: return "letters";
    case 3: return "numbers";
    case 4: return "accented";
    default: return nullptr;
    }
}

struct State {
    uint32_t set = 0;
    std::string text;
};

// The keyboard's current character set id, and the text typed; nullopt when unreadable.
std::optional<State> Read(uint32_t keyboard) {
    try {
        const uint32_t count = Memory::Read32(keyboard + kSetCountOffset);
        const uint32_t index = Memory::Read32(keyboard + kSetIndexOffset);
        if (count == 0 || count > kMaxSets || index >= count) return std::nullopt;
        State state;
        state.set = Memory::Read32(keyboard + kSetIdsOffset + 4 * index);
        const uint32_t text = Memory::Read32(keyboard + kTextPointerOffset);
        for (uint32_t i = 0; i < kMaxTextBytes; ++i) {
            const char c = static_cast<char>(Memory::Read8(text + i));
            if (c == '\0') return state;
            state.text += c;
        }
        return std::nullopt;
    } catch (const Memory::AccessViolation&) {
        return std::nullopt;
    }
}

// The last UTF-8 character of `text`.
std::string LastCharacter(const std::string& text) {
    size_t start = text.size();
    while (start > 0) {
        --start;
        if ((static_cast<uint8_t>(text[start]) & 0xC0) != 0x80) break;
    }
    return text.substr(start);
}

// "ABC, 3 of 14": the key under the cursor.
std::string KeyLine(uint32_t keyboard) {
    uint32_t key = 0;
    if (!Memory::TryRead32(keyboard + kKeyOffset, key) || key >= kKeyCount) return {};
    const auto state = Read(keyboard);
    if (!state) return {};
    std::string label;
    if (key == kDeleteKey) {
        label = AccessibilityText::String("keyboard.delete");
    } else if (key == kSwitchKey) {
        label = AccessibilityText::String("keyboard.switch");
    } else if (key == kOkKey) {
        label = AccessibilityText::Join(
            {AccessibilityText::String("keyboard.ok"), AccessibilityText::Prepare(state->text)});
    } else if (const char* set = SetKey(state->set)) {
        label = AccessibilityText::StringAt(std::string("keyboard.") + set, key - 1);
    }
    if (label.empty()) return {};
    return AccessibilityText::Join({label, AccessibilityText::Position(key + 1, kKeyCount)});
}

} // namespace KeyboardSpeech

extern "C" void KeyboardOpen_80674614(CpuContext* ctx) {
    const uint32_t keyboard = ctx->gpr[3];
    func_80674614(ctx);
    if (AccessibilityText::SilencedByMods()) return;
    if (const std::string line = KeyboardSpeech::KeyLine(keyboard); !line.empty()) Speech::Say(line, true);
}

extern "C" void KeyboardMoveCursor_80674c28(CpuContext* ctx) {
    const uint32_t keyboard = ctx->gpr[3];
    uint32_t before = 0;
    const bool known = Memory::TryRead32(keyboard + KeyboardSpeech::kKeyOffset, before);
    func_80674C28(ctx);
    uint32_t after = 0;
    if (!known || !Memory::TryRead32(keyboard + KeyboardSpeech::kKeyOffset, after) || after == before ||
        AccessibilityText::SilencedByMods())
        return;
    if (const std::string line = KeyboardSpeech::KeyLine(keyboard); !line.empty()) Speech::Say(line, true);
}

// Typing, deleting, cycling a key's characters, a random name or a character-set switch: what changed.
extern "C" void KeyboardAct_80674e84(CpuContext* ctx) {
    using namespace KeyboardSpeech;
    const uint32_t keyboard = ctx->gpr[3];
    const auto before = Read(keyboard);
    func_80674E84(ctx);
    if (!before || AccessibilityText::SilencedByMods()) return;
    const auto after = Read(keyboard);
    if (!after) return;
    const std::string& was = before->text;
    const std::string& now = after->text;
    const std::string stem = was.substr(0, was.size() - LastCharacter(was).size());
    std::string line;
    if (after->set != before->set) {
        if (const char* set = SetKey(after->set))
            line = AccessibilityText::String(std::string("keyboard.set_names.") + set);
    } else if (now.size() < was.size() && was.compare(0, now.size(), now) == 0) {
        line = AccessibilityText::String("keyboard.deleted");
        if (const size_t at = line.find("{text}"); at != std::string::npos)
            line.replace(at, 6, AccessibilityText::Prepare(was.substr(now.size())));
    } else if (now.size() > was.size() && now.compare(0, was.size(), was) == 0) {
        line = AccessibilityText::Prepare(LastCharacter(now));  // typed
    } else if (!was.empty() && now.compare(0, stem.size(), stem) == 0 &&
               now.size() - stem.size() == LastCharacter(now).size()) {
        line = AccessibilityText::Prepare(LastCharacter(now));  // the key's next character replaced the last one
    } else if (now != was) {
        line = AccessibilityText::Prepare(now);  // replaced at once (a random name)
    }
    if (!line.empty()) Speech::Say(line, true);
}

REGISTER_NATIVE_FUNCTION_AS(0x80674614, KeyboardOpen_80674614, "KeyboardOpen_80674614");
REGISTER_NATIVE_FUNCTION_AS(0x80674C28, KeyboardMoveCursor_80674c28, "KeyboardMoveCursor_80674c28");
REGISTER_NATIVE_FUNCTION_AS(0x80674E84, KeyboardAct_80674e84, "KeyboardAct_80674e84");
