#include "chat/Unsafe.hpp"

#include "model/Evaluations.hpp" // epochSeconds

#include <algorithm>
#include <cmath>

namespace ke::chat {

std::vector<UnsafeEntry> parseUnsafe(const nlohmann::json& field) {
    std::vector<UnsafeEntry> out;
    if (!field.is_array()) {
        return out;
    }
    for (const nlohmann::json& e : field) {
        if (!e.is_object() || !e.contains("principal") || !e["principal"].is_string()) {
            continue;
        }
        out.push_back(UnsafeEntry{e["principal"].get<std::string>(), e.value("until", std::string()), e.value("by", std::string()),
                                  e.value("kind", std::string()), e.value("agent", std::string())});
    }
    return out;
}

std::vector<UnsafeEntry> parseUnsafeRead(const nlohmann::json& result) {
    if (!result.is_object() || !result.contains("object/fields") || !result["object/fields"].is_object()) {
        return {};
    }
    const nlohmann::json& fields = result["object/fields"];
    if (!fields.contains("world/unsafe") || !fields["world/unsafe"].is_object()) {
        return {};
    }
    return parseUnsafe(fields["world/unsafe"].value("field/value", nlohmann::json()));
}

long minutesLeft(const std::string& until, double nowEpoch) {
    const double end = model::epochSeconds(until);
    if (end < 0 || end <= nowEpoch) {
        return 0;
    }
    return static_cast<long>(std::ceil((end - nowEpoch) / 60.0));
}

std::string unsafeLabel(const std::vector<UnsafeEntry>& entries, const std::function<std::string(const std::string&)>& nameOf,
                        double nowEpoch) {
    if (entries.empty()) {
        return "";
    }
    if (entries.size() > 1) {
        return "UNSAFE " + std::to_string(entries.size());
    }
    return "UNSAFE " + (entries[0].kind == "shared-terminal" ? std::string("terminal") : nameOf(entries[0].principal)) + " " + std::to_string(minutesLeft(entries[0].until, nowEpoch)) + "m";
}

std::vector<const model::TaskInfo*> ranUnsafe(const std::vector<const model::TaskInfo*>& tasks) {
    std::vector<const model::TaskInfo*> out;
    for (const model::TaskInfo* t : tasks) {
        const model::FieldState* rule = t->field("task/rule");
        if (rule != nullptr && rule->value == "unsafe") {
            out.push_back(t);
        }
    }
    std::sort(out.begin(), out.end(), [](const model::TaskInfo* a, const model::TaskInfo* b) { return a->created > b->created; });
    return out;
}

} // namespace ke::chat
