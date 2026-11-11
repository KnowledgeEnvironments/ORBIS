#include "chat/FieldValue.hpp"

#include <cstring>

#include <cctype>
#include <cstdlib>

namespace ke::chat {

namespace {

bool isInstant(const std::string& t) {
    // 2026-10-30T20:00:00Z, optional fraction: what the world's JSON form writes
    if (t.size() < 20 || t.back() != 'Z') {
        return false;
    }
    static constexpr const char* kShape = "dddd-dd-ddTdd:dd:dd";
    for (std::size_t i = 0; i < 19; ++i) {
        const char want = kShape[i];
        const bool ok = want == 'd' ? std::isdigit(static_cast<unsigned char>(t[i])) != 0 : t[i] == want;
        if (!ok) {
            return false;
        }
    }
    for (std::size_t i = 19; i + 1 < t.size(); ++i) {
        const bool ok = (i == 19 && t[i] == '.') || (i > 19 && std::isdigit(static_cast<unsigned char>(t[i])) != 0);
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

ParsedField parseFieldValue(std::string_view kind, const std::string& text, const SymbolTable* table) {
    ParsedField p;
    if (kind == "string") {
        p.ok = true;
        p.value = text;
        p.type = "string";
    } else if (kind == "number") {
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (text.empty() || end == text.c_str() || *end != '\0') {
            p.error = "a number";
        } else {
            p.ok = true;
            p.value = v;
            p.type = "number";
        }
    } else if (kind == "boolean") {
        if (text == "true" || text == "false") {
            p.ok = true;
            p.value = text == "true";
            p.type = "boolean";
        } else {
            p.error = "true or false";
        }
    } else if (kind == "keyword") {
        std::string k = text;
        if (!k.empty() && k.front() == ':') {
            k.erase(k.begin());
        }
        if (k.empty() || k.find_first_of(" \t\n") != std::string::npos) {
            p.error = "one word, no spaces";
        } else {
            p.ok = true;
            p.value = k;
            p.type = "keyword";
        }
    } else if (kind == "instant") {
        if (isInstant(text)) {
            p.ok = true;
            p.value = text;
            p.type = "instant";
        } else {
            p.error = "ISO-8601 UTC, like 2026-10-30T20:00:00Z";
        }
    } else if (kind == "ref") {
        std::string r = text;
        if (!r.empty() && r.front() == '@') {
            r.erase(r.begin());
        }
        const SymbolTable::Entry* e = nullptr;
        if (table) {
            e = table->byId(r);
            e = e ? e : table->byName(r);
            e = e || r.size() < 8 ? e : table->byPrefix(r);
        }
        if (!e) {
            p.error = "an object you can read: type a name, or @ to look one up";
        } else {
            p.ok = true;
            p.value = e->id;
            p.type = json::array({"ref", e->cls.empty() ? "object" : e->cls});
        }
    } else if (kind == "list" || kind == "map") {
        const json parsed = json::parse(text, nullptr, false);
        if (parsed.is_discarded()) {
            p.error = "JSON does not parse";
        } else if (kind == "list" && !parsed.is_array()) {
            p.error = "a JSON array, like [\"a\", \"b\"]";
        } else if (kind == "map" && !parsed.is_object()) {
            p.error = "a JSON object, like {\"a\": 1}";
        } else {
            p.ok = true;
            p.value = parsed;
            p.type = kind == "map" ? json("map") : json("list");
            if (parsed.is_array() && !parsed.empty()) {
                bool strings = true, numbers = true;
                for (const json& v : parsed) {
                    strings = strings && v.is_string();
                    numbers = numbers && v.is_number();
                }
                if (strings || numbers) {
                    p.type = json::array({"list", strings ? "string" : "number"});
                }
            }
        }
    } else {
        p.error = "unknown type";
    }
    return p;
}

std::string fieldNameHint(std::string_view name) {
    auto starts = [&](std::string_view p) { return name.substr(0, p.size()) == p; };
    auto ends = [&](std::string_view p) { return name.size() > p.size() && name.substr(name.size() - p.size()) == p; };
    if (ends("/hint")) {
        return "a one-line comment shown beside that Field, to whoever reads it";
    }
    if (ends("/comment")) {
        return "a comment thread about that Field";
    }
    if (starts("instructions/")) {
        return "joins this object's system prompt, every cycle";
    }
    if (starts("memory/")) {
        return "always in the prompt, and counts against memory/budget";
    }
    if (starts("policy/")) {
        return "world policy: read by the kernel, not only by the model";
    }
    if (starts("serialize/")) {
        return "tunes what this Principal's prompt keeps and drops";
    }
    if (starts("loop/")) {
        return "the resident loop reads these; some are written by the loop itself";
    }
    if (starts("view/")) {
        return "read by clients, for how this object is shown";
    }
    return "";
}

std::string typeName(const json& type) {
    if (type.is_string()) {
        return type.get<std::string>();
    }
    if (type.is_array() && !type.empty()) {
        std::string s;
        for (const auto& t : type) {
            if (!s.empty()) {
                s += " ";
            }
            s += typeName(t);
        }
        return s;
    }
    return type.is_null() ? "?" : type.dump();
}

std::string scalarName(const json& type) {
    if (type.is_string()) {
        return type.get<std::string>();
    }
    if (type.is_array() && !type.empty() && type.front().is_string()) {
        return type.front().get<std::string>();
    }
    return "";
}

// Text typed into a value box, as the Field's type wants it.
json valueFor(const json& type, const std::string& text) {
    const std::string t = scalarName(type);
    if (t == "string" || t == "keyword" || t == "instant" || t == "bytes" || t == "ref" || t.empty()) {
        return text;
    }
    if (t == "number") {
        try {
            return std::stod(text);
        } catch (...) {
            return text;
        }
    }
    if (t == "boolean") {
        return text == "true" || text == "1" || text == "yes";
    }
    const json parsed = json::parse(text, nullptr, false);
    return parsed.is_discarded() ? json(text) : parsed;
}

std::string editText(const json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

// `<field>/hint` and `<field>/comment` are facets of `<field>`; `object/hint` and `object/comment`
// are the object's own. Returns the base name and the facet ("hint" | "comment"), or "" for a
// plain Field.
std::pair<std::string, std::string> facetOf(const std::string& name) {
    for (const char* suffix : {"/hint", "/comment"}) {
        const std::size_t n = std::strlen(suffix);
        if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0) {
            return {name.substr(0, name.size() - n), std::string(suffix + 1)};
        }
    }
    return {name, ""};
}

std::string reasoningShown(const std::string& loopMode, const std::string& modelMode) {
    if (loopMode == "on" || loopMode == "off") {
        return loopMode;
    }
    return modelMode == "on" ? "on" : "off";
}

} // namespace ke::chat
