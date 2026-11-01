// Small things every module draws the same way.
#pragma once

#include "modules/Module.hpp"

#include <imgui.h>
#include <string>

namespace ke::modules {

inline ImVec2 topLeft(const Rect& r) { return ImVec2(r.x, r.y); }
inline ImVec2 bottomRight(const Rect& r) { return ImVec2(r.x + r.w, r.y + r.h); }
inline bool hoveredRect(const Rect& r) { return ImGui::IsMouseHoveringRect(topLeft(r), bottomRight(r), true); }

// Cmd on a Mac (ImGui swaps it to Ctrl), Ctrl elsewhere: the shortcut key.
inline bool shortcut(ImGuiKey key) { return ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(key, false); }
// The key a person would call Ctrl, which on a Mac ImGui hands over as Super.
inline bool physicalCtrl() {
    const ImGuiIO& io = ImGui::GetIO();
    return io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
}

// The module's view of a world err: `err conflict the file changed`, plainly.
inline std::string errLine(const Response& r) {
    return r.err ? "err " + r.err->code + (r.err->message.empty() ? "" : " " + r.err->message) : std::string("err");
}

} // namespace ke::modules
