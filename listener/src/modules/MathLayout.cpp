#include "modules/MathLayout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace ke::modules {

namespace {

constexpr float kAscent = 0.78f, kDescent = 0.22f; // of the size, for a line of glyphs
constexpr float kAxis = 0.28f;                     // the math axis above the baseline
constexpr float kRule = 0.06f;                     // a fraction bar, of the size
constexpr float kScript = 0.72f, kMinSize = 0.5f;

enum class Cls { Ord, Op, Bin, Rel, Open, Close, Punct };

struct Sym {
    const char* text;
    Cls cls;
};

const std::map<std::string, Sym, std::less<>>& symbols() {
    static const std::map<std::string, Sym, std::less<>> table = {
        // Greek
        {"alpha", {"α", Cls::Ord}}, {"beta", {"β", Cls::Ord}}, {"gamma", {"γ", Cls::Ord}}, {"delta", {"δ", Cls::Ord}},
        {"epsilon", {"ε", Cls::Ord}}, {"varepsilon", {"ε", Cls::Ord}}, {"zeta", {"ζ", Cls::Ord}}, {"eta", {"η", Cls::Ord}},
        {"theta", {"θ", Cls::Ord}}, {"vartheta", {"ϑ", Cls::Ord}}, {"iota", {"ι", Cls::Ord}}, {"kappa", {"κ", Cls::Ord}},
        {"lambda", {"λ", Cls::Ord}}, {"mu", {"μ", Cls::Ord}}, {"nu", {"ν", Cls::Ord}}, {"xi", {"ξ", Cls::Ord}},
        {"pi", {"π", Cls::Ord}}, {"varpi", {"ϖ", Cls::Ord}}, {"rho", {"ρ", Cls::Ord}}, {"varrho", {"ϱ", Cls::Ord}},
        {"sigma", {"σ", Cls::Ord}}, {"varsigma", {"ς", Cls::Ord}}, {"tau", {"τ", Cls::Ord}}, {"upsilon", {"υ", Cls::Ord}},
        {"phi", {"φ", Cls::Ord}}, {"varphi", {"φ", Cls::Ord}}, {"chi", {"χ", Cls::Ord}}, {"psi", {"ψ", Cls::Ord}},
        {"omega", {"ω", Cls::Ord}}, {"Gamma", {"Γ", Cls::Ord}}, {"Delta", {"Δ", Cls::Ord}}, {"Theta", {"Θ", Cls::Ord}},
        {"Lambda", {"Λ", Cls::Ord}}, {"Xi", {"Ξ", Cls::Ord}}, {"Pi", {"Π", Cls::Ord}}, {"Sigma", {"Σ", Cls::Ord}},
        {"Upsilon", {"Υ", Cls::Ord}}, {"Phi", {"Φ", Cls::Ord}}, {"Psi", {"Ψ", Cls::Ord}}, {"Omega", {"Ω", Cls::Ord}},
        // large operators
        {"sum", {"∑", Cls::Op}}, {"prod", {"∏", Cls::Op}}, {"int", {"∫", Cls::Op}}, {"oint", {"∮", Cls::Op}},
        {"bigcup", {"⋃", Cls::Op}}, {"bigcap", {"⋂", Cls::Op}}, {"coprod", {"∐", Cls::Op}},
        // binary
        {"pm", {"±", Cls::Bin}}, {"mp", {"∓", Cls::Bin}}, {"times", {"×", Cls::Bin}}, {"div", {"÷", Cls::Bin}},
        {"cdot", {"·", Cls::Bin}}, {"ast", {"∗", Cls::Bin}}, {"star", {"⋆", Cls::Bin}}, {"circ", {"∘", Cls::Bin}},
        {"bullet", {"•", Cls::Bin}}, {"oplus", {"⊕", Cls::Bin}}, {"otimes", {"⊗", Cls::Bin}}, {"cup", {"∪", Cls::Bin}},
        {"cap", {"∩", Cls::Bin}}, {"wedge", {"∧", Cls::Bin}}, {"vee", {"∨", Cls::Bin}}, {"land", {"∧", Cls::Bin}},
        {"lor", {"∨", Cls::Bin}}, {"setminus", {"∖", Cls::Bin}},
        // relations
        {"leq", {"≤", Cls::Rel}}, {"le", {"≤", Cls::Rel}}, {"geq", {"≥", Cls::Rel}}, {"ge", {"≥", Cls::Rel}},
        {"neq", {"≠", Cls::Rel}}, {"ne", {"≠", Cls::Rel}}, {"equiv", {"≡", Cls::Rel}}, {"approx", {"≈", Cls::Rel}},
        {"sim", {"∼", Cls::Rel}}, {"simeq", {"≃", Cls::Rel}}, {"cong", {"≅", Cls::Rel}}, {"propto", {"∝", Cls::Rel}},
        {"ll", {"≪", Cls::Rel}}, {"gg", {"≫", Cls::Rel}}, {"in", {"∈", Cls::Rel}}, {"notin", {"∉", Cls::Rel}},
        {"ni", {"∋", Cls::Rel}}, {"subset", {"⊂", Cls::Rel}}, {"supset", {"⊃", Cls::Rel}},
        {"subseteq", {"⊆", Cls::Rel}}, {"supseteq", {"⊇", Cls::Rel}}, {"to", {"→", Cls::Rel}},
        {"rightarrow", {"→", Cls::Rel}}, {"leftarrow", {"←", Cls::Rel}}, {"gets", {"←", Cls::Rel}},
        {"leftrightarrow", {"↔", Cls::Rel}}, {"Rightarrow", {"⇒", Cls::Rel}}, {"Leftarrow", {"⇐", Cls::Rel}},
        {"Leftrightarrow", {"⇔", Cls::Rel}}, {"implies", {"⇒", Cls::Rel}}, {"iff", {"⇔", Cls::Rel}},
        {"mapsto", {"↦", Cls::Rel}}, {"perp", {"⊥", Cls::Rel}}, {"parallel", {"∥", Cls::Rel}}, {"mid", {"∣", Cls::Rel}},
        // ordinary
        {"infty", {"∞", Cls::Ord}}, {"partial", {"∂", Cls::Ord}}, {"nabla", {"∇", Cls::Ord}}, {"forall", {"∀", Cls::Ord}},
        {"exists", {"∃", Cls::Ord}}, {"nexists", {"∄", Cls::Ord}}, {"emptyset", {"∅", Cls::Ord}},
        {"varnothing", {"∅", Cls::Ord}}, {"neg", {"¬", Cls::Ord}}, {"lnot", {"¬", Cls::Ord}}, {"angle", {"∠", Cls::Ord}},
        {"prime", {"′", Cls::Ord}}, {"hbar", {"ℏ", Cls::Ord}}, {"ell", {"ℓ", Cls::Ord}}, {"Re", {"ℜ", Cls::Ord}},
        {"Im", {"ℑ", Cls::Ord}}, {"aleph", {"ℵ", Cls::Ord}}, {"dots", {"…", Cls::Ord}}, {"ldots", {"…", Cls::Ord}},
        {"cdots", {"⋯", Cls::Ord}}, {"vdots", {"⋮", Cls::Ord}}, {"ddots", {"⋱", Cls::Ord}}, {"top", {"⊤", Cls::Ord}},
        {"bot", {"⊥", Cls::Ord}}, {"square", {"□", Cls::Ord}}, {"triangle", {"△", Cls::Ord}},
        // delimiters and escapes
        {"langle", {"⟨", Cls::Open}}, {"rangle", {"⟩", Cls::Close}}, {"lfloor", {"⌊", Cls::Open}},
        {"rfloor", {"⌋", Cls::Close}}, {"lceil", {"⌈", Cls::Open}}, {"rceil", {"⌉", Cls::Close}},
        {"{", {"{", Cls::Open}}, {"}", {"}", Cls::Close}}, {"lbrace", {"{", Cls::Open}}, {"rbrace", {"}", Cls::Close}},
        {"|", {"‖", Cls::Ord}}, {"Vert", {"‖", Cls::Ord}}, {"vert", {"|", Cls::Ord}}, {"%", {"%", Cls::Ord}},
        {"$", {"$", Cls::Ord}}, {"#", {"#", Cls::Ord}}, {"&", {"&", Cls::Ord}}, {"_", {"_", Cls::Ord}},
    };
    return table;
}

bool isFunctionName(std::string_view n, bool& limits) {
    static const char* const kPlain[] = {"sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh",
                                         "tanh", "log", "ln", "exp", "det", "dim", "ker", "deg", "arg", "gcd", "Pr", "hom"};
    static const char* const kLimits[] = {"lim", "liminf", "limsup", "max", "min", "sup", "inf", "argmax", "argmin"};
    for (const char* k : kLimits) {
        if (n == k) {
            limits = true;
            return true;
        }
    }
    limits = false;
    return std::any_of(std::begin(kPlain), std::end(kPlain), [&](const char* k) { return n == k; });
}

std::string blackboard(char c) {
    switch (c) {
    case 'C': return "ℂ";
    case 'H': return "ℍ";
    case 'N': return "ℕ";
    case 'P': return "ℙ";
    case 'Q': return "ℚ";
    case 'R': return "ℝ";
    case 'Z': return "ℤ";
    default: return std::string(1, c);
    }
}

void place(MathBox& into, const MathBox& b, float dx, float dy) {
    for (MathGlyph g : b.glyphs) {
        g.x += dx;
        g.y += dy;
        into.glyphs.push_back(std::move(g));
    }
    for (MathRule r : b.rules) {
        r.x0 += dx;
        r.x1 += dx;
        r.y0 += dy;
        r.y1 += dy;
        into.rules.push_back(r);
    }
}

// Beside the last thing on the line: width grows, heights take the maximum.
void beside(MathBox& line, const MathBox& b, float gap = 0.0f) {
    line.width += gap;
    place(line, b, line.width, 0.0f);
    line.width += b.width;
    line.ascent = std::max(line.ascent, b.ascent);
    line.descent = std::max(line.descent, b.descent);
}

class Parser {
  public:
    Parser(std::string_view s, const MathMeasure& m, bool display) : s_(s), m_(m), display_(display) {}

    MathBox run(float size) {
        MathBox b = list(size, 0, display_);
        if (i_ < s_.size() && error_.empty()) {
            error_ = "unexpected " + std::string(1, s_[i_]);
        }
        b.ok = error_.empty();
        b.error = error_;
        return b;
    }

  private:
    struct Cell {
        MathBox box;
        Cls cls = Cls::Ord;
    };

    void skipSpace() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) {
            ++i_;
        }
    }
    bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }
    bool startsWith(std::string_view w) const { return s_.substr(i_, w.size()) == w; }
    void fail(std::string why) {
        if (error_.empty()) {
            error_ = std::move(why);
        }
    }

    MathBox glyph(std::string text, float size) const {
        MathBox b;
        b.width = m_(text, size);
        b.ascent = size * kAscent;
        b.descent = size * kDescent;
        b.glyphs.push_back(MathGlyph{std::move(text), 0.0f, 0.0f, size});
        return b;
    }

    std::string command() {
        ++i_; // the backslash
        std::string n;
        if (i_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i_]))) {
            while (i_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i_]))) {
                n += s_[i_++];
            }
        } else if (i_ < s_.size()) {
            n += s_[i_++];
        }
        return n;
    }

    // The atoms of a group, up to `stop` (0: the end of input), with TeX's spacing between classes.
    MathBox list(float size, char stop, bool display, bool cellMode = false) {
        MathBox line;
        line.ascent = size * kAscent;
        line.descent = size * kDescent;
        bool any = false;
        Cls prev = Cls::Ord;
        while (true) {
            skipSpace();
            if (i_ >= s_.size() || !error_.empty()) {
                break;
            }
            if (stop != 0 && at(stop)) {
                break;
            }
            if (cellMode && (at('&') || startsWith("\\\\") || startsWith("\\end"))) {
                break;
            }
            if (startsWith("\\right") && stop == 0 && inLeft_ > 0) {
                break;
            }
            if (at('}') && stop == 0) {
                break;
            }
            Cell c = atom(size, display);
            float gap = 0.0f;
            if (any && size >= 0.9f * base(size)) {
                const bool wide = c.cls == Cls::Rel || prev == Cls::Rel;
                const bool medium = c.cls == Cls::Bin || prev == Cls::Bin;
                const bool thin = c.cls == Cls::Op || prev == Cls::Op || prev == Cls::Punct;
                gap = size * (wide ? 0.28f : medium ? 0.22f : thin ? 0.17f : 0.0f);
            }
            beside(line, c.box, gap);
            prev = c.cls;
            any = true;
        }
        return line;
    }

    float base(float size) const { return std::max(size, base_); }

    MathBox group(float size, bool display) {
        skipSpace();
        if (at('{')) {
            ++i_;
            MathBox b = list(size, '}', display);
            if (!at('}')) {
                fail("missing }");
            } else {
                ++i_;
            }
            return b;
        }
        return atom(size, display).box;
    }

    std::string rawGroup() {
        skipSpace();
        std::string out;
        if (!at('{')) {
            if (i_ < s_.size()) {
                out += s_[i_++];
            }
            return out;
        }
        int depth = 0;
        for (; i_ < s_.size(); ++i_) {
            if (s_[i_] == '{' && depth++ == 0) {
                continue;
            }
            if (s_[i_] == '}' && --depth == 0) {
                ++i_;
                return out;
            }
            out += s_[i_];
        }
        fail("missing }");
        return out;
    }

    MathBox fraction(MathBox n, MathBox d, float size, bool bar) {
        const float w = std::max(n.width, d.width) + size * 0.3f;
        const float axis = size * kAxis;
        const float gap = size * 0.18f;
        MathBox b;
        b.width = w;
        place(b, n, (w - n.width) / 2, -axis - gap - n.descent);
        place(b, d, (w - d.width) / 2, -axis + gap + d.ascent);
        if (bar) {
            b.rules.push_back(MathRule{0.0f, -axis, w, -axis});
        }
        b.ascent = axis + gap + n.height();
        b.descent = gap + d.height() - axis;
        return b;
    }

    MathBox radical(MathBox body, const MathBox* index, float size) {
        const float clear = size * 0.14f;
        const float top = body.ascent + clear;
        const float bottom = body.descent;
        const float hook = size * 0.5f;
        const float left = index ? std::max(0.0f, index->width - hook * 0.45f) : 0.0f;
        MathBox b;
        b.width = left + hook + body.width + size * 0.08f;
        place(b, body, left + hook, 0.0f);
        const float mid = (bottom - top) * 0.45f;
        b.rules.push_back(MathRule{left, mid + size * 0.08f, left + hook * 0.28f, mid});
        b.rules.push_back(MathRule{left + hook * 0.28f, mid, left + hook * 0.55f, bottom});
        b.rules.push_back(MathRule{left + hook * 0.55f, bottom, left + hook, -top});
        b.rules.push_back(MathRule{left + hook, -top, b.width, -top});
        b.ascent = top + kRule * size;
        b.descent = bottom;
        if (index) {
            place(b, *index, 0.0f, mid - index->descent - size * 0.1f);
            b.ascent = std::max(b.ascent, -(mid - index->descent - size * 0.1f) + index->ascent);
        }
        return b;
    }

    // A delimiter as tall as `a`+`d`: straight ones are strokes, curved ones the glyph, larger.
    MathBox delimiter(const std::string& d, float a, float dn, float size) {
        MathBox b;
        const float h = a + dn;
        if (d.empty() || d == ".") {
            return b;
        }
        if (h <= size * 1.1f) {
            return glyph(d, size);
        }
        const float w = size * 0.32f;
        if (d == "|" || d == "‖") {
            b.width = d == "|" ? w : w * 1.4f;
            b.rules.push_back(MathRule{w / 2, -a, w / 2, dn});
            if (d == "‖") {
                b.rules.push_back(MathRule{w, -a, w, dn});
            }
        } else if (d == "[" || d == "]") {
            const float x0 = d == "[" ? w * 0.3f : w * 0.7f;
            const float x1 = d == "[" ? w : 0.0f;
            b.width = w;
            b.rules.push_back(MathRule{x0, -a, x0, dn});
            b.rules.push_back(MathRule{x0, -a, x1, -a});
            b.rules.push_back(MathRule{x0, dn, x1, dn});
        } else if (d == "(" || d == ")") {
            // A bow of strokes: the font's parenthesis stops scaling cleanly past two lines.
            const bool open = d == "(";
            const float depth = std::min(w * 0.75f, h * 0.2f);
            b.width = depth + size * 0.08f;
            const int kSteps = 12;
            float px = 0.0f, py = 0.0f;
            for (int k = 0; k <= kSteps; ++k) {
                const float t = static_cast<float>(k) / kSteps;
                const float bow = std::sin(t * 3.14159265f) * depth;
                const float x = open ? depth - bow : bow;
                const float y = -a + h * t;
                if (k > 0) {
                    b.rules.push_back(MathRule{px + size * 0.04f, py, x + size * 0.04f, y});
                }
                px = x;
                py = y;
            }
        } else if (d == "{" || d == "}") {
            const bool open = d == "{";
            const float x = open ? w * 0.6f : w * 0.4f, tip = open ? 0.0f : w, end = open ? w : 0.0f, mid = (dn - a) / 2;
            b.width = w;
            b.rules.push_back(MathRule{end, -a, x, -a + w * 0.3f});
            b.rules.push_back(MathRule{x, -a + w * 0.3f, x, mid - w * 0.3f});
            b.rules.push_back(MathRule{x, mid - w * 0.3f, tip, mid});
            b.rules.push_back(MathRule{tip, mid, x, mid + w * 0.3f});
            b.rules.push_back(MathRule{x, mid + w * 0.3f, x, dn - w * 0.3f});
            b.rules.push_back(MathRule{x, dn - w * 0.3f, end, dn});
        } else {
            return glyph(d, size * std::min(3.0f, h / size));
        }
        b.ascent = a;
        b.descent = dn;
        return b;
    }

    std::string delimiterName() {
        skipSpace();
        if (at('\\')) {
            const std::string n = command();
            auto it = symbols().find(n);
            if (it == symbols().end()) {
                fail("unknown delimiter \\" + n);
                return "";
            }
            return it->second.text;
        }
        return i_ < s_.size() ? std::string(1, s_[i_++]) : std::string();
    }

    MathBox matrix(const std::string& env, float size) {
        std::vector<std::vector<MathBox>> rows(1);
        while (true) {
            rows.back().push_back(list(size, 0, false, true));
            skipSpace();
            if (at('&')) {
                ++i_;
            } else if (startsWith("\\\\")) {
                i_ += 2;
                rows.emplace_back();
            } else {
                break;
            }
            if (!error_.empty() || i_ >= s_.size()) {
                break;
            }
        }
        if (!startsWith("\\end")) {
            fail("missing \\end{" + env + "}");
            return {};
        }
        i_ += 4;
        if (rawGroup() != env) {
            fail("mismatched \\end");
        }
        if (rows.back().size() == 1 && rows.back()[0].glyphs.empty()) {
            rows.pop_back();
        }
        std::size_t cols = 0;
        for (const auto& r : rows) {
            cols = std::max(cols, r.size());
        }
        std::vector<float> width(cols, 0.0f);
        for (const auto& r : rows) {
            for (std::size_t c = 0; c < r.size(); ++c) {
                width[c] = std::max(width[c], r[c].width);
            }
        }
        const bool aligned = env == "aligned" || env == "align" || env == "align*" || env == "split";
        const bool leftAligned = env == "cases";
        const float colGap = aligned ? 0.0f : size * 0.8f;
        const float rowGap = size * 0.28f;
        float height = 0.0f;
        std::vector<float> ascent(rows.size(), 0.0f), descent(rows.size(), 0.0f);
        for (std::size_t y = 0; y < rows.size(); ++y) {
            for (const MathBox& c : rows[y]) {
                ascent[y] = std::max(ascent[y], c.ascent);
                descent[y] = std::max(descent[y], c.descent);
            }
            height += ascent[y] + descent[y] + (y > 0 ? rowGap : 0.0f);
        }
        MathBox body;
        float yTop = -height / 2 - size * kAxis;
        for (std::size_t y = 0; y < rows.size(); ++y) {
            const float baseline = yTop + ascent[y];
            float x = 0.0f;
            for (std::size_t c = 0; c < rows[y].size(); ++c) {
                const MathBox& cell = rows[y][c];
                float dx = (width[c] - cell.width) / 2;
                if (aligned) {
                    dx = c % 2 == 0 ? width[c] - cell.width : 0.0f;
                } else if (leftAligned) {
                    dx = 0.0f;
                }
                place(body, cell, x + dx, baseline);
                x += width[c] + colGap;
            }
            yTop += ascent[y] + descent[y] + rowGap;
        }
        for (std::size_t c = 0; c < cols; ++c) {
            body.width += width[c] + (c > 0 ? colGap : 0.0f);
        }
        body.ascent = height / 2 + size * kAxis;
        body.descent = height / 2 - size * kAxis;
        std::string open, close;
        if (env == "pmatrix") {
            open = "(", close = ")";
        } else if (env == "bmatrix") {
            open = "[", close = "]";
        } else if (env == "vmatrix") {
            open = "|", close = "|";
        } else if (env == "Vmatrix") {
            open = "‖", close = "‖";
        } else if (env == "Bmatrix") {
            open = "{", close = "}";
        } else if (env == "cases") {
            open = "{";
        }
        MathBox out;
        beside(out, delimiter(open, body.ascent, body.descent, size));
        beside(out, body, size * 0.1f);
        beside(out, delimiter(close, body.ascent, body.descent, size), size * 0.1f);
        return out;
    }

    MathBox accent(const std::string& mark, MathBox body, float size) {
        MathBox b = body;
        const float y = -body.ascent - size * 0.02f;
        if (mark == "-") {
            b.rules.push_back(MathRule{0.0f, y, body.width, y});
        } else if (mark == "_") {
            b.rules.push_back(MathRule{0.0f, body.descent + size * 0.04f, body.width, body.descent + size * 0.04f});
            b.descent += size * 0.08f;
            return b;
        } else if (mark == ">") {
            const float tip = size * 0.16f;
            b.rules.push_back(MathRule{0.0f, y, body.width, y});
            b.rules.push_back(MathRule{body.width, y, body.width - tip, y - tip * 0.6f});
            b.rules.push_back(MathRule{body.width, y, body.width - tip, y + tip * 0.6f});
            b.ascent += tip * 0.6f;
        } else if (mark == "^") {
            const float w = std::min(body.width, size * 0.5f), x = (body.width - w) / 2;
            b.rules.push_back(MathRule{x, y, x + w / 2, y - size * 0.14f});
            b.rules.push_back(MathRule{x + w / 2, y - size * 0.14f, x + w, y});
            b.ascent += size * 0.14f;
        } else if (mark == "~") {
            const float w = std::min(body.width, size * 0.5f), x = (body.width - w) / 2, a = size * 0.07f;
            b.rules.push_back(MathRule{x, y, x + w * 0.33f, y - a});
            b.rules.push_back(MathRule{x + w * 0.33f, y - a, x + w * 0.66f, y});
            b.rules.push_back(MathRule{x + w * 0.66f, y, x + w, y - a});
            b.ascent += a;
        } else if (mark == ".") {
            const float x = body.width / 2, d = size * 0.05f;
            b.rules.push_back(MathRule{x - d, y - size * 0.06f, x + d, y - size * 0.06f});
        }
        b.ascent += size * 0.08f;
        return b;
    }

    Cell attachScripts(Cell c, float size, bool limits) {
        MathBox sup, sub;
        bool hasSup = false, hasSub = false;
        const float small = std::max(size * kScript, base(size) * kMinSize);
        while (true) {
            skipSpace();
            if (at('^') && !hasSup) {
                ++i_;
                sup = group(small, false);
                hasSup = true;
            } else if (at('_') && !hasSub) {
                ++i_;
                sub = group(small, false);
                hasSub = true;
            } else if (at('\'')) {
                ++i_;
                beside(c.box, glyph("′", size));
            } else {
                break;
            }
        }
        if (!hasSup && !hasSub) {
            return c;
        }
        MathBox b;
        const MathBox& n = c.box;
        if (limits) {
            const float w = std::max({n.width, hasSup ? sup.width : 0.0f, hasSub ? sub.width : 0.0f});
            b.width = w;
            place(b, n, (w - n.width) / 2, 0.0f);
            b.ascent = n.ascent;
            b.descent = n.descent;
            const float gap = size * 0.1f;
            if (hasSup) {
                place(b, sup, (w - sup.width) / 2, -n.ascent - gap - sup.descent);
                b.ascent += gap + sup.height();
            }
            if (hasSub) {
                place(b, sub, (w - sub.width) / 2, n.descent + gap + sub.ascent);
                b.descent += gap + sub.height();
            }
            c.box = std::move(b);
            return c;
        }
        b = n;
        const float raise = std::max(size * 0.42f, n.ascent - size * 0.3f);
        const float drop = std::max(size * 0.2f, n.descent - size * 0.05f);
        float w = 0.0f;
        if (hasSup) {
            place(b, sup, n.width + size * 0.05f, -raise);
            b.ascent = std::max(b.ascent, raise + sup.ascent);
            w = sup.width;
        }
        if (hasSub) {
            place(b, sub, n.width + size * 0.05f, drop);
            b.descent = std::max(b.descent, drop + sub.descent);
            w = std::max(w, sub.width);
        }
        b.width = n.width + size * 0.08f + w;
        c.box = std::move(b);
        return c;
    }

    Cell atom(float size, bool display) {
        Cell c;
        bool limits = false;
        const char ch = s_[i_];
        if (ch == '{') {
            c.box = group(size, display);
        } else if (ch == '}') {
            fail("unexpected }");
            ++i_;
        } else if (ch == '^' || ch == '_') {
            c.box.ascent = size * kAscent;
            c.box.descent = size * kDescent; // a script on nothing
        } else if (ch == '\\') {
            const std::string n = command();
            c = commandAtom(n, size, display, limits);
        } else {
            std::size_t len = 1;
            const unsigned char u = static_cast<unsigned char>(ch);
            if (u >= 0xF0) {
                len = 4;
            } else if (u >= 0xE0) {
                len = 3;
            } else if (u >= 0xC0) {
                len = 2;
            }
            const std::string text(s_.substr(i_, len));
            i_ += text.size();
            c.box = glyph(ch == '-' ? "−" : text, size);
            if (ch == '+' || ch == '-' || ch == '*') {
                c.cls = Cls::Bin;
            } else if (ch == '=' || ch == '<' || ch == '>' || ch == ':') {
                c.cls = Cls::Rel;
            } else if (ch == '(' || ch == '[') {
                c.cls = Cls::Open;
            } else if (ch == ')' || ch == ']') {
                c.cls = Cls::Close;
            } else if (ch == ',' || ch == ';') {
                c.cls = Cls::Punct;
            }
        }
        return attachScripts(std::move(c), size, limits);
    }

    Cell commandAtom(const std::string& n, float size, bool display, bool& limits) {
        Cell c;
        if (auto it = symbols().find(n); it != symbols().end()) {
            c.cls = it->second.cls;
            if (c.cls == Cls::Op) {
                c.box = glyph(it->second.text, display ? size * 1.7f : size * 1.15f);
                const float shift = (c.box.ascent - c.box.descent) / 2 - size * kAxis;
                for (MathGlyph& g : c.box.glyphs) {
                    g.y += shift;
                }
                c.box.ascent -= shift;
                c.box.descent += shift;
                limits = display && n != "int" && n != "oint";
                return c;
            }
            c.box = glyph(it->second.text, size);
            return c;
        }
        if (bool lim = false; isFunctionName(n, lim)) {
            c.box = glyph(n, size);
            c.cls = Cls::Op;
            limits = lim && display;
            return c;
        }
        if (n == "frac" || n == "dfrac" || n == "tfrac" || n == "cfrac" || n == "binom") {
            const bool big = n == "dfrac" || n == "cfrac" || (display && n != "tfrac");
            const float inner = big ? size : std::max(size * kScript, base(size) * kMinSize);
            MathBox top = group(inner, false);
            MathBox bottom = group(inner, false);
            c.box = fraction(std::move(top), std::move(bottom), inner, n != "binom");
            if (n == "binom") {
                MathBox wrapped;
                beside(wrapped, delimiter("(", c.box.ascent, c.box.descent, size));
                beside(wrapped, c.box);
                beside(wrapped, delimiter(")", c.box.ascent, c.box.descent, size));
                c.box = std::move(wrapped);
            }
            return c;
        }
        if (n == "sqrt") {
            skipSpace();
            MathBox index;
            bool hasIndex = false;
            if (at('[')) {
                ++i_;
                index = list(std::max(size * 0.55f, base(size) * kMinSize), ']', false);
                ++i_;
                hasIndex = true;
            }
            c.box = radical(group(size, display), hasIndex ? &index : nullptr, size);
            return c;
        }
        if (n == "left") {
            const std::string open = delimiterName();
            ++inLeft_;
            MathBox body = list(size, 0, display);
            --inLeft_;
            if (!startsWith("\\right")) {
                fail("\\left with no \\right");
                return c;
            }
            i_ += 6;
            const std::string close = delimiterName();
            const float a = body.ascent, d = body.descent;
            beside(c.box, delimiter(open, a, d, size));
            beside(c.box, body, size * 0.06f);
            beside(c.box, delimiter(close, a, d, size), size * 0.06f);
            return c;
        }
        if (n == "begin") {
            const std::string env = rawGroup();
            static const char* const kEnvs[] = {"matrix", "pmatrix", "bmatrix", "vmatrix", "Vmatrix", "Bmatrix",
                                                "cases", "aligned", "align", "align*", "split", "array", "gathered"};
            if (std::none_of(std::begin(kEnvs), std::end(kEnvs), [&](const char* k) { return env == k; })) {
                fail("unknown environment " + env);
                return c;
            }
            if (env == "array") {
                rawGroup(); // the column spec
            }
            c.box = matrix(env, size);
            return c;
        }
        if (n == "text" || n == "textrm" || n == "mathrm" || n == "operatorname" || n == "textbf" || n == "texttt") {
            const std::string t = rawGroup();
            c.box = glyph(t, size);
            return c;
        }
        if (n == "mathbf" || n == "boldsymbol" || n == "mathit" || n == "mathcal" || n == "mathsf" || n == "mathtt" ||
            n == "mathfrak" || n == "displaystyle" || n == "textstyle") {
            if (n == "displaystyle" || n == "textstyle") {
                return c; // the size is the caller's
            }
            c.box = group(size, display);
            return c;
        }
        if (n == "mathbb") {
            const std::string t = rawGroup();
            std::string out;
            for (char l : t) {
                out += blackboard(l);
            }
            c.box = glyph(out, size);
            return c;
        }
        static const std::map<std::string, std::string, std::less<>> kAccents = {
            {"overline", "-"}, {"bar", "-"}, {"underline", "_"}, {"vec", ">"}, {"overrightarrow", ">"}, {"hat", "^"},
            {"widehat", "^"}, {"tilde", "~"}, {"widetilde", "~"}, {"dot", "."}, {"ddot", "."}};
        if (auto it = kAccents.find(n); it != kAccents.end()) {
            c.box = accent(it->second, group(size, display), size);
            return c;
        }
        static const std::map<std::string, float, std::less<>> kSpaces = {
            {",", 0.17f}, {":", 0.22f}, {";", 0.28f}, {"!", -0.17f}, {" ", 0.33f}, {"quad", 1.0f}, {"qquad", 2.0f}};
        if (auto it = kSpaces.find(n); it != kSpaces.end()) {
            c.box.width = size * it->second;
            return c;
        }
        fail("unknown command \\" + n);
        return c;
    }

    std::string_view s_;
    const MathMeasure& m_;
    bool display_;
    std::size_t i_ = 0;
    int inLeft_ = 0;
    float base_ = 0.0f;
    std::string error_;

  public:
    void setBase(float b) { base_ = b; }
};

} // namespace

MathBox layoutMath(std::string_view tex, float size, const MathMeasure& measure, bool display) {
    Parser p(tex, measure, display);
    p.setBase(size);
    return p.run(size);
}

MathBox scaled(const MathBox& box, float factor) {
    MathBox b = box;
    b.width *= factor;
    b.ascent *= factor;
    b.descent *= factor;
    for (MathGlyph& g : b.glyphs) {
        g.x *= factor;
        g.y *= factor;
        g.size *= factor;
    }
    for (MathRule& r : b.rules) {
        r.x0 *= factor;
        r.x1 *= factor;
        r.y0 *= factor;
        r.y1 *= factor;
    }
    return b;
}

std::size_t mathSpanClose(std::string_view s, std::size_t at) {
    if (at >= s.size() || s[at] != '$' || at + 1 >= s.size()) {
        return std::string_view::npos;
    }
    const char first = s[at + 1];
    if (std::isspace(static_cast<unsigned char>(first)) || first == '$') {
        return std::string_view::npos;
    }
    if (at > 0 && std::isalnum(static_cast<unsigned char>(s[at - 1]))) {
        return std::string_view::npos; // US$5 and $HOME$PATH
    }
    for (std::size_t i = at + 1; i < s.size(); ++i) {
        if (s[i] == '\\') {
            ++i;
            continue;
        }
        if (s[i] != '$') {
            continue;
        }
        const bool spaceBefore = std::isspace(static_cast<unsigned char>(s[i - 1])) != 0;
        const bool digitAfter = i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) != 0;
        if (!spaceBefore && !digitAfter) {
            return i;
        }
    }
    return std::string_view::npos;
}

namespace {

bool hasCommand(std::string_view tex, std::string_view name) {
    for (std::size_t at = tex.find(name); at != std::string_view::npos; at = tex.find(name, at + 1)) {
        const std::size_t end = at + name.size();
        if (at > 0 && tex[at - 1] == '\\' && (end >= tex.size() || std::isalpha(static_cast<unsigned char>(tex[end])) == 0)) {
            return true;
        }
    }
    return false;
}

// Three words in a row, once what commands hold is set aside: `\text{a b c}` is still math.
bool sentence(std::string_view tex) {
    int run = 0;
    for (std::size_t i = 0; i < tex.size();) {
        const unsigned char c = static_cast<unsigned char>(tex[i]);
        if (c == '\\') {
            ++i;
            while (i < tex.size() && std::isalpha(static_cast<unsigned char>(tex[i])) != 0) {
                ++i;
            }
            while (i < tex.size() && tex[i] == '{') {
                const std::size_t close = tex.find('}', i);
                i = close == std::string_view::npos ? tex.size() : close + 1;
            }
            run = 0;
        } else if (std::isalpha(c) != 0) {
            std::size_t e = i;
            while (e < tex.size() && std::isalpha(static_cast<unsigned char>(tex[e])) != 0) {
                ++e;
            }
            run = e - i >= 2 ? run + 1 : 0;
            if (run >= 3) {
                return true;
            }
            i = e;
        } else if (c == ' ' || c == '\n' || c == '\t') {
            ++i;
        } else {
            run = 0;
            ++i;
        }
    }
    return false;
}

} // namespace

TexKind texKind(std::string_view tex) {
    if (hasCommand(tex, "documentclass") || tex.find("\\begin{document}") != std::string_view::npos) {
        return TexKind::Document;
    }
    for (const std::string_view name : {"section", "subsection", "subsubsection", "paragraph", "part", "chapter", "title",
                                        "maketitle", "textbf", "textit", "texttt", "textsc", "emph", "underline", "item"}) {
        if (hasCommand(tex, name)) {
            return TexKind::Text;
        }
    }
    for (const std::string_view env : {"itemize", "enumerate", "description", "center", "quote", "tabular", "verbatim", "tikzpicture",
                                       "tikzcd"}) {
        if (tex.find("\\begin{" + std::string(env) + "}") != std::string_view::npos) {
            return TexKind::Text;
        }
    }
    return sentence(tex) ? TexKind::Text : TexKind::Math;
}

std::string texProse(std::string_view text) {
    struct Form {
        std::string_view command, before, after;
    };
    static constexpr Form kForms[] = {{"\\subsubsection", "### ", ""}, {"\\subsection", "## ", ""}, {"\\section", "# ", ""},
                                      {"\\textbf", "**", "**"},       {"\\textit", "*", "*"},      {"\\emph", "*", "*"},
                                      {"\\texttt", "`", "`"}};
    std::string out;
    bool code = false;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '`') {
            code = !code;
        }
        bool taken = false;
        if (!code && text[i] == '\\') {
            for (const Form& f : kForms) {
                if (text.compare(i, f.command.size(), f.command) != 0) {
                    continue;
                }
                std::size_t open = i + f.command.size();
                if (open < text.size() && text[open] == '*') {
                    ++open;
                }
                if (open >= text.size() || text[open] != '{') {
                    continue;
                }
                std::size_t close = open + 1;
                int depth = 1;
                while (close < text.size()) {
                    depth += text[close] == '{' ? 1 : text[close] == '}' ? -1 : 0;
                    if (depth == 0) {
                        break;
                    }
                    ++close;
                }
                if (depth != 0) {
                    continue;
                }
                out += f.before;
                out += texProse(text.substr(open + 1, close - open - 1));
                out += f.after;
                i = close + 1;
                taken = true;
                break;
            }
        }
        if (!taken) {
            out += text[i];
            ++i;
        }
    }
    return out;
}

} // namespace ke::modules
