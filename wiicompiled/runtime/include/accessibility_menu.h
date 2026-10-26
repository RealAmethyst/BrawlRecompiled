#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct CpuContext;

// Options, Accessibility (src/accessibility/accessibility_menu.cpp): the accessibility settings, as an eighth Options
// item the runtime draws and speaks itself. The game's Options screen keeps its seven buttons; the eighth sits right of
// Erase Data as game cursor 7, which no game button highlights. Its page reads the game's own menu input (so button
// mappings and every controller work) while the Options screen receives none.
namespace AccessibilityMenu {

constexpr uint16_t kEntryCursor = 7;

// The entry is there: the game's code is the disc's (a mod that changes the menu leaves Options as it was).
bool Available();
// Items on the Options screen the player can reach: 8 with the entry, else the game's 7.
size_t OptionsItemCount();
// "Accessibility, <description>, 8 of 8": the entry as the Options screen speaks it.
std::string EntryLine();

// muProcMenu::moveCursor (main_menu.cpp's hook) for the Options screen: moves onto and off the entry. Returns true
// when it handled the move (r3 holds the result, 1 when the cursor moved); false leaves it to the game.
bool MoveOptionsCursor(CpuContext* ctx);

// Once per frame inside the ImGui frame (settings_overlay::Draw): the entry on the Options screen, then the page.
void Draw();

} // namespace AccessibilityMenu
