#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "guest_code_patch.h"

// Text for screen-reader speech (speech.h). Every function returns an empty string when it cannot verify its source,
// and logs why: speaking nothing beats speaking something wrong.
namespace AccessibilityText {

// Entry `index` of a message table (.msbin) the game has loaded at guest address `msbin`, as speakable text: markup
// removed, line breaks and tabs as spaces, full-width forms as ASCII.
std::string MessageEntry(uint32_t msbin, uint32_t index);

// Entry `index` of a message table file on the game disc (a path under files/, e.g. "menu/msg/stage_name_en.msbin"),
// for text the game shows from a table it has not loaded at that moment. Files are read once and kept.
std::string DiscMessageEntry(std::string_view path, uint32_t index);

// Host-side UTF-8 game text (copied out of guest memory) as speakable text: markup removed, full-width forms folded.
std::string Prepare(const std::string& text);

// `length` bytes of game text at guest `address` (raw message text, markup included), as speakable text.
std::string GuestText(uint32_t address, uint32_t length);

// Up to `maxUnits` UTF-16 code units at guest `address` (ending early at a NUL), as UTF-8: player-entered names.
std::string GuestUtf16(uint32_t address, size_t maxUnits);

// A NUL-terminated string the game keeps in its own data (at most `maxBytes`), as speakable text.
std::string GuestCString(uint32_t address, size_t maxBytes);

// The accessibility layer's own wording (assets/accessibility/strings_en.toml beside the executable), by dotted key.
std::string String(std::string_view key);

// Item `index` of the string array at `key`.
std::string StringAt(std::string_view key, size_t index);

// "3 of 6": a menu item's 1-based place among the items the player can reach.
std::string Position(size_t index, size_t total);

// "40 percent": a slider's position, which the game shows only as a position.
std::string Percent(unsigned value);

// True once a mod has patched the game's code (Gecko codes, SD mods): hooks then stay silent, since the functions and
// layouts they read may no longer be the disc's.
inline bool SilencedByMods() { return GuestCodePatch::Active(); }

// Logs a problem the first time it occurs only, so a broken source does not flood the log on every cursor move.
void LogOnce(const std::string& message);

// Joins the non-empty parts with ", ", the separator between the fields of one spoken item.
std::string Join(std::initializer_list<std::string_view> parts);

} // namespace AccessibilityText
