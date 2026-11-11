// The drive API's small public surface for UI code: named hit-rects. A pane registers the
// rectangle of a thing a test might want to click (`tab:GRAPH`, `principal:root`,
// `button:STOP`) with one call per frame; the registry is cleared every frame and is a no-op
// unless a --drive socket is open, so the panes pay nothing in normal runs.
//
// Target names resolved by `click {target}` / `drag` / `scroll`:
//   pane:<TITLE>        an ImGui window by title (PRINCIPALS, HISTORY, SESSION, BIND, ...)
//   tab:<NAME>          a slanted tab (CHAT, GRAPH, TRANSCRIPT, PROMPT, FIELDS, a Principal)
//   principal:<NAME>    a row of the PRINCIPALS pane, by bound name
//   button:<WORD>       a bracket button ([ STOP ], [ THINKING ON ], [ SEND BIND OPS ], ...)
//   link:<text>         a linked symbol on screen (root.route, seq 680)
//   input               the current terminal's input line
//   <x>,<y>             window coordinates in points
#pragma once

#include <imgui.h>
#include <string>

namespace ke::drive {

// True while a --drive socket is open.
bool active();
// Register a named rectangle for this frame (screen coordinates in points).
void hit(const std::string& name, ImVec2 min, ImVec2 max);

} // namespace ke::drive
