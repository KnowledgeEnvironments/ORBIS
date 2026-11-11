#include "model/Principals.hpp"

#include "model/Snapshot.hpp"

#include <algorithm>
#include <cctype>

namespace ke::model {

namespace {

std::string str(const json& j, const char* key) {
    if (!j.is_object()) {
        return "";
    }
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool principalClass(const std::string& cls) {
    return cls == "principal" || cls == "model" || cls == "adapter" || cls == "group";
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

} // namespace

const FieldState* PrincipalInfo::field(std::string_view name) const {
    auto it = fields.find(std::string(name));
    return it == fields.end() ? nullptr : &it->second;
}

namespace {
std::vector<std::string> idList(const FieldState* f) {
    std::vector<std::string> out;
    if (!f) {
        return out;
    }
    if (f->value.is_array()) {
        for (const auto& v : f->value) {
            if (v.is_string()) {
                out.push_back(v.get<std::string>());
            }
        }
    } else if (f->value.is_string()) {
        out.push_back(f->value.get<std::string>());
    }
    return out;
}
} // namespace

std::vector<std::string> PrincipalInfo::routes() const {
    return idList(field("principal/routes"));
}

std::vector<std::string> PrincipalInfo::models() const {
    return idList(field("principal/models"));
}

std::vector<std::string> PrincipalInfo::workspaces() const {
    return idList(field("principal/workspaces"));
}

std::vector<std::string> PrincipalInfo::members() const {
    return idList(field("group/members"));
}

int PrincipalInfo::loopIteration() const {
    const FieldState* f = field("loop/iteration");
    return f && f->value.is_number() ? f->value.get<int>() : 0;
}

int PrincipalInfo::loopTurnLimit() const {
    const FieldState* f = field("loop/turn-limit");
    return f && f->value.is_number() ? f->value.get<int>() : 0;
}

int PrincipalInfo::loopQueued() const {
    const FieldState* f = field("loop/queue");
    return f && f->value.is_array() ? static_cast<int>(f->value.size()) : 0;
}

std::string PrincipalInfo::loopStalled() const {
    const FieldState* f = field("loop/stalled");
    return f && f->value.is_string() ? f->value.get<std::string>() : std::string();
}

LoopState loopState(const PrincipalInfo& p) {
    if (p.drivenBy != "process") {
        return LoopState::None;
    }
    if (p.loopIteration() > 0) {
        return LoopState::Running;
    }
    if (!p.loopStalled().empty()) {
        return LoopState::Stalled;
    }
    return LoopState::Idle;
}

const char* name(LoopState s) {
    switch (s) {
    case LoopState::Idle:
        return "IDLE";
    case LoopState::Running:
        return "CARRIER";
    case LoopState::Stalled:
        return "STALLED";
    case LoopState::None:
        return "";
    }
    return "";
}

bool Principals::apply(const Event& e) {
    if (e.seq <= applied_) {
        return false;
    }
    applied_ = e.seq;
    const json& b = e.body;
    if (e.kind == "field-set" && str(b, "name") == "message/kind" && b.contains("value") && b["value"].is_string()) {
        messageKinds_[e.stream] = b["value"].get<std::string>();
    }
    if (e.kind == "created") {
        const std::string cls = str(b, "class");
        if (cls == "message" && b.contains("fields") && b["fields"].is_object()) {
            const json& fields = b["fields"];
            if (auto k = fields.find("message/kind"); k != fields.end() && k->is_string()) {
                messageKinds_[e.stream] = k->get<std::string>();
            }
        }
        if (cls == "route") {
            RouteInfo& r = routes_[e.stream];
            r.id = e.stream;
            r.created = e.seq;
            const json fields = b.is_object() && b.contains("fields") && b["fields"].is_object() ? b["fields"] : json::object();
            r.transport = str(fields, "route/transport");
            r.address = str(fields, "route/address");
            if (fields.contains("route/members")) {
                r.members = refList(fields["route/members"]);
            }
            for (const auto& [k, v] : fields.items()) {
                FieldState f;
                f.name = k;
                f.value = v;
                f.by = e.actor;
                f.at = e.at;
                f.seq = e.seq;
                r.fields[k] = std::move(f);
            }
            return true;
        }
        if (!principalClass(cls)) {
            return false;
        }
        PrincipalInfo& p = principals_[e.stream];
        p.id = e.stream;
        p.cls = cls;
        p.created = e.seq;
        p.createdAt = e.at;
        p.createdBy = e.actor;
        if (b.is_object() && b.contains("fields") && b["fields"].is_object()) {
            for (const auto& [k, v] : b["fields"].items()) {
                FieldState f;
                f.name = k;
                f.value = v;
                f.by = e.actor;
                f.at = e.at;
                f.seq = e.seq;
                p.fields[k] = std::move(f);
            }
        }
        p.drivenBy = str(b.is_object() && b.contains("fields") ? b["fields"] : json(), "principal/driven-by");
        return true;
    }
    if (e.kind == "bound") {
        const std::string object = str(b, "object");
        const std::string name = str(b, "name");
        if (object.empty() || name.empty()) {
            return false;
        }
        names_[object] = name;
        ++namesVersion_;
        return true;
    }
    if (e.kind == "unbound") {
        const std::string object = str(b, "object");
        auto it = names_.find(object);
        if (it != names_.end() && it->second == str(b, "name")) {
            names_.erase(it);
            ++namesVersion_;
            return true;
        }
        return false;
    }
    if (e.kind == "field-set") {
        const std::string name = str(b, "name");
        if (name.empty()) {
            return false;
        }
        if (name == "class/description" && b.contains("value") && b["value"].is_string()) {
            descriptions_[e.stream] = b["value"].get<std::string>();
        }
        auto it = principals_.find(e.stream);
        if (it == principals_.end()) {
            auto rt = routes_.find(e.stream);
            if (rt == routes_.end()) {
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
            if (name == "route/transport" && f.value.is_string()) {
                rt->second.transport = f.value.get<std::string>();
            } else if (name == "route/members") {
                rt->second.members = refList(f.value);
            }
            rt->second.fields[name] = std::move(f);
            return true;
        }
        if (b.contains("cleared") && b["cleared"].is_boolean() && b["cleared"].get<bool>()) {
            it->second.fields.erase(name); // field.clear: gone from reads, the log keeps it
            return true;
        }
        FieldState f;
        f.name = name;
        f.type = b.contains("type") ? b["type"] : json();
        f.value = b.contains("value") ? b["value"] : json();
        f.reader = str(b, "reader");
        f.by = b.contains("by") && b["by"].is_string() ? b["by"].get<std::string>() : e.actor;
        f.at = e.at;
        f.seq = e.seq;
        f.previous = b.contains("previous") ? b["previous"] : json();
        it->second.fields[name] = std::move(f);
        if (name == "principal/driven-by" && it->second.fields[name].value.is_string()) {
            it->second.drivenBy = it->second.fields[name].value.get<std::string>();
        }
        return true;
    }
    if (e.kind == "retired") {
        RetiredInfo r;
        r.seq = e.seq;
        if (auto it = names_.find(e.stream); it != names_.end()) {
            r.name = it->second;
        }
        if (b.contains("names") && b["names"].is_array() && !b["names"].empty() && b["names"][0].is_array() &&
            b["names"][0].size() == 2 && b["names"][0][1].is_string()) {
            r.name = b["names"][0][1].get<std::string>();
        }
        if (const PrincipalInfo* p = get(e.stream)) {
            r.cls = p->cls;
        } else if (route(e.stream)) {
            r.cls = "route";
        }
        retired_[e.stream] = std::move(r);
        ++namesVersion_;
        return true;
    }
    if (e.kind == "restored") {
        if (retired_.erase(e.stream) != 0) {
            ++namesVersion_;
            return true;
        }
        return false;
    }
    if (e.kind == "sent" || e.kind == "delivered") {
        MessageRef m;
        m.message = str(b, "message");
        if (m.message.empty()) {
            return false;
        }
        m.from = e.actor;
        m.recipient = str(b, "recipient");
        m.route = str(b, "route");
        m.seq = e.seq;
        m.at = e.at;
        m.via = e.via;
        messages_.push_back(std::move(m));
        return true;
    }
    return false;
}

void Principals::seed(const SnapshotObject& o) {
    if (const FieldState* d = o.field("class/description"); d && d->value.is_string()) {
        descriptions_[o.id] = d->value.get<std::string>();
    }
    if (!o.name.empty()) {
        names_[o.id] = o.name;
        ++namesVersion_;
    }
    if (o.cls == "route") {
        RouteInfo& r = routes_[o.id];
        r.id = o.id;
        r.created = o.created;
        r.fields = o.fields;
        if (const FieldState* f = o.field("route/transport"); f && f->value.is_string()) {
            r.transport = f->value.get<std::string>();
        }
        if (const FieldState* f = o.field("route/address"); f && f->value.is_string()) {
            r.address = f->value.get<std::string>();
        }
        if (const FieldState* f = o.field("route/members")) {
            r.members = refList(f->value);
        }
        return;
    }
    if (principalClass(o.cls)) {
        PrincipalInfo& p = principals_[o.id];
        p.id = o.id;
        p.cls = o.cls;
        p.created = o.created;
        p.createdAt = o.createdAt;
        if (!o.actor.empty()) {
            p.createdBy = o.actor;
        }
        p.fields = o.fields;
        if (const FieldState* f = o.field("principal/driven-by"); f && f->value.is_string()) {
            p.drivenBy = f->value.get<std::string>();
        }
        return;
    }
    if (o.cls == "message") {
        // One ref per recipient (as `delivered` events would be), or one `sent`-like ref alone.
        MessageRef base;
        base.message = o.id;
        if (const FieldState* f = o.field("message/kind"); f && f->value.is_string()) {
            messageKinds_[o.id] = f->value.get<std::string>();
        }
        if (const FieldState* f = o.field("message/from"); f && f->value.is_string()) {
            base.from = f->value.get<std::string>();
        } else {
            base.from = o.actor;
        }
        if (const FieldState* f = o.field("message/route"); f && f->value.is_string()) {
            base.route = f->value.get<std::string>();
        }
        base.seq = o.created;
        base.at = o.createdAt;
        for (const char* key : {"message/via", "object/via"}) {
            if (const FieldState* f = o.field(key); f && f->value.is_string() && !f->value.get<std::string>().empty()) {
                base.via = f->value.get<std::string>();
            }
        }
        if (!base.via && o.raw.is_object() && o.raw.contains("object/via") && o.raw["object/via"].is_string()) {
            base.via = o.raw["object/via"].get<std::string>();
        }
        std::vector<std::string> to;
        if (const FieldState* f = o.field("message/to")) {
            to = refList(f->value);
        }
        if (to.empty()) {
            messages_.push_back(base);
        } else {
            for (const std::string& r : to) {
                MessageRef m = base;
                m.recipient = r;
                messages_.push_back(std::move(m));
            }
        }
        std::stable_sort(messages_.begin(), messages_.end(),
                         [](const MessageRef& a, const MessageRef& b) { return a.seq < b.seq; });
    }
}

const RouteInfo* Principals::route(std::string_view id) const {
    auto it = routes_.find(std::string(id));
    return it == routes_.end() ? nullptr : &it->second;
}

const FieldState* Principals::fieldOf(std::string_view object, std::string_view name) const {
    if (const PrincipalInfo* p = get(object)) {
        return p->field(name);
    }
    if (const RouteInfo* r = route(object)) {
        auto it = r->fields.find(std::string(name));
        return it == r->fields.end() ? nullptr : &it->second;
    }
    return nullptr;
}

const PrincipalInfo* Principals::get(std::string_view id) const {
    auto it = principals_.find(std::string(id));
    return it == principals_.end() ? nullptr : &it->second;
}

std::string Principals::describe(std::string_view id, const std::string& cls) const {
    // `class/description` sits on the schema Object, named schema.<Class> (or schema.<kind> for an
    // Adapter kind), so the key is the word after the last dot.
    std::string want = cls;
    if (const FieldState* binds = fieldOf(id, "adapter/binds"); binds && binds->value.is_string()) {
        want = binds->value.get<std::string>();
    }
    const auto lowered = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string key = lowered(want);
    std::string fallback;
    for (const auto& [schema, text] : descriptions_) {
        auto n = names_.find(schema);
        if (n == names_.end()) {
            continue;
        }
        const std::size_t dot = n->second.rfind('.');
        const std::string tail = lowered(dot == std::string::npos ? n->second : n->second.substr(dot + 1));
        if (tail == key) {
            return text;
        }
        if (tail == lowered(cls)) {
            fallback = text;
        }
    }
    return fallback;
}

std::vector<const PrincipalInfo*> Principals::ordered() const {
    std::vector<const PrincipalInfo*> out;
    out.reserve(principals_.size());
    for (const auto& [id, p] : principals_) {
        if (retired_.count(id) == 0) {
            out.push_back(&p);
        }
    }
    // Pinned first (in creation order among themselves), then the rest; creation order overall.
    std::stable_sort(out.begin(), out.end(), [this](const PrincipalInfo* a, const PrincipalInfo* b) {
        const bool pa = pins_.count(a->id) != 0, pb = pins_.count(b->id) != 0;
        if (pa != pb) {
            return pa; // a pinned row precedes an unpinned one
        }
        return a->created < b->created; // within each group, creation order
    });
    return out;
}

std::string Principals::nameOf(std::string_view id) const {
    if (const FieldState* f = fieldOf(id, "object/label"); f && f->value.is_string() && !f->value.get<std::string>().empty()) {
        return f->value.get<std::string>();
    }
    return boundName(id);
}

std::string Principals::boundName(std::string_view id) const {
    auto it = names_.find(std::string(id));
    if (it != names_.end()) {
        return it->second;
    }
    for (const char* key : {"object/name", "name"}) {
        if (const FieldState* f = fieldOf(id, key); f && f->value.is_string() && !f->value.get<std::string>().empty()) {
            return f->value.get<std::string>();
        }
    }
    if (auto a = aliases_.find(std::string(id)); a != aliases_.end()) {
        return a->second;
    }
    if (auto r = retired_.find(std::string(id)); r != retired_.end() && !r->second.name.empty()) {
        return r->second.name;
    }
    return std::string(id.substr(0, 8));
}

std::string Principals::messageKind(const std::string& message) const {
    auto it = messageKinds_.find(message);
    return it != messageKinds_.end() ? it->second : std::string();
}

void Principals::clear() {
    messageKinds_.clear();
    principals_.clear();
    routes_.clear();
    names_.clear();
    aliases_.clear();
    messages_.clear();
    retired_.clear();
    descriptions_.clear();
    applied_ = -1;
    ++namesVersion_;
}

std::vector<std::string> recipientsFor(const Principals& fold, const std::string& principal, const std::string& route,
                                       const std::string& self) {
    if (principal != self) {
        return {principal};
    }
    std::vector<std::string> out;
    auto add = [&out, &self](const std::string& id) {
        if (!id.empty() && id != self && std::find(out.begin(), out.end(), id) == out.end()) {
            out.push_back(id);
        }
    };
    if (const RouteInfo* r = fold.route(route)) {
        for (const std::string& m : r->members) {
            add(m);
        }
    }
    if (out.empty()) {
        for (const MessageRef& m : fold.messages()) {
            if (m.route == route) {
                add(m.from);
                add(m.recipient);
            }
        }
    }
    if (out.empty()) {
        if (const PrincipalInfo* me = fold.get(self)) {
            add(me->createdBy);
        }
    }
    // appliances (driven by nothing: the world, workspaces) never answer a line; drop them
    std::erase_if(out, [&fold](const std::string& id) {
        const PrincipalInfo* pi = fold.get(id);
        return pi && pi->drivenBy == "none";
    });
    if (out.empty()) {
        out.push_back(self); // nobody else is known: a line to yourself is a poke to your own loop (sprint 0)
    }
    return out;
}

} // namespace ke::model
