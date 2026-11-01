// The approvals inbox: everything waiting on a decision in one place, by who asked, so a person is
// not hunting through transcripts. Rows stay where they are until the window is cleared, so a
// decided row never moves the one under the cursor.
#pragma once

#include "chat/Approvals.hpp"
#include "chat/Chat.hpp"
#include "ui/RuleForm.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

namespace ke::ui {

class ApprovalsPane {
  public:
    void toggle() { open_ = !open_; }
    bool open() const { return open_; }
    void draw(const model::Tasks& tasks, const model::Principals& principals, chat::Chat& chat, ImFont* mono);

  private:
    void note(const model::Tasks& tasks);
    void decide(chat::Chat& chat, const std::vector<std::string>& ids, const char* decision);
    void approveOne(chat::Chat& chat, const model::TaskInfo& t, bool remember);
    void drawRow(const model::TaskInfo& t, const model::Principals& principals);
    void drawDetail(const model::TaskInfo& t, const model::Principals& principals, chat::Chat& chat);
    void drawEditor(const model::TaskInfo& t, chat::Chat& chat);
    std::vector<std::string> targets() const;
    void askAll(std::vector<std::string> ids, const std::string& who);
    void drawUnsafeLog(const model::Tasks& tasks, const model::Principals& principals);

    bool open_ = false;
    std::vector<std::string> rows_; // first-seen order, kept until CLEAR DECIDED
    std::set<std::string> selected_;
    std::string cursor_;
    std::string reason_;
    std::string note_;
    std::map<std::string, std::string> outcome_;

    RuleForm form_;
    bool editing_ = false;
    float detailH_ = 0.0f;            // the detail's content height, last frame
    std::vector<std::string> allIds_; // APPROVE ALL waiting on its confirm
    std::string allWho_;
};

} // namespace ke::ui
