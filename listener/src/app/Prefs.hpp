// Settings that survive a restart: on iOS the world's URL, the token, and an optional principal
// (there is no command line); on both platforms the whimsy switches. prefs.json sits next to
// session.json (macOS: ~/Library/Application Support/ORBIS/, iOS: SDL_GetPrefPath("ORBIS",
// "listener")). On macOS url/token come from the flags in main.cpp and are not read from here.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ke {

struct Prefs {
    std::string url;
    std::string token;
    std::optional<std::string> principal;
    std::string bundledToken; // the token baked into the bundle, last adopted, so a new bake wins once
    // View switches.
    bool thinking = false; // SHOW REASONING: the Evaluation's evaluation/thinking Field above each reply
    std::string filter = "all"; // the transcript filter: all | chat | system (heartbeats, approval results)
    bool treeFlat = false;      // PRINCIPALS: the flat table instead of the tree
    std::vector<std::string> treeExpanded; // PRINCIPALS: rows unfolded
    bool historyOpen = false; // HISTORY whole; the desktop starts with the strip
    std::string theme = "prometheus"; // a theme file in the data dir, when there is one; per client
    // VIEW, FONT, per client: "" keeps the theme's face. Sizes in points.
    std::string fontUi, fontMono;
    std::string labelCase; // VIEW, LABELS: caps or sentence; "" keeps the theme's
    bool mapTwilight = false; // VIEW, TWILIGHT MAP: the map at dusk
    bool focusFollowsMouse = true; // VIEW, FOCUS FOLLOWS MOUSE: typing goes where the pointer is, with no click
    bool osNotices = true; // WORLD, OS NOTIFICATIONS: loud notices reach the desktop's own centre
    float fontUiSize = 14.0f, fontMonoSize = 14.0f;
    std::string searchLast;     // the last query typed in the search box, offered again next time
};

std::optional<Prefs> loadPrefs(const std::filesystem::path& file);
bool savePrefs(const std::filesystem::path& file, const Prefs& prefs);

} // namespace ke
