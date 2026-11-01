// Every frame in docs/protocol-examples/*.json parses to the right kind and round-trips:
// parse -> emit -> parse gives an equal frame and equal JSON.
#include "net/Protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <filesystem>
#include <fstream>
#include <map>

using namespace ke;

namespace {

const std::map<std::string, FrameKind> kExpected = {
    {"ack.json", FrameKind::Ack},
    {"act-create.request.json", FrameKind::Request},
    {"attach.request.json", FrameKind::Request},
    {"attach.response.json", FrameKind::Response},
    {"err.json", FrameKind::Response},
    {"push.json", FrameKind::Push},
    {"reattach.request.json", FrameKind::Request},
    {"set-field.request.json", FrameKind::Request},
    {"subscribe.request.json", FrameKind::Request},
    {"subscribe.response.json", FrameKind::Response},
};

json load(const std::filesystem::path& p) {
    std::ifstream in(p);
    REQUIRE(in.good());
    return json::parse(in);
}

std::vector<std::string> exampleFiles() {
    std::vector<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(LISTENER_PROTOCOL_EXAMPLES_DIR)) {
        if (entry.path().extension() == ".json") {
            out.push_back(entry.path().filename().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("every protocol example has an expected kind", "[protocol]") {
    const auto files = exampleFiles();
    REQUIRE(files.size() == kExpected.size());
    for (const auto& f : files) {
        INFO(f);
        CHECK(kExpected.count(f) == 1);
    }
}

TEST_CASE("protocol examples parse to the right kind and round-trip", "[protocol]") {
    const std::string file = GENERATE(from_range(exampleFiles()));
    INFO(file);
    const json original = load(std::filesystem::path(LISTENER_PROTOCOL_EXAMPLES_DIR) / file);

    REQUIRE(kindOf(original) == kExpected.at(file));
    const Frame frame = parseFrame(original);
    REQUIRE(kindOf(frame) == kExpected.at(file));

    const json emitted = toJson(frame);
    CHECK(emitted == original);

    const Frame again = decode(encode(frame));
    CHECK(again == frame);
    CHECK(toJson(again) == original);
}

TEST_CASE("frame kind is decided by key presence", "[protocol]") {
    CHECK(kindOf(json{{"ack", "history"}, {"seq", 1}}) == FrameKind::Ack);
    CHECK(kindOf(json{{"sub", "history"}, {"events", json::array()}, {"as-of", 1}}) == FrameKind::Push);
    CHECK(kindOf(json{{"id", "r1"}, {"op", "detach"}, {"args", json::object()}}) == FrameKind::Request);
    CHECK(kindOf(json{{"id", "r1"}, {"ok", true}, {"result", json::object()}, {"seq", 1}}) ==
          FrameKind::Response);
    CHECK_THROWS_AS(kindOf(json{{"id", "r1"}}), ProtocolError);
    CHECK_THROWS_AS(kindOf(json::array()), ProtocolError);
    CHECK_THROWS_AS(decode("not json"), ProtocolError);
    CHECK_THROWS_AS(decode(R"({"ack":"history"})"), ProtocolError); // seq missing
}

TEST_CASE("push events keep optional keys only when present", "[protocol]") {
    const json original = load(std::filesystem::path(LISTENER_PROTOCOL_EXAMPLES_DIR) / "push.json");
    const Push p = std::get<Push>(parseFrame(original));
    REQUIRE(p.events.size() == 2);
    CHECK(p.sub == "history");
    CHECK(p.asOf == 44);
    CHECK_FALSE(p.lagging);
    CHECK(p.events[0].kind == "attached");
    CHECK(p.events[1].seq == 44);
    CHECK_FALSE(p.events[0].via.has_value());
    CHECK_FALSE(p.events[0].toJson().contains("event/via"));

    Event e = p.events[1];
    e.via = "0b5a2c3e-2222-4c2a-8f1e-000000000001";
    e.streams = {"0b5a2c3e-3333-4c2a-8f1e-000000000001"};
    CHECK(Event::fromJson(e.toJson()) == e);
}

TEST_CASE("error responses carry the enumerated code", "[protocol]") {
    const Response r = std::get<Response>(
        decode(R"({"id":"r3","ok":false,"err":{"code":"not-implemented","message":"nope"},"seq":9})"));
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.err.has_value());
    CHECK(r.err->code == "not-implemented");
    CHECK(r.seq == 9);
    CHECK(encode(errResponse("r3", "not-implemented", "nope", 9)) == encode(r));
}

TEST_CASE("a push keeps its from cursor and omits it when absent", "[protocol]") {
    Push p;
    p.sub = "history";
    p.asOf = 10;
    const json without = toJson(Frame{p});
    CHECK_FALSE(without.contains("from"));
    p.from = 7;
    const json with = toJson(Frame{p});
    REQUIRE(with.contains("from"));
    CHECK(with["from"] == 7);
    const Frame back = parseFrame(with);
    REQUIRE(std::holds_alternative<Push>(back));
    CHECK(std::get<Push>(back).from == 7);
}

TEST_CASE("pane frames: a whole screen, a delta, an exit, an edit", "[protocol]") {
    const auto full = std::get<PaneData>(decode(R"({"pane":"p","seq":3,"full":true,"cols":80,"rows":24,"data":"\u001b[2Jhi"})"));
    CHECK(full.full);
    CHECK(full.cols == 80);
    CHECK(full.data == "\x1b[2Jhi");
    const auto delta = std::get<PaneData>(decode(R"({"pane":"p","seq":4,"base":3,"data":"x"})"));
    REQUIRE(delta.base.has_value());
    CHECK(*delta.base == 3);
    CHECK_FALSE(delta.full);
    const auto gone = std::get<PaneData>(decode(R"({"pane":"p","seq":5,"exited":true,"exit":2})"));
    CHECK(gone.exited);
    CHECK(gone.exitCode == 2);
    const auto edit = std::get<PaneData>(decode(R"({"pane":"p","seq":6,"base":5,"edit":{"at":1,"del":0,"text":"a","who":"w","version":2}})"));
    CHECK(edit.edit["who"] == "w");
    CHECK(std::get<PaneData>(decode(encode(Frame{edit}))) == edit);
}
