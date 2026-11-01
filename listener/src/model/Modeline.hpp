// The status line along the bottom of the main window: what is running, then what just happened.
// Plain text from the record, nothing to click. No SDL, no ImGui.
#pragma once

#include "model/Event.hpp"
#include "model/Principals.hpp"

#include <string>
#include <vector>

namespace ke::model {

// One event in a few words, or "" for what nobody needs to see: reads, streaming and bookkeeping.
std::string describeEvent(const Event& e, const Principals& principals);

// `root running // person set instructions/charter on root`, `idle` on a quiet world.
std::string modeline(const std::vector<Event>& rows, const Principals& principals);

} // namespace ke::model
