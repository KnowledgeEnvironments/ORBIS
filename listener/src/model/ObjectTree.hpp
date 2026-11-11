// The PRINCIPALS tree, read as a person would say it: who acts here, and for each one what it
// thinks with, what it touches and who it talks to. Pure, derived from real relations
// (principal/models, principal/adapters, principal/workspaces, route/members), never from dots in
// names. A thing several actors use simply appears under each, and hover says who else. What no
// actor uses lands in `unattached`, which is where leftovers show up.
#pragma once

#include "model/Principals.hpp"

#include <string>
#include <utility>
#include <vector>

namespace ke::model {

struct TreeItem {
    std::string id;                  // what a click opens
    std::string route;               // `talks to`: the Route it goes through
    std::vector<std::string> others; // who else uses it
    bool operator==(const TreeItem&) const = default;
};

// One line per purpose: `model  root.model`, `talks to  person, root`.
struct TreeLine {
    std::string label;
    std::vector<TreeItem> items;
    bool operator==(const TreeLine&) const = default;
};

struct TreeRoot {
    std::string id;
    std::string role; // you, person, agent
    std::vector<TreeLine> lines;
    bool operator==(const TreeRoot&) const = default;
};

struct ObjectTree {
    std::vector<TreeRoot> roots;        // people first, then agents, by name
    std::vector<std::string> unattached;
};

// `self` is the person this client attaches as: their row says `you`.
ObjectTree buildObjectTree(const Principals& fold, const std::string& self = "");

// What each word in the tree means, one line each, for the `?`.
std::vector<std::pair<std::string, std::string>> treeLegend();

// Why an object may not be deleted, in plain words, or "" when it may: the world, root and the
// first person hold everything else up.
std::string deleteBlocked(const Principals& fold, const std::string& id);

} // namespace ke::model
