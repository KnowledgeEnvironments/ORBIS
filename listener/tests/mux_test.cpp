// The pane fold and the layout tree: what the multiplexer stands on, without a window.
#include "model/PaneFold.hpp"
#include "model/PaneLayout.hpp"
#include "model/Snapshot.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

Event ev(Seq seq, std::string kind, std::string stream, json body) {
    Event e;
    e.seq = seq;
    e.kind = std::move(kind);
    e.stream = std::move(stream);
    e.actor = "person";
    e.body = std::move(body);
    return e;
}

const LayoutRect kRect{0, 0, 1000, 600};

} // namespace

TEST_CASE("panes fold from created, field-set and the pane events") {
    PaneFold f;
    CHECK_FALSE(f.apply(ev(1, "created", "x", {{"class", "object"}, {"fields", {{"note", "n"}}}})));
    CHECK(f.apply(ev(2, "created", "p1", {{"class", "object"}, {"fields", {{"pane/kind", "terminal"}, {"pane/target", "a1"}, {"pane/title", "sh"}, {"pane/state", "open"}, {"pane/size", {{"cols", 80}, {"rows", 24}}}}}})));
    const PaneInfo* p = f.get("p1");
    REQUIRE(p != nullptr);
    CHECK(p->kind == "terminal");
    CHECK(p->cols == 80);
    CHECK(p->open());
    CHECK(f.apply(ev(3, "field-set", "p1", {{"name", "pane/size"}, {"value", {{"cols", 120}, {"rows", 40}}}})));
    CHECK(f.get("p1")->rows == 40);
    Event in = ev(4, "pane-input", "p1", {{"pane", "p1"}, {"who", "root"}, {"text", "ls"}});
    in.via = "eval";
    CHECK(f.apply(in));
    CHECK(f.get("p1")->lastInput.model);
    CHECK(f.get("p1")->lastInput.text == "ls");
    CHECK(f.apply(ev(5, "field-set", "p1", {{"name", "pane/exit"}, {"value", 2}})));
    CHECK(f.apply(ev(6, "pane-closed", "p1", {{"pane", "p1"}})));
    CHECK_FALSE(f.get("p1")->open());
    CHECK(f.get("p1")->exitCode == 2);
}

TEST_CASE("a pane seeded from a snapshot equals one fed its events") {
    json o = {{"object/id", "p2"}, {"object/class", "object"}, {"object/fields", {{"pane/kind", {{"field/value", "editor"}}}, {"pane/title", {{"field/value", "notes"}}}}}};
    PaneFold f;
    f.seed(SnapshotObject::parse(o));
    REQUIRE(f.get("p2") != nullptr);
    CHECK(f.get("p2")->kind == "editor");
    CHECK(f.get("p2")->title == "notes");
}

TEST_CASE("a pane on a Field names it, so a second EDIT finds the same tab") {
    json o = {{"object/id", "p3"}, {"object/class", "object"},
              {"object/fields", {{"pane/kind", {{"field/value", "editor"}}}, {"pane/field", {{"field/value", "instructions/own"}}}}}};
    PaneFold f;
    f.seed(SnapshotObject::parse(o));
    REQUIRE(f.get("p3") != nullptr);
    CHECK(f.get("p3")->field == "instructions/own");
}

TEST_CASE("the layout places, tabs, splits and survives a round trip") {
    PaneLayout l;
    CHECK(l.empty());
    l.add("a");
    l.add("b", "a");
    REQUIRE(l.leaves(kRect).size() == 1);
    CHECK(l.leaves(kRect)[0].active == 1);
    l.place("c", "b", Edge::Right);
    l.place("d", "c", Edge::Bottom);
    CHECK(l.leaves(kRect).size() == 3);
    CHECK(l.dividers(kRect, 4).size() == 2);
    const PaneLayout again = PaneLayout::fromJson(l.toJson());
    CHECK(again.toJson() == l.toJson());
    l.remove("c");
    CHECK(l.leaves(kRect).size() == 2);
    l.remove("d");
    CHECK(l.leaves(kRect).size() == 1);
    l.remove("a");
    l.remove("b");
    CHECK(l.empty());
    CHECK(PaneLayout::fromJson(json::object()).empty());
    CHECK(PaneLayout::fromJson(json{{"split", "h"}, {"a", 3}}).empty());
}

TEST_CASE("focus moves to the nearest neighbour, and stops at the edge") {
    PaneLayout l;
    l.add("left");
    l.place("top", "left", Edge::Right);
    l.place("bottom", "top", Edge::Bottom);
    CHECK(l.neighbour("left", Direction::Right, kRect).has_value());
    CHECK(*l.neighbour("top", Direction::Down, kRect) == "bottom");
    CHECK(*l.neighbour("bottom", Direction::Up, kRect) == "top");
    CHECK(*l.neighbour("bottom", Direction::Left, kRect) == "left");
    CHECK_FALSE(l.neighbour("left", Direction::Left, kRect).has_value());
    CHECK_FALSE(l.neighbour("gone", Direction::Left, kRect).has_value());
}

TEST_CASE("moving a pane rearranges, a stale layout drops dead panes, ratios stay sane") {
    PaneLayout l;
    l.add("a");
    l.place("b", "a", Edge::Right);
    l.place("c", "b", Edge::Bottom);
    l.move("c", "a", Edge::Tabs);
    CHECK(l.leaves(kRect).size() == 2);
    l.move("a", "a", Edge::Left); // onto itself: nothing
    CHECK(l.contains("a"));
    l.setRatio("", 0.99f);
    CHECK(l.toJson()["ratio"].get<float>() <= 0.99f);
    l.retain({"b"});
    CHECK(l.panes() == std::vector<std::string>{"b"});
}
