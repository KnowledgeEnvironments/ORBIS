// An agent's mark, drawn so no font has to carry it: a tick, a cross, a dot.
#pragma once

#include "model/Activity.hpp"

#include <imgui.h>

namespace ke::ui {

// Square, `size` wide, with its top left at `at`.
void activityMark(ImDrawList* dl, ImVec2 at, float size, model::Activity a, double seconds);
// As an item at the cursor, with its word as a tooltip.
void activityMarkItem(model::Activity a, double seconds);

} // namespace ke::ui
