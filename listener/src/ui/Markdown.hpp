// Markdown for the transcript, Field values and instructions: a small line-based parser that
// turns the model's text into SelDoc paragraphs of styled runs (ui/SelectableText), so
// selection and copy keep working across styles. No library; unknown constructs fall back to
// plain text. Rendering rules (docs/listener-style.md, "Rich text"):
//   `code`            amber on a raised black box          **bold** / __bold__   white
//   *italic* / _it_   dim phosphor                          # heading             white, `#` dropped, kept in a copy
//   - item / 1. item  hanging indent, `·` or the number dim ``` fenced ```        boxed block,
//                     language tag and [ COPY ] in the corner, lines in phosphor
//   [text](url), bare http(s)://   cyan, underlined; the url is a tooltip and right-click copy,
//                                  never opened by the listener
//   | a | b | tables  aligned columns, `|` dim             ---                   a dim rule
//   > quote           a dim left bar, text dim
// Field markers the serializer emits, such as `(message/text)` or `seq 680`, stay plain:
// emphasis only applies between word boundaries. Each paragraph keeps its source line so a
// block copy (triple-click) yields the markdown, not the rendering.
#pragma once

#include "chat/RichText.hpp"
#include "chat/Symbols.hpp"
#include "ui/SelectableText.hpp"

#include <set>
#include <string>
#include <vector>

namespace ke::ui {

struct MdCodeBlock {
    std::size_t firstPara; // paragraph indices in the SelDoc, inclusive
    std::size_t lastPara;
    std::string lang;
    std::string source; // the code without the fences
};

struct MdLink {
    std::size_t para;
    std::size_t runIndex; // the run inside the paragraph
    std::string url;
};

// A piece of TeX math in the document. Its source stays in the runs, so selection and copy yield
// the source; the drawer hides that text and draws the typeset box in its place. `display`: a
// paragraph of kind 5 of its own (`$$`, or a ```tex fence), otherwise run `run` of `para`.
struct MdMath {
    std::size_t para;
    std::size_t run;
    std::string source; // the TeX, without its delimiters
    bool display;
};

// Where to look symbols up while building (chat/Symbols). Null table: no symbol links.
struct SymbolScan {
    const chat::SymbolTable* table = nullptr;
    chat::ScanContext ctx;
};

struct MdSymbol {
    std::size_t para;
    std::size_t run;
    chat::Symbol symbol;
};

// Decorations the drawer needs beyond runs: code block frames, link urls, symbols. Set `scan`
// before building a document to link symbols; it is read only while building.
struct MdExtras {
    std::vector<MdCodeBlock> codeBlocks;
    std::vector<MdLink> links;
    std::vector<MdSymbol> symbols; // in document order
    std::vector<MdMath> math;
    // The text's own references (chat/RichText), set before building. Non-null means it has a refs
    // Field: links come from these spans alone and the matcher never reads this text.
    const std::vector<chat::RefSpan>* refs = nullptr;
    const SymbolScan* scan = nullptr;
};

// One run of a paragraph, split around its symbols (cyan, or dim when unresolved).
void addLinkedRuns(SelPara& p, std::size_t paraIndex, SelRun run, bool code, MdExtras& extras);
// A plain paragraph with its symbols linked; block copies yield `text` itself.
void appendLinked(SelDoc& doc, const std::string& text, ImU32 color, int block, MdExtras& extras, int kind = 0);
// The symbol at a character offset of the document, or nullptr.
const chat::Symbol* symbolAt(const MdExtras& md, const SelDoc& doc, long long offset);

// Appends `src` rendered as markdown to `doc` as paragraphs of `block`, base colour `base`.
// Paragraph indices in `extras` are absolute in `doc`. `cellWidth` is the width of one
// monospace glyph in pixels (indents are measured in it); below zero it is read from the
// current ImGui font, so tests without a context pass it explicitly.
void appendMarkdown(SelDoc& doc, const std::string& src, ImU32 base, int block, MdExtras& extras,
                    float cellWidth = -1.0f);

// Sources the world would not typeset: they need room for their source, not for a page.
std::set<std::string>& failedTypesets();

inline constexpr float kDocumentLines = 60.0f;    // the height a document's first page gets, in lines
inline constexpr float kDisplayMathScale = 1.2f; // display math reads a size larger than its text

// Inline runs of one line (exposed for tests): code, bold, italic, links; plain otherwise.
struct MdInlineRun {
    std::string text;
    enum Kind { Plain, Code, Bold, Italic, Link, Math } kind = Plain;
    std::string url; // Link
};
std::vector<MdInlineRun> parseInline(const std::string& line);

// True when the text has any markdown construct worth rendering (otherwise a plain paragraph
// is cheaper and identical).
bool looksLikeMarkdown(const std::string& text);

} // namespace ke::ui
