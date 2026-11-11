#include "ui/Markdown.hpp"

#include "modules/MathLayout.hpp"

#include "ui/Theme.hpp"

#include <algorithm>
#include <cctype>

namespace ke::ui {

namespace {

bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '/' || c == '-';
}

// A closing marker `m` at or after `from`, at a word boundary (so `a_b_c` and `x*y` stay plain).
std::size_t findClose(const std::string& s, std::size_t from, const std::string& m) {
    std::size_t p = from;
    while ((p = s.find(m, p)) != std::string::npos) {
        const bool before = p > 0 && !std::isspace(static_cast<unsigned char>(s[p - 1]));
        const std::size_t after = p + m.size();
        const bool boundary = after >= s.size() || !isWordChar(s[after]) || m == "`";
        if (before && boundary) {
            return p;
        }
        p += 1;
    }
    return std::string::npos;
}

bool urlEnd(char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == ')' || c == ']' || c == '>' || c == '"' || c == '\'';
}

void push(std::vector<MdInlineRun>& out, std::string text, MdInlineRun::Kind kind, std::string url = {}) {
    if (text.empty()) {
        return;
    }
    if (!out.empty() && out.back().kind == kind && kind == MdInlineRun::Plain) {
        out.back().text += text;
        return;
    }
    MdInlineRun r;
    r.text = std::move(text);
    r.kind = kind;
    r.url = std::move(url);
    out.push_back(std::move(r));
}

} // namespace

std::vector<MdInlineRun> parseInline(const std::string& s) {
    std::vector<MdInlineRun> out;
    std::string plain;
    std::size_t i = 0;
    auto flush = [&] {
        push(out, plain, MdInlineRun::Plain);
        plain.clear();
    };
    while (i < s.size()) {
        const char c = s[i];
        const bool atStart = i == 0 || !isWordChar(s[i - 1]);
        // inline code: backticks win over everything inside them
        if (c == '`') {
            const std::size_t close = s.find('`', i + 1);
            if (close != std::string::npos && close > i + 1) {
                flush();
                push(out, s.substr(i + 1, close - i - 1), MdInlineRun::Code);
                i = close + 1;
                continue;
            }
        }
        // inline math: the run keeps the whole span, dollars included, so a copy yields the source
        if (c == '$') {
            const std::size_t close = modules::mathSpanClose(s, i);
            if (close != std::string_view::npos) {
                flush();
                push(out, s.substr(i, close - i + 1), MdInlineRun::Math);
                i = close + 1;
                continue;
            }
        }
        // links [text](url)
        if (c == '[') {
            const std::size_t rb = s.find("](", i);
            const std::size_t rp = rb == std::string::npos ? rb : s.find(')', rb + 2);
            if (rb != std::string::npos && rp != std::string::npos && rb > i + 1) {
                flush();
                push(out, s.substr(i + 1, rb - i - 1), MdInlineRun::Link, s.substr(rb + 2, rp - rb - 2));
                i = rp + 1;
                continue;
            }
        }
        // bare urls
        if ((s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0) && atStart) {
            std::size_t e = i;
            while (e < s.size() && !urlEnd(s[e])) {
                ++e;
            }
            while (e > i && (s[e - 1] == '.' || s[e - 1] == ',' || s[e - 1] == ';' || s[e - 1] == ':')) {
                --e; // trailing punctuation belongs to the sentence
            }
            flush();
            push(out, s.substr(i, e - i), MdInlineRun::Link, s.substr(i, e - i));
            i = e;
            continue;
        }
        // bold ** or __
        if ((s.compare(i, 2, "**") == 0 || s.compare(i, 2, "__") == 0) && atStart && i + 2 < s.size() &&
            !std::isspace(static_cast<unsigned char>(s[i + 2]))) {
            const std::string m = s.substr(i, 2);
            const std::size_t close = findClose(s, i + 2, m);
            if (close != std::string::npos) {
                flush();
                push(out, s.substr(i + 2, close - i - 2), MdInlineRun::Bold);
                i = close + 2;
                continue;
            }
        }
        // italic * or _
        if ((c == '*' || c == '_') && atStart && i + 1 < s.size() && !std::isspace(static_cast<unsigned char>(s[i + 1])) &&
            s[i + 1] != c) {
            const std::size_t close = findClose(s, i + 1, std::string(1, c));
            if (close != std::string::npos) {
                flush();
                push(out, s.substr(i + 1, close - i - 1), MdInlineRun::Italic);
                i = close + 1;
                continue;
            }
        }
        plain += c;
        ++i;
    }
    flush();
    return out;
}

bool looksLikeMarkdown(const std::string& t) {
    if (!t.empty() && t[0] == '\\' && t.find('\\', 1) != std::string::npos) {
        return true; // perhaps a bare formula: the parser decides
    }
    if (t.find('`') != std::string::npos || t.find("**") != std::string::npos || t.find("](") != std::string::npos ||
        t.find("http://") != std::string::npos || t.find("https://") != std::string::npos) {
        return true;
    }
    for (std::size_t at = t.find('$'); at != std::string::npos; at = t.find('$', at + 1)) {
        if (t.compare(at, 2, "$$") == 0 || modules::mathSpanClose(t, at) != std::string_view::npos) {
            return true;
        }
    }
    std::size_t lineStart = 0;
    while (lineStart <= t.size()) {
        std::size_t e = t.find('\n', lineStart);
        if (e == std::string::npos) {
            e = t.size();
        }
        std::size_t p = lineStart;
        while (p < e && t[p] == ' ') {
            ++p;
        }
        if (p < e) {
            const char c = t[p];
            if (c == '#' || c == '>' || c == '|' || ((c == '-' || c == '*' || c == '+') && p + 1 < e && t[p + 1] == ' ') ||
                (std::isdigit(static_cast<unsigned char>(c)) && p + 1 < e && (t[p + 1] == '.' || t[p + 1] == ')')) ||
                t.compare(p, 3, "---") == 0 || t.compare(p, 3, "***") == 0) {
                return true;
            }
        }
        if (e == t.size()) {
            break;
        }
        lineStart = e + 1;
    }
    return false;
}

std::set<std::string>& failedTypesets() {
    static std::set<std::string> failed;
    return failed;
}

namespace {

ImU32 colorFor(MdInlineRun::Kind k, ImU32 base) {
    switch (k) {
    case MdInlineRun::Code:
        return Ink::inspected;
    case MdInlineRun::Bold:
        return Ink::strong;
    case MdInlineRun::Italic:
        return Ink::secondary;
    case MdInlineRun::Link:
        return Ink::nav;
    default:
        return base;
    }
}

SelPara paraFromInline(const std::string& line, ImU32 base, int block, float indent, std::size_t paraIndex,
                       MdExtras& extras, bool quote, const std::string& prefix = {}, ImU32 prefixColor = 0) {
    SelPara p;
    p.block = block;
    p.indent = indent;
    p.quote = quote;
    p.source = line;
    if (!prefix.empty()) {
        p.runs.push_back(SelRun{prefix, prefixColor});
    }
    for (MdInlineRun& r : parseInline(line)) {
        SelRun run{std::move(r.text), colorFor(r.kind, quote && r.kind == MdInlineRun::Plain ? Ink::secondary : base)};
        if (r.kind == MdInlineRun::Math) {
            // Hidden, not removed: the source is what a selection copies, and the box is drawn over it.
            extras.math.push_back(MdMath{paraIndex, p.runs.size(), run.text.substr(1, run.text.size() - 2), false});
            run.color = 0;
            p.runs.push_back(std::move(run));
            continue;
        }
        if (r.kind == MdInlineRun::Code) {
            run.style = SelRun::CodeBox;
        } else if (r.kind == MdInlineRun::Link) {
            run.style = SelRun::Underline;
            extras.links.push_back(MdLink{paraIndex, p.runs.size(), r.url});
        }
        if (r.kind == MdInlineRun::Link) {
            p.runs.push_back(std::move(run));
        } else {
            addLinkedRuns(p, paraIndex, std::move(run), r.kind == MdInlineRun::Code, extras);
        }
    }
    return p;
}

float mathSize() { return ImGui::GetCurrentContext() != nullptr ? ImGui::GetFontSize() : 14.0f; }

bool mathParses(const std::string& tex) {
    return modules::layoutMath(tex, 14.0f, [](std::string_view s, float z) { return static_cast<float>(s.size()) * z * 0.6f; }, true).ok;
}

// A paragraph of its own: blank space the drawer fills, tall enough for the box or its source.
void appendDisplayMath(SelDoc& doc, const std::string& tex, int block, MdExtras& extras, const std::string& source) {
    const float size = mathSize();
    const modules::MathBox box = modules::layoutMath(
        tex, size * kDisplayMathScale, [](std::string_view s, float z) { return static_cast<float>(s.size()) * z * 0.6f; }, true);
    int lines = 1;
    for (char c : tex) {
        lines += c == '\n' ? 1 : 0;
    }
    SelPara p;
    p.block = block;
    p.kind = 5;
    p.source = source;
    // What the layout cannot size comes back as an image: room for one, which shrinks to fit. A page
    // needs a page's height. Two lines more than the source, so a failure's note never lands on it.
    const modules::TexKind kind = modules::texKind(tex);
    p.reserve = failedTypesets().count(tex) != 0 ? (static_cast<float>(lines) + 2.0f) * size
                : kind == modules::TexKind::Document ? size * kDocumentLines
                : kind == modules::TexKind::Math && box.ok ? std::max(box.height(), static_cast<float>(lines) * size) + size * 0.8f
                : std::max(static_cast<float>(lines) * size, size * 5.0f) + size * 2.8f;
    p.runs.push_back(SelRun{source, 0});
    extras.math.push_back(MdMath{doc.paras.size(), 0, tex, true});
    doc.add(std::move(p));
}

// A model sometimes sends a formula with no fence and no dollars. A block that opens with a command
// and is made of math, not of prose, is a formula.
bool bareFormula(const std::string& block) {
    if (block.empty() || block[0] != '\\') {
        return false;
    }
    int commands = 0;
    int words = 0;
    for (std::size_t i = 0; i < block.size(); ++i) {
        if (block[i] == '\\') {
            ++commands;
            while (i + 1 < block.size() && std::isalpha(static_cast<unsigned char>(block[i + 1])) != 0) {
                ++i;
            }
        } else if (std::isalpha(static_cast<unsigned char>(block[i])) != 0) {
            std::size_t e = i;
            while (e < block.size() && std::isalpha(static_cast<unsigned char>(block[e])) != 0) {
                ++e;
            }
            words += e - i > 3 && block.find('{') == std::string::npos ? 1 : 0;
            i = e - 1;
        }
    }
    return commands >= 2 && words == 0;
}

std::string blockFrom(const std::vector<std::string>& lines, std::size_t from, std::size_t& next) {
    std::string block;
    next = from;
    while (next < lines.size() && !lines[next].empty()) {
        block += (block.empty() ? "" : "\n") + lines[next];
        ++next;
    }
    return block;
}

// Text a model wrapped in dollars is not math, and it is not a failure: it reads as text.
std::string dollarsAroundText(std::string line) {
    bool code = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '`') {
            code = !code;
        }
        if (line[i] != '$' || code) {
            continue;
        }
        const std::size_t close = modules::mathSpanClose(line, i);
        if (close == std::string_view::npos) {
            continue;
        }
        const std::string tex = line.substr(i + 1, close - i - 1);
        if (modules::texKind(tex) == modules::TexKind::Math) {
            i = close;
            continue;
        }
        const std::string prose = modules::texProse(tex);
        line.replace(i, close - i + 1, prose);
        i += prose.empty() ? 0 : prose.size() - 1;
    }
    return line;
}

// Inline math the layout cannot draw is typeset by the world as an image, too tall to ride in a line:
// it gets a line of its own, and the words around it stay where they are. Not inside a code fence.
std::vector<std::string> liftHardMath(std::vector<std::string> lines) {
    std::vector<std::string> out;
    bool fenced = false;
    for (std::size_t n = 0; n < lines.size(); ++n) {
        std::string& l = lines[n];
        if (l.compare(0, 3, "```") == 0 || l.compare(0, 3, "~~~") == 0) {
            fenced = !fenced;
        }
        if (fenced || l.compare(0, 2, "$$") == 0) {
            out.push_back(std::move(l));
            continue;
        }
        // a whole document with no fence: to its end, as one
        if (l.compare(0, 14, "\\documentclass") == 0 || l.compare(0, 16, "\\begin{document}") == 0) {
            out.emplace_back("```tex");
            std::size_t end = n;
            while (end < lines.size()) {
                out.push_back(lines[end]);
                if (lines[end].find("\\end{document}") != std::string::npos) {
                    break;
                }
                ++end;
            }
            out.emplace_back("```");
            n = std::min(end, lines.size() - 1);
            continue;
        }
        l = modules::texProse(dollarsAroundText(std::move(l)));
        if ((n == 0 || lines[n - 1].empty()) && !l.empty() && l[0] == '\\') {
            std::size_t next = n;
            const std::string block = blockFrom(lines, n, next);
            if (bareFormula(block)) {
                out.push_back("$$" + block + "$$");
                n = next - 1;
                continue;
            }
        }
        std::size_t from = 0;
        bool code = false;
        for (std::size_t i = 0; i < l.size(); ++i) {
            if (l[i] == '`') {
                code = !code;
            }
            if (l[i] != '$' || code) {
                continue;
            }
            const std::size_t close = modules::mathSpanClose(l, i);
            if (close == std::string_view::npos) {
                continue;
            }
            const std::string tex = l.substr(i + 1, close - i - 1);
            if (!mathParses(tex)) {
                if (i > from) {
                    out.push_back(l.substr(from, i - from));
                    out.emplace_back();
                }
                out.push_back("$$" + tex + "$$");
                out.emplace_back();
                from = close + 1;
            }
            i = close;
        }
        if (from == 0) {
            out.push_back(std::move(l));
        } else if (from < l.size()) {
            out.push_back(l.substr(from));
        }
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& src) {
    std::vector<std::string> lines;
    std::size_t s = 0;
    while (true) {
        std::size_t e = src.find('\n', s);
        if (e == std::string::npos) {
            lines.push_back(src.substr(s));
            break;
        }
        std::string l = src.substr(s, e - s);
        if (!l.empty() && l.back() == '\r') {
            l.pop_back();
        }
        lines.push_back(std::move(l));
        s = e + 1;
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

std::vector<std::string> splitCells(const std::string& row) {
    std::string r = row;
    auto trim = [](std::string s) {
        while (!s.empty() && s.front() == ' ') {
            s.erase(s.begin());
        }
        while (!s.empty() && s.back() == ' ') {
            s.pop_back();
        }
        return s;
    };
    r = trim(r);
    if (!r.empty() && r.front() == '|') {
        r.erase(r.begin());
    }
    if (!r.empty() && r.back() == '|') {
        r.pop_back();
    }
    std::vector<std::string> cells;
    std::size_t s = 0;
    while (true) {
        const std::size_t e = r.find('|', s);
        cells.push_back(trim(r.substr(s, e == std::string::npos ? std::string::npos : e - s)));
        if (e == std::string::npos) {
            break;
        }
        s = e + 1;
    }
    return cells;
}

bool isSeparatorRow(const std::string& row) {
    bool dash = false;
    for (char c : row) {
        if (c == '-') {
            dash = true;
        } else if (c != '|' && c != ':' && c != ' ') {
            return false;
        }
    }
    return dash;
}

std::size_t glyphs(const std::string& s) {
    std::size_t n = 0;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c & 0xC0u) != 0x80u) {
            ++n;
        }
    }
    return n;
}

} // namespace

void addLinkedRuns(SelPara& p, std::size_t paraIndex, SelRun run, bool code, MdExtras& extras) {
    if (extras.scan == nullptr || extras.scan->table == nullptr || extras.refs != nullptr) {
        p.runs.push_back(std::move(run));
        return;
    }
    chat::ScanContext ctx = extras.scan->ctx;
    ctx.code = code;
    const std::vector<chat::Symbol> spans = extras.scan->table->scan(run.text, ctx);
    if (spans.empty()) {
        p.runs.push_back(std::move(run));
        return;
    }
    std::size_t at = 0;
    for (const chat::Symbol& s : spans) {
        if (s.start > at) {
            SelRun before{run.text.substr(at, s.start - at), run.color};
            before.style = run.style;
            p.runs.push_back(std::move(before));
        }
        SelRun link{s.display, s.kind == chat::SymbolKind::Unresolved ? Ink::secondary : Ink::nav};
        link.style = run.style;
        extras.symbols.push_back(MdSymbol{paraIndex, p.runs.size(), s});
        p.runs.push_back(std::move(link));
        at = s.end;
    }
    if (at < run.text.size()) {
        SelRun after{run.text.substr(at), run.color};
        after.style = run.style;
        p.runs.push_back(std::move(after));
    }
}

void appendMarkdownBody(SelDoc& doc, const std::string& src, ImU32 base, int block, MdExtras& extras, float cellWidth);

namespace {

// Spans survive every Markdown transform by standing in the source as one private-use character
// each: bold, tables, lists and code carry them along like any letter, and once the paragraphs
// exist each one becomes a linked run. So a span maps to its rendered run with no offset arithmetic.
constexpr char32_t kRefBase = 0xE000;

std::string refMark(std::size_t index) {
    const char32_t c = kRefBase + static_cast<char32_t>(index);
    std::string out;
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
    return out;
}

// The index of the mark at `at`, or -1.
int refAt(const std::string& s, std::size_t at, std::size_t count) {
    if (at + 3 > s.size() || (static_cast<unsigned char>(s[at]) & 0xF0) != 0xE0) {
        return -1;
    }
    const char32_t c = (static_cast<char32_t>(static_cast<unsigned char>(s[at]) & 0x0F) << 12) |
                       (static_cast<char32_t>(static_cast<unsigned char>(s[at + 1]) & 0x3F) << 6) |
                       static_cast<char32_t>(static_cast<unsigned char>(s[at + 2]) & 0x3F);
    if (c < kRefBase || c >= kRefBase + count) {
        return -1;
    }
    return static_cast<int>(c - kRefBase);
}

std::string withMarks(const std::string& text, const std::vector<chat::RefSpan>& refs) {
    std::string out;
    std::size_t at = 0;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        out.append(text, at, refs[i].start - at);
        out += refMark(i);
        at = refs[i].end;
    }
    out.append(text, at, std::string::npos);
    return out;
}

std::string withoutMarks(const std::string& s, const std::string& text, const std::vector<chat::RefSpan>& refs) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (const int r = refAt(s, i, refs.size()); r >= 0) {
            const chat::RefSpan& span = refs[static_cast<std::size_t>(r)];
            out.append(text, span.start, span.end - span.start);
            i += 3;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// Each mark in paragraphs [first, end) becomes a linked run. Run indices the extras hold move with it.
void settleRefs(SelDoc& doc, std::size_t first, const std::string& text, const std::vector<chat::RefSpan>& refs, MdExtras& extras) {
    const chat::SymbolTable* table = extras.scan != nullptr ? extras.scan->table : nullptr;
    const model::Principals* fold = extras.scan != nullptr ? extras.scan->ctx.fold : nullptr;
    for (std::size_t pi = first; pi < doc.paras.size(); ++pi) {
        SelPara& p = doc.paras[pi];
        p.source = withoutMarks(p.source, text, refs);
        std::vector<SelRun> runs;
        std::vector<std::size_t> moved;
        for (const SelRun& run : p.runs) {
            moved.push_back(runs.size());
            std::size_t start = 0;
            for (std::size_t i = 0; i < run.text.size();) {
                const int r = refAt(run.text, i, refs.size());
                if (r < 0) {
                    ++i;
                    continue;
                }
                if (i > start) {
                    SelRun before{run.text.substr(start, i - start), run.color};
                    before.style = run.style;
                    runs.push_back(std::move(before));
                }
                const chat::RefSpan& span = refs[static_cast<std::size_t>(r)];
                const std::string current = table != nullptr && fold != nullptr && table->byId(span.object) != nullptr
                                                ? fold->nameOf(span.object) : std::string();
                chat::Symbol sym = chat::symbolOf(span, text, current);
                SelRun link{sym.display, Ink::nav};
                link.style = run.style;
                extras.symbols.push_back(MdSymbol{pi, runs.size(), std::move(sym)});
                runs.push_back(std::move(link));
                i += 3;
                start = i;
            }
            if (start < run.text.size() || run.text.empty()) {
                SelRun rest{run.text.substr(start), run.color};
                rest.style = run.style;
                runs.push_back(std::move(rest));
            }
        }
        p.runs = std::move(runs);
        for (MdLink& l : extras.links) {
            if (l.para == pi && l.runIndex < moved.size()) {
                l.runIndex = moved[l.runIndex];
            }
        }
        for (MdMath& m : extras.math) {
            if (m.para == pi && !m.display && m.run < moved.size()) {
                m.run = moved[m.run];
            }
        }
    }
    for (MdCodeBlock& cb : extras.codeBlocks) {
        cb.source = withoutMarks(cb.source, text, refs);
    }
}

} // namespace

void appendLinked(SelDoc& doc, const std::string& text, ImU32 color, int block, MdExtras& extras, int kind) {
    const bool spans = extras.refs != nullptr && !extras.refs->empty();
    const std::string shown = spans ? withMarks(text, *extras.refs) : text;
    const std::size_t first = doc.paras.size();
    SelPara p;
    p.block = block;
    p.kind = kind;
    p.source = shown;
    addLinkedRuns(p, doc.paras.size(), SelRun{shown, color}, false, extras);
    doc.add(std::move(p));
    if (spans) {
        settleRefs(doc, first, text, *extras.refs, extras);
    }
}

const chat::Symbol* symbolAt(const MdExtras& md, const SelDoc& doc, long long offset) {
    if (offset < 0 || md.symbols.empty() || doc.paras.empty() || doc.starts.empty()) {
        return nullptr;
    }
    const std::size_t para = doc.paraOfOffset(static_cast<std::size_t>(offset));
    if (para >= doc.paras.size()) {
        return nullptr;
    }
    auto it = std::lower_bound(md.symbols.begin(), md.symbols.end(), para,
                               [](const MdSymbol& s, std::size_t v) { return s.para < v; });
    if (it == md.symbols.end() || it->para != para) {
        return nullptr;
    }
    const std::size_t rel = static_cast<std::size_t>(offset) - doc.starts[para];
    std::size_t at = 0;
    const std::vector<SelRun>& runs = doc.paras[para].runs;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const std::size_t end = at + runs[r].text.size();
        if (rel >= at && rel < end) {
            for (; it != md.symbols.end() && it->para == para; ++it) {
                if (it->run == r) {
                    return &it->symbol;
                }
            }
            return nullptr;
        }
        at = end;
    }
    return nullptr;
}

void appendMarkdown(SelDoc& doc, const std::string& src, ImU32 base, int block, MdExtras& extras, float cellWidth) {
    if (extras.refs != nullptr && !extras.refs->empty()) {
        const std::vector<chat::RefSpan>& refs = *extras.refs;
        const std::size_t first = doc.paras.size();
        appendMarkdownBody(doc, withMarks(src, refs), base, block, extras, cellWidth);
        settleRefs(doc, first, src, refs, extras);
        return;
    }
    appendMarkdownBody(doc, src, base, block, extras, cellWidth);
}

void appendMarkdownBody(SelDoc& doc, const std::string& src, ImU32 base, int block, MdExtras& extras, float cellWidth) {
    const std::vector<std::string> lines = liftHardMath(splitLines(src));
    const float cw = cellWidth > 0.0f ? cellWidth
                     : (ImGui::GetCurrentContext() && ImGui::GetFont()) ? ImGui::CalcTextSize("0").x
                                                                         : 7.0f;
    std::size_t i = 0;
    while (i < lines.size()) {
        const std::string& raw = lines[i];
        std::size_t lead = 0;
        while (lead < raw.size() && raw[lead] == ' ') {
            ++lead;
        }
        const std::string line = raw.substr(lead);
        // display math: `$$ ... $$` on one line, or across several
        if (line.compare(0, 2, "$$") == 0) {
            std::string body = line.substr(2);
            std::size_t j = i;
            std::size_t close = body.find("$$");
            while (close == std::string::npos && j + 1 < lines.size()) {
                ++j;
                body += "\n" + lines[j];
                close = body.find("$$");
            }
            if (close != std::string::npos) {
                body.resize(close);
                appendDisplayMath(doc, body, block, extras, "$$" + body + "$$");
                i = j + 1;
                continue;
            }
        }
        // a ```tex fence is typeset: math here, a text fragment or a whole document by the world
        if (line.compare(0, 6, "```tex") == 0 || line.compare(0, 8, "```latex") == 0 || line.compare(0, 7, "```math") == 0) {
            std::string body;
            std::size_t j = i + 1;
            while (j < lines.size() && lines[j].compare(0, 3, "```") != 0) {
                body += (body.empty() ? "" : "\n") + lines[j];
                ++j;
            }
            if (j < lines.size() && !body.empty()) {
                appendDisplayMath(doc, body, block, extras, "```tex\n" + body + "\n```");
                i = j + 1;
                continue;
            }
        }
        // fenced code
        if (line.compare(0, 3, "```") == 0 || line.compare(0, 3, "~~~") == 0) {
            const std::string fence = line.substr(0, 3);
            std::string lang = line.substr(3);
            while (!lang.empty() && lang.front() == ' ') {
                lang.erase(lang.begin());
            }
            std::size_t j = i + 1;
            MdCodeBlock cb;
            cb.firstPara = doc.paras.size();
            cb.lang = lang;
            {
                // the block's header row, inside its frame: the language tag and [ COPY ] sit here
                // (drawMarkdownExtras), above the code, so the bracket never covers a line of it
                SelPara head;
                head.block = block;
                head.kind = 5;
                head.code = true;
                head.indent = cw;
                doc.add(std::move(head));
            }
            while (j < lines.size()) {
                std::string l = lines[j];
                std::size_t ld = 0;
                while (ld < l.size() && l[ld] == ' ') {
                    ++ld;
                }
                if (l.compare(ld, 3, fence) == 0) {
                    break;
                }
                SelPara p;
                p.block = block;
                p.source = l;
                p.code = true;
                p.indent = cw; // room for the frame
                p.runs.push_back(SelRun{l.empty() ? std::string(" ") : l, Ink::primary});
                doc.add(std::move(p));
                cb.source += l + "\n";
                ++j;
            }
            if (doc.paras.size() == cb.firstPara + 1) { // an empty block still gets a frame to click
                SelPara p;
                p.block = block;
                p.code = true;
                p.indent = cw;
                p.runs.push_back(SelRun{" ", Ink::primary});
                doc.add(std::move(p));
            }
            cb.lastPara = doc.paras.size() - 1;
            extras.codeBlocks.push_back(std::move(cb));
            i = j < lines.size() ? j + 1 : j;
            continue;
        }
        // horizontal rule
        if ((line.size() >= 3 && (line.find_first_not_of('-') == std::string::npos ||
                                  line.find_first_not_of('*') == std::string::npos))) {
            SelPara p;
            p.block = block;
            p.kind = 4;
            p.source = line;
            doc.add(std::move(p));
            ++i;
            continue;
        }
        // headings
        if (line[0] == '#') {
            std::size_t level = 0;
            while (level < line.size() && line[level] == '#' && level < 6) {
                ++level;
            }
            if (level < line.size() && line[level] == ' ') {
                SelPara p = paraFromInline(line.substr(level + 1), Ink::strong, block, 0.0f, doc.paras.size(), extras,
                                           false);
                p.source = line; // a copy still yields the `#`
                for (SelRun& r : p.runs) {
                    if (r.style == 0 && r.color == Ink::strong) {
                        r.color = Ink::strong;
                    }
                }
                doc.add(std::move(p));
                ++i;
                continue;
            }
        }
        // table: this line and the next contain pipes, the next is a separator row
        if (line.find('|') != std::string::npos && i + 1 < lines.size() && isSeparatorRow(lines[i + 1]) &&
            lines[i + 1].find('|') != std::string::npos) {
            std::vector<std::vector<std::string>> rows;
            std::size_t j = i;
            while (j < lines.size() && lines[j].find('|') != std::string::npos) {
                if (!isSeparatorRow(lines[j])) {
                    rows.push_back(splitCells(lines[j]));
                }
                ++j;
            }
            std::vector<std::size_t> widths;
            for (const auto& r : rows) {
                for (std::size_t c = 0; c < r.size(); ++c) {
                    if (widths.size() <= c) {
                        widths.push_back(0);
                    }
                    widths[c] = std::max(widths[c], glyphs(r[c]));
                }
            }
            for (std::size_t ri = 0; ri < rows.size(); ++ri) {
                SelPara p;
                p.block = block;
                p.source = lines[i + (ri == 0 ? 0 : ri + 1)];
                for (std::size_t c = 0; c < widths.size(); ++c) {
                    const std::string cell = c < rows[ri].size() ? rows[ri][c] : std::string();
                    p.runs.push_back(SelRun{c == 0 ? "" : " ", Ink::secondary});
                    p.runs.push_back(SelRun{cell + std::string(widths[c] - std::min(widths[c], glyphs(cell)), ' '),
                                            ri == 0 ? Ink::strong : base});
                    if (c + 1 < widths.size()) {
                        p.runs.push_back(SelRun{" |", Ink::secondary});
                    }
                }
                doc.add(std::move(p));
                if (ri == 0) { // the separator as a dim rule of the table's width
                    SelPara sep;
                    sep.block = block;
                    sep.source = lines[i + 1];
                    std::size_t total = 0;
                    for (std::size_t w : widths) {
                        total += w + 3;
                    }
                    sep.runs.push_back(SelRun{std::string(total > 3 ? total - 3 : 0, '-'), Ink::secondary});
                    doc.add(std::move(sep));
                }
            }
            i = j;
            continue;
        }
        // blockquote
        if (line[0] == '>') {
            std::string q = line.substr(1);
            if (!q.empty() && q[0] == ' ') {
                q.erase(q.begin());
            }
            SelPara p = paraFromInline(q, Ink::secondary, block, cw, doc.paras.size(), extras, true);
            p.source = line;
            doc.add(std::move(p));
            ++i;
            continue;
        }
        // bullet and numbered lists (hanging indent)
        if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
            const float depth = static_cast<float>(lead / 2) * 2.0f * cw;
            SelPara p = paraFromInline(line.substr(2), base, block, depth + 2.0f * cw, doc.paras.size(), extras,
                                       false, "· ", Ink::secondary);
            p.source = raw;
            p.hang = 2.0f * cw;
            doc.add(std::move(p));
            ++i;
            continue;
        }
        {
            std::size_t d = 0;
            while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) {
                ++d;
            }
            if (d > 0 && d < 4 && d + 1 < line.size() && (line[d] == '.' || line[d] == ')') && line[d + 1] == ' ') {
                const std::string num = line.substr(0, d + 1) + " ";
                const float depth = static_cast<float>(lead / 2) * 2.0f * cw;
                SelPara p = paraFromInline(line.substr(d + 2), base, block, depth + static_cast<float>(num.size()) * cw,
                                           doc.paras.size(), extras, false, num, Ink::secondary);
                p.source = raw;
                p.hang = static_cast<float>(num.size()) * cw;
                doc.add(std::move(p));
                ++i;
                continue;
            }
        }
        // plain paragraph line (blank lines become half-height spacers)
        if (line.empty()) {
            SelPara p;
            p.block = block;
            p.kind = 3;
            doc.add(std::move(p));
        } else {
            doc.add(paraFromInline(raw, base, block, 0.0f, doc.paras.size(), extras, false));
        }
        ++i;
    }
}

} // namespace ke::ui
