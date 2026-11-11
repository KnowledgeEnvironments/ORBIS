// Text the user can select by the character. ImGui's text widgets are not selectable, so the
// transcript is drawn here by hand: paragraphs of coloured runs, word-wrapped to a width, with
// the mouse mapping to character offsets in one flat document. Rules (docs/listener-style.md):
//   drag            selects characters across lines, inverse video
//   double-click    selects the word; triple-click selects the whole block (a thin cyan frame)
//   click on space  clears; Escape clears
//   Cmd-C           copies exactly the selected characters (or the block's text)
// Layout, then input, then draw. The word-wrap layout is the expensive part, so a caller that
// keeps a SelLayout gets it reused while the document version, the width and the font size are
// unchanged; the selection offsets are clamped so the fold can change the text under them.
#pragma once

#include <imgui.h>
#include <string>
#include <utility>
#include <vector>

namespace ke::ui {

struct SelRun {
    std::string text;
    ImU32 color;
    enum Style : unsigned char { None = 0, CodeBox = 1, Underline = 2 };
    unsigned char style = None; // CodeBox: inline code on a raised black box; Underline: a link
};

struct SelPara {
    std::vector<SelRun> runs;
    int block = 0;   // the transcript line this paragraph belongs to
    int kind = 0;    // 0 text, 1 head (the caller shows a tooltip on hover), 2 toggle (click acts), 3 spacer, 4 rule, 5 reserved (see reserve)
    float indent = 0.0f; // x offset of the paragraph's lines (lists, quotes, code blocks)
    float hang = 0.0f;   // the first line starts `hang` left of `indent` (its bullet or number)
    bool quote = false;  // a dim bar on the left
    bool code = false;   // part of a fenced code block (the owner frames the span)
    float reserve = 0.0f; // kind 5: a blank region of this height, drawn over by the owner (a view block)
    std::string source;  // the markdown this paragraph was rendered from; block copies yield it
    std::string text() const;
};

struct SelDoc {
    unsigned long long version = 0; // bumped by the owner whenever paras change (layout cache key)
    std::vector<SelPara> paras;
    std::string all;                // paragraphs joined by '\n'
    std::vector<std::size_t> starts; // offset of each paragraph in `all`
    void add(SelPara p);
    void add(std::string text, ImU32 color, int block, int kind = 0);
    void finish();
    std::string blockText(int block) const;
    std::size_t paraOfOffset(std::size_t offset) const;
};

struct SelState {
    long long anchor = -1; // character offsets into SelDoc::all, -1 none
    long long extent = -1;
    int block = -1;        // a whole-block selection (triple-click)
    bool dragging = false;
    bool hasText() const { return anchor >= 0 && extent >= 0 && anchor != extent; }
    bool any() const { return hasText() || block >= 0; }
    void clear() {
        anchor = extent = -1;
        block = -1;
        dragging = false;
    }
};

// A visual line of the laid-out document.
struct SelLine {
    std::size_t para;
    std::size_t begin; // byte offsets into the paragraph text
    std::size_t end;
    float y;
    float h;
};

// The layout of one SelDoc at one width: kept by the caller across frames.
struct SelLayout {
    unsigned long long version = ~0ULL;
    float wrap = -1.0f;
    float fontSize = -1.0f;
    const void* font = nullptr;
    std::vector<SelLine> lines;
    std::vector<std::string> texts;                  // paragraph texts (runs joined)
    std::vector<std::pair<float, float>> blockSpan; // y0, y1 per block index
    float height = 0.0f;
};

struct SelResult {
    int hoveredPara = -1;
    int clickedPara = -1; // a kind-2 paragraph that was clicked this frame
    long long hoveredOffset = -1;    // the character under the mouse, in SelDoc::all
    long long clickedOffset = -1;    // a left click that did not drag, released this frame
    long long rightClickOffset = -1; // a right click this frame
    bool changed = false; // the selection changed (a frame is due)
    float height = 0.0f;  // what was laid out
};

// Draws `doc` from the current cursor, `wrap` wide, handles the mouse and Cmd-C / Escape when
// `interactive`, and advances the cursor past it. Selection colours: inverse video for text,
// a thin cyan frame for a block.
SelResult drawSelectableDoc(const SelDoc& doc, SelState& st, float wrap, ImFont* font, float fontSize,
                            bool interactive, SelLayout* cache = nullptr);
// Lays `doc` out `wrap` wide into `out` (reusing it when the key matches). Pure; testable without a window.
void layoutSelectableDoc(const SelDoc& doc, float wrap, ImFont* font, float fontSize, SelLayout& out);

// The selected characters (text selection) or the selected block's text; "" when none.
std::string selectedText(const SelDoc& doc, const SelState& st);
// The vertical span (y0, y1 relative to the document origin) of paragraphs [first, last] in a
// layout; false when none of them produced a line.
bool paraSpan(const SelLayout& lay, std::size_t first, std::size_t last, float& y0, float& y1);
// Where the byte `offset` of paragraph `para` lies on screen, for `origin` the document's top left.
bool paraOffsetPos(const SelDoc& doc, const SelLayout& lay, ImFont* font, float fontSize, ImVec2 origin, std::size_t para,
                   std::size_t offset, ImVec2& pos, float& lineH);
// Word boundaries around `offset` (spaces and newlines split words).
std::pair<std::size_t, std::size_t> wordAt(const std::string& all, std::size_t offset);

} // namespace ke::ui
