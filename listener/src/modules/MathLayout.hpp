// TeX math, laid out natively: a parser for the math people and models actually write, and boxes
// of glyphs and rules placed by the TeX conventions that matter (fractions, scripts, limits,
// radicals, stretched delimiters, matrices). No ImGui and no file: the caller says how wide a
// string is, so it is testable with no window and builds wherever C++ does. Anything it does not
// know fails plainly with `ok` false, and the caller shows the source.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ke::modules {

struct MathGlyph {
    std::string text;
    float x = 0.0f, y = 0.0f; // the glyph's left and baseline; y grows down
    float size = 0.0f;
};

struct MathRule {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f; // a stroke, any angle
};

struct MathBox {
    float width = 0.0f, ascent = 0.0f, descent = 0.0f; // from the baseline
    std::vector<MathGlyph> glyphs;
    std::vector<MathRule> rules;
    bool ok = true;
    std::string error; // why not, for the tooltip
    float height() const { return ascent + descent; }
};

// The width of `text` at `size` in the font it will be drawn with.
using MathMeasure = std::function<float(std::string_view text, float size)>;

// `display`: limits above and below, full-size fractions; otherwise the compact inline form.
MathBox layoutMath(std::string_view tex, float size, const MathMeasure& measure, bool display);
MathBox scaled(const MathBox& box, float factor);

// The `$` rule (docs/listener-style.md): a math span opens with `$` not followed by a space and
// closes at the next `$` not preceded by a space and not followed by a digit, so prices and shell
// text stay text. Returns the index of the closing `$`, or npos when `at` opens nothing.
std::size_t mathSpanClose(std::string_view s, std::size_t at);

// What a piece of TeX is: math, text with typesetting commands or sentences in it, or a whole
// document. Only math goes through the layout; the rest is the world's to typeset.
enum class TexKind { Math, Text, Document };
TexKind texKind(std::string_view tex);
// The text commands a message uses in prose, as Markdown: `\section*{a}` is `# a`, `\textbf{a}` is
// `**a**`, `\texttt{a}` is a code span. Backticks are left alone, and so is anything else.
std::string texProse(std::string_view text);

} // namespace ke::modules
