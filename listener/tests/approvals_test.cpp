#include <catch2/catch_test_macros.hpp>

#include "chat/Approvals.hpp"

using namespace ke;
using namespace ke::chat;
using nlohmann::json;

namespace {
model::TaskInfo shellTask(const std::string& id, const std::string& by, const std::string& cmd) {
    model::TaskInfo t;
    t.id = id;
    t.kind = "approval";
    t.state = "pending";
    t.requestedBy = by;
    t.op = json{{"action", "task.run"}, {"args", {{"cmd", cmd}}}};
    return t;
}
} // namespace

TEST_CASE("the inbox groups by who asked and keeps first-seen order") {
    const auto a = shellTask("1", "grid", "ls");
    const auto b = shellTask("2", "atlas", "make");
    const auto c = shellTask("3", "grid", "pwd");
    const auto groups = groupApprovals({&a, &b, &c});
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].requester == "grid");
    CHECK(groups[0].tasks.size() == 2);
    CHECK(groups[1].requester == "atlas");
}

TEST_CASE("a draft starts from the program and leaves out what is empty") {
    const auto t = shellTask("1", "grid", "git status --short");
    RuleDraft d = draftFor(t);
    CHECK(d.programs == "git");
    CHECK(approvalAction(t) == "task.run");
    CHECK(draftMatch(d) == json{{"programs", {"git"}}});
    d.argsPrefix = "status --short";
    d.preset = "read-only";
    d.uses = 3;
    d.until = "2026-11-01T00:00:00Z";
    const json r = draftRemember(d);
    CHECK(r["match"]["args-prefix"] == json{"status", "--short"});
    CHECK(r["match"]["preset"] == "read-only");
    CHECK(r["uses"] == 3);
    CHECK(r["until"] == "2026-11-01T00:00:00Z");
    CHECK(draftRemember(RuleDraft{}).empty());
}

TEST_CASE("a stored rule reads back in plain words") {
    const json field = json::array({json{{"id", "r1"}, {"action", "task.run"}, {"match", {{"programs", {"git", "ls"}}, {"preset", "read-only"}}},
                                         {"uses-left", 1}, {"used", 4}, {"by", "p"}, {"note", "reads"}},
                                    json{{"action", "no id"}}, json("junk")});
    const auto rules = parseRules(field);
    REQUIRE(rules.size() == 1);
    CHECK(matchText(rules[0].match) == "preset read-only · programs git ls");
    CHECK(ruleLimit(rules[0]) == "1 use left");
    ApprovalRule open;
    CHECK(ruleLimit(open) == "no end");
    CHECK(matchText(json::object()) == "anything");
    CHECK(instantAfter(0, 3600) == "1970-01-01T01:00:00Z");
}

TEST_CASE("the command is found however the op was spelled") {
    model::TaskInfo t;
    t.op = json{{"op", "act"}, {"args", {{"action", "run"}, {"args", {{"cmd", "ls -la"}}}}}};
    CHECK(approvalCommand(t) == "ls -la");
    t.op = json{{"op", "grant"}, {"args", {{"to", "x"}}}};
    CHECK(approvalCommand(t).empty());
    CHECK(approvalAction(t) == "grant");
}

TEST_CASE("a rule-admitted run is one line") {
    auto t = shellTask("1", "grid", "git status");
    t.fields["task/exit"].value = 0;
    CHECK(autoRunLine(t, "reads") == "auto · rule reads · git status → exit 0");
}
