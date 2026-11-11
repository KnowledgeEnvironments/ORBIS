// What the panes show, tested on the pure folds under them (no SDL, no ImGui): the streaming
// assembly behind the terminal's provisional block, the progress/thinking line, Route edges from
// an explicit `route/members` Field, and the one naming rule. Events in the JSON form the world
// pushes.
#include "app/FrameFit.hpp"
#include "chat/Chat.hpp"
#include "chat/FieldValue.hpp"
#include "model/Evaluations.hpp"
#include "model/Graph.hpp"
#include "model/PaneLayout.hpp"
#include "model/Principals.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

constexpr const char* kWorld = "d470bce8-5578-400f-8f31-3504f178c6a8";
constexpr const char* kRoot = "034968ad-3358-4e57-a31b-d1bde0c15b31";
constexpr const char* kPerson = "28b70d40-2916-4adb-967a-2dea363599dc";
constexpr const char* kRoute = "bfb9c6ed-8354-420d-a950-a9b9681af4bc";
constexpr const char* kModel = "b7785bd5-9284-4427-b0b0-54f95bb6f960";
constexpr const char* kEval0 = "11111111-0000-4000-8000-000000000001";
constexpr const char* kEval = "03d1ac77-b9e5-498b-b28e-da884b8ddfea";

Event ev(Seq seq, const char* kind, const char* stream, const char* actor, json body) {
    Event e;
    e.seq = seq;
    e.id = "0b5a2c3e-2222-4c2a-8f1e-" + std::to_string(100000000000 + seq);
    e.stream = stream;
    e.actor = actor;
    e.kind = kind;
    e.at = "2026-10-30T15:18:" + std::string(seq % 60 < 10 ? "0" : "") + std::to_string(seq % 60) + ".000Z";
    e.body = std::move(body);
    return e;
}

json principalFields(const char* driven, const char* ns) {
    return {{"principal/driven-by", driven}, {"principal/namespace", ns}};
}

Event created(Seq seq, const char* evaluation) {
    return ev(seq, "created", evaluation, kRoot,
              {{"class", "evaluation"},
               {"fields", {{"evaluation/initiator", kRoot}, {"evaluation/model", kModel}, {"evaluation/state", "queued"}}}});
}

Event evaluated(Seq seq, const char* evaluation, json body) {
    body["evaluation"] = evaluation;
    body["model"] = kModel;
    return ev(seq, "evaluated", evaluation, kRoot, std::move(body));
}

// A finished earlier call with usage, so the progress line has a "last call" to show.
void previousCall(Evaluations& e) {
    e.apply(created(10, kEval0));
    e.apply(evaluated(11, kEval0, {{"state", "running"}}));
    e.apply(evaluated(12, kEval0,
                      {{"state", "done"}, {"outcome", {{"usage", {{"prompt-tokens", 5800}, {"completion-tokens", 300}}}}}}));
}

} // namespace

TEST_CASE("streaming: deltas accumulate once, thinking counts only grow, done ends the provisional block",
          "[panes][streaming]") {
    Evaluations e;
    previousCall(e);
    e.apply(created(20, kEval));
    REQUIRE(e.streaming(kRoot) == nullptr); // nothing received: no provisional block
    e.apply(evaluated(21, kEval, {{"state", "running"}}));
    REQUIRE(e.streaming(kRoot) == nullptr);

    SECTION("reasoning first, then content; duplicates, reorders and unknown keys are ignored") {
        REQUIRE(e.apply(evaluated(22, kEval, {{"state", "streaming"}, {"thinking-delta", "Let me "}, {"thinking-chars", 7}, {"seq-in-stream", 1}})));
        const EvaluationInfo* sv = e.streaming(kRoot);
        REQUIRE(sv != nullptr);
        CHECK(sv->thinkingPartial == "Let me ");
        CHECK(sv->thinkingChars == 7);
        CHECK_FALSE(sv->replying());
        // a count without text (the world may send only the counter)
        REQUIRE(e.apply(evaluated(23, kEval, {{"state", "streaming"}, {"thinking-chars", 1200}, {"seq-in-stream", 2}, {"future-key", true}})));
        CHECK(e.get(kEval)->thinkingChars == 1200);
        CHECK(e.get(kEval)->thinkingPartial == "Let me ");
        // a stale count never lowers it
        REQUIRE(e.apply(evaluated(24, kEval, {{"state", "streaming"}, {"thinking-chars", 900}, {"seq-in-stream", 3}})));
        CHECK(e.get(kEval)->thinkingChars == 1200);
        // content deltas
        REQUIRE(e.apply(evaluated(25, kEval, {{"state", "streaming"}, {"delta", "Smoke "}, {"seq-in-stream", 4}})));
        REQUIRE(e.apply(evaluated(26, kEval, {{"state", "streaming"}, {"delta", "acknowledged."}, {"seq-in-stream", 5}})));
        CHECK_FALSE(e.apply(evaluated(27, kEval, {{"state", "streaming"}, {"delta", "acknowledged."}, {"seq-in-stream", 5}}))); // duplicate
        CHECK_FALSE(e.apply(evaluated(28, kEval, {{"state", "streaming"}, {"delta", "Smoke "}, {"seq-in-stream", 4}})));       // reorder
        sv = e.streaming(kRoot);
        REQUIRE(sv != nullptr);
        CHECK(sv->partial == "Smoke acknowledged.");
        CHECK(sv->replying());
        CHECK(sv->streamSeq == 5);
        // the final event: the provisional block goes away, the text is the done text
        REQUIRE(e.apply(evaluated(29, kEval,
                                  {{"state", "done"},
                                   {"outcome", {{"text", "Smoke acknowledged."}, {"usage", {{"prompt-tokens", 2287}, {"completion-tokens", 1016}}}}}})));
        CHECK(e.streaming(kRoot) == nullptr);
        CHECK(e.get(kEval)->done());
        CHECK(e.get(kEval)->tokensIn == 2287);
        // a late delta after done changes nothing
        CHECK_FALSE(e.apply(evaluated(30, kEval, {{"state", "streaming"}, {"delta", "late"}, {"seq-in-stream", 6}})));
        CHECK(e.get(kEval)->partial == "Smoke acknowledged.");
        CHECK(e.get(kEval)->state == "done");
    }

    SECTION("a full partial replaces the text and never goes backwards on seq") {
        REQUIRE(e.apply(evaluated(22, kEval, {{"state", "streaming"}, {"delta", "ab"}, {"seq-in-stream", 1}})));
        REQUIRE(e.apply(evaluated(23, kEval, {{"state", "streaming"}, {"partial", "abcd"}, {"seq-in-stream", 3}})));
        CHECK(e.get(kEval)->partial == "abcd");
        CHECK_FALSE(e.apply(evaluated(24, kEval, {{"state", "streaming"}, {"delta", "c"}, {"seq-in-stream", 2}})));
        CHECK(e.get(kEval)->partial == "abcd");
    }
}

TEST_CASE("progress line: evaluating, thinking, thinking with chars, replying", "[panes][progress]") {
    Evaluations e;
    previousCall(e);
    const double start = epochSeconds("2026-10-30T15:18:21.000Z");
    REQUIRE(start > 0);

    // the loop woke but no Evaluation is running yet
    CHECK(progressLine(e, kRoot, "root", 1, start) == "root working · turn 1/8 · last call 5.8K in");

    e.apply(created(20, kEval));
    e.apply(evaluated(21, kEval, {{"state", "running"}})); // at 15:18:21
    CHECK(progressLine(e, kRoot, "root", 1, start + 31, 8, 0, "gpt-test") ==
          "root thinking · gpt-test · turn 1/8 · 31 s · last call 5.8K in");
    CHECK(progressLine(e, kRoot, "root", 0, start + 31) == "root thinking · 31 s · last call 5.8K in");
    CHECK(progressLine(e, kRoot, "root", 1, start + 31, 40, 2) ==
          "root thinking · turn 1/40 · 31 s · last call 5.8K in · 2 messages queued");

    e.apply(evaluated(22, kEval, {{"state", "streaming"}, {"thinking-chars", 1234}, {"seq-in-stream", 1}}));
    CHECK(progressLine(e, kRoot, "root", 0, start + 31) == "root thinking · 1.2K chars · 31 s");

    std::string text(420, 'x');
    e.apply(evaluated(23, kEval, {{"state", "streaming"}, {"delta", text}, {"seq-in-stream", 2}}));
    CHECK(progressLine(e, kRoot, "root", 2, start + 34) == "root replying · turn 2/8 · 420 chars · 34 s");

    e.apply(evaluated(24, kEval, {{"state", "done"}, {"outcome", {{"usage", {{"prompt-tokens", 6000}, {"completion-tokens", 500}}}}}}));
    // after done the loop may still be between iterations: back to evaluating, with the new last call
    CHECK(progressLine(e, kRoot, "root", 2, start + 40) == "root working · turn 2/8 · last call 6.0K in");
}

TEST_CASE("graph: an explicit route/members Field names the ends; without it holders are inferred", "[panes][graph]") {
    Principals p;
    p.apply(ev(1, "created", kWorld, kWorld, {{"class", "principal"}, {"fields", principalFields("none", kWorld)}}));
    p.apply(ev(2, "created", kRoot, kWorld, {{"class", "principal"}, {"fields", principalFields("process", kRoot)}}));
    p.apply(ev(3, "bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", "root"}, {"object", kRoot}}));
    p.apply(ev(4, "created", kPerson, kRoot, {{"class", "principal"}, {"fields", principalFields("person", kPerson)}}));
    p.apply(ev(5, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "person"}, {"object", kPerson}}));

    SECTION("held by root alone, no members: an inbox stub on root") {
        p.apply(ev(6, "created", kRoute, kRoot, {{"class", "route"}, {"fields", {{"route/address", "root"}}}}));
        p.apply(ev(7, "field-set", kRoot, kRoot,
                   {{"object", kRoot}, {"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})}, {"value", json::array({kRoute})}, {"reader", "both"}}));
        const GraphModel g = buildGraph(p);
        int routeEdges = 0, inbox = 0;
        for (const GraphEdge& e : g.edges) {
            routeEdges += e.kind == EdgeKind::Route;
            inbox += e.kind == EdgeKind::Inbox;
        }
        CHECK(routeEdges == 0);
        CHECK(inbox == 1);
    }

    SECTION("route/members at creation: one Route edge between the members, no stub") {
        p.apply(ev(6, "created", kRoute, kRoot,
                   {{"class", "route"}, {"fields", {{"route/address", "root"}, {"route/members", json::array({kRoot, kPerson})}}}}));
        p.apply(ev(7, "field-set", kRoot, kRoot,
                   {{"object", kRoot}, {"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})}, {"value", json::array({kRoute})}, {"reader", "both"}}));
        REQUIRE(p.route(kRoute) != nullptr);
        CHECK(p.route(kRoute)->members == std::vector<std::string>{kRoot, kPerson});
        const GraphModel g = buildGraph(p);
        std::vector<GraphEdge> routes;
        for (const GraphEdge& e : g.edges) {
            if (e.kind == EdgeKind::Route || e.kind == EdgeKind::Inbox) {
                routes.push_back(e);
            }
        }
        REQUIRE(routes.size() == 1);
        CHECK(routes[0].kind == EdgeKind::Route);
        CHECK(routes[0].from == kRoot);
        CHECK(routes[0].to == kPerson);
        CHECK(routes[0].route == kRoute);
    }

    SECTION("route/members set later replaces the inferred ends; an unknown member is dropped") {
        p.apply(ev(6, "created", kRoute, kRoot, {{"class", "route"}, {"fields", {{"route/address", "root"}}}}));
        p.apply(ev(7, "field-set", kRoot, kRoot,
                   {{"object", kRoot}, {"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})}, {"value", json::array({kRoute})}, {"reader", "both"}}));
        p.apply(ev(8, "field-set", kRoute, kRoot,
                   {{"object", kRoute}, {"name", "route/members"}, {"type", json::array({"list", json::array({"ref", "principal"})})},
                    {"value", json::array({kPerson, kRoot, "ffffffff-0000-4000-8000-000000000000"})}, {"reader", "both"}}));
        const GraphModel g = buildGraph(p);
        std::vector<GraphEdge> routes;
        for (const GraphEdge& e : g.edges) {
            if (e.kind == EdgeKind::Route || e.kind == EdgeKind::Inbox) {
                routes.push_back(e);
            }
        }
        REQUIRE(routes.size() == 1);
        CHECK(routes[0].from == kPerson);
        CHECK(routes[0].to == kRoot);
    }
}

TEST_CASE("nameOf: the one naming rule, also for routes and unknown ids", "[panes][names]") {
    Principals p;
    p.apply(ev(1, "created", kWorld, kWorld, {{"class", "principal"}, {"fields", principalFields("none", kWorld)}}));
    p.apply(ev(2, "created", kRoot, kWorld, {{"class", "principal"}, {"fields", principalFields("process", kRoot)}}));
    p.apply(ev(3, "bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", "root"}, {"object", kRoot}}));
    p.apply(ev(4, "created", kRoute, kRoot, {{"class", "route"}, {"fields", {{"route/address", "root"}, {"name", "root.route"}}}}));
    CHECK(p.nameOf(kRoot) == "root");
    CHECK(p.nameOf(kRoute) == "root.route");                                         // an object's own name Field
    CHECK(p.nameOf("ffffffff-0000-4000-8000-000000000000") == "ffffffff");          // nothing known: the short id
    p.apply(ev(5, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "root.inbox"}, {"object", kRoute}}));
    CHECK(p.nameOf(kRoute) == "root.inbox"); // a binding beats the Field
}

TEST_CASE("a cancelled evaluation is done, keeps its partial text, and is what a cancel targets", "[panes][cancel]") {
    Evaluations e;
    e.apply(created(30, kEval));
    e.apply(evaluated(31, kEval, {{"state", "running"}}));
    REQUIRE(e.running(kRoot) != nullptr);
    CHECK(e.running(kRoot)->id == kEval); // Esc Esc / STOP sends `act cancel` on this id
    REQUIRE(e.apply(evaluated(32, kEval, {{"state", "streaming"}, {"delta", "Half an ans"}, {"seq-in-stream", 1}})));
    REQUIRE(e.apply(evaluated(33, kEval, {{"state", "cancelled"}})));
    const EvaluationInfo* v = e.get(kEval);
    REQUIRE(v != nullptr);
    CHECK(v->cancelled());
    CHECK(v->done());
    CHECK(v->partial == "Half an ans");         // what had arrived stays for the dim `cancelled` block
    CHECK(e.running(kRoot) == nullptr);          // nothing to cancel any more
    CHECK(e.streaming(kRoot) == nullptr);        // no provisional block after the end
    CHECK_FALSE(e.apply(evaluated(34, kEval, {{"state", "streaming"}, {"delta", "late"}, {"seq-in-stream", 2}}))); // a late delta is ignored
    CHECK(v->partial == "Half an ans");
    const auto orphans = e.cancelledAfter(kRoot, 0);
    REQUIRE(orphans.size() == 1);
    CHECK(orphans[0]->id == kEval);
    CHECK(e.cancelledAfter(kRoot, 30).empty()); // created at 30: not after 30
    // The progress line stops calling it live.
    const std::string line = progressLine(e, kRoot, "root", 0, epochSeconds("2026-10-30T15:18:40.000Z"));
    CHECK(line.rfind("root working", 0) == 0);
}

TEST_CASE("totals are cached per fold seq and refreshed when it moves", "[panes][totals]") {
    Evaluations e;
    e.apply(created(40, kEval0));
    e.apply(evaluated(41, kEval0, {{"state", "done"}, {"outcome", {{"usage", {{"prompt-tokens", 100}, {"completion-tokens", 10}}}}}}));
    const Totals a = e.totals(kRoot);
    const Totals b = e.totals(kRoot); // same seq: the cached answer
    CHECK(a.tokensIn == 100);
    CHECK(b.tokensIn == a.tokensIn);
    CHECK(b.evaluations == 1);
    e.apply(created(42, kEval));
    e.apply(evaluated(43, kEval, {{"state", "done"}, {"outcome", {{"usage", {{"prompt-tokens", 50}, {"completion-tokens", 5}}}}}}));
    const Totals c = e.totals(kRoot);
    CHECK(c.evaluations == 2);
    CHECK(c.tokensIn == 150);
}

TEST_CASE("a frame fits the swapchain across fractional-density rounding, never across a resize") {
    using ke::fitFrame;
    // the iPhone Duo's inner screen: 2.81 pixels a point, swapchain 2007 x 2853
    const float density = 2007.0f / 714.0f;
    const auto inner = fitFrame(2007, 2853, 714.0f, 1015.0f, density, density);
    CHECK(inner.draw);
    CHECK(static_cast<std::uint32_t>(714.0f * inner.scaleX) <= 2007);
    CHECK(static_cast<std::uint32_t>(1015.0f * inner.scaleY) <= 2853);
    // exact sizes pass unchanged
    const auto exact = fitFrame(1440, 900, 1440.0f, 900.0f, 1.0f, 1.0f);
    CHECK(exact.draw);
    CHECK(exact.scaleX == 1.0f);
    // the viewport is brought inside when rounding lands above the target
    const auto over = fitFrame(1000, 1000, 500.5f, 500.5f, 2.0f, 2.0f);
    CHECK(over.draw);
    CHECK(static_cast<std::uint32_t>(500.5f * over.scaleX) <= 1000);
    // fold or rotate in flight: skip the frame
    CHECK_FALSE(fitFrame(1398, 2034, 714.0f, 1015.0f, density, density).draw);
    CHECK_FALSE(fitFrame(0, 0, 714.0f, 1015.0f, density, density).draw);
    CHECK_FALSE(fitFrame(2007, 2853, 0.0f, 1015.0f, density, density).draw);
}

TEST_CASE("the cost line says whether this reply reasoned") {
    Evaluations e;
    REQUIRE(e.apply(created(10, kEval)));
    Event f;
    f.seq = 11;
    f.kind = "field-set";
    f.stream = kEval;
    f.actor = kRoot;
    f.id = "e-11";
    f.body = json{{"name", "evaluation/context"},
                  {"type", "map"},
                  {"reader", "both"},
                  {"value", {{"serialization-bytes", 2048}, {"prompt-tokens", 10}, {"completion-tokens", 5}, {"objects", 3}, {"reasoning", "off"}}}};
    REQUIRE(e.apply(f));
    const EvaluationInfo* v = e.get(kEval);
    REQUIRE(v != nullptr);
    CHECK(v->reasoning == "off");
    CHECK(costLine({v}, e).find("reasoning off") != std::string::npos);
}

TEST_CASE("the reasoning row reads what the model will do") {
    using ke::chat::reasoningShown;
    CHECK(reasoningShown("on", "off") == "on");
    CHECK(reasoningShown("off", "on") == "off");
    CHECK(reasoningShown("", "on") == "on");
    CHECK(reasoningShown("", "auto") == "off");
    CHECK(reasoningShown("", "") == "off");
}

TEST_CASE("a byte-range read pages until the Field is done") {
    using ke::chat::nextBytePage;
    using nlohmann::json;
    const auto page = [](long from, long to, long size) {
        return json{{"object/fields", {{"x/y-byte-range", {{"field/value", json::array({from, to})}}},
                                       {"x/y-size", {{"field/value", size}}}}}};
    };
    CHECK(nextBytePage(page(0, 65536, 100000), "x/y") == 65536);
    CHECK_FALSE(nextBytePage(page(65536, 100000, 100000), "x/y").has_value());
    CHECK_FALSE(nextBytePage(json::object(), "x/y").has_value());
}

TEST_CASE("a layout holds floats, and a whole-window edge", "[panes]") {
    ke::model::PaneLayout l;
    l.add("view:chat");
    l.place("view:history", "view:chat", ke::model::Edge::Bottom);
    l.floatOut("view:history", ke::model::LayoutRect{10, 20, 300, 200});
    CHECK(l.floating("view:history"));
    CHECK_FALSE(l.contains("view:history"));

    const ke::model::PaneLayout back = ke::model::PaneLayout::fromJson(l.toJson());
    REQUIRE(back.floats().size() == 1);
    CHECK(back.floats()[0].rect.w == 300);

    l.dock("view:history", "view:chat", ke::model::Edge::Bottom);
    CHECK_FALSE(l.floating("view:history"));
    CHECK(l.contains("view:history"));
    const auto side = l.sideOf("view:history");
    REQUIRE(side.has_value());
    CHECK(side->second);
    CHECK_FALSE(side->horizontal);
    CHECK(side->alone);

    l.wrap("view:principals", ke::model::Edge::Left, 0.3f);
    const auto leaves = l.leaves(ke::model::LayoutRect{0, 0, 1000, 500});
    REQUIRE(leaves.size() == 3);
    CHECK(leaves[0].tabs == std::vector<std::string>{"view:principals"});
    CHECK(leaves[0].rect.w > 299.0f);
    CHECK(leaves[0].rect.w < 301.0f);
}

