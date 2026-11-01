// Resume bookkeeping without a network: the Session is driven by hand through onOpen/onFrame
// and observed through the frames it hands to its send callback (docs/protocol.md "Session").
#include "net/Session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace ke;

namespace {

constexpr const char* kPrincipal = "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0001";

struct Harness {
    MemorySessionStore store;
    EventStore events;
    std::vector<Frame> sent;
    std::vector<GapReport> gaps;
    std::unique_ptr<Session> session;

    explicit Harness(SessionConfig cfg = {}) {
        if (cfg.token.empty()) {
            cfg.token = "root-token";
        }
        events.onGap = [this](const GapReport& g) { gaps.push_back(g); };
        session =
            std::make_unique<Session>(cfg, store, events, [this](const Frame& f) { sent.push_back(f); });
    }

    const Request& lastRequest() const { return std::get<Request>(sent.back()); }

    std::vector<Request> requests() const {
        std::vector<Request> out;
        for (const Frame& f : sent) {
            if (auto* r = std::get_if<Request>(&f)) {
                out.push_back(*r);
            }
        }
        return out;
    }
    std::vector<Ack> acks() const {
        std::vector<Ack> out;
        for (const Frame& f : sent) {
            if (auto* a = std::get_if<Ack>(&f)) {
                out.push_back(*a);
            }
        }
        return out;
    }

    // World side: answer the pending attach.
    void answerAttach(const std::string& sessionId, bool resumed, Seq head) {
        const Request& r = lastRequest();
        REQUIRE(r.op == "attach");
        session->onFrame(okResponse(
            r.id,
            json{{"session", sessionId}, {"principal", kPrincipal}, {"resumed", resumed}, {"seq", head}},
            head));
    }
    void answerSubscribe(Seq head) {
        const Request& r = lastRequest();
        REQUIRE(r.op == "subscribe");
        session->onFrame(okResponse(r.id, json{{"sub", r.args.at("sub")}, {"as-of", head}}, head));
    }
    void push(Seq from, Seq to, Seq asOf, bool lagging = false, std::optional<Seq> cursorFrom = std::nullopt) {
        Push p;
        p.sub = "history";
        p.asOf = asOf;
        p.lagging = lagging;
        p.from = cursorFrom;
        for (Seq s = from; s <= to; ++s) {
            Event e;
            e.id = "e-" + std::to_string(s);
            e.stream = kPrincipal;
            e.actor = kPrincipal;
            e.seq = s;
            e.kind = "field-set";
            e.at = "2026-10-29T06:00:00.000Z";
            e.body = {{"seq", s}};
            p.events.push_back(e);
        }
        session->onFrame(p);
    }
};

} // namespace

TEST_CASE("first attach is fresh and subscribes without after", "[session]") {
    Harness h;
    h.session->onOpen();
    const Request& attach = h.lastRequest();
    CHECK(attach.op == "attach");
    CHECK(attach.args.at("token") == "root-token");
    CHECK(attach.args.at("client-kind") == "listener");
    CHECK_FALSE(attach.args.contains("session"));
    CHECK_FALSE(attach.args.contains("subs"));
    CHECK_FALSE(attach.args.contains("principal"));

    h.answerAttach("s-01", false, 42);
    CHECK(h.session->attached());
    CHECK(h.session->sessionId() == "s-01");
    CHECK(h.session->principal() == kPrincipal);
    CHECK(h.session->head() == 42);
    CHECK(h.store.value->session == "s-01");

    const Request& sub = h.lastRequest();
    CHECK(sub.op == "subscribe");
    CHECK(sub.args.at("sub") == "history");
    CHECK(sub.args.at("window") == 256);
    CHECK_FALSE(sub.args.contains("after"));
    h.answerSubscribe(42);
    CHECK(h.session->subs().at("history").subscribed);
}

TEST_CASE("a new client on a long log starts a window behind the head", "[session]") {
    SessionConfig cfg;
    cfg.historyWindow = 2000;
    Harness h(cfg);
    h.session->onOpen();
    h.answerAttach("s-1", false, 220000);
    CHECK(h.lastRequest().op == "subscribe");
    CHECK(h.lastRequest().args.at("after") == 218000);
    h.answerSubscribe(220000);
}

TEST_CASE("a short log and a client near the head are not windowed", "[session]") {
    SessionConfig cfg;
    cfg.historyWindow = 2000;
    Harness fresh(cfg);
    fresh.session->onOpen();
    fresh.answerAttach("s-1", false, 1500);
    CHECK_FALSE(fresh.lastRequest().args.contains("after"));

    MemorySessionStore store;
    store.value = Persisted{"s-old", kPrincipal, {{"history", 219500}}};
    EventStore events;
    std::vector<Frame> sent;
    cfg.token = "root-token";
    Session session(cfg, store, events, [&](const Frame& f) { sent.push_back(f); });
    session.onOpen();
    const std::string id = std::get<Request>(sent.back()).id;
    session.onFrame(okResponse(id, json{{"session", "s-2"}, {"principal", kPrincipal}, {"resumed", false}, {"seq", 220000}}, 220000));
    CHECK(std::get<Request>(sent.back()).args.at("after") == 219500);
}

TEST_CASE("a returning client far behind leaves its old cursor, even on a resumed session", "[session]") {
    SessionConfig cfg;
    cfg.token = "root-token";
    cfg.historyWindow = 2000;
    MemorySessionStore store;
    store.value = Persisted{"s-old", kPrincipal, {{"history", 7000}}};
    EventStore events;
    std::vector<Frame> sent;
    Session session(cfg, store, events, [&](const Frame& f) { sent.push_back(f); });
    session.onOpen();
    const std::string id = std::get<Request>(sent.back()).id;
    session.onFrame(okResponse(id, json{{"session", "s-old"}, {"principal", kPrincipal}, {"resumed", true}, {"seq", 220000}}, 220000));
    const Request& r = std::get<Request>(sent.back());
    CHECK(r.op == "subscribe");
    CHECK(r.args.at("after") == 218000);
}

TEST_CASE("rows paged in above the oldest stay in order and never overlap", "[session]") {
    EventStore s;
    const auto ev = [](Seq n) {
        Event e;
        e.seq = n;
        return e;
    };
    s.append("history", ev(100));
    s.append("history", ev(101));
    s.prepend({ev(98), ev(99), ev(100)});
    REQUIRE(s.rows().size() == 4);
    CHECK(s.rows().front().seq == 98);
    CHECK(s.oldestSeq() == 98);
    CHECK(s.prepends() == 1);
    CHECK(s.rows().back().seq == 101);
    s.prepend({});
    CHECK(s.prepends() == 1);
}

TEST_CASE("pushes land, are acked, and the acked map is persisted", "[session]") {
    Harness h;
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    h.answerSubscribe(42);

    h.push(43, 44, 44);
    REQUIRE(h.events.rows().size() == 2);
    REQUIRE(h.acks().size() == 1);
    CHECK(h.acks()[0].sub == "history");
    CHECK(h.acks()[0].seq == 44);
    CHECK(h.store.value->acked.at("history") == 44);

    const SubState& s = h.session->subs().at("history");
    CHECK(s.cursor == 44);
    CHECK(s.acked == 44);
    CHECK(s.asOf == 44);
    CHECK(s.unacked() == 0);
    CHECK(s.lag() == 0);
    CHECK(h.session->head() == 44);
    CHECK(h.gaps.empty());
}

TEST_CASE("an off-the-record push goes to its handler and touches nothing else", "[session]") {
    Harness h;
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    h.answerSubscribe(42);
    std::vector<Push> taken;
    h.session->scratchPush = [&taken](const Push& p) { taken.push_back(p); };
    const std::size_t sentBefore = h.sent.size();

    Push p;
    p.sub = "scratch-abc";
    p.asOf = 9000; // the fork's own head, which must never become ours
    Event e;
    e.id = "e-x";
    e.stream = kPrincipal;
    e.actor = kPrincipal;
    e.seq = 8999;
    e.kind = "evaluated";
    e.at = "2026-10-29T06:00:00.000Z";
    e.body = {{"state", "streaming"}, {"delta", "secret"}};
    p.events.push_back(e);
    h.session->onFrame(p);

    REQUIRE(taken.size() == 1);
    CHECK(taken[0].events[0].body["delta"] == "secret");
    CHECK(h.events.rows().empty());
    CHECK(h.session->head() == 42);
    CHECK(h.sent.size() == sentBefore); // no ack and no unsubscribe as a stale sub
    CHECK(h.session->subs().count("scratch-abc") == 0);
}

TEST_CASE("reattach sends the persisted session and acked map", "[session]") {
    Harness h;
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    h.answerSubscribe(42);
    h.push(43, 50, 50);
    h.session->onClose();
    CHECK_FALSE(h.session->attached());

    h.session->onOpen();
    const Request& re = h.lastRequest();
    CHECK(re.op == "attach");
    CHECK(re.args.at("session") == "s-01");
    CHECK(re.args.at("subs") == json{{"history", 50}});

    SECTION("resumed=true: no re-subscribe, delivery continues at acked+1") {
        const auto before = h.requests().size();
        h.answerAttach("s-01", true, 53);
        CHECK(h.requests().size() == before); // nothing new went out
        CHECK(h.session->subs().at("history").subscribed);
        CHECK(h.session->resumes() == 1);
        h.push(51, 53, 53);
        CHECK(h.events.rows().size() == 11);
        CHECK(h.gaps.empty());
        CHECK(h.acks().back().seq == 53);
    }

    SECTION("resumed=false: re-subscribe with after = acked") {
        h.answerAttach("s-02", false, 53);
        const Request& sub = h.lastRequest();
        CHECK(sub.op == "subscribe");
        CHECK(sub.args.at("after") == 50);
        CHECK(h.session->sessionId() == "s-02");
        CHECK(h.store.value->session == "s-02");
        h.answerSubscribe(53);
        h.push(51, 53, 53);
        CHECK(h.events.rows().size() == 11);
        CHECK(h.gaps.empty());
    }
}

TEST_CASE("duplicates by seq are dropped and still acked", "[session]") {
    Harness h;
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    h.answerSubscribe(42);
    h.push(43, 45, 45);
    h.push(44, 46, 46); // 44 and 45 again
    CHECK(h.events.rows().size() == 4);
    CHECK(h.events.duplicates() == 0); // the Session filtered them before the store
    CHECK(h.session->subs().at("history").duplicates == 2);
    CHECK(h.acks().back().seq == 46);
    CHECK(h.gaps.empty());

    h.push(43, 44, 46); // nothing new: no row, but still acked at the cursor
    CHECK(h.events.rows().size() == 4);
    CHECK(h.acks().back().seq == 46);
}

TEST_CASE("a persisted cursor is loaded and used on the first attach of a new run", "[session]") {
    MemorySessionStore store;
    store.value = Persisted{"s-09", kPrincipal, {{"history", 57}}};
    EventStore events;
    std::vector<Frame> sent;
    SessionConfig cfg;
    cfg.token = "root-token";
    Session session(cfg, store, events, [&](const Frame& f) { sent.push_back(f); });
    session.onOpen();
    const Request& r = std::get<Request>(sent.back());
    CHECK(r.args.at("session") == "s-09");
    CHECK(r.args.at("subs") == json{{"history", 57}}); // reattach.request.json shape
    CHECK(session.subs().at("history").acked == 57);
}

TEST_CASE("window accounting and lagging follow the pushes", "[session]") {
    SessionConfig cfg;
    cfg.token = "t";
    cfg.window = 4;
    Harness h(cfg);
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    CHECK(h.lastRequest().args.at("window") == 4);
    h.answerSubscribe(42);
    h.push(43, 46, 60, true);
    const SubState& s = h.session->subs().at("history");
    CHECK(s.lagging);
    CHECK(s.lag() == 14);
    CHECK(s.received == 4);
    CHECK(s.acksSent == 1);
    CHECK(h.session->head() == 60);
}

TEST_CASE("a skipped seq is a filtered hole, not a gap; a head regression is a gap", "[session][store]") {
    Harness h;
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    h.answerSubscribe(42);
    h.push(43, 44, 44);
    h.push(47, 47, 47); // 45..46 were not for us: the world's cursor skipped them
    CHECK(h.gaps.empty());
    REQUIRE(h.events.holes().size() == 1);
    CHECK(h.events.holes()[0].expected == 45);
    CHECK(h.events.holes()[0].got == 47);
    CHECK_FALSE(h.events.holes()[0].withinPush); // the hole sits between two pushes
    CHECK(h.events.gaps().empty());
    // A hole inside one push is certainly a filter, never a loss.
    Push p;
    p.sub = "history";
    p.asOf = 50;
    for (Seq s : {48, 50}) {
        Event e;
        e.seq = s;
        e.id = "e-" + std::to_string(s);
        e.kind = "field-set";
        e.stream = "x";
        e.actor = "x";
        e.at = "2026-10-30T20:00:00.000Z";
        e.body = json::object();
        p.events.push_back(e);
    }
    h.session->onFrame(Frame{p});
    REQUIRE(h.events.holes().size() == 2);
    CHECK(h.events.holes()[1].withinPush);
    CHECK(h.events.gaps().empty());
    // The world's head falls below our cursor: a different or truncated log. That is a gap.
    h.push(51, 51, 30);
    REQUIRE(h.gaps.size() == 1);
    CHECK(h.gaps[0].expected == 50);
    CHECK(h.gaps[0].got == 30);
    CHECK(h.events.gaps().size() == 1);
    h.push(52, 52, 30); // the same regression is not counted twice
    CHECK(h.events.gaps().size() == 1);
}

TEST_CASE("errors are recorded and ad-hoc requests reach their handler", "[session]") {
    Harness h;
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    h.answerSubscribe(42);
    std::optional<Response> got;
    const std::string id =
        h.session->request("bind", json{{"name", "x"}, {"object", "y"}}, [&](const Response& r) { got = r; });
    h.session->onFrame(errResponse(id, "not-implemented", "bind is later", 42));
    REQUIRE(got.has_value());
    CHECK_FALSE(got->ok);
    CHECK(h.session->lastError() == "bind: not-implemented (bind is later)");
}

TEST_CASE("a quiet request's err reaches its handler but not lastError", "[session]") {
    Harness h;
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    h.answerSubscribe(42);
    std::optional<Response> got;
    const std::string id = h.session->request(
        "bind", json{{"name", "workspace/hostone"}, {"object", "y"}}, [&](const Response& r) { got = r; }, true);
    h.session->onFrame(errResponse(id, "unresolved", "does not resolve", 42));
    REQUIRE(got.has_value());
    CHECK_FALSE(got->ok);
    CHECK(got->err->code == "unresolved");
    CHECK(h.session->lastError().empty());
}

TEST_CASE("attach failure leaves the session unattached", "[session]") {
    Harness h;
    h.session->onOpen();
    const Request& r = h.lastRequest();
    h.session->onFrame(errResponse(r.id, "unauthenticated", "bad token", 1));
    CHECK_FALSE(h.session->attached());
    CHECK_FALSE(h.session->attaching());
    CHECK(h.session->lastError().find("unauthenticated") != std::string::npos);
}

TEST_CASE("persisted state round-trips through json", "[session]") {
    Persisted p{"s-01", std::string(kPrincipal), {{"history", 44}, {"tasks", 9}}};
    CHECK(Persisted::fromJson(p.toJson()) == p);
    Persisted noPrincipal{"s-02", std::nullopt, {}};
    CHECK(Persisted::fromJson(noPrincipal.toJson()) == noPrincipal);
}

TEST_CASE("the session file keeps one entry per world and adopts the old single-entry shape", "[session][store]") {
    const auto dir = std::filesystem::temp_directory_path() / ("orbis-session-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto file = dir / "session.json";
    {
        std::ofstream out(file);
        out << R"({"session":"s-old","principal":")" << kPrincipal << R"(","acked":{"history":4293}})" << "\n";
    }
    const std::string a = "ws://hostone:7777/ws?format=json";
    const std::string b = "ws://vantage:7777/ws";
    CHECK(FileSessionStore::worldKey(a) == "ws://hostone:7777/ws"); // the query is not part of the key

    FileSessionStore storeA(file, a);
    auto loadedA = storeA.load();
    REQUIRE(loadedA.has_value()); // the single-entry file was this machine's one world
    CHECK(loadedA->session == "s-old");
    CHECK(loadedA->acked.at("history") == 4293);

    FileSessionStore storeB(file, b);
    CHECK_FALSE(storeB.load().has_value()); // another world starts fresh, it never inherits A's acked seq

    storeA.save(*loadedA);                                      // rewritten in the per-world shape
    storeB.save(Persisted{"s-b", std::string(kPrincipal), {{"history", 7}}});
    CHECK(FileSessionStore(file, a).load()->session == "s-old"); // both survive side by side
    CHECK(FileSessionStore(file, a).load()->acked.at("history") == 4293);
    CHECK(FileSessionStore(file, b).load()->session == "s-b");
    CHECK(FileSessionStore(file, b).load()->acked.at("history") == 7);
    {
        std::ifstream in(file);
        const json j = json::parse(in);
        CHECK(j.contains("worlds"));
        CHECK_FALSE(j.contains("session")); // the old top-level entry is gone
    }
    // No world key: the single-entry shape, as before (tools and tests that do not name a world).
    FileSessionStore plain(dir / "plain.json");
    plain.save(Persisted{"s-p", std::nullopt, {{"history", 1}}});
    CHECK(plain.load()->session == "s-p");
    std::filesystem::remove_all(dir);
}

TEST_CASE("a push's from above our cursor is a filtered hole across pushes, never a red gap", "[session][store]") {
    Harness h;
    h.session->onOpen();
    h.answerAttach("s-01", false, 42);
    h.answerSubscribe(42);
    h.push(43, 44, 44, false, 42);
    // the world read from our cursor (44) and 45..46 were not ours: a hole inside the push, dim
    h.push(47, 47, 47, false, 44);
    CHECK(h.gaps.empty());
    CHECK(h.events.holes().size() == 1);
    // the world's cursor moved to 50 while we hold 47: the world read 48..50, matched none of it
    // for us and advanced without pushing (pump-sub keeps the last seq READ). A hole, dim.
    h.push(51, 51, 51, false, 50);
    CHECK(h.gaps.empty());
    REQUIRE(h.events.holes().size() == 2);
    CHECK(h.events.holes()[1].expected == 48);
    CHECK(h.events.holes()[1].got == 51);
    // the next push continues from 51: no new hole
    h.push(52, 52, 52, false, 51);
    CHECK(h.events.holes().size() == 2);
    // the one fault the client can still see: the world's head below our cursor
    h.push(53, 53, 10, false, 52);
    REQUIRE(h.gaps.size() == 1);
    CHECK(h.gaps[0].got == 10);
}
