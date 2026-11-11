#include "chat/Conversations.hpp"

#include <algorithm>
#include <cctype>

namespace ke::chat {

namespace {

std::string str(const json& o, const char* key) {
    return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
}

long num(const json& o, const char* key) {
    return o.contains(key) && o[key].is_number() ? o[key].get<long>() : 0;
}

bool containsFolded(std::string_view text, std::string_view query) {
    auto eq = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    return std::search(text.begin(), text.end(), query.begin(), query.end(), eq) != text.end();
}

} // namespace

std::vector<ConversationRow> parseConversations(const json& result) {
    std::vector<ConversationRow> out;
    if (!result.is_object() || !result.contains("conversations") || !result["conversations"].is_array()) {
        return out;
    }
    for (const json& j : result["conversations"]) {
        if (!j.is_object() || str(j, "thread").empty()) {
            continue;
        }
        ConversationRow r;
        r.thread = str(j, "thread");
        r.route = str(j, "route");
        r.title = str(j, "title");
        r.firstAt = str(j, "first-at");
        r.lastAt = str(j, "last-at");
        r.lastSeq = j.contains("last-seq") && j["last-seq"].is_number() ? j["last-seq"].get<long long>() : 0;
        r.messages = num(j, "messages");
        r.tokensIn = num(j, "tokens-in");
        r.tokensOut = num(j, "tokens-out");
        r.compressed = j.contains("compressed") && j["compressed"].is_boolean() && j["compressed"].get<bool>();
        if (j.contains("participants") && j["participants"].is_array()) {
            for (const json& p : j["participants"]) {
                if (p.is_string()) {
                    r.participants.push_back(p.get<std::string>());
                }
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

bool conversationMatches(const ConversationRow& row, std::string_view query) {
    if (query.empty() || containsFolded(row.title, query)) {
        return true;
    }
    return std::any_of(row.participants.begin(), row.participants.end(), [query](const std::string& p) { return containsFolded(p, query); });
}

std::vector<Directive> directiveMatches(std::string_view line) {
    std::vector<Directive> out;
    if (line.empty() || line.front() != '/' || line.find(' ') != std::string_view::npos) {
        return out;
    }
    for (const Directive& d : kDirectives) {
        if (std::string_view(d.word).rfind(line, 0) == 0) {
            out.push_back(d);
        }
    }
    return out;
}

long compressAt(long configured, long budgetTokens) {
    return configured > 0 ? configured : budgetTokens * 7 / 10;
}

bool nearCompression(long contextTokens, long threshold) {
    return threshold > 0 && contextTokens * 10 >= threshold * 9;
}

ContextGauge contextGauge(long contextTokens, long compressAtTokens, long windowTokens) {
    ContextGauge g;
    g.used = std::max(0L, contextTokens);
    g.window = std::max(0L, windowTokens);
    g.compressAt = compressAtTokens;
    g.known = g.window > 0;
    if (g.known) {
        g.fill = std::min(1.0f, static_cast<float>(g.used) / static_cast<float>(g.window));
        g.mark = std::min(1.0f, static_cast<float>(g.compressAt) / static_cast<float>(g.window));
    }
    g.near = nearCompression(g.used, g.compressAt);
    return g;
}

} // namespace ke::chat
