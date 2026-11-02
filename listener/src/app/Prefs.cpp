#include "app/Prefs.hpp"

#include "util/Log.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace ke {

std::optional<Prefs> loadPrefs(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        return std::nullopt;
    }
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        log::warn("prefs: ignoring unreadable {}", file.string());
        return std::nullopt;
    }
    Prefs p;
    p.url = j.value("url", "");
    p.token = j.value("token", "");
    p.bundledToken = j.value("bundled-token", "");
    if (j.contains("view") && j["view"].is_object()) {
        p.thinking = j["view"].value("thinking", false);
        p.filter = j["view"].value("filter", "all");
        p.searchLast = j["view"].value("search-last", "");
        p.treeFlat = j["view"].value("tree-flat", false);
        p.theme = j["view"].value("theme", std::string("prometheus"));
        p.historyOpen = j["view"].value("history-open", false);
        p.fontUi = j["view"].value("font-ui", std::string());
        p.labelCase = j["view"].value("label-case", std::string());
        p.osNotices = j["view"].value("os-notices", true);
        p.mapTwilight = j["view"].value("map-twilight", false);
        p.focusFollowsMouse = j["view"].value("focus-follows-mouse", true);
        p.tooltips = j["view"].value("tooltips", true);
        p.fontMono = j["view"].value("font-mono", std::string());
        p.fontUiSize = j["view"].value("font-ui-size", 14.0f);
        p.fontMonoSize = j["view"].value("font-mono-size", 14.0f);
        if (j["view"].contains("tree-expanded") && j["view"]["tree-expanded"].is_array()) {
            for (const auto& id : j["view"]["tree-expanded"]) {
                if (id.is_string()) {
                    p.treeExpanded.push_back(id.get<std::string>());
                }
            }
        }
        if (j["view"].contains("pinned") && j["view"]["pinned"].is_array()) {
            for (const auto& id : j["view"]["pinned"]) {
                if (id.is_string()) {
                    p.pinned.push_back(id.get<std::string>());
                }
            }
        }
        if (p.filter != "all" && p.filter != "chat" && p.filter != "system") {
            p.filter = "all";
        }
    }
    if (j.contains("principal") && j["principal"].is_string() && !j["principal"].get<std::string>().empty()) {
        p.principal = j["principal"].get<std::string>();
    }
    return p;
}

bool savePrefs(const std::filesystem::path& file, const Prefs& prefs) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    nlohmann::json j = {{"url", prefs.url},
                        {"token", prefs.token},
                        {"bundled-token", prefs.bundledToken},
                        {"view", {{"thinking", prefs.thinking}, {"filter", prefs.filter}, {"search-last", prefs.searchLast},
                                  {"tree-flat", prefs.treeFlat}, {"theme", prefs.theme},
                                  {"font-ui", prefs.fontUi}, {"label-case", prefs.labelCase}, {"focus-follows-mouse", prefs.focusFollowsMouse}, {"tooltips", prefs.tooltips}, {"os-notices", prefs.osNotices}, {"map-twilight", prefs.mapTwilight}, {"font-mono", prefs.fontMono},
                                  {"font-ui-size", prefs.fontUiSize}, {"font-mono-size", prefs.fontMonoSize}, {"history-open", prefs.historyOpen}, {"tree-expanded", prefs.treeExpanded}, {"pinned", prefs.pinned}}}};
    if (prefs.principal) {
        j["principal"] = *prefs.principal;
    }
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            log::warn("prefs: could not write {}", tmp.string());
            return false;
        }
        out << j.dump(2) << '\n';
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        log::warn("prefs: could not write {}: {}", file.string(), ec.message());
        return false;
    }
    return true;
}

} // namespace ke
