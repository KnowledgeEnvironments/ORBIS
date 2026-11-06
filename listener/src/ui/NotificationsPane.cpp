#include "ui/NotificationsPane.hpp"

#include "drive/Commands.hpp"
#include "ui/Chrome.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/Theme.hpp"

#include <string>

namespace ke::ui {

namespace {

ImU32 levelInk(const std::string& level) { return chat::loud(level) ? Ink::inspected : Ink::secondary; }

void markRead(chat::Notifications& notifications, chat::Chat& chat, const std::string& id) {
    notifications.markRead({id});
    chat.actNamed("notify.read", json{{"notifications", json::array({id})}}, [](const Response&) {});
}

} // namespace

bool noticesChip(const chat::Notifications& notifications, bool open, bool menuBar) {
    const int unread = notifications.unread();
    const std::string label = (menuBar ? upper("notices") : std::string("notices")) + " " + std::to_string(unread);
    const bool hot = notifications.worstUnread().has_value() && chat::loud(*notifications.worstUnread());
    const ImU32 ink = hot ? Ink::inspected : unread > 0 ? Ink::primary : Ink::secondary;
    const bool clicked = menuBar ? menuBarItem(label, "notices", open, ink) : bracketButtonNamed(label.c_str(), "notices", true, open, ink);
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("what the world's Principals have told you");
    }
    return clicked;
}

void NotificationsPane::draw(chat::Notifications& notifications, const model::Principals& principals, chat::Chat& chat, ImFont* mono) {
    if (!open_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::CalcTextSize("0").x * 72, ImGui::GetTextLineHeight() * 26), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("NOTICES###notices", &open_, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(mono, 0.0f);
    drive::hit("pane:notices", ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                                                             ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
    if (bracketButtonNamed("mark all read", "notices-read", notifications.unread() > 0)) {
        notifications.markAllRead();
        chat.actNamed("notify.read", json::object(), [](const Response&) {});
    }
    ImGui::SameLine();
    dimText("%d unread", notifications.unread());
    ImGui::BeginChild("notices-well", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    touchDragScroll();
    if (notifications.rows().empty()) {
        dimText("nothing yet: a Principal can tell you things here without a Route");
    }
    int row = 0;
    const float wrap = ImGui::GetContentRegionAvail().x;
    // A copy of the row: a click below changes the list being walked.
    for (const chat::Notification n : notifications.rows()) {
        ImGui::PushID(n.id.c_str());
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float textH = ImGui::CalcTextSize(n.text.c_str(), nullptr, false, wrap).y;
        const float h = ImGui::GetTextLineHeightWithSpacing() + textH + ImGui::GetStyle().ItemSpacing.y;
        ImGui::SetNextItemAllowOverlap();
        const bool clicked = ImGui::InvisibleButton("row", ImVec2(wrap, h));
        const bool hot = ImGui::IsItemHovered();
        drive::hit("notice:" + std::to_string(row), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hot) {
            dl->AddRectFilled(start, ImVec2(start.x + wrap, start.y + h), Surface::hover);
        }
        ImGui::SetCursorScreenPos(start);
        text(n.read ? Ink::secondary : levelInk(n.level), upper(n.level.empty() ? "info" : n.level));
        ImGui::SameLine();
        text(n.read ? Ink::secondary : Ink::nav, principals.nameOf(n.from));
        ImGui::SameLine();
        dimText("%s", n.at.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::PushStyleColor(ImGuiCol_Text, n.read ? Ink::secondary : Ink::primary);
        ImGui::TextUnformatted(n.text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        if (hot && !n.object.empty()) {
            ke::ui::tip("about %s: click opens it", principals.nameOf(n.object).c_str());
        }
        if (clicked) {
            if (!n.read) {
                markRead(notifications, chat, n.id);
            }
            if (!n.object.empty()) {
                LinkRequest r;
                const model::PrincipalInfo* p = principals.get(n.object);
                r.kind = p != nullptr && p->cls == "principal" ? LinkRequest::Terminal : LinkRequest::Inspect;
                r.object = n.object;
                symbolUi().ask(std::move(r));
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + h));
        ImGui::Dummy(ImVec2(wrap, ImGui::GetStyle().ItemSpacing.y));
        ImGui::PopID();
        ++row;
    }
    ImGui::EndChild();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopFont();
    ImGui::End();
}

} // namespace ke::ui
