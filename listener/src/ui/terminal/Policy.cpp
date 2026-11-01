#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

// What may run without asking, and what must ask. `policy/*` Fields change only from a person's
// own session; the world answers err person-only otherwise, and that is shown as it is.
void TerminalPane::drawPolicy(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat) {
    if (!info) {
        return;
    }
    dimText("what %s may do without asking; anything else waits in APPROVALS", principals.nameOf(tab.principal).c_str());
    if (!policyAdding_ && plusAtRight("rule-new", "add a standing rule")) {
        policyForm_.reset(chat::RuleDraft{}, "");
        policyAdding_ = true;
        policyStatus_.clear();
    }
    if (const model::FieldState* gate = info->field("policy/approval"); gate && gate->value.is_array()) {
        std::string words;
        for (const json& w : gate->value) {
            if (w.is_string()) {
                words += (words.empty() ? "" : " ") + w.get<std::string>();
            }
        }
        dimText("always asks: %s", words.c_str());
    }
    if (!policyStatus_.empty()) {
        text(Ink::inspected, policyStatus_);
    }
    const model::FieldState* field = info->field("policy/approval-rules");
    const std::vector<chat::ApprovalRule> rules = chat::parseRules(field ? field->value : json());
    if (rules.empty()) {
        dimText("no standing rules");
    } else if (ImGui::BeginTable("rules", 7, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
        for (const char* h : {"ACTION", "MATCH", "ENDS", "USED", "BY", "NOTE", ""}) {
            ImGui::TableSetupColumn(h);
        }
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < 7; ++c) {
            ImGui::TableSetColumnIndex(c);
            dimText("%s", ImGui::TableGetColumnName(c));
        }
        for (const chat::ApprovalRule& r : rules) {
            ImGui::TableNextRow();
            ImGui::PushID(r.id.c_str());
            ImGui::TableNextColumn();
            text(Ink::primary, r.action);
            ImGui::TableNextColumn();
            text(Ink::strong, chat::matchText(r.match));
            ImGui::TableNextColumn();
            text(Ink::secondary, chat::ruleLimit(r));
            ImGui::TableNextColumn();
            text(Ink::secondary, std::to_string(r.used));
            ImGui::TableNextColumn();
            text(Ink::secondary, principals.nameOf(r.by));
            ImGui::TableNextColumn();
            text(Ink::secondary, r.note);
            ImGui::TableNextColumn();
            if (bracketButtonNamed("remove", "rule-remove:" + chat::idPrefix(r.id))) {
                policyStatus_.clear();
                chat.actNamed("approval.rule.remove", json{{"principal", tab.principal}, {"rule", r.id}}, [this](const Response& res) {
                    if (!res.ok) {
                        policyStatus_ = res.err ? res.err->code + ": " + res.err->message : std::string("refused");
                    }
                });
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!policyAdding_) {
        return;
    }
    ImGui::SeparatorText("NEW RULE");
    policyForm_.draw(tab.principal, chat);
    if (bracketButtonNamed("add rule", "rule-add")) {
        json args = {{"principal", tab.principal}, {"action", "task.run"}, {"match", chat::draftMatch(policyForm_.draft)}};
        const json remember = chat::draftRemember(policyForm_.draft);
        if (remember.contains("until")) {
            args["until"] = remember["until"];
        }
        if (remember.contains("uses")) {
            args["uses"] = remember["uses"];
        }
        if (!policyForm_.draft.note.empty()) {
            args["note"] = policyForm_.draft.note;
        }
        chat.actNamed("approval.rule.add", std::move(args), [this](const Response& res) {
            if (res.ok) {
                policyAdding_ = false;
            } else {
                policyStatus_ = res.err ? res.err->code + ": " + res.err->message : std::string("refused");
            }
        });
    }
    ImGui::SameLine();
    if (bracketButtonNamed("cancel", "rule-new-cancel")) {
        policyAdding_ = false;
    }
}

} // namespace ke::ui
