// An Event as the world pushes it (docs/protocol.md "Events"; spec 4.4; ADR 0003).
// Keys are the namespaced strings of the JSON form ("event/seq"), ADR 0002.
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ke {

using json = nlohmann::json;
using Seq = std::int64_t;

struct Event {
    std::string id;                        // event/id (uuid)
    std::string stream;                    // event/stream (uuid, partition key)
    std::vector<std::string> streams;      // event/streams? (empty when absent)
    Seq seq = 0;                           // event/seq, global over the world log
    std::string kind;                      // event/kind, one of the sixteen kinds
    std::string actor;                     // event/actor (uuid)
    std::optional<std::string> via;        // event/via?
    std::optional<std::string> delegation; // event/delegation?
    std::string at;                        // event/at, ISO-8601 UTC
    json body = json::object();            // event/body, shape per kind (knowledge.schema.events/bodies)

    static Event fromJson(const json& j);
    json toJson() const;
    bool operator==(const Event&) const = default;
};

// A row of a chat turn's bookkeeping: streaming, cost and iteration counts, and the Evaluation it
// happened in. HISTORY leaves these out unless asked, so a day of chat is its Messages and what was
// done is what stands out. An op, its result, a failure and every Message stay.
inline bool isRoutine(const Event& e) {
    if (!e.body.is_object()) {
        return false;
    }
    if (e.kind == "evaluated") {
        const std::string state = e.body.value("state", std::string());
        return state != "failed" && state != "cancelled";
    }
    if (e.kind == "created") {
        const std::string cls = e.body.value("class", std::string());
        return cls == "evaluation" || cls == "serialization";
    }
    if (e.kind != "field-set") {
        return false;
    }
    const std::string name = e.body.value("name", std::string());
    if (name == "evaluation/ops" || name == "evaluation/results" || name == "loop/stalled") {
        const auto value = e.body.find("value");
        return value == e.body.end() || value->empty() || (value->is_string() && value->get_ref<const std::string&>().empty());
    }
    return name.rfind("cost/", 0) == 0 || name == "evaluation/iteration" || name == "evaluation/context" ||
           name == "evaluation/cost" || name == "loop/iteration" || name == "loop/turn-limit" || name == "loop/current" ||
           name == "loop/seen-seq";
}

// The kinds, in protocol order. Used by the mock and the UI chips.
inline constexpr const char* kKinds[] = {
    "created", "bound",     "unbound",    "field-set", "field-read", "resolved", "granted",  "revoked",
    "sent",    "delivered", "task-moved", "evaluated", "fired",      "attached", "detached", "retired",
    "restored"};

} // namespace ke
