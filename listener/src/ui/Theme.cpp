#include "ui/Theme.hpp"

#include "ui/Fonts.hpp"

#include "drive/Commands.hpp"
#include "ui/Chrome.hpp"

#include <map>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <imgui_internal.h>

namespace ke::ui {

Anim animAt(double t) {
    Anim a;
    a.t = t;
    a.blinkOn = std::fmod(t, kBlinkPeriod) < kBlinkPeriod / 2.0;
    static constexpr const char* frames[] = {"|", "/", "-", "\\"};
    a.spinner = frames[static_cast<int>(t / kSpinnerPeriod) % 4];
    return a;
}

Family familyOf(std::string_view kind) {
    if (kind == "created" || kind == "retired" || kind == "attached" || kind == "detached" ||
        kind == "bound" || kind == "unbound") {
        return Family::Lifecycle;
    }
    if (kind == "field-set" || kind == "field-read") {
        return Family::Field;
    }
    if (kind == "resolved" || kind == "granted" || kind == "revoked") {
        return Family::Access;
    }
    if (kind == "sent" || kind == "delivered") {
        return Family::Transport;
    }
    return Family::Other; // task-moved, evaluated, fired
}

ImU32 colorOf(Family f) {
    switch (f) {
    case Family::Lifecycle:
        return Ink::nav;
    case Family::Field:
        return Ink::inspected;
    case Family::Access:
        return Ink::unresolved;
    case Family::Transport:
        return Ink::nav; // messages are references between principals
    case Family::Other:
        return Ink::primary;
    }
    return Ink::primary;
}

ImU32 kindColor(std::string_view kind) {
    return colorOf(familyOf(kind));
}

namespace {
ThemeSpec g_theme;
int g_themeVersion = 0;
float g_scale = 1.0f; // the UI scale applyTheme was given; tab metrics are in points at scale 1
constexpr float kTabPadX = 8.0f;
constexpr float kTabPadY = 3.0f;
constexpr float kTabGap = 6.0f;
constexpr float kTabRowGap = 4.0f;
} // namespace

void applyTheme(float scale) {
    g_scale = scale;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle(); // reset before scaling so repeated calls stay stable
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 0.0f; // panes draw their own chamfered outline
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.DockingSeparatorSize = 1.0f;
    style.WindowPadding = ImVec2(8, 6);
    style.FramePadding = ImVec2(5, 2);
    style.CellPadding = ImVec2(5, 1); // tight rows
    style.ItemSpacing = ImVec2(7, 3);
    style.ItemInnerSpacing = ImVec2(5, 3);
    style.ScrollbarSize = 8.0f;
    style.GrabMinSize = 8.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.AntiAliasedLines = false; // crisp 1 px lines and sharp diagonals
    style.AntiAliasedFill = false;
    style.ScaleAllSizes(scale);
    style.InputTextCursorSize = std::max(2.0f, std::floor(2.0f * scale)); // seen at a glance, not a hairline
    style.FontScaleDpi = scale;

    const ThemeSpec& t = g_theme;
    auto color = [](Rgba c) { return ImGui::ColorConvertU32ToFloat4(c); };
    const ImVec4 primary = color(t.ink.primary);
    const ImVec4 secondary = color(t.ink.secondary);
    const ImVec4 pane = color(t.surface.pane);
    const ImVec4 well = color(t.surface.well);
    const ImVec4 border = color(t.surface.border);
    const ImVec4 rule = color(t.surface.rule);
    const ImVec4 selection = color(t.surface.selection);
    const ImVec4 hover = color(t.surface.hover);
    const ImVec4 thumb = color(t.surface.scrollThumb);
    const ImVec4 thumbHover = color(t.surface.scrollThumbHover);
    const ImVec4 none = ImVec4(0, 0, 0, 0);

    ImVec4* c = style.Colors;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        c[i] = pane;
    }
    c[ImGuiCol_Text] = primary;
    c[ImGuiCol_TextDisabled] = secondary;
    c[ImGuiCol_WindowBg] = pane;
    c[ImGuiCol_ChildBg] = well;
    c[ImGuiCol_PopupBg] = pane;
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = well;
    c[ImGuiCol_FrameBgHovered] = well;
    c[ImGuiCol_FrameBgActive] = selection; // the field you are typing in is lit
    c[ImGuiCol_TitleBg] = pane;
    c[ImGuiCol_TitleBgActive] = pane;
    c[ImGuiCol_TitleBgCollapsed] = pane;
    c[ImGuiCol_MenuBarBg] = pane;
    c[ImGuiCol_ScrollbarBg] = color(t.surface.scrollTrack);
    c[ImGuiCol_ScrollbarGrab] = thumb;
    c[ImGuiCol_ScrollbarGrabHovered] = thumbHover;
    c[ImGuiCol_ScrollbarGrabActive] = thumbHover;
    c[ImGuiCol_CheckMark] = primary;
    c[ImGuiCol_SliderGrab] = primary;
    c[ImGuiCol_SliderGrabActive] = primary;
    c[ImGuiCol_Button] = color(t.surface.control);
    c[ImGuiCol_ButtonHovered] = color(t.surface.control);
    c[ImGuiCol_ButtonActive] = selection;
    c[ImGuiCol_Header] = selection;
    c[ImGuiCol_HeaderHovered] = hover;
    c[ImGuiCol_HeaderActive] = selection;
    c[ImGuiCol_Separator] = rule;
    c[ImGuiCol_SeparatorHovered] = thumbHover;
    c[ImGuiCol_SeparatorActive] = thumbHover;
    c[ImGuiCol_ResizeGrip] = thumb;
    c[ImGuiCol_ResizeGripHovered] = thumbHover;
    c[ImGuiCol_ResizeGripActive] = thumbHover;
    c[ImGuiCol_Tab] = pane;
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_TabSelected] = selection;
    c[ImGuiCol_TabSelectedOverline] = selection;
    c[ImGuiCol_TabDimmed] = pane;
    c[ImGuiCol_TabDimmedSelected] = hover;
    c[ImGuiCol_TabDimmedSelectedOverline] = hover;
    c[ImGuiCol_DockingPreview] = hover;
    c[ImGuiCol_DockingEmptyBg] = color(t.surface.background);
    c[ImGuiCol_PlotLines] = primary;
    c[ImGuiCol_PlotHistogram] = primary;
    c[ImGuiCol_TableHeaderBg] = pane;
    c[ImGuiCol_TableBorderStrong] = rule;
    c[ImGuiCol_TableBorderLight] = rule;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = none; // no tints
    c[ImGuiCol_TextSelectedBg] = hover;
    c[ImGuiCol_InputTextCursor] = color(t.ink.inspected); // the insertion point stands out in every field
    c[ImGuiCol_DragDropTarget] = thumbHover;
    c[ImGuiCol_NavCursor] = thumbHover;
    c[ImGuiCol_NavWindowingHighlight] = thumbHover;
    c[ImGuiCol_NavWindowingDimBg] = none;
    c[ImGuiCol_ModalWindowDimBg] = none;
}

void setTheme(const ThemeSpec& spec, float scale) {
    g_theme = spec;
    ++g_themeVersion;
    Ink::primary = spec.ink.primary;
    Ink::secondary = spec.ink.secondary;
    Ink::strong = spec.ink.strong;
    Ink::nav = spec.ink.nav;
    Ink::inspected = spec.ink.inspected;
    Ink::unresolved = spec.ink.unresolved;
    Ink::connected = spec.ink.connected;
    Ink::fatal = spec.ink.fatal;
    Ink::unsafe = spec.ink.unsafe;
    Ink::onSelection = spec.ink.onSelection;
    Ink::onPaper = spec.ink.onPaper;
    Surface::background = spec.surface.background;
    Surface::selection = spec.surface.selection;
    Surface::hover = spec.surface.hover;
    Surface::code = spec.surface.code;
    Surface::codeBorder = spec.surface.codeBorder;
    Surface::paper = spec.surface.paper;
    Surface::twilightOrange = spec.surface.twilightOrange;
    Surface::twilightBlue = spec.surface.twilightBlue;
    Surface::twilightBlack = spec.surface.twilightBlack;
    Surface::twilightStar = spec.surface.twilightStar;
    applyTheme(scale);
}

const ThemeSpec& theme() {
    return g_theme;
}

int themeVersion() {
    return g_themeVersion;
}

// ---- text -----------------------------------------------------------------------------------

void dimText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    // A heading typed in capitals is a label, and follows the label case.
    const std::string_view s(buf);
    const bool heading = !s.empty() && std::none_of(s.begin(), s.end(), [](unsigned char c) { return std::islower(c) != 0; });
    ImGui::PushStyleColor(ImGuiCol_Text, Ink::secondary);
    ImGui::TextUnformatted(heading ? upper(s).c_str() : buf);
    ImGui::PopStyleColor();
}

void text(ImU32 color, std::string_view s) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(s.data(), s.data() + s.size());
    ImGui::PopStyleColor();
}

namespace {

std::string g_labelCase; // the client's choice, "" for the theme's

bool sentenceLabels() { return g_labelCase.empty() ? g_theme.sentenceLabels : g_labelCase == "sentence"; }

std::string allCaps(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// Words that are names, not labels: they keep their capitals in either case.
bool keepsCaps(std::string_view word) {
    static constexpr std::string_view kKept[] = {"CTX", "CARRIER", "TEX", "URL", "ID", "OK", "UNSAFE", "EVAL"};
    return std::find(std::begin(kKept), std::end(kKept), word) != std::end(kKept);
}

} // namespace

void setLabelCase(const std::string& labelCase) { g_labelCase = labelCase; }

std::string upper(std::string_view s) {
    const std::string caps = allCaps(s);
    if (!sentenceLabels()) {
        return caps;
    }
    std::string out;
    bool first = true;
    std::size_t at = 0;
    while (at <= caps.size()) {
        std::size_t end = at;
        while (end < caps.size() && std::isalnum(static_cast<unsigned char>(caps[end])) != 0) {
            ++end;
        }
        const std::string_view word = std::string_view(caps).substr(at, end - at);
        if (keepsCaps(word)) {
            out += word;
            first = first && word.empty();
        } else {
            for (char c : word) {
                out += first ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                first = false;
            }
        }
        if (end < caps.size()) {
            out += caps[end];
        }
        at = end + 1;
    }
    return out;
}

// Every enabled menu item is a drive target, menu:<word> in lower case with hyphens, so none goes
// untested. A disabled one is no target: nothing can click it.
static void menuTarget(const char* word) {
    std::string name = "menu:";
    for (const char* c = word; *c != '\0'; ++c) {
        name += *c == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    }
    drive::hit(name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}
bool menuItem(const char* word, const char* shortcut, bool selected, bool enabled) {
    const bool clicked = ImGui::MenuItem(upper(word).c_str(), shortcut, selected, enabled);
    if (enabled) {
        menuTarget(word);
    }
    return clicked;
}
bool menuItem(const char* word, const char* shortcut, bool* selected, bool enabled) {
    const bool clicked = ImGui::MenuItem(upper(word).c_str(), shortcut, selected, enabled);
    if (enabled) {
        menuTarget(word);
    }
    return clicked;
}
bool beginMenu(const char* word, bool enabled) { return ImGui::BeginMenu((upper(word) + "###" + word).c_str(), enabled); }


std::string spaced(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i) {
            out += ' ';
        }
        out += s[i];
    }
    return out;
}

bool chip(std::string_view label, ImU32 color) {
    const std::string token = "[" + upper(label) + "]";
    text(color, token);
    return ImGui::IsItemHovered();
}

bool kindChip(std::string_view kind) {
    return chip(kind, kindColor(kind));
}

Confirmed confirmRow(const char* word, const std::string& yesName, const std::string& noName, bool& takeFocus) {
    const bool yes = bracketButtonNamed(word, yesName);
    ImGui::SameLine();
    if (takeFocus) {
        ImGui::ClearActiveID(); // a text field left open would swallow the Enter meant for CANCEL
        takeFocus = false;
    }
    const bool no = bracketButtonNamed("cancel", noName);
    // CANCEL is what the keyboard does here, so it wears the mark of the default.
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Ink::nav);
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool enter = !typing && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                   ImGui::IsKeyPressed(ImGuiKey_Space, false));
    if (no || enter || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        return Confirmed::No;
    }
    return yes ? Confirmed::Yes : Confirmed::Waiting;
}

bool bracketButton(const char* word, bool enabled, bool on) {
    return bracketButtonNamed(word, word, enabled, on, Ink::primary);
}

bool bracketButtonNamed(const char* word, const std::string& name, bool enabled, bool on, ImU32 color) {
    const std::string label = std::string("[ ") + upper(word) + " ]";
    const ImVec2 size = ImGui::CalcTextSize(label.c_str());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(name.c_str());
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    const bool pressed = ImGui::InvisibleButton("##b", ImVec2(size.x, size.y));
    drive::hit("button:" + name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool held = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    if (!enabled) {
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    if (!theme().bracketedControls) {
        // The same box the bracketed word takes, so a theme never moves the layout.
        const std::string plain = upper(word);
        const ImVec2 inner = ImGui::CalcTextSize(plain.c_str());
        const ImVec2 at(pos.x + std::floor((size.x - inner.x) / 2.0f), pos.y);
        chrome(dl, ImVec2(pos.x, pos.y - 1.0f), ImVec2(max.x, max.y + 1.0f), (held || on) && enabled ? ChromeRole::PressedControl : ChromeRole::RaisedControl);
        dl->AddText(at, !enabled ? Ink::secondary : (held || on) ? Ink::onSelection : hovered ? Ink::nav : color, plain.c_str());
        return pressed;
    }
    if (!enabled) {
        dl->AddText(pos, Ink::secondary, label.c_str());
    } else if (held || on) {
        dl->AddRectFilled(pos, max, color);
        dl->AddText(pos, Ink::onSelection, label.c_str());
    } else if (hovered) {
        dl->AddRectFilled(pos, max, color == Ink::primary ? Surface::hover : color); // hover inverts too
        dl->AddText(pos, Ink::onSelection, label.c_str());
    } else {
        dl->AddText(pos, color, label.c_str());
    }
    return pressed;
}

bool disclosure(bool open, const std::string& name) {
    const float side = squareSide();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(name.c_str());
    const bool pressed = ImGui::InvisibleButton("##d", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const float r = std::floor(side * 0.28f);
    const ImVec2 c(std::floor(pos.x + side / 2.0f), std::floor(pos.y + side / 2.0f));
    const ImU32 ink = hovered ? Ink::strong : Ink::secondary;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (open) {
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r, c.y - r * 0.6f), ImVec2(c.x, c.y + r * 0.8f), ink);
    } else {
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x + r * 0.8f, c.y), ImVec2(c.x - r * 0.6f, c.y + r), ink);
    }
    return pressed;
}

float contentRight() { return ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x; }

float squareSide() { return std::floor(ImGui::CalcTextSize("[+]").x); }

bool squareButton(const char* glyph, const std::string& name, bool enabled, bool on) {
    const std::string label = std::string("[") + glyph + "]";
    const ImVec2 size = ImGui::CalcTextSize(label.c_str());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(name.c_str());
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    const bool pressed = ImGui::InvisibleButton("##s", size);
    drive::hit("button:" + name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool held = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    if (!enabled) {
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    if (!enabled) {
        dl->AddText(pos, Ink::secondary, label.c_str());
    } else if (held || on || hovered) {
        dl->AddRectFilled(pos, max, held || on ? Ink::primary : Surface::hover);
        dl->AddText(pos, Ink::onSelection, label.c_str());
    } else {
        dl->AddText(pos, Ink::primary, label.c_str());
    }
    return pressed;
}

bool plusAtRight(const std::string& name, const char* tooltip, bool enabled) {
    const float x = contentRight() - squareSide();
    if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + ImGui::GetStyle().ItemSpacing.x <= x + 1.0f) {
        ImGui::SameLine(x);
    }
    const bool pressed = squareButton("+", name, enabled);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

float bracketRowWidth(std::initializer_list<const char*> words) {
    float w = 0.0f;
    int n = 0;
    for (const char* word : words) {
        w += ImGui::CalcTextSize((std::string("[ ") + upper(word) + " ]").c_str()).x;
        ++n;
    }
    return w + static_cast<float>(std::max(0, n - 1)) * ImGui::GetStyle().ItemSpacing.x;
}

void rowAtRight(float width) {
    const float x = contentRight() - width;
    if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + ImGui::GetStyle().ItemSpacing.x < x) {
        ImGui::SameLine(x);
    } else {
        ImGui::SetCursorPosX(x);
    }
}

void dialogGap() { ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y + 2.0f)); }

bool textLink(const char* word, const std::string& name) {
    const std::string shown = upper(word);
    const ImVec2 size = ImGui::CalcTextSize(shown.c_str());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(name.c_str());
    const bool pressed = ImGui::InvisibleButton("##l", size);
    drive::hit("button:" + name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, hovered ? Ink::strong : Ink::secondary, shown.c_str());
    if (hovered) {
        dl->AddLine(ImVec2(pos.x, pos.y + size.y), ImVec2(pos.x + size.x, pos.y + size.y), Ink::strong);
    }
    return pressed;
}

void slantBar(float fraction, int cells, ImU32 on, ImU32 off) {
    if (fraction < 0.0f) {
        fraction = 0.0f;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    const int lit = static_cast<int>(std::lround(fraction * static_cast<float>(cells)));
    const std::string a(static_cast<std::size_t>(lit), '/');
    const std::string b(static_cast<std::size_t>(cells - lit), '.');
    text(on, a);
    ImGui::SameLine(0.0f, 0.0f);
    text(off, b);
}

// ---- angular --------------------------------------------------------------------------------

void outlineRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color, bool filled) {
    if (filled) {
        dl->AddRectFilled(min, max, color);
    } else {
        dl->AddRect(min, max, color);
    }
}

void paneTitle(const char* title) {
    // A tab that sits on its panel's rule, in the condensed face: a heading, not a control.
    const std::string label = upper(title);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = condensedFont() != nullptr ? condensedFont() : ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float pad = std::floor(size * 0.5f);
    const float h = std::floor(textSize.y + 3.0f);
    const float right = pos.x + ImGui::GetContentRegionAvail().x;
    const float y1 = std::floor(pos.y + h) + 0.5f;
    const float x0 = std::floor(pos.x) + 0.5f, x1 = std::floor(pos.x + textSize.x + 2.0f * pad) + 0.5f;
    dl->AddLine(ImVec2(pos.x, y1), ImVec2(x0, y1), Surface::codeBorder);
    dl->AddLine(ImVec2(x0, y1), ImVec2(x0, std::floor(pos.y) + 0.5f), Surface::codeBorder);
    dl->AddLine(ImVec2(x0, std::floor(pos.y) + 0.5f), ImVec2(x1, std::floor(pos.y) + 0.5f), Surface::codeBorder);
    dl->AddLine(ImVec2(x1, std::floor(pos.y) + 0.5f), ImVec2(x1, y1), Surface::codeBorder);
    dl->AddLine(ImVec2(x1, y1), ImVec2(right, y1), Surface::codeBorder);
    dl->AddText(font, size, ImVec2(pos.x + pad, pos.y + 2.0f), Ink::secondary, label.c_str());
    ImGui::Dummy(ImVec2(textSize.x + 2.0f * pad, h + 2.0f));
}

namespace {
std::map<std::string, PanePlace> g_places;
std::map<std::string, ImVec4> g_floatRects;
std::map<std::string, bool> g_floatClosed;
std::map<std::string, bool> g_floating;
std::vector<bool> g_paneBegun; // a view out of sight has no window to end
} // namespace

void placePane(const std::string& name, PanePlace place) { g_places[name] = place; }

void restoreCursor(ImVec2 at) {
    ImGui::SetCursorScreenPos(at);
    ImGui::GetCurrentWindow()->DC.IsSetPos = false;
}

ImVec4 paneFloatRect(const std::string& name) {
    const auto it = g_floatRects.find(name);
    return it == g_floatRects.end() ? ImVec4(0, 0, 0, 0) : it->second;
}

bool takePaneFloatClosed(const std::string& name) {
    const bool closed = g_floatClosed[name];
    g_floatClosed[name] = false;
    return closed;
}

bool beginPane(const char* name, ImGuiWindowFlags flags, bool title) {
    const auto place = g_places.find(name);
    bool hosted = false;
    bool keepOpen = true;
    bool open = false;
    if (place != g_places.end()) {
        const PanePlace& at = place->second;
        if (!at.shown) {
            g_paneBegun.push_back(false);
            return false;
        }
        flags |= ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
        const bool wasFloating = g_floating[name];
        g_floating[name] = at.floating;
        if (at.floating) {
            // leaving the tree: the window is the one that sat in its leaf, so it is placed and raised once
            const ImGuiCond when = wasFloating ? ImGuiCond_Appearing : ImGuiCond_Always;
            ImGui::SetNextWindowPos(at.pos, when);
            ImGui::SetNextWindowSize(at.size, when);
            if (!wasFloating) {
                ImGui::SetNextWindowFocus();
            }
            open = ImGui::Begin(name, &keepOpen, flags | ImGuiWindowFlags_NoCollapse);
            if (!keepOpen) {
                g_floatClosed[name] = true;
            }
            g_floatRects[name] = ImVec4(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowSize().x, ImGui::GetWindowSize().y);
        } else {
            ImGui::SetNextWindowPos(at.pos);
            ImGui::SetNextWindowSize(at.size);
            open = ImGui::Begin(name, nullptr,
                                flags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
            if (wasFloating || ImGui::IsWindowAppearing()) {
                ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow()); // floats stay over the tree
            }
        }
        hosted = true;
    } else {
        open = ImGui::Begin(name, nullptr, flags);
    }
    g_paneBegun.push_back(true);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    chrome(ImGui::GetWindowDrawList(), min, ImVec2(min.x + size.x, min.y + size.y), ChromeRole::RaisedPane);
    if (title && !hosted) { // a placed view's name is on its tab
        paneTitle(name);
    }
    return open;
}

void endPane() {
    const bool begun = g_paneBegun.empty() || g_paneBegun.back();
    if (!g_paneBegun.empty()) {
        g_paneBegun.pop_back();
    }
    if (begun) {
        ImGui::End();
    }
}

ImU32 rgbInk(unsigned char r, unsigned char g, unsigned char b) { return IM_COL32(r, g, b, 255); }

ImU32 ansiInk(int index) {
    if (index < 16) {
        const Rgba c = g_theme.ansi[std::clamp(index, 0, 15)];
        return c != 0 ? c : Ink::primary;
    }
    if (index < 232) {
        const int i = index - 16;
        const auto level = [](int v) { return static_cast<unsigned char>(v == 0 ? 0 : 55 + v * 40); };
        return rgbInk(level(i / 36), level((i / 6) % 6), level(i % 6));
    }
    const auto grey = static_cast<unsigned char>(8 + (index - 232) * 10);
    return rgbInk(grey, grey, grey);
}

void treeGuide(ImDrawList* dl, float x, float rowTop, float rowBottom, float toX, bool last) {
    const float px = std::floor(x) + 0.5f;
    const float mid = std::floor((rowTop + rowBottom) / 2) + 0.5f;
    dl->AddLine(ImVec2(px, std::floor(rowTop)), ImVec2(px, last ? mid : std::floor(rowBottom)), Ink::secondary);
    dl->AddLine(ImVec2(px, mid), ImVec2(std::floor(toX), mid), Ink::secondary);
}

float tabStripHeight() {
    return std::floor(ImGui::GetTextLineHeight() + 2.0f * kTabPadY * g_scale);
}

namespace {
float tabWidth(const char* name) {
    return std::floor(ImGui::CalcTextSize(upper(name).c_str()).x + 2.0f * kTabPadX * g_scale);
}
} // namespace

float tabStripWidth(const char* const* names, int count) {
    const float height = tabStripHeight();
    float w = 0.0f;
    for (int i = 0; i < count; ++i) {
        w += tabWidth(names[i]) + (i > 0 ? std::floor(kTabGap * g_scale) : 0.0f);
    }
    return w;
}

int tabStrip(const char* id, const char* const* names, int count, int active) {
    const float height = tabStripHeight();
    const float gap = std::floor(kTabGap * g_scale);
    const float pad = std::floor(kTabPadX * g_scale);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float y0 = std::floor(pos.y);
    float x = std::floor(pos.x);
    int result = active;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        const std::string label = upper(names[i]);
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        const float w = tabWidth(names[i]);
        ImGui::SetCursorScreenPos(ImVec2(x, y0));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tab", ImVec2(w, height))) {
            result = i;
        }
        drive::hit(std::string("tab:") + names[i], ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        drive::hit(std::string(id) + ":" + names[i], ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); // when a tab shares its name
        ImGui::PopID();
        const bool isActive = i == active;
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 at(x + pad, y0 + std::floor((height - textSize.y) / 2));
        chrome(dl, ImVec2(x, y0), ImVec2(x + w, y0 + height), isActive ? ChromeRole::PressedControl : ChromeRole::RaisedControl);
        dl->AddText(at, isActive ? Ink::onSelection : hovered ? Ink::nav : Ink::primary, label.c_str());
        x += w + gap;
    }
    ImGui::PopID();
    // The strip is one item: SameLine() continues the row, the next row starts kTabRowGap below
    // (the item's own vertical spacing, so no cursor move extends the window: ImGui warns on that).
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, std::floor(kTabRowGap * g_scale)));
    ImGui::Dummy(ImVec2(std::max(0.0f, x - gap - pos.x), height));
    ImGui::PopStyleVar();
    return result;
}

void hazardTape(ImDrawList* dl, ImVec2 min, ImVec2 max, double seconds) {
    const float height = max.y - min.y;
    dl->PushClipRect(min, max, true);
    dl->AddRectFilled(min, max, g_theme.surface.background);
    const float stripe = height * 0.9f;
    const float shift = static_cast<float>(std::fmod(seconds * 24.0, stripe * 2.0));
    for (float x = min.x - height - stripe * 2.0f + shift; x < max.x; x += stripe * 2.0f) {
        const ImVec2 quad[4] = {ImVec2(x + height, min.y), ImVec2(x + height + stripe, min.y),
                                ImVec2(x + stripe, max.y), ImVec2(x, max.y)};
        dl->AddConvexPolyFilled(quad, 4, Ink::inspected);
    }
    // Classic Mac OS: a rounded tube set into the surface. Light falls from above across the stripes,
    // and the edge is inset, dark above and left, light below and right.
    const float mid = min.y + height * 0.42f;
    dl->AddRectFilledMultiColor(min, ImVec2(max.x, mid), IM_COL32(255, 255, 255, 120), IM_COL32(255, 255, 255, 120),
                                IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    dl->AddRectFilledMultiColor(ImVec2(min.x, mid), max, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150),
                                IM_COL32(0, 0, 0, 150));
    dl->PopClipRect();
    const ImU32 dark = IM_COL32(0, 0, 0, 200);
    const ImU32 light = IM_COL32(255, 255, 255, 110);
    const ImVec2 end(max.x - 1.0f, max.y - 1.0f);
    dl->AddLine(min, ImVec2(end.x, min.y), dark);
    dl->AddLine(min, ImVec2(min.x, end.y), dark);
    dl->AddLine(ImVec2(min.x, end.y), end, light);
    dl->AddLine(ImVec2(end.x, min.y), ImVec2(end.x, max.y), light);
}

void hazardSpinner(float width, double seconds) {
    const float height = ImGui::GetTextLineHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    ImGui::Dummy(ImVec2(width, height));
    hazardTape(ImGui::GetWindowDrawList(), min, max, seconds);
}

void outlineFloatingWindows() {
    // a screenshot renders the frame first: a second pass adds to lists already counted, and overruns the upload
    if (!ImGui::GetCurrentContext()->WithinFrameScope) {
        return;
    }
    const float thick = std::max(1.0f, std::floor(g_scale));
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Border);
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
        // a modal takes WindowBorderSize, not PopupBorderSize, so it needs this too; a plain popup draws its own
        const bool modal = (w->Flags & ImGuiWindowFlags_Modal) != 0;
        const ImGuiWindowFlags skip = ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip |
                                      (modal ? 0 : ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoTitleBar);
        if (!w->WasActive || w->Hidden || w->DockIsActive || (w->Flags & skip) != 0) {
            continue;
        }
        const ImVec2 max(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        w->DrawList->PushClipRect(w->Pos, max, false);
        w->DrawList->AddRect(w->Pos, max, ink, 0.0f, thick);
        w->DrawList->PopClipRect();
    }
}

void boldText(ImU32 color, std::string_view s) {
    text(color, s);
    // where the line landed, which a menu bar sets lower than the cursor was
    const ImVec2 at = ImGui::GetItemRectMin();
    const float strike = std::max(1.0f, std::floor(g_scale));
    ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + strike, at.y), color, s.data(), s.data() + s.size());
}

void wrappedTooltip(const std::string& text) {
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::CalcTextSize("0").x * 64.0f);
        ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void blockCursor(ImDrawList* dl, ImVec2 pos, ImVec2 cell, bool on, bool active, const char* ch) {
    const ImVec2 max(pos.x + cell.x, pos.y + cell.y);
    if (!active) {
        dl->AddRect(pos, max, Ink::secondary);
        return;
    }
    if (on) {
        dl->AddRectFilled(pos, max, Ink::primary);
        if (ch && *ch) {
            dl->AddText(pos, Surface::background, ch, ch + 1);
        }
    } else {
        dl->AddRectFilled(pos, max, ImGui::GetColorU32(ImGuiCol_FrameBg)); // also hides ImGui's own 1 px caret
        if (ch && *ch) {
            dl->AddText(pos, Ink::primary, ch, ch + 1);
        }
    }
}


// ---- misc -----------------------------------------------------------------------------------

ImU32 stateColor(bool attached, bool connecting) {
    if (attached) {
        return Ink::connected;
    }
    return connecting ? Ink::inspected : Ink::fatal;
}

bool touchDragScroll() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseSource != ImGuiMouseSource_TouchScreen ||
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ||
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        return false;
    }
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = g.CurrentWindow;
    // A finger on a scrollbar grab already scrolls; do not fight it.
    if (g.ActiveId != 0 && (g.ActiveId == ImGui::GetWindowScrollbarID(window, ImGuiAxis_X) ||
                            g.ActiveId == ImGui::GetWindowScrollbarID(window, ImGuiAxis_Y))) {
        return false;
    }
    if (io.MouseDelta.x == 0.0f && io.MouseDelta.y == 0.0f) {
        return false;
    }
    // A widget pressed at the start of the drag must not fire on release.
    if (g.ActiveId != 0 && g.ActiveIdWindow == window) {
        ImGui::ClearActiveID();
    }
    if (window->ScrollMax.y > 0.0f) {
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    }
    if (window->ScrollMax.x > 0.0f) {
        ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
    }
    return true;
}

} // namespace ke::ui
