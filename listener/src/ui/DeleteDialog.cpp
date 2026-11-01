#include "ui/DeleteDialog.hpp"

#include "drive/Commands.hpp"
#include "model/ObjectTree.hpp"
#include "ui/Theme.hpp"

#include <imgui_stdlib.h>

namespace ke::ui {

void DeleteDialog::open(const std::string& object) {
    object_ = object;
    reason_.clear();
    error_.clear();
    conflict_ = false;
    busy_ = false;
    open_ = true;
    justOpened_ = true;
}

void DeleteDialog::draw(const model::Principals& fold, chat::Chat& chat) {
    if (!open_) {
        return;
    }
    if (justOpened_) {
        ImGui::OpenPopup("DELETE");
        justOpened_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::CalcTextSize("0").x * 64, 0.0f));
    if (!ImGui::BeginPopupModal("DELETE", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    const std::string name = fold.nameOf(object_);
    const std::string blocked = model::deleteBlocked(fold, object_);
    text(Ink::strong, "Delete " + name + "?");
    ImGui::PushTextWrapPos(0.0f);
    text(Ink::primary, "It disappears from every list and from every prompt. Nothing is erased: the log keeps "
                            "its whole history, and SHOW DELETED in PRINCIPALS can restore it.");
    ImGui::PopTextWrapPos();
    dimText("%s", object_.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushStyleColor(ImGuiCol_Border, Ink::secondary);
    ImGui::InputTextWithHint("##reason", "why (optional, kept on the log)", &reason_);
    ImGui::PopStyleColor();
    drive::hit("input:delete-reason", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!blocked.empty()) {
        text(Ink::inspected, blocked);
    }
    if (!error_.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        text(conflict_ ? Ink::inspected : Ink::inspected, error_);
        ImGui::PopTextWrapPos();
    }
    auto retire = [this, &chat](bool force) {
        json args = {{"object", object_}};
        if (!reason_.empty()) {
            args["reason"] = reason_;
        }
        if (force) {
            args["force"] = true;
        }
        busy_ = true;
        error_.clear();
        chat.actNamed("object.retire", std::move(args), [this](const Response& r) {
            busy_ = false;
            if (r.ok) {
                open_ = false;
                return;
            }
            conflict_ = r.err && r.err->code == "conflict";
            error_ = r.err ? r.err->code + ": " + r.err->message : std::string("refused");
        });
    };
    dialogGap();
    if (bracketButtonNamed("delete", "confirm-delete", blocked.empty() && !busy_ && !conflict_)) {
        retire(false);
    }
    if (conflict_) {
        ImGui::SameLine();
        if (bracketButtonNamed("force", "force-delete", blocked.empty() && !busy_)) {
            retire(true);
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("delete it although the objects above still use it");
        }
    }
    ImGui::SameLine();
    if (bracketButtonNamed("cancel", "cancel-delete") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        open_ = false;
    }
    if (!open_) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace ke::ui
