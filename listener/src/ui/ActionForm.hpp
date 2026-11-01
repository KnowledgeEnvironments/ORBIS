// The widgets for an action's argument form (sprint 1 L6): see ui/ActionSchema.hpp for the
// schema parsing and the values; this half needs ImGui and the theme.
#pragma once

#include "ui/ActionSchema.hpp"

#include <imgui.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ke::ui {

// ---- widgets ------------------------------------------------------------------------------------
struct FormContext {
    // Models etc. for Id args named in `idChoices` (name -> list of {id, label}).
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> idChoices;
    std::string driveScope; // drive target prefix: `input:new:` -> `input:new:<arg>`
    float labelWidth = 0.0f; // 0: measured from the longest name
};
// Draws the widgets for `specs` into `v`; returns true when something changed this frame.
bool drawForm(const std::vector<ArgSpec>& specs, FormValues& v, const FormContext& ctx, const std::string& path = "");

} // namespace ke::ui
