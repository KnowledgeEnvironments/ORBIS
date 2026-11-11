// Command-line options shared by the windowed app and --smoke.
#pragma once

#include <optional>
#include <string>

namespace ke {

struct Options {
    std::string url = "ws://127.0.0.1:7777/ws";
    std::string format = "json"; // fixed; the listener speaks the JSON form only (ADR 0002)
    std::string token;
    std::string tokenProblem; // why token is empty: shown in SESSION, nobody attaches
    std::optional<std::string> principal;
    std::string assetsDir;
    std::string dataDir;  // where session.json (and prefs.json on iOS) live; empty = platform default
    int smokeSeconds = 0; // > 0 runs the network stack without SDL for that long and exits
    std::string driveSocket; // --drive <unix socket> / LISTENER_DRIVE: the UX drive API (desktop only)
};

} // namespace ke
