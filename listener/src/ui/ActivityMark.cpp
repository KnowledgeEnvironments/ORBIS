#include "ui/ActivityMark.hpp"

#include "ui/Theme.hpp"

#include <cmath>

namespace ke::ui {

void activityMark(ImDrawList* dl, ImVec2 at, float size, model::Activity a, double seconds) {
    const float s = std::floor(size);
    const ImVec2 c(at.x + s * 0.5f, at.y + s * 0.5f);
    const float thick = std::max(1.5f, s * 0.14f);
    switch (a) {
    case model::Activity::NeedsYou:
        dl->AddRectFilled(ImVec2(c.x - thick * 0.6f, at.y + s * 0.12f), ImVec2(c.x + thick * 0.6f, at.y + s * 0.62f), Ink::inspected);
        dl->AddRectFilled(ImVec2(c.x - thick * 0.6f, at.y + s * 0.74f), ImVec2(c.x + thick * 0.6f, at.y + s * 0.74f + thick * 1.2f), Ink::inspected);
        break;
    case model::Activity::Failed:
        dl->AddLine(ImVec2(at.x + s * 0.2f, at.y + s * 0.2f), ImVec2(at.x + s * 0.8f, at.y + s * 0.8f), Ink::fatal, thick);
        dl->AddLine(ImVec2(at.x + s * 0.8f, at.y + s * 0.2f), ImVec2(at.x + s * 0.2f, at.y + s * 0.8f), Ink::fatal, thick);
        break;
    case model::Activity::Finished: {
        const ImVec2 tick[3] = {ImVec2(at.x + s * 0.14f, at.y + s * 0.55f), ImVec2(at.x + s * 0.40f, at.y + s * 0.80f),
                                ImVec2(at.x + s * 0.88f, at.y + s * 0.20f)};
        dl->AddPolyline(tick, 3, stateColor(true, false), thick);
        break;
    }
    case model::Activity::Working:
        hazardTape(dl, ImVec2(at.x, at.y + s * 0.3f), ImVec2(at.x + s, at.y + s * 0.7f), seconds);
        break;
    case model::Activity::Idle:
        dl->AddCircleFilled(c, s * 0.15f, Ink::secondary);
        break;
    case model::Activity::Inactive:
        dl->AddCircle(c, s * 0.2f, Ink::secondary, 0, 1.0f);
        break;
    case model::Activity::None:
        break;
    }
}

void activityMarkItem(model::Activity a, double seconds) {
    const float s = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(s, s));
    activityMark(ImGui::GetWindowDrawList(), at, s, a, seconds);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", model::word(a));
    }
}

} // namespace ke::ui
