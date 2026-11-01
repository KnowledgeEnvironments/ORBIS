#include "model/Graph.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace ke::model {

const char* name(EdgeKind k) {
    switch (k) {
    case EdgeKind::Route:
        return "route";
    case EdgeKind::Inbox:
        return "inbox";
    case EdgeKind::Model:
        return "model";
    case EdgeKind::Workspace:
        return "workspace";
    case EdgeKind::Adapter:
        return "adapter";
    }
    return "";
}

const GraphNode* GraphModel::node(std::string_view id) const {
    for (const GraphNode& n : nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

GraphNode* GraphModel::node(std::string_view id) {
    for (GraphNode& n : nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

GraphModel buildGraph(const Principals& principals) {
    GraphModel g;
    const auto rows = principals.ordered();
    // The world: the earliest Principal driven by nothing (kernel-zero creates it first, with
    // itself as actor). Everything hangs off it.
    const PrincipalInfo* world = nullptr;
    for (const PrincipalInfo* p : rows) {
        if (p->cls == "principal" && p->drivenBy == "none") {
            world = p;
            break;
        }
    }
    std::set<std::string> workspaces;
    for (const PrincipalInfo* p : rows) {
        for (const std::string& w : p->workspaces()) {
            workspaces.insert(w);
        }
    }
    for (const PrincipalInfo* p : rows) {
        GraphNode n;
        n.id = p->id;
        n.name = principals.nameOf(p->id);
        n.cls = p->cls;
        n.drivenBy = p->drivenBy;
        n.loop = loopState(*p);
        n.isWorld = p == world;
        n.created = p->created;
        if (p->cls == "model") {
            if (const FieldState* f = p->field("model/id"); f && f->value.is_string()) {
                n.sub = f->value.get<std::string>();
            }
            if (const FieldState* f = p->field("model/status"); f && f->value.is_string()) {
                n.status = f->value.get<std::string>();
            }
        }
        if (const FieldState* pos = p->field("view/position"); pos && pos->value.is_object()) {
            const json& v = pos->value;
            if (v.contains("x") && v["x"].is_number() && v.contains("y") && v["y"].is_number()) {
                n.placed = true;
                n.x = v["x"].get<float>();
                n.y = v["y"].get<float>();
            }
        }
        if (n.isWorld) {
            n.layer = 0;
        } else if (p->cls == "group" || (p->cls == "principal" && p->drivenBy == "process")) {
            n.layer = 1;
        } else if (p->cls == "model" || p->cls == "adapter" || workspaces.count(p->id) != 0) {
            n.layer = 2;
        } else if (p->cls == "principal" && p->drivenBy == "person") {
            n.layer = 3;
        } else {
            n.layer = 4;
        }
        g.nodes.push_back(std::move(n));
    }

    auto known = [&g](const std::string& id) { return g.node(id) != nullptr; };
    auto addEdge = [&g](GraphEdge e) {
        for (GraphEdge& have : g.edges) {
            if (have.from == e.from && have.to == e.to && have.route == e.route && have.kind == e.kind) {
                have.traffic = have.traffic || e.traffic;
                return;
            }
        }
        g.edges.push_back(std::move(e));
    };

    // Bindings.
    for (const PrincipalInfo* p : rows) {
        for (const std::string& e : p->models()) {
            if (known(e)) {
                addEdge(GraphEdge{p->id, e, "", principals.nameOf(e), EdgeKind::Model, false});
            }
        }
        for (const std::string& w : p->workspaces()) {
            if (known(w)) {
                addEdge(GraphEdge{p->id, w, "", principals.nameOf(w), EdgeKind::Workspace, false});
            }
        }
        for (const std::string& r : p->routes()) {
            if (const RouteInfo* ri = principals.route(r); ri && !ri->transport.empty() && known(ri->transport)) {
                addEdge(GraphEdge{p->id, ri->transport, r, principals.nameOf(r), EdgeKind::Adapter, false});
            }
        }
    }
    // Routes: an explicit `route/members` Field names the ends exactly; without one the holders
    // of the Route in `principal/routes`, in creation order, are inferred to be its ends.
    std::map<std::string, std::vector<std::string>> holders;
    for (const auto& [rid, ri] : principals.routes()) {
        for (const std::string& m : ri.members) {
            if (known(m) && std::find(holders[rid].begin(), holders[rid].end(), m) == holders[rid].end()) {
                holders[rid].push_back(m);
            }
        }
    }
    for (const PrincipalInfo* p : rows) {
        for (const std::string& r : p->routes()) {
            if (const RouteInfo* ri = principals.route(r); ri && !ri->members.empty()) {
                continue; // explicit membership wins over holding
            }
            holders[r].push_back(p->id);
        }
    }
    for (const auto& [route, hs] : holders) {
        for (std::size_t i = 0; i < hs.size(); ++i) {
            for (std::size_t j = i + 1; j < hs.size(); ++j) {
                addEdge(GraphEdge{hs[i], hs[j], route, principals.nameOf(route), EdgeKind::Route, false});
            }
        }
    }
    // Traffic: deliveries from one Principal to another on a Route.
    std::set<std::string> routesWithTraffic;
    for (const MessageRef& m : principals.messages()) {
        if (m.recipient.empty() || m.route.empty() || !known(m.from) || !known(m.recipient)) {
            continue;
        }
        if (m.from == m.recipient) {
            routesWithTraffic.insert(m.route);
            continue; // a Principal writing to itself: its inbox, shown as the stub
        }
        routesWithTraffic.insert(m.route);
        addEdge(GraphEdge{m.from, m.recipient, m.route, principals.nameOf(m.route), EdgeKind::Route, true});
    }
    // Inbox stubs: a Route held by one Principal that no edge shows.
    for (const auto& [route, hs] : holders) {
        bool shown = false;
        for (const GraphEdge& e : g.edges) {
            if (e.route == route && e.kind == EdgeKind::Route) {
                shown = true;
                break;
            }
        }
        if (!shown) {
            for (const std::string& h : hs) {
                addEdge(GraphEdge{h, h, route, principals.nameOf(route), EdgeKind::Inbox, routesWithTraffic.count(route) != 0});
            }
        }
    }
    // Groups.
    for (const PrincipalInfo* p : rows) {
        if (p->cls != "group") {
            continue;
        }
        GraphGroup grp;
        grp.id = p->id;
        grp.name = principals.nameOf(p->id);
        for (const std::string& m : p->members()) {
            if (known(m)) {
                grp.members.push_back(m);
            }
        }
        g.groups.push_back(std::move(grp));
    }
    return g;
}

void layoutLayers(GraphModel& g, const LayoutMetrics& m) {
    std::map<int, std::vector<GraphNode*>> byLayer;
    for (GraphNode& n : g.nodes) {
        if (!n.placed) {
            byLayer[n.layer].push_back(&n);
        }
    }
    for (auto& [layer, nodes] : byLayer) {
        std::sort(nodes.begin(), nodes.end(), [](const GraphNode* a, const GraphNode* b) {
            return a->created != b->created ? a->created < b->created : a->id < b->id;
        });
        const float y = m.originY + static_cast<float>(layer) * (m.nodeH + m.gapY);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            nodes[i]->x = m.originX + static_cast<float>(i) * (m.nodeW + m.gapX);
            nodes[i]->y = y;
        }
    }
}

std::vector<P2> angularPath(float ax, float ay, float bx, float by, float nodeH) {
    std::vector<P2> pts;
    pts.push_back({ax, ay});
    const float dx = bx - ax;
    const float dy = by - ay;
    const float ady = std::fabs(dy);
    const float stub = 16.0f;
    if (dx >= 2 * stub + ady) {
        // horizontal, 45-degree diagonal, horizontal
        const float flat = (dx - ady) / 2.0f;
        pts.push_back({ax + flat, ay});
        pts.push_back({ax + flat + ady, by});
    } else if (dx >= 2 * stub) {
        // not enough room for the diagonal: orthogonal through the middle
        pts.push_back({ax + dx / 2.0f, ay});
        pts.push_back({ax + dx / 2.0f, by});
    } else {
        // B is left of A (or too close): out, down below both, back, up
        const float lane = std::max(ay, by) + nodeH / 2.0f + 24.0f;
        pts.push_back({ax + stub, ay});
        pts.push_back({ax + stub, lane});
        pts.push_back({bx - stub, lane});
        pts.push_back({bx - stub, by});
    }
    pts.push_back({bx, by});
    return pts;
}

} // namespace ke::model
