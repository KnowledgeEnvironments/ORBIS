// The seven surfaces every pane is drawn with. Panes consume these and never restyle locally: what
// each role looks like comes from the current ThemeSpec.
#pragma once

#include "ui/ThemeSpec.hpp"

#include <imgui.h>

namespace ke::ui {

// A role over this rectangle: its face, then its bevel, or a one pixel outline when it has none.
void chrome(ImDrawList* dl, ImVec2 min, ImVec2 max, ChromeRole role);

} // namespace ke::ui
