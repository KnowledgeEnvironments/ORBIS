#include "chat/Search.hpp"

#include <algorithm>
#include <cctype>

namespace ke::chat {

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool wordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '/';
}

// Whole word: the match is not continued by word characters on either side.
bool wholeWord(const std::string& hay, std::size_t at, std::size_t len) {
    const bool before = at == 0 || !wordChar(hay[at - 1]);
    const bool after = at + len >= hay.size() || !wordChar(hay[at + len]);
    return before && after;
}

std::string valueText(const json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_null()) {
        return "";
    }
    return v.dump();
}

} // namespace

SearchHit snippetOf(const std::string& value, const std::string& text, std::size_t context) {
    SearchHit h;
    if (text.empty()) {
        h.snippet = value.substr(0, std::min(value.size(), 2 * context));
        return h;
    }
    const std::string hay = lower(value);
    const std::string needle = lower(text);
    const std::size_t at = hay.find(needle);
    if (at == std::string::npos) {
        h.snippet = value.substr(0, std::min(value.size(), 2 * context));
        return h;
    }
    const std::size_t from = at > context ? at - context : 0;
    const std::size_t to = std::min(value.size(), at + needle.size() + context);
    std::string s;
    if (from > 0) {
        s += "\xe2\x80\xa6"; // …
    }
    s += value.substr(from, to - from);
    if (to < value.size()) {
        s += "\xe2\x80\xa6";
    }
    // Newlines flatten to spaces so a hit is one line.
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    h.snippet = s;
    h.matchStart = (from > 0 ? 3 : 0) + (at - from);
    h.matchLen = needle.size();
    h.score = wholeWord(hay, at, needle.size()) ? 2.0 : 1.0;
    return h;
}

std::vector<SearchHit> localSearch(const model::Principals& fold, const MessageCache& messages, const std::string& text,
                                   std::size_t limit) {
    std::vector<SearchHit> hits;
    if (text.empty()) {
        return hits;
    }
    const std::string needle = lower(text);
    auto consider = [&](const std::string& object, const std::string& field, const std::string& kind, const std::string& value,
                        Seq seq, const std::string& name) {
        if (lower(value).find(needle) == std::string::npos) {
            return;
        }
        SearchHit h = snippetOf(value, text);
        h.object = object;
        h.field = field;
        h.kind = kind;
        h.seq = seq;
        h.name = name;
        hits.push_back(std::move(h));
    };
    for (const model::PrincipalInfo* p : fold.ordered()) {
        const std::string name = fold.nameOf(p->id);
        consider(p->id, "", "object", name, p->created, name);
        for (const auto& [fname, f] : p->fields) {
            consider(p->id, fname, "field", valueText(f.value), f.seq, name);
        }
    }
    for (const auto& [id, r] : fold.routes()) {
        const std::string name = fold.nameOf(id);
        consider(id, "", "object", name, r.created, name);
        for (const auto& [fname, f] : r.fields) {
            consider(id, fname, "field", valueText(f.value), f.seq, name);
        }
    }
    messages.forEachReady([&](const MessageRow& m) {
        consider(m.id, "message/text", "message", m.text, m.seq, fold.nameOf(m.from));
    });
    std::stable_sort(hits.begin(), hits.end(), [](const SearchHit& a, const SearchHit& b) {
        if (a.seq != b.seq) {
            return a.seq > b.seq;
        }
        return a.score > b.score;
    });
    if (hits.size() > limit) {
        hits.resize(limit);
    }
    return hits;
}

std::vector<SearchHit> parseWorldHits(const json& result, const std::string& text) {
    std::vector<SearchHit> hits;
    if (!result.is_object() || !result.contains("hits") || !result["hits"].is_array()) {
        return hits;
    }
    for (const json& j : result["hits"]) {
        if (!j.is_object()) {
            continue;
        }
        const std::string snippet = j.value("snippet", std::string());
        SearchHit h = snippetOf(snippet, text, snippet.size() + 1); // keep the world's snippet whole, find the match
        h.snippet = snippet;
        if (h.matchStart != std::string::npos) {
            h.matchStart = lower(snippet).find(lower(text));
        }
        h.object = j.value("object", std::string());
        h.field = j.value("field", std::string());
        h.name = j.value("name", std::string());
        h.kind = j.value("kind", std::string());
        if (h.kind.empty()) {
            h.kind = h.field.empty() ? "object" : (h.field == "message/text" ? "message" : "field");
        }
        h.seq = j.contains("seq") && j["seq"].is_number_integer() ? j["seq"].get<Seq>() : 0;
        h.score = j.contains("score") && j["score"].is_number() ? j["score"].get<double>() : h.score;
        hits.push_back(std::move(h));
    }
    return hits;
}

} // namespace ke::chat
