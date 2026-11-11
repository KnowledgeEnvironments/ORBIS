// Tokens as a fold (sprint 2): created token Objects, their later Fields, the snapshot seed, and
// the label reaching the naming rule.
#include "model/Snapshot.hpp"
#include "model/Tokens.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::model;

namespace {

constexpr const char* kRoot = "034968ad-3358-4e57-a31b-d1bde0c15b31";
constexpr const char* kPerson = "28b70d40-2916-4adb-967a-2dea363599dc";
constexpr const char* kTok1 = "aa000000-0000-4000-8000-000000000001";
constexpr const char* kTok2 = "aa000000-0000-4000-8000-000000000002";

Event ev(Seq seq, const char* kind, const char* stream, const char* actor, json body) {
    Event e;
    e.seq = seq;
    e.id = "0b5a2c3e-3333-4c2a-8f1e-" + std::to_string(100000000000 + seq);
    e.stream = stream;
    e.actor = actor;
    e.kind = kind;
    e.at = "2026-10-30T22:" + std::string(seq % 60 < 10 ? "0" : "") + std::to_string(seq % 60) + ":00.000Z";
    e.body = std::move(body);
    return e;
}

} // namespace

TEST_CASE("token Objects fold into DEVICES rows; a later revoke lands; labels reach nameOf") {
    Tokens tokens;
    std::map<std::string, std::string> labels;
    tokens.onLabel = [&](const std::string& id, const std::string& label) { labels[id] = label; };
    REQUIRE(tokens.apply(ev(10, "created", kTok1, kRoot,
                            {{"class", "object"},
                             {"fields",
                              {{"token/principal", kPerson},
                               {"token/hash", "sha256:abc"},
                               {"token/label", "hostone desktop"},
                               {"token/issued-by", kRoot},
                               {"token/expires-at", "2027-01-01T00:00:00.000Z"}}}})));
    REQUIRE_FALSE(tokens.apply(ev(11, "created", "bb000000-0000-4000-8000-000000000009", kRoot,
                                  {{"class", "object"}, {"fields", {{"file/name", "x"}}}}))); // not a token
    REQUIRE(tokens.apply(ev(12, "created", kTok2, kRoot,
                            {{"class", "object"},
                             {"fields", {{"token/principal", kPerson}, {"token/hash", "sha256:def"}, {"token/label", "laptop"}}}})));
    REQUIRE(tokens.apply(ev(13, "field-set", kTok2, kPerson,
                            {{"name", "token/last-used"}, {"type", "instant"}, {"value", "2026-10-30T22:13:00.000Z"}})));
    REQUIRE(tokens.apply(ev(14, "field-set", kTok1, kRoot,
                            {{"name", "token/revoked-at"}, {"type", "instant"}, {"value", "2026-10-30T22:14:00.000Z"}})));
    const std::vector<const TokenInfo*> rows = tokens.forPrincipal(kPerson);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]->id == kTok1);
    CHECK(rows[0]->label == "hostone desktop");
    CHECK(rows[0]->revoked());
    CHECK(rows[0]->state(1.8e9) == "revoked");
    CHECK(rows[1]->label == "laptop");
    CHECK(rows[1]->lastUsed == "2026-10-30T22:13:00.000Z");
    CHECK(rows[1]->state(1.8e9) == "live");
    CHECK(labels[kTok1] == "hostone desktop");
    CHECK(labels[kTok2] == "laptop");
    CHECK(tokens.forPrincipal(kRoot).empty());

    SECTION("an expiry in the past reads expired") {
        Tokens t2;
        t2.apply(ev(20, "created", kTok1, kRoot,
                    {{"class", "object"},
                     {"fields", {{"token/principal", kPerson}, {"token/label", "old"}, {"token/expires-at", "2020-01-01T00:00:00.000Z"}}}}));
        CHECK(t2.get(kTok1)->state(1.8e9) == "expired");
    }

    SECTION("a token/* field-set on an unseen Object starts the row (Grant came later)") {
        Tokens t3;
        REQUIRE(t3.apply(ev(30, "field-set", kTok2, kRoot, {{"name", "token/label"}, {"type", "string"}, {"value", "late"}})));
        REQUIRE(t3.get(kTok2) != nullptr);
        CHECK(t3.get(kTok2)->label == "late");
    }
}

TEST_CASE("a snapshot seeds the token fold as the events would") {
    Tokens fromEvents;
    fromEvents.apply(ev(10, "created", kTok1, kRoot,
                        {{"class", "object"},
                         {"fields", {{"token/principal", kPerson}, {"token/hash", "sha256:abc"}, {"token/label", "hostone desktop"}}}}));
    fromEvents.apply(ev(14, "field-set", kTok1, kRoot,
                        {{"name", "token/revoked-at"}, {"type", "instant"}, {"value", "2026-10-30T22:14:00.000Z"}}));
    const json obj = {
        {"object/id", kTok1},
        {"object/class", "object"},
        {"object/stream", kTok1},
        {"object/created-seq", 10},
        {"object/created-at", "2026-10-30T22:10:00.000Z"},
        {"object/fields",
         {{"token/principal", {{"field/value", kPerson}, {"field/type", "ref"}, {"field/set-seq", 10}}},
          {"token/hash", {{"field/value", "sha256:abc"}, {"field/type", "string"}, {"field/set-seq", 10}}},
          {"token/label", {{"field/value", "hostone desktop"}, {"field/type", "string"}, {"field/set-seq", 10}}},
          {"token/revoked-at", {{"field/value", "2026-10-30T22:14:00.000Z"}, {"field/type", "instant"}, {"field/set-seq", 14}}}}}};
    Tokens seeded;
    seeded.seed(SnapshotObject::parse(obj));
    const TokenInfo* a = fromEvents.get(kTok1);
    const TokenInfo* b = seeded.get(kTok1);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->principal == b->principal);
    CHECK(a->label == b->label);
    CHECK(a->revokedAt == b->revokedAt);
    CHECK(a->created == b->created);
    CHECK(seeded.forPrincipal(kPerson).size() == 1);
}
