// Approval gates and named actions as the listener folds them (sprint 1 L5/L6): a Task's
// lifecycle from the events the world appends, the card's text helpers, an approval Message's
// kind and task, and the world's `action/*` Objects with their schemas.
#include "chat/MessageCache.hpp"
#include "model/Actions.hpp"
#include "model/Evaluations.hpp"
#include "model/Snapshot.hpp"
#include "model/Tasks.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

constexpr const char* kWorld = "d470bce8-5578-400f-8f31-3504f178c6a8";
constexpr const char* kRoot = "034968ad-3358-4e57-a31b-d1bde0c15b31";
constexpr const char* kPerson = "28b70d40-2916-4adb-967a-2dea363599dc";
constexpr const char* kAtlas = "9b60b24f-1111-4000-8000-000000000001";
constexpr const char* kShell = "3f816b7e-4a28-5277-91c4-52eb8c718e99";
constexpr const char* kTask = "7a1b2c3d-0000-4000-8000-000000000042";
constexpr const char* kTask2 = "7a1b2c3d-0000-4000-8000-000000000043";
constexpr const char* kEval = "03d1ac77-b9e5-498b-b28e-da884b8ddfea";
constexpr const char* kMsg = "d67530c2-5f89-4c9a-a319-1d2fb171e740";
constexpr const char* kAction = "aa11bb22-0000-4000-8000-000000000007";

Event ev(Seq seq, const char* kind, const char* stream, const char* actor, json body) {
    Event e;
    e.seq = seq;
    e.id = "0b5a2c3e-3333-4c2a-8f1e-" + std::to_string(100000000000 + seq);
    e.stream = stream;
    e.actor = actor;
    e.kind = kind;
    e.at = "2026-10-30T20:" + std::string(seq % 60 < 10 ? "0" : "") + std::to_string(seq % 60) + ":00.000Z";
    e.body = std::move(body);
    return e;
}

json runOp() {
    return {{"action", "task.run"}, {"args", {{"adapter", kShell}, {"cmd", "make test-unit"}, {"timeout-s", 120}}}};
}

Event approvalCreated(Seq seq) {
    return ev(seq, "created", kTask, kRoot,
              {{"class", "task"},
               {"fields",
                {{"task/kind", "approval"},
                 {"task/state", "pending"},
                 {"task/op", runOp()},
                 {"task/summary", "task.run: make test-unit (shell.local)"},
                 {"task/requested-by", kRoot},
                 {"task/evaluation", kEval},
                 {"task/message", kMsg},
                 {"task/approvers", json::array({kPerson})},
                 {"task/expires-at", "2026-10-30T21:00:00.000Z"}}}});
}

std::string nameOf(const std::string& id) {
    if (id == kRoot) {
        return "root";
    }
    if (id == kPerson) {
        return "person";
    }
    if (id == kAtlas) {
        return "atlas";
    }
    if (id == kShell) {
        return "shell.local";
    }
    return id.substr(0, 8);
}

} // namespace

TEST_CASE("tasks: an approval is pending, counted for its approver and requester, then decided", "[tasks]") {
    Tasks t;
    REQUIRE(t.apply(approvalCreated(10)));
    const TaskInfo* a = t.get(kTask);
    REQUIRE(a != nullptr);
    CHECK(a->approval());
    CHECK(a->pending());
    CHECK(a->requestedBy == kRoot);
    CHECK(a->approvers == std::vector<std::string>{kPerson});
    CHECK(a->summary == "task.run: make test-unit (shell.local)");
    CHECK(t.pending().size() == 1);
    CHECK(t.pendingFor(kPerson) == 1);
    CHECK(t.pendingFor(kAtlas) == 0); // not an approver
    CHECK(t.pendingRequestedBy(kRoot) == 1);
    CHECK(t.pendingRequestedBy(kPerson) == 0);
    CHECK(opSummary(a->op, nameOf) == "task.run: make test-unit (shell.local)");
    const std::vector<OpLine> lines = opDetail(a->op, nameOf);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "make test-unit");
    CHECK(lines[0].code);
    CHECK(lines[1].text == "adapter shell.local · timeout 120 s");
    CHECK(timeLeft(*a, epochSeconds("2026-10-30T20:55:48.000Z")) == "4 m 12 s");
    CHECK(timeLeft(*a, epochSeconds("2026-10-30T21:00:01.000Z")) == "expired");

    // the person approves: task-moved by the person, then the world runs it and records the result
    REQUIRE(t.apply(ev(11, "task-moved", kTask, kPerson, {{"task", kTask}, {"from", "pending"}, {"to", "approved"}})));
    CHECK_FALSE(a->pending());
    CHECK(a->decidedBy == kPerson);
    CHECK(collapsedLine(*a, nameOf) == "approved by person · running");
    REQUIRE(t.apply(ev(12, "field-set", kTask, kWorld,
                       {{"name", "task/result"}, {"type", "map"}, {"value", {{"exit", 0}, {"seq", 99}}}})));
    REQUIRE(t.apply(ev(13, "task-moved", kTask, kWorld, {{"task", kTask}, {"from", "approved"}, {"to", "done"}})));
    CHECK(collapsedLine(*a, nameOf) == "approved by person · ran · exit 0");
    CHECK(t.pending().empty());
    CHECK(t.pendingFor(kPerson) == 0);

    // a second one, denied with a reason written as Fields before the move
    const json grantOp = {{"op", "grant"},
                          {"args", {{"grantee", kAtlas}, {"scope", {{"prefix", "atlas"}}}, {"ops", json::array({"read", "write"})}}}};
    const json grantFields = {{"task/kind", "approval"}, {"task/state", "pending"}, {"task/op", grantOp}};
    REQUIRE(t.apply(ev(20, "created", kTask2, kAtlas, {{"class", "task"}, {"fields", grantFields}})));
    const TaskInfo* b = t.get(kTask2);
    REQUIRE(b != nullptr);
    CHECK(b->requestedBy == kAtlas); // the actor when the Field is absent
    CHECK(b->approvers.empty());
    CHECK(t.pendingFor(kPerson) == 1); // no approvers named: anyone may decide
    CHECK(opSummary(b->op, nameOf) == "grant read, write on prefix atlas to atlas");
    REQUIRE(t.apply(ev(21, "field-set", kTask2, kPerson, {{"name", "task/reason"}, {"type", "string"}, {"value", "not now"}})));
    const json refType = json::array({"ref", "principal"});
    REQUIRE(t.apply(ev(22, "field-set", kTask2, kPerson, {{"name", "task/decided-by"}, {"type", refType}, {"value", kPerson}})));
    REQUIRE(t.apply(ev(23, "field-set", kTask2, kPerson, {{"name", "task/state"}, {"type", "keyword"}, {"value", "denied"}})));
    CHECK_FALSE(b->pending());
    CHECK(collapsedLine(*b, nameOf) == "denied by person: not now");
    // dupes and older seqs are ignored
    CHECK_FALSE(t.apply(ev(23, "task-moved", kTask2, kPerson, {{"task", kTask2}, {"to", "pending"}})));
    CHECK(b->state == "denied");
}

TEST_CASE("tasks: a Task first seen through a task/* Field or a move still folds", "[tasks]") {
    Tasks t;
    // the created event predates the Grant that let this listener see the Task: never arrives
    REQUIRE(t.apply(ev(40, "field-set", kTask, kRoot, {{"name", "task/kind"}, {"type", "keyword"}, {"value", "approval"}})));
    REQUIRE(t.apply(ev(41, "field-set", kTask, kRoot, {{"name", "task/summary"}, {"type", "string"}, {"value", "run x"}})));
    REQUIRE(t.get(kTask) != nullptr);
    CHECK(t.get(kTask)->pending());
    CHECK(t.pending().size() == 1);
    REQUIRE(t.apply(ev(42, "task-moved", kTask2, kPerson, {{"task", kTask2}, {"from", "pending"}, {"to", "denied"}})));
    REQUIRE(t.get(kTask2) != nullptr);
    CHECK(t.get(kTask2)->state == "denied");
    CHECK(t.get(kTask2)->approval()); // the approval lifecycle names the kind
    CHECK_FALSE(t.apply(ev(43, "field-set", kRoot, kRoot, {{"name", "loop/motto"}, {"type", "string"}, {"value", "x"}}))); // not a task
}

TEST_CASE("tasks: a shell run is a plain Task, not an approval", "[tasks]") {
    Tasks t;
    REQUIRE(t.apply(ev(5, "created", kTask, kRoot,
                       {{"class", "task"}, {"fields", {{"task/owner", kRoot}, {"task/state", "open"}, {"task/title", "make test-unit"}}}})));
    REQUIRE(t.apply(ev(6, "task-moved", kTask, kRoot, {{"task", kTask}, {"from", "open"}, {"to", "active"}})));
    const TaskInfo* a = t.get(kTask);
    REQUIRE(a != nullptr);
    CHECK_FALSE(a->approval());
    CHECK_FALSE(a->pending());
    CHECK(a->state == "active");
    CHECK(a->summary == "make test-unit");
    CHECK(t.pending().empty());
}

TEST_CASE("tasks: seeding from a snapshot object equals the events", "[tasks][snapshot]") {
    Tasks fromEvents;
    fromEvents.apply(approvalCreated(10));
    json obj = {{"object/id", kTask},
                {"object/class", "task"},
                {"object/actor", kRoot},
                {"object/created-seq", 10},
                {"object/fields",
                 {{"task/kind", {{"field/value", "approval"}, {"field/set-seq", 10}}},
                  {"task/state", {{"field/value", "pending"}, {"field/set-seq", 10}}},
                  {"task/op", {{"field/value", runOp()}, {"field/set-seq", 10}}},
                  {"task/summary", {{"field/value", "task.run: make test-unit (shell.local)"}, {"field/set-seq", 10}}},
                  {"task/requested-by", {{"field/value", kRoot}, {"field/set-seq", 10}}},
                  {"task/approvers", {{"field/value", json::array({kPerson})}, {"field/set-seq", 10}}}}}};
    Tasks seeded;
    seeded.seed(SnapshotObject::parse(obj));
    const TaskInfo* a = fromEvents.get(kTask);
    const TaskInfo* b = seeded.get(kTask);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->kind == b->kind);
    CHECK(a->state == b->state);
    CHECK(a->summary == b->summary);
    CHECK(a->requestedBy == b->requestedBy);
    CHECK(a->approvers == b->approvers);
    CHECK(a->op == b->op);
    CHECK(seeded.pendingFor(kPerson) == 1);
}

TEST_CASE("approval messages carry their kind and task once read", "[tasks][messages]") {
    const json state = {{"object/id", kMsg},
                        {"object/class", "message"},
                        {"object/fields",
                         {{"message/from", {{"field/value", kRoot}}},
                          {"message/to", {{"field/value", json::array({kPerson})}}},
                          {"message/text", {{"field/value", "approve: task.run make test-unit"}}},
                          {"message/kind", {{"field/value", "approval"}}},
                          {"message/task", {{"field/value", kTask}}}}}};
    const chat::MessageRow m = chat::MessageCache::parseMessage(kMsg, state);
    CHECK(m.kind == "approval");
    CHECK(m.task == kTask);
    CHECK(m.text == "approve: task.run make test-unit");
    const json plain = {{"object/fields", {{"message/text", {{"field/value", "hello"}}}}}};
    CHECK(chat::MessageCache::parseMessage(kMsg, plain).kind.empty());
}

TEST_CASE("actions: the world's action objects by name, schema and help, from events and from a snapshot", "[actions]") {
    Actions a;
    const std::string schema = "[:map [:adapter :string] [:cmd :string] [:cwd {:optional true} :string] [:timeout-s {:optional true} :int]]";
    REQUIRE(a.apply(ev(30, "created", kAction, kRoot,
                       {{"class", "object"},
                        {"fields",
                         {{"action/name", "task.run"},
                          {"action/schema", schema},
                          {"action/handler", "task.run"},
                          {"action/gated", false},
                          {"instructions/own", "Run a command through a shell Adapter as a Task you own."}}}})));
    REQUIRE(a.apply(ev(31, "bound", kWorld, kRoot, {{"namespace", kWorld}, {"name", "action/task.run"}, {"object", kAction}})));
    const ActionInfo* run = a.get("task.run");
    REQUIRE(run != nullptr);
    CHECK(run->id == kAction);
    CHECK(run->schema == schema);
    CHECK(run->handler == "task.run");
    CHECK_FALSE(run->gated);
    CHECK(run->help.rfind("Run a command", 0) == 0);
    REQUIRE(a.apply(ev(32, "field-set", kAction, kRoot, {{"name", "action/gated"}, {"type", "boolean"}, {"value", true}})));
    CHECK(run->gated);
    // an action known only by its binding (Fields not yet seen) still lists
    REQUIRE(a.apply(ev(33, "bound", kWorld, kRoot, {{"namespace", kWorld}, {"name", "action/route.join"}, {"object", kTask2}})));
    REQUIRE(a.get("route.join") != nullptr);
    CHECK(a.ordered().size() == 2);
    CHECK(a.ordered().front()->name == "route.join"); // by name
    // a field-set on an unrelated stream is not an action
    CHECK_FALSE(a.apply(ev(34, "field-set", kRoot, kRoot, {{"name", "loop/motto"}, {"type", "string"}, {"value", "x"}})));

    Actions seeded;
    json obj = {{"object/id", kAction},
                {"object/class", "object"},
                {"object/names", json::array({json::array({kWorld, "action/task.run"})})},
                {"object/fields",
                 {{"action/name", {{"field/value", "task.run"}, {"field/set-seq", 30}}},
                  {"action/schema", {{"field/value", schema}, {"field/set-seq", 30}}},
                  {"instructions/own", {{"field/value", "Run a command."}, {"field/set-seq", 30}}}}}};
    seeded.seed(SnapshotObject::parse(obj));
    const ActionInfo* s = seeded.get("task.run");
    REQUIRE(s != nullptr);
    CHECK(s->schema == schema);
    CHECK(s->help == "Run a command.");
}

TEST_CASE("tasks: an approval that ran shows its run's exit and duration on the collapsed line (task/ran)", "[tasks]") {
    constexpr const char* kRan = "7a1b2c3d-0000-4000-8000-0000000000aa";
    Tasks t;
    REQUIRE(t.apply(approvalCreated(10)));
    REQUIRE(t.apply(ev(11, "task-moved", kTask, kPerson, {{"task", kTask}, {"from", "pending"}, {"to", "approved"}})));
    // the run the approval executed: a plain Task of its own, with the command, exit and duration
    REQUIRE(t.apply(ev(12, "created", kRan, kRoot,
                       {{"class", "task"},
                        {"fields", {{"task/owner", kRoot}, {"task/state", "done"}, {"task/command", "make test-unit"},
                                    {"task/exit", 0}, {"task/duration-ms", 129400}}}})));
    // gates/approve! mirrors the run on the approval (world pass 13)
    REQUIRE(t.apply(ev(13, "field-set", kTask, kPerson, {{"name", "task/ran"}, {"type", json::array({"ref", "task"})}, {"value", kRan}})));
    REQUIRE(t.apply(ev(14, "field-set", kTask, kPerson, {{"name", "task/exit"}, {"type", "number"}, {"value", 0}})));
    REQUIRE(t.apply(ev(15, "task-moved", kTask, kWorld, {{"task", kTask}, {"from", "approved"}, {"to", "done"}})));
    const TaskInfo* a = t.get(kTask);
    REQUIRE(a != nullptr);
    CHECK(collapsedLine(*a, nameOf, &t) == "approved by person · ran · exit 0 · 129 s");
    // without the fold, the mirrored exit alone
    CHECK(collapsedLine(*a, nameOf) == "approved by person · ran · exit 0");
}

TEST_CASE("a card says the command, whichever shape carried it, and never shows JSON", "[tasks]") {
    const NameFn names = [](const std::string& id) { return id == kShell ? std::string("shell.local") : id; };
    const json run = {{"op", "act"}, {"args", {{"object", kShell}, {"action", "run"}, {"args", {{"cmd", "make test"}}}}}};
    CHECK(opSummary(run, names) == "task.run: make test (shell.local)");
    const std::vector<OpLine> lines = opDetail(run, names);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "make test");
    CHECK(lines[0].code);
    CHECK(lines[1].text == "adapter shell.local");

    const json send = {{"action", "pane.send"}, {"args", {{"pane", kShell}, {"text", "ls -la"}}}};
    const std::vector<OpLine> keys = opDetail(send, names);
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].text == "ls -la");
    CHECK(keys[1].text == "pane     shell.local");

    const json other = {{"action", "route.create"}, {"args", {{"name", "a.to.b"}, {"members", json::array({"x", "y"})}, {"hops", 2}}}};
    for (const OpLine& l : opDetail(other, names)) {
        CHECK_FALSE(l.code);
        CHECK(l.text.find('{') == std::string::npos);
    }
    CHECK(opDetail(other, names).size() == 3);
}
