#include "ui/UnsafeView.hpp"
#include "ui/terminal/Internal.hpp"

#include <map>
#include <string>
#include <vector>

namespace ke::ui {

using namespace term;

void TerminalPane::drawStatusBar(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals,
                                 const model::Evaluations& evals, chat::Chat& chat, const Session& session) {
    // The context counter for this Principal, from the Evaluations fold, and the THINKING switch.
    {
        std::string st = model::statusLine(evals, tab.principal);
        if (st.empty()) {
            st = "CTX none yet";
        }
        // Amber as the conversation nears the point where a wake compresses it first.
        bool nearLimit = false;
        if (info) {
            auto number = [info](const char* name) {
                const model::FieldState* f = info->field(name);
                return f && f->value.is_number() ? f->value.get<long>() : 0L;
            };
            const model::EvaluationInfo* last = evals.latestWithUsage(tab.principal);
            nearLimit = last && last->ctxIn > 0 &&
                        chat::nearCompression(static_cast<long>(last->ctxIn), chat::compressAt(number("serialize/compress-at"), number("serialize/budget-tokens")));
        }
        // The figures for now on the line; the running totals on hover.
        std::string shown = st.substr(0, st.find("TOTAL"));
        while (!shown.empty() && (shown.back() == ' ' || static_cast<unsigned char>(shown.back()) >= 0x80)) {
            shown.pop_back(); // the separator before the totals
        }
        text(nearLimit ? Ink::inspected : Ink::secondary, shown.empty() ? st : shown);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", (st + "\ncontext of the last model call and the running total for " + principals.nameOf(tab.principal)).c_str());
        }
        if (tasks_) {
            // Approvals waiting on this Principal (as approver) or asked by it (its own gated ops).
            const std::size_t forMe = tasks_->pendingFor(tab.principal);
            const std::size_t mine = tasks_->pendingRequestedBy(tab.principal);
            std::size_t total = 0; // the union: a Task root asked for and may decide counts once
            for (const model::TaskInfo* t : tasks_->pending()) {
                const bool approver = t->approvers.empty() ||
                                      std::find(t->approvers.begin(), t->approvers.end(), tab.principal) != t->approvers.end();
                if (approver || t->requestedBy == tab.principal) {
                    ++total;
                }
            }
            if (total > 0) {
                ImGui::SameLine();
                text(Ink::secondary, "·");
                ImGui::SameLine();
                chip(std::to_string(total) + " TO APPROVE", Ink::inspected);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%zu approval(s) waiting on %s, %zu asked by it; the cards are in the transcript", forMe,
                                      principals.nameOf(tab.principal).c_str(), mine);
                }
            }
        }
        // What is not needed every minute lives under ACTIONS, so the row stays one quiet line.
        auto occasional = [&] {
            if (info != nullptr && info->cls == "principal") {
                unsafeToggle(*info);
            }
            if (info && info->cls == "principal" && info->drivenBy != "person") {
                if (bracketButtonNamed("compress", "compress", session.attached())) {
                    sendDirective(tab, info, principals, chat, session, "/compress");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("replace the older part of this conversation with a summary; nothing is deleted");
                }
                ImGui::SameLine();
                if (bracketButtonNamed("off the record", "off-the-record", true, false, Ink::unresolved)) {
                    LinkRequest q;
                    q.kind = LinkRequest::Scratch;
                    q.object = tab.principal;
                    symbolUi().ask(std::move(q));
                }
            }
            ImGui::Separator();
        };
        // The world's named actions for this Principal (model/Actions): a menu, each item opening
        // its form in the app's dialog; nothing hardcoded, the world's list is the menu.
        if (actions_ && actions_->size() > 0) {
            // Facts on the left, the control on the right; it wraps under the facts when narrow.
            const float actionsW = ImGui::CalcTextSize("[ ACTIONS ]").x;
            const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            if (rightEdge - ImGui::GetItemRectMax().x > actionsW) {
                ImGui::SameLine(rightEdge - actionsW - ImGui::GetWindowPos().x);
            }
            if (bracketButton("actions")) {
                ImGui::OpenPopup("terminal-actions");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("the world's named actions (act {action args}), opened as forms");
            }
            if (ImGui::BeginPopup("terminal-actions")) {
                occasional();
                text(Ink::strong, "actions for " + principals.nameOf(tab.principal));
                ImGui::Separator();
                const auto item = [&](const model::ActionInfo* a) {
                    json args = json::object();
                    if (a->name == "workspace.mount" || a->name == "route.join") {
                        args["principal"] = tab.principal;
                    } else if (a->name == "route.create") {
                        args["members"] = json::array({tab.principal});
                    }
                    const std::string label = upper(a->name) + (a->gated ? "  (gated)" : "");
                    if (menuItem(label.c_str())) {
                        actionRequest_ = a->name;
                        actionArgs_ = args;
                    }
                    drive::hit("menu:" + a->name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                    if (ImGui::IsItemHovered() && !a->help.empty()) {
                        wrappedTooltip(a->help);
                    }
                };
                // Too many actions for one list: a family folds into a submenu, TASK stays on top.
                std::vector<std::string> families;
                std::map<std::string, std::vector<const model::ActionInfo*>> byFamily;
                for (const model::ActionInfo* a : actions_->ordered()) {
                    const std::string family = a->name.substr(0, a->name.find('.'));
                    if (byFamily[family].empty()) {
                        families.push_back(family);
                    }
                    byFamily[family].push_back(a);
                }
                for (const std::string& family : families) {
                    const auto& members = byFamily[family];
                    if (family == "task" || members.size() == 1) {
                        for (const model::ActionInfo* a : members) {
                            item(a);
                        }
                    }
                }
                for (const std::string& family : families) {
                    const auto& members = byFamily[family];
                    if (family != "task" && members.size() > 1 && beginMenu(upper(family).c_str())) {
                        for (const model::ActionInfo* a : members) {
                            item(a);
                        }
                        ImGui::EndMenu();
                    }
                }
                ImGui::EndPopup();
            }
        }
        const model::PrincipalInfo* workerInfo = principals.get(tab.worker);
        const bool loopRunning = workerInfo && workerInfo->loopIteration() > 0;
        const double now = steadySeconds();
        if (loopRunning && tab.escAt >= 0 && now - tab.escAt <= kEscEscSeconds + 1.0) {
            ImGui::SameLine();
            text(Ink::inspected, now - tab.escAt <= kEscEscSeconds ? "·  ESC again to stop" : "·  ESC ESC stops");
        }
    }
}

} // namespace ke::ui
