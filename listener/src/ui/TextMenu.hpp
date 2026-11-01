// Cut, Copy, Paste and Select All on every text field: a right click, or a long press on touch.
// One menu, once per frame; the fields report themselves through ui/ImguiHooks, so a new field
// has it without a line of code. The menu's action reaches the field the same way a shortcut does.
#pragma once

#include <imgui.h>

namespace ke::ui {

// Before ImGui::NewFrame: the last chosen action's key events.
void textMenuPump();
// After the panes, inside the frame.
void textMenu();
// The same event a key chord makes, for whatever has the keyboard: EDIT in the menu bar.
void sendShortcut(ImGuiKey key);

} // namespace ke::ui
