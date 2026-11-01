// Tasks as a pure fold over Events (ground rule 3), for the approval cards (sprint 1 L5) and the
// runs a shell Adapter records:
//
//   created {class task fields}            -> a Task (task/kind, task/state, task/op, ...)
//   field-set on a Task's stream           -> any Field (task/summary, task/decided-by, task/result, ...)
//   task-moved {task from to}              -> the state (`to`); a later task/state Field also wins
//
// Approval gates (world track): a gated op becomes a Task with task/kind "approval" and task/state
// pending; task.approve / task.deny move it; the approver sees it as a Message with
// message/kind "approval" and message/task on a Route. Nothing here is special to root: the
// approvers, the requester and the op all come from the Task's Fields.
#pragma once

#include "model/Event.hpp"
#include "model/Principals.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ke::model {

struct TaskInfo {
    std::string id;
    std::string kind;    // task/kind: "approval", or "" for a plain Task (a shell run)
    std::string state;   // task/state or the last task-moved `to`: pending approved denied done failed expired, open active cancelled
    std::string summary; // task/summary (approval) or task/title / task/command (runs)
    std::string requestedBy;
    std::string evaluation;
    std::string message;
    std::vector<std::string> approvers;
    std::string expiresAt; // ISO-8601, "" when none
    std::string decidedBy;
    std::string decidedAt;
    std::string note;
    std::string reason;
    json op;     // task/op: the gated op verbatim, e.g. {"action":"task.run","args":{...}} or {"op":"grant",...}
    json result; // task/result when the op ran
    std::string owner; // task/owner
    std::map<std::string, FieldState> fields;
    Seq created = 0;
    std::string createdAt;
    Seq last = 0; // seq of the latest event about it
    bool approval() const { return kind == "approval"; }
    bool pending() const { return approval() && (state == "pending" || state.empty()); }
    bool decided() const { return approval() && !pending(); }
    const FieldState* field(std::string_view name) const;
};

using NameFn = std::function<std::string(const std::string& id)>;

// The op in one line for the card's head: `task.run: make test-unit (shell.local)`,
// `grant read, write on prefix atlas to atlas`, `principal.create atlas`, else `<op>`.
std::string opSummary(const json& op, const NameFn& nameOf);
// The op's detail lines for the card body: for task.run the command (code), cwd, timeout; for a
// grant grantee, scope, ops; for principal.create the name and grant; else the args as JSON.
// `code` marks the line that should render in a code box.
struct OpLine {
    std::string text;
    bool code = false;
};
std::vector<OpLine> opDetail(const json& op, const NameFn& nameOf);
// The collapsed line once a Task is decided: `approved by alice · ran · exit 0 · 129 s`,
// `denied by alice: not now`, `expired`, `approved by alice · failed: <reason>`. With `all`, the
// run the approval executed (task/ran, world pass 13) lends its exit and duration.
class Tasks;
std::string collapsedLine(const TaskInfo& t, const NameFn& nameOf, const Tasks* all = nullptr);
// `4 m 12 s` left until expiresAt at nowEpoch, `expired` when past, "" when no expiry.
std::string timeLeft(const TaskInfo& t, double nowEpoch);

struct SnapshotObject;

class Tasks {
  public:
    bool apply(const Event& e);
    void seed(const SnapshotObject& o);
    void setApplied(Seq seq) { applied_ = std::max(applied_, seq); }
    const TaskInfo* get(std::string_view id) const;
    // Every Task in creation order.
    std::vector<const TaskInfo*> ordered() const;
    // Pending approvals, oldest first.
    std::vector<const TaskInfo*> pending() const;
    // Pending approvals `principal` asked for.
    std::size_t pendingRequestedBy(std::string_view principal) const;
    // Pending approvals `principal` may decide: it is among task/approvers, or the Task names none.
    std::size_t pendingFor(std::string_view principal) const;
    std::size_t size() const { return tasks_.size(); }
    Seq applied() const { return applied_; }
    void clear();

  private:
    void setField(TaskInfo& t, const std::string& name, const FieldState& f);
    std::map<std::string, TaskInfo> tasks_;
    Seq applied_ = -1;
};

} // namespace ke::model
