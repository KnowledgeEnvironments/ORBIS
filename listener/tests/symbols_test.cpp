// The symbol matcher: spans over text from the fold alone (docs/listener-style.md, "Symbols").
#include "chat/FieldValue.hpp"
#include "chat/RichText.hpp"
#include "chat/Symbols.hpp"
#include "ui/Markdown.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke;
using namespace ke::chat;
using namespace ke::model;

namespace {

const char* kRoot = "11111111-aaaa-4000-8000-000000000001";
const char* kGrid = "22222222-aaaa-4000-8000-000000000002";
const char* kFs = "33333333-aaaa-4000-8000-000000000003";
const char* kRoute = "44444444-aaaa-4000-8000-000000000004";
const char* kTwinA = "55555555-aaaa-4000-8000-000000000005";
const char* kTwinB = "55555555-bbbb-4000-8000-000000000006";
const char* kMsg = "66666666-aaaa-4000-8000-000000000007";

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

json principal(const char* drivenBy) { return json{{"class", "principal"}, {"fields", {{"principal/driven-by", drivenBy}}}}; }

Principals fold() {
    Principals p;
    Seq s = 1;
    auto bind = [&](const char* id, const char* name) {
        p.apply(ev(s++, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", name}, {"object", id}}));
    };
    p.apply(ev(s++, "created", kRoot, kRoot, principal("process")));
    bind(kRoot, "root");
    p.apply(ev(s++, "created", kGrid, kRoot, principal("process")));
    bind(kGrid, "grid");
    p.apply(ev(s++, "created", kFs, kRoot, {{"class", "adapter"}, {"fields", json::object()}}));
    bind(kFs, "grid.fs");
    p.apply(ev(s++, "created", kRoute, kRoot, {{"class", "route"}, {"fields", json::object()}}));
    bind(kRoute, "root.route");
    p.apply(ev(s++, "created", kTwinA, kRoot, principal("none")));
    p.apply(ev(s++, "created", kTwinB, kRoot, principal("none")));
    p.apply(ev(s++, "field-set", kRoot, kRoot,
               {{"name", "loop/stalled"}, {"type", "string"}, {"value", ""}, {"reader", "both"}}));
    p.apply(ev(s++, "sent", kRoot, kRoot, {{"message", kMsg}, {"route", kRoute}}));
    return p;
}

std::string at(std::string_view text, const Symbol& s) { return std::string(text.substr(s.start, s.end - s.start)); }

} // namespace

TEST_CASE("dotted and slashed names link; a word in prose does not") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    const std::string text = "Ask root on root.route, then read grid.fs/README.md for grid.";
    const auto spans = t.scan(text);
    REQUIRE(spans.size() == 2);
    CHECK(at(text, spans[0]) == "root.route");
    CHECK(spans[0].kind == SymbolKind::Object);
    CHECK(spans[0].object == kRoute);
    CHECK(at(text, spans[1]) == "grid.fs/README.md");
    CHECK(spans[1].kind == SymbolKind::Path);
    CHECK(spans[1].object == kFs);
    CHECK(spans[1].path == "README.md");
}

TEST_CASE("the client never guesses at a file: a path with no mount is the world's to mark") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    CHECK(t.scan("see README.md:7-15 and example.com:443 and docs/a.md:30").empty());
}

TEST_CASE("a mount's citation keeps its line") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    const std::string text = "read grid.fs/README.md:12 now";
    const auto spans = t.scan(text);
    REQUIRE(spans.size() == 1);
    CHECK(at(text, spans[0]) == "grid.fs/README.md:12");
    CHECK(spans[0].path == "README.md");
    CHECK(spans[0].line == 12);
}

TEST_CASE("single-word names link in a code span") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    ScanContext code;
    code.code = true;
    const auto spans = t.scan("root", code);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].object == kRoot);
    CHECK(t.scan("root").empty());
}

TEST_CASE("a sentence's full stop is not part of a name") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    const std::string text = "It lives on root.route.";
    const auto spans = t.scan(text);
    REQUIRE(spans.size() == 1);
    CHECK(at(text, spans[0]) == "root.route");
}

TEST_CASE("a full id displays by name; a hex prefix links only when unique") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    const std::string text = std::string("see ") + kGrid + " and 22222222 and 55555555";
    const auto spans = t.scan(text);
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].display == "grid");
    CHECK(spans[0].object == kGrid);
    CHECK(spans[1].display == "22222222");
    CHECK(spans[1].object == kGrid);
}

TEST_CASE("an id that matches nothing is unresolved where an id belongs, plain elsewhere") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    const auto mistyped = t.scan("message 0fe4c2fd said so");
    REQUIRE(mistyped.size() == 1);
    CHECK(mistyped[0].kind == SymbolKind::Unresolved);
    REQUIRE(t.scan("the build (0fe4c2fd)").size() == 1);
    CHECK(t.scan("commit 0fe4c2fd").empty());
    CHECK(t.scan("message 55555555").empty()); // ambiguous is not mistyped
}

TEST_CASE("a reference tag displays as its name, a half-copied one too") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    const std::string full = std::string("[[") + kRoute + "|root.route|route]]";
    auto spans = t.scan(full);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].start == 0);
    CHECK(spans[0].end == full.size());
    CHECK(spans[0].display == "root.route");
    const std::string bare = std::string("[[") + kRoute + "]]";
    spans = t.scan(bare);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].display == "root.route");
    const auto gone = t.scan("[[99999999-aaaa-4000-8000-000000000009|secret|principal]]");
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].kind == SymbolKind::Unresolved);
    CHECK(t.scan("[[bytes 12 abcdef]]").empty());
}

TEST_CASE("a Field links on the object in context") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    ScanContext ctx;
    ctx.fold = &p;
    ctx.object = kRoot;
    const auto spans = t.scan("see loop/stalled and loop/motto", ctx);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].kind == SymbolKind::Field);
    CHECK(spans[0].field == "loop/stalled");
    CHECK(spans[0].object == kRoot);
}

TEST_CASE("seq N links to its row") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    const std::string text = "written at seq 680, then seq=12 and sequence 9";
    const auto spans = t.scan(text);
    REQUIRE(spans.size() == 2);
    CHECK(at(text, spans[0]) == "seq 680");
    CHECK(spans[0].seq == 680);
    CHECK(spans[1].seq == 12);
}

TEST_CASE("what is outside the fold stays plain") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    CHECK(t.scan("atlas.fs/ARCHITECTURE.md and atlas.route").empty());
}

TEST_CASE("the table rebuilds only when something new appears") {
    Principals p = fold();
    SymbolTable t;
    CHECK(t.refresh(p));
    const auto v = t.version();
    CHECK_FALSE(t.refresh(p));
    CHECK(t.version() == v);
    p.apply(ev(100, "field-set", kRoot, kRoot, {{"name", "x/y"}, {"type", "string"}, {"value", "z"}, {"reader", "both"}}));
    CHECK_FALSE(t.refresh(p));
    p.apply(ev(101, "bound", kRoot, kRoot, {{"namespace", kRoot}, {"name", "new.name"}, {"object", kTwinA}}));
    CHECK(t.refresh(p));
    CHECK(t.byName("new.name") != nullptr);
}

TEST_CASE("completion ranks Principals first") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    const auto hits = t.complete("gr");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0]->name == "grid");
    CHECK(hits[1]->name == "grid.fs");
    CHECK(t.complete("GRID.").size() == 1);
    CHECK(t.complete("nothing").empty());
}

TEST_CASE("a typed value is checked before it is sent") {
    const Principals p = fold();
    SymbolTable t;
    t.refresh(p);
    CHECK(parseFieldValue("number", "42.5", &t).value == 42.5);
    CHECK_FALSE(parseFieldValue("number", "4x", &t).ok);
    CHECK(parseFieldValue("boolean", "true", &t).value == true);
    CHECK_FALSE(parseFieldValue("boolean", "yes", &t).ok);
    CHECK(parseFieldValue("keyword", ":on", &t).value == "on");
    CHECK_FALSE(parseFieldValue("keyword", "two words", &t).ok);
    CHECK(parseFieldValue("instant", "2026-10-30T20:00:00.123Z", &t).ok);
    CHECK_FALSE(parseFieldValue("instant", "yesterday", &t).ok);
    const ParsedField ref = parseFieldValue("ref", "@root.route", &t);
    REQUIRE(ref.ok);
    CHECK(ref.value == kRoute);
    CHECK(ref.type == json::array({"ref", "route"}));
    CHECK_FALSE(parseFieldValue("ref", "nowhere", &t).ok);
    CHECK(parseFieldValue("list", "[\"a\", \"b\"]", &t).type == json::array({"list", "string"}));
    CHECK(parseFieldValue("list", "[1, \"b\"]", &t).type == "list");
    CHECK_FALSE(parseFieldValue("list", "{\"a\": 1}", &t).ok);
    CHECK_FALSE(parseFieldValue("map", "[1]", &t).ok);
    CHECK_FALSE(parseFieldValue("map", "{bad", &t).ok);
}

TEST_CASE("a Field's name says what the world does with it") {
    CHECK(fieldNameHint("instructions/own").find("system prompt") != std::string::npos);
    CHECK(fieldNameHint("memory/project").find("always in the prompt") != std::string::npos);
    CHECK(fieldNameHint("loop/stalled/hint").find("one-line comment") != std::string::npos);
    CHECK(fieldNameHint("notes/today").empty());
}

TEST_CASE("absolute paths and mount paths link as files", "[symbols]") {
    model::Principals fold;
    chat::SymbolTable table;
    table.refresh(fold);
    const auto hits = table.scan("see /path/to/notes.md, and/or / and /usr/bin/vim.");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].kind == chat::SymbolKind::Path);
    CHECK(hits[0].path == "/path/to/notes.md");
    CHECK(hits[0].object.empty());
    CHECK(hits[1].path == "/usr/bin/vim");
    CHECK(table.scan("a/b and http://x.y/z/q").empty());
}

TEST_CASE("references are spans in code points, and survive Markdown", "[richtext]") {
    using nlohmann::json;
    const std::string text = "日本語 **ask ivy** about seq 12 and `ivy`\n\n| a |\n|---|\n| ivy |\n";
    const json refs = json::array({json{{"start", 10}, {"end", 13}, {"kind", "object"}, {"object", "ID"}, {"name", "ivy"}},
                                   json{{"start", 26}, {"end", 28}, {"kind", "seq"}, {"seq", 12}},
                                   json{{"start", 999}, {"end", 1000}, {"kind", "object"}, {"object", "X"}}});
    const std::vector<RefSpan> spans = parseRefs(refs, text);
    {
        const std::string cited = "see README.md:7-15 now";
        const auto path = parseRefs(json::array({json{{"start", 4}, {"end", 18}, {"kind", "path"}, {"object", "m"},
                                                      {"path", "README.md"}, {"line", 7}}}), cited);
        REQUIRE(path.size() == 1);
        const Symbol s = symbolOf(path[0], cited, "");
        CHECK(s.kind == SymbolKind::Path);
        CHECK(s.object == "m");
        CHECK(s.path == "README.md");
        CHECK(s.line == 7);
        CHECK(s.display == "README.md:7-15");
    }
    REQUIRE(spans.size() == 2);
    CHECK(text.substr(spans[0].start, spans[0].end - spans[0].start) == "ivy");
    CHECK(text.substr(spans[1].start, spans[1].end - spans[1].start) == "12");

    ui::SelDoc doc;
    ui::MdExtras md;
    md.refs = &spans;
    ui::appendMarkdown(doc, text, 0, 0, md, 7.0f);
    doc.finish();
    REQUIRE(md.symbols.size() == 2);
    CHECK(md.symbols[0].symbol.object == "ID");
    CHECK(md.symbols[0].symbol.display == "ivy"); // the name it was written with: nothing else knows it
    CHECK(md.symbols[1].symbol.kind == SymbolKind::Seq);
    CHECK(md.symbols[1].symbol.seq == 12);
    // The run is where the bold put it: the mark carried through the transform.
    const ui::SelPara& first = doc.paras[md.symbols[0].para];
    CHECK(first.runs[md.symbols[0].run].text == "ivy");
    CHECK(doc.all.find("\xee") == std::string::npos); // no private mark is left behind
    CHECK(first.source.find("ask ivy") != std::string::npos);
}

TEST_CASE("text with a refs Field is never matched; text without one is", "[richtext]") {
    Principals fold;
    SymbolTable table;
    fold.alias("11111111-2222-4333-8444-555555555555", "shown");
    table.refresh(fold);
    const std::string text = "see /path/to/a.md here";
    ui::SymbolScan scan;
    scan.table = &table;

    ui::SelDoc plain;
    ui::MdExtras fallback;
    fallback.scan = &scan;
    ui::appendLinked(plain, text, 0, 0, fallback);
    CHECK(fallback.symbols.size() == 1); // the matcher finds the path

    ui::SelDoc tagged;
    ui::MdExtras given;
    given.scan = &scan;
    const std::vector<RefSpan> none;
    given.refs = &none;
    ui::appendLinked(tagged, text, 0, 0, given);
    CHECK(given.symbols.empty()); // an empty refs Field means nothing links
}

TEST_CASE("an @ mention sends its exact span", "[richtext]") {
    const nlohmann::json refs = mentionRefs("héllo ivy, not ivysaur", {{"ID", "ivy"}, {"NONE", "gone"}});
    REQUIRE(refs.size() == 1);
    CHECK(refs[0]["start"] == 6);
    CHECK(refs[0]["end"] == 9);
    CHECK(refs[0]["object"] == "ID");
}
