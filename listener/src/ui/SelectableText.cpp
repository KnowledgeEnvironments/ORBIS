#include "ui/SelectableText.hpp"

#include "drive/Commands.hpp"
#include "ui/Fonts.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <imgui_internal.h> // ImTextCharFromUtf8
#include <cmath>

namespace ke::ui {

std::string SelPara::text() const {
    std::string s;
    for (const SelRun& r : runs) {
        s += r.text;
    }
    return s;
}

void SelDoc::add(SelPara p) {
    paras.push_back(std::move(p));
}

void SelDoc::add(std::string text, ImU32 color, int block, int kind) {
    SelPara p;
    p.runs.push_back(SelRun{std::move(text), color});
    p.block = block;
    p.kind = kind;
    paras.push_back(std::move(p));
}

void SelDoc::finish() {
    all.clear();
    starts.clear();
    for (std::size_t i = 0; i < paras.size(); ++i) {
        if (i > 0) {
            all += '\n';
        }
        starts.push_back(all.size());
        all += paras[i].text();
    }
}

std::string SelDoc::blockText(int block) const {
    // A block copy yields the markdown source where a paragraph has one (ui/Markdown), the
    // rendered text otherwise.
    std::string s;
    for (std::size_t i = 0; i < paras.size(); ++i) {
        // a spacer, or a reserved row with nothing to say (a code block's header row), is not text
        if (paras[i].block == block && paras[i].kind != 3 && !(paras[i].kind == 5 && paras[i].source.empty())) {
            s += (paras[i].source.empty() ? paras[i].text() : paras[i].source) + "\n";
        }
    }
    return s;
}

bool paraSpan(const SelLayout& lay, std::size_t first, std::size_t last, float& y0, float& y1) {
    bool any = false;
    for (const SelLine& l : lay.lines) {
        if (l.para >= first && l.para <= last) {
            if (!any) {
                y0 = l.y;
                y1 = l.y + l.h;
                any = true;
            } else {
                y0 = std::min(y0, l.y);
                y1 = std::max(y1, l.y + l.h);
            }
        }
    }
    return any;
}

std::size_t SelDoc::paraOfOffset(std::size_t offset) const {
    if (starts.empty()) {
        return 0;
    }
    auto it = std::upper_bound(starts.begin(), starts.end(), offset);
    return static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, (it - starts.begin()) - 1));
}

std::pair<std::size_t, std::size_t> wordAt(const std::string& all, std::size_t offset) {
    auto isSep = [](char c) { return c == ' ' || c == '\n' || c == '\t'; };
    if (all.empty()) {
        return {0, 0};
    }
    offset = std::min(offset, all.size() - 1);
    if (isSep(all[offset])) {
        return {offset, offset + 1};
    }
    std::size_t a = offset;
    while (a > 0 && !isSep(all[a - 1])) {
        --a;
    }
    std::size_t b = offset;
    while (b < all.size() && !isSep(all[b])) {
        ++b;
    }
    return {a, b};
}

std::string selectedText(const SelDoc& doc, const SelState& st) {
    if (st.hasText()) {
        const std::size_t a = static_cast<std::size_t>(std::min(st.anchor, st.extent));
        const std::size_t b = std::min(doc.all.size(), static_cast<std::size_t>(std::max(st.anchor, st.extent)));
        return a < b ? doc.all.substr(a, b - a) : std::string();
    }
    if (st.block >= 0) {
        return doc.blockText(st.block);
    }
    return "";
}

namespace {

// Width of the first `n` bytes of `s` at `size` (monospace or not; measured, not assumed).
float widthOf(ImFont* font, float size, const char* s, const char* e) {
    return s >= e ? 0.0f : font->CalcTextSizeA(size, FLT_MAX, 0.0f, s, e).x;
}

// The byte offset in [begin,end) whose glyph the x position falls on (or `end` past the line).
std::size_t offsetAtX(ImFont* font, float size, const std::string& text, std::size_t begin, std::size_t end, float x) {
    float acc = 0.0f;
    std::size_t p = begin;
    while (p < end) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, text.c_str() + p, text.c_str() + end);
        const std::size_t next = p + static_cast<std::size_t>(std::max(1, n));
        const float w = widthOf(font, size, text.c_str() + p, text.c_str() + next);
        if (x < acc + w / 2) {
            return p;
        }
        acc += w;
        p = next;
    }
    return end;
}

} // namespace

void layoutSelectableDoc(const SelDoc& doc, float wrap, ImFont* font, float fontSize, SelLayout& out) {
    if (out.version == doc.version && out.wrap == wrap && out.fontSize == fontSize && out.font == font &&
        out.texts.size() == doc.paras.size()) {
        return;
    }
    out.version = doc.version;
    out.wrap = wrap;
    out.fontSize = fontSize;
    out.font = font;
    out.lines.clear();
    out.texts.assign(doc.paras.size(), std::string());
    out.blockSpan.clear();
    const float lineH = fontSize;
    const float lineGap = 1.0f;
    const float paraGap = 2.0f;
    float y = 0.0f;
    for (std::size_t pi = 0; pi < doc.paras.size(); ++pi) {
        const SelPara& p = doc.paras[pi];
        out.texts[pi] = p.text();
        const std::string& t = out.texts[pi];
        if (p.kind == 3) { // spacer: half a line, no text
            out.lines.push_back(SelLine{pi, 0, 0, y, lineH * 0.5f});
            y += lineH * 0.5f;
            continue;
        }
        if (p.kind == 4) { // rule: a line's height, drawn by the drawer
            out.lines.push_back(SelLine{pi, 0, 0, y, lineH});
            y += lineH + paraGap;
            continue;
        }
        if (p.kind == 5) { // reserved: a blank region the owner draws into (a view block's body)
            const float h = std::max(lineH, p.reserve);
            out.lines.push_back(SelLine{pi, 0, 0, y, h});
            y += h + paraGap;
            const std::size_t b5 = static_cast<std::size_t>(std::max(0, p.block));
            if (out.blockSpan.size() <= b5) {
                out.blockSpan.resize(b5 + 1, {1e9f, -1e9f});
            }
            out.blockSpan[b5].first = std::min(out.blockSpan[b5].first, out.lines.back().y);
            out.blockSpan[b5].second = std::max(out.blockSpan[b5].second, y - paraGap);
            continue;
        }
        if (t.empty()) {
            out.lines.push_back(SelLine{pi, 0, 0, y, lineH});
            y += lineH + paraGap;
        } else {
            const char* s = t.c_str();
            const char* e = s + t.size();
            while (s < e) {
                const bool first = s == t.c_str();
                const float lineWrap = wrap - p.indent + (first ? p.hang : 0.0f);
                const char* stop = font->CalcWordWrapPosition(fontSize, s, e, std::max(10.0f, lineWrap));
                if (stop <= s) {
                    stop = s + 1; // a glyph wider than the box: advance anyway
                }
                out.lines.push_back(SelLine{pi, static_cast<std::size_t>(s - t.c_str()),
                                            static_cast<std::size_t>(stop - t.c_str()), y, lineH});
                y += lineH + lineGap;
                s = stop;
                while (s < e && *s == ' ') { // wrapped-away leading space stays selectable but invisible
                    ++s;
                }
            }
            y += paraGap;
        }
        const std::size_t b = static_cast<std::size_t>(std::max(0, p.block));
        if (out.blockSpan.size() <= b) {
            out.blockSpan.resize(b + 1, {1e9f, -1e9f});
        }
        out.blockSpan[b].first = std::min(out.blockSpan[b].first, out.lines.back().y);
        out.blockSpan[b].second = std::max(out.blockSpan[b].second, y - paraGap);
    }
    out.height = y;
}

SelResult drawSelectableDoc(const SelDoc& doc, SelState& st, float wrap, ImFont* font, float fontSize,
                            bool interactive, SelLayout* cache) {
    SelResult res;
    if (font == monoFont()) {
        fontSize = monoPixels(); // SETTINGS, FONT: monospace has a size of its own
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float lineH = fontSize; // ImGui's text line height at this size
    const float lineGap = 1.0f;   // between visual lines of one paragraph
    SelLayout local;
    SelLayout& lay = cache ? *cache : local;
    layoutSelectableDoc(doc, wrap, font, fontSize, lay);
    const std::vector<SelLine>& lines = lay.lines;
    const std::vector<std::string>& texts = lay.texts;
    const std::vector<std::pair<float, float>>& blockSpan = lay.blockSpan;
    const float y = lay.height;
    res.height = y;

    // ---- input
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool hovered = interactive && ImGui::IsWindowHovered() && mouse.x >= origin.x && mouse.x < origin.x + wrap &&
                         mouse.y >= origin.y && mouse.y < origin.y + y;
    auto lineAt = [&](float my, bool clamp) -> const SelLine* {
        const float ly = my - origin.y;
        if (lines.empty()) {
            return nullptr;
        }
        if (ly < 0) {
            return clamp ? &lines.front() : nullptr;
        }
        // Lines are in y order: the last one starting at or above ly.
        auto it = std::upper_bound(lines.begin(), lines.end(), ly,
                                   [](float v, const SelLine& l) { return v < l.y; });
        if (it != lines.begin()) {
            --it;
            if (ly >= it->y && ly < it->y + it->h + lineGap) {
                return &*it;
            }
        }
        return clamp ? &lines.back() : nullptr;
    };
    auto lineX = [&](const SelLine& l) {
        const SelPara& p = doc.paras[l.para];
        return origin.x + p.indent - (l.begin == 0 ? p.hang : 0.0f);
    };
    auto offsetAt = [&](const SelLine& l, float mx) -> long long {
        const SelPara& p = doc.paras[l.para];
        if (p.kind == 3 || p.kind == 4 || p.kind == 5) {
            return static_cast<long long>(doc.starts[l.para]);
        }
        const std::size_t off = offsetAtX(font, fontSize, texts[l.para], l.begin, l.end, mx - lineX(l));
        return static_cast<long long>(doc.starts[l.para] + off);
    };
    if (hovered) {
        if (const SelLine* l = lineAt(mouse.y, false)) {
            res.hoveredPara = static_cast<int>(l->para);
            res.hoveredOffset = offsetAt(*l, mouse.x);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                res.rightClickOffset = res.hoveredOffset;
            }
        }
    }
    if (interactive && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const SelLine* l = lineAt(mouse.y, false);
        const int clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
        if (!l) {
            st.clear();
            res.changed = true;
        } else if (doc.paras[l->para].kind == 2) {
            res.clickedPara = static_cast<int>(l->para);
        } else if (clicks >= 3) {
            st.clear();
            st.block = doc.paras[l->para].block;
            res.changed = true;
        } else if (clicks == 2) {
            const long long o = offsetAt(*l, mouse.x);
            const auto [a, b] = wordAt(doc.all, static_cast<std::size_t>(std::max(0LL, o)));
            st.block = -1;
            st.anchor = static_cast<long long>(a);
            st.extent = static_cast<long long>(b);
            st.dragging = false;
            res.changed = true;
        } else {
            st.block = -1;
            st.anchor = st.extent = offsetAt(*l, mouse.x);
            st.dragging = true;
            res.changed = true;
        }
    }
    if (st.dragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (const SelLine* l = lineAt(mouse.y, true)) {
                const long long o = offsetAt(*l, mouse.x);
                if (o != st.extent) {
                    st.extent = o;
                    res.changed = true;
                }
            }
        } else {
            st.dragging = false;
            if (st.anchor >= 0 && st.anchor == st.extent) {
                res.clickedOffset = st.anchor; // pressed and released in place: a click, not a selection
            }
        }
    }
    if (interactive && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && st.any()) {
            st.clear();
            res.changed = true;
        }
        if ((ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_C) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) &&
            st.any()) {
            ImGui::SetClipboardText(selectedText(doc, st).c_str());
        }
    }
    // Cmd-A or Ctrl-A over this text, unless a field has the keyboard: all of it, as a text field would.
    if (interactive && hovered && !io.WantTextInput &&
        (ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_A) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))) {
        st.block = -1;
        st.anchor = 0;
        st.extent = static_cast<long long>(doc.all.size());
    }
    // Right click on a selection, or a long press on one: the same words as a text field.
    const bool longPress = hovered && st.any() && io.MouseSource == ImGuiMouseSource_TouchScreen &&
                           ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) > 0 && io.MouseDownDuration[0] > 0.5f;
    if (interactive && ((res.rightClickOffset >= 0 && st.any()) || longPress)) {
        ImGui::OpenPopup("##docmenu");
    }
    if (ImGui::BeginPopup("##docmenu")) {
        if (menuItem("COPY", nullptr, false, st.any())) {
            ImGui::SetClipboardText(selectedText(doc, st).c_str());
        }
        drive::hit("menu:copy", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (menuItem("SELECT ALL")) {
            st.block = -1;
            st.anchor = 0;
            st.extent = static_cast<long long>(doc.all.size());
        }
        ImGui::EndPopup();
    }
    // Clamp to the document (the text may have changed under the selection).
    const long long total = static_cast<long long>(doc.all.size());
    st.anchor = std::min(st.anchor, total);
    st.extent = std::min(st.extent, total);

    // ---- draw
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const long long selA = st.hasText() ? std::min(st.anchor, st.extent) : -1;
    const long long selB = st.hasText() ? std::max(st.anchor, st.extent) : -1;
    const float clipTop = ImGui::GetWindowPos().y - lineH;
    const float clipBottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() + lineH;
    for (const SelLine& l : lines) {
        const float sy = origin.y + l.y;
        if (sy > clipBottom || sy + l.h < clipTop) {
            continue;
        }
        const SelPara& p = doc.paras[l.para];
        if (p.kind == 4) { // a dim rule across the width
            const float ry = std::floor(sy + l.h / 2) + 0.5f;
            dl->AddLine(ImVec2(origin.x, ry), ImVec2(origin.x + wrap, ry), Ink::secondary);
            continue;
        }
        if (p.kind == 3 || p.kind == 5 || l.begin == l.end) {
            continue;
        }
        const float x0line = lineX(l);
        if (p.quote) { // blockquote: a 2 px dim bar left of the text
            const float bx = std::floor(origin.x + p.indent - fontSize * 0.6f);
            dl->AddRectFilled(ImVec2(bx, sy), ImVec2(bx + 2.0f, sy + l.h + lineGap), Surface::hover);
        }
        const std::string& t = texts[l.para];
        const long long base = static_cast<long long>(doc.starts[l.para]);
        // Selection highlight behind this line's selected span.
        if (selA >= 0) {
            const long long la = base + static_cast<long long>(l.begin);
            const long long lb = base + static_cast<long long>(l.end);
            const long long a = std::max(selA, la);
            const long long b = std::min(selB, lb);
            if (a < b || (selA <= la && selB > lb)) {
                const float x0 = x0line + widthOf(font, fontSize, t.c_str() + l.begin, t.c_str() + static_cast<std::size_t>(std::max(a, la) - base));
                const float x1 = x0line + widthOf(font, fontSize, t.c_str() + l.begin, t.c_str() + static_cast<std::size_t>(std::min(std::max(b, la), lb) - base));
                dl->AddRectFilled(ImVec2(x0, sy), ImVec2(std::max(x1, x0 + 2.0f), sy + l.h), Surface::selection);
            }
        }
        // Runs, split by the visual line and the selection.
        float x = x0line;
        std::size_t runStart = 0;
        for (const SelRun& r : p.runs) {
            const std::size_t rs = runStart;
            const std::size_t re = runStart + r.text.size();
            runStart = re;
            const std::size_t a = std::max(rs, l.begin);
            const std::size_t b = std::min(re, l.end);
            if (a >= b) {
                continue;
            }
            // Up to three pieces: before, inside, after the selection.
            std::size_t cuts[4] = {a, a, a, b};
            if (selA >= 0) {
                const std::size_t sa = static_cast<std::size_t>(std::clamp(selA - base, static_cast<long long>(a), static_cast<long long>(b)));
                const std::size_t sb = static_cast<std::size_t>(std::clamp(selB - base, static_cast<long long>(a), static_cast<long long>(b)));
                cuts[1] = sa;
                cuts[2] = sb;
            }
            for (int k = 0; k < 3; ++k) {
                const std::size_t ps = cuts[k];
                const std::size_t pe = cuts[k + 1];
                if (ps >= pe) {
                    continue;
                }
                const bool inSel = k == 1;
                const float pw = widthOf(font, fontSize, t.c_str() + ps, t.c_str() + pe);
                if (r.style == SelRun::CodeBox && !inSel) { // inline code: a raised black box
                    dl->AddRectFilled(ImVec2(x - 1.0f, sy - 1.0f), ImVec2(x + pw + 1.0f, sy + l.h), Surface::code);
                    dl->AddRect(ImVec2(x - 1.0f, sy - 1.0f), ImVec2(x + pw + 1.0f, sy + l.h), Surface::codeBorder);
                }
                dl->AddText(font, fontSize, ImVec2(x, sy), inSel ? Ink::onSelection : r.color, t.c_str() + ps, t.c_str() + pe);
                if (r.style == SelRun::Underline) { // a link: underlined, never opened by the listener
                    const float uy = std::floor(sy + l.h - 1.0f) + 0.5f;
                    dl->AddLine(ImVec2(x, uy), ImVec2(x + pw, uy), inSel ? Ink::onSelection : r.color);
                }
                x += pw;
            }
        }
    }
    if (st.block >= 0 && static_cast<std::size_t>(st.block) < blockSpan.size() && blockSpan[static_cast<std::size_t>(st.block)].second > 0) {
        const auto& span = blockSpan[static_cast<std::size_t>(st.block)];
        dl->AddRect(ImVec2(origin.x - 3.0f, origin.y + span.first - 2.0f),
                    ImVec2(origin.x + wrap + 3.0f, origin.y + span.second + 2.0f), Ink::nav);
    }
    ImGui::Dummy(ImVec2(wrap, y));
    return res;
}

bool paraOffsetPos(const SelDoc& doc, const SelLayout& lay, ImFont* font, float fontSize, ImVec2 origin, std::size_t para,
                   std::size_t offset, ImVec2& pos, float& lineH) {
    if (para >= doc.paras.size() || para >= lay.texts.size()) {
        return false;
    }
    const SelPara& p = doc.paras[para];
    for (const SelLine& l : lay.lines) {
        if (l.para == para && offset >= l.begin && offset <= l.end) {
            const std::string& t = lay.texts[para];
            const float x0 = origin.x + p.indent - (l.begin == 0 ? p.hang : 0.0f);
            pos = ImVec2(x0 + widthOf(font, fontSize, t.c_str() + l.begin, t.c_str() + offset), origin.y + l.y);
            lineH = l.h;
            return true;
        }
    }
    return false;
}

} // namespace ke::ui
