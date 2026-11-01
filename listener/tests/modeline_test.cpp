#include "model/Modeline.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

Event event(Seq seq, const std::string& kind, json body = json::object(), const std::string& stream = "a",
            const std::string& actor = "a") {
    Event e;
    e.seq = seq;
    e.kind = kind;
    e.stream = stream;
    e.actor = actor;
    e.body = std::move(body);
    return e;
}

} // namespace

TEST_CASE("a quiet world says idle") {
    Principals principals;
    CHECK(modeline({}, principals) == "idle");
}

TEST_CASE("reads and bookkeeping are not news") {
    Principals principals;
    CHECK(describeEvent(event(1, "field-read"), principals).empty());
    CHECK(describeEvent(event(2, "field-set", json{{"name", "cost/total"}}), principals).empty());
    CHECK(describeEvent(event(3, "field-set", json{{"name", "model/status"}}), principals).empty());
    CHECK(describeEvent(event(4, "evaluated", json{{"state", "running"}}), principals).empty());
}

TEST_CASE("the newest thing that happened is the line") {
    Principals principals;
    const std::vector<Event> rows = {event(1, "sent"), event(2, "field-set", json{{"name", "instructions/own"}}),
                                     event(3, "field-read")};
    const std::string line = modeline(rows, principals);
    CHECK(line.find("set instructions/own") != std::string::npos);
    CHECK(line.find("sent") == std::string::npos);
}
