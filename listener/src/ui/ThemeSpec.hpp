// What a theme is, as data. Colour meanings are the rule (docs/listener-style.md); a theme only
// says what those colours look like, what shape the chrome has, and what type it uses. Themes
// live in assets/themes/*.json and in <data dir>/themes/*.json, so a new one needs no code. Pure.
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ke::ui {

using Rgba = std::uint32_t; // ImU32's layout: 0xAABBGGRR

// The seven roles every surface of the client is drawn with.
enum class ChromeRole { RaisedPane, InsetWell, RaisedControl, PressedControl, TitleWell, ScrollTrack, ScrollControl };
inline constexpr int kChromeRoles = 7;

struct ChromeSpec {
    Rgba face = 0;
    Rgba high = 0;
    Rgba low = 0;
    float bevel = 0.0f;
    bool inset = false;
};

struct ThemeSpec {
    std::string name;
    std::string title;

    struct Ink {
        Rgba onPaper = 0, primary = 0, secondary = 0, strong = 0, nav = 0, inspected = 0, unresolved = 0, connected = 0, fatal = 0, onSelection = 0, unsafe = 0;
    } ink;
    struct Surface {
        Rgba paper = 0, twilightOrange = 0, twilightBlue = 0, twilightBlack = 0, twilightStar = 0, background = 0, pane = 0, well = 0, control = 0, selection = 0, hover = 0, border = 0, rule = 0, code = 0, codeBorder = 0;
        Rgba scrollTrack = 0, scrollThumb = 0, scrollThumbHover = 0;
    } surface;
    ChromeSpec chrome[kChromeRoles];
    bool sentenceLabels = false; // type.label-case: labels in caps or in sentence case
    Rgba brand[3] = {}; // the mark's three bars, red, green, blue; a theme may override them
    // The terminal's sixteen ANSI colours, so a program's colours belong to the theme. The rest of
    // the 256 are the standard cube and greys.
    Rgba ansi[16] = {};

    bool bracketedControls = false;

    std::vector<std::string> fonts; // absolute, or a name under assets/fonts; the first that exists

    const ChromeSpec& role(ChromeRole r) const { return chrome[static_cast<int>(r)]; }
};

// `#RRGGBB` or `#RRGGBBAA`.
std::optional<Rgba> parseColor(const std::string& text);
// A theme file over `base`: whatever it leaves out comes from there. nullopt, with `error` set,
// when it is not a theme at all or a colour does not read.
std::optional<ThemeSpec> parseTheme(const nlohmann::json& j, const ThemeSpec& base, std::string* error = nullptr);

} // namespace ke::ui
