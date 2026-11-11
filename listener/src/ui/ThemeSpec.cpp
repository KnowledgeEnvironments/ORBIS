#include "ui/ThemeSpec.hpp"

#include <cstdio>

namespace ke::ui {

using nlohmann::json;

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

class Reader {
  public:
    explicit Reader(std::string* error) : error_(error) {}

    void color(const json& o, const char* key, Rgba& out) {
        if (!o.contains(key)) {
            return;
        }
        const std::optional<Rgba> c = o[key].is_string() ? parseColor(o[key].get<std::string>()) : std::nullopt;
        if (!c) {
            fail(std::string(key) + " is not a colour");
            return;
        }
        out = *c;
    }

    void fail(std::string why) {
        ok_ = false;
        if (error_ && error_->empty()) {
            *error_ = std::move(why);
        }
    }

    bool ok() const { return ok_; }

  private:
    std::string* error_;
    bool ok_ = true;
};

const json& section(const json& j, const char* key) {
    static const json empty = json::object();
    return j.contains(key) && j[key].is_object() ? j[key] : empty;
}

} // namespace

std::optional<Rgba> parseColor(const std::string& text) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') {
        return std::nullopt;
    }
    std::uint32_t parts[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i < (text.size() - 1) / 2; ++i) {
        const int hi = hexDigit(text[1 + i * 2]);
        const int lo = hexDigit(text[2 + i * 2]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        parts[i] = static_cast<std::uint32_t>(hi * 16 + lo);
    }
    return parts[0] | parts[1] << 8 | parts[2] << 16 | parts[3] << 24;
}

std::optional<ThemeSpec> parseTheme(const json& j, const ThemeSpec& base, std::string* error) {
    Reader r(error);
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()) {
        r.fail("a theme needs a name");
        return std::nullopt;
    }
    ThemeSpec t = base;
    t.name = j["name"].get<std::string>();
    t.title = j.value("title", t.name);

    const json& ink = section(j, "ink");
    r.color(ink, "primary", t.ink.primary);
    r.color(ink, "secondary", t.ink.secondary);
    r.color(ink, "strong", t.ink.strong);
    r.color(ink, "nav", t.ink.nav);
    r.color(ink, "inspected", t.ink.inspected);
    r.color(ink, "unresolved", t.ink.unresolved);
    r.color(ink, "connected", t.ink.connected);
    r.color(ink, "fatal", t.ink.fatal);
    r.color(ink, "on-selection", t.ink.onSelection);
    r.color(ink, "on-paper", t.ink.onPaper);
    r.color(ink, "unsafe", t.ink.unsafe);

    const json& surface = section(j, "surface");
    r.color(surface, "background", t.surface.background);
    r.color(surface, "pane", t.surface.pane);
    r.color(surface, "well", t.surface.well);
    r.color(surface, "control", t.surface.control);
    r.color(surface, "selection", t.surface.selection);
    r.color(surface, "hover", t.surface.hover);
    r.color(surface, "border", t.surface.border);
    r.color(surface, "rule", t.surface.rule);
    r.color(surface, "code", t.surface.code);
    r.color(surface, "code-border", t.surface.codeBorder);
    r.color(surface, "paper", t.surface.paper);
    r.color(surface, "twilight-orange", t.surface.twilightOrange);
    r.color(surface, "twilight-blue", t.surface.twilightBlue);
    r.color(surface, "twilight-black", t.surface.twilightBlack);
    r.color(surface, "twilight-star", t.surface.twilightStar);
    r.color(surface, "scroll-track", t.surface.scrollTrack);
    r.color(surface, "scroll-thumb", t.surface.scrollThumb);
    r.color(surface, "scroll-thumb-hover", t.surface.scrollThumbHover);

    const json& brand = section(j, "brand");
    r.color(brand, "red", t.brand[0]);
    r.color(brand, "green", t.brand[1]);
    r.color(brand, "blue", t.brand[2]);

    if (j.contains("ansi") && j["ansi"].is_object()) {
        static constexpr const char* kAnsi[16] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
                                                  "bright-black", "bright-red", "bright-green", "bright-yellow",
                                                  "bright-blue", "bright-magenta", "bright-cyan", "bright-white"};
        for (int i = 0; i < 16; ++i) {
            r.color(j["ansi"], kAnsi[i], t.ansi[i]);
        }
    }

    static constexpr const char* kRoles[kChromeRoles] = {"raised-pane", "inset-well", "raised-control", "pressed-control",
                                                         "title-well", "scroll-track", "scroll-control"};
    const json& chrome = section(j, "chrome");
    for (int i = 0; i < kChromeRoles; ++i) {
        const json& c = section(chrome, kRoles[i]);
        ChromeSpec& out = t.chrome[i];
        r.color(c, "face", out.face);
        r.color(c, "high", out.high);
        r.color(c, "low", out.low);
        if (c.contains("bevel") && c["bevel"].is_number()) {
            out.bevel = c["bevel"].get<float>();
        }
        if (c.contains("inset") && c["inset"].is_boolean()) {
            out.inset = c["inset"].get<bool>();
        }
    }

    const json& geometry = section(j, "geometry");
    if (geometry.contains("controls")) {
        t.bracketedControls = geometry.value("controls", "") == "bracketed";
    }

    const json& type = section(j, "type");
    if (type.contains("label-case")) {
        t.sentenceLabels = type.value("label-case", "") == "sentence";
    }
    if (type.contains("fonts") && type["fonts"].is_array()) {
        t.fonts.clear();
        for (const json& f : type["fonts"]) {
            if (f.is_string()) {
                t.fonts.push_back(f.get<std::string>());
            }
        }
    }

    if (!r.ok()) {
        return std::nullopt;
    }
    return t;
}

} // namespace ke::ui
