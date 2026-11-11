// The map a person lands on, and the notifications fold: pure, no UI.
#include "chat/Notifications.hpp"
#include "model/WorldMap.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

constexpr const char* kWorld = "d470bce8-5578-400f-8f31-3504f178c6a8";
constexpr const char* kRoot = "034968ad-3358-4e57-a31b-d1bde0c15b31";
constexpr const char* kKid = "0349aaaa-3358-4e57-a31b-d1bde0c15b32";
constexpr const char* kGrandkid = "0349bbbb-3358-4e57-a31b-d1bde0c15b33";
constexpr const char* kPerson = "28b70d40-2916-4adb-967a-2dea363599dc";
constexpr const char* kModel = "b7785bd5-9284-4427-b0b0-54f95bb6f960";
constexpr const char* kShell = "ef281625-29cf-4e9c-b73d-2ad835b3819c";
constexpr const char* kWire = "ef282625-29cf-4e9c-b73d-2ad835b3819d";
constexpr const char* kRoute = "bfb9c6ed-8354-420d-a950-a9b9681af4bc";
constexpr const char* kMsg = "d67530c2-5f89-4c9a-a319-1d2fb171e740";
constexpr const char* kTask = "7a1b2c3d-0000-4000-8000-000000000042";
constexpr const char* kPane = "7a1b2c3d-0000-4000-8000-0000000000aa";
constexpr const char* kColl = "7a1b2c3d-0000-4000-8000-0000000000cc";

Seq g_seq = 0;

Event ev(const char* kind, const char* stream, const char* actor, json body) {
    Event e;
    e.seq = ++g_seq;
    e.id = "0b5a2c3e-2222-4c2a-8f1e-" + std::to_string(100000000000 + e.seq);
    e.stream = stream;
    e.actor = actor;
    e.kind = kind;
    e.at = "2026-10-31T10:00:00.000Z";
    e.body = std::move(body);
    return e;
}

json principal(const char* driven, const char* ns) {
    return {{"class", "principal"}, {"fields", {{"principal/driven-by", driven}, {"principal/namespace", ns}}}};
}

void named(Principals& p, const char* id, const char* name) {
    p.apply(ev("bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", name}, {"object", id}}));
}

struct World {
    Principals principals;
    Tasks tasks;
    PaneFold panes;
};

World sample() {
    World w;
    Principals& p = w.principals;
    p.apply(ev("created", kWorld, kWorld, principal("none", kWorld)));
    p.apply(ev("created", kRoot, kWorld, principal("process", kRoot)));
    named(p, kRoot, "root");
    p.apply(ev("created", kPerson, kRoot, principal("person", kPerson)));
    named(p, kPerson, "person");
    p.apply(ev("created", kModel, kRoot, {{"class", "model"}, {"fields", {{"principal/driven-by", "none"}}}}));
    named(p, kModel, "root.model");
    p.apply(ev("field-set", kModel, kRoot, {{"name", "model/id"}, {"type", "string"}, {"value", "glm"}, {"reader", "both"}}));
    p.apply(ev("created", kShell, kRoot, {{"class", "adapter"}, {"fields", {{"principal/driven-by", "none"}, {"adapter/binds", "host"}}}}));
    named(p, kShell, "shell.local");
    p.apply(ev("field-set", kShell, kRoot, {{"name", "adapter/binds"}, {"type", "string"}, {"value", "host"}, {"reader", "both"}}));
    p.apply(ev("created", kWire, kRoot, {{"class", "adapter"}, {"fields", {{"principal/driven-by", "none"}, {"adapter/binds", "transport"}}}}));
    p.apply(ev("field-set", kWire, kRoot, {{"name", "adapter/binds"}, {"type", "string"}, {"value", "transport"}, {"reader", "both"}}));
    p.apply(ev("created", kKid, kRoot, principal("process", kKid)));
    named(p, kKid, "grid");
    p.apply(ev("created", kGrandkid, kKid, principal("process", kGrandkid)));
    named(p, kGrandkid, "tiny");
    p.apply(ev("created", kRoute, kRoot, {{"class", "route"}, {"fields", {{"route/address", "root"}}}}));
    p.apply(ev("field-set", kRoot, kRoot, {{"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})}, {"value", json::array({kRoute})}, {"reader", "both"}}));
    p.apply(ev("field-set", kPerson, kRoot, {{"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})}, {"value", json::array({kRoute})}, {"reader", "both"}}));
    p.apply(ev("field-set", kRoot, kRoot, {{"name", "principal/adapters"}, {"type", "x"}, {"value", json::array({kShell})}, {"reader", "both"}}));
    p.apply(ev("field-set", kRoot, kRoot, {{"name", "principal/models"}, {"type", json::array({"list", json::array({"ref", "model"})})}, {"value", json::array({kModel})}, {"reader", "both"}}));
    return w;
}

MapMetrics metrics() { return MapMetrics{18.0f, 8.0f, 0.0f}; }

void noOverlap(const WorldMap& map) {
    for (std::size_t i = 0; i < map.actors.size(); ++i) {
        for (std::size_t j = i + 1; j < map.actors.size(); ++j) {
            const MapActor& a = map.actors[i];
            const MapActor& b = map.actors[j];
            const bool apart = a.right() <= b.x || b.right() <= a.x || a.bottom() <= b.y || b.bottom() <= a.y;
            CHECK(apart);
        }
    }
    for (const MapActor& a : map.actors) {
        CHECK(a.x >= 0.0f);
        CHECK(a.right() <= map.width);
        for (const MapHeading& h : map.headings) {
            const bool clear = h.y + 18.0f <= a.y || h.x >= a.right() || h.y >= a.bottom() || h.x + 80.0f <= a.x;
            CHECK(clear);
        }
    }
}

} // namespace

TEST_CASE("map: people, agents and models are actors; the world and Adapters are not", "[map]") {
    World w = sample();
    const WorldMap map = buildMap(w.principals, w.tasks, w.panes, {});
    REQUIRE(map.actor(kRoot) != nullptr);
    CHECK(map.actor(kRoot)->kind == ActorKind::Agent);
    CHECK(map.actor(kPerson)->kind == ActorKind::Person);
    CHECK(map.actor(kModel)->kind == ActorKind::Model);
    CHECK(map.actor(kModel)->detail == "glm");
    CHECK(map.actor(kWorld) == nullptr);
    CHECK(map.actor(kShell) == nullptr);
}

TEST_CASE("map: equipment is a port on its actor, and plumbing is left out", "[map]") {
    World w = sample();
    w.panes.apply(ev("created", kPane, kKid, {{"class", "pane"}, {"fields", {{"pane/kind", "terminal"}, {"pane/state", "open"}, {"pane/owner", kKid}, {"pane/title", "zsh"}}}}));
    const WorldMap map = buildMap(w.principals, w.tasks, w.panes, {});
    REQUIRE(map.actor(kRoot)->ports.size() == 1);
    CHECK(map.actor(kRoot)->ports[0].name == "shell.local");
    CHECK(map.actor(kRoot)->ports[0].kind == "host");
    REQUIRE(map.actor(kKid)->ports.size() == 1);
    CHECK(map.actor(kKid)->ports[0].pane);
    CHECK(map.actor(kKid)->ports[0].name == "zsh");
}

TEST_CASE("map: an agent hangs under the agent that made it", "[map]") {
    World w = sample();
    const WorldMap map = buildMap(w.principals, w.tasks, w.panes, {});
    CHECK(map.actor(kRoot)->depth == 0);
    CHECK(map.actor(kKid)->depth == 1);
    CHECK(map.actor(kKid)->parent == kRoot);
    CHECK(map.actor(kGrandkid)->depth == 2);
    CHECK(map.actor(kPerson)->parent.empty());
}

TEST_CASE("map: links are Routes between actors and bound Models", "[map]") {
    World w = sample();
    WorldMap map = buildMap(w.principals, w.tasks, w.panes, {});
    REQUIRE(map.link(kRoot, kPerson) != nullptr);
    CHECK(map.link(kRoot, kPerson)->kind == LinkKind::Route);
    CHECK_FALSE(map.link(kRoot, kPerson)->traffic);
    REQUIRE(map.link(kRoot, kModel) != nullptr);
    CHECK(map.link(kRoot, kModel)->kind == LinkKind::Model);
    w.principals.apply(ev("delivered", kRoute, kPerson, {{"message", kMsg}, {"route", kRoute}, {"recipient", kRoot}}));
    map = buildMap(w.principals, w.tasks, w.panes, {});
    CHECK(map.link(kRoot, kPerson)->traffic);
    CHECK(map.link(kRoot, kPerson)->messages == 1);
}

TEST_CASE("map: a card shows on the actor that waits, with buttons only for who may decide", "[map]") {
    World w = sample();
    w.tasks.apply(ev("created", kTask, kKid, {{"class", "task"}, {"fields", {{"task/kind", "approval"}, {"task/state", "pending"}, {"task/summary", "make\ntest"}, {"task/requested-by", kKid}, {"task/approvers", json::array({kPerson})}}}}));
    MapState mine;
    mine.self = kPerson;
    WorldMap map = buildMap(w.principals, w.tasks, w.panes, mine);
    REQUIRE(map.actor(kKid)->waiting == 1);
    CHECK(map.actor(kKid)->waitingTask == kTask);
    CHECK(map.actor(kKid)->waitingSummary == "make test");
    CHECK(map.actor(kKid)->canDecide);
    MapState other;
    other.self = kRoot;
    map = buildMap(w.principals, w.tasks, w.panes, other);
    CHECK(map.actor(kKid)->waiting == 1);
    CHECK_FALSE(map.actor(kKid)->canDecide);
}

TEST_CASE("map: state changes what a node says, never where it sits", "[map]") {
    World w = sample();
    WorldMap before = buildMap(w.principals, w.tasks, w.panes, {});
    layoutMap(before, 1400.0f, metrics());
    MapState loud;
    loud.self = kPerson;
    loud.unsafe.insert(kKid);
    loud.unread[kRoot] = 4;
    w.principals.apply(ev("field-set", kRoot, kRoot, {{"name", "loop/iteration"}, {"type", "number"}, {"value", 3}, {"reader", "both"}}));
    w.tasks.apply(ev("created", kTask, kKid, {{"class", "task"}, {"fields", {{"task/kind", "approval"}, {"task/state", "pending"}, {"task/summary", "x"}, {"task/requested-by", kKid}, {"task/approvers", json::array({kPerson})}}}}));
    WorldMap after = buildMap(w.principals, w.tasks, w.panes, loud);
    layoutMap(after, 1400.0f, metrics());
    REQUIRE(before.actors.size() == after.actors.size());
    for (std::size_t i = 0; i < before.actors.size(); ++i) {
        CHECK(before.actors[i].id == after.actors[i].id);
        CHECK(before.actors[i].x == after.actors[i].x);
        CHECK(before.actors[i].y == after.actors[i].y);
        CHECK(before.actors[i].h == after.actors[i].h);
    }
}

TEST_CASE("map: nothing overlaps, in columns or stacked at phone width", "[map]") {
    World w = sample();
    WorldMap map = buildMap(w.principals, w.tasks, w.panes, {});
    layoutMap(map, 1400.0f, metrics());
    CHECK_FALSE(map.stacked);
    noOverlap(map);
    CHECK(map.actor(kPerson)->column == 0);
    CHECK(map.actor(kRoot)->column == 1);
    CHECK(map.actor(kModel)->column == 2);
    CHECK(map.actor(kKid)->x > map.actor(kRoot)->x);
    for (const MapLink& l : map.links) {
        CHECK(l.path.size() >= 2);
    }
    layoutMap(map, 340.0f, metrics());
    CHECK(map.stacked);
    noOverlap(map);
    CHECK(map.headings.size() == 3);
    for (const MapActor& a : map.actors) {
        CHECK(a.right() <= 340.0f);
    }
}

TEST_CASE("map: two links never share a lane", "[map]") {
    World w = sample();
    WorldMap map = buildMap(w.principals, w.tasks, w.panes, {});
    MapLink extra;
    extra.from = kKid;
    extra.to = kModel;
    extra.kind = LinkKind::Model;
    map.links.push_back(extra);
    extra.from = kGrandkid;
    map.links.push_back(extra);
    layoutMap(map, 1400.0f, metrics());
    std::vector<float> lanes;
    for (const MapLink& l : map.links) {
        REQUIRE(l.path.size() == 4);
        lanes.push_back(l.path[1].x);
    }
    std::sort(lanes.begin(), lanes.end());
    CHECK(std::adjacent_find(lanes.begin(), lanes.end()) == lanes.end());
}

TEST_CASE("map: the first visit ends when a person speaks or a second agent exists", "[map]") {
    World w = sample();
    WorldMap map = buildMap(w.principals, w.tasks, w.panes, {});
    CHECK_FALSE(firstVisit(map, w.principals, kPerson)); // three agents
    map.actors.erase(std::remove_if(map.actors.begin(), map.actors.end(),
                                    [](const MapActor& a) { return a.id == kKid || a.id == kGrandkid; }),
                     map.actors.end());
    CHECK(firstVisit(map, w.principals, kPerson));
    w.principals.apply(ev("sent", kRoute, kPerson, {{"message", kMsg}, {"route", kRoute}}));
    CHECK_FALSE(firstVisit(map, w.principals, kPerson));
}

TEST_CASE("unread: only what arrived after attach, cleared by reading", "[notify]") {
    std::vector<MessageRef> messages;
    MessageRef old;
    old.message = kMsg;
    old.from = kRoot;
    old.recipient = kPerson;
    old.seq = 5;
    messages.push_back(old);
    chat::MessageUnread u;
    u.update(messages, kPerson, -1, "");
    CHECK(u.from(kRoot) == 0);
    u.update(messages, kPerson, 10, "");
    CHECK(u.from(kRoot) == 0);
    MessageRef fresh = old;
    fresh.seq = 11;
    messages.push_back(fresh);
    MessageRef mine = old;
    mine.seq = 12;
    mine.from = kPerson;
    messages.push_back(mine);
    MessageRef theirs = old;
    theirs.seq = 13;
    theirs.recipient = kKid;
    messages.push_back(theirs);
    u.update(messages, kPerson, 10, "");
    CHECK(u.from(kRoot) == 1);
    u.update(messages, kPerson, 10, kRoot);
    CHECK(u.from(kRoot) == 0);
}

TEST_CASE("notifications: seeded from notify.list, nothing old is loud", "[notify]") {
    chat::Notifications n;
    n.seed(json{{"result", {{"collection", kColl}, {"unread", 1}, {"rows", json::array({
        json{{"notification", kTask}, {"level", "urgent"}, {"text", "disk full"}, {"from", kRoot}, {"at", "t"}, {"read", false}, {"object", kKid}},
        json{{"notification", kMsg}, {"level", "info"}, {"text", "ok"}, {"from", kKid}, {"at", "t"}, {"read", true}}})}}}});
    CHECK(n.collection() == kColl);
    REQUIRE(n.rows().size() == 2);
    CHECK(n.unread() == 1);
    CHECK(n.unreadFrom(kRoot) == 1);
    CHECK(n.unreadFrom(kKid) == 0);
    CHECK(n.rows()[0].object == kKid);
    CHECK(n.worstUnread() == "urgent");
    CHECK(n.takeLoud().empty());
}

TEST_CASE("notifications: a live one lands on top and only the loud reach the OS, once", "[notify]") {
    chat::Notifications n;
    json fields = {{"notification/to", json::array({kPerson})}, {"notification/from", kRoot},
                   {"notification/level", "attention"}, {"notification/text", "look"}, {"notification/at", "t"}};
    Event e = ev("created", kTask, kRoot, {{"class", "object"}, {"fields", fields}});
    CHECK(n.apply(e, kPerson));
    CHECK_FALSE(n.apply(e, kPerson)); // the same event twice is one notification
    REQUIRE(n.rows().size() == 1);
    CHECK(n.takeLoud().size() == 1);
    CHECK(n.takeLoud().empty());
    fields["notification/level"] = "info";
    CHECK(n.apply(ev("created", kMsg, kRoot, {{"class", "object"}, {"fields", fields}}), kPerson));
    CHECK(n.takeLoud().empty());
    CHECK(n.rows()[0].id == kMsg);
    CHECK(n.unread() == 2);
}

TEST_CASE("notifications: someone else's are left alone; read state follows the world", "[notify]") {
    chat::Notifications n;
    json fields = {{"notification/to", json::array({kPerson})}, {"notification/from", kRoot},
                   {"notification/level", "info"}, {"notification/text", "look"}, {"notification/at", "t"}};
    CHECK_FALSE(n.apply(ev("created", kTask, kRoot, {{"class", "object"}, {"fields", fields}}), kRoot));
    CHECK_FALSE(n.apply(ev("created", kKid, kRoot, {{"class", "object"}, {"fields", {{"name", "not one"}}}}), kPerson));
    CHECK(n.apply(ev("created", kTask, kRoot, {{"class", "object"}, {"fields", fields}}), kPerson));
    CHECK_FALSE(n.apply(ev("field-set", kTask, kRoot, {{"name", "notification/read-by"}, {"value", json::array({kRoot})}}), kPerson));
    CHECK(n.unread() == 1);
    CHECK(n.apply(ev("field-set", kTask, kPerson, {{"name", "notification/read-by"}, {"value", json::array({kPerson})}}), kPerson));
    CHECK(n.unread() == 0);
    n.markAllRead();
    CHECK(n.unread() == 0);
}
