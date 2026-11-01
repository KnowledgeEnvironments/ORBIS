// The world as a map, the listener's welcome. Pure: a function of the folds
// with no UI in it. Actors are who does things: people, agents and Models, each a live node in a
// column of its own. Equipment (Adapters, mounts, panes) is never a node: it is a port on its actor.
// Links are Routes that exist and Models that are bound; a Route that has carried a Message says so.
//
// The place of a node depends only on creation order and on what it holds, never on what it is
// doing, so the map reads the same every visit: state changes what a node says, not where it sits.
#pragma once

#include "model/Graph.hpp"
#include "model/PaneFold.hpp"
#include "model/Activity.hpp"
#include "model/Principals.hpp"
#include "model/Tasks.hpp"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ke::model {

enum class ActorKind { Person, Agent, Group, Model };
const char* name(ActorKind k);

struct MapPort {
    std::string id;
    std::string name;
    std::string kind; // adapter, mount, or a pane's kind
    bool pane = false;
    bool open = true; // a pane that has exited is not
};

struct MapActor {
    std::string id;
    std::string name;
    ActorKind kind = ActorKind::Agent;
    std::string parent; // the agent that made this one, drawn as the tree in its column
    int depth = 0;
    LoopState loop = LoopState::None;
    std::string stalled;
    std::string modelStatus;
    std::string detail; // a Model's model
    bool unsafe = false;
    int unread = 0;
    Activity activity = Activity::None;
    int waiting = 0;                // approvals its work is waiting on
    std::string waitingTask;        // the oldest of them
    std::string waitingSummary;     // what it asks, in one line
    bool canDecide = false;         // this client's Principal may decide waitingTask
    std::vector<MapPort> ports;     // the first few, in creation order
    int morePorts = 0;              // and how many are not drawn
    Seq created = 0;
    // Layout, in points.
    int column = 0;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    float bottom() const { return y + h; }
    float right() const { return x + w; }
    float midY() const { return y + h / 2.0f; }
};

enum class LinkKind { Route, Model };

struct MapLink {
    std::string from;
    std::string to;
    std::string route; // "" for a Model binding
    LinkKind kind = LinkKind::Route;
    bool traffic = false;
    int messages = 0;
    std::vector<P2> path; // set by layoutMap
};

struct MapHeading {
    std::string text;
    float x = 0.0f, y = 0.0f;
};

struct WorldMap {
    std::vector<MapActor> actors;
    std::vector<MapLink> links;
    std::vector<MapHeading> headings;
    float width = 0.0f, height = 0.0f;
    bool stacked = false; // one column: a phone, a narrow window; links are not drawn
    const MapActor* actor(std::string_view id) const;
    MapActor* actor(std::string_view id);
    // The link joining two actors, either way round, or nullptr.
    const MapLink* link(std::string_view a, std::string_view b) const;
};

struct MapState {
    std::string self;                  // this client's Principal
    std::set<std::string> unsafe;      // world/unsafe
    std::map<std::string, int> unread; // by actor
    std::map<std::string, Activity> activity; // by agent
};

inline constexpr int kMaxPorts = 3;

// Who is on the map, what they hold, how they are joined. No positions yet.
WorldMap buildMap(const Principals& principals, const Tasks& tasks, const PaneFold& panes, const MapState& state);

struct MapMetrics {
    float lineH = 18.0f;
    float charW = 8.0f;
    float top = 0.0f; // room above the columns, for the welcome card
};

// Places everything. Below the width of three columns it stacks them, so nothing shrinks to fit.
void layoutMap(WorldMap& map, float availableWidth, const MapMetrics& metrics);

// True when this world has not been used yet: one agent at most, and nothing said to it.
bool firstVisit(const WorldMap& map, const Principals& principals, const std::string& self);

} // namespace ke::model
