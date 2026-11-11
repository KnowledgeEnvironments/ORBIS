#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

void TerminalPane::decide(Tab& tab, chat::Chat& chat, const std::string& task, bool approve) {
    json args = {{"task", task}};
    if (!approve) {
        const std::string reason = tab.denyReasons[task];
        args["reason"] = reason.empty() ? std::string("denied from the listener") : reason;
    }
    tab.taskResults[task] = approve ? "approving\u2026" : "denying\u2026";
    tab.taskResultErr[task] = false;
    std::map<std::string, std::string>* results = &tab.taskResults;
    std::map<std::string, bool>* errs = &tab.taskResultErr;
    chat.actNamed(approve ? "task.approve" : "task.deny", args, [results, errs, task](const Response& r) {
        if (r.ok) {
            (*results)[task] = "ok seq " + std::to_string(r.result.value("seq", r.seq));
            (*errs)[task] = false;
        } else {
            (*results)[task] = "err " + (r.err ? r.err->code + (r.err->message.empty() ? "" : " " + r.err->message) : std::string("?"));
            (*errs)[task] = true;
        }
    });
}

void TerminalPane::drawCards(Tab& tab, model::Principals& principals, chat::Chat& chat, const Session& session,
                             ImVec2 origin, float wrap) {
    (void)principals;
    View& view = tab.view_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = ImGui::CalcTextSize("0").x;
    const double nowEpoch = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const ImVec2 after = ImGui::GetCursorScreenPos();
    std::string held;
    for (const Card& c : view.cards) {
        float y0 = 0.0f, y1 = 0.0f;
        if (!paraSpan(view.layout, c.firstPara, c.lastPara, y0, y1)) {
            continue;
        }
        const ImVec2 a(origin.x + 1.0f, origin.y + y0 - 3.0f);
        const ImVec2 b(origin.x + wrap - 1.0f, origin.y + y1 + 3.0f);
        outlineRect(dl, a, b, c.pending ? Ink::inspected : Ink::secondary);
        if (c.pending && ImGui::IsMouseHoveringRect(a, b, false)) {
            held = c.task;
        }
        const model::TaskInfo* task = tasks_ ? tasks_->get(c.task) : nullptr;
        model::TaskInfo unknown; // pending, no expiry known
        if (!task) {
            unknown.id = c.task;
            unknown.kind = "approval";
            unknown.state = "pending";
            task = &unknown;
            // the card came from the Message alone (the Task's events may predate our Grant): read it once
            if (tasks_ && session.attached() && !taskReads_.count(c.task)) {
                taskReads_.insert(c.task);
                model::Tasks* tasks = tasks_;
                chat.readObject(c.task, [tasks, id = c.task](const Response& r) {
                    if (r.ok && r.result.is_object()) {
                        tasks->seed(model::SnapshotObject::parse(r.result));
                    }
                });
            }
        }
        if (!c.pending) {
            continue;
        }
        float cy0 = 0.0f, cy1 = 0.0f;
        if (!paraSpan(view.layout, c.controlsPara, c.controlsPara, cy0, cy1)) {
            continue;
        }
        const std::string shortId = chat::idPrefix(c.task);
        // The controls row, drawn as ordinary items placed on the slot the document left for it.
        // The row above may have ended on SameLine; an empty item clears that, or this row's
        // SameLine would use the last row's height and throw DENY off the screen.
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 2.0f * cw, origin.y + cy0));
        ImGui::PushID(c.task.c_str());
        const bool busy = tab.taskResults.count(c.task) && !tab.taskResultErr[c.task] && tab.taskResults[c.task].rfind("ok", 0) != 0 &&
                          tab.taskResults[c.task].find("\u2026") != std::string::npos;
        const bool open = task->pending();
        if (bracketButtonNamed("approve", "approve:" + shortId, session.attached() && !busy && open, open, Ink::primary)) {
            decide(tab, chat, c.task, true);
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("act task.approve {task %s}", c.task.c_str());
        }
        ImGui::SameLine();
        if (bracketButtonNamed("deny", "deny:" + shortId, session.attached() && !busy && open, false, Ink::inspected)) {
            decide(tab, chat, c.task, false);
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("act task.deny {task %s reason}", c.task.c_str());
        }
        if (c.rows >= 2) {
            // the second reserved row: the reason box and the time left, never over the next block
            float ry0 = 0.0f, ry1 = 0.0f;
            if (paraSpan(view.layout, c.controlsPara + 1, c.controlsPara + 1, ry0, ry1)) {
                ImGui::SetCursorScreenPos(ImVec2(origin.x + 2.0f * cw, origin.y + ry0));
            } else {
                ImGui::SameLine();
            }
        } else {
            ImGui::SameLine();
        }
        std::string& reason = tab.denyReasons[c.task];
        const float rowAvail = origin.x + wrap - ImGui::GetCursorScreenPos().x;
        ImGui::SetNextItemWidth(c.rows >= 2 ? std::max(cw * 12.0f, rowAvail - cw * 18.0f)
                                            : std::max(cw * 16.0f, std::min(cw * 40.0f, rowAvail - cw * 24.0f)));
        ImGui::InputTextWithHint("##reason", "reason (for DENY)", &reason);
        drive::hit("input:deny-reason:" + shortId, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        const std::string left = model::timeLeft(*task, nowEpoch);
        if (!left.empty()) {
            text(left == "expired" ? Ink::inspected : Ink::secondary, left == "expired" ? left : left + " left");
            ImGui::SameLine();
        } else {
            text(Ink::secondary, "pending"); // no expiry known yet: the Task's Fields are still on their way
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("the Task's Fields have not reached this listener yet; reading it");
            }
            ImGui::SameLine();
        }
        if (auto it = tab.taskResults.find(c.task); it != tab.taskResults.end()) {
            text(tab.taskResultErr[c.task] ? Ink::inspected : Ink::secondary, it->second);
        }
        ImGui::PopID();
    }
    tab.heldCard = held;
    ui::restoreCursor(after);
    ImGui::Dummy(ImVec2(0.0f, 0.0f)); // an item at the restored cursor, so the move extends nothing (ImGui's rule)
}

} // namespace ke::ui
