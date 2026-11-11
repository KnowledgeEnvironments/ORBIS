#include "app/Worlds.hpp"

#include "util/Log.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sys/stat.h>

namespace ke::app {

namespace {

std::string urlDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const std::string hex = s.substr(i + 1, 2);
            char* end = nullptr;
            const long v = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                out.push_back(static_cast<char>(v));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

} // namespace

WorldBook::WorldBook(std::filesystem::path sessionFile)
    : session_(std::move(sessionFile)), tokens_(session_.parent_path() / "worlds.json") {}

std::map<std::string, std::string> WorldBook::readTokens() const {
    std::map<std::string, std::string> out;
    std::ifstream in(tokens_);
    if (!in) {
        return out;
    }
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("tokens") || !j["tokens"].is_object()) {
        return out;
    }
    for (auto& [url, tok] : j["tokens"].items()) {
        if (tok.is_string()) {
            out[url] = tok.get<std::string>();
        }
    }
    return out;
}

bool WorldBook::writeTokens(const std::map<std::string, std::string>& tokens) const {
    std::error_code ec;
    std::filesystem::create_directories(tokens_.parent_path(), ec);
    nlohmann::json j = {{"tokens", tokens}};
    const std::filesystem::path tmp = tokens_.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            log::warn("worlds: could not write {}", tmp.string());
            return false;
        }
        out << j.dump(2) << '\n';
    }
    ::chmod(tmp.c_str(), 0600); // tokens: owner only
    std::filesystem::rename(tmp, tokens_, ec);
    if (ec) {
        log::warn("worlds: could not write {}: {}", tokens_.string(), ec.message());
        return false;
    }
    return true;
}

std::vector<KnownWorld> WorldBook::list(const std::string& currentUrl) const {
    std::vector<KnownWorld> out;
    const std::map<std::string, std::string> tokens = readTokens();
    const std::string currentKey = FileSessionStore::worldKey(currentUrl);
    bool sawCurrent = false;
    for (const FileSessionStore::KnownWorld& k : FileSessionStore::listWorlds(session_)) {
        KnownWorld w;
        w.url = k.url;
        w.id = k.entry.world;
        w.name = k.entry.worldName;
        w.principal = k.entry.principal.value_or("");
        w.lastAttached = k.entry.lastAttached;
        w.hasToken = tokens.count(k.url) != 0;
        sawCurrent = sawCurrent || k.url == currentKey;
        out.push_back(std::move(w));
    }
    for (const auto& [url, tok] : tokens) {
        bool known = false;
        for (const KnownWorld& w : out) {
            known = known || w.url == url;
        }
        if (!known) {
            KnownWorld w;
            w.url = url;
            w.hasToken = true;
            sawCurrent = sawCurrent || url == currentKey;
            out.push_back(std::move(w));
        }
    }
    if (!sawCurrent && !currentKey.empty()) {
        KnownWorld w;
        w.url = currentKey;
        out.insert(out.begin(), std::move(w));
    }
    return out;
}

std::optional<std::string> WorldBook::tokenFor(const std::string& url) const {
    const std::map<std::string, std::string> tokens = readTokens();
    auto it = tokens.find(FileSessionStore::worldKey(url));
    if (it == tokens.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool WorldBook::remember(const std::string& url, const std::string& token) const {
    std::map<std::string, std::string> tokens = readTokens();
    tokens[FileSessionStore::worldKey(url)] = token;
    return writeTokens(tokens);
}

bool WorldBook::forget(const std::string& url) const {
    std::map<std::string, std::string> tokens = readTokens();
    tokens.erase(FileSessionStore::worldKey(url));
    return writeTokens(tokens);
}

PairLink parsePairLink(const std::string& link) {
    PairLink out;
    const std::string prefix = "orbis://pair?";
    if (link.rfind(prefix, 0) != 0) {
        return out;
    }
    std::string q = link.substr(prefix.size());
    std::size_t pos = 0;
    while (pos <= q.size()) {
        const std::size_t amp = q.find('&', pos);
        const std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq);
            const std::string v = urlDecode(kv.substr(eq + 1));
            if (k == "url") {
                out.url = v;
            } else if (k == "code") {
                out.code = v;
            } else if (k == "label") {
                out.label = v;
            }
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return out;
}

std::string makePairLink(const std::string& url, const std::string& code) {
    return "orbis://pair?url=" + urlEncode(url) + "&code=" + urlEncode(code);
}

} // namespace ke::app
