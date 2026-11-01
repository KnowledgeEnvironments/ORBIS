#include "model/Snapshot.hpp"

#include "model/Tokens.hpp"

#include "model/Actions.hpp"
#include "model/Evaluations.hpp"
#include "model/Tasks.hpp"

#include <algorithm>

namespace ke::model {

namespace {

std::string str(const json& j, const char* key) {
    if (!j.is_object()) {
        return "";
    }
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

long long num(const json& j, const char* key, long long fallback = -1) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? static_cast<long long>(it->get<double>()) : fallback;
}

} // namespace

const FieldState* SnapshotObject::field(std::string_view name) const {
    auto it = fields.find(std::string(name));
    return it == fields.end() ? nullptr : &it->second;
}

SnapshotObject SnapshotObject::parse(const json& o) {
    SnapshotObject s;
    s.raw = o;
    s.id = str(o, "object/id");
    if (s.id.empty()) {
        s.id = str(o, "id");
    }
    s.cls = str(o, "object/class");
    if (s.cls.empty()) {
        s.cls = str(o, "class");
    }
    s.stream = str(o, "object/stream");
    if (s.stream.empty()) {
        s.stream = s.id;
    }
    s.actor = str(o, "object/actor");
    // names: [[namespace name] ...], the latest binding wins; a plain list of strings is accepted too
    if (o.is_object() && o.contains("object/names") && o["object/names"].is_array()) {
        for (const auto& n : o["object/names"]) {
            if (n.is_array() && n.size() >= 2 && n[1].is_string()) {
                s.name = n[1].get<std::string>();
            } else if (n.is_string()) {
                s.name = n.get<std::string>();
            }
        }
    }
    // a retired Object's names are unbound; the world keeps what it was called
    if (s.name.empty() && o.is_object() && o.contains("object/fields") && o["object/fields"].is_object() &&
        o["object/fields"].contains("object/last-names")) {
        const json& f = o["object/fields"]["object/last-names"];
        const json& v = f.is_object() && f.contains("field/value") ? f["field/value"] : f;
        if (v.is_array() && !v.empty() && v[0].is_string()) {
            s.name = v[0].get<std::string>();
        }
    }
    Seq minSeq = -1;
    std::string minAt;
    if (o.is_object() && o.contains("object/fields") && o["object/fields"].is_object()) {
        for (const auto& [name, f] : o["object/fields"].items()) {
            FieldState fs;
            fs.name = name;
            if (f.is_object()) {
                fs.value = f.contains("field/value") ? f["field/value"] : json();
                fs.type = f.contains("field/type") ? f["field/type"] : json();
                fs.reader = str(f, "field/reader");
                fs.by = str(f, "field/author");
                fs.at = str(f, "field/set-at");
                fs.seq = num(f, "field/set-seq", 0);
            } else {
                fs.value = f; // a bare value: the world's compact form
            }
            if (fs.seq > 0 && (minSeq < 0 || fs.seq < minSeq)) {
                minSeq = fs.seq;
                minAt = fs.at;
            }
            s.fields[name] = std::move(fs);
        }
    }
    const long long createdSeq = num(o, "object/created-seq");
    s.created = createdSeq >= 0 ? createdSeq : std::max<Seq>(minSeq, 0);
    s.createdAt = str(o, "object/created-at");
    if (s.createdAt.empty()) {
        s.createdAt = minAt;
    }
    // class attributes the world lists top-level as well as in Fields (principal/driven-by)
    for (const char* key : {"principal/driven-by", "principal/namespace"}) {
        if (o.is_object() && o.contains(key) && !s.fields.count(key)) {
            FieldState fs;
            fs.name = key;
            fs.value = o[key];
            fs.seq = s.created;
            fs.at = s.createdAt;
            fs.by = s.actor;
            s.fields[key] = std::move(fs);
        }
    }
    return s;
}

SnapshotStats seedSnapshot(const json& objects, Principals& principals, Evaluations& evaluations,
                           const std::function<void(const SnapshotObject&)>& onMessage, Tasks* tasks, Actions* actions,
                           Tokens* tokens) {
    SnapshotStats st;
    if (!objects.is_array()) {
        return st;
    }
    // Objects in creation order so the folds' `ordered()` views match a replay.
    std::vector<SnapshotObject> parsed;
    parsed.reserve(objects.size());
    for (const auto& o : objects) {
        SnapshotObject s = SnapshotObject::parse(o);
        if (!s.id.empty()) {
            parsed.push_back(std::move(s));
        }
    }
    std::stable_sort(parsed.begin(), parsed.end(),
                     [](const SnapshotObject& a, const SnapshotObject& b) { return a.created < b.created; });
    for (const SnapshotObject& s : parsed) {
        ++st.objects;
        if (s.cls == "principal" || s.cls == "model" || s.cls == "adapter" || s.cls == "group") {
            ++st.principals;
        } else if (s.cls == "route") {
            ++st.routes;
        } else if (s.cls == "evaluation" || s.cls == "serialization") {
            ++st.evaluations;
        } else if (s.cls == "message") {
            ++st.messages;
        } else {
            ++st.others;
        }
        principals.seed(s);
        evaluations.seed(s);
        if (tasks) {
            tasks->seed(s);
        }
        if (actions) {
            actions->seed(s);
        }
        if (tokens) {
            tokens->seed(s);
        }
        if (s.cls == "message" && onMessage) {
            onMessage(s);
        }
    }
    return st;
}

} // namespace ke::model
