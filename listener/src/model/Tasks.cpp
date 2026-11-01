#include "model/Tasks.hpp"

#include "model/Evaluations.hpp" // epochSeconds
#include "model/Snapshot.hpp"

#include <algorithm>
#include <cmath>

namespace ke::model {

namespace {

std::string str(const json& j, const char* key) {
    if (!j.is_object()) {
        return "";
    }
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<std::string> refList(const json& v) {
    std::vector<std::string> out;
    if (v.is_array()) {
        for (const auto& x : v) {
            if (x.is_string()) {
                out.push_back(x.get<std::string>());
            }
        }
    } else if (v.is_string()) {
        out.push_back(v.get<std::string>());
    }
    return out;
}

std::string shortId(const std::string& id) {
    return id.size() > 8 ? id.substr(0, 8) : id;
}

std::string nameOr(const NameFn& nameOf, const std::string& id) {
    if (id.empty()) {
        return "";
    }
    return nameOf ? nameOf(id) : shortId(id);
}

// The op's name and args whatever shape the world used: {"action":"task.run","args":{...}},
// {"op":"grant","args":{...}}, or {"op":"act","args":{"action":"task.run","args":{...}}}.
std::pair<std::string, json> unwrap(const json& op) {
    if (!op.is_object()) {
        return {op.is_string() ? op.get<std::string>() : std::string(), json::object()};
    }
    json args = op.contains("args") && op["args"].is_object() ? op["args"] : json::object();
    std::string name = str(op, "action");
    if (name.empty()) {
        name = str(op, "op");
    }
    if (name == "act") {
        const std::string inner = str(args, "action");
        if (!inner.empty()) {
            json innerArgs = args.contains("args") && args["args"].is_object() ? args["args"] : json::object();
            // `act run` on a shell Adapter is the same command as task.run, and reads the same.
            if (inner == "run" && args.contains("object")) {
                innerArgs["adapter"] = args["object"];
                return {"task.run", innerArgs};
            }
            return {inner, innerArgs};
        }
        if (args.contains("object")) {
            return {"act " + str(args, "action"), args};
        }
    }
    return {name, args};
}

std::string joinStrings(const json& v) {
    std::string out;
    for (const std::string& s : refList(v)) {
        out += (out.empty() ? "" : ", ") + s;
    }
    return out;
}

std::string scopeText(const json& scope, const NameFn& nameOf) {
    if (scope.is_string()) {
        return scope.get<std::string>();
    }
    if (scope.is_object() && scope.contains("prefix") && scope["prefix"].is_string()) {
        return "prefix " + scope["prefix"].get<std::string>();
    }
    if (scope.is_array()) {
        std::string out;
        for (const std::string& id : refList(scope)) {
            out += (out.empty() ? "" : ", ") + nameOr(nameOf, id);
        }
        return out.empty() ? "nothing" : out;
    }
    return scope.is_null() ? std::string("?") : scope.dump();
}

} // namespace

const FieldState* TaskInfo::field(std::string_view name) const {
    auto it = fields.find(std::string(name));
    return it == fields.end() ? nullptr : &it->second;
}

std::string opSummary(const json& op, const NameFn& nameOf) {
    auto [name, args] = unwrap(op);
    if (name.empty()) {
        return "an op";
    }
    if (name == "task.run") {
        std::string s = "task.run: " + str(args, "cmd");
        if (const std::string a = str(args, "adapter"); !a.empty()) {
            s += " (" + (a.size() == 36 ? nameOr(nameOf, a) : a) + ")";
        }
        return s;
    }
    if (name == "grant") {
        return "grant " + joinStrings(args.contains("ops") ? args["ops"] : json()) + " on " +
               scopeText(args.contains("scope") ? args["scope"] : json(), nameOf) + " to " +
               nameOr(nameOf, str(args, "grantee"));
    }
    if (name == "principal.create") {
        return "principal.create " + str(args, "name");
    }
    if (name == "workspace.mount") {
        return "workspace.mount " + str(args, "path") + " as " + str(args, "name");
    }
    if (name == "set-field") {
        return "set-field " + str(args, "field") + " on " + nameOr(nameOf, str(args, "object"));
    }
    if (name == "send") {
        return "send on " + nameOr(nameOf, str(args, "route"));
    }
    return name;
}

std::vector<OpLine> opDetail(const json& op, const NameFn& nameOf) {
    std::vector<OpLine> out;
    auto [name, args] = unwrap(op);
    if (name == "task.run") {
        out.push_back(OpLine{str(args, "cmd"), true});
        std::string meta;
        if (const std::string a = str(args, "adapter"); !a.empty()) {
            meta += "adapter " + (a.size() == 36 ? nameOr(nameOf, a) : a);
        }
        if (const std::string c = str(args, "cwd"); !c.empty()) {
            meta += (meta.empty() ? "" : " · ") + std::string("cwd ") + c;
        }
        if (args.contains("timeout-s") && args["timeout-s"].is_number()) {
            meta += (meta.empty() ? "" : " · ") + std::string("timeout ") + std::to_string(args["timeout-s"].get<long long>()) + " s";
        }
        if (!meta.empty()) {
            out.push_back(OpLine{meta, false});
        }
        return out;
    }
    if (name == "grant") {
        out.push_back(OpLine{"grantee  " + nameOr(nameOf, str(args, "grantee")), false});
        out.push_back(OpLine{"scope    " + scopeText(args.contains("scope") ? args["scope"] : json(), nameOf), false});
        out.push_back(OpLine{"ops      " + joinStrings(args.contains("ops") ? args["ops"] : json()), false});
        if (args.contains("delegable") && args["delegable"].is_boolean() && args["delegable"].get<bool>()) {
            out.push_back(OpLine{"delegable", false});
        }
        return out;
    }
    if (name == "principal.create") {
        out.push_back(OpLine{"name     " + str(args, "name"), false});
        if (args.contains("grant") && args["grant"].is_object()) {
            const json& g = args["grant"];
            out.push_back(OpLine{"grant    " + joinStrings(g.contains("ops") ? g["ops"] : json()) + " on " +
                                     scopeText(g.contains("scope") ? g["scope"] : json(), nameOf),
                                 false});
        }
        if (const std::string e = str(args, "model"); !e.empty()) {
            out.push_back(OpLine{"model   " + nameOr(nameOf, e), false});
        }
        if (const std::string c = str(args, "charter"); !c.empty()) {
            out.push_back(OpLine{"charter  " + (c.size() > 160 ? c.substr(0, 160) + "…" : c), false});
        }
        return out;
    }
    if (name == "workspace.mount") {
        out.push_back(OpLine{"path     " + str(args, "path"), false});
        out.push_back(OpLine{"mount    " + str(args, "name") +
                                 (args.contains("shell") && args["shell"].is_boolean() && !args["shell"].get<bool>() ? "" : " + shell") +
                                 (args.contains("read-only") && args["read-only"].is_boolean() && args["read-only"].get<bool>() ? " (read-only)" : ""),
                             false});
        out.push_back(OpLine{"for      " + nameOr(nameOf, str(args, "principal")), false});
        return out;
    }
    if (name == "pane.send") {
        for (const char* key : {"text", "keys"}) {
            if (const std::string v = str(args, key); !v.empty()) {
                out.push_back(OpLine{v, true});
            }
        }
        out.push_back(OpLine{"pane     " + nameOr(nameOf, str(args, "pane")), false});
        return out;
    }
    // Anything else, as what a person would say: one line each, no JSON.
    const json& shown = !args.empty() ? args : op;
    if (shown.is_object()) {
        for (const auto& [key, value] : shown.items()) {
            std::string v;
            if (value.is_string()) {
                const std::string s = value.get<std::string>();
                v = s.size() == 36 ? nameOr(nameOf, s) : s;
            } else if (value.is_array() && !value.empty() && value[0].is_string()) {
                v = joinStrings(value);
            } else {
                v = value.dump();
            }
            std::string label = key + ":";
            label.resize(std::max<std::size_t>(label.size() + 1, 10), ' ');
            out.push_back(OpLine{label + v, false});
        }
    }
    return out;
}

std::string collapsedLine(const TaskInfo& t, const NameFn& nameOf, const Tasks* all) {
    const std::string who = nameOr(nameOf, t.decidedBy);
    auto by = [&](const char* verb) { return std::string(verb) + (who.empty() ? "" : " by " + who); };
    if (t.state == "denied") {
        return by("denied") + (t.reason.empty() ? "" : ": " + t.reason);
    }
    if (t.state == "expired") {
        return "expired" + std::string(t.expiresAt.empty() ? "" : " at " + t.expiresAt);
    }
    if (t.state == "approved" || t.state == "done" || t.state == "failed") {
        std::string s = by("approved");
        if (!t.note.empty()) {
            s += " (" + t.note + ")";
        }
        if (t.state == "approved") {
            s += " · running";
        } else if (t.state == "done") {
            s += " · ran";
            // the run this approval executed (task/ran, task/exit mirrored by gates/approve!, pass 13)
            const FieldState* ranF = t.field("task/ran");
            const FieldState* exitF = t.field("task/exit");
            if (ranF && ranF->value.is_string()) {
                if (exitF && exitF->value.is_number()) {
                    s += " · exit " + std::to_string(exitF->value.get<long long>());
                }
                if (const TaskInfo* ran = all ? all->get(ranF->value.get<std::string>()) : nullptr) {
                    if (const FieldState* du = ran->field("task/duration-ms"); du && du->value.is_number()) {
                        s += " · " + std::to_string(du->value.get<long long>() / 1000) + " s";
                    }
                    if (!(exitF && exitF->value.is_number())) {
                        if (const FieldState* ex2 = ran->field("task/exit"); ex2 && ex2->value.is_number()) {
                            s += " · exit " + std::to_string(ex2->value.get<long long>());
                        }
                    }
                }
                return s;
            }
            if (t.result.is_object()) {
                if (t.result.contains("exit") && t.result["exit"].is_number()) {
                    s += " · exit " + std::to_string(t.result["exit"].get<long long>());
                } else if (t.result.contains("seq") && t.result["seq"].is_number()) {
                    s += " · seq " + std::to_string(t.result["seq"].get<long long>());
                }
                // the world's {ok text}: the op's own one-line answer
                if (t.result.contains("ok") && t.result["ok"].is_boolean()) {
                    s += t.result["ok"].get<bool>() ? " · ok" : " · not ok";
                }
                if (const std::string text = str(t.result, "text"); !text.empty()) {
                    s += ": " + (text.size() > 100 ? text.substr(0, 100) + "…" : text);
                }
            } else if (const FieldState* ex = t.field("task/exit"); ex && ex->value.is_number()) {
                s += " · exit " + std::to_string(ex->value.get<long long>());
            }
        } else {
            s += " · failed";
            if (!t.reason.empty()) {
                s += ": " + t.reason;
            } else if (t.result.is_object() && t.result.contains("err")) {
                const json& e = t.result["err"];
                s += ": " + (e.is_object() ? str(e, "code") + " " + str(e, "message") : e.dump());
            } else if (const std::string text = t.result.is_object() ? str(t.result, "text") : std::string(); !text.empty()) {
                s += ": " + (text.size() > 100 ? text.substr(0, 100) + "…" : text);
            }
        }
        return s;
    }
    return t.state.empty() ? std::string("pending") : t.state;
}

std::string timeLeft(const TaskInfo& t, double nowEpoch) {
    if (t.expiresAt.empty()) {
        return "";
    }
    const double exp = epochSeconds(t.expiresAt);
    if (exp < 0) {
        return "";
    }
    const long long left = static_cast<long long>(std::floor(exp - nowEpoch));
    if (left <= 0) {
        return "expired";
    }
    if (left >= 3600) {
        return std::to_string(left / 3600) + " h " + std::to_string((left % 3600) / 60) + " m";
    }
    if (left >= 60) {
        return std::to_string(left / 60) + " m " + std::to_string(left % 60) + " s";
    }
    return std::to_string(left) + " s";
}

void Tasks::setField(TaskInfo& t, const std::string& name, const FieldState& f) {
    t.fields[name] = f;
    const json& v = f.value;
    auto s = [&]() { return v.is_string() ? v.get<std::string>() : std::string(); };
    if (name == "task/kind") {
        t.kind = s();
    } else if (name == "task/state") {
        t.state = s();
    } else if (name == "task/summary") {
        t.summary = s();
    } else if (name == "task/title" || name == "task/command") {
        if (t.summary.empty() || !t.approval()) {
            t.summary = s();
        }
    } else if (name == "task/requested-by") {
        t.requestedBy = s();
    } else if (name == "task/evaluation") {
        t.evaluation = s();
    } else if (name == "task/message") {
        t.message = s();
    } else if (name == "task/approvers") {
        t.approvers = refList(v);
    } else if (name == "task/expires-at") {
        t.expiresAt = s();
    } else if (name == "task/decided-by") {
        t.decidedBy = s();
    } else if (name == "task/decided-at") {
        t.decidedAt = s();
    } else if (name == "task/note") {
        t.note = s();
    } else if (name == "task/reason") {
        t.reason = s();
    } else if (name == "task/op") {
        t.op = v;
    } else if (name == "task/result") {
        t.result = v;
    } else if (name == "task/owner") {
        t.owner = s();
    }
}

bool Tasks::apply(const Event& e) {
    if (e.seq <= applied_) {
        return false;
    }
    applied_ = e.seq;
    const json& b = e.body;
    if (e.kind == "created") {
        if (str(b, "class") != "task") {
            return false;
        }
        TaskInfo& t = tasks_[e.stream];
        t.id = e.stream;
        t.created = e.seq;
        t.createdAt = e.at;
        t.last = e.seq;
        if (b.is_object() && b.contains("fields") && b["fields"].is_object()) {
            for (const auto& [k, v] : b["fields"].items()) {
                FieldState f;
                f.name = k;
                f.value = v;
                f.by = e.actor;
                f.at = e.at;
                f.seq = e.seq;
                setField(t, k, f);
            }
        }
        if (t.requestedBy.empty() && t.approval()) {
            t.requestedBy = e.actor;
        }
        return true;
    }
    if (e.kind == "field-set") {
        const std::string name = str(b, "name");
        if (name.empty()) {
            return false;
        }
        auto it = tasks_.find(e.stream);
        if (it == tasks_.end()) {
            // A Task whose `created` this listener never saw (it predates the Grant that makes
            // the Task visible to it): its first task/* Field is where it starts here.
            if (name.rfind("task/", 0) != 0) {
                return false;
            }
            TaskInfo& t = tasks_[e.stream];
            t.id = e.stream;
            t.created = e.seq;
            t.createdAt = e.at;
            it = tasks_.find(e.stream);
        }
        FieldState f;
        f.name = name;
        f.type = b.contains("type") ? b["type"] : json();
        f.value = b.contains("value") ? b["value"] : json();
        f.reader = str(b, "reader");
        f.by = b.contains("by") && b["by"].is_string() ? b["by"].get<std::string>() : e.actor;
        f.at = e.at;
        f.seq = e.seq;
        setField(it->second, name, f);
        it->second.last = e.seq;
        return true;
    }
    if (e.kind == "task-moved") {
        std::string id = str(b, "task");
        if (id.empty()) {
            id = e.stream;
        }
        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            TaskInfo& t = tasks_[id];
            t.id = id;
            t.created = e.seq;
            t.createdAt = e.at;
            it = tasks_.find(id);
        }
        const std::string to = str(b, "to");
        if (!to.empty()) {
            it->second.state = to;
            // pending / approved / denied / expired are the approval lifecycle: a Task that moves
            // through them is an approval even when its task/kind Field never reached this fold
            if (it->second.kind.empty() && (to == "pending" || to == "approved" || to == "denied" || to == "expired")) {
                it->second.kind = "approval";
            }
        }
        if (it->second.approval() && (to == "approved" || to == "denied") && it->second.decidedBy.empty()) {
            it->second.decidedBy = e.actor;
            it->second.decidedAt = e.at;
        }
        it->second.last = e.seq;
        return true;
    }
    return false;
}

void Tasks::seed(const SnapshotObject& o) {
    if (o.cls != "task") {
        return;
    }
    TaskInfo& t = tasks_[o.id];
    t.id = o.id;
    t.created = o.created;
    t.createdAt = o.createdAt;
    t.last = o.created;
    for (const auto& [k, f] : o.fields) {
        setField(t, k, f);
        t.last = std::max(t.last, f.seq);
    }
    if (t.requestedBy.empty() && t.approval()) {
        t.requestedBy = o.actor;
    }
}

const TaskInfo* Tasks::get(std::string_view id) const {
    auto it = tasks_.find(std::string(id));
    return it == tasks_.end() ? nullptr : &it->second;
}

std::vector<const TaskInfo*> Tasks::ordered() const {
    std::vector<const TaskInfo*> out;
    out.reserve(tasks_.size());
    for (const auto& [id, t] : tasks_) {
        out.push_back(&t);
    }
    std::sort(out.begin(), out.end(), [](const TaskInfo* a, const TaskInfo* b) { return a->created < b->created; });
    return out;
}

std::vector<const TaskInfo*> Tasks::pending() const {
    std::vector<const TaskInfo*> out;
    for (const TaskInfo* t : ordered()) {
        if (t->pending()) {
            out.push_back(t);
        }
    }
    return out;
}

std::size_t Tasks::pendingRequestedBy(std::string_view principal) const {
    std::size_t n = 0;
    for (const auto& [id, t] : tasks_) {
        if (t.pending() && t.requestedBy == principal) {
            ++n;
        }
    }
    return n;
}

std::size_t Tasks::pendingFor(std::string_view principal) const {
    std::size_t n = 0;
    for (const auto& [id, t] : tasks_) {
        if (!t.pending()) {
            continue;
        }
        if (t.approvers.empty() || std::find(t.approvers.begin(), t.approvers.end(), std::string(principal)) != t.approvers.end()) {
            ++n;
        }
    }
    return n;
}

void Tasks::clear() {
    tasks_.clear();
    applied_ = -1;
}

} // namespace ke::model
