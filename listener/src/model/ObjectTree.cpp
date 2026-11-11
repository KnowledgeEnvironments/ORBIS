#include "model/ObjectTree.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace ke::model {

namespace {

bool isPerson(const PrincipalInfo& p) {
    return p.cls == "principal" && p.drivenBy == "person";
}

// An agent thinks: a Principal with no Model, a bridge for one, is an appliance.
bool isActor(const PrincipalInfo& p) {
    return isPerson(p) || (p.cls == "principal" && !p.models().empty());
}

std::string text(const PrincipalInfo& p, const char* field) {
    const FieldState* f = p.field(field);
    return f && f->value.is_string() ? f->value.get<std::string>() : std::string();
}

// What an Adapter is to the actor using it: commands it can run, or a folder. A folder is `reads`
// only when the world says it is read-only; nothing else tells the two apart.
const char* adapterLabel(const PrincipalInfo* adapter) {
    if (!adapter) {
        return "files";
    }
    if (text(*adapter, "adapter/binds") == "host") {
        return "shell";
    }
    const FieldState* f = adapter->field("adapter/read-only");
    return f && f->value.is_boolean() && f->value.get<bool>() ? "reads" : "files";
}

std::vector<std::string> ids(const PrincipalInfo& p, const char* field) {
    std::vector<std::string> out;
    if (const FieldState* f = p.field(field); f && f->value.is_array()) {
        for (const json& v : f->value) {
            if (v.is_string() && v.get<std::string>() != p.id) {
                out.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

const PrincipalInfo* firstPerson(const Principals& fold) {
    const PrincipalInfo* first = nullptr;
    for (const PrincipalInfo* p : fold.ordered()) {
        if (p->cls == "principal" && p->drivenBy == "person" && (!first || p->created < first->created)) {
            first = p;
        }
    }
    return first;
}

} // namespace

ObjectTree buildObjectTree(const Principals& fold, const std::string& self) {
    ObjectTree t;
    std::vector<const PrincipalInfo*> actors;
    for (const PrincipalInfo* p : fold.ordered()) {
        if (isActor(*p)) {
            actors.push_back(p);
        }
    }
    std::sort(actors.begin(), actors.end(), [&fold](const PrincipalInfo* a, const PrincipalInfo* b) {
        const int ra = isPerson(*a) ? 0 : 1, rb = isPerson(*b) ? 0 : 1;
        return ra != rb ? ra < rb : fold.nameOf(a->id) < fold.nameOf(b->id);
    });
    // A reference to what this reader cannot see, or to a retired Object, is no row.
    auto live = [&fold](const std::string& id) { return !fold.retired(id) && (fold.get(id) || fold.route(id)); };

    std::map<std::string, std::vector<std::string>> users;
    auto use = [&users](const std::string& thing, const std::string& who) {
        auto& v = users[thing];
        if (std::find(v.begin(), v.end(), who) == v.end()) {
            v.push_back(who);
        }
    };
    for (const PrincipalInfo* p : actors) {
        for (const char* field : {"principal/models", "principal/adapters", "principal/workspaces", "principal/routes"}) {
            for (const std::string& id : ids(*p, field)) {
                if (live(id)) {
                    use(id, p->id);
                }
            }
        }
    }
    for (const auto& [id, route] : fold.routes()) {
        for (const std::string& m : route.members) {
            if (const PrincipalInfo* p = fold.get(m); p && isActor(*p) && !fold.retired(id)) {
                use(id, m);
            }
        }
    }
    auto othersOf = [&users](const std::string& thing, const std::string& who) {
        std::vector<std::string> out;
        for (const std::string& u : users[thing]) {
            if (u != who) {
                out.push_back(u);
            }
        }
        return out;
    };

    for (const PrincipalInfo* p : actors) {
        TreeRoot root;
        root.id = p->id;
        root.role = !isPerson(*p) ? "agent" : p->id == self ? "you" : "person";
        auto add = [&root](const std::string& label, TreeItem item) {
            auto line = std::find_if(root.lines.begin(), root.lines.end(), [&label](const TreeLine& l) { return l.label == label; });
            if (line == root.lines.end()) {
                root.lines.push_back(TreeLine{label, {}});
                line = root.lines.end() - 1;
            }
            auto same = [&item](const TreeItem& o) { return o.id == item.id; };
            if (std::none_of(line->items.begin(), line->items.end(), same)) {
                line->items.push_back(std::move(item));
            }
        };
        for (const std::string& id : ids(*p, "principal/models")) {
            if (live(id)) {
                add("model", TreeItem{id, "", othersOf(id, p->id)});
            }
        }
        for (const std::string& id : ids(*p, "principal/adapters")) {
            if (live(id)) {
                add(adapterLabel(fold.get(id)), TreeItem{id, "", othersOf(id, p->id)});
            }
        }
        for (const std::string& id : ids(*p, "principal/workspaces")) {
            if (live(id)) {
                add("works in", TreeItem{id, "", othersOf(id, p->id)});
            }
        }
        // Routes collapse into who is at the other end; a Route with nobody else stays itself.
        std::vector<std::string> routes = ids(*p, "principal/routes");
        for (const auto& [id, route] : fold.routes()) {
            if (std::find(route.members.begin(), route.members.end(), p->id) != route.members.end()) {
                routes.push_back(id);
            }
        }
        for (const std::string& id : routes) {
            const RouteInfo* route = fold.route(id);
            if (!route || fold.retired(id)) {
                continue;
            }
            bool someone = false;
            for (const std::string& m : route->members) {
                if (m != p->id && fold.get(m) && !fold.retired(m)) {
                    add("talks to", TreeItem{m, id, {}});
                    someone = true;
                }
            }
            if (!someone) {
                add("talks to", TreeItem{id, id, {}});
            }
        }
        t.roots.push_back(std::move(root));
    }
    for (const PrincipalInfo* p : fold.ordered()) {
        const bool world = fold.nameOf(p->id) == "world" || (!p->createdBy.empty() && p->createdBy == p->id);
        if (!world && !isActor(*p) && users.find(p->id) == users.end()) {
            t.unattached.push_back(p->id);
        }
    }
    for (const auto& [id, route] : fold.routes()) {
        if (users.find(id) == users.end() && !fold.retired(id)) {
            t.unattached.push_back(id);
        }
    }
    return t;
}

std::vector<std::pair<std::string, std::string>> treeLegend() {
    return {{"you", "your own seat in this world"},
            {"agent", "a Principal with a model of its own"},
            {"model", "the Model it thinks with"},
            {"files", "a folder it can work in"},
            {"reads", "a folder it can only read"},
            {"shell", "commands it can run, each one gated"},
            {"works in", "its workspace"},
            {"talks to", "who it can message; right-click a name for the Route"},
            {"UNATTACHED", "nothing live uses these, so they may be leftovers"}};
}

std::string deleteBlocked(const Principals& fold, const std::string& id) {
    const PrincipalInfo* p = fold.get(id);
    if (!p) {
        return "";
    }
    // By name, as the world resolves them: a snapshot does not say who created what.
    const std::string name = fold.nameOf(id);
    if (name == "world" || (!p->createdBy.empty() && p->createdBy == p->id)) {
        return "the world itself cannot be deleted";
    }
    if (name == "root" && p->cls == "principal") {
        return "root holds every Grant in this world";
    }
    if (name.rfind("schema.", 0) == 0) {
        return "the world's schema cannot be deleted";
    }
    if (const PrincipalInfo* first = firstPerson(fold); first && first->id == id) {
        return "the first person is who approves things here";
    }
    return "";
}

} // namespace ke::model
