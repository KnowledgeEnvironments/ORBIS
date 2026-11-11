// Unsafe mode as a client sees it: `world/unsafe` on the world, a list of who is in it. The red
// indicator arms from this and nothing else, so a second client, or an arm from elsewhere, shows.
#pragma once

#include "model/Tasks.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ke::chat {

struct UnsafeEntry {
    std::string principal;
    std::string until; // ISO-8601
    std::string by;
    std::string kind;  // "" a Principal, `unconfined` an Adapter, `shared-terminal` a person's pane
    std::string agent; // a shared terminal: who has it
};

// A missing or malformed Field reads as off.
std::vector<UnsafeEntry> parseUnsafe(const nlohmann::json& field);
// The same list from `read {object <world>, field "world/unsafe"}`, which answers the Object.
std::vector<UnsafeEntry> parseUnsafeRead(const nlohmann::json& result);
// The rest of the time in whole minutes, at least 1 while it is running, 0 once past.
long minutesLeft(const std::string& until, double nowEpoch);
// `UNSAFE ivy 23m`, or `UNSAFE 3` for several; "" when nobody is in it.
std::string unsafeLabel(const std::vector<UnsafeEntry>& entries, const std::function<std::string(const std::string&)>& nameOf,
                        double nowEpoch);
// Tasks a Principal's unsafe mode let through: `task/rule` "unsafe", newest first.
std::vector<const model::TaskInfo*> ranUnsafe(const std::vector<const model::TaskInfo*>& tasks);

} // namespace ke::chat
