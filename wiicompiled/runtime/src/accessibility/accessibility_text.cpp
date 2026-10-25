#include "accessibility_text.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <toml.hpp>

namespace AccessibilityText {
namespace {

// A message table starts with one big-endian u32 offset per entry, from the table's start; the first offset is
// therefore the table size. An entry runs to the next entry's offset (the last to the end of the file) and ends at
// its first NUL (Message::getPrintIndexData 0x8006B134). The file's size is not known in memory, so the last entry is
// read up to its NUL within kMaxEntryBytes. These bounds reject a pointer that is not a message table.
constexpr uint32_t kMaxOffsetTableBytes = 0x10000;
constexpr uint32_t kMaxEntryBytes = 0x1000;

// Byte length of the markup code at text[i] (code < 0x20), including the code itself; nullopt for a code that is
// not markup. From the game's own skip table (fn_8006CF54, jump table 0x804541F8) and tag processor (fn_8006D438,
// jump table 0x80454270); every entry of the disc's English message tables parses with it.
std::optional<size_t> MarkupLength(const std::string& text, size_t i) {
    const auto at = [&](size_t k) -> unsigned { return k < text.size() ? static_cast<uint8_t>(text[k]) : 0u; };
    const auto axes = [&](size_t k) { return static_cast<size_t>(__builtin_popcount(at(k) & 7u)); };
    switch (at(i)) {
    case 0x09: case 0x0A: case 0x13: return 1;
    case 0x02: case 0x07: case 0x08: case 0x0E: case 0x12: case 0x1B: case 0x1D: return 2;
    case 0x0D: case 0x0F: case 0x10: case 0x1C: return 3;
    case 0x03: case 0x04: case 0x0C: case 0x11: return 5;
    case 0x0B: return 3 + at(i + 1) + at(i + 2);  // ruby: reading bytes, then base bytes
    case 0x14: return 2 + 2 * axes(i + 1);        // cursor position, 16-bit per axis
    case 0x16: return 2 + 4 * axes(i + 1);        // cursor position, float per axis
    default: return std::nullopt;
    }
}

// The speakable form of a raw entry: markup dropped (a ruby keeps its base text), tabs and line breaks as spaces,
// full-width ASCII forms (U+FF01..U+FF5E) and the ideographic space folded to ASCII, runs of spaces collapsed.
std::optional<std::string> Speakable(const std::string& raw) {
    std::string text;
    for (size_t i = 0; i < raw.size();) {
        const auto byte = static_cast<uint8_t>(raw[i]);
        if (byte >= 0x20) {
            if (byte == 0xEF && i + 2 < raw.size()) {
                const auto b1 = static_cast<uint8_t>(raw[i + 1]), b2 = static_cast<uint8_t>(raw[i + 2]);
                const unsigned codePoint = ((byte & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
                if (codePoint >= 0xFF01 && codePoint <= 0xFF5E) {
                    text += static_cast<char>(codePoint - 0xFEE0);
                    i += 3;
                    continue;
                }
            } else if (byte == 0xE3 && raw.compare(i, 3, "\xE3\x80\x80") == 0) {
                text += ' ';
                i += 3;
                continue;
            }
            text += raw[i++];
            continue;
        }
        const auto length = MarkupLength(raw, i);
        if (!length || i + *length > raw.size()) return std::nullopt;
        if (byte == 0x09 || byte == 0x0A) {
            text += ' ';
        } else if (byte == 0x0B) {
            const size_t reading = static_cast<uint8_t>(raw[i + 1]);
            const size_t base = static_cast<uint8_t>(raw[i + 2]);
            text.append(raw, i + 3 + reading, base);
        }
        i += *length;
    }
    std::string collapsed;
    for (const char c : text) {
        if (c == ' ' && (collapsed.empty() || collapsed.back() == ' ')) continue;
        collapsed += c;
    }
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    return collapsed;
}

const toml::value* Strings() {
    static std::once_flag loaded;
    static std::optional<toml::value> strings;
    std::call_once(loaded, [] {
        const auto directory = RuntimeConfigFile::ExecutableDirectory();
        if (!directory) {
            LogOnce("strings unavailable: the executable's folder is unknown");
            return;
        }
        const auto path = *directory / "accessibility" / "strings_en.toml";
        try {
            strings = toml::parse(path.string());
        } catch (const std::exception& e) {
            LogOnce(std::string("strings unavailable: ") + e.what());
        }
    });
    return strings ? &*strings : nullptr;
}

const toml::value* Find(std::string_view key) {
    const toml::value* node = Strings();
    while (node != nullptr && !key.empty()) {
        const size_t dot = key.find('.');
        const std::string part(key.substr(0, dot));
        key = dot == std::string_view::npos ? std::string_view{} : key.substr(dot + 1);
        node = node->is_table() && node->contains(part) ? &node->at(part) : nullptr;
    }
    return node;
}

} // namespace

void LogOnce(const std::string& message) {
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::lock_guard lock(mutex);
    if (logged.insert(message).second) RT_LOG(RT_TAG_ACCESSIBILITY) << message << std::endl;
}

std::string MessageEntry(uint32_t msbin, uint32_t index) {
    try {
        const uint32_t tableBytes = Memory::Read32(msbin);
        if (tableBytes < 4 || tableBytes > kMaxOffsetTableBytes || tableBytes % 4 != 0) {
            LogOnce("message table rejected: bad offset table");
            return {};
        }
        const uint32_t count = tableBytes / 4;
        if (index >= count) {
            LogOnce("message table rejected: entry out of range");
            return {};
        }
        const uint32_t start = Memory::Read32(msbin + 4 * index);
        const uint32_t end = index + 1 < count ? Memory::Read32(msbin + 4 * (index + 1)) : start + kMaxEntryBytes;
        if (start < tableBytes || end < start || end - start > kMaxEntryBytes) {
            LogOnce("message table rejected: bad entry bounds");
            return {};
        }
        std::string raw;
        for (uint32_t address = msbin + start; address < msbin + end; ++address) {
            const char c = static_cast<char>(Memory::Read8(address));
            if (c == '\0') break;
            raw += c;
        }
        auto text = Speakable(raw);
        if (!text) {
            LogOnce("message entry rejected: unknown markup");
            return {};
        }
        return std::move(*text);
    } catch (const Memory::AccessViolation&) {
        LogOnce("message table rejected: unmapped address");
        return {};
    }
}

std::string String(std::string_view key) {
    const toml::value* node = Find(key);
    if (node == nullptr || !node->is_string()) {
        LogOnce("missing string " + std::string(key));
        return {};
    }
    return node->as_string();
}

std::string StringAt(std::string_view key, size_t index) {
    const toml::value* node = Find(key);
    if (node == nullptr || !node->is_array() || index >= node->as_array().size() ||
        !node->as_array()[index].is_string()) {
        LogOnce("missing string " + std::string(key) + "[" + std::to_string(index) + "]");
        return {};
    }
    return node->as_array()[index].as_string();
}

std::string Position(size_t index, size_t total) {
    std::string text = String("position");
    for (const auto& [field, value] : {std::pair<std::string_view, size_t>{"{index}", index},
                                       std::pair<std::string_view, size_t>{"{total}", total}}) {
        if (const size_t at = text.find(field); at != std::string::npos) text.replace(at, field.size(), std::to_string(value));
    }
    return text;
}

std::string Join(std::initializer_list<std::string_view> parts) {
    std::string text;
    for (const std::string_view part : parts) {
        if (part.empty()) continue;
        if (!text.empty()) text += ", ";
        text += part;
    }
    return text;
}

} // namespace AccessibilityText
