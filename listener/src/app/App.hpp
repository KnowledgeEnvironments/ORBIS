// The windowed listener: SDL3 window, SDL_GPU (Metal) device, Dear ImGui docking (ADR 0005).
// Idle-friendly: the loop blocks in SDL_WaitEventTimeout and renders only after events; the
// network thread wakes it with an SDL user event. The screensaver stays enabled (user rule).
#pragma once

#include "app/Options.hpp"

namespace ke {

int runApp(const Options& opts);

}
