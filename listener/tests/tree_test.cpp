// The PRINCIPALS tree: real relations, shared things under each user, the rest unattached.
#include "model/ObjectTree.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

const char* kWorld = "00000000-aaaa-4000-8000-000000000000";
const char* kRoot = "11111111-aaaa-4000-8000-000000000001";
const char* kPerson = "22222222-aaaa-4000-8000-000000000002";
const char* kGrid = "33333333-aaaa-4000-8000-000000000003";
const char* kModel = "44444444-aaaa-4000-8000-000000000004";
const char* kFs = "55555555-aaaa-4000-8000-000000000005";
const char* kRoute = "66666666-aaaa-4000-8000-000000000006";
const char* kJunk = "77777777-aaaa-4000-8000-000000000007";
const char* kShell = "88888888-aaaa-4000-8000-000000000008";
const char* kMount = "99999999-aaaa-4000-8000-000000000009";
const char* kBridge = "aaaaaaaa-aaaa-4000-8000-00000000000a";

Seq g_seq = 0;

Event ev(const char* kind, const char* stream, const char* actor, json body) {
    Event e;
    e.seq = ++g_seq;
    e.kind = kind;
    e.stream = stream;
    e.actor = actor;
    e.body = std::move(body);
    e.id = "e-" + std::to_string(e.seq);
    e.at = "2026-10-30T20:00:00.000Z";
    return e;
}

json made(const char* cls, const char* drivenBy = nullptr) {
    json fields = json::object();
    if (drivenBy) {
        fields["principal/driven-by"] = drivenBy;
    }
    return json{{"class", cls}, {"fields", fields}};
}

void field(Principals& p, const char* object, const char* name, json value) {
    p.apply(ev("field-set", object, kRoot, {{"name", name}, {"type", "list"}, {"value", std::move(value)}, {"reader", "both"}}));
}

void name(Principals& p, const char* object, const char* n) {
    p.apply(ev("bound", kRoot, kRoot, {{"namespace", kWorld}, {"name", n}, {"object", object}}));
}

Principals world() {
    Principals p;
    g_seq = 0;
    p.apply(ev("created", kWorld, kWorld, made("principal", "none")));
    p.apply(ev("created", kRoot, kWorld, made("principal", "process")));
    p.apply(ev("created", kPerson, kRoot, made("principal", "person")));
    p.apply(ev("created", kGrid, kRoot, made("principal", "process")));
    p.apply(ev("created", kModel, kRoot, made("model")));
    p.apply(ev("created", kFs, kRoot, made("adapter")));
    p.apply(ev("created", kJunk, kRoot, made("adapter")));
    p.apply(ev("created", kRoute, kRoot, made("route")));
    name(p, kRoot, "root");
    name(p, kPerson, "person");
    name(p, kGrid, "grid");
    field(p, kRoot, "principal/models", json::array({kModel}));
    field(p, kGrid, "principal/models", json::array({kModel}));
    field(p, kGrid, "principal/adapters", json::array({kFs}));
    field(p, kGrid, "principal/routes", json::array({kRoute}));
    field(p, kRoute, "route/members", json::array({kGrid, kPerson}));
    return p;
}

const TreeRoot* rootOf(const ObjectTree& t, const char* id) {
    for (const TreeRoot& r : t.roots) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace

const TreeLine* lineOf(const TreeRoot& r, const char* label) {
    for (const TreeLine& l : r.lines) {
        if (l.label == label) {
            return &l;
        }
    }
    return nullptr;
}

TEST_CASE("the top level is who acts: people first, then agents, by name") {
    const ObjectTree t = buildObjectTree(world(), kPerson);
    REQUIRE(t.roots.size() == 3);
    CHECK(t.roots[0].id == kPerson);
    CHECK(t.roots[0].role == "you");
    CHECK(t.roots[1].id == kGrid);
    CHECK(t.roots[1].role == "agent");
    CHECK(t.roots[2].id == kRoot);
    CHECK(buildObjectTree(world(), kGrid).roots[0].role == "person"); // someone else's client
}

TEST_CASE("children are one line per purpose, in plain words") {
    Principals p = world();
    p.apply(ev("created", kShell, kRoot, made("adapter")));
    p.apply(ev("field-set", kShell, kRoot, {{"name", "adapter/binds"}, {"type", "keyword"}, {"value", "host"}, {"reader", "both"}}));
    p.apply(ev("created", kMount, kRoot, made("adapter")));
    p.apply(ev("field-set", kMount, kRoot, {{"name", "adapter/read-only"}, {"type", "boolean"}, {"value", true}, {"reader", "both"}}));
    field(p, kGrid, "principal/adapters", json::array({kFs, kShell, kMount}));
    const ObjectTree t = buildObjectTree(p);
    const TreeRoot* grid = rootOf(t, kGrid);
    REQUIRE(grid != nullptr);
    REQUIRE(lineOf(*grid, "model"));
    CHECK(lineOf(*grid, "model")->items[0].id == kModel);
    REQUIRE(lineOf(*grid, "files"));
    CHECK(lineOf(*grid, "files")->items[0].id == kFs);
    REQUIRE(lineOf(*grid, "shell"));
    CHECK(lineOf(*grid, "shell")->items[0].id == kShell);
    REQUIRE(lineOf(*grid, "reads")); // the world says so
    CHECK(lineOf(*grid, "reads")->items[0].id == kMount);
}

TEST_CASE("a shared thing simply appears where it is used, and knows who else") {
    const ObjectTree t = buildObjectTree(world());
    const TreeRoot* root = rootOf(t, kRoot);
    const TreeRoot* grid = rootOf(t, kGrid);
    REQUIRE(root != nullptr);
    REQUIRE(grid != nullptr);
    REQUIRE(lineOf(*root, "model"));
    CHECK(lineOf(*root, "model")->items[0].others == std::vector<std::string>{kGrid});
    REQUIRE(lineOf(*grid, "model"));
    CHECK(lineOf(*grid, "model")->items[0].others == std::vector<std::string>{kRoot});
    REQUIRE(lineOf(*grid, "files"));
    CHECK(lineOf(*grid, "files")->items[0].others.empty());
}

TEST_CASE("Routes collapse into who is at the other end") {
    Principals p = world();
    field(p, kRoute, "route/members", json::array({kGrid, kPerson, kRoot}));
    const ObjectTree t = buildObjectTree(p);
    const TreeRoot* grid = rootOf(t, kGrid);
    REQUIRE(grid != nullptr);
    const TreeLine* talks = lineOf(*grid, "talks to");
    REQUIRE(talks);
    REQUIRE(talks->items.size() == 2);
    CHECK(talks->items[0].id == kPerson);
    CHECK(talks->items[0].route == kRoute); // the Route stays reachable
    field(p, kRoute, "route/members", json::array({kGrid}));
    const ObjectTree t2 = buildObjectTree(p);
    const TreeRoot* alone = rootOf(t2, kGrid);
    REQUIRE(alone != nullptr);
    REQUIRE(lineOf(*alone, "talks to"));
    CHECK(lineOf(*alone, "talks to")->items[0].id == kRoute); // nobody else: the Route itself
}

TEST_CASE("an appliance with no Model is not an agent, and no live user means unattached") {
    Principals p = world();
    name(p, kJunk, "grid.leftover");
    p.apply(ev("created", kBridge, kRoot, made("principal", "process")));
    const ObjectTree t = buildObjectTree(p);
    CHECK(rootOf(t, kBridge) == nullptr);
    CHECK(std::find(t.unattached.begin(), t.unattached.end(), kBridge) != t.unattached.end());
    CHECK(std::find(t.unattached.begin(), t.unattached.end(), kJunk) != t.unattached.end());
    CHECK(std::find(t.unattached.begin(), t.unattached.end(), kFs) == t.unattached.end());
    CHECK(std::find(t.unattached.begin(), t.unattached.end(), kWorld) == t.unattached.end());
    field(p, kBridge, "principal/models", json::array({kModel}));
    const ObjectTree with = buildObjectTree(p);
    CHECK(rootOf(with, kBridge) != nullptr);
}

TEST_CASE("a retired object leaves the tree and comes back restored") {
    Principals p = world();
    p.apply(ev("retired", kFs, kRoot, {{"names", json::array({json::array({kWorld, "grid.fs"})})}}));
    CHECK(p.retired(kFs));
    CHECK(p.retiredObjects().at(kFs).name == "grid.fs");
    const ObjectTree t = buildObjectTree(p);
    const TreeRoot* grid = rootOf(t, kGrid);
    REQUIRE(grid != nullptr);
    CHECK(lineOf(*grid, "files") == nullptr);
    p.apply(ev("restored", kFs, kRoot, json::object()));
    CHECK_FALSE(p.retired(kFs));
}

TEST_CASE("the legend explains every word the tree uses") {
    std::vector<std::string> words;
    for (const auto& [word, meaning] : treeLegend()) {
        CHECK_FALSE(meaning.empty());
        words.push_back(word);
    }
    for (const char* w : {"you", "agent", "model", "files", "reads", "shell", "works in", "talks to", "UNATTACHED"}) {
        CHECK(std::find(words.begin(), words.end(), w) != words.end());
    }
}

TEST_CASE("what the world stands on cannot be deleted") {
    const Principals p = world();
    CHECK_FALSE(deleteBlocked(p, kWorld).empty());
    CHECK_FALSE(deleteBlocked(p, kRoot).empty());
    CHECK_FALSE(deleteBlocked(p, kPerson).empty());
    CHECK(deleteBlocked(p, kGrid).empty());
    CHECK(deleteBlocked(p, kFs).empty());
}
