#include "chat/Approvals.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace ke::chat {

namespace {

json words(const std::string& text) {
    json out = json::array();
    std::istringstream in(text);
    std::string w;
    while (in >> w) {
        out.push_back(w);
    }
    return out;
}

std::string joined(const json& list) {
    std::string out;
    if (list.is_array()) {
        for (const json& v : list) {
            if (v.is_string()) {
                out += (out.empty() ? "" : " ") + v.get<std::string>();
            }
        }
    }
    return out;
}

std::string str(const json& o, const char* key) {
    return o.is_object() && o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
}

} // namespace

std::vector<ApprovalGroup> groupApprovals(const std::vector<const model::TaskInfo*>& pending) {
    std::vector<ApprovalGroup> out;
    for (const model::TaskInfo* t : pending) {
        auto it = std::find_if(out.begin(), out.end(), [t](const ApprovalGroup& g) { return g.requester == t->requestedBy; });
        if (it == out.end()) {
            out.push_back(ApprovalGroup{t->requestedBy, {t}});
        } else {
            it->tasks.push_back(t);
        }
    }
    return out;
}

std::string approvalCommand(const model::TaskInfo& t) {
    if (!t.op.is_object() || !t.op.contains("args") || !t.op["args"].is_object()) {
        return "";
    }
    const json& args = t.op["args"];
    if (const std::string cmd = str(args, "cmd"); !cmd.empty()) {
        return cmd;
    }
    // `act {object action args}` stores the call as it came, one level down.
    return args.contains("args") ? str(args["args"], "cmd") : std::string();
}

std::string approvalAction(const model::TaskInfo& t) {
    if (!approvalCommand(t).empty()) {
        return "task.run";
    }
    const std::string action = str(t.op, "action");
    return action.empty() ? str(t.op, "op") : action;
}

RuleDraft draftFor(const model::TaskInfo& t) {
    RuleDraft d;
    const std::string cmd = approvalCommand(t);
    d.programs = cmd.substr(0, cmd.find(' '));
    return d;
}

json draftMatch(const RuleDraft& d) {
    json m = json::object();
    if (json v = words(d.programs); !v.empty()) {
        m["programs"] = std::move(v);
    }
    if (json v = words(d.argsPrefix); !v.empty()) {
        m["args-prefix"] = std::move(v);
    }
    if (json v = words(d.paths); !v.empty()) {
        m["paths"] = std::move(v);
    }
    if (!d.preset.empty()) {
        m["preset"] = d.preset;
    }
    return m;
}

json draftRemember(const RuleDraft& d) {
    json r = json::object();
    if (json m = draftMatch(d); !m.empty()) {
        r["match"] = std::move(m);
    }
    if (!d.until.empty()) {
        r["until"] = d.until;
    }
    if (d.uses > 0) {
        r["uses"] = d.uses;
    }
    return r;
}

std::string instantAfter(double nowEpoch, long seconds) {
    const std::time_t t = static_cast<std::time_t>(nowEpoch) + seconds;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

std::vector<ApprovalRule> parseRules(const json& field) {
    std::vector<ApprovalRule> out;
    if (!field.is_array()) {
        return out;
    }
    for (const json& j : field) {
        if (!j.is_object() || str(j, "id").empty()) {
            continue;
        }
        ApprovalRule r;
        r.id = str(j, "id");
        r.action = str(j, "action");
        r.match = j.contains("match") ? j["match"] : json::object();
        r.until = str(j, "until");
        if (j.contains("uses-left") && j["uses-left"].is_number()) {
            r.usesLeft = j["uses-left"].get<long>();
        }
        r.used = j.contains("used") && j["used"].is_number() ? j["used"].get<long>() : 0;
        r.by = str(j, "by");
        r.at = str(j, "at");
        r.note = str(j, "note");
        out.push_back(std::move(r));
    }
    return out;
}

std::string matchText(const json& match) {
    std::string out;
    auto add = [&out](const std::string& head, const std::string& value) {
        if (!value.empty()) {
            out += (out.empty() ? "" : " · ") + head + " " + value;
        }
    };
    add("preset", str(match, "preset"));
    if (match.is_object()) {
        for (const char* key : {"programs", "args-prefix", "paths"}) {
            if (match.contains(key)) {
                add(key, joined(match[key]));
            }
        }
    }
    return out.empty() ? "anything" : out;
}

std::string ruleLimit(const ApprovalRule& r) {
    std::string out;
    if (r.usesLeft) {
        out = std::to_string(*r.usesLeft) + (*r.usesLeft == 1 ? " use left" : " uses left");
    }
    if (!r.until.empty()) {
        out += (out.empty() ? "" : " · ") + ("until " + r.until.substr(0, 16));
    }
    return out.empty() ? "no end" : out;
}

std::string autoRunLine(const model::TaskInfo& t, const std::string& ruleNote) {
    std::string out = "auto · rule";
    if (!ruleNote.empty()) {
        out += " " + ruleNote;
    }
    const std::string cmd = approvalCommand(t);
    out += " · " + (cmd.empty() ? t.summary : cmd);
    if (const model::FieldState* f = t.field("task/exit"); f && f->value.is_number()) {
        out += " → exit " + std::to_string(f->value.get<long>());
    }
    return out;
}

} // namespace ke::chat
