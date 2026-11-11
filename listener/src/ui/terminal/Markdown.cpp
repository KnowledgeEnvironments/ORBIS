#include "ui/Fonts.hpp"
#include "ui/MathView.hpp"
#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

namespace term {

// The drive API's `link:<text>` targets: what is on screen, since a test clicks what a person could.
void registerSymbolHits(const MdExtras& md, const SelDoc& doc, const SelLayout& lay, ImFont* font, ImVec2 origin) {
    if (!drive::active()) {
        return;
    }
    std::set<std::size_t> visible;
    const float top = ImGui::GetWindowPos().y;
    const float bottom = top + ImGui::GetWindowHeight();
    for (const SelLine& l : lay.lines) {
        const float y = origin.y + l.y;
        if (y >= top && y + l.h <= bottom) {
            visible.insert(l.para);
        }
    }
    for (const MdSymbol& m : md.symbols) {
        if (visible.count(m.para) == 0) {
            continue;
        }
        std::size_t at = 0;
        const std::vector<SelRun>& runs = doc.paras[m.para].runs;
        for (std::size_t r = 0; r < m.run && r < runs.size(); ++r) {
            at += runs[r].text.size();
        }
        ImVec2 pos;
        float h = 0.0f;
        if (paraOffsetPos(doc, lay, font, ImGui::GetFontSize(), origin, m.para, at, pos, h)) {
            // A link that wraps: its first line, the part a person can be sure of clicking.
            const float w = std::min(ImGui::CalcTextSize(m.symbol.display.c_str()).x, origin.x + lay.wrap - pos.x);
            drive::hit("link:" + m.symbol.display, pos, ImVec2(pos.x + w, pos.y + h));
        }
    }
}

namespace {

// The typeset boxes over a document's hidden source. An inline span that wrapped across lines has
// nowhere to sit, so it shows its source.
void drawMathInDoc(const MdExtras& md, const SelDoc& doc, const SelLayout& lay, ImVec2 origin, float wrap, int& n) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = monoFont();
    const float size = monoPixels();
    for (const MdMath& m : md.math) {
        if (m.display) {
            float y0 = 0.0f, y1 = 0.0f;
            if (paraSpan(lay, m.para, m.para, y0, y1)) {
                drawDisplayMath(m.source, ImVec2(origin.x, origin.y + y0), ImVec2(origin.x + wrap, origin.y + y1), n++);
            }
            continue;
        }
        std::size_t at = 0;
        const std::vector<SelRun>& runs = doc.paras[m.para].runs;
        for (std::size_t r = 0; r < m.run && r < runs.size(); ++r) {
            at += runs[r].text.size();
        }
        const std::size_t len = m.run < runs.size() ? runs[m.run].text.size() : 0;
        ImVec2 a, b;
        float lh = 0.0f;
        if (!paraOffsetPos(doc, lay, font, size, origin, m.para, at, a, lh) ||
            !paraOffsetPos(doc, lay, font, size, origin, m.para, at + len, b, lh)) {
            continue;
        }
        if (a.y != b.y) {
            dl->AddText(a, Ink::secondary, runs[m.run].text.c_str());
            continue;
        }
        drawInlineMath(m.source, runs[m.run].text, a, b.x - a.x, lh);
    }
}

} // namespace

// Code block frames with a language tag and [ COPY ], and the link tooltips, over a drawn
// SelDoc. Pure draw-list work plus one InvisibleButton per code block, after the document.
// The document is drawn in channel 1 of `split`; the code blocks' raised fill goes to channel 0
// so it sits behind the text, the frames and brackets on top in channel 2.
void drawMarkdownExtras(const MdExtras& md, const SelDoc& doc, const SelLayout& lay, ImVec2 origin, float wrap,
                        int hoveredPara, const char* key, ImDrawListSplitter* split = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = ImGui::CalcTextSize("0").x;
    const float lh = ImGui::GetTextLineHeight();
    ImGui::PushID(key);
    int n = 0;
    for (const MdCodeBlock& cb : md.codeBlocks) {
        float y0 = 0.0f, y1 = 0.0f;
        if (!paraSpan(lay, cb.firstPara, cb.lastPara, y0, y1)) {
            continue;
        }
        const ImVec2 a(origin.x + 1.0f, origin.y + y0 - 2.0f);
        const ImVec2 b(origin.x + wrap - 1.0f, origin.y + y1 + 2.0f);
        if (split) {
            split->SetCurrentChannel(dl, 0);
            dl->AddRectFilled(a, b, Surface::code);
            split->SetCurrentChannel(dl, 2);
        }
        outlineRect(dl, a, b, Ink::secondary);
        // The title band: the block's first row (a reserved paragraph, ui/Markdown), tinted like a
        // view block's bar. The language on the left, dim; `copy` on the right, a quiet word that
        // says `copied` for a moment after a click. Nothing is drawn over the code.
        float h0 = 0.0f, h1 = 0.0f;
        const bool hasBand = doc.paras[cb.firstPara].kind == 5 && paraSpan(lay, cb.firstPara, cb.firstPara, h0, h1);
        const float bandY0 = a.y + 1.0f;
        const float bandY1 = hasBand ? origin.y + h1 : a.y + 1.0f + lh;
        if (hasBand) {
            if (split) {
                split->SetCurrentChannel(dl, 0);
            }
            dl->AddRectFilled(ImVec2(a.x + 1.0f, bandY0), ImVec2(b.x - 1.0f, bandY1), Surface::hover);
            if (split) {
                split->SetCurrentChannel(dl, 2);
            }
        }
        const float textY = std::floor(bandY0 + (bandY1 - bandY0 - lh) * 0.5f);
        if (!cb.lang.empty()) {
            dl->AddText(ImVec2(a.x + cw, textY), Ink::secondary, cb.lang.c_str());
        }
        static int copiedId = -1;
        static double copiedAt = -10.0;
        const ImGuiID id = ImGui::GetID(n++);
        const bool justCopied = copiedId == static_cast<int>(id) && ImGui::GetTime() - copiedAt < 1.5;
        const char* word = justCopied ? "copied" : "copy";
        const float wordW = ImGui::CalcTextSize(word).x;
        const ImRect bb(ImVec2(b.x - cw - wordW, textY), ImVec2(b.x - cw, textY + lh));
        bool hov = false, held = false, pressed = false;
        if (ImGui::ItemAdd(bb, id)) {
            pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
        }
        drive::hit("button:copy-code:" + std::to_string(n - 1), bb.Min, bb.Max);
        dl->AddText(bb.Min, justCopied ? Ink::secondary : Ink::nav, word);
        if (hov && !justCopied) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            dl->AddLine(ImVec2(bb.Min.x, bb.Max.y - 1.0f), ImVec2(bb.Max.x, bb.Max.y - 1.0f), Ink::nav);
            ke::ui::tip("copy the code block");
        }
        if (pressed) {
            ImGui::SetClipboardText(cb.source.c_str());
            copiedId = static_cast<int>(id);
            copiedAt = ImGui::GetTime();
        }
    }
    drawMathInDoc(md, doc, lay, origin, wrap, n);
    // Links: the url as a tooltip on the paragraph, right-click copies it.
    if (hoveredPara >= 0) {
        std::string urls;
        for (const MdLink& l : md.links) {
            if (l.para == static_cast<std::size_t>(hoveredPara)) {
                urls += (urls.empty() ? "" : "\n") + l.url;
            }
        }
        if (!urls.empty()) {
            ke::ui::tip("link: %s\nright-click to copy; the listener never opens links", urls.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::SetClipboardText(urls.c_str());
            }
        }
    }
    (void)doc;
    ImGui::PopID();
}

} // namespace term

} // namespace ke::ui
