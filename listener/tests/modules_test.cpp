// The module layer with no window: the registry, what a file opens in, the text buffer, listings,
// the math layout and the `$` rule.
#include "chat/Unsafe.hpp"
#include "model/Evaluations.hpp"
#include "modules/FileListing.hpp"
#include "modules/MathLayout.hpp"
#include "modules/Registry.hpp"
#include "modules/TexDocument.hpp"
#include "modules/TextBuffer.hpp"
#include "drive/Commands.hpp"
#include "ui/Fonts.hpp"
#include "ui/Markdown.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke::modules;

// The document's menu registers a drive target; a test binary has no drive.
namespace ke::ui {
bool menuItem(const char*, const char*, bool, bool) { return false; }
ImFont* monoFont() { return nullptr; }
float monoPixels() { return 14.0f; }
} // namespace ke::ui
namespace ke::drive {
bool active() { return false; }
void hit(const std::string&, ImVec2, ImVec2) {}
} // namespace ke::drive

namespace {
float mono(std::string_view s, float z) { return static_cast<float>(s.size()) * z * 0.6f; }
} // namespace

TEST_CASE("the registry decides what a file opens in", "[modules]") {
    CHECK(kindForPath("README.md") == "markdown");
    CHECK(kindForPath("/a/b/PAPER.TEX") == "latex");
    CHECK(kindForPath("shot.PNG") == "image");
    CHECK(kindForPath("main.cpp") == "editor");
    CHECK(kindForPath("Makefile") == "editor");
    CHECK(kindForPath("src/") == "files");
    REQUIRE(specFor("terminal") != nullptr);
    CHECK(specFor("nothing") == nullptr);
    CHECK(std::string(openWith("a.md").front()->kind) == "markdown");
    bool hasEditor = false, hasImage = false;
    for (const ModuleSpec* s : openWith("a.md")) {
        hasEditor = hasEditor || std::string_view(s->kind) == "editor";
        hasImage = hasImage || std::string_view(s->kind) == "image";
    }
    CHECK(hasEditor);
    CHECK_FALSE(hasImage);
}

TEST_CASE("click opens a file, shift always inspects", "[modules]") {
    CHECK(clickAction(true, false) == ClickAction::OpenModule);
    CHECK(clickAction(true, true) == ClickAction::Inspect);
    CHECK(clickAction(false, false) == ClickAction::Default);
    CHECK(clickAction(false, true) == ClickAction::Inspect);
}

TEST_CASE("text buffer edits, undoes and reports edits in code points", "[modules]") {
    TextBuffer b;
    b.setText("héllo\nworld");
    CHECK(b.lineCount() == 2);
    b.moveTo(TextPos{0, 7});
    CHECK(b.cursor().col == 6);
    b.insert("!");
    CHECK(b.text() == "héllo!\nworld");
    auto edits = b.takeEdits();
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].start == 5); // é is one code point
    CHECK(edits[0].end == 5);
    CHECK(b.dirty());
    REQUIRE(b.undo());
    CHECK(b.text() == "héllo\nworld");
    REQUIRE(b.redo());
    CHECK(b.text() == "héllo!\nworld");
    b.moveDocStart();
    b.moveDocEnd(true);
    CHECK(b.selectedText() == "héllo!\nworld");
    b.insert("x\ny\nz");
    CHECK(b.lineCount() == 3);
    b.undo();
    CHECK(b.lineCount() == 2);
}

TEST_CASE("typing in a run undoes as one step", "[modules]") {
    TextBuffer b;
    for (char c : std::string("abc")) {
        b.insert(std::string_view(&c, 1));
    }
    b.undo();
    CHECK(b.text().empty());
}

TEST_CASE("find wraps and counts", "[modules]") {
    TextBuffer b;
    b.setText("one two one\nthree one");
    CHECK(b.matchCount("one") == 3);
    REQUIRE(b.find("one"));
    CHECK(b.selectedText() == "one");
    CHECK(b.cursor() == TextPos{0, 3});
    b.find("one");
    CHECK(b.cursor() == TextPos{0, 11});
    b.find("one");
    CHECK(b.cursor().line == 1);
    b.find("one");
    CHECK(b.cursor() == TextPos{0, 3});
    CHECK_FALSE(b.find("absent"));
}

TEST_CASE("offsets and positions invert across lines and multibyte text", "[modules]") {
    TextBuffer b;
    b.setText("ab\n日本語\nz");
    const TextPos p{1, 6};
    CHECK(b.offsetOf(p) == 5);
    CHECK(b.posOf(5) == p);
}

TEST_CASE("a listing reads however the world says it", "[modules]") {
    using nlohmann::json;
    auto a = parseListing(json{{"entries", json::array({json{{"name", "b.md"}, {"size", 2048}, {"kind", "file"}},
                                                        json{{"name", "dir"}, {"kind", "dir"}}})}});
    REQUIRE(a.size() == 2);
    sortListing(a, FileSort::Name);
    CHECK(a[0].name == "dir");
    CHECK(formatSize(a[1].size) == "2.0K");
    CHECK(kindLabel(a[1]) == "markdown");
    auto b = parseListing(json{{"text", "src/\nREADME.md\n"}});
    REQUIRE(b.size() == 2);
    CHECK(b[0].directory());
    CHECK(parentPath("/a/b") == "/a");
    CHECK(parentPath("/a") == "/");
    CHECK(joinPath("/a/", "b") == "/a/b");
}

TEST_CASE("the panel keeps its place across a refresh", "[modules]") {
    FilePanel p;
    p.set({FileEntry{"a", 1, "", "file"}, FileEntry{"b", 1, "", "file"}});
    p.move(1);
    CHECK(p.current()->name == "b");
    p.set({FileEntry{"0", 1, "", "file"}, FileEntry{"a", 1, "", "file"}, FileEntry{"b", 1, "", "file"}});
    CHECK(p.current()->name == "b");
}

TEST_CASE("the dollar rule leaves prices and shell text alone", "[modules][math]") {
    const std::string_view npos_check = "x";
    (void)npos_check;
    CHECK(mathSpanClose("$x^2$ is", 0) == 4);
    CHECK(mathSpanClose("costs $5 and $6 today", 6) == std::string_view::npos); // closing $ before a digit
    CHECK(mathSpanClose("$ 5 and $", 0) == std::string_view::npos);            // a space after the opening
    CHECK(mathSpanClose("echo $HOME and $PATH ok", 5) == std::string_view::npos);
    CHECK(mathSpanClose("US$5 or $6$", 2) == std::string_view::npos);
    CHECK(mathSpanClose("$a$ and $b$", 8) == 10);
    auto runs = ke::ui::parseInline("area $\\pi r^2$ for $5 and $6");
    REQUIRE(runs.size() == 3);
    CHECK(runs[1].kind == ke::ui::MdInlineRun::Math);
    CHECK(runs[1].text == "$\\pi r^2$");
    CHECK(runs[2].text == " for $5 and $6");
}

TEST_CASE("math lays out with the structure TeX gives it", "[modules][math]") {
    const MathBox plain = layoutMath("x", 10.0f, mono, false);
    REQUIRE(plain.ok);
    const MathBox frac = layoutMath("\\frac{a}{b}", 10.0f, mono, true);
    REQUIRE(frac.ok);
    CHECK(frac.glyphs.size() == 2);
    CHECK(frac.rules.size() == 1);
    CHECK(frac.glyphs[0].y < frac.glyphs[1].y); // numerator above denominator
    CHECK(frac.height() > plain.height() * 1.5f);
    const MathBox sup = layoutMath("x^2", 10.0f, mono, false);
    REQUIRE(sup.glyphs.size() == 2);
    CHECK(sup.glyphs[1].y < sup.glyphs[0].y);
    CHECK(sup.glyphs[1].size < sup.glyphs[0].size);
    const MathBox sum = layoutMath("\\sum_{i=0}^{n} i", 10.0f, mono, true);
    REQUIRE(sum.ok);
    float top = 0.0f, bottom = 0.0f;
    for (const MathGlyph& g : sum.glyphs) {
        top = std::min(top, g.y);
        bottom = std::max(bottom, g.y);
    }
    CHECK(top < -5.0f);
    CHECK(bottom > 5.0f); // limits above and below in display
    const MathBox mat = layoutMath("\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}", 10.0f, mono, true);
    REQUIRE(mat.ok);
    CHECK(mat.glyphs.size() == 4);
    CHECK_FALSE(mat.rules.empty());
    CHECK(layoutMath("\\sqrt{x}", 10.0f, mono, false).rules.size() >= 4);
    CHECK(layoutMath("\\left(\\frac{a}{b}\\right)", 10.0f, mono, true).ok);
    CHECK(layoutMath("\\alpha + \\beta \\leq \\gamma", 10.0f, mono, false).glyphs.size() == 5);
}

TEST_CASE("math that does not parse says why", "[modules][math]") {
    const MathBox bad = layoutMath("\\nosuchcommand{x}", 10.0f, mono, false);
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("nosuchcommand") != std::string::npos);
    CHECK_FALSE(layoutMath("\\frac{a", 10.0f, mono, false).ok);
    CHECK_FALSE(layoutMath("\\begin{nothing}x\\end{nothing}", 10.0f, mono, false).ok);
}

TEST_CASE("markdown keeps math source in the document for copying", "[modules][math]") {
    ke::ui::SelDoc doc;
    ke::ui::MdExtras md;
    ke::ui::appendMarkdown(doc, "inline $x^2$ here\n\n$$\\frac{a}{b}$$\n\n```tex\n\\alpha\n```\n\n```tex\n\\documentclass{article}\n```\n",
                           0, 0, md, 7.0f);
    doc.finish();
    REQUIRE(md.math.size() == 4);
    CHECK_FALSE(md.math[0].display);
    CHECK(md.math[1].display);
    CHECK(md.math[1].source == "\\frac{a}{b}");
    CHECK(md.math[2].source == "\\alpha");
    CHECK(doc.all.find("$x^2$") != std::string::npos);
    CHECK(doc.all.find("$$\\frac{a}{b}$$") != std::string::npos);
    CHECK(md.math[3].source == "\\documentclass{article}"); // a whole document is typeset, not a code block
    CHECK(md.codeBlocks.empty());
}

TEST_CASE("dollars around text are not math", "[modules][math]") {
    ke::ui::SelDoc doc;
    ke::ui::MdExtras md;
    ke::ui::appendMarkdown(doc,
                           "should typeset:\n\n$ $\n$\\section*{The grid Project}$\n$\\subsection*{Scope}$\n"
                           "Research on \\textbf{GRiD} with $x^2$ inside.\n",
                           0, 0, md, 7.0f);
    doc.finish();
    REQUIRE(md.math.size() == 1);
    CHECK(md.math[0].source == "x^2");
    CHECK(doc.all.find("The grid Project") != std::string::npos);
    CHECK(doc.all.find("\\section") == std::string::npos);
    CHECK(doc.all.find("GRiD") != std::string::npos);
}

TEST_CASE("a bare document is one display", "[modules][math]") {
    ke::ui::SelDoc doc;
    ke::ui::MdExtras md;
    ke::ui::appendMarkdown(doc, "before\n\n\\documentclass{article}\n\\begin{document}\nHello\n\\end{document}\n\nafter\n", 0, 0, md, 7.0f);
    doc.finish();
    REQUIRE(md.math.size() == 1);
    CHECK(md.math[0].display);
    CHECK(md.math[0].source.find("\\end{document}") != std::string::npos);
    CHECK(doc.all.find("after") != std::string::npos);
}

TEST_CASE("a tex document reads as markdown", "[modules]") {
    const std::string md = texToMarkdown(R"(\documentclass{article}
\title{On Things}
\begin{document}
\maketitle
\section{Start}
Some \textbf{bold} and \emph{soft} text, $x^2$ too. % gone
\begin{itemize}
\item one
\end{itemize}
\begin{equation}
E = mc^2
\end{equation}
\end{document}
after)");
    CHECK(md.find("# On Things") != std::string::npos);
    CHECK(md.find("## Start") != std::string::npos);
    CHECK(md.find("**bold**") != std::string::npos);
    CHECK(md.find("*soft*") != std::string::npos);
    CHECK(md.find("gone") == std::string::npos);
    CHECK(md.find("- one") != std::string::npos);
    CHECK(md.find("$$\nE = mc^2\n$$") != std::string::npos);
    CHECK(md.find("after") == std::string::npos);
    CHECK(md.find("documentclass") == std::string::npos);
}

TEST_CASE("a remote edit lands in code points and the cursor keeps its place", "[modules]") {
    TextBuffer b;
    b.setText("héllo world");
    b.moveTo(TextPos{0, 7}); // after `héllo `... in bytes
    const std::size_t at = b.offsetOf(b.cursor());
    b.applyRemote(0, 0, "日本 ");
    CHECK(b.text() == "日本 héllo world");
    CHECK(b.offsetOf(b.cursor()) == at + 3);
    CHECK(b.takeEdits().empty()); // nothing goes back to the world
    b.applyRemote(3, 5, "hi");
    CHECK(b.text() == "日本 hi world");
    CHECK_FALSE(b.dirty());
}

TEST_CASE("the unsafe indicator reads world/unsafe and nothing else", "[modules]") {
    using nlohmann::json;
    // what `read` answers: the whole Object, a second client's first sight of the list
    const json read = {{"object/fields", {{"world/unsafe", {{"field/value", json::array({json{{"principal", "p"}, {"until", "u"}, {"by", "b"}}})}}}}}};
    CHECK(ke::chat::parseUnsafeRead(read).size() == 1);
    CHECK(ke::chat::parseUnsafeRead(json{{"object/fields", json::object()}}).empty());
    CHECK(ke::chat::parseUnsafe(json()).empty());
    CHECK(ke::chat::parseUnsafe(json::object()).empty());
    const auto one = ke::chat::parseUnsafe(json::array({json{{"principal", "p"}, {"until", "2026-10-31T12:30:00Z"}, {"by", "h"}}}));
    REQUIRE(one.size() == 1);
    const double now = ke::model::epochSeconds("2026-10-31T12:07:00Z");
    const auto nameOf = [](const std::string&) { return std::string("ivy"); };
    CHECK(ke::chat::unsafeLabel({}, nameOf, now).empty());
    CHECK(ke::chat::unsafeLabel(one, nameOf, now) == "UNSAFE ivy 23m");
    CHECK(ke::chat::unsafeLabel({one[0], one[0], one[0]}, nameOf, now) == "UNSAFE 3");
    CHECK(ke::chat::minutesLeft("2026-10-31T12:00:00Z", now) == 0);
}

TEST_CASE("a shared terminal reads as a terminal in the indicator", "[unsafe]") {
    using nlohmann::json;
    const json field = json::array({{{"principal", "p1"}, {"agent", "a1"}, {"until", "2099-01-01T00:00:00Z"}, {"by", "b"}, {"kind", "shared-terminal"}}});
    const auto entries = ke::chat::parseUnsafe(field);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].kind == "shared-terminal");
    CHECK(entries[0].agent == "a1");
    const auto nameOf = [](const std::string&) { return std::string("uuid-ish"); };
    CHECK(ke::chat::unsafeLabel(entries, nameOf, 0.0).rfind("UNSAFE terminal ", 0) == 0);
}

TEST_CASE("what a piece of TeX is", "[modules]") {
    CHECK(texKind("\\frac{a}{b}+\\text{a word or two here}") == TexKind::Math);
    CHECK(texKind("x^2") == TexKind::Math);
    CHECK(texKind("\\section*{The grid Project}") == TexKind::Text);
    CHECK(texKind("Research on \\textbf{GRiD}") == TexKind::Text);
    CHECK(texKind("some plain words here") == TexKind::Text);
    CHECK(texKind("\\begin{tikzpicture}\\draw (0,0) -- (1,1);\\end{tikzpicture}") == TexKind::Text);
    CHECK(texKind("\\begin{tikzcd} A \\arrow[r] & B \\end{tikzcd}") == TexKind::Text);
    CHECK(texKind("\\documentclass{article}") == TexKind::Document);
    CHECK(texKind("\\begin{document}x\\end{document}") == TexKind::Document);
}

TEST_CASE("text commands read as Markdown", "[modules]") {
    CHECK(texProse("\\section*{The grid Project}") == "# The grid Project");
    CHECK(texProse("\\subsection*{Scope}") == "## Scope");
    CHECK(texProse("Research on \\textbf{GRiD} in \\texttt{gridsources/}") == "Research on **GRiD** in `gridsources/`");
    CHECK(texProse("\\section*{The \\texttt{grid} Project}") == "# The `grid` Project");
    CHECK(texProse("`\\textbf{a}` stays") == "`\\textbf{a}` stays");
    CHECK(texProse("\\frac{a}{b}") == "\\frac{a}{b}");
}
