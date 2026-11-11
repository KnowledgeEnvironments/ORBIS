// The visual rule (docs/listener-style.md): phosphor on pure black, 1 px borders, rounding 0,
// inverse video for selection and focus, monospace only. Angular bits (chamfers, parallelogram
// tabs, slanted bars) are drawn with the draw list. Every colour here has a row in the palette
// table; nothing else appears.
#pragma once

#include "ui/ThemeSpec.hpp"

#include <imgui.h>
#include <initializer_list>
#include <string>
#include <string_view>

namespace ke::ui {

// What a colour means is the rule everywhere; the theme only says what it looks like. Panes use
// these roles and nothing else (tests/no_literal_colours.py), and they change when the theme does.
struct Ink {
    static inline ImU32 primary = 0;     // content
    static inline ImU32 secondary = 0;   // content that can wait
    static inline ImU32 strong = 0;      // what the eye should land on
    static inline ImU32 nav = 0;         // navigation, hover, current scope, links
    static inline ImU32 inspected = 0;   // the inspected object, evidence, warnings, blockers, pending approvals
    static inline ImU32 unresolved = 0;  // an abstraction, or a binding that has not resolved
    static inline ImU32 connected = 0;   // reviewed, connected, done well
    static inline ImU32 fatal = 0;       // fatal failure only
    static inline ImU32 unsafe = 0;      // approvals are off somewhere: red in every theme, never themed away
    static inline ImU32 onSelection = 0; // text over a selection
    static inline ImU32 onPaper = 0;     // text over Surface::paper
};
struct Surface {
    static inline ImU32 background = 0;
    static inline ImU32 selection = 0;
    static inline ImU32 hover = 0;
    static inline ImU32 code = 0;
    static inline ImU32 codeBorder = 0;
    static inline ImU32 paper = 0; // under a rendered page: a page is black ink on nothing
    static inline ImU32 twilightOrange = 0, twilightBlue = 0, twilightBlack = 0, twilightStar = 0; // the map's twilight field
};
// A QR code is black on white in every theme, or a phone cannot read it.
inline constexpr ImU32 kQrDark = IM_COL32(0, 0, 0, 255);
inline constexpr ImU32 kQrLight = IM_COL32(255, 255, 255, 255);

// Per-frame animation phase, computed once by the app from one clock.
struct Anim {
    double t = 0.0;            // seconds since start
    bool blinkOn = true;       // 530 ms period, half on
    const char* spinner = "|"; // current frame of | / - \ at 120 ms
};
constexpr double kBlinkPeriod = 0.530;
constexpr double kSpinnerPeriod = 0.120;
Anim animAt(double t);

enum class Family { Lifecycle, Field, Access, Transport, Other };
Family familyOf(std::string_view kind);
ImU32 colorOf(Family f);
ImU32 kindColor(std::string_view kind);

// The current theme. setTheme applies it live; applyTheme re-applies it at a new scale.
const ThemeSpec& theme();
void setTheme(const ThemeSpec& spec, float scale);
void applyTheme(float scale);
// Bumps on every setTheme: documents that hold colours rebuild when it moves.
int themeVersion();

// A terminal's colours: 0..15 from the theme, then the standard cube and greys. `rgbInk` is a
// program's own truecolour, passed through.
ImU32 ansiInk(int index);
ImU32 rgbInk(unsigned char r, unsigned char g, unsigned char b);

// ---- text ---------------------------------------------------------------------------------
void dimText(const char* fmt, ...);
void text(ImU32 color, std::string_view s);
// The same line struck twice, a pixel apart: the fonts ship one weight.
void boldText(ImU32 color, std::string_view s);
// A bracketed uppercase token in its colour: `[FIELD-SET]`. Returns true when hovered.
bool chip(std::string_view label, ImU32 color);
bool kindChip(std::string_view kind);
// A bracketed word acting as a button: `[ DROP ]`, inverse while pressed.
bool bracketButton(const char* word, bool enabled = true, bool on = false);
// The same, with a drive target name of its own (`button:<name>`) so several buttons may share
// a label (`[ APPROVE ]` on every card) and still be addressed one by one; `name` scopes the id.
// The two buttons of "are you sure": `word` and CANCEL, in the row the caller's question sits on.
// `takeFocus` is true the frame the question appears, and hands the keyboard to CANCEL: Enter, Space
// and Escape all cancel, so a stray key loses nothing. Shift on the opening click skips all this.
enum class Confirmed { Waiting, Yes, No };
Confirmed confirmRow(const char* word, const std::string& yesName, const std::string& noName, bool& takeFocus);

bool bracketButtonNamed(const char* word, const std::string& name, bool enabled = true, bool on = false,
                        ImU32 color = Ink::primary);
// A row of slanted bars filling left to right: `////....` (on in phosphor, rest in dim).
void slantBar(float fraction, int cells, ImU32 on = Ink::primary, ImU32 off = Ink::secondary);

// ---- angular ------------------------------------------------------------------------------
// A 1 px outline, or the rectangle filled.
void outlineRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color, bool filled = false);
// Begin a pane window: black, 1 px chamfered phosphor outline, and its title in a 1 px box,
// uppercase and letter-spaced (`H I S T O R Y`). `name` is the ImGui window id and the title.
// Back to where the layout was after drawing over it: no item follows, so ImGui must not read this
// as a try to grow the window.
void restoreCursor(ImVec2 at);

// Where the desktop layout put a view: its window takes this place, or is not drawn at all. A view
// with no entry is an ordinary window, as on a phone.
struct PanePlace {
    bool shown = false;
    bool floating = false;
    ImVec2 pos{0.0f, 0.0f};
    ImVec2 size{0.0f, 0.0f};
};
void placePane(const std::string& name, PanePlace place);
// A floating view's window this frame: where the person moved it, and whether they closed it.
ImVec4 paneFloatRect(const std::string& name);
bool takePaneFloatClosed(const std::string& name);
bool beginPane(const char* name, ImGuiWindowFlags flags = 0, bool title = true);
void endPane();
// Just the title box, for places that are not their own window (the phone's tab bodies).
void paneTitle(const char* title);
// The strip of plain words, for small choices inside a pane. The strip
// is one item afterwards, so SameLine() continues on its row. Returns the active index.
float tabStripHeight();
float tabStripWidth(const char* const* names, int count);
int tabStrip(const char* id, const char* const* names, int count, int active);
// A block cursor cell: inverse when on (black glyph on phosphor), hollow when the field is not
// active; `ch` is the glyph under the cursor, 0 for none.
// Hazard tape: yellow and black stripes marching while a model is thinking or generating. In the
// theme's inspected colour and its background, so a theme can change it. Advances the cursor.
void hazardSpinner(float width, double seconds);
// The same tape over any rectangle, for what is not on the layout cursor.
void hazardTape(ImDrawList* dl, ImVec2 min, ImVec2 max, double seconds);

void blockCursor(ImDrawList* dl, ImVec2 pos, ImVec2 cell, bool on, bool active, const char* ch);
// Every floating window with a title bar gets the border docked panes get from chrome(). Once a frame,
// before ImGui::Render.
void outlineFloatingWindows();
// One language of controls: `[+]` for add, and the same bracket for anything that small: a count, `[x]`.
// `squareSide` is the width of `[+]`.
float squareSide();
// The right edge of the content, in the units ImGui::SameLine takes.
float contentRight();
bool squareButton(const char* glyph, const std::string& name, bool enabled = true, bool on = false);
// A `+` at the right end of the row the cursor is on, its tooltip saying what it adds.
bool plusAtRight(const std::string& name, const char* tooltip, bool enabled = true);
// The width bracket buttons take side by side, to right-align a row of them.
float bracketRowWidth(std::initializer_list<const char*> words);
// Put the next `width` of controls at the right of this row, or of a row of their own when it is full.
void rowAtRight(float width);
// A row's fold: a small triangle, because `+` means add.
bool disclosure(bool open, const std::string& name);
// Space above a dialog's row of buttons, so they never touch the field above them.
void dialogGap();
// A quiet word that acts: text with no frame, for a control that sits inside running text.
bool textLink(const char* word, const std::string& name);
// A text area's rim, set into the panel: what scrolls under it reads as behind glass, not cut off.
void insetRim(ImDrawList* dl, ImVec2 min, ImVec2 max);
// A tooltip that wraps: help text from the world can run to a paragraph.
void wrappedTooltip(const std::string& text);
// A card is more than a label, and it should not open because the pointer passed. True with Shift held,
// so the card shows; otherwise false, and the status line says what is here and that there is more.
bool detailShown(const std::string& hint);
// A one-line tooltip. VIEW, TOOLTIPS turns these and the hints off; Shift still opens a card.
void tip(const char* fmt, ...) IM_FMTARGS(1);
void setTooltipsOn(bool on);
// What the status line shows in place of the log's latest, for the frame just drawn: "" for nothing.
std::string takeDetailHint();

// A tree's connector for one row: the vertical guide down from the parent at `x`, and the elbow
// into the child at the row's middle; a last child stops there (an L), the others run on (a tee).
// 1 px on whole pixels in the secondary ink. One helper for every tree.
void treeGuide(ImDrawList* dl, float x, float rowTop, float rowBottom, float toX, bool last);

// ---- misc ---------------------------------------------------------------------------------
std::string spaced(std::string_view s);     // "HISTORY" -> "H I S T O R Y"
// A label in the label case, caps or sentence, set by the theme or VIEW. Every label goes through
// here, so one setting changes them all; CTX and CARRIER and the like keep their capitals.
std::string upper(std::string_view s);
void setLabelCase(const std::string& labelCase); // "caps", "sentence", "" for the theme's
bool menuItem(const char* word, const char* shortcut = nullptr, bool selected = false, bool enabled = true);
bool menuItem(const char* word, const char* shortcut, bool* selected, bool enabled = true);
bool beginMenu(const char* word, bool enabled = true);
// A menu-bar word that opens a window rather than a menu (WORLD, APPROVALS, NOTICES, SEARCH): drawn like
// FILE and EDIT beside it, highlighted while its window is open; `shown` is already in the label case.
// Drive target `button:<name>`. Buttons with brackets are for doing things, not for the menu bar.
bool menuBarItem(const std::string& shown, const std::string& name, bool open, ImU32 color = Ink::primary);
ImU32 stateColor(bool attached, bool connecting);
// Touch screens have no wheel: a finger dragged over the current window (or table) scrolls it.
// No-op for a mouse. Returns true when it scrolled this frame.
bool touchDragScroll();

} // namespace ke::ui
