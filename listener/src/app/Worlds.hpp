// The worlds this machine can attach to (sprint 2: several worlds). session.json already keeps one
// entry per world URL (session, principal, acked, and since `hello` the world's id and name); the
// token for each lives apart in worlds.json, mode 0600, next to it. Nothing here talks to the
// network: the picker reads both files, `hello` on a fresh socket answers name and head.
#pragma once

#include "net/Session.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ke::app {

struct KnownWorld {
    std::string url;      // the key (scheme host port path, no query)
    std::string id;       // world id from hello, "" when never heard
    std::string name;     // world/name, "" when unknown
    std::string principal;
    std::string lastAttached; // ISO-8601, "" when never
    bool hasToken = false;
};

class WorldBook {
  public:
    // sessionFile: the session.json path; worlds.json sits beside it.
    explicit WorldBook(std::filesystem::path sessionFile);
    const std::filesystem::path& tokensFile() const { return tokens_; }
    // Known worlds, most recently attached first; the current URL is included even when unknown.
    std::vector<KnownWorld> list(const std::string& currentUrl) const;
    std::optional<std::string> tokenFor(const std::string& url) const;
    // Save the token for a URL (0600). Returns false when the file could not be written.
    bool remember(const std::string& url, const std::string& token) const;
    bool forget(const std::string& url) const;

  private:
    std::map<std::string, std::string> readTokens() const;
    bool writeTokens(const std::map<std::string, std::string>& tokens) const;
    std::filesystem::path session_;
    std::filesystem::path tokens_;
};

// `orbis://pair?url=<ws url>&code=<code>` (what the DEVICES tab shows as text and QR, and what the
// phone receives through its URL scheme): parse one, "" fields when absent.
struct PairLink {
    std::string url;
    std::string code;
    std::string label;
    bool valid() const { return !url.empty() && !code.empty(); }
};
PairLink parsePairLink(const std::string& link);
std::string makePairLink(const std::string& url, const std::string& code);

} // namespace ke::app
