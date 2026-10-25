#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

// Text for screen-reader speech (speech.h). Every function returns an empty string when it cannot verify its source,
// and logs why: speaking nothing beats speaking something wrong.
namespace AccessibilityText {

// Entry `index` of a message table (.msbin) the game has loaded at guest address `msbin`, as speakable text: markup
// removed, line breaks and tabs as spaces, full-width forms as ASCII.
std::string MessageEntry(uint32_t msbin, uint32_t index);

// The accessibility layer's own wording (assets/accessibility/strings_en.toml beside the executable), by dotted key.
std::string String(std::string_view key);

// Item `index` of the string array at `key`.
std::string StringAt(std::string_view key, size_t index);

// "3 of 6": a menu item's 1-based place among the items the player can reach.
std::string Position(size_t index, size_t total);

// Logs a problem the first time it occurs only, so a broken source does not flood the log on every cursor move.
void LogOnce(const std::string& message);

// Joins the non-empty parts with ", ", the separator between the fields of one spoken item.
std::string Join(std::initializer_list<std::string_view> parts);

} // namespace AccessibilityText
