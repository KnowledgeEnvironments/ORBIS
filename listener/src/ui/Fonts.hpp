// The interface face and the monospace face, chosen per client (docs/listener-style.md). JetBrains
// Mono ships with the app; the rest come from the platform.
#pragma once

#include "ui/ThemeSpec.hpp"

#include <imgui.h>
#include <string>
#include <vector>

namespace ke::ui {

struct Fonts {
    ImFont* mono = nullptr; // transcripts, Field values, terminals, editors
    ImFont* ui = nullptr;   // everything else
    std::string monoSource; // path actually loaded, for the log
};

Fonts loadFonts(const std::string& assetsDir);
// The theme's face: the first of its fonts that exists. Fonts are kept once loaded, so moving
// between themes is free; call it between frames.
void useThemeFont(Fonts& fonts, const ThemeSpec& theme, const std::string& assetsDir);

// SETTINGS FONT. Bundled faces, then what the platform offers; a name no longer present falls back
// to the theme's face. ImGui bakes glyphs at each size on demand, so a change never rebuilds an
// atlas: it costs the first glyphs drawn, not a frame.
struct FontOption {
    std::string name;
    std::string path;
    bool mono = false;
};
std::vector<FontOption> fontOptions(const std::string& assetsDir);
// Interface and monospace faces and sizes, live. `monoScale`: monospace size over interface size.
void useFonts(Fonts& fonts, const std::string& uiName, const std::string& monoName, float uiSize, float monoSize,
              const ThemeSpec& theme, const std::string& assetsDir);
// The monospace face at its size, for text that lines up in columns.
void pushMono();
void popMono();
ImFont* monoFont();
ImFont* condensedFont(); // for headings; null when the bundled face is missing
float monoPixels(); // what pushMono makes ImGui::GetFontSize() say

} // namespace ke::ui
