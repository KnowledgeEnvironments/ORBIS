// The approvals inbox and the rule editor, pure: pending Tasks by who asked, the command behind a
// gated op, and a standing rule as a person drafts it and as the world stores it.
#pragma once

#include "model/Tasks.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ke::chat {

struct ApprovalGroup {
    std::string requester;
    std::vector<const model::TaskInfo*> tasks;
};

// In first-seen order, so a group never moves when another gains a Task.
std::vector<ApprovalGroup> groupApprovals(const std::vector<const model::TaskInfo*>& pending);

// The command of a gated shell run, "" for any other op.
std::string approvalCommand(const model::TaskInfo& t);
// The gate word a rule would name: task.run for a shell run, else the action or op.
std::string approvalAction(const model::TaskInfo& t);

// A rule as typed: lists are space separated.
struct RuleDraft {
    std::string programs;
    std::string argsPrefix;
    std::string paths;
    std::string preset;
    std::string until; // ISO-8601, "" for no end
    int uses = 0;      // 0 for unlimited
    std::string note;
};

// Starts from the command's program, the narrowest useful rule.
RuleDraft draftFor(const model::TaskInfo& t);
json draftMatch(const RuleDraft& d);
// `task.approve`'s `remember`.
json draftRemember(const RuleDraft& d);
// An instant `seconds` after `nowEpoch`.
std::string instantAfter(double nowEpoch, long seconds);

struct ApprovalRule {
    std::string id;
    std::string action;
    json match;
    std::string until;
    std::optional<long> usesLeft;
    long used = 0;
    std::string by;
    std::string at;
    std::string note;
};

// `policy/approval-rules` as the world stores it; anything unreadable is skipped.
std::vector<ApprovalRule> parseRules(const json& field);
// `programs git ls · preset read-only · paths src`.
std::string matchText(const json& match);
// What ends the rule: `3 uses left`, `until 2026-11-01`, `no end`.
std::string ruleLimit(const ApprovalRule& r);

// One line for a run a rule admitted, under `system` in the transcript.
std::string autoRunLine(const model::TaskInfo& t, const std::string& ruleNote);

} // namespace ke::chat
