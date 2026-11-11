#include "ui/Chrome.hpp"

#include "ui/Theme.hpp"

#include <algorithm>
#include <cmath>

namespace ke::ui {

namespace {

constexpr bool visible(Rgba c) {
    return (c >> 24) != 0;
}

void bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, const ChromeSpec& s) {
    const float t = s.bevel;
    const ImU32 topLeft = s.inset ? s.low : s.high;
    const ImU32 bottomRight = s.inset ? s.high : s.low;
    dl->AddRectFilled(a, ImVec2(b.x, a.y + t), topLeft);
    dl->AddRectFilled(a, ImVec2(a.x + t, b.y), topLeft);
    dl->AddRectFilled(ImVec2(a.x, b.y - t), b, bottomRight);
    dl->AddRectFilled(ImVec2(b.x - t, a.y), b, bottomRight);
}

} // namespace

void insetRim(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    ChromeSpec s = theme().role(ChromeRole::InsetWell);
    s.inset = true;
    s.bevel = s.bevel > 0.0f ? s.bevel : std::max(1.0f, std::floor(ImGui::GetStyle().FramePadding.y * 0.5f));
    bevel(dl, min, max, s);
}

void chrome(ImDrawList* dl, ImVec2 min, ImVec2 max, ChromeRole role) {
    const ChromeSpec& s = theme().role(role);
    if (visible(s.face)) {
        dl->AddRectFilled(min, max, s.face);
    }
    if (s.bevel > 0.0f) {
        bevel(dl, min, max, s);
    } else if (visible(s.high)) {
        dl->AddRect(min, max, s.high);
    }
}

} // namespace ke::ui
