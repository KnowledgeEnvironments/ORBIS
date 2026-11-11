#include <catch2/catch_test_macros.hpp>

#include "chat/Conversations.hpp"

using namespace ke;
using namespace ke::chat;
using nlohmann::json;

TEST_CASE("conversations read as rows") {
    const json result = {{"events", json::array()},
                         {"conversations", json::array({json{{"thread", "t1"}, {"route", "r"}, {"title", "Fix the Build"}, {"participants", {"person", "grid"}},
                                                             {"last-seq", 42}, {"messages", 7}, {"tokens-in", 100}, {"compressed", true}},
                                                        json{{"title", "no thread"}}})}};
    const auto rows = parseConversations(result);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].lastSeq == 42);
    CHECK(rows[0].compressed);
    CHECK(conversationMatches(rows[0], "build"));
    CHECK(conversationMatches(rows[0], "GRID"));
    CHECK_FALSE(conversationMatches(rows[0], "atlas"));
    CHECK(conversationMatches(rows[0], ""));
    CHECK(parseConversations(json{{"events", json::array()}}).empty());
}

TEST_CASE("directives complete while the line is one word") {
    CHECK(directiveMatches("/").size() == 3);
    REQUIRE(directiveMatches("/re").size() == 1);
    CHECK(std::string(directiveMatches("/re")[0].word) == "/resume");
    CHECK(directiveMatches("/resume abc").empty());
    CHECK(directiveMatches("hello").empty());
    CHECK(directiveMatches("/Users/x").empty());
}

TEST_CASE("the bar warns near compression") {
    CHECK(compressAt(0, 10000) == 7000);
    CHECK(compressAt(5000, 10000) == 5000);
    CHECK(nearCompression(6400, 7000));
    CHECK_FALSE(nearCompression(5000, 7000));

    const ContextGauge g = contextGauge(60000, 70000, 100000);
    CHECK(g.known);
    CHECK(g.fill == 0.6f);
    CHECK(g.mark == 0.7f);
    CHECK_FALSE(g.near);
    CHECK(contextGauge(65000, 70000, 100000).near);
    CHECK(contextGauge(200000, 70000, 100000).fill == 1.0f);
    CHECK_FALSE(contextGauge(4000, 3000, 0).known);
    CHECK_FALSE(nearCompression(5000, 0));
}
