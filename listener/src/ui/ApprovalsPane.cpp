#include "chat/Unsafe.hpp"
#include "ui/ApprovalsPane.hpp"

#include "drive/Commands.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <chrono>

#include <imgui_stdlib.h>

namespace ke::ui {

namespace {

double nowEpoch() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

void ApprovalsPane::note(const model::Tasks& tasks) {
    for (const model::TaskInfo* t : tasks.pending()) {
        if (std::find(rows_.begin(), rows_.end(), t->id) == rows_.end()) {
            rows_.push_back(t->id);
        }
    }
    if (cursor_.empty() || std::find(rows_.begin(), rows_.end(), cursor_) == rows_.end()) {
        cursor_ = rows_.empty() ? std::string() : rows_.front();
    }
}

std::vector<std::string> ApprovalsPane::targets() const {
    if (!selected_.empty()) {
        return std::vector<std::string>(selected_.begin(), selected_.end());
    }
    return cursor_.empty() ? std::vector<std::string>{} : std::vector<std::string>{cursor_};
}

void ApprovalsPane::decide(chat::Chat& chat, const std::vector<std::string>& ids, const char* decision) {
    if (ids.empty()) {
        return;
    }
    json list = json::array();
    for (const std::string& id : ids) {
        list.push_back(id);
        outcome_[id] = std::string(decision) + "…";
    }
    json args = {{"tasks", std::move(list)}, {"decision", decision}};
    if (!reason_.empty()) {
        args["reason"] = reason_;
    } else if (std::string_view(decision) == "deny") {
        args["reason"] = "denied from the inbox";
    }
    chat.actNamed("task.decide-many", std::move(args), [this, ids](const Response& r) {
        if (!r.ok) {
            for (const std::string& id : ids) {
                outcome_[id] = "err " + (r.err ? r.err->code + " " + r.err->message : std::string("refused"));
            }
            return;
        }
        if (r.result.contains("results") && r.result["results"].is_array()) {
            for (const json& one : r.result["results"]) {
                const std::string id = one.value("task", "");
                outcome_[id] = one.value("ok", false) ? one.value("state", std::string("done")) : "err " + one.dump();
            }
        }
    });
    selected_.clear();
    reason_.clear();
}

void ApprovalsPane::approveOne(chat::Chat& chat, const model::TaskInfo& t, bool remember) {
    json args = {{"task", t.id}};
    if (!note_.empty()) {
        args["note"] = note_;
    }
    if (remember && editing_) {
        args["remember"] = chat::draftRemember(form_.draft);
    }
    outcome_[t.id] = "approve…";
    chat.actNamed("task.approve", std::move(args), [this, id = t.id, remember](const Response& r) {
        if (!r.ok) {
            outcome_[id] = "err " + (r.err ? r.err->code + " " + r.err->message : std::string("refused"));
            return;
        }
        outcome_[id] = remember ? "approved and remembered" : "approved";
    });
    editing_ = false;
}

void ApprovalsPane::drawRow(const model::TaskInfo& t, const model::Principals& principals) {
    ImGui::PushID(t.id.c_str());
    const std::string shortId = chat::idPrefix(t.id);
    bool sel = selected_.count(t.id) != 0;
    const bool live = t.pending();
    ImGui::BeginDisabled(!live);
    if (ImGui::Checkbox("##pick", &sel)) {
        if (sel) {
            selected_.insert(t.id);
        } else {
            selected_.erase(t.id);
        }
    }
    drive::hit("check:approval:" + shortId, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();
    ImGui::SameLine();
    const std::string line = model::opSummary(t.op, [&principals](const std::string& id) { return principals.nameOf(id); });
    ImGui::PushStyleColor(ImGuiCol_Text, live ? Ink::strong : Ink::secondary);
    if (ImGui::Selectable((line + "##row").c_str(), cursor_ == t.id, ImGuiSelectableFlags_AllowOverlap)) {
        if (cursor_ != t.id) {
            editing_ = false;
        }
        cursor_ = t.id;
    }
    ImGui::PopStyleColor();
    drive::hit("approval:" + shortId, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!live) {
        ImGui::SameLine();
        text(Ink::secondary, model::collapsedLine(t, [&principals](const std::string& id) { return principals.nameOf(id); }));
    } else if (auto it = outcome_.find(t.id); it != outcome_.end()) {
        ImGui::SameLine();
        text(it->second.rfind("err", 0) == 0 ? Ink::inspected : Ink::secondary, it->second);
    }
    ImGui::PopID();
}

void ApprovalsPane::drawEditor(const model::TaskInfo& t, chat::Chat& chat) {
    ImGui::SeparatorText("ALWAYS ALLOW");
    if (bracketButtonNamed("approve and remember", "approve-remember", t.pending(), false, Ink::primary)) {
        approveOne(chat, t, true);
        return;
    }
    ImGui::SameLine();
    if (bracketButtonNamed("approve once", "approve-once", t.pending())) {
        approveOne(chat, t, false);
        return;
    }
    ImGui::SameLine();
    if (bracketButtonNamed("cancel", "rule-cancel")) {
        editing_ = false;
        return;
    }
    form_.draw(t.requestedBy, chat); // under the buttons, so a short window never hides them
}

void ApprovalsPane::drawDetail(const model::TaskInfo& t, const model::Principals& principals, chat::Chat& chat) {
    const model::NameFn nameOf = [&principals](const std::string& id) { return principals.nameOf(id); };
    text(Ink::strong, principals.nameOf(t.requestedBy) + " asks to:");
    ImGui::PushTextWrapPos(0.0f);
    for (const model::OpLine& l : model::opDetail(t.op, nameOf)) {
        text(l.code ? Ink::inspected : Ink::primary, l.text);
    }
    if (!t.summary.empty()) {
        text(Ink::secondary, t.summary);
    }
    ImGui::PopTextWrapPos();
    const std::string left = model::timeLeft(t, nowEpoch());
    if (!left.empty()) {
        text(left == "expired" ? Ink::inspected : Ink::secondary, left == "expired" ? left : left + " left");
    }
    if (editing_) {
        drawEditor(t, chat);
        return;
    }
    if (!t.pending()) {
        return;
    }
    if (bracketButtonNamed("approve", "approve", true, false, Ink::primary)) {
        approveOne(chat, t, false);
    }
    ImGui::SameLine();
    if (bracketButtonNamed("deny", "deny")) {
        decide(chat, {t.id}, "deny");
    }
    if (!chat::approvalCommand(t).empty()) {
        ImGui::SameLine();
        if (bracketButtonNamed("always\xe2\x80\xa6", "always")) {
            form_.reset(chat::draftFor(t), chat::approvalCommand(t));
            editing_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("approve this and let commands like it run without asking");
        }
    }
}

namespace {

constexpr float kUnsafeLogLines = 6.0f;

std::vector<const model::TaskInfo*> ranUnsafeNow(const model::Tasks& tasks) { return chat::ranUnsafe(tasks.ordered()); }

} // namespace

void ApprovalsPane::askAll(std::vector<std::string> ids, const std::string& who) {
    allIds_ = std::move(ids);
    allWho_ = who;
}

// What ran with approvals off: a dim line each, so unsafe mode leaves something to read.
void ApprovalsPane::drawUnsafeLog(const model::Tasks& tasks, const model::Principals& principals) {
    const std::vector<const model::TaskInfo*> ran = ranUnsafeNow(tasks);
    if (ran.empty()) {
        return;
    }
    dimText("RAN UNDER UNSAFE MODE");
    const auto nameOf = [&principals](const std::string& id) { return principals.nameOf(id); };
    if (ImGui::BeginChild("unsafe-log", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * kUnsafeLogLines),
                          ImGuiChildFlags_AlwaysUseWindowPadding)) {
        for (const model::TaskInfo* t : ran) {
            text(Ink::secondary, principals.nameOf(t->requestedBy) + "  " + model::opSummary(t->op, nameOf));
        }
    }
    ImGui::EndChild();
}

void ApprovalsPane::draw(const model::Tasks& tasks, const model::Principals& principals, chat::Chat& chat, ImFont* mono) {
    note(tasks);
    if (!open_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::CalcTextSize("0").x * 92, ImGui::GetTextLineHeight() * 34), ImGuiCond_FirstUseEver);
    // No window scrollbar: the wells scroll, and one here would take its width from the right inset.
    if (!ImGui::Begin("APPROVALS###approvals", &open_,
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(mono, 0.0f);
    drive::hit("pane:approvals", ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                                                              ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
    std::vector<const model::TaskInfo*> shown;
    for (const std::string& id : rows_) {
        if (const model::TaskInfo* t = tasks.get(id)) {
            shown.push_back(t);
        }
    }
    const std::vector<chat::ApprovalGroup> groups = chat::groupApprovals(shown);
    const bool any = !targets().empty();
    if (bracketButtonNamed("approve selected", "approve-selected", any && !selected_.empty(), false, Ink::primary)) {
        decide(chat, targets(), "approve");
    }
    ImGui::SameLine();
    if (bracketButtonNamed("deny selected", "deny-selected", any && !selected_.empty())) {
        decide(chat, targets(), "deny");
    }
    ImGui::SameLine();
    {
        // Everything waiting, overall: the confirm says how many.
        std::vector<std::string> live;
        for (const model::TaskInfo* t : shown) {
            if (t->pending()) {
                live.push_back(t->id);
            }
        }
        if (bracketButtonNamed("approve all", "approve-all", !live.empty())) {
            askAll(std::move(live), "everyone");
        }
    }
    ImGui::SameLine();
    if (bracketButtonNamed("clear decided", "clear-decided", shown.size() != tasks.pending().size())) {
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [&tasks](const std::string& id) {
                        const model::TaskInfo* t = tasks.get(id);
                        return !t || !t->pending();
                    }), rows_.end());
    }
    ImGui::SetNextItemWidth(-FLT_MIN); // its own line: the hint reads whole at any width
    ImGui::InputTextWithHint("##reason", "reason for this decision, kept on the Task", &reason_);
    drive::hit("input:approval-reason", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const float logH = ranUnsafeNow(tasks).empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * (kUnsafeLogLines + 1.0f);
    const float room = ImGui::GetContentRegionAvail().y - logH;
    // The queue takes what the detail leaves, down to the window's own inset: never a gap below it.
    const model::TaskInfo* detail = tasks.get(cursor_);
    const float detailH = detail ? std::clamp(detailH_, ImGui::GetTextLineHeight() * 3.0f, room * (editing_ ? 0.7f : 0.5f)) : 0.0f;
    const float listH = std::max(room - detailH - (detail ? ImGui::GetStyle().ItemSpacing.y : 0.0f), ImGui::GetTextLineHeight() * 5);
    // One inset on every side of both wells: the theme's window padding.
    if (ImGui::BeginChild("rows", ImVec2(0, listH), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        if (groups.empty()) {
            dimText("nothing is waiting on you");
        }
        for (const chat::ApprovalGroup& g : groups) {
            ImGui::PushID(g.requester.c_str());
            const std::string who = principals.nameOf(g.requester);
            text(Ink::inspected, who.empty() ? std::string("unknown") : who);
            std::vector<std::string> live;
            for (const model::TaskInfo* t : g.tasks) {
                if (t->pending()) {
                    live.push_back(t->id);
                }
            }
            ImGui::SameLine();
            const std::string all = "approve all from " + who;
            if (bracketButtonNamed(all.c_str(), "approve-all:" + who, !live.empty())) {
                askAll(live, who);
            }
            for (const model::TaskInfo* t : g.tasks) {
                drawRow(*t, principals);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (detail != nullptr) {
        if (ImGui::BeginChild("detail", ImVec2(0, detailH), ImGuiChildFlags_AlwaysUseWindowPadding)) {
            drawDetail(*detail, principals, chat);
            detailH_ = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y; // what it needs, for the next frame
        }
        ImGui::EndChild();
        insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    drawUnsafeLog(tasks, principals);
    if (!allIds_.empty() && !ImGui::IsPopupOpen("APPROVE ALL")) {
        ImGui::OpenPopup("APPROVE ALL");
    }
    if (ImGui::BeginPopupModal("APPROVE ALL", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        text(Ink::strong, "Approve " + std::to_string(allIds_.size()) + " waiting from " + allWho_ + "?");
        dialogGap();
        if (bracketButtonNamed("APPROVE", "approve-all-yes", true, false, Ink::primary)) {
            decide(chat, allIds_, "approve");
            allIds_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (bracketButtonNamed("CANCEL", "approve-all-no") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            allIds_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // a, d and the arrows, while no box has the keyboard
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        std::vector<std::string> order;
        for (const chat::ApprovalGroup& g : groups) {
            for (const model::TaskInfo* t : g.tasks) {
                order.push_back(t->id);
            }
        }
        auto at = std::find(order.begin(), order.end(), cursor_);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && at != order.end() && at + 1 != order.end()) {
            cursor_ = *(at + 1);
            editing_ = false;
        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && at != order.begin() && at != order.end()) {
            cursor_ = *(at - 1);
            editing_ = false;
        } else if (ImGui::IsKeyPressed(ImGuiKey_A, false) && any) {
            decide(chat, targets(), "approve");
        } else if (ImGui::IsKeyPressed(ImGuiKey_D, false) && any) {
            decide(chat, targets(), "deny");
        }
    }
    ImGui::PopFont();
    ImGui::End();
}

} // namespace ke::ui
