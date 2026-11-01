#include "model/Modeline.hpp"

#include <array>
#include <string_view>

namespace ke::model {

namespace {

constexpr std::size_t kLookBack = 256; // the newest rows a line is taken from

bool bookkeeping(std::string_view field) {
    constexpr std::array<std::string_view, 9> kQuiet = {"cost/",  "loop/",  "evaluation/", "serialization/", "view/",
                                                        "task/",  "pane/",  "world/",      "memory/used"};
    for (std::string_view prefix : kQuiet) {
        if (field.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    constexpr std::array<std::string_view, 3> kProbe = {"model/status", "model/available", "model/probed-at"};
    for (std::string_view name : kProbe) {
        if (field == name) {
            return true;
        }
    }
    return false;
}

bool quietClass(std::string_view cls) {
    constexpr std::array<std::string_view, 7> kQuiet = {"message", "evaluation", "serialization", "task", "grant", "pane", "object"};
    for (std::string_view name : kQuiet) {
        if (cls == name) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string describeEvent(const Event& e, const Principals& principals) {
    const std::string who = principals.nameOf(e.actor);
    const std::string what = principals.nameOf(e.stream);
    if (e.kind == "sent") {
        return who + " sent a Message";
    }
    if (e.kind == "field-set") {
        const std::string field = e.body.value("name", std::string());
        if (field.empty() || bookkeeping(field)) {
            return "";
        }
        return e.actor == e.stream ? who + " set " + field : who + " set " + field + " on " + what;
    }
    if (e.kind == "created") {
        const std::string cls = e.body.value("class", std::string());
        return cls.empty() || quietClass(cls) ? "" : who + " created " + cls + " " + what;
    }
    if (e.kind == "task-moved") {
        return who + " moved a Task to " + e.body.value("to", std::string("?"));
    }
    if (e.kind == "evaluated") {
        const std::string state = e.body.value("state", std::string());
        return state == "running" || state == "pending" || state.empty() ? "" : who + ": call " + state;
    }
    if (e.kind == "fired") {
        return "a Trigger fired";
    }
    if (e.kind == "granted") {
        return who + " granted access to " + principals.nameOf(e.body.value("grantee", std::string()));
    }
    if (e.kind == "revoked") {
        return who + " revoked a Grant";
    }
    if (e.kind == "bound") {
        return who + " bound " + e.body.value("name", std::string("a name"));
    }
    if (e.kind == "retired") {
        return who + " retired " + what;
    }
    if (e.kind == "restored") {
        return who + " restored " + what;
    }
    if (e.kind == "attached") {
        return who + " attached";
    }
    if (e.kind == "detached") {
        return who + " detached";
    }
    return "";
}

std::string modeline(const std::vector<Event>& rows, const Principals& principals) {
    std::string doing;
    for (const PrincipalInfo* p : principals.ordered()) {
        const LoopState state = loopState(*p);
        if (state != LoopState::Running && state != LoopState::Stalled) {
            continue;
        }
        doing += (doing.empty() ? "" : ", ") + principals.nameOf(p->id) + " " + name(state);
    }
    std::string last;
    const std::size_t stop = rows.size() > kLookBack ? rows.size() - kLookBack : 0;
    for (std::size_t i = rows.size(); i-- > stop && last.empty();) {
        last = describeEvent(rows[i], principals);
    }
    if (doing.empty() && last.empty()) {
        return "idle";
    }
    if (doing.empty()) {
        return last;
    }
    return last.empty() ? doing : doing + "  //  " + last;
}

} // namespace ke::model
