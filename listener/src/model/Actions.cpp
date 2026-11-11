#include "model/Actions.hpp"

#include "model/Snapshot.hpp"

#include <algorithm>

namespace ke::model {

namespace {

constexpr const char* kPrefix = "action/";

std::string str(const json& j, const char* key) {
    if (!j.is_object()) {
        return "";
    }
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

const FieldState* ActionInfo::field(std::string_view name) const {
    auto it = fields.find(std::string(name));
    return it == fields.end() ? nullptr : &it->second;
}

ActionInfo& Actions::ensure(const std::string& id) {
    ActionInfo& a = actions_[id];
    a.id = id;
    return a;
}

void Actions::setField(ActionInfo& a, const std::string& name, const FieldState& f) {
    a.fields[name] = f;
    const json& v = f.value;
    auto s = [&]() { return v.is_string() ? v.get<std::string>() : std::string(); };
    if (name == "action/name") {
        if (!a.name.empty() && a.name != s()) {
            byName_.erase(a.name);
        }
        a.name = s();
        if (!a.name.empty()) {
            byName_[a.name] = a.id;
        }
    } else if (name == "action/schema") {
        a.schema = v.is_string() ? v.get<std::string>() : v.dump();
    } else if (name == "action/handler") {
        a.handler = s();
    } else if (name == "action/gated") {
        a.gated = v.is_boolean() ? v.get<bool>() : (v.is_string() && v.get<std::string>() == "true");
    } else if (name == "instructions/own") {
        a.help = s();
    }
}

bool Actions::apply(const Event& e) {
    if (e.seq <= applied_) {
        return false;
    }
    applied_ = e.seq;
    const json& b = e.body;
    if (e.kind == "created") {
        if (!(b.is_object() && b.contains("fields") && b["fields"].is_object() && b["fields"].contains("action/name"))) {
            return false;
        }
        ActionInfo& a = ensure(e.stream);
        a.created = e.seq;
        for (const auto& [k, v] : b["fields"].items()) {
            FieldState f;
            f.name = k;
            f.value = v;
            f.by = e.actor;
            f.at = e.at;
            f.seq = e.seq;
            setField(a, k, f);
        }
        return true;
    }
    if (e.kind == "bound") {
        const std::string name = str(b, "name");
        const std::string object = str(b, "object");
        if (object.empty() || name.rfind(kPrefix, 0) != 0) {
            return false;
        }
        ActionInfo& a = ensure(object);
        if (a.created == 0) {
            a.created = e.seq;
        }
        if (a.name.empty()) {
            a.name = name.substr(std::string(kPrefix).size());
            byName_[a.name] = object;
        }
        return true;
    }
    if (e.kind == "field-set") {
        auto it = actions_.find(e.stream);
        if (it == actions_.end()) {
            return false;
        }
        const std::string name = str(b, "name");
        if (name.empty()) {
            return false;
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
        return true;
    }
    return false;
}

void Actions::seed(const SnapshotObject& o) {
    const bool named = o.name.rfind(kPrefix, 0) == 0;
    const bool hasField = o.field("action/name") != nullptr;
    if (!named && !hasField) {
        return;
    }
    ActionInfo& a = ensure(o.id);
    a.created = o.created;
    for (const auto& [k, f] : o.fields) {
        setField(a, k, f);
    }
    if (a.name.empty() && named) {
        a.name = o.name.substr(std::string(kPrefix).size());
        byName_[a.name] = o.id;
    }
}

const ActionInfo* Actions::get(std::string_view name) const {
    auto it = byName_.find(std::string(name));
    if (it == byName_.end()) {
        return nullptr;
    }
    return byId(it->second);
}

const ActionInfo* Actions::byId(std::string_view id) const {
    auto it = actions_.find(std::string(id));
    return it == actions_.end() ? nullptr : &it->second;
}

std::vector<const ActionInfo*> Actions::ordered() const {
    // Reads the client makes for itself: they have panes and dialogs, and only crowd a form list.
    const auto listed = [](const std::string& name) {
        return name.rfind("doc.", 0) != 0 && name != "field.diff" && name != "path.complete" && name != "conversation.compressor";
    };
    std::vector<const ActionInfo*> out;
    for (const auto& [name, id] : byName_) {
        const ActionInfo* a = byId(id);
        if (a != nullptr && listed(name)) {
            out.push_back(a);
        }
    }
    return out;
}

void Actions::clear() {
    actions_.clear();
    byName_.clear();
    applied_ = -1;
}

} // namespace ke::model
