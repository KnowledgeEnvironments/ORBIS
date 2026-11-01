// The Principals fold and the transcript assembly, driven by hand with events in the JSON form
// the world pushes: no network, no UI.
#include "chat/MessageCache.hpp"
#include "chat/Transcript.hpp"
#include "model/Principals.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

constexpr const char* kWorld = "d470bce8-5578-400f-8f31-3504f178c6a8";
constexpr const char* kRoot = "034968ad-3358-4e57-a31b-d1bde0c15b31";
constexpr const char* kPerson = "28b70d40-2916-4adb-967a-2dea363599dc";
constexpr const char* kRoute = "bfb9c6ed-8354-420d-a950-a9b9681af4bc";
constexpr const char* kMsg1 = "d67530c2-5f89-4c9a-a319-1d2fb171e740";
constexpr const char* kMsg2 = "80659512-4bbe-4b99-8226-4414f6ef6e4d";
constexpr const char* kEval = "03d1ac77-b9e5-498b-b28e-da884b8ddfea";

Event ev(Seq seq, const char* kind, const char* stream, const char* actor, json body, const char* via = nullptr) {
    Event e;
    e.seq = seq;
    e.id = "0b5a2c3e-1111-4c2a-8f1e-" + std::to_string(100000000000 + seq);
    e.stream = stream;
    e.actor = actor;
    e.kind = kind;
    e.at = "2026-10-30T15:" + std::string(seq < 10 ? "0" : "") + std::to_string(seq % 60) + ":00.000Z";
    e.body = std::move(body);
    if (via) {
        e.via = via;
    }
    return e;
}

Principals boot() {
    Principals p;
    p.apply(ev(1, "created", kWorld, kWorld,
               {{"class", "principal"}, {"fields", {{"principal/driven-by", "none"}, {"principal/namespace", kWorld}}}}));
    p.apply(ev(2, "created", kRoot, kWorld,
               {{"class", "principal"}, {"fields", {{"principal/driven-by", "process"}, {"principal/namespace", kRoot}}}}));
    p.apply(ev(3, "created", "8733bdc3-b259-41de-8a25-19f94460e2e1", kWorld,
               {{"class", "object"}, {"fields", {{"schema/name", "schema.args.act"}}}})); // not a principal
    p.apply(ev(5, "bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", "world"}, {"object", kWorld}}));
    p.apply(ev(6, "bound", kWorld, kWorld, {{"namespace", kWorld}, {"name", "root"}, {"object", kRoot}}));
    p.apply(ev(7, "field-set", kRoot, kRoot,
               {{"name", "principal/routes"}, {"type", json::array({"list", json::array({"ref", "route"})})},
                {"value", json::array({kRoute})}, {"reader", "both"}, {"by", kRoot}}));
    p.apply(ev(8, "field-set", kRoot, kWorld,
               {{"name", "instructions/charter"}, {"type", "string"}, {"value", "# Charter"}, {"reader", "both"}}));
    p.apply(ev(9, "created", kPerson, kRoot,
               {{"class", "principal"}, {"fields", {{"principal/driven-by", "person"}, {"principal/namespace", kPerson}}}}));
    p.apply(ev(10, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "person"}, {"object", kPerson}}));
    return p;
}

} // namespace

TEST_CASE("transcript selection copies head lines and bodies", "[transcript]") {
    using chat::TranscriptLine;
    std::vector<TranscriptLine> lines(3);
    lines[0].fromName = "root";
    lines[0].at = "2026-10-30T16:58:55.000Z";
    lines[0].mine = true;
    lines[0].ready = true;
    lines[0].text = "what do you know?";
    lines[1].fromName = "root";
    lines[1].at = "2026-10-30T16:59:16.000Z";
    lines[1].via = kEval;
    lines[1].ready = true;
    lines[1].text = "I am root.\nI know my charter.";
    lines[2].fromName = "person";
    lines[2].at = "2026-10-30T17:00:00.000Z";
    lines[2].failure = "unresolved";
    CHECK(chat::transcriptText(lines, 0, 0) == "> root  16:58:55\nwhat do you know?\n");
    CHECK(chat::transcriptText(lines, 1, 1) == "root  16:59:16  [via 03d1ac77]\nI am root.\nI know my charter.\n");
    // Either order, clamped, blank line between messages, unreadable bodies named.
    const std::string all = chat::transcriptText(lines, 9, 0);
    CHECK(all == "> root  16:58:55\nwhat do you know?\n\nroot  16:59:16  [via 03d1ac77]\nI am root.\nI know my charter.\n\n"
                 "person  17:00:00\n<message unreadable: unresolved>\n");
    CHECK(chat::transcriptText({}, 0, 0).empty());
}

TEST_CASE("nameOf: binding, then an object/name Field, then the short id", "[principals]") {
    Principals p = boot();
    CHECK(p.nameOf(kRoot) == "root");            // bound
    CHECK(p.nameOf("8733bdc3-b259-41de-8a25-19f94460e2e1") == "8733bdc3"); // nothing known: 8 chars
    const char* kModel = "b7785bd5-9284-4427-b0b0-54f95bb6f960";
    p.apply(ev(20, "created", kModel, kRoot, {{"class", "model"}, {"fields", {{"principal/driven-by", "none"}}}}));
    CHECK(p.nameOf(kModel) == "b7785bd5");       // a principal without a name yet
    p.apply(ev(21, "field-set", kModel, kRoot,
               {{"name", "object/name"}, {"type", "string"}, {"value", "glm-flash"}, {"reader", "both"}}));
    CHECK(p.nameOf(kModel) == "glm-flash");      // the Field stands in
    p.apply(ev(22, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "root.model"}, {"object", kModel}}));
    CHECK(p.nameOf(kModel) == "root.model");    // a binding wins
    p.apply(ev(23, "unbound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "root.model"}, {"object", kModel}}));
    CHECK(p.nameOf(kModel) == "glm-flash");      // back to the Field
}

TEST_CASE("fold: principals, names, driven-by, routes", "[principals]") {
    Principals p = boot();
    CHECK(p.size() == 3); // world, root, person; the schema object is not a principal
    const auto rows = p.ordered();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0]->id == kWorld);
    CHECK(rows[1]->id == kRoot);
    CHECK(rows[2]->id == kPerson);
    CHECK(p.nameOf(kWorld) == "world");
    CHECK(p.nameOf(kRoot) == "root");
    CHECK(p.nameOf(kPerson) == "person");
    CHECK(p.nameOf("8733bdc3-b259-41de-8a25-19f94460e2e1") == "8733bdc3"); // unnamed: the prefix
    CHECK_FALSE(p.hasName("8733bdc3-b259-41de-8a25-19f94460e2e1"));
    const PrincipalInfo* root = p.get(kRoot);
    REQUIRE(root != nullptr);
    CHECK(root->drivenBy == "process");
    CHECK(root->cls == "principal");
    CHECK(root->routes() == std::vector<std::string>{kRoute});
    REQUIRE(root->field("instructions/charter") != nullptr);
    CHECK(root->field("instructions/charter")->value == "# Charter");
    CHECK(root->field("instructions/charter")->by == kWorld); // actor when the body has no `by`
    CHECK(root->field("principal/routes")->by == kRoot);
    CHECK(root->field("principal/routes")->seq == 7);
    CHECK(p.get(kPerson)->drivenBy == "person");
    CHECK(p.get(kPerson)->routes().empty());
}

TEST_CASE("fold: loop state follows loop/iteration and loop/stalled; later seq wins; dupes ignored",
          "[principals]") {
    Principals p = boot();
    CHECK(loopState(*p.get(kRoot)) == LoopState::Idle);
    CHECK(loopState(*p.get(kPerson)) == LoopState::None);
    CHECK(loopState(*p.get(kWorld)) == LoopState::None);
    CHECK(p.apply(ev(11, "field-set", kRoot, kRoot,
                     {{"name", "loop/iteration"}, {"type", "number"}, {"value", 1}, {"reader", "both"}})));
    CHECK(loopState(*p.get(kRoot)) == LoopState::Running);
    CHECK(p.get(kRoot)->loopIteration() == 1);
    p.apply(ev(12, "field-set", kRoot, kRoot,
               {{"name", "loop/stalled"}, {"type", "string"}, {"value", "evaluation failed: No route to host"}, {"reader", "both"}}));
    p.apply(ev(13, "field-set", kRoot, kRoot,
               {{"name", "loop/iteration"}, {"type", "number"}, {"value", 0}, {"reader", "both"}, {"previous", 1}}));
    CHECK(loopState(*p.get(kRoot)) == LoopState::Stalled);
    CHECK(p.get(kRoot)->loopStalled() == "evaluation failed: No route to host");
    // A duplicate (same or lower seq) changes nothing.
    CHECK_FALSE(p.apply(ev(13, "field-set", kRoot, kRoot,
                           {{"name", "loop/stalled"}, {"type", "string"}, {"value", "bogus"}, {"reader", "both"}})));
    CHECK(p.get(kRoot)->loopStalled() == "evaluation failed: No route to host");
    p.apply(ev(14, "field-set", kRoot, kRoot, {{"name", "loop/stalled"}, {"type", "string"}, {"value", ""}, {"reader", "both"}}));
    CHECK(loopState(*p.get(kRoot)) == LoopState::Idle);
    CHECK(p.applied() == 14);
    // Events for an unknown object do not create principals.
    CHECK_FALSE(p.apply(ev(15, "field-set", "ffffffff-0000-0000-0000-000000000000", kRoot,
                           {{"name", "x"}, {"type", "string"}, {"value", "y"}, {"reader", "both"}})));
    CHECK(p.size() == 3);
}

TEST_CASE("fold: a rebinding renames; unbound forgets", "[principals]") {
    Principals p = boot();
    p.apply(ev(20, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "ian"}, {"object", kPerson}}));
    CHECK(p.nameOf(kPerson) == "ian");
    p.apply(ev(21, "unbound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "ian"}, {"object", kPerson}}));
    CHECK(p.nameOf(kPerson) == "28b70d40");
}

TEST_CASE("transcript: one line per message, in log order, via and mine", "[transcript]") {
    Principals p = boot();
    // person -> root: sent on person's stream, delivered to root.
    p.apply(ev(30, "sent", kPerson, kPerson, {{"message", kMsg1}, {"route", kRoute}}));
    p.apply(ev(31, "delivered", kRoot, kPerson, {{"message", kMsg1}, {"route", kRoute}, {"recipient", kRoot}}));
    // root -> person, produced by an Evaluation.
    p.apply(ev(40, "sent", kRoot, kRoot, {{"message", kMsg2}, {"route", kRoute}}, kEval));
    p.apply(ev(41, "delivered", kPerson, kRoot, {{"message", kMsg2}, {"route", kRoute}, {"recipient", kPerson}}, kEval));
    REQUIRE(p.messages().size() == 4);

    chat::MessageCache cache;
    cache.request(kMsg1);
    json m1 = {{"object/id", kMsg1}, {"object/class", "message"}, {"object/fields", json::object()},
               {"message/from", kPerson}, {"message/to", json::array({kRoot})}, {"message/route", kRoute},
               {"message/text", "what do you know?"}, {"message/tags", json::array()}};
    cache.onRead(kMsg1, m1);
    // kMsg2 not read yet.

    auto nameOf = [&p](const std::string& id) { return p.nameOf(id); };
    SECTION("as seen from root's terminal, typed by person") {
        const auto lines = chat::assembleTranscript(p.messages(), cache, kRoot, kPerson, nameOf);
        REQUIRE(lines.size() == 2);
        CHECK(lines[0].message == kMsg1);
        CHECK(lines[0].seq == 31); // root first sees it when it is delivered
        CHECK(lines[0].fromName == "person");
        CHECK(lines[0].mine);
        CHECK(lines[0].ready);
        CHECK(lines[0].text == "what do you know?");
        CHECK_FALSE(lines[0].via);
        CHECK(lines[1].message == kMsg2);
        CHECK(lines[1].fromName == "root");
        CHECK_FALSE(lines[1].mine);
        CHECK_FALSE(lines[1].ready);
        REQUIRE(lines[1].via);
        CHECK(*lines[1].via == kEval);
    }
    SECTION("as seen from person's terminal when the listener is root: the loop's reply is not ours") {
        const auto lines = chat::assembleTranscript(p.messages(), cache, kPerson, kRoot, nameOf);
        REQUIRE(lines.size() == 2);
        CHECK_FALSE(lines[0].mine);
        CHECK_FALSE(lines[1].mine); // from root, but via an Evaluation
        p.apply(ev(50, "sent", kRoot, kRoot, {{"message", "7a000000-0000-4000-8000-000000000003"}, {"route", kRoute}}));
        const auto again = chat::assembleTranscript(p.messages(), cache, kRoot, kRoot, nameOf);
        REQUIRE(again.size() == 3);
        CHECK(again[2].mine); // typed by root's listener: no via
    }
    SECTION("a principal with no messages has an empty transcript") {
        CHECK(chat::assembleTranscript(p.messages(), cache, kWorld, kRoot, nameOf).empty());
    }
    SECTION("a failed read shows its code") {
        cache.request(kMsg2);
        cache.onReadFailed(kMsg2, "unresolved");
        const auto lines = chat::assembleTranscript(p.messages(), cache, kRoot, kPerson, nameOf);
        REQUIRE(lines.size() == 2);
        CHECK(lines[1].failure == "unresolved");
    }
}

TEST_CASE("recipientsFor: another tab addresses that Principal; the own tab addresses the Route's others",
          "[principals]") {
    Principals p = boot();
    // root's tab, attached as person: to = root
    CHECK(recipientsFor(p, kRoot, kRoute, kPerson) == std::vector<std::string>{kRoot});
    // person's own tab before the Route has members or traffic: the creator (root made person at seq 9)
    CHECK(recipientsFor(p, kPerson, kRoute, kPerson) == std::vector<std::string>{kRoot});
    // root's own tab on a fresh world: created by the world (driven by nothing), so the line goes to itself
    CHECK(recipientsFor(p, kRoot, kRoute, kRoot) == std::vector<std::string>{kRoot});
    // traffic on the Route: participants minus self, deduplicated
    p.apply(ev(11, "sent", kRoot, kRoot, {{"message", kMsg1}, {"route", kRoute}}));
    p.apply(ev(12, "delivered", kPerson, kRoot, {{"message", kMsg1}, {"route", kRoute}, {"recipient", kPerson}}));
    const std::vector<std::string> viaTraffic = recipientsFor(p, kPerson, kRoute, kPerson);
    REQUIRE(viaTraffic.size() == 1);
    CHECK(viaTraffic.front() == kRoot);
    // explicit route/members win and self is dropped
    p.apply(ev(13, "created", kRoute, kRoot,
               {{"class", "route"},
                {"fields", {{"route/address", "root"}, {"route/members", json::array({kPerson, kRoot, kWorld})}}}}));
    CHECK(recipientsFor(p, kPerson, kRoute, kPerson) == std::vector<std::string>{kRoot}); // world is an appliance
    // root's own tab on the same Route: person (world, driven by nothing, is dropped)
    CHECK(recipientsFor(p, kRoot, kRoute, kRoot) == std::vector<std::string>{kPerson});
    // createdBy is folded from the created event's actor
    REQUIRE(p.get(kPerson) != nullptr);
    CHECK(p.get(kPerson)->createdBy == kRoot);
}

TEST_CASE("a Message's device labels its sender in the transcript") {
    model::Principals p;
    Event e;
    e.seq = 1;
    e.kind = "sent";
    e.actor = kPerson;
    e.stream = kPerson;
    e.body = json{{"message", "m-1"}, {"route", "r-1"}};
    p.apply(e);
    chat::MessageCache cache;
    cache.noteEvent(e);
    cache.request("m-1");
    cache.onRead("m-1", json{{"message/from", kPerson}, {"message/text", "hello"}, {"message/device", "hosttwo"}});
    const auto name = [](const std::string&) { return std::string("person"); };
    const auto lines = chat::assembleTranscript(p.messages(), cache, kPerson, kPerson, name);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].fromName == "person on hosttwo");
    cache.onRead("m-1", json{{"message/from", kPerson}, {"message/text", "hello"}});
    CHECK(chat::assembleTranscript(p.messages(), cache, kPerson, kPerson, name)[0].fromName == "person");
}

TEST_CASE("a name is the label, then the bound name, then a short id", "[principals]") {
    Principals p;
    Event created;
    created.seq = 1;
    created.stream = "a1b2c3d4-0000-4000-8000-000000000001";
    created.actor = created.stream;
    created.kind = "created";
    created.body = {{"class", "principal"}, {"fields", {{"principal/driven-by", "process"}}}};
    p.apply(created);
    CHECK(p.nameOf(created.stream) == "a1b2c3d4");
    Event bound = created;
    bound.seq = 2;
    bound.kind = "bound";
    bound.body = {{"namespace", created.stream}, {"name", "ivy"}, {"object", created.stream}};
    p.apply(bound);
    CHECK(p.nameOf(created.stream) == "ivy");
    Event label = created;
    label.seq = 3;
    label.kind = "field-set";
    label.body = {{"name", "object/label"}, {"type", "string"}, {"value", "Ivy the Reviewer"}, {"reader", "both"}};
    p.apply(label);
    CHECK(p.nameOf(created.stream) == "Ivy the Reviewer");
    CHECK(p.boundName(created.stream) == "ivy");
    label.seq = 4;
    label.body["value"] = "";
    p.apply(label);
    CHECK(p.nameOf(created.stream) == "ivy");
}
