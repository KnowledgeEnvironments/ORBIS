// Thread grouping in the transcript (world pass 8: message/thread, message/asked-by, message/hop),
// tested on the pure functions under the terminal: no SDL, no ImGui.
#include "chat/Transcript.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::chat;

namespace {

TranscriptLine line(const char* id, const char* from, const char* fromName, Seq seq, const char* thread = "",
                    const char* askedBy = "", const char* askedByName = "", int hop = 0) {
    TranscriptLine l;
    l.message = id;
    l.from = from;
    l.fromName = fromName;
    l.seq = seq;
    l.thread = thread;
    l.askedBy = askedBy;
    l.askedByName = askedByName;
    l.hop = hop;
    l.ready = true;
    return l;
}

constexpr const char* kPerson = "p-person";
constexpr const char* kRoot = "p-root";
constexpr const char* kGrid = "p-grid";
constexpr const char* kM1 = "m-1"; // person -> root, the thread root
constexpr const char* kM2 = "m-2"; // root -> grid, hop 1, in the thread
constexpr const char* kM3 = "m-3"; // grid -> root, hop 2, in the thread
constexpr const char* kM4 = "m-4"; // root -> person, the relay
constexpr const char* kM5 = "m-5"; // an unrelated line with no thread

} // namespace

TEST_CASE("threads: lines sharing a message/thread form a group; singletons do not", "[threads]") {
    std::vector<TranscriptLine> lines = {
        line(kM1, kPerson, "person", 10, kM1, kPerson, "person"),
        line(kM5, kGrid, "grid", 12),
        line(kM2, kRoot, "root", 20, kM1, kPerson, "person", 1),
        line(kM3, kGrid, "grid", 30, kM1, kPerson, "person", 2),
        line(kM4, kRoot, "root", 40, kM1, kPerson, "person", 1),
    };
    const auto groups = threadGroups(lines);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].thread == kM1);
    CHECK(groups[0].askedBy == kPerson);
    CHECK(groups[0].askedByName == "person");
    CHECK(groups[0].lines == std::vector<std::size_t>{0, 2, 3, 4});

    SECTION("in root's terminal, grid's hop-2 answer is indented; root's and person's lines are not") {
        const auto places = threadPlaces(lines, groups, kRoot);
        REQUIRE(places.size() == 5);
        CHECK(places[0].group == 0);
        CHECK(places[0].first);
        CHECK(places[0].depth == 0);
        CHECK(places[1].group == -1); // the unrelated line
        CHECK(places[2].depth == 0);  // root -> grid: root is this terminal's principal
        CHECK(places[3].depth == 1);  // grid -> root: another Principal inside person's thread
        CHECK(places[4].depth == 0);
        CHECK(places[4].last);
    }
    SECTION("in grid's terminal, root is the other Principal and grid's own line stays flush") {
        const auto places = threadPlaces(lines, groups, kGrid);
        CHECK(places[2].depth == 1); // root -> grid: neither the asker nor this terminal's principal
        CHECK(places[3].depth == 0); // grid's own answer, hop 2 or not
        CHECK(places[4].depth == 1); // root's relay to person, seen from grid
    }
}

TEST_CASE("threads: a line without message/thread is its own root when others reply into it", "[threads]") {
    std::vector<TranscriptLine> lines = {
        line(kM1, kPerson, "person", 10),                               // an old Message, no thread Field yet
        line(kM2, kRoot, "root", 20, kM1, kPerson, "person", 1),        // a reply that names it as its thread
    };
    const auto groups = threadGroups(lines);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].thread == kM1);
    CHECK(groups[0].askedBy == kPerson);
    CHECK(groups[0].lines == std::vector<std::size_t>{0, 1});
}

TEST_CASE("threads: two singletons make no group", "[threads]") {
    std::vector<TranscriptLine> lines = {line(kM1, kPerson, "person", 10, kM1, kPerson, "person"),
                                         line(kM5, kGrid, "grid", 12, kM5, kGrid, "grid")};
    CHECK(threadGroups(lines).empty());
    const auto places = threadPlaces(lines, threadGroups(lines), kRoot);
    CHECK(places[0].group == -1);
    CHECK(places[1].group == -1);
}

TEST_CASE("threads: assembleThread gathers every participant's Messages of one thread, once, in log order", "[threads]") {
    // the fold's refs: person -> root (root of the thread), root -> grid (hop 1), grid -> root (hop 2,
    // seen on both streams), root -> person (the relay), and an unrelated line
    std::vector<model::MessageRef> refs;
    auto ref = [&](const char* m, const char* from, const char* to, Seq seq, const char* via = nullptr) {
        model::MessageRef r;
        r.message = m;
        r.from = from;
        r.recipient = to;
        r.seq = seq;
        r.at = "2026-10-30T20:00:00.000Z";
        if (via) {
            r.via = via;
        }
        refs.push_back(r);
    };
    ref(kM1, kPerson, kRoot, 10);
    ref(kM2, kRoot, kGrid, 20, "e-1");
    ref(kM3, kGrid, kRoot, 30, "e-2");
    ref(kM3, kGrid, "", 30, "e-2"); // the `sent` of the same Message
    ref(kM4, kRoot, kPerson, 40, "e-3");
    ref(kM5, kGrid, kRoot, 50);
    MessageCache cache;
    auto put = [&](const char* id, const char* from, const char* thread, int hop, const char* text) {
        cache.request(id);
        cache.onRead(id, json{{"message/from", from}, {"message/text", text}, {"message/thread", thread},
                              {"message/asked-by", kPerson}, {"message/hop", hop}});
    };
    put(kM1, kPerson, kM1, 0, "status of grid?");
    put(kM2, kRoot, kM1, 1, "grid, status?");
    put(kM3, kGrid, kM1, 2, "idle");
    put(kM4, kRoot, kM1, 1, "grid says idle");
    put(kM5, kGrid, kM5, 0, "unrelated");
    const auto lines = assembleThread(kM1, refs, cache, kPerson, [](const std::string& id) { return id.substr(2); });
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].message == kM1);
    CHECK(lines[0].mine); // person's own line, seen as person
    CHECK(lines[1].message == kM2);
    CHECK(lines[2].message == kM3);
    CHECK(lines[2].hop == 2);
    CHECK(lines[2].via == std::string("e-2"));
    CHECK(lines[3].message == kM4);
    CHECK(lines[3].askedByName == "person");
    for (const TranscriptLine& l : lines) {
        CHECK(l.thread == std::string(kM1));
    }
}

TEST_CASE("a compressed stretch is one span, counted, and only its own lines fall under it") {
    using namespace ke::chat;
    auto line = [](std::string id, std::string thread, ke::Seq seq) {
        TranscriptLine l;
        l.message = std::move(id);
        l.thread = std::move(thread);
        l.seq = seq;
        return l;
    };
    TranscriptLine root = line("t", "t", 10);
    root.summary = "they discussed the kettle";
    root.summaryFrom = 10;
    root.summaryThrough = 30;
    const std::vector<TranscriptLine> lines{root, line("a", "t", 20), line("b", "t", 30), line("c", "t", 31), line("x", "other", 25)};
    const auto spans = summarySpans(lines);
    REQUIRE(spans.size() == 1);
    CHECK(spans.at("t").messages == 3);
    CHECK(spanOf(lines[1], spans) != nullptr);
    CHECK(spanOf(lines[3], spans) == nullptr); // newer than the summary: stays whole
    CHECK(spanOf(lines[4], spans) == nullptr); // another conversation
}
