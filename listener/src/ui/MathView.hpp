// TeX math on screen: the box from modules/MathLayout drawn in theme colours, inline in a line of
// text or as a display block with its source one click away. Anything that does not parse shows
// its source, plainly, with the reason on hover.
#pragma once

#include <utility>
#include <vector>
#include <imgui.h>
#include <string>

namespace ke::chat {
class Chat;
}

namespace ke::ui {

// `tex` fitted into `width` at the line starting `at`. `shown` is the source a parse failure shows.
// Math on something other than the theme's surface: `ink`, 0 for the theme's. Set around a draw.
void setMathInk(ImU32 ink);
// Where a formula the layout cannot draw goes: the world typesets it, and the image comes back.
void setMathWorld(chat::Chat* chat);
// Formulas the world has typeset, and those it could not: {ready, failed}. For a test.
std::pair<int, int> typesetCounts();
// One the world was asked for, and where it was last drawn. For a test.
struct TypesetSeen {
    std::string source; // its head
    std::string reason; // empty unless it failed
    int pages = 1;
    bool page = false;
    bool failed = false;
    std::size_t log = 0;
    float left = 0, top = 0, right = 0, bottom = 0; // its paragraph
    float noteTop = 0, noteBottom = 0;
};
std::vector<TypesetSeen> typesetsSeen();
void drawInlineMath(const std::string& tex, const std::string& shown, ImVec2 at, float width, float lineHeight);
// Centred in the region [min, max], with [ SOURCE ] and [ COPY ] in its corner.
void drawDisplayMath(const std::string& tex, ImVec2 min, ImVec2 max, int id);

} // namespace ke::ui
