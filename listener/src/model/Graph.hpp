// The world as a graph, derived from the Principals fold (pure, no UI):
//   nodes  every Principal the fold knows: principals, models, adapters, groups
//   edges  Route   the Route's `route/members` when the world records them, else two Principals
//                  holding the same Route in `principal/routes`; plus any delivery on a Route
//                  from one Principal to another (traffic); a Route held by one Principal with
//                  no traffic is an Inbox stub on that node
//          Model  principal -> each of its `principal/models`
//          Workspace principal -> each of its `principal/workspaces`
//          Adapter principal -> the `route/transport` Adapter of each Route it holds
//   groups a Group's `group/members`, drawn as a dashed enclosure
// Placement is state in the world: a node whose object carries `view/position` {x y} is
// `placed`; the rest get a deterministic layered layout that is never written back until the
// user moves something (spec principle 8: the client owns legibility, the log owns state).
#pragma once

#include "model/Principals.hpp"

#include <string>
#include <vector>

namespace ke::model {

struct GraphNode {
    std::string id;
    std::string name;
    std::string cls;
    std::string drivenBy;
    LoopState loop = LoopState::None;
    bool isWorld = false;
    bool placed = false; // position came from `view/position`
    float x = 0.0f;
    float y = 0.0f;
    int layer = 0;
    Seq created = 0;
    std::string sub;    // models: model/id (shown under the name)
    std::string status; // models: model/status (ok | unreachable | unauthenticated | unknown), "" when unset
};

enum class EdgeKind { Route, Inbox, Model, Workspace, Adapter };
const char* name(EdgeKind k);

struct GraphEdge {
    std::string from;
    std::string to; // == from for an Inbox stub
    std::string route; // the Route id for Route/Inbox/Adapter edges
    std::string label;
    EdgeKind kind = EdgeKind::Route;
    bool traffic = false; // seen as a delivery, not only as a shared holding
    bool operator==(const GraphEdge&) const = default;
};

struct GraphGroup {
    std::string id;
    std::string name;
    std::vector<std::string> members;
};

struct GraphModel {
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
    std::vector<GraphGroup> groups;
    const GraphNode* node(std::string_view id) const;
    GraphNode* node(std::string_view id);
};

struct LayoutMetrics {
    float nodeW = 200.0f;
    float nodeH = 58.0f;
    float gapX = 48.0f;
    float gapY = 72.0f;
    float originX = 40.0f;
    float originY = 40.0f;
};

// Nodes, edges and groups from the fold. Nodes carry their `view/position` when the world has
// one; the others are at (0,0) and unplaced until layoutLayers runs.
GraphModel buildGraph(const Principals& principals);

// Layered placement for unplaced nodes, deterministic in the fold's creation order:
//   0 the world Principal   1 process Principals and Groups   2 models, adapters, workspaces
//   3 person Principals     4 anything else
// Placed nodes keep their position. No two laid-out nodes overlap.
void layoutLayers(GraphModel& g, const LayoutMetrics& m = {});

// A rectangle as a 45-degree-bend polyline from the right middle of A to the left middle of B
// (angular whimsy, never a bezier): horizontal, then diagonal, then horizontal when there is
// room; orthogonal otherwise; a detour below both when B is left of A. Points in graph units.
struct P2 {
    float x, y;
    bool operator==(const P2&) const = default;
};
std::vector<P2> angularPath(float ax, float ay, float bx, float by, float nodeH);

} // namespace ke::model
