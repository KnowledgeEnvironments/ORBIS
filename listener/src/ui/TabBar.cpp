#include "ui/TabBar.hpp"

#include "ui/ActivityMark.hpp"

#include "drive/Commands.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <cmath>
#include <imgui_internal.h>

namespace ke::ui {

namespace {

constexpr float kPad = 8.0f;
constexpr float kGap = 2.0f;

float closeWidth() { return ImGui::CalcTextSize("x").x + 6.0f; }

float tabWidthOf(const TabItem& t, bool withClose) {
    float w = kPad * 2 + ImGui::CalcTextSize(t.label.c_str()).x;
    if (!t.note.empty()) {
        w += ImGui::CalcTextSize(t.note.c_str()).x + kPad / 2;
    }
    if (!t.glyph.empty()) {
        w += ImGui::CalcTextSize(t.glyph.c_str()).x + kPad / 2;
    }
    if (t.dirty) {
        w += ImGui::CalcTextSize("*").x;
    }
    if (t.activity != model::Activity::None) {
        w += ImGui::GetTextLineHeight() + kPad / 2;
    }
    if (t.badge > 0) {
        w += ImGui::CalcTextSize(std::to_string(t.badge).c_str()).x + kPad;
    }
    if (withClose) {
        w += closeWidth(); // reserved on every closable tab, so widths never jump on hover
    }
    return std::floor(w);
}

} // namespace

float tabBarHeight() { return std::floor(ImGui::GetTextLineHeight() + 8.0f); }

TabBarResult tabBar(const char* id, const std::vector<TabItem>& tabs, int active, float width, TabBarState& state) {
    TabBarResult res;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = tabBarHeight();
    const float y0 = std::floor(pos.y), y1 = y0 + h;
    const float x0 = std::floor(pos.x);
    float total = 0.0f;
    std::vector<float> widths;
    for (const TabItem& t : tabs) {
        widths.push_back(tabWidthOf(t, t.closable));
        total += widths.back() + kGap;
    }
    int pinned = 0;
    float pinW = 0.0f;
    while (pinned < static_cast<int>(tabs.size()) && tabs[static_cast<std::size_t>(pinned)].pinned) {
        pinW += widths[static_cast<std::size_t>(pinned)] + kGap;
        ++pinned;
    }
    total -= pinW;
    const float listW = ImGui::CalcTextSize("v").x + 12.0f;
    const bool overflow = total + pinW > width;
    const float room = (overflow ? width - listW : width) - pinW;
    if (active != state.lastActive) {
        state.lastActive = active;
        float at = 0.0f;
        for (int i = pinned; i < active && i < static_cast<int>(widths.size()); ++i) {
            at += widths[static_cast<std::size_t>(i)] + kGap;
        }
        if (active >= pinned && active < static_cast<int>(widths.size())) {
            if (at < state.scroll) {
                state.scroll = at;
            } else if (at + widths[static_cast<std::size_t>(active)] > state.scroll + room) {
                state.scroll = at + widths[static_cast<std::size_t>(active)] - room;
            }
        }
    }
    state.scroll = overflow ? std::clamp(state.scroll, 0.0f, std::max(0.0f, total - room)) : 0.0f;

    ImGui::PushID(id);
    const float stripRight = x0 + pinW + room;
    float x = x0;
    float activeA = -1.0f, activeB = -1.0f;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (i == pinned) {
            x = x0 + pinW - state.scroll;
        }
        const TabItem& t = tabs[static_cast<std::size_t>(i)];
        const float w = widths[static_cast<std::size_t>(i)];
        const ImVec2 a(x, y0), b(x + w, y1);
        const bool isActive = i == active;
        const float left = i < pinned ? x0 : x0 + pinW; // what scrolls passes under what is pinned
        const bool visible = b.x > left && a.x < stripRight;
        if (visible) {
            dl->PushClipRect(ImVec2(left, y0), ImVec2(stripRight, y1 + 1.0f), true);
            ImGui::PushID(t.id.c_str());
            ImGui::SetCursorScreenPos(a);
            const float clippedW = std::min(b.x, stripRight) - std::max(a.x, left);
            ImGui::SetCursorScreenPos(ImVec2(std::max(a.x, left), y0));
            ImGui::SetNextItemAllowOverlap();
            const bool clicked = ImGui::InvisibleButton("tab", ImVec2(std::max(1.0f, clippedW), h),
                                                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                            ImGuiButtonFlags_MouseButtonMiddle);
            // A driven click can move the pointer between frames; activation is still an unambiguous
            // left pick. Keep release for right/middle and ordinary pointer clicks.
            const bool leftActivated = ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            const float closeW = closeWidth();
            const ImVec2 closeA(b.x - closeW - 2.0f, y0 + 2.0f), closeB(b.x - 2.0f, y1 - 1.0f);
            const bool overClose = t.closable && ImGui::IsMouseHoveringRect(closeA, closeB);
            // the x sits on top, and holds the press: the tab must stay hovered under both, or the x goes
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem |
                                                      ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            // The tab target means pick the tab, never its overlapping close button. If clipping
            // leaves only the close end visible, omit it so automation (and accessibility tooling)
            // uses the overflow list instead of issuing an ambiguous click.
            const ImVec2 tabA = ImGui::GetItemRectMin();
            const ImVec2 tabB(std::min(ImGui::GetItemRectMax().x, closeA.x), ImGui::GetItemRectMax().y);
            if (!t.closable || tabB.x > tabA.x + 1.0f) {
                drive::hit("tab:" + (t.target.empty() ? t.label : t.target), tabA, t.closable ? tabB : ImGui::GetItemRectMax());
            }
            if (clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Middle) && t.closable) {
                res.closed = i;
            } else if (clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                res.menu = i;
            } else if (!overClose && (clicked || leftActivated ||
                                      (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(a, b)))) {
                res.picked = i;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                res.dragged = i;
            }
            // The face: an active tab takes the content's surface and has no bottom edge.
            if (isActive) {
                dl->AddRectFilled(a, ImVec2(b.x, y1 + 1.0f), Surface::background);
                dl->AddLine(ImVec2(a.x, y1), a, Ink::nav);
                dl->AddLine(a, ImVec2(b.x, y0), Ink::nav);
                dl->AddLine(ImVec2(b.x, y0), ImVec2(b.x, y1), Ink::nav);
                activeA = a.x;
                activeB = b.x;
            } else {
                dl->AddRectFilled(ImVec2(a.x, y0 + 2.0f), b, hovered ? Surface::hover : Surface::code);
                dl->AddRect(ImVec2(a.x, y0 + 2.0f), ImVec2(b.x, y1 + 1.0f), Surface::codeBorder);
            }
            float tx = a.x + kPad;
            const float ty = y0 + std::floor((h - ImGui::GetTextLineHeight()) / 2);
            if (!t.glyph.empty()) {
                dl->AddText(ImVec2(tx, ty), t.attention ? Ink::inspected : Ink::secondary, t.glyph.c_str());
                tx += ImGui::CalcTextSize(t.glyph.c_str()).x + kPad / 2;
            }
            if (t.activity != model::Activity::None) {
                activityMark(dl, ImVec2(tx, ty), ImGui::GetTextLineHeight(), t.activity, ImGui::GetTime());
                tx += ImGui::GetTextLineHeight() + kPad / 2;
            }
            dl->AddText(ImVec2(tx, ty), isActive ? Ink::strong : hovered ? Ink::nav : Ink::primary, t.label.c_str());
            tx += ImGui::CalcTextSize(t.label.c_str()).x;
            if (!t.note.empty()) {
                tx += kPad / 2;
                dl->AddText(ImVec2(tx, ty), Ink::secondary, t.note.c_str());
                tx += ImGui::CalcTextSize(t.note.c_str()).x;
            }
            if (t.dirty) {
                dl->AddText(ImVec2(tx, ty), Ink::inspected, "*");
                tx += ImGui::CalcTextSize("*").x;
            }
            if (t.badge > 0) {
                const std::string n = std::to_string(t.badge);
                const float nw = ImGui::CalcTextSize(n.c_str()).x;
                dl->AddRectFilled(ImVec2(tx + kPad / 4, ty), ImVec2(tx + kPad * 0.75f + nw, ty + ImGui::GetTextLineHeight()), Ink::nav);
                dl->AddText(ImVec2(tx + kPad / 2, ty), Surface::background, n.c_str());
            }
            if (t.alarm) {
                dl->AddRectFilled(ImVec2(a.x + 2.0f, y0 + 4.0f), ImVec2(a.x + 6.0f, y0 + 8.0f), Ink::unsafe);
            }
            if (t.closable && (hovered || isActive)) {
                ImGui::SetCursorScreenPos(closeA);
                const bool closing = ImGui::InvisibleButton("close", ImVec2(closeB.x - closeA.x, closeB.y - closeA.y));
                const bool closeHover = ImGui::IsItemHovered();
                drive::hit("close:" + (t.target.empty() ? t.label : t.target), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (closeHover) {
                    dl->AddRectFilled(closeA, closeB, Surface::selection);
                }
                dl->AddText(ImVec2(closeA.x + 3.0f, ty), closeHover ? Ink::onSelection : Ink::secondary, "x");
                if (closing) {
                    res.closed = i;
                    res.picked = -1;
                }
            } else if (t.closable && drive::active()) {
                // A test closes what a person can: register where the `x` would appear.
                drive::hit("close:" + (t.target.empty() ? t.label : t.target), closeA, closeB);
            }
            ImGui::PopID();
            dl->PopClipRect();
        }
        x += w + kGap;
    }
    // The baseline: under everything except the active tab, which stays open into its content.
    const float right = x0 + width;
    if (activeA >= 0.0f) {
        dl->AddLine(ImVec2(x0, y1), ImVec2(std::max(x0, activeA), y1), Ink::nav);
        dl->AddLine(ImVec2(std::min(right, activeB), y1), ImVec2(right, y1), Ink::nav);
    } else {
        dl->AddLine(ImVec2(x0, y1), ImVec2(right, y1), Ink::nav);
    }
    if (overflow) {
        const ImVec2 at(stripRight, y0);
        ImGui::SetCursorScreenPos(at);
        if (ImGui::InvisibleButton("list", ImVec2(listW, h))) {
            ImGui::OpenPopup("tablist");
        }
        drive::hit(std::string("tablist:") + id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        dl->AddText(ImVec2(at.x + 6.0f, y0 + 4.0f), ImGui::IsItemHovered() ? Ink::nav : Ink::secondary, "v");
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(x0 + pinW, y0), ImVec2(stripRight, y1))) {
            state.scroll -= ImGui::GetIO().MouseWheel * 24.0f + ImGui::GetIO().MouseWheelH * 24.0f;
        }
        if (ImGui::BeginPopup("tablist")) {
            for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
                const TabItem& t = tabs[static_cast<std::size_t>(i)];
                const std::string row = (t.glyph.empty() ? std::string() : t.glyph + " ") + t.label + (t.note.empty() ? "" : " " + t.note);
                if (ImGui::Selectable(row.c_str(), i == active)) {
                    res.picked = i;
                }
                drive::hit("tablist-item:" + (t.target.empty() ? t.label : t.target), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, y1 + 1.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(pos.x, y1 + 2.0f));
    return res;
}

int subTabs(const char* id, const char* const* names, int count, int active) {
    int result = active;
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    float x = pos.x + 4.0f;
    float y = pos.y;
    const float h = ImGui::GetTextLineHeight() + 2.0f;
    const float right = pos.x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < count; ++i) {
        const std::string shown = upper(names[i]); // the label case; the drive target keeps the name
        const ImVec2 size = ImGui::CalcTextSize(shown.c_str());
        if (x + size.x + 8.0f > right && x > pos.x + 4.0f) { // a narrow pane: the strip wraps
            x = pos.x + 4.0f;
            y += h;
        }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("sub", ImVec2(size.x + 8.0f, h))) {
            result = i;
        }
        drive::hit(std::string("tab:") + names[i], ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddText(ImVec2(x + 4.0f, y + 1.0f), i == active ? Ink::strong : hovered ? Ink::nav : Ink::secondary, shown.c_str());
        if (i == active) {
            dl->AddLine(ImVec2(x + 4.0f, y + h - 1.0f), ImVec2(x + 4.0f + size.x, y + h - 1.0f), Ink::nav);
        }
        x += size.x + 12.0f;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(right - pos.x, y - pos.y + h));
    return result;
}

float subChoiceWidth(const char* lead, const char* const* names, int count) {
    float w = ImGui::CalcTextSize(upper(lead).c_str()).x + 8.0f;
    for (int i = 0; i < count; ++i) {
        w += ImGui::CalcTextSize(upper(names[i]).c_str()).x + 12.0f;
    }
    return w;
}

int subChoice(const char* id, const char* lead, const char* const* names, const char* const* targets, int count, int active) {
    int result = active;
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight() + 2.0f;
    const std::string leadText = upper(lead);
    dl->AddText(ImVec2(pos.x, pos.y + 1.0f), Ink::secondary, leadText.c_str());
    float x = pos.x + ImGui::CalcTextSize(leadText.c_str()).x + 8.0f;
    for (int i = 0; i < count; ++i) {
        const std::string shown = upper(names[i]);
        const ImVec2 size = ImGui::CalcTextSize(shown.c_str());
        ImGui::SetCursorScreenPos(ImVec2(x - 4.0f, pos.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("choice", ImVec2(size.x + 8.0f, h))) {
            result = i;
        }
        drive::hit(std::string("button:") + targets[i], ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddText(ImVec2(x, pos.y + 1.0f), i == active ? Ink::strong : hovered ? Ink::nav : Ink::secondary, shown.c_str());
        if (i == active) {
            dl->AddLine(ImVec2(x, pos.y + h - 1.0f), ImVec2(x + size.x, pos.y + h - 1.0f), Ink::nav);
        }
        x += size.x + 12.0f;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(x - pos.x, h));
    return result;
}

} // namespace ke::ui
