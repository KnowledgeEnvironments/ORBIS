// The graph derived from the fold, its layered layout, and the Evaluations fold. Events in the
// JSON form the world pushes; no UI.
#include "model/Evaluations.hpp"
#include "model/Graph.hpp"
#include "model/Principals.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

constexpr const char* kWorld = "d470bce8-5578-400f-8f31-3504f178c6a8";
constexpr const char* kRoot = "034968ad-3358-4e57-a31b-d1bde0c15b31";
constexpr const char* kWs = "7c1ca986-e89c-47cf-bcf2-7d976404837b";
constexpr const char* kModel = "b7785bd5-9284-4427-b0b0-54f95bb6f960";
constexpr const char* kAdapter = "ef281625-29cf-4e9c-b73d-2ad835b3819c";
constexpr const char* kRoute = "bfb9c6ed-8354-420d-a950-a9b9681af4bc";
constexpr const char* kPerson = "28b70d40-2916-4adb-967a-2dea363599dc";
constexpr const char* kMsg = "d67530c2-5f89-4c9a-a319-1d2fb171e740";
constexpr const char* kEval = "03d1ac77-b9e5-498b-b28e-da884b8ddfea";
constexpr const char* kEval2 = "2f266b21-0000-4000-8000-000000000002";
constexpr const char* kSer = "cac07e7d-6eb2-497a-9163-eb222ad1a32e";

Event ev(Seq seq, const char* kind, const char* stream, const char* actor, json body, const char* via = nullptr) {
    Event e;
    e.seq = seq;
    e.id = "0b5a2c3e-2222-4c2a-8f1e-" + std::to_string(100000000000 + seq);
    e.stream = stream;
    e.actor = actor;
    e.kind = kind;
    e.at = "2026-10-30T15:18:" + std::string(seq % 60 < 10 ? "0" : "") + std::to_string(seq % 60) + ".000Z";
    e.body = std::move(body);
    if (via) {
        e.via = via;
    }
    return e;
}

json principalFields(const char* driven, const char* ns) {
    return {{"principal/driven-by", driven}, {"principal/namespace", ns}};
}

// The kernel-zero set: world, root (process), root.workspace, root.model, transport.listener
// (adapter), root.route held by root, person.
Principals kernelZero() {
    Principals p;
    p.apply(ev(1, "created", kWorld, kWorld, {{"class", "principal"}, {"fields", principalFields("none", kWorld)}}));
    p.apply(ev(2, "created", kRoot, kWorld, {{"class", "principal"}, {"fields", principalFields("process", kRoot)}}));
    p.apply(ev(3, "bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", "world"}, {"object", kWorld}}));
    p.apply(ev(4, "bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", "root"}, {"object", kRoot}}));
    p.apply(ev(5, "created", kWs, kRoot, {{"class", "principal"}, {"fields", principalFields("none", kWs)}}));
    p.apply(ev(6, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "root.workspace"}, {"object", kWs}}));
    p.apply(ev(7, "created", kModel, kRoot,
               {{"class", "model"}, {"fields", {{"principal/driven-by", "none"}, {"model/kind", "model"}}}}));
    p.apply(ev(8, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "root.model"}, {"object", kModel}}));
    p.apply(ev(9, "created", kAdapter, kRoot,
               {{"class", "adapter"}, {"fields", {{"principal/driven-by", "none"}, {"adapter/binds", "transport"}}}}));
    p.apply(ev(10, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "transport.listener"}, {"object", kAdapter}}));
    p.apply(ev(11, "created", kRoute, kRoot,
               {{"class", "route"}, {"fields", {{"route/transport", kAdapter}, {"route/address", "root"}}}}));
    p.apply(ev(12, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "root.route"}, {"object", kRoute}}));
    p.apply(ev(13, "field-set", kRoot, kRoot,
               {{"name", "principal/workspaces"}, {"type", json::array({"list", json::array({"ref", "principal"})})},
                {"value", json::array({kWs})}, {"reader", "both"}}));
    p.apply(ev(14, "field-set", kRoot, kRoot,
               {{"name", "principal/models"}, {"type", json::array({"list", json::array({"ref", "model"})})},
                {"value", json::array({kModel})}, {"reader", "both"}}));
    p.apply(ev(15, "field-set", kRoot, kRoot,
               {{"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})},
                {"value", json::array({kRoute})}, {"reader", "both"}}));
    p.apply(ev(16, "created", kPerson, kRoot, {{"class", "principal"}, {"fields", principalFields("person", kPerson)}}));
    p.apply(ev(17, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "person"}, {"object", kPerson}}));
    return p;
}

const GraphEdge* find(const GraphModel& g, const char* from, const char* to, EdgeKind kind) {
    for (const GraphEdge& e : g.edges) {
        if (e.from == from && e.to == to && e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

bool overlap(const GraphNode& a, const GraphNode& b, const LayoutMetrics& m) {
    return !(a.x + m.nodeW <= b.x || b.x + m.nodeW <= a.x || a.y + m.nodeH <= b.y || b.y + m.nodeH <= a.y);
}

} // namespace

TEST_CASE("graph: nodes and edges from the kernel-zero fold", "[graph]") {
    Principals p = kernelZero();
    REQUIRE(p.route(kRoute) != nullptr);
    CHECK(p.route(kRoute)->transport == kAdapter);
    GraphModel g = buildGraph(p);
    CHECK(g.nodes.size() == 6);
    REQUIRE(g.node(kWorld) != nullptr);
    CHECK(g.node(kWorld)->isWorld);
    CHECK(g.node(kWorld)->layer == 0);
    CHECK(g.node(kRoot)->layer == 1);
    CHECK(g.node(kRoot)->loop == LoopState::Idle);
    CHECK(g.node(kWs)->layer == 2);     // a workspace, though driven by none
    CHECK(g.node(kModel)->layer == 2);
    CHECK(g.node(kAdapter)->layer == 2);
    CHECK(g.node(kPerson)->layer == 3);
    CHECK(g.node(kRoot)->name == "root");
    // Bindings.
    REQUIRE(find(g, kRoot, kModel, EdgeKind::Model) != nullptr);
    CHECK(find(g, kRoot, kModel, EdgeKind::Model)->label == "root.model");
    REQUIRE(find(g, kRoot, kWs, EdgeKind::Workspace) != nullptr);
    REQUIRE(find(g, kRoot, kAdapter, EdgeKind::Adapter) != nullptr);
    CHECK(find(g, kRoot, kAdapter, EdgeKind::Adapter)->route == kRoute);
    // root.route is held by root alone with no traffic: an inbox stub, no Route edge.
    REQUIRE(find(g, kRoot, kRoot, EdgeKind::Inbox) != nullptr);
    CHECK_FALSE(find(g, kRoot, kRoot, EdgeKind::Inbox)->traffic);
    CHECK(find(g, kRoot, kPerson, EdgeKind::Route) == nullptr);
    CHECK(g.groups.empty());
}

TEST_CASE("graph: a shared route and a delivery make Route edges", "[graph]") {
    Principals p = kernelZero();
    // person now holds root.route too (a route between them), and root delivered to person.
    p.apply(ev(20, "field-set", kPerson, kRoot,
               {{"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})},
                {"value", json::array({kRoute})}, {"reader", "both"}}));
    GraphModel g = buildGraph(p);
    const GraphEdge* shared = find(g, kRoot, kPerson, EdgeKind::Route);
    REQUIRE(shared != nullptr);
    CHECK(shared->label == "root.route");
    CHECK_FALSE(shared->traffic);
    CHECK(find(g, kRoot, kRoot, EdgeKind::Inbox) == nullptr); // the route is shown as an edge now
    p.apply(ev(21, "delivered", kRoot, kRoot, {{"message", kMsg}, {"route", kRoute}, {"recipient", kPerson}}));
    g = buildGraph(p);
    REQUIRE(find(g, kRoot, kPerson, EdgeKind::Route) != nullptr);
    CHECK(find(g, kRoot, kPerson, EdgeKind::Route)->traffic);
    CHECK(g.edges.size() == 5); // model, workspace, root->adapter, person->adapter, route (deduplicated)
    // A delivery on a route nobody shares still draws the traffic edge.
    p.apply(ev(22, "delivered", kPerson, kPerson, {{"message", kMsg}, {"route", "ffffffff-0000-4000-8000-000000000001"}, {"recipient", kRoot}}));
    g = buildGraph(p);
    REQUIRE(find(g, kPerson, kRoot, EdgeKind::Route) != nullptr);
}

TEST_CASE("graph: view/position places a node; the rest are laid out without overlap", "[graph]") {
    Principals p = kernelZero();
    p.apply(ev(30, "field-set", kPerson, kPerson,
               {{"name", "view/position"}, {"type", "map"}, {"value", {{"x", 640.0}, {"y", 90.0}}}, {"reader", "both"}}));
    GraphModel g = buildGraph(p);
    REQUIRE(g.node(kPerson)->placed);
    CHECK(g.node(kPerson)->x == 640.0f);
    CHECK_FALSE(g.node(kRoot)->placed);
    const LayoutMetrics m;
    layoutLayers(g, m);
    CHECK(g.node(kPerson)->x == 640.0f); // untouched
    CHECK(g.node(kPerson)->y == 90.0f);
    CHECK(g.node(kWorld)->y == m.originY);
    CHECK(g.node(kRoot)->y == m.originY + (m.nodeH + m.gapY));
    CHECK(g.node(kWs)->y == m.originY + 2 * (m.nodeH + m.gapY));
    CHECK(g.node(kModel)->x == g.node(kWs)->x + m.nodeW + m.gapX); // creation order within the layer
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        for (std::size_t j = i + 1; j < g.nodes.size(); ++j) {
            if (!g.nodes[i].placed && !g.nodes[j].placed) {
                CHECK_FALSE(overlap(g.nodes[i], g.nodes[j], m));
            }
        }
    }
    // Deterministic: the same fold lays out the same.
    GraphModel again = buildGraph(p);
    layoutLayers(again, m);
    for (const GraphNode& n : g.nodes) {
        CHECK(again.node(n.id)->x == n.x);
        CHECK(again.node(n.id)->y == n.y);
    }
}

TEST_CASE("graph: angular paths bend at 45 degrees when there is room", "[graph]") {
    auto pts = angularPath(0, 0, 200, 40, 58);
    REQUIRE(pts.size() == 4);
    CHECK(pts[1] == P2{80, 0});
    CHECK(pts[2] == P2{120, 40}); // the diagonal spans |dy| horizontally: 45 degrees
    pts = angularPath(0, 0, 50, 300, 58); // no room: orthogonal through the middle
    REQUIRE(pts.size() == 4);
    CHECK(pts[1] == P2{25, 0});
    CHECK(pts[2] == P2{25, 300});
    pts = angularPath(300, 0, 0, 0, 58); // B left of A: a lane below both
    REQUIRE(pts.size() == 6);
    CHECK(pts[2].y == pts[3].y);
    CHECK(pts[2].y > 0);
}

TEST_CASE("evaluations: usage, serialization size, ops and results", "[evaluations]") {
    Evaluations v;
    v.apply(ev(100, "created", kEval, kRoot,
               {{"class", "evaluation"},
                {"fields", {{"evaluation/initiator", kRoot}, {"evaluation/model", kModel}, {"evaluation/state", "queued"}}}}));
    v.apply(ev(101, "created", kSer, kRoot,
               {{"class", "serialization"},
                {"fields", {{"serialization/size", {{"bytes", 8113}}}, {"serialization/hash", "d092057e"}}}},
               kEval));
    v.apply(ev(102, "evaluated", kEval, kRoot, {{"evaluation", kEval}, {"state", "running"}, {"serialization", kSer}}));
    v.apply(ev(103, "evaluated", kEval, kRoot,
               {{"evaluation", kEval},
                {"state", "done"},
                {"model", kModel},
                {"cost", {{"tokens-in", 2653.0}, {"tokens-out", 366.0}}},
                {"outcome", {{"text", "..."}, {"usage", {{"prompt-tokens", 2653}, {"completion-tokens", 366}}}}}}));
    v.apply(ev(104, "field-set", kEval, kRoot,
               {{"name", "evaluation/iteration"}, {"type", "number"}, {"value", 1}, {"reader", "both"}}, kEval));
    v.apply(ev(105, "field-set", kEval, kRoot,
               {{"name", "evaluation/ops"}, {"type", json::array({"list", "map"})},
                {"value", json::array({{{"op", "set-field"}, {"args", {{"field", "loop/motto"}}}}})}, {"reader", "both"}},
               kEval));
    v.apply(ev(106, "field-set", kEval, kRoot,
               {{"name", "evaluation/results"}, {"type", json::array({"list", "map"})},
                {"value", json::array({{{"op", "set-field"}, {"ok", true}, {"seq", 680}}})}, {"reader", "both"}},
               kEval));
    v.apply(ev(107, "field-set", kEval, kRoot,
               {{"name", "evaluation/thinking"}, {"type", "string"}, {"value", "The field is mine to set."}, {"reader", "both"}},
               kEval));
    // A second evaluation of the same wake, usage only through outcome.usage.
    v.apply(ev(110, "created", kEval2, kRoot, {{"class", "evaluation"}, {"fields", {{"evaluation/initiator", kRoot}}}}));
    v.apply(ev(111, "evaluated", kEval2, kRoot,
               {{"evaluation", kEval2}, {"state", "done"},
                {"outcome", {{"usage", {{"prompt-tokens", 3421}, {"completion-tokens", 2246}}}}}}));
    const EvaluationInfo* e1 = v.get(kEval);
    REQUIRE(e1 != nullptr);
    CHECK(e1->principal == kRoot);
    CHECK(e1->model == kModel);
    CHECK(e1->state == "done");
    CHECK(e1->serialization == kSer);
    CHECK(e1->tokensIn == 2653);
    CHECK(e1->tokensOut == 366);
    CHECK(e1->iteration == 1);
    REQUIRE(e1->ops.size() == 1);
    CHECK(e1->ops[0]["op"] == "set-field");
    REQUIRE(e1->results.size() == 1);
    CHECK(e1->results[0]["seq"] == 680);
    CHECK(e1->thinking == "The field is mine to set.");
    REQUIRE(v.serialization(kSer) != nullptr);
    CHECK(v.serialization(kSer)->bytes == 8113);
    CHECK(v.get(kEval2)->tokensIn == 3421);
    // The wake that produced a reply at seq 120 after a request at seq 99.
    const auto wake = v.between(kRoot, 99, 120);
    REQUIRE(wake.size() == 2);
    CHECK(wake[0]->id == kEval);
    CHECK(wake[1]->id == kEval2);
    CHECK(v.between(kPerson, 0, 999).empty());
    CHECK(costLine(wake, v) == "6074 in / 2612 out · 2 evaluations"); // eval2 has no serialization
    CHECK(costLine({e1}, v) == "2653 in / 366 out · 7.9 KiB serialized · 1 evaluation");
    Totals t = v.totals(kRoot);
    CHECK(t.evaluations == 2);
    CHECK(t.tokensIn == 6074);
    CHECK(totalsLine(t) == "2 evaluations · 6074 in / 2612 out");
    // evaluation/context, when the world records it, drives the line and the running totals.
    v.apply(ev(120, "field-set", kEval2, kRoot,
               {{"name", "evaluation/context"}, {"type", "map"},
                {"value", {{"serialization-bytes", 12698}, {"prompt-tokens", 3421}, {"completion-tokens", 2246}, {"objects", 14}}},
                {"reader", "both"}},
               kEval2));
    CHECK(v.get(kEval2)->ctxBytes == 12698);
    CHECK(v.get(kEval2)->ctxObjects == 14);
    CHECK(costLine({v.get(kEval2)}, v) == "ctx 12.4 KiB · 3421 in / 2246 out · 14 objects");
    CHECK(costLine(v.between(kRoot, 99, 130), v) == "ctx 12.4 KiB · 6074 in / 2612 out · 14 objects · 2 evaluations");
    t = v.totals(kRoot);
    CHECK(t.withContext == 1);
    CHECK(totalsLine(t) == "2 evaluations · 6074 in / 2612 out · ctx 12.4 KiB · 14 objects");
    CHECK(totalsShort(t) == "2 ev · 6,074/2,612");
    CHECK(kTokens(987) == "987");
    CHECK(kTokens(1200000) == "1.2M");
    REQUIRE(v.latest(kRoot) != nullptr);
    CHECK(v.latest(kRoot)->id == kEval2);
    CHECK(statusLine(v, kRoot) == "CTX last 12.4 KiB · 3421 in / 2246 out  ·  TOTAL 2 evals · 6.1K in / 2.6K out");
    CHECK(statusLine(v, kPerson).empty());
    // Progress while a loop runs: elapsed from the running event, the previous call's size.
    const char* kEval3 = "3a3a3a3a-0000-4000-8000-000000000003";
    v.apply(ev(130, "created", kEval3, kRoot, {{"class", "evaluation"}, {"fields", {{"evaluation/initiator", kRoot}}}}));
    Event run = ev(131, "evaluated", kEval3, kRoot, {{"evaluation", kEval3}, {"state", "running"}});
    run.at = "2026-10-30T15:18:00.500Z";
    v.apply(run);
    CHECK(epochSeconds("2026-10-30T15:18:00.500Z") == epochSeconds("2026-10-30T15:17:00.500Z") + 60.0);
    CHECK(epochSeconds("garbage") < 0);
    const double now = epochSeconds("2026-10-30T15:18:47.900Z");
    CHECK(progressLine(v, kRoot, "root", 2, now) == "root thinking · turn 2/8 · set-field · 47 s · last call 3.4K in"); // running, nothing received yet
    // Streaming: deltas accumulate in order, duplicates and reorders drop, nothing after done.
    CHECK(v.streaming(kRoot) == nullptr);
    v.apply(ev(132, "evaluated", kEval3, kRoot, {{"evaluation", kEval3}, {"state", "streaming"}, {"delta", "The loop "}, {"seq-in-stream", 1}}));
    v.apply(ev(133, "evaluated", kEval3, kRoot, {{"evaluation", kEval3}, {"state", "streaming"}, {"delta", "lives in "}, {"seq-in-stream", 2}}));
    v.apply(ev(134, "evaluated", kEval3, kRoot, {{"evaluation", kEval3}, {"state", "streaming"}, {"delta", "lives in "}, {"seq-in-stream", 2}})); // duplicate
    v.apply(ev(135, "evaluated", kEval3, kRoot, {{"evaluation", kEval3}, {"state", "streaming"}, {"delta", "core.clj."}, {"seq-in-stream", 3}}));
    REQUIRE(v.streaming(kRoot) != nullptr);
    CHECK(v.streaming(kRoot)->partial == "The loop lives in core.clj.");
    CHECK(v.get(kEval3)->state == "streaming");
    v.apply(ev(136, "evaluated", kEval3, kRoot, {{"evaluation", kEval3}, {"state", "done"}, {"outcome", {{"text", "The loop lives in core.clj."}}}}));
    CHECK(v.streaming(kRoot) == nullptr); // done: the Message carries the text from here on
    v.apply(ev(137, "evaluated", kEval3, kRoot, {{"evaluation", kEval3}, {"state", "streaming"}, {"delta", "late"}, {"seq-in-stream", 4}}));
    CHECK(v.get(kEval3)->partial == "The loop lives in core.clj."); // a late delta changes nothing
    CHECK(v.get(kEval3)->state == "done");
    CHECK(humanBytes(512) == "512 B");
    CHECK(humanBytes(-1).empty());
}

TEST_CASE("graph: a Model node carries its model id and status", "[graph]") {
    Principals p = kernelZero();
    p.apply(ev(18, "field-set", kModel, kRoot,
               {{"name", "model/id"}, {"type", "string"}, {"value", "your-model"}, {"reader", "both"}}));
    GraphModel g = buildGraph(p);
    REQUIRE(g.node(kModel) != nullptr);
    CHECK(g.node(kModel)->sub == "your-model");
    CHECK(g.node(kModel)->status.empty()); // unknown until the world probes it
    p.apply(ev(19, "field-set", kModel, kRoot,
               {{"name", "model/status"}, {"type", "string"}, {"value", "unreachable"}, {"reader", "both"}}));
    g = buildGraph(p);
    CHECK(g.node(kModel)->status == "unreachable");
    CHECK(g.node(kRoot)->sub.empty()); // only models carry one
}

TEST_CASE("context gauge marks a recorded first wake call, not a token drop", "[evaluations]") {
    EvaluationInfo e;
    e.iteration = 1;
    CHECK_FALSE(newWakeContext(e));
    e.wake = "wake-a";
    CHECK(newWakeContext(e));
    e.iteration = 2;
    e.ctxIn = 100;
    CHECK_FALSE(newWakeContext(e));
    e.wake = "wake-b";
    e.iteration = 1;
    CHECK(newWakeContext(e));
}
