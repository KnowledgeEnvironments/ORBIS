#include "ui/Markdown.hpp"
#include "ui/Theme.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

using namespace ke::ui;

namespace {
std::string joined(const SelPara& p) {
    return p.text();
}
} // namespace

TEST_CASE("inline runs: code, bold, italic, links; field markers stay plain", "[markdown]") {
    auto runs = parseInline("set `loop/motto` to **what is** and *maybe* see [spec](https://x.y/s) (message/text) seq 680 a_b_c x*y");
    REQUIRE(runs.size() >= 8);
    CHECK(runs[0].kind == MdInlineRun::Plain);
    CHECK(runs[1].kind == MdInlineRun::Code);
    CHECK(runs[1].text == "loop/motto");
    CHECK(runs[3].kind == MdInlineRun::Bold);
    CHECK(runs[3].text == "what is");
    CHECK(runs[5].kind == MdInlineRun::Italic);
    CHECK(runs[5].text == "maybe");
    CHECK(runs[7].kind == MdInlineRun::Link);
    CHECK(runs[7].text == "spec");
    CHECK(runs[7].url == "https://x.y/s");
    CHECK(runs.back().kind == MdInlineRun::Plain);
    CHECK(runs.back().text.find("(message/text) seq 680 a_b_c x*y") != std::string::npos);
}

TEST_CASE("inline nesting and bare urls", "[markdown]") {
    auto runs = parseInline("**bold with `code`** and a lone * star");
    REQUIRE(runs.size() >= 2);
    CHECK(runs[0].kind == MdInlineRun::Bold);
    CHECK(runs[0].text == "bold with `code`"); // v1: the outer bold wins, code inside is literal
    CHECK(runs.back().text.find("lone * star") != std::string::npos);
    auto bare = parseInline("see https://localhost:8000/v1/models.");
    REQUIRE(bare.size() == 3);
    CHECK(bare[1].kind == MdInlineRun::Link);
    CHECK(bare[1].text == "https://localhost:8000/v1/models"); // the period stays in the sentence
    CHECK(bare[2].text == ".");
}

TEST_CASE("fences, headings, lists, quotes, rules and a table become paragraphs", "[markdown]") {
    SelDoc doc; // no ImGui context: the cell width is passed in
    MdExtras md;
    const std::string src =
        "# Title here\n"
        "intro `x`\n"
        "- first item\n"
        "- second **item**\n"
        "1. one\n"
        "2. two\n"
        "> quoted\n"
        "---\n"
        "```clojure\n(defn f [x]\n  x)\n```\n"
        "| a | bb |\n|---|---|\n| 1 | 22 |\n"
        "tail";
    appendMarkdown(doc, src, Ink::primary, 7, md, 7.0f);
    doc.finish();
    REQUIRE(md.codeBlocks.size() == 1);
    CHECK(md.codeBlocks[0].lang == "clojure");
    CHECK(md.codeBlocks[0].source == "(defn f [x]\n  x)\n");
    CHECK(doc.paras[md.codeBlocks[0].firstPara].code);
    // a header row for the language tag and [ COPY ], then the two lines: the bracket never covers code
    CHECK(doc.paras[md.codeBlocks[0].firstPara].kind == 5);
    CHECK(md.codeBlocks[0].lastPara - md.codeBlocks[0].firstPara == 2);
    CHECK(joined(doc.paras[0]) == "Title here");
    CHECK(doc.paras[0].runs.back().color == Ink::strong);
    bool sawCode = false;
    for (const SelRun& r : doc.paras[1].runs) {
        if (r.style == SelRun::CodeBox) {
            sawCode = true;
            CHECK(r.color == Ink::inspected);
            CHECK(r.text == "x");
        }
    }
    CHECK(sawCode);
    CHECK(doc.paras[2].runs[0].text == "· ");
    CHECK(doc.paras[2].hang > 0.0f);
    CHECK(doc.paras[4].runs[0].text == "1. ");
    CHECK(doc.paras[6].quote);
    CHECK(doc.paras[7].kind == 4);
    const std::size_t tableRow = md.codeBlocks[0].lastPara + 1;
    CHECK(joined(doc.paras[tableRow]) == "a | bb");
    CHECK(joined(doc.paras[tableRow + 1]) == "------");
    CHECK(joined(doc.paras[tableRow + 2]) == "1 | 22");
    CHECK(doc.paras[0].source == "# Title here");
    CHECK(doc.blockText(7).find("**item**") != std::string::npos); // a block copy is the markdown
    CHECK(doc.blockText(7).find("```") == std::string::npos);      // the fences are not paragraphs
    CHECK(joined(doc.paras.back()) == "tail");
}

TEST_CASE("looksLikeMarkdown is a cheap gate", "[markdown]") {
    CHECK(looksLikeMarkdown("has `code`"));
    CHECK(looksLikeMarkdown("- item"));
    CHECK(looksLikeMarkdown("1. item"));
    CHECK(looksLikeMarkdown("# h"));
    CHECK_FALSE(looksLikeMarkdown("plain words (message/text) seq 680"));
    CHECK_FALSE(looksLikeMarkdown("2026-10-30 at 18:40 - fine"));
}
