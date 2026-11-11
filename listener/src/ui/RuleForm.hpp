// A standing approval rule as a form, shared by the inbox's ALWAYS and the POLICY tab. It tests the
// draft against a command through `approval.rule.test` as it is typed, so a person sees what the
// rule would admit before keeping it.
#pragma once

#include "chat/Approvals.hpp"
#include "chat/Chat.hpp"

#include <string>

namespace ke::ui {

struct RuleForm {
    chat::RuleDraft draft;
    std::string tryCmd;

    void reset(chat::RuleDraft d, std::string cmd);
    void draw(const std::string& principal, chat::Chat& chat);

  private:
    void test(const std::string& principal, chat::Chat& chat);
    std::string testedKey_;
    std::string result_;
    bool matches_ = false;
    bool testing_ = false;
    int untilChoice_ = 0;
};

} // namespace ke::ui
