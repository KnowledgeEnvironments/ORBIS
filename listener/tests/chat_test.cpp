// The input line (history, parse), the message cache, and the Chat driven by hand through a
// Session with a recording send callback: no network, no UI.
#include "chat/Chat.hpp"
#include "chat/InputLine.hpp"
#include "chat/MessageCache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

using namespace ke;
using namespace ke::chat;

TEST_CASE("HISTORY's routine: a chat turn's bookkeeping goes, what was done stays") {
    const auto row = [](const char* kind, json body) {
        Event e;
        e.kind = kind;
        e.body = std::move(body);
        return e;
    };
    CHECK(isRoutine(row("evaluated", {{"state", "streaming"}, {"delta", "hi"}})));
    CHECK(isRoutine(row("evaluated", {{"state", "done"}})));
    CHECK(isRoutine(row("created", {{"class", "evaluation"}})));
    CHECK(isRoutine(row("created", {{"class", "serialization"}})));
    CHECK(isRoutine(row("field-set", {{"name", "cost/total"}, {"value", 0.1}})));
    CHECK(isRoutine(row("field-set", {{"name", "loop/iteration"}, {"value", 1}})));
    CHECK(isRoutine(row("field-set", {{"name", "evaluation/ops"}, {"value", json::array()}})));
    CHECK(isRoutine(row("field-set", {{"name", "loop/stalled"}, {"value", ""}})));

    CHECK_FALSE(isRoutine(row("evaluated", {{"state", "failed"}})));
    CHECK_FALSE(isRoutine(row("evaluated", {{"state", "cancelled"}})));
    CHECK_FALSE(isRoutine(row("created", {{"class", "task"}})));
    CHECK_FALSE(isRoutine(row("sent", {{"message", "m"}})));
    CHECK_FALSE(isRoutine(row("delivered", {{"message", "m"}})));
    CHECK_FALSE(isRoutine(row("task-moved", {{"to", "done"}})));
    CHECK_FALSE(isRoutine(row("field-set", {{"name", "evaluation/ops"}, {"value", json::array({json{{"op", "read"}}})}})));
    CHECK_FALSE(isRoutine(row("field-set", {{"name", "evaluation/results"}, {"value", json::array({json{{"ok", true}}})}})));
    CHECK_FALSE(isRoutine(row("field-set", {{"name", "memory/notes"}, {"value", "x"}})));
    CHECK_FALSE(isRoutine(row("field-set", {{"name", "loop/stalled"}, {"value", "x"}})));
}

namespace {

constexpr const char* kPrincipal = "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0001";
constexpr const char* kRoute = "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0101";
constexpr const char* kMessage = "6d0e5a7e-2222-4c2a-8f1e-000000000001";

Event event(Seq seq, const char* kind, json body) {
    Event e;
    e.seq = seq;
    e.id = "0b5a2c3e-1111-4c2a-8f1e-" + std::to_string(100000000000 + seq);
    e.stream = kPrincipal;
    e.actor = kPrincipal;
    e.kind = kind;
    e.at = "2026-10-30T06:00:00.000Z";
    e.body = std::move(body);
    return e;
}

json messageState(const char* id, const std::string& text, bool asFields) {
    json m = {{"object/id", id}, {"object/class", "message"}, {"object/stream", id}, {"object/names", json::array()}};
    json attrs = {{"message/from", kPrincipal}, {"message/to", json::array({kPrincipal})}, {"message/route", kRoute},
                  {"message/text", text},        {"message/tags", json::array()},           {"message/seq", 7}};
    if (asFields) {
        json fields = json::object();
        for (auto& [k, v] : attrs.items()) {
            fields[k] = {{"field/name", k}, {"field/value", v}};
        }
        m["object/fields"] = fields;
    } else {
        m["object/fields"] = json::object();
        m.update(attrs);
    }
    return m;
}

struct Harness {
    MemorySessionStore store;
    EventStore events;
    std::vector<Frame> sent;
    std::unique_ptr<Session> session;
    std::unique_ptr<Chat> chat;

    Harness() {
        SessionConfig cfg;
        cfg.token = "root-token";
        session = std::make_unique<Session>(cfg, store, events, [this](const Frame& f) { sent.push_back(f); });
        chat = std::make_unique<Chat>(*session, events);
    }

    std::vector<Request> requests(const std::string& op = "") const {
        std::vector<Request> out;
        for (const Frame& f : sent) {
            if (auto* r = std::get_if<Request>(&f); r && (op.empty() || r->op == op)) {
                out.push_back(*r);
            }
        }
        return out;
    }
    const Request& last(const std::string& op) const {
        const auto rs = requests(op);
        REQUIRE(!rs.empty());
        static Request copy;
        copy = rs.back();
        return copy;
    }

    // World side: open, attach (fresh), subscribe answered; then the Chat ticks and reads the route.
    void attach(Seq head = 220, bool resumed = false) {
        chat->onOpen();
        session->onOpen();
        const Request& a = last("attach");
        session->onFrame(okResponse(
            a.id, json{{"session", "s-01"}, {"principal", kPrincipal}, {"resumed", resumed}, {"seq", head}}, head));
        if (!resumed) {
            const Request& s = last("subscribe");
            session->onFrame(okResponse(s.id, json{{"sub", "history"}, {"as-of", head}}, head));
        }
        chat->tick();
    }
    void answerRouteRead(json routes) {
        const Request& r = last("read");
        REQUIRE(r.args["object"] == kPrincipal);
        json state = {{"object/id", kPrincipal}, {"object/class", "principal"}, {"object/fields", json::object()}};
        if (!routes.is_null()) {
            state["object/fields"]["principal/routes"] = {{"field/name", "principal/routes"}, {"field/value", routes}};
        }
        session->onFrame(okResponse(r.id, state, 221));
    }
    void push(std::vector<Event> evs, Seq asOf) {
        session->onFrame(Push{"history", std::move(evs), asOf, false});
        chat->tick();
    }
};

} // namespace

// ---- InputLine ------------------------------------------------------------------------------

TEST_CASE("input history keeps the last 50 lines and recalls them with up/down", "[chat][input]") {
    InputLine in;
    for (int i = 0; i < 60; ++i) {
        in.push("line " + std::to_string(i));
    }
    REQUIRE(in.history().size() == InputLine::kMax);
    CHECK(in.history().front() == "line 10");
    CHECK(in.history().back() == "line 59");

    CHECK(in.up("draft") == "line 59");
    CHECK(in.up("ignored") == "line 58");
    CHECK(in.down() == "line 59");
    CHECK(in.down() == "draft"); // past the newest: the draft comes back
    CHECK_FALSE(in.recalling());
    CHECK_FALSE(in.down().has_value());

    in.push("");
    in.push("   ");
    in.push("line 59"); // immediate repeat is not kept
    REQUIRE(in.history().size() == InputLine::kMax);
    CHECK(in.history().back() == "line 59");

    InputLine empty;
    CHECK_FALSE(empty.up("x").has_value());
    CHECK_FALSE(empty.down().has_value());
}

TEST_CASE("parse: who am i, plain text, and ref tags", "[chat][input]") {
    CHECK(parseLine("").kind == CommandKind::Empty);
    CHECK(parseLine("   \t").kind == CommandKind::Empty);
    CHECK(parseLine("who am i").kind == CommandKind::WhoAmI);
    CHECK(parseLine("  Who  AM i \n").kind == CommandKind::WhoAmI);
    CHECK(parseLine("who am i?").kind == CommandKind::Text);

    const Command plain = parseLine("  hello world ");
    CHECK(plain.kind == CommandKind::Text);
    CHECK(plain.text == "hello world");
    CHECK(plain.tags.empty());

    CHECK(refPrefixes("see @5a1f7b0e and @5a1f-7b0e-7d6b, not @abc or @zz") ==
          std::vector<std::string>{"5a1f7b0e", "5a1f7b0e7d6b"});
    CHECK(idHasPrefix(kPrincipal, "5a1f7b0e"));
    CHECK(idHasPrefix(kPrincipal, "5A1F7B0E7D6B"));
    CHECK_FALSE(idHasPrefix(kPrincipal, "5a1f7b0f"));
    CHECK_FALSE(idHasPrefix("5a1f", "5a1f7b0e"));

    auto resolver = [](std::string_view p) { return idHasPrefix(kPrincipal, p) ? std::string(kPrincipal) : ""; };
    const Command tagged = parseLine("ping @5a1f7b0e twice @5a1f7b0e and @deadbeef", resolver);
    CHECK(tagged.kind == CommandKind::Text);
    CHECK(tagged.text == "ping @5a1f7b0e twice @5a1f7b0e and @deadbeef");
    CHECK(tagged.tags == std::vector<std::string>{kPrincipal});
    CHECK(parseLine("ping @5a1f7b0e").tags.empty()); // no resolver, no tags
}

// ---- MessageCache ---------------------------------------------------------------------------

TEST_CASE("message cache: one read per message across sent and delivered", "[chat][messages]") {
    MessageCache cache;
    const Event sent = event(10, "sent", {{"message", kMessage}, {"route", kRoute}});
    const Event delivered = event(11, "delivered", {{"message", kMessage}, {"route", kRoute}, {"recipient", kPrincipal}});
    const Event other = event(12, "field-set", {{"name", "x"}});
    const Event bare = event(13, "sent", {{"mock", true}});

    CHECK(MessageCache::messageIdOf(sent) == kMessage);
    CHECK_FALSE(MessageCache::messageIdOf(other).has_value());
    CHECK_FALSE(MessageCache::messageIdOf(bare).has_value());

    CHECK(cache.noteEvent(sent) == kMessage);
    CHECK_FALSE(cache.noteEvent(delivered).has_value()); // already pending
    CHECK_FALSE(cache.noteEvent(other).has_value());
    CHECK(cache.state(kMessage) == MessageState::Pending);
    CHECK(cache.pending() == 1);
    CHECK(cache.get(kMessage) == nullptr);

    cache.onRead(kMessage, messageState(kMessage, "hi there", true));
    REQUIRE(cache.get(kMessage) != nullptr);
    CHECK(cache.get(kMessage)->text == "hi there");
    CHECK(cache.get(kMessage)->from == kPrincipal);
    CHECK(cache.get(kMessage)->route == kRoute);
    CHECK(cache.get(kMessage)->to == std::vector<std::string>{kPrincipal});
    CHECK(cache.get(kMessage)->seq == 7);
    CHECK_FALSE(cache.get(kMessage)->inReplyTo.has_value());
    CHECK(cache.ready() == 1);
    CHECK(cache.pending() == 0);
    CHECK_FALSE(cache.noteEvent(delivered).has_value()); // ready: still no second read
}

TEST_CASE("message cache: top-level attributes, failures, and lost reads", "[chat][messages]") {
    json flat = messageState(kMessage, "flat form", false);
    flat["message/in-reply-to"] = "6d0e5a7e-2222-4c2a-8f1e-000000000000";
    const MessageRow row = MessageCache::parseMessage(kMessage, flat);
    CHECK(row.text == "flat form");
    CHECK(row.inReplyTo == "6d0e5a7e-2222-4c2a-8f1e-000000000000");
    CHECK(idPrefix(kMessage) == "6d0e5a7e");
    CHECK(stateValue(flat, "message/text") == "flat form");
    CHECK(stateValue(flat, "nope").is_null());
    CHECK(stateValue(json(3), "x").is_null());

    MessageCache cache;
    CHECK(cache.request(kMessage));
    CHECK_FALSE(cache.request(kMessage));
    cache.onReadFailed(kMessage, "unresolved");
    CHECK(cache.state(kMessage) == MessageState::Failed);
    REQUIRE(cache.failure(kMessage) != nullptr);
    CHECK(*cache.failure(kMessage) == "unresolved");
    CHECK(cache.pending() == 0);

    MessageCache lost;
    lost.request("a");
    lost.request("b");
    lost.onRead("b", messageState("b", "done", true));
    const auto again = lost.resetPending();
    CHECK(again == std::vector<std::string>{"a"});
    CHECK(lost.state("a") == MessageState::Unknown);
    CHECK(lost.state("b") == MessageState::Ready);
    CHECK(lost.request("a"));
}

// ---- Chat -----------------------------------------------------------------------------------

TEST_CASE("chat: boot lines, route discovery, and the send request", "[chat]") {
    Harness h;
    h.attach();

    // One line says who and where.
    REQUIRE(h.chat->notes().size() == 1);
    CHECK(h.chat->notes()[0].text == std::string("ORBIS // ATTACHED AS ") + kPrincipal + " // HEAD 220");

    // The route read went out at attach; no route yet -> NONE with a 5 s retry pending.
    CHECK(h.chat->routeState() == RouteState::Searching);
    h.answerRouteRead(json());
    CHECK(h.chat->routeState() == RouteState::None);
    CHECK(h.chat->nextDeadline(Clock::now()).has_value());
    h.chat->submit("early");
    CHECK(h.chat->notes().back().text == "[ SEND: NO ROUTE ] early");
    CHECK(h.requests("send").empty());

    // The retry fires after 5 s and finds the route.
    const auto reads = h.requests("read").size();
    h.chat->tick(Clock::now() + std::chrono::seconds(6));
    REQUIRE(h.requests("read").size() == reads + 1);
    h.answerRouteRead(json::array({kRoute, "other"}));
    CHECK(h.chat->routeState() == RouteState::Found);
    CHECK(h.chat->route() == kRoute);
    CHECK(h.chat->stats().routeReads == 2);

    h.chat->submit("hello @5a1f7b0e");
    const Request& send = h.last("send");
    CHECK(send.args["to"] == json::array({kPrincipal}));
    CHECK(send.args["route"] == kRoute);
    CHECK(send.args["text"] == "hello @5a1f7b0e");
    CHECK(send.args["tags"] == json::array()); // nothing in the store starts with that prefix yet
    CHECK(h.chat->notes().back().text == "> hello @5a1f7b0e");
    CHECK(h.chat->input().history().back() == "hello @5a1f7b0e");

    h.session->onFrame(okResponse(send.id, json{{"message", kMessage}, {"seq", 230}}, 230));
    CHECK(h.chat->stats().sendsOk == 1);
    CHECK(h.chat->stats().lastMessageId == kMessage);
}

TEST_CASE("chat: not-implemented is shown once, unresolved every time", "[chat]") {
    Harness h;
    h.attach();
    h.answerRouteRead(json::array({kRoute}));
    const std::size_t before = h.chat->notes().size();

    h.chat->submit("one");
    h.session->onFrame(errResponse(h.last("send").id, "not-implemented", "op send is not built yet", 221));
    h.chat->submit("two");
    h.session->onFrame(errResponse(h.last("send").id, "not-implemented", "op send is not built yet", 222));
    int notBuilt = 0;
    for (std::size_t i = before; i < h.chat->notes().size(); ++i) {
        notBuilt += h.chat->notes()[i].text == "[ SEND: NOT BUILT ]";
    }
    CHECK(notBuilt == 1);
    CHECK(h.chat->sendNotBuilt());
    CHECK(h.chat->stats().sendsErr == 2);
    CHECK(h.chat->stats().lastSendCode == "not-implemented");

    h.chat->submit("three");
    h.session->onFrame(errResponse(h.last("send").id, "unresolved", "does not resolve", 223));
    CHECK(h.chat->notes().back().text == "[ SEND: UNRESOLVED ] does not resolve");
    CHECK(h.chat->notes().back().kind == NoteKind::Error);
}

TEST_CASE("chat: sent and delivered rows read the Message once; who am i prints fields", "[chat]") {
    Harness h;
    h.attach();
    h.answerRouteRead(json::array({kRoute}));
    const auto readsBefore = h.requests("read").size();

    h.push({event(221, "sent", {{"message", kMessage}, {"route", kRoute}}),
            event(222, "delivered", {{"message", kMessage}, {"route", kRoute}, {"recipient", kPrincipal}})},
           222);
    REQUIRE(h.requests("read").size() == readsBefore + 1);
    const Request& read = h.last("read");
    CHECK(read.args["object"] == kMessage);
    CHECK(h.chat->messages().state(kMessage) == MessageState::Pending);
    h.session->onFrame(okResponse(read.id, messageState(kMessage, "round trip", true), 222));
    REQUIRE(h.chat->messages().get(kMessage) != nullptr);
    CHECK(h.chat->messages().get(kMessage)->text == "round trip");

    // A ref tag now resolves against the store (the event's stream is the principal).
    h.chat->submit("re @5a1f7b0e");
    CHECK(h.last("send").args["tags"] == json::array({kPrincipal}));

    const std::size_t notesBefore = h.chat->notes().size();
    h.chat->submit("who am i");
    const Request& who = h.last("read");
    CHECK(who.args["object"] == kPrincipal);
    json state = {{"object/id", kPrincipal},
                  {"object/class", "principal"},
                  {"object/fields",
                   {{"principal/driven-by", {{"field/name", "principal/driven-by"}, {"field/value", "person"}}},
                    {"notes/x", {{"field/name", "notes/x"}, {"field/value", "multi\nline"}}}}}};
    h.session->onFrame(okResponse(who.id, state, 223));
    REQUIRE(h.chat->notes().size() == notesBefore + 4);
    CHECK(h.chat->notes()[notesBefore].chip == "WHO");
    CHECK(h.chat->notes()[notesBefore].text == std::string("object/id ") + kPrincipal);
    CHECK(h.chat->notes()[notesBefore + 2].text == "notes/x = multi line");
    CHECK(h.chat->notes()[notesBefore + 3].text == "principal/driven-by = person");
    CHECK(h.chat->stats().whoRows == 4);
    CHECK(h.chat->input().history().back() == "who am i");
}

TEST_CASE("chat: carrier lost and resumed, pending reads retried after the drop", "[chat]") {
    Harness h;
    h.attach();
    h.answerRouteRead(json::array({kRoute}));
    h.push({event(221, "sent", {{"message", kMessage}, {"route", kRoute}})}, 221);
    CHECK(h.chat->messages().pending() == 1);
    CHECK_FALSE(h.chat->carrierLost());

    h.session->onClose();
    h.chat->onClosed();
    CHECK(h.chat->carrierLost());
    CHECK(h.chat->messages().state(kMessage) == MessageState::Unknown);

    const auto readsBefore = h.requests("read").size();
    h.attach(225, true);
    CHECK_FALSE(h.chat->carrierLost());
    CHECK(h.chat->resumedFrom() == 221);
    CHECK(h.chat->routeState() == RouteState::Found); // a known route survives the drop
    REQUIRE(h.requests("read").size() == readsBefore + 1); // the lost message read, not the route
    CHECK(h.last("read").args["object"] == kMessage);
    CHECK(h.chat->messages().state(kMessage) == MessageState::Pending);
}

TEST_CASE("chat: boot off prints one line; streaming hook follows evaluated events", "[chat]") {
    Harness h;
    h.attach();
    REQUIRE(h.chat->notes().size() == 1);
    CHECK(h.chat->notes()[0].text == std::string("ORBIS // ATTACHED AS ") + kPrincipal + " // HEAD 220");
    CHECK(h.chat->notes()[0].cursor);

    h.push({event(221, "evaluated", {{"evaluation", "ev-1"}, {"state", "streaming"}})}, 221);
    CHECK(h.chat->streamingSeqs() == std::set<Seq>{221});
    h.push({event(222, "evaluated", {{"evaluation", "ev-1"}, {"state", "done"}})}, 222);
    CHECK(h.chat->streamingSeqs().empty());
}

TEST_CASE("a named action's nested result is flattened for its handler", "[chat][actions]") {
    Harness h;
    h.attach();
    h.answerRouteRead(json::array());
    std::optional<Response> seen;
    h.chat->actNamed("token.issue", json{{"principal", kPrincipal}, {"label", "desk"}},
                     [&seen](const Response& r) { seen = r; });
    const Request& a = h.last("act");
    CHECK(a.args["action"] == "token.issue");
    // the world answers {result {...} object seq}: the handler reads the keys at the top
    h.session->onFrame(okResponse(
        a.id, json{{"result", json{{"token", "t-1"}, {"secret", "s3cr3t-s3cr3t-s3cr3t"}, {"label", "desk"}}}, {"object", "t-1"}},
        300));
    REQUIRE(seen.has_value());
    CHECK(seen->ok);
    CHECK(seen->result["secret"] == "s3cr3t-s3cr3t-s3cr3t");
    CHECK(seen->result["token"] == "t-1");
    CHECK(seen->result["object"] == "t-1");
    CHECK_FALSE(seen->result.contains("result"));
    // a flat answer passes through untouched
    seen.reset();
    h.chat->actNamed("model.probe", json{{"model", "e-1"}}, [&seen](const Response& r) { seen = r; });
    const Request& b = h.last("act");
    h.session->onFrame(okResponse(b.id, json{{"model", "e-1"}, {"status", "ok"}}, 301));
    REQUIRE(seen.has_value());
    CHECK(seen->result["status"] == "ok");
}

TEST_CASE("every act frame carries args, as the schema requires") {
    std::ifstream in(std::string(LISTENER_PROTOCOL_EXAMPLES_DIR) + "/../../listener/generated/schema/ke.args.act.json");
    REQUIRE(in.good());
    const json schema = json::parse(in);
    REQUIRE(schema.contains("required"));
    Harness h;
    h.chat->actOn(kMessage, "cancel", json(), [](const Response&) {});
    h.chat->actOn(kMessage, "run", json{{"cmd", "ls"}}, [](const Response&) {});
    h.chat->actNamed("task.run", json(), [](const Response&) {});
    const auto acts = h.requests("act");
    REQUIRE(acts.size() == 3);
    for (const Request& r : acts) {
        for (const auto& key : schema["required"]) {
            CHECK(r.args.contains(key.get<std::string>()));
        }
        REQUIRE(r.args.contains("args"));
        CHECK(r.args["args"].is_object());
    }
    CHECK(acts[0].args["args"].empty());
    CHECK(acts[1].args["args"]["cmd"] == "ls");
}
