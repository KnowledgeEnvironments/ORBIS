// Unsafe mode on screen: the red indicator in the main window, the confirm that arms a Principal,
// the mark on its tab and tree row. Red in every theme, never blinking.
#pragma once

#include "chat/Chat.hpp"
#include "chat/Unsafe.hpp"
#include "model/Principals.hpp"

#include <imgui.h>
#include <string>
#include <vector>

namespace ke::ui {

// Set each frame from the Runtime; the marks read it.
void setUnsafe(const std::vector<chat::UnsafeEntry>& entries);
bool isUnsafe(const std::string& principal);
// The label's width, so the bar keeps room for it at any window size.
float unsafeIndicatorWidth(const model::Principals& principals, double nowEpoch);
// `UNSAFE ivy 23m` in the bar; a click lists them with one-click off.
void unsafeIndicator(const model::Principals& principals, chat::Chat& chat, double nowEpoch);
// `UNSAFE MODE (disable approvals)` for one Principal, or its off switch while armed.
void unsafeToggle(const model::PrincipalInfo& principal);
// `LEAVE THE BOX` for a shell Adapter, or its way back while it is out.
void unconfineToggle(const model::PrincipalInfo& adapter);
// Offer one of a person's terminals to one agent: the SHARE dialog opens next frame.
void askShareTerminal(const std::string& pane);
// The confirm, once per frame.
void unsafeDialog(const model::Principals& principals, chat::Chat& chat);
// The small red square on a tab or row.
void unsafeMark(ImDrawList* dl, ImVec2 at, float size);

} // namespace ke::ui
