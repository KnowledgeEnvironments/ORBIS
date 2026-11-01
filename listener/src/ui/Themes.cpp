#include "ui/Themes.hpp"

#include "util/Log.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ke::ui {

namespace {

// assets/themes/*.json, embedded so a phone needs no bundle resource: prometheus first, the default.
const char* const kBuiltin[] = {
#include "builtin_themes.inc"
};

} // namespace

ThemeLibrary::ThemeLibrary() {
    ThemeSpec base;
    for (const char* text : kBuiltin) {
        std::string error;
        if (auto t = parseTheme(nlohmann::json::parse(text, nullptr, false), base, &error)) {
            if (themes_.empty()) {
                base = *t;
            }
            themes_.push_back(std::move(*t));
        } else {
            log::error("themes: a shipped theme does not read: {}", error);
        }
    }
}

void ThemeLibrary::loadDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        std::ifstream in(file);
        std::stringstream text;
        text << in.rdbuf();
        std::string error;
        auto t = parseTheme(nlohmann::json::parse(text.str(), nullptr, false), themes_.front(), &error);
        if (!t) {
            log::warn("themes: skipping {}: {}", file.string(), error);
            continue;
        }
        auto same = std::find_if(themes_.begin(), themes_.end(), [&t](const ThemeSpec& o) { return o.name == t->name; });
        if (same != themes_.end()) {
            *same = std::move(*t);
        } else {
            themes_.push_back(std::move(*t));
        }
    }
}

ThemeLibrary& themeLibrary() {
    static ThemeLibrary library;
    return library;
}

const ThemeSpec& ThemeLibrary::named(const std::string& name) const {
    auto it = std::find_if(themes_.begin(), themes_.end(), [&name](const ThemeSpec& t) { return t.name == name; });
    return it != themes_.end() ? *it : themes_.front();
}

} // namespace ke::ui
