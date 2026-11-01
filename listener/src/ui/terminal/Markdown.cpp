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
        // Corner: language tag and [ COPY ]. The bracket is an overlay item at an explicit
        // rectangle (ItemAdd + ButtonBehavior), so the layout cursor never moves for it.
        const std::string copyLabel = "[ COPY ]";
        const float copyW = ImGui::CalcTextSize(copyLabel.c_str()).x;
        float x = b.x - copyW - cw;
        const ImRect bb(ImVec2(x, a.y + 1.0f), ImVec2(x + copyW, a.y + 1.0f + lh));
        const ImGuiID id = ImGui::GetID(n++);
        bool hov = false, held = false, pressed = false;
        if (ImGui::ItemAdd(bb, id)) {
            pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
        }
        if (hov) {
            dl->AddRectFilled(bb.Min, bb.Max, Surface::hover);
        }
        dl->AddText(bb.Min, hov ? Ink::onSelection : Ink::primary, copyLabel.c_str());
        if (pressed) {
            ImGui::SetClipboardText(cb.source.c_str());
        }
        if (hov) {
            ImGui::SetTooltip("copy the code block");
        }
        if (!cb.lang.empty()) {
            const std::string tag = upper(cb.lang);
            x -= ImGui::CalcTextSize(tag.c_str()).x + 2 * cw;
            dl->AddText(ImVec2(x, a.y + 1.0f), Ink::secondary, tag.c_str());
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
            ImGui::SetTooltip("link: %s\nright-click to copy; the listener never opens links", urls.c_str());
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
