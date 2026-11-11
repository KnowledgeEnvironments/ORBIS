// The queue strip's rows: what the world lists, then this listener's lines on their way.
#include "chat/PromptSections.hpp"
#include "chat/Queue.hpp"
#include "model/ScratchFold.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::chat;

TEST_CASE("a line sent while working shows at once and is not doubled when the world lists it") {
    MessageCache cache;
    Event sent;
    sent.seq = 1;
    sent.kind = "sent";
    sent.body = json{{"message", "m-1"}, {"route", "r"}};
    cache.noteEvent(sent);
    cache.request("m-1");
    cache.onRead("m-1", json{{"message/text", "second thought"}, {"message/from", "p"}});
    std::vector<PendingLine> pending{{"second thought", 0}, {"third thought", 0}};
    auto rows = queueRows(json(), cache, pending);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].pending);
    const json listed = json::array({json{{"message", "m-1"}, {"from", "p"}, {"at", "t"}, {"kind", "text"}}});
    rows = queueRows(listed, cache, pending);
    REQUIRE(rows.size() == 2);
    CHECK_FALSE(rows[0].pending);
    CHECK(rows[0].text == "second thought");
    CHECK(rows[1].pending);
    CHECK(pending.size() == 1);
}

TEST_CASE("a reply acknowledges a pending kernel command that never entered loop queue") {
    MessageCache cache;
    cache.onRead("old", json{{"message/text", "old reply"}, {"message/in-reply-to", "old-command"}, {"message/seq", 9}});
    std::vector<PendingLine> pending{{"/new", 10}};
    auto rows = queueRows(json::array(), cache, pending);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].pendingIndex == 0);

    cache.onRead("reply", json{{"message/text", "new conversation"}, {"message/in-reply-to", "command"}, {"message/seq", 11}});
    rows = queueRows(json::array(), cache, pending);
    CHECK(rows.empty());
    CHECK(pending.empty());
}

TEST_CASE("a queue row shows one line") {
    CHECK(firstLine("a\nb") == "a");
    CHECK(firstLine(std::string(100, 'x'), 10) == std::string(10, 'x') + "…");
}

TEST_CASE("a prompt reads as sections with their bytes") {
    const std::string serialized = json::array({json{{"role", "system"}, {"content", "You are root.\n\n# Acting\nYou may act.\n## principal root\n=== field instructions/charter (:string)\nBe good.\n=== field memory/notes (:string)\nRemember.\n"}},
                                                json{{"role", "user"}, {"content", "hello"}}}).dump();
    const auto sections = promptSections(serialized);
    std::vector<std::string> names;
    for (const auto& s : sections) {
        names.push_back(s.name);
    }
    CHECK(names == std::vector<std::string>{"identity", "acting", "objects", "instructions", "memory", "message"});
    CHECK(sections[3].text.find("Be good.") != std::string::npos);
    CHECK(sections[5].role == "user");
    CHECK(sections[5].bytes() == 6);
    const std::string cut = serialized.substr(0, serialized.find("Be good.") + 4) + "\n[... range truncated at 16384 bytes; ask for fewer lines]";
    const auto partial = promptSections(cut);
    REQUIRE(partial.size() == 4);
    CHECK(partial[3].name == "instructions");
    CHECK(partial[3].text.find("Be g") != std::string::npos);
    const auto plain = promptSections("not json");
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].name == "text");
}

namespace {
Event pushed(const char* kind, json body, Seq seq) {
    Event e;
    e.kind = kind;
    e.seq = seq;
    e.stream = "ev";
    e.actor = "grid";
    e.at = "2026-10-31T05:00:00.000Z";
    e.body = std::move(body);
    return e;
}
} // namespace

TEST_CASE("off the record folds from a pushed sequence, with the terminal's own counters") {
    model::ScratchFold fold;
    fold.setPrincipal("grid");
    fold.said("hello");
    CHECK(fold.answering());
    fold.apply(pushed("created", json{{"class", "evaluation"}, {"fields", {{"evaluation/initiator", "grid"}, {"evaluation/state", "queued"}, {"evaluation/model", "e"}}}}, 1));
    fold.apply(pushed("evaluated", json{{"state", "running"}, {"evaluation", "ev"}}, 2));
    fold.apply(pushed("evaluated", json{{"state", "streaming"}, {"evaluation", "ev"}, {"thinking-delta", "hmm"}, {"thinking-chars", 3}}, 3));
    CHECK(fold.thinking() == "hmm");
    CHECK(fold.progress("grid", 0.0).find("thinking") != std::string::npos);
    fold.apply(pushed("evaluated", json{{"state", "streaming"}, {"evaluation", "ev"}, {"delta", "My "}}, 4));
    fold.apply(pushed("evaluated", json{{"state", "streaming"}, {"evaluation", "ev"}, {"delta", "name"}}, 5));
    CHECK(fold.partial() == "My name");
    CHECK(fold.progress("grid", 0.0).find("replying") != std::string::npos);
    fold.apply(pushed("evaluated", json{{"state", "done"}, {"evaluation", "ev"}, {"cost", {{"tokens-in", 5}, {"tokens-out", 3}}},
                                        {"outcome", {{"text", "My name is grid"}, {"usage", {{"prompt-tokens", 5}, {"completion-tokens", 3}}}}}}, 6));
    REQUIRE(fold.lines().size() == 2);
    CHECK(fold.lines()[0].self);
    CHECK(fold.lines()[1].text == "My name is grid");
    CHECK(fold.partial().empty());
    CHECK_FALSE(fold.answering());
    CHECK(fold.tokensIn() == 5);
    CHECK(fold.tokensOut() == 3);
    CHECK(fold.costLine(fold.lines()[1]).find("5 in / 3 out") != std::string::npos);
    fold.said("again");
    fold.apply(pushed("evaluated", json{{"state", "failed"}, {"evaluation", "ev"}, {"error", "model down"}}, 7));
    CHECK(fold.lines().back().note);
    CHECK_FALSE(fold.answering());
    fold.clear();
    CHECK(fold.lines().empty());
    CHECK(fold.tokensIn() == 0);
}
