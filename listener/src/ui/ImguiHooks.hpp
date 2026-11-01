// See ImguiHooks.cpp.
#pragma once

namespace ke::ui {

struct InputSeen {
    unsigned int id = 0; // the text field under the mouse this frame, 0 for none
    unsigned int window = 0;
};

const InputSeen& inputUnderMouse();
void clearInputSeen();
void enableItemHooks(); // once, after the ImGui context exists

} // namespace ke::ui
