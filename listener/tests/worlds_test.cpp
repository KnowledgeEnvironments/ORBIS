// Several worlds (sprint 2): the per-world token book, the pairing link, and the Session's
// `hello` handshake that names the world and starts fresh when the id under a URL changes.
#include "app/Worlds.hpp"
#include "net/Session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace ke;

namespace {

std::filesystem::path tempDir() {
    char tmpl[] = "/tmp/orbis-worlds-XXXXXX";
    const char* d = mkdtemp(tmpl);
    REQUIRE(d != nullptr);
    return std::filesystem::path(d);
}

} // namespace

TEST_CASE("the pairing link round-trips url and code") {
    const std::string link = app::makePairLink("ws://192.0.2.10:7777/ws", "482913");
    CHECK(link == "orbis://pair?url=ws%3A%2F%2F192.0.2.10%3A7777%2Fws&code=482913");
    const app::PairLink p = app::parsePairLink(link);
    CHECK(p.valid());
    CHECK(p.url == "ws://192.0.2.10:7777/ws");
    CHECK(p.code == "482913");
    CHECK_FALSE(app::parsePairLink("https://example.com/?code=1").valid());
    CHECK_FALSE(app::parsePairLink("orbis://pair?url=ws%3A%2F%2Fx").valid());
}

TEST_CASE("the world book keeps one token per world url, owner-only, and lists known worlds") {
    const std::filesystem::path dir = tempDir();
    const std::filesystem::path sessionFile = dir / "session.json";
    {
        // two worlds in session.json, one attached more recently
        std::ofstream out(sessionFile);
        out << R"({"worlds": {
            "ws://hostone:7777/ws": {"session": "s-1", "principal": "p-1", "acked": {}, "world": "w-hostone", "world-name": "hostone", "last-attached": "2026-10-30T20:00:00Z"},
            "ws://vantage:7777/ws": {"session": "s-2", "principal": "p-2", "acked": {}, "world": "w-vantage", "world-name": "vantage", "last-attached": "2026-10-30T21:00:00Z"}
        }})";
    }
    app::WorldBook book(sessionFile);
    CHECK_FALSE(book.tokenFor("ws://hostone:7777/ws?format=json").has_value());
    REQUIRE(book.remember("ws://hostone:7777/ws?format=json", "tok-hostone"));
    REQUIRE(book.tokenFor("ws://hostone:7777/ws").value_or("") == "tok-hostone");
    struct stat st{};
    REQUIRE(::stat(book.tokensFile().c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    const std::vector<app::KnownWorld> worlds = book.list("ws://modelhost:7777/ws?format=json");
    REQUIRE(worlds.size() == 3);
    CHECK(worlds[0].url == "ws://modelhost:7777/ws"); // the current one first, even when unknown
    CHECK(worlds[1].name == "vantage");             // then most recently attached
    CHECK(worlds[2].name == "hostone");
    CHECK(worlds[2].hasToken);
    CHECK_FALSE(worlds[1].hasToken);
    CHECK(book.forget("ws://hostone:7777/ws"));
    CHECK_FALSE(book.tokenFor("ws://hostone:7777/ws").has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("hello names the world; a changed id under the url starts a fresh session") {
    MemorySessionStore store;
    EventStore events;
    std::vector<Frame> sent;
    SessionConfig cfg;
    cfg.token = "t";
    cfg.hello = true;
    auto requests = [&] {
        std::vector<Request> out;
        for (const Frame& f : sent) {
            if (auto* r = std::get_if<Request>(&f)) {
                out.push_back(*r);
            }
        }
        return out;
    };
    {
        Session s(cfg, store, events, [&](const Frame& f) { sent.push_back(f); });
        s.onOpen();
        REQUIRE(requests().size() == 1);
        CHECK(requests()[0].op == "hello");
        s.onFrame(Response{requests()[0].id, true,
                           json{{"world", "w-aaaaaaaa-1"}, {"name", "hostone"}, {"url", "ws://hostone:7777/ws"}, {"head", 40},
                                {"storage", {{"world-bytes", 12582912}, {"disk-total-bytes", 1000000000}, {"disk-free-bytes", 400000000}}}},
                           std::nullopt, 40});
        REQUIRE(requests().size() == 2);
        CHECK(requests()[1].op == "attach");
        CHECK_FALSE(requests()[1].args.contains("session")); // first time: fresh
        CHECK(s.world().name == "hostone");
        CHECK(s.world().head == 40);
        CHECK(s.world().worldBytes == 12582912);
        CHECK(s.world().diskTotalBytes == 1000000000);
        CHECK(s.world().diskFreeBytes == 400000000);
        s.onFrame(Response{requests()[1].id, true, json{{"session", "s-1"}, {"principal", "p-1"}, {"resumed", false}, {"seq", 41}},
                           std::nullopt, 41});
        REQUIRE(store.value.has_value());
        CHECK(store.value->world == "w-aaaaaaaa-1");
        CHECK(store.value->worldName == "hostone");
        CHECK_FALSE(store.value->lastAttached.empty());
        CHECK(s.freshReason().empty());
    }
    sent.clear();
    {
        // same store (same URL), another world answers hello: the saved session is not offered
        Session s(cfg, store, events, [&](const Frame& f) { sent.push_back(f); });
        s.onOpen();
        s.onFrame(Response{requests()[0].id, true, json{{"world", "w-bbbbbbbb-2"}, {"name", "scratch"}, {"head", 3}}, std::nullopt, 3});
        REQUIRE(requests().size() == 2);
        CHECK(requests()[1].op == "attach");
        CHECK_FALSE(requests()[1].args.contains("session"));
        CHECK_FALSE(s.freshReason().empty());
        CHECK(s.persisted().session.empty());
    }
    sent.clear();
    {
        // a world without the op: err, then the attach goes ahead with the saved session
        store.value = Persisted{"s-9", std::string("p-1"), {{"history", 7}}, "w-aaaaaaaa-1", "hostone", ""};
        Session s(cfg, store, events, [&](const Frame& f) { sent.push_back(f); });
        s.onOpen();
        s.onFrame(Response{requests()[0].id, false, json(), Err{"not-implemented", "no hello here", std::nullopt}, 0});
        REQUIRE(requests().size() == 2);
        CHECK(requests()[1].op == "attach");
        CHECK(requests()[1].args.value("session", "") == "s-9");
        CHECK(s.world().id.empty());
    }
}
