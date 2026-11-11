#include "chat/RichText.hpp"

#include <algorithm>
#include <cctype>

namespace ke::chat {

namespace {

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

std::size_t byteAt(std::string_view s, std::size_t codePoint) {
    std::size_t b = 0;
    while (codePoint > 0 && b < s.size()) {
        ++b;
        while (b < s.size() && continuation(s[b])) {
            ++b;
        }
        --codePoint;
    }
    return codePoint == 0 ? b : std::string_view::npos;
}

std::size_t codePointsOf(std::string_view s) {
    return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char c) { return !continuation(c); }));
}

bool wordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || (c & 0x80) != 0; }

} // namespace

std::vector<RefSpan> parseRefs(const nlohmann::json& refs, std::string_view text) {
    std::vector<RefSpan> out;
    if (!refs.is_array()) {
        return out;
    }
    for (const nlohmann::json& r : refs) {
        if (!r.is_object() || !r.contains("start") || !r.contains("end") || !r["start"].is_number_integer() || !r["end"].is_number_integer() || r["start"].get<long long>() < 0 || r["end"].get<long long>() < 0) {
            continue;
        }
        RefSpan s;
        s.start = byteAt(text, r["start"].get<std::size_t>());
        s.end = byteAt(text, r["end"].get<std::size_t>());
        if (s.start == std::string_view::npos || s.end == std::string_view::npos || s.start >= s.end) {
            continue;
        }
        const std::string kind = r.value("kind", std::string("object"));
        s.kind = kind == "field" ? SymbolKind::Field : kind == "seq" ? SymbolKind::Seq : kind == "path" ? SymbolKind::Path : SymbolKind::Object;
        s.path = r.value("path", std::string());
        s.line = r.value("line", 0);
        s.object = r.value("object", std::string());
        s.field = r.value("field", std::string());
        s.seq = r.value("seq", Seq{0});
        s.name = r.value("name", std::string());
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const RefSpan& a, const RefSpan& b) { return a.start < b.start; });
    std::vector<RefSpan> clean;
    for (RefSpan& s : out) {
        if (clean.empty() || s.start >= clean.back().end) {
            clean.push_back(std::move(s));
        }
    }
    return clean;
}

std::vector<RefSpan> spansIn(const std::vector<RefSpan>& spans, std::string_view text, std::string_view part) {
    std::vector<RefSpan> out;
    const std::size_t base = text.find(part);
    if (part.empty() || base == std::string_view::npos) {
        return out;
    }
    for (RefSpan s : spans) {
        if (s.start >= base && s.end <= base + part.size()) {
            s.start -= base;
            s.end -= base;
            out.push_back(std::move(s));
        }
    }
    return out;
}

Symbol symbolOf(const RefSpan& span, std::string_view text, const std::string& current) {
    Symbol s;
    s.start = span.start;
    s.end = span.end;
    s.kind = span.kind;
    s.object = span.object;
    s.field = span.field;
    s.seq = span.seq;
    s.path = span.path;
    s.line = span.line;
    if (span.kind == SymbolKind::Object && !current.empty()) {
        s.display = current;
    } else if (span.kind == SymbolKind::Object && !span.name.empty()) {
        s.display = span.name;
    } else {
        s.display = std::string(text.substr(span.start, span.end - span.start));
    }
    return s;
}

nlohmann::json mentionRefs(std::string_view text, const std::vector<std::pair<std::string, std::string>>& picked) {
    nlohmann::json refs = nlohmann::json::array();
    for (const auto& [id, name] : picked) {
        if (name.empty()) {
            continue;
        }
        for (std::size_t at = text.find(name); at != std::string_view::npos; at = text.find(name, at + 1)) {
            const bool before = at == 0 || !wordChar(text[at - 1]);
            const bool after = at + name.size() >= text.size() || !wordChar(text[at + name.size()]);
            if (before && after) {
                const std::size_t start = codePointsOf(text.substr(0, at));
                refs.push_back({{"start", start}, {"end", start + codePointsOf(name)}, {"kind", "object"}, {"object", id}, {"name", name}});
                break;
            }
        }
    }
    return refs;
}

} // namespace ke::chat
