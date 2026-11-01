#include "ui/RuleForm.hpp"

#include "drive/Commands.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <chrono>

#include <imgui_stdlib.h>

namespace ke::ui {

namespace {

constexpr const char* kUntil[] = {"no end", "1 hour", "1 day", "1 week"};
constexpr long kUntilSeconds[] = {0, 3600, 86400, 604800};
constexpr const char* kPresets[] = {"no preset", "read-only", "test"};

double nowEpoch() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void labelled(const char* label, const char* id, std::string* value, const char* hint) {
    dimText("%-10s", label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint(id, hint, value);
    drive::hit(std::string("input:rule:") + (id + 2), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace

void RuleForm::reset(chat::RuleDraft d, std::string cmd) {
    draft = std::move(d);
    tryCmd = std::move(cmd);
    testedKey_.clear();
    result_.clear();
    matches_ = false;
    untilChoice_ = 0;
}

void RuleForm::test(const std::string& principal, chat::Chat& chat) {
    if (tryCmd.empty()) {
        result_.clear();
        return;
    }
    const json match = chat::draftMatch(draft);
    const std::string key = principal + "\n" + tryCmd + "\n" + match.dump();
    if (key == testedKey_ || testing_) {
        return;
    }
    testedKey_ = key;
    testing_ = true;
    chat.actNamed("approval.rule.test", json{{"principal", principal}, {"cmd", tryCmd}, {"match", match}}, [this](const Response& r) {
        testing_ = false;
        if (!r.ok) {
            matches_ = false;
            result_ = r.err ? r.err->code + ": " + r.err->message : std::string("refused");
            return;
        }
        const json& v = r.result.contains("result") ? r.result["result"] : r.result;
        matches_ = v.value("matches", false);
        result_ = v.value("reason", std::string());
    });
}

void RuleForm::draw(const std::string& principal, chat::Chat& chat) {
    chat::RuleDraft& d = draft;
    labelled("programs", "##programs", &d.programs, "git ls");
    labelled("arguments", "##args-prefix", &d.argsPrefix, "the command must start with these");
    labelled("paths", "##paths", &d.paths, "paths it may touch, inside the Adapter's root");
    dimText("%-10s", "preset");
    ImGui::SameLine();
    for (const char* p : kPresets) {
        const std::string value = p == kPresets[0] ? std::string() : std::string(p);
        if (bracketButtonNamed(p, std::string("rule-preset:") + p, true, d.preset == value)) {
            d.preset = value;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    dimText("%-10s", "ends");
    ImGui::SameLine();
    for (int i = 0; i < 4; ++i) {
        if (bracketButtonNamed(kUntil[i], std::string("rule-until:") + kUntil[i], true, untilChoice_ == i)) {
            untilChoice_ = i;
            d.until = i == 0 ? std::string() : chat::instantAfter(nowEpoch(), kUntilSeconds[i]);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    dimText("%-10s", "uses");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 12);
    ImGui::InputInt("##uses", &d.uses, 0, 0);
    d.uses = std::max(0, d.uses);
    drive::hit("input:rule:uses", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    dimText("0 for no limit");
    labelled("note", "##note", &d.note, "why this is safe");
    labelled("try", "##try", &tryCmd, "a command to test the rule against");
    test(principal, chat);
    if (!tryCmd.empty() && !result_.empty()) {
        text(matches_ ? Ink::primary : Ink::inspected, (matches_ ? "would run: " : "would still ask: ") + result_);
    }
}

} // namespace ke::ui
