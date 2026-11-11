// Snapshot boot (sprint 1 L7): seeding the folds from the per-object shape `read` returns must
// leave them as the equivalent events would. The world's `history {snapshot true}` is a derived
// view of the log, never a second source of truth, so the two paths have to agree.
#include "chat/MessageCache.hpp"
#include "model/Evaluations.hpp"
#include "model/Graph.hpp"
#include "model/Principals.hpp"
#include "model/Snapshot.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

constexpr const char* kWorld = "d470bce8-5578-400f-8f31-3504f178c6a8";
constexpr const char* kRoot = "034968ad-3358-4e57-a31b-d1bde0c15b31";
constexpr const char* kPerson = "28b70d40-2916-4adb-967a-2dea363599dc";
constexpr const char* kModel = "b7785bd5-9284-4427-b0b0-54f95bb6f960";
constexpr const char* kRoute = "bfb9c6ed-8354-420d-a950-a9b9681af4bc";
constexpr const char* kMsg = "d67530c2-5f89-4c9a-a319-1d2fb171e740";
constexpr const char* kEval = "03d1ac77-b9e5-498b-b28e-da884b8ddfea";
constexpr const char* kSer = "cac07e7d-6eb2-497a-9163-eb222ad1a32e";

std::string at(Seq seq) {
    return "2026-10-30T15:" + std::string(seq < 10 ? "0" : "") + std::to_string(seq % 60) + ":00.000Z";
}

Event ev(Seq seq, const char* kind, const char* stream, const char* actor, json body, const char* via = nullptr) {
    Event e;
    e.seq = seq;
    e.id = "0b5a2c3e-1111-4c2a-8f1e-" + std::to_string(100000000000 + seq);
    e.stream = stream;
    e.actor = actor;
    e.kind = kind;
    e.at = at(seq);
    e.body = std::move(body);
    if (via) {
        e.via = via;
    }
    return e;
}

// The events of a small world: created, bound, field-set, a Route, a Message, an Evaluation.
std::vector<Event> events() {
    std::vector<Event> v;
    v.push_back(ev(1, "created", kWorld, kWorld,
                   {{"class", "principal"}, {"fields", {{"principal/driven-by", "none"}, {"principal/namespace", kWorld}}}}));
    v.push_back(ev(2, "created", kRoot, kWorld,
                   {{"class", "principal"}, {"fields", {{"principal/driven-by", "process"}, {"principal/namespace", kRoot}}}}));
    v.push_back(ev(3, "bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", "world"}, {"object", kWorld}}));
    v.push_back(ev(4, "bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", "root"}, {"object", kRoot}}));
    v.push_back(ev(5, "created", kModel, kRoot, {{"class", "model"}, {"fields", {{"model/kind", "model"}}}}));
    v.push_back(ev(6, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "root.model"}, {"object", kModel}}));
    v.push_back(ev(7, "created", kRoute, kRoot,
                   {{"class", "route"}, {"fields", {{"route/address", "root"}, {"route/members", json::array({kRoot})}}}}));
    v.push_back(ev(8, "field-set", kRoot, kRoot,
                   {{"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})},
                    {"value", json::array({kRoute})}, {"reader", "both"}, {"by", kRoot}}));
    v.push_back(ev(9, "field-set", kRoot, kWorld,
                   {{"name", "instructions/charter"}, {"type", "string"}, {"value", "# Charter"}, {"reader", "both"}}));
    v.push_back(ev(10, "field-set", kRoot, kRoot,
                   {{"name", "principal/models"}, {"type", json::array({"list", json::array({"ref", "model"})})},
                    {"value", json::array({kModel})}, {"reader", "both"}}));
    v.push_back(ev(11, "created", kPerson, kRoot,
                   {{"class", "principal"}, {"fields", {{"principal/driven-by", "person"}, {"principal/namespace", kPerson}}}}));
    v.push_back(ev(12, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "person"}, {"object", kPerson}}));
    v.push_back(ev(13, "created", kMsg, kPerson,
                   {{"class", "message"},
                    {"fields",
                     {{"message/from", kPerson}, {"message/to", json::array({kRoot})}, {"message/route", kRoute},
                      {"message/text", "what do you know?"}}}}));
    v.push_back(ev(14, "sent", kPerson, kPerson, {{"message", kMsg}, {"route", kRoute}}));
    v.push_back(ev(15, "delivered", kRoot, kPerson, {{"message", kMsg}, {"route", kRoute}, {"recipient", kRoot}}));
    v.push_back(ev(16, "created", kSer, kRoot,
                   {{"class", "serialization"}, {"fields", {{"serialization/size", {{"bytes", 8113}}}, {"serialization/hash", "d092"}}}}));
    v.push_back(ev(17, "created", kEval, kRoot,
                   {{"class", "evaluation"},
                    {"fields", {{"evaluation/initiator", kRoot}, {"evaluation/model", kModel}, {"evaluation/state", "queued"}, {"evaluation/message", kMsg}}}}));
    v.push_back(ev(18, "evaluated", kEval, kRoot, {{"evaluation", kEval}, {"state", "running"}, {"model", kModel}, {"serialization", kSer}}));
    v.push_back(ev(19, "evaluated", kEval, kRoot,
                   {{"evaluation", kEval},
                    {"state", "done"},
                    {"model", kModel},
                    {"outcome", {{"usage", {{"prompt-tokens", 2287}, {"completion-tokens", 1016}}}}}}));
    v.push_back(ev(20, "field-set", kEval, kRoot,
                   {{"name", "evaluation/iteration"}, {"type", "number"}, {"value", 1}, {"reader", "both"}}, kEval));
    v.push_back(ev(21, "field-set", kEval, kRoot,
                   {{"name", "evaluation/context"},
                    {"type", "map"},
                    {"value", {{"serialization-bytes", 8113}, {"prompt-tokens", 2287}, {"completion-tokens", 1016}, {"objects", 5}}},
                    {"reader", "both"}},
                   kEval));
    return v;
}

json fieldJson(const json& value, const json& type, const char* by, Seq seq) {
    return json{{"field/value", value}, {"field/type", type},     {"field/reader", "both"},
                {"field/author", by},   {"field/set-seq", seq},   {"field/set-at", at(seq)}};
}

// The same world as a snapshot taken at seq 21, in the shape `read` returns for each object.
json snapshot() {
    json objects = json::array();
    objects.push_back({{"object/id", kWorld},
                       {"object/class", "principal"},
                       {"object/stream", kWorld},
                       {"object/names", json::array({json::array({kWorld, "world"})})},
                       {"object/created-seq", 1},
                       {"object/created-at", at(1)},
                       {"object/fields",
                        {{"principal/driven-by", fieldJson("none", "keyword", kWorld, 1)},
                         {"principal/namespace", fieldJson(kWorld, json::array({"ref", "principal"}), kWorld, 1)}}}});
    objects.push_back({{"object/id", kRoot},
                       {"object/class", "principal"},
                       {"object/stream", kRoot},
                       {"object/names", json::array({json::array({kWorld, "root"})})},
                       {"object/created-seq", 2},
                       {"object/created-at", at(2)},
                       {"principal/driven-by", "process"},
                       {"object/fields",
                        {{"principal/driven-by", fieldJson("process", "keyword", kWorld, 2)},
                         {"principal/namespace", fieldJson(kRoot, json::array({"ref", "principal"}), kWorld, 2)},
                         {"principal/routes",
                          fieldJson(json::array({kRoute}), json::array({"list", json::array({"ref", "route"})}), kRoot, 8)},
                         {"instructions/charter", fieldJson("# Charter", "string", kWorld, 9)},
                         {"principal/models",
                          fieldJson(json::array({kModel}), json::array({"list", json::array({"ref", "model"})}), kRoot, 10)}}}});
    objects.push_back({{"object/id", kModel},
                       {"object/class", "model"},
                       {"object/names", json::array({json::array({kRoot, "root.model"})})},
                       {"object/created-seq", 5},
                       {"object/created-at", at(5)},
                       {"object/fields", {{"model/kind", fieldJson("model", "keyword", kRoot, 5)}}}});
    objects.push_back({{"object/id", kRoute},
                       {"object/class", "route"},
                       {"object/created-seq", 7},
                       {"object/created-at", at(7)},
                       {"object/fields",
                        {{"route/address", fieldJson("root", "string", kRoot, 7)},
                         {"route/members", fieldJson(json::array({kRoot}), json::array({"list", json::array({"ref", "principal"})}), kRoot, 7)}}}});
    objects.push_back({{"object/id", kPerson},
                       {"object/class", "principal"},
                       {"object/names", json::array({json::array({kRoot, "person"})})},
                       {"object/created-seq", 11},
                       {"object/created-at", at(11)},
                       {"object/fields",
                        {{"principal/driven-by", fieldJson("person", "keyword", kRoot, 11)},
                         {"principal/namespace", fieldJson(kPerson, json::array({"ref", "principal"}), kRoot, 11)}}}});
    objects.push_back({{"object/id", kMsg},
                       {"object/class", "message"},
                       {"object/created-seq", 13},
                       {"object/created-at", at(13)},
                       {"object/fields",
                        {{"message/from", fieldJson(kPerson, json::array({"ref", "principal"}), kPerson, 13)},
                         {"message/to", fieldJson(json::array({kRoot}), json::array({"set", json::array({"ref", "principal"})}), kPerson, 13)},
                         {"message/route", fieldJson(kRoute, json::array({"ref", "route"}), kPerson, 13)},
                         {"message/text", fieldJson("what do you know?", "string", kPerson, 13)}}}});
    objects.push_back({{"object/id", kSer},
                       {"object/class", "serialization"},
                       {"object/created-seq", 16},
                       {"object/fields",
                        {{"serialization/size", fieldJson({{"bytes", 8113}}, "map", kRoot, 16)},
                         {"serialization/hash", fieldJson("d092", "string", kRoot, 16)}}}});
    objects.push_back({{"object/id", kEval},
                       {"object/class", "evaluation"},
                       {"object/created-seq", 17},
                       {"object/created-at", at(17)},
                       {"object/fields",
                        {{"evaluation/initiator", fieldJson(kRoot, json::array({"ref", "principal"}), kRoot, 17)},
                         {"evaluation/model", fieldJson(kModel, json::array({"ref", "model"}), kRoot, 17)},
                         {"evaluation/message", fieldJson(kMsg, json::array({"ref", "message"}), kRoot, 17)},
                         {"evaluation/state", fieldJson("done", "keyword", kRoot, 19)},
                         {"evaluation/serialization", fieldJson(kSer, json::array({"ref", "serialization"}), kRoot, 18)},
                         {"evaluation/iteration", fieldJson(1, "number", kRoot, 20)},
                         {"evaluation/context",
                          fieldJson({{"serialization-bytes", 8113}, {"prompt-tokens", 2287}, {"completion-tokens", 1016}, {"objects", 5}},
                                    "map", kRoot, 21)}}}});
    return json{{"snapshot", {{"seq", 21}, {"objects", objects}}}, {"events", json::array()}};
}

struct Folds {
    Principals principals;
    Evaluations evaluations;
    chat::MessageCache messages;
};

Folds fromEvents() {
    Folds f;
    for (const Event& e : events()) {
        f.principals.apply(e);
        f.evaluations.apply(e);
        if (auto id = f.messages.noteEvent(e)) {
            // the chat would `read` it; the Message's state map as `read` returns it
            f.messages.onRead(*id, json{{"object/id", *id},
                                        {"object/fields",
                                         {{"message/from", {{"field/value", kPerson}}},
                                          {"message/to", {{"field/value", json::array({kRoot})}}},
                                          {"message/route", {{"field/value", kRoute}}},
                                          {"message/text", {{"field/value", "what do you know?"}}}}}});
        }
    }
    return f;
}

Folds fromSnapshot() {
    Folds f;
    const json snap = snapshot();
    seedSnapshot(snap["snapshot"]["objects"], f.principals, f.evaluations, [&f](const SnapshotObject& m) {
        f.messages.request(m.id);
        f.messages.onRead(m.id, m.raw);
    });
    f.principals.setApplied(snap["snapshot"]["seq"].get<Seq>());
    f.evaluations.setApplied(snap["snapshot"]["seq"].get<Seq>());
    return f;
}

} // namespace

TEST_CASE("snapshot: the Principals fold matches a replay", "[snapshot]") {
    Folds a = fromEvents();
    Folds b = fromSnapshot();
    REQUIRE(a.principals.size() == b.principals.size());
    const auto ra = a.principals.ordered();
    const auto rb = b.principals.ordered();
    REQUIRE(ra.size() == rb.size());
    for (std::size_t i = 0; i < ra.size(); ++i) {
        INFO("principal " << ra[i]->id);
        CHECK(ra[i]->id == rb[i]->id);
        CHECK(ra[i]->cls == rb[i]->cls);
        CHECK(ra[i]->drivenBy == rb[i]->drivenBy);
        CHECK(ra[i]->created == rb[i]->created);
        CHECK(a.principals.nameOf(ra[i]->id) == b.principals.nameOf(rb[i]->id));
        CHECK(ra[i]->routes() == rb[i]->routes());
        CHECK(ra[i]->models() == rb[i]->models());
        // every Field: value, author and seq agree (types may be absent on a created event)
        REQUIRE(ra[i]->fields.size() == rb[i]->fields.size());
        for (const auto& [name, fa] : ra[i]->fields) {
            INFO("field " << name);
            const FieldState* fb = rb[i]->field(name);
            REQUIRE(fb != nullptr);
            CHECK(fa.value == fb->value);
            CHECK(fa.by == fb->by);
            CHECK(fa.seq == fb->seq);
        }
    }
    CHECK(a.principals.nameOf(kModel) == "root.model");
    CHECK(b.principals.nameOf(kModel) == "root.model");
    // Routes
    const RouteInfo* qa = a.principals.route(kRoute);
    const RouteInfo* qb = b.principals.route(kRoute);
    REQUIRE(qa);
    REQUIRE(qb);
    CHECK(qa->address == qb->address);
    CHECK(qa->members == qb->members);
    // The tail applies on top of a snapshot exactly as it would after a replay.
    CHECK(b.principals.applied() == 21);
    Event late = ev(22, "field-set", kRoot, kRoot, {{"name", "loop/stalled"}, {"type", "string"}, {"value", "x"}, {"reader", "both"}});
    CHECK(a.principals.apply(late));
    CHECK(b.principals.apply(late));
    CHECK(b.principals.get(kRoot)->loopStalled() == "x");
    // An event at or below the snapshot seq is ignored, as a duplicate would be after a replay.
    CHECK_FALSE(b.principals.apply(ev(9, "field-set", kRoot, kWorld, {{"name", "instructions/charter"}, {"value", "old"}})));
    CHECK(b.principals.get(kRoot)->field("instructions/charter")->value == "# Charter");
}

TEST_CASE("snapshot: Messages seed the transcript's refs and the cache", "[snapshot]") {
    Folds a = fromEvents();
    Folds b = fromSnapshot();
    // the replay saw `sent` + `delivered` (two refs); the snapshot has one object with one recipient
    REQUIRE_FALSE(b.principals.messages().empty());
    const MessageRef& m = b.principals.messages().front();
    CHECK(m.message == kMsg);
    CHECK(m.from == kPerson);
    CHECK(m.recipient == kRoot);
    CHECK(m.route == kRoute);
    CHECK(m.seq == 13);
    CHECK(a.messages.state(kMsg) == chat::MessageState::Ready);
    CHECK(b.messages.state(kMsg) == chat::MessageState::Ready);
    REQUIRE(b.messages.get(kMsg));
    CHECK(b.messages.get(kMsg)->text == a.messages.get(kMsg)->text);
    CHECK(b.messages.get(kMsg)->from == kPerson);
    CHECK(b.messages.pending() == 0);
}

TEST_CASE("snapshot: Evaluations carry state, tokens, context and serialization", "[snapshot]") {
    Folds a = fromEvents();
    Folds b = fromSnapshot();
    const EvaluationInfo* ea = a.evaluations.get(kEval);
    const EvaluationInfo* eb = b.evaluations.get(kEval);
    REQUIRE(ea);
    REQUIRE(eb);
    // the wake that answered the question is found by evaluation/message from either fold: a
    // snapshot-booted transcript needs no ref's via to print the cost line
    CHECK(ea->message == kMsg);
    CHECK(eb->message == kMsg);
    REQUIRE(a.evaluations.forMessage(kMsg, kRoot).size() == 1);
    REQUIRE(b.evaluations.forMessage(kMsg, kRoot).size() == 1);
    CHECK(a.evaluations.forMessage(kMsg, kRoot)[0]->id == kEval);
    CHECK(b.evaluations.forMessage(kMsg, kRoot)[0]->id == kEval);
    CHECK(b.evaluations.forMessage(kMsg, kPerson).empty());
    CHECK(b.evaluations.forMessage("no-such-message").empty());
    REQUIRE(ea);
    REQUIRE(eb);
    CHECK(eb->principal == ea->principal);
    CHECK(eb->model == ea->model);
    CHECK(eb->state == "done");
    CHECK(eb->done());
    CHECK(eb->tokensIn == ea->tokensIn);
    CHECK(eb->tokensOut == ea->tokensOut);
    CHECK(eb->ctxBytes == ea->ctxBytes);
    CHECK(eb->ctxObjects == ea->ctxObjects);
    CHECK(eb->iteration == ea->iteration);
    CHECK(eb->serialization == kSer);
    const SerializationInfo* sa = a.evaluations.serialization(kSer);
    const SerializationInfo* sb = b.evaluations.serialization(kSer);
    REQUIRE(sa);
    REQUIRE(sb);
    CHECK(sb->bytes == sa->bytes);
    CHECK(sb->hash == sa->hash);
    CHECK(model::totalsLine(a.evaluations.totals(kRoot)) == model::totalsLine(b.evaluations.totals(kRoot)));
    CHECK(b.evaluations.running(kRoot) == nullptr); // nothing seeded looks live
    CHECK(b.evaluations.applied() == 21);
}

TEST_CASE("snapshot: the graph built from either fold is the same", "[snapshot]") {
    Folds a = fromEvents();
    Folds b = fromSnapshot();
    GraphModel ga = buildGraph(a.principals);
    GraphModel gb = buildGraph(b.principals);
    REQUIRE(ga.nodes.size() == gb.nodes.size());
    for (std::size_t i = 0; i < ga.nodes.size(); ++i) {
        CHECK(ga.nodes[i].id == gb.nodes[i].id);
        CHECK(ga.nodes[i].name == gb.nodes[i].name);
        CHECK(ga.nodes[i].cls == gb.nodes[i].cls);
    }
    CHECK(ga.edges == gb.edges);
}

TEST_CASE("snapshot: tolerant of the compact and partial shapes", "[snapshot]") {
    // bare values instead of field maps, no names, no created-seq: the earliest set-seq stands in
    Principals p;
    Evaluations e;
    json objects = json::array({json{{"object/id", kRoot}, {"object/class", "principal"},
                                     {"object/fields", {{"principal/driven-by", "process"}, {"loop/iteration", {{"field/value", 2}, {"field/set-seq", 40}}}}}}});
    const SnapshotStats st = seedSnapshot(objects, p, e);
    CHECK(st.objects == 1);
    CHECK(st.principals == 1);
    REQUIRE(p.get(kRoot));
    CHECK(p.get(kRoot)->drivenBy == "process");
    CHECK(p.get(kRoot)->created == 40);
    CHECK(p.get(kRoot)->loopIteration() == 2);
    CHECK(p.nameOf(kRoot) == "034968ad");
    CHECK(seedSnapshot(json::object(), p, e).objects == 0); // not an array: nothing happens
}

TEST_CASE("a retired Object keeps its name after its bindings are gone") {
    const nlohmann::json raw = {{"object/id", "11111111-1111-1111-1111-111111111111"},
                                {"object/class", "adapter"},
                                {"object/names", nlohmann::json::array()},
                                {"object/fields", {{"object/last-names", {{"field/value", {"grid.fs"}}}}}}};
    const ke::model::SnapshotObject o = ke::model::SnapshotObject::parse(raw);
    CHECK(o.name == "grid.fs");
    ke::model::Principals p;
    p.noteRetired(o.id, ke::model::Principals::RetiredInfo{o.name, o.cls, 0});
    CHECK(p.nameOf(o.id) == "grid.fs");
}
