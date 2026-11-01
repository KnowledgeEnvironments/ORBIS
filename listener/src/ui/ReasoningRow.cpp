#include "ui/ReasoningRow.hpp"

#include "ui/Theme.hpp"

#include <imgui.h>

namespace ke::ui {

namespace {
constexpr const char* kCost = "reasoning on adds tens of seconds per call on this model";

float word(const char* s) {
    return ImGui::CalcTextSize(s).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
}

float modeWidth() { return ImGui::CalcTextSize(upper("REASONING").c_str()).x + ImGui::GetStyle().ItemSpacing.x + word("OFF") + word("ON"); }
float showWidth() { return word("SHOW"); }
} // namespace

std::optional<std::string> drawReasoningRow(const ReasoningRow& row, bool& show) {
    std::optional<std::string> chosen;
    if (row.settable) {
        text(Ink::secondary, upper("REASONING"));
        ImGui::SameLine();
        const auto choice = [&](const char* label, const char* value) {
            if (bracketButtonNamed(label, row.scope + "reasoning-" + value, true, row.mode == value) && row.mode != value) {
                chosen = value;
            }
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("%s\n%s\none message only: start it with /think or /fast", kCost, row.source.c_str());
            }
        };
        choice("off", "off");
        ImGui::SameLine();
        choice("on", "on");
        if (ImGui::GetContentRegionAvail().x > showWidth()) {
            ImGui::SameLine();
        }
    }
    if (bracketButtonNamed("show", row.scope + "reasoning-show", true, show)) {
        show = !show;
    }
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("display only: shows or hides reasoning above each reply; it does not change what the model does");
    }
    return chosen;
}

float reasoningRowHeight(float width, bool settable) {
    const float row = ImGui::GetFrameHeightWithSpacing();
    return settable && width < modeWidth() + showWidth() ? row * 2.0f : row;
}

} // namespace ke::ui
