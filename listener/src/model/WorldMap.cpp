#include "model/WorldMap.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace ke::model {

namespace {

constexpr float kActorChars = 30.0f;  // a node's width, in characters
constexpr float kGutterChars = 8.0f;  // the channels between columns, where links run
constexpr float kIndentChars = 3.0f;  // one level of the tree
constexpr float kStackedChars = 52.0f; // one column stops growing here
constexpr float kGapLines = 0.8f;     // between two nodes
constexpr float kPadLines = 0.3f;     // inside a node, top and bottom

std::string fieldText(const PrincipalInfo& p, std::string_view name) {
    const FieldState* f = p.field(name);
    return f != nullptr && f->value.is_string() ? f->value.get<std::string>() : std::string();
}

// One line of a card's summary: nothing in a node may wrap.
std::string oneLine(std::string s) {
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
}

// Lines of text a node holds. Only what the world holds decides this, never what a node is doing:
// that is what keeps the map still.
int bodyLines(const MapActor& a) {
    const int ports = static_cast<int>(a.ports.size()) + (a.morePorts > 0 ? 1 : 0);
    switch (a.kind) {
    case ActorKind::Agent:
        return 3 + ports; // name, state, a line for its card
    case ActorKind::Person:
    case ActorKind::Group:
    case ActorKind::Model:
        return 2 + ports;
    }
    return 2;
}

} // namespace

const char* name(ActorKind k) {
    switch (k) {
    case ActorKind::Person:
        return "person";
    case ActorKind::Agent:
        return "agent";
    case ActorKind::Group:
        return "group";
    case ActorKind::Model:
        return "model";
    }
    return "";
}

const MapActor* WorldMap::actor(std::string_view id) const {
    for (const MapActor& a : actors) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

MapActor* WorldMap::actor(std::string_view id) {
    for (MapActor& a : actors) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

const MapLink* WorldMap::link(std::string_view a, std::string_view b) const {
    for (const MapLink& l : links) {
        if ((l.from == a && l.to == b) || (l.from == b && l.to == a)) {
            return &l;
        }
    }
    return nullptr;
}

WorldMap buildMap(const Principals& principals, const Tasks& tasks, const PaneFold& panes, const MapState& state) {
    WorldMap map;
    const auto rows = principals.ordered();

    for (const PrincipalInfo* p : rows) {
        MapActor a;
        if (p->cls == "principal" && p->drivenBy == "person") {
            a.kind = ActorKind::Person;
        } else if (p->cls == "principal" && p->drivenBy == "process") {
            a.kind = ActorKind::Agent;
        } else if (p->cls == "model") {
            a.kind = ActorKind::Model;
        } else if (p->cls == "group") {
            a.kind = ActorKind::Group;
        } else {
            continue; // the world itself, Adapters
        }
        a.id = p->id;
        a.name = principals.nameOf(p->id);
        a.created = p->created;
        a.loop = loopState(*p);
        a.stalled = p->loopStalled();
        a.unsafe = state.unsafe.count(p->id) != 0;
        if (auto it = state.unread.find(p->id); it != state.unread.end()) {
            a.unread = it->second;
        }
        if (auto it = state.activity.find(p->id); it != state.activity.end()) {
            a.activity = it->second;
        }
        if (a.kind == ActorKind::Model) {
            a.detail = fieldText(*p, "model/id");
            a.modelStatus = fieldText(*p, "model/status");
        }
        map.actors.push_back(std::move(a));
    }

    // The tree: an agent hangs under the agent that made it.
    for (MapActor& a : map.actors) {
        if (a.kind != ActorKind::Agent) {
            continue;
        }
        const PrincipalInfo* p = principals.get(a.id);
        const MapActor* maker = p != nullptr ? map.actor(p->createdBy) : nullptr;
        if (maker != nullptr && maker->kind == ActorKind::Agent && maker->id != a.id) {
            a.parent = maker->id;
        }
    }
    {
        // Children come right after their parent, so the column reads as a tree.
        std::vector<MapActor> agents, others;
        for (MapActor& a : map.actors) {
            (a.kind == ActorKind::Agent ? agents : others).push_back(std::move(a));
        }
        std::vector<MapActor> ordered;
        std::set<std::string> placed;
        std::function<void(MapActor&, int)> walk = [&](MapActor& a, int depth) {
            if (!placed.insert(a.id).second) {
                return;
            }
            a.depth = depth;
            ordered.push_back(a);
            for (MapActor& child : agents) {
                if (child.parent == a.id) {
                    walk(child, depth + 1);
                }
            }
        };
        for (MapActor& a : agents) {
            if (a.parent.empty()) {
                walk(a, 0);
            }
        }
        for (MapActor& a : agents) {
            walk(a, 0); // a cycle in the record still shows every actor
        }
        map.actors = std::move(others);
        map.actors.insert(map.actors.end(), std::make_move_iterator(ordered.begin()), std::make_move_iterator(ordered.end()));
    }

    // Equipment: a port on whoever holds it, else on its maker, else on the first agent.
    const MapActor* firstAgent = nullptr;
    for (const MapActor& a : map.actors) {
        if (a.kind == ActorKind::Agent) {
            firstAgent = &a;
            break;
        }
    }
    const std::string fallback = firstAgent != nullptr ? firstAgent->id : std::string();
    auto addPort = [&map](const std::string& owner, MapPort port) {
        MapActor* a = map.actor(owner);
        if (a == nullptr) {
            return;
        }
        if (static_cast<int>(a->ports.size()) < kMaxPorts) {
            a->ports.push_back(std::move(port));
        } else {
            ++a->morePorts;
        }
    };
    std::map<std::string, std::string> held;
    for (const PrincipalInfo* p : rows) {
        const FieldState* f = p->field("principal/adapters");
        if (f == nullptr || !f->value.is_array()) {
            continue;
        }
        for (const json& id : f->value) {
            if (id.is_string()) {
                held.emplace(id.get<std::string>(), p->id);
            }
        }
    }
    for (const PrincipalInfo* p : rows) {
        if (p->cls != "adapter") {
            continue;
        }
        auto it = held.find(p->id);
        const std::string binds = fieldText(*p, "adapter/binds");
        // A transport is plumbing the world runs on, not something an actor works with.
        if (it == held.end() && binds == "transport") {
            continue;
        }
        std::string owner = it != held.end() ? it->second : p->createdBy;
        if (map.actor(owner) == nullptr) {
            owner = fallback;
        }
        addPort(owner, MapPort{p->id, principals.nameOf(p->id), binds.empty() ? "adapter" : binds, false, true});
    }
    for (const PaneInfo* pane : panes.ordered()) {
        if (!pane->open()) {
            continue;
        }
        const std::string owner = map.actor(pane->owner) != nullptr ? pane->owner : fallback;
        addPort(owner, MapPort{pane->id, pane->shown(), pane->kind, true, true});
    }

    // Cards: an actor waiting on a person.
    for (const TaskInfo* t : tasks.pending()) {
        MapActor* a = map.actor(t->requestedBy);
        if (a == nullptr) {
            continue;
        }
        const bool mine = std::find(t->approvers.begin(), t->approvers.end(), state.self) != t->approvers.end();
        ++a->waiting;
        if (a->waitingTask.empty() || (mine && !a->canDecide)) {
            a->waitingTask = t->id;
            a->waitingSummary = oneLine(t->summary);
            a->canDecide = mine;
        }
    }

    // Links, from the same graph the views use: Routes between actors, and bound Models.
    const GraphModel g = buildGraph(principals);
    std::map<std::string, int> counts; // messages on a route between two actors
    auto key = [](const std::string& a, const std::string& b) { return a < b ? a + "|" + b : b + "|" + a; };
    for (const MessageRef& m : principals.messages()) {
        if (!m.recipient.empty() && m.from != m.recipient) {
            ++counts[key(m.from, m.recipient)];
        }
    }
    auto add = [&map, &counts, &key](const GraphEdge& e, LinkKind kind) {
        if (map.actor(e.from) == nullptr || map.actor(e.to) == nullptr || e.from == e.to) {
            return;
        }
        for (MapLink& have : map.links) {
            if (have.kind == kind && ((have.from == e.from && have.to == e.to) || (have.from == e.to && have.to == e.from))) {
                have.traffic = have.traffic || e.traffic;
                return;
            }
        }
        MapLink l;
        l.from = e.from;
        l.to = e.to;
        l.route = e.route;
        l.kind = kind;
        l.traffic = e.traffic;
        if (kind == LinkKind::Route) {
            if (auto it = counts.find(key(e.from, e.to)); it != counts.end()) {
                l.messages = it->second;
            }
        }
        map.links.push_back(std::move(l));
    };
    for (const GraphEdge& e : g.edges) {
        if (e.kind == EdgeKind::Route) {
            add(e, LinkKind::Route);
        } else if (e.kind == EdgeKind::Model) {
            add(e, LinkKind::Model);
        }
    }
    return map;
}

void layoutMap(WorldMap& map, float availableWidth, const MapMetrics& m) {
    const float pad = m.lineH * kPadLines;
    const float gap = m.lineH * kGapLines;
    const float headingH = m.lineH * 1.4f;
    const float actorW = m.charW * kActorChars;
    const float gutter = m.charW * kGutterChars;
    const float indent = m.charW * kIndentChars;
    const float margin = m.charW;

    int maxDepth = 0;
    for (const MapActor& a : map.actors) {
        maxDepth = std::max(maxDepth, a.depth);
    }
    const float agentColumn = actorW + static_cast<float>(maxDepth) * indent;
    const float wide = margin * 2.0f + actorW + gutter + agentColumn + gutter + actorW;
    map.stacked = availableWidth < wide;
    map.headings.clear();

    auto columnOf = [](const MapActor& a) {
        switch (a.kind) {
        case ActorKind::Person:
            return 0;
        case ActorKind::Agent:
        case ActorKind::Group:
            return 1;
        case ActorKind::Model:
            return 2;
        }
        return 1;
    };
    static const char* const kHeadings[3] = {"people", "agents", "models"};

    if (map.stacked) {
        const float width = std::min(std::max(availableWidth - margin * 2.0f, m.charW * 16.0f), m.charW * kStackedChars);
        float y = m.top;
        for (int column = 0; column < 3; ++column) {
            bool any = false;
            for (MapActor& a : map.actors) {
                if (columnOf(a) != column) {
                    continue;
                }
                if (!any) {
                    map.headings.push_back(MapHeading{kHeadings[column], margin, y});
                    y += headingH;
                    any = true;
                }
                a.column = 0;
                const float in = std::min(static_cast<float>(a.depth) * indent, width / 3.0f);
                a.x = margin + in;
                a.w = width - in;
                a.y = y;
                a.h = static_cast<float>(bodyLines(a)) * m.lineH + 2.0f * pad;
                y += a.h + gap;
            }
        }
        map.width = availableWidth;
        map.height = y;
        for (MapLink& l : map.links) {
            l.path.clear();
        }
        return;
    }

    const float columnX[3] = {margin, margin + actorW + gutter, margin + actorW + gutter + agentColumn + gutter};
    for (int column = 0; column < 3; ++column) {
        float y = m.top;
        bool any = false;
        for (MapActor& a : map.actors) {
            if (columnOf(a) != column) {
                continue;
            }
            if (!any) {
                map.headings.push_back(MapHeading{kHeadings[column], columnX[column], y});
                any = true;
                y += headingH;
            }
            a.column = column;
            a.x = columnX[column] + static_cast<float>(a.depth) * indent;
            a.w = actorW;
            a.y = y;
            a.h = static_cast<float>(bodyLines(a)) * m.lineH + 2.0f * pad;
            y += a.h + gap;
        }
        map.height = std::max(map.height, y);
    }
    map.width = columnX[2] + actorW + margin;

    // Links run in channels between the columns, each on a lane of its own, so no two share a line.
    float lane[2] = {0.0f, 0.0f};
    const float step = std::max(4.0f, gutter / 12.0f);
    auto nextLane = [&](int channel) {
        const float start = channel == 0 ? columnX[0] + actorW : columnX[1] + agentColumn;
        lane[channel] += step;
        const float room = gutter - 2.0f * step;
        return start + step + std::fmod(lane[channel], room);
    };
    // Where a link leaves a node: spread down its side, so two links never meet at one point.
    std::map<std::string, int> uses;
    auto side = [&](const MapActor& a, bool right) {
        const std::string k = a.id + (right ? "r" : "l");
        const int n = uses[k]++;
        const float y = a.y + m.lineH * 0.5f + static_cast<float>(n) * step;
        return P2{right ? a.right() : a.x, std::min(y, a.bottom() - 2.0f)};
    };
    for (MapLink& l : map.links) {
        const MapActor* a = map.actor(l.from);
        const MapActor* b = map.actor(l.to);
        l.path.clear();
        if (a == nullptr || b == nullptr) {
            continue;
        }
        if (a->column > b->column) {
            std::swap(a, b);
        }
        if (a->column == b->column) {
            // A bracket out of the side facing the next channel and back in.
            const bool right = a->column < 2;
            const float x = nextLane(a->column == 0 ? 0 : 1);
            const float laneX = right ? x : columnX[2] - (x - (columnX[1] + agentColumn));
            const P2 p = side(*a, right), q = side(*b, right);
            l.path = {p, {laneX, p.y}, {laneX, q.y}, q};
        } else if (b->column == a->column + 1) {
            const float x = nextLane(a->column);
            const P2 p = side(*a, true), q = side(*b, false);
            l.path = {p, {x, p.y}, {x, q.y}, q};
        } else {
            // A person to a Model: round the agents, along the top.
            const float x0 = nextLane(0), x1 = nextLane(1);
            const float over = m.top + 2.0f;
            const P2 p = side(*a, true), q = side(*b, false);
            l.path = {p, {x0, p.y}, {x0, over}, {x1, over}, {x1, q.y}, q};
        }
    }
}

bool firstVisit(const WorldMap& map, const Principals& principals, const std::string& self) {
    int agents = 0;
    for (const MapActor& a : map.actors) {
        if (a.kind == ActorKind::Agent) {
            ++agents;
        }
    }
    if (agents > 1) {
        return false;
    }
    for (const MessageRef& m : principals.messages()) {
        if (m.from == self) {
            return false;
        }
    }
    return true;
}

} // namespace ke::model
