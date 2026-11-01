// Local search over the folds (the fallback while a world has no `search` op) and the parsing of
// the world's hits: both land in SearchHit with the match offsets the pane inverts.
#include "chat/Search.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::chat;
using namespace ke::model;

namespace {

const char* kRoot = "11111111-0000-4000-8000-000000000001";
const char* kPerson = "11111111-0000-4000-8000-000000000002";
const char* kMsg = "22222222-0000-4000-8000-000000000001";

Event ev(Seq seq, const char* kind, const char* stream, const char* actor, json body) {
    Event e;
    e.seq = seq;
    e.kind = kind;
    e.stream = stream;
    e.actor = actor;
    e.body = std::move(body);
    e.id = "e-" + std::to_string(seq);
    e.at = "2026-10-30T20:00:00.000Z";
    return e;
}

Principals fold() {
    Principals p;
    p.apply(ev(1, "created", kRoot, kRoot, {{"class", "principal"}, {"fields", {{"principal/driven-by", "process"}}}}));
    p.apply(ev(2, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "root"}, {"object", kRoot}}));
    p.apply(ev(3, "created", kPerson, kRoot, {{"class", "principal"}, {"fields", {{"principal/driven-by", "person"}}}}));
    p.apply(ev(4, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "person"}, {"object", kPerson}}));
    p.apply(ev(5, "field-set", kRoot, kRoot,
               {{"name", "memory/notes"}, {"type", "string"},
                {"value", "The user's desktop is called vantage and runs the second world.\nNothing else yet."},
                {"reader", "both"}}));
    p.apply(ev(6, "field-set", kPerson, kRoot,
               {{"name", "instructions/charter"}, {"type", "string"}, {"value", "You are a person; ask root."}, {"reader", "both"}}));
    return p;
}

} // namespace

TEST_CASE("search: snippets carry the match offsets, whole words score higher", "[search]") {
    SearchHit h = snippetOf("alpha beta gamma delta epsilon", "gamma", 6);
    CHECK(h.matchStart != std::string::npos);
    CHECK(h.snippet.substr(h.matchStart, h.matchLen) == "gamma");
    CHECK(h.score == 2.0);
    CHECK(h.snippet.rfind("\xe2\x80\xa6", 0) == 0); // clipped on the left
    SearchHit sub = snippetOf("eclipsed", "clips");
    CHECK(sub.score == 1.0);
    CHECK(sub.snippet.substr(sub.matchStart, sub.matchLen) == "clips");
    SearchHit none = snippetOf("nothing here", "zzz");
    CHECK(none.matchStart == std::string::npos);
    SearchHit ci = snippetOf("Vantage runs the world", "VANTAGE");
    CHECK(ci.matchStart == 0);
    CHECK(ci.snippet.substr(0, 7) == "Vantage"); // the original case is kept
}

TEST_CASE("search: local hits over Fields, names and Messages, newest first, limited", "[search]") {
    Principals p = fold();
    MessageCache cache;
    cache.request(kMsg);
    cache.onRead(kMsg, json{{"message/from", kPerson}, {"message/text", "What is vantage?"}, {"message/route", "r"},
                           {"message/to", json::array({kRoot})}});
    // seq comes from the event; a read alone leaves 0, which sorts last
    std::vector<SearchHit> hits = localSearch(p, cache, "vantage");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].object == kRoot); // the Field at seq 5 is newer than the Message at seq 0
    CHECK(hits[0].field == "memory/notes");
    CHECK(hits[0].kind == "field");
    CHECK(hits[0].name == "root");
    CHECK(hits[0].snippet.find("vantage") != std::string::npos);
    CHECK(hits[0].snippet.find('\n') == std::string::npos); // one line
    CHECK(hits[1].kind == "message");
    CHECK(hits[1].object == kMsg);
    CHECK(hits[1].name == "person");
    // A bound name is an object hit.
    std::vector<SearchHit> names = localSearch(p, cache, "pers");
    REQUIRE(!names.empty());
    bool objectHit = false;
    for (const SearchHit& h : names) {
        objectHit = objectHit || (h.kind == "object" && h.object == kPerson);
    }
    CHECK(objectHit);
    // Limit.
    CHECK(localSearch(p, cache, "e", 1).size() == 1);
    CHECK(localSearch(p, cache, "").empty());
}

TEST_CASE("search: the world's hits parse with their kinds and offsets", "[search]") {
    const json result = {{"total", 2},
                         {"hits", json::array({json{{"object", kRoot}, {"field", "memory/notes"}, {"name", "root"}, {"seq", 5},
                                                    {"snippet", "desktop is called vantage and"}, {"score", 1.5}, {"kind", "field"}},
                                               json{{"object", kMsg}, {"seq", 9}, {"snippet", "What is Vantage?"}}})}};
    std::vector<SearchHit> hits = parseWorldHits(result, "vantage");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].kind == "field");
    CHECK(hits[0].score == 1.5);
    CHECK(hits[0].snippet.substr(hits[0].matchStart, hits[0].matchLen) == "vantage");
    CHECK(hits[1].kind == "object"); // no field, no kind: an object hit
    CHECK(hits[1].seq == 9);
    CHECK(hits[1].snippet.substr(hits[1].matchStart, hits[1].matchLen) == "Vantage");
    CHECK(parseWorldHits(json{{"total", 0}}, "x").empty());
}
