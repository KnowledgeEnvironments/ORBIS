#include "ui/Fonts.hpp"

#include "platform/Platform.hpp"
#include "util/Log.hpp"

#include <SDL3/SDL_iostream.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <imgui_freetype.h>

namespace ke::ui {

namespace {

// Through SDL's IO, not ImGui's fopen: on Android a relative path is an asset inside the APK. The
// atlas owns the bytes for the life of the font (it rasterizes on demand), so they are ImGui's
// allocation. Null when the file is not there.
ImFont* addFontFile(const std::string& path, float sizePx, ImFontConfig& cfg) {
    size_t size = 0;
    void* bytes = SDL_LoadFile(path.c_str(), &size);
    if (bytes == nullptr) {
        return nullptr;
    }
    void* owned = IM_ALLOC(size);
    std::memcpy(owned, bytes, size);
    SDL_free(bytes);
    cfg.FontDataOwnedByAtlas = true;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(size), sizePx, &cfg);
}

ImFont* load(const std::string& preferred, const std::string& fallback, float sizePx, std::string& used) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
    for (const std::string& path : {preferred, fallback}) {
        if (path.empty()) {
            continue;
        }
        cfg.FontNo = 0; // first face of a .ttc (Menlo-Regular)
        if (ImFont* f = addFontFile(path, sizePx, cfg)) {
            used = path;
            return f;
        }
    }
    used = "imgui default";
    return io.Fonts->AddFontDefault();
}

std::map<std::string, ImFont*>& loaded() {
    static std::map<std::string, ImFont*> fonts;
    return fonts;
}

} // namespace

void useThemeFont(Fonts& fonts, const ThemeSpec& theme, const std::string& assetsDir) {
    ImGuiIO& io = ImGui::GetIO();
    for (const std::string& name : theme.fonts) {
        const std::string path = !name.empty() && name.front() == '/' ? name : assetsDir + "/fonts/" + name;
        const std::string key = path;
        ImFont*& font = loaded()[key];
        if (!font) {
            ImFontConfig cfg;
            cfg.FontNo = 0;
            cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
            font = addFontFile(path, 14.0f, cfg);
            // A theme's face rarely has the math: typeset symbols fall through to the bundled fonts.
            for (const char* more : {"JetBrainsMono.ttf", "Inter.ttf"}) {
                if (font != nullptr && path.find(more) == std::string::npos) {
                    ImFontConfig merge;
                    merge.MergeMode = true;
                    merge.DstFont = font;
                    merge.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
                    addFontFile(assetsDir + "/fonts/" + more, 14.0f, merge);
                }
            }
        }
        if (font) {
            fonts.mono = font;
            fonts.ui = font;
            fonts.monoSource = path;
            io.FontDefault = font;
            log::info("fonts: {}", path);
            return;
        }
    }
}

namespace {

ImFont* g_mono = nullptr;
ImFont* g_condensed = nullptr;
float g_monoSize = 14.0f; // in ImGui's base units, so the DPI scale still applies

ImFont* faceAt(const std::string& path) {
    ImFont*& font = loaded()[path];
    if (font == nullptr) {
        ImFontConfig cfg;
        cfg.FontNo = 0;
        cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
        font = addFontFile(path, 14.0f, cfg); // null when the platform has no such file
    }
    return font;
}

} // namespace

std::vector<FontOption> fontOptions(const std::string& assetsDir) {
    std::vector<FontOption> out;
    out.push_back(FontOption{"JetBrains Mono", assetsDir + "/fonts/JetBrainsMono.ttf", true});
    out.push_back(FontOption{"Monaspace Xenon", assetsDir + "/fonts/MonaspaceXenon.ttf", true});
    out.push_back(FontOption{"Inter", assetsDir + "/fonts/Inter.ttf", false});
    for (const platform::SystemFont& f : platform::systemFonts()) {
        out.push_back(FontOption{f.name, f.path, f.mono});
    }
    return out;
}

void useFonts(Fonts& fonts, const std::string& uiName, const std::string& monoName, float uiSize, float monoSize,
              const ThemeSpec& theme, const std::string& assetsDir) {
    useThemeFont(fonts, theme, assetsDir);
    const auto named = [&](const std::string& name) -> ImFont* {
        for (const FontOption& o : fontOptions(assetsDir)) {
            if (o.name == name) {
                return faceAt(o.path);
            }
        }
        return nullptr;
    };
    if (ImFont* f = uiName.empty() ? nullptr : named(uiName)) {
        fonts.ui = f;
        ImGui::GetIO().FontDefault = f;
    }
    if (ImFont* f = monoName.empty() ? nullptr : named(monoName)) {
        fonts.mono = f;
    }
    g_mono = fonts.mono;
    g_condensed = faceAt(assetsDir + "/fonts/RobotoCondensed.ttf");
    g_monoSize = 14.0f * monoSize / std::max(1.0f, uiSize);
}

ImFont* condensedFont() { return g_condensed; }

void pushMono() { ImGui::PushFont(g_mono, g_monoSize); }
void popMono() { ImGui::PopFont(); }
ImFont* monoFont() { return g_mono; }
float monoPixels() {
    const ImGuiStyle& style = ImGui::GetStyle();
    return g_monoSize * style.FontScaleMain * style.FontScaleDpi;
}

Fonts loadFonts(const std::string& assetsDir) {
    ImGuiIO& io = ImGui::GetIO();
    Fonts fonts;
    fonts.mono = load(assetsDir + "/fonts/JetBrainsMono.ttf", "/System/Library/Fonts/Menlo.ttc", 14.0f,
                      fonts.monoSource);
    fonts.ui = fonts.mono;
    io.FontDefault = fonts.mono;
    g_mono = fonts.mono;
    log::info("fonts: mono={}", fonts.monoSource);
    return fonts;
}

} // namespace ke::ui
