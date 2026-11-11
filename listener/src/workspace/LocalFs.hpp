// STUB (sprint 1 does the real thing, spec 15.6): the listener's host as a workspace. For now it
// only prepares the `bind` op that names the local host; the world is expected to answer
// `err not-implemented` or `unresolved` until the host Object exists.
#pragma once

#include "net/Protocol.hpp"

#include <string>

namespace ke::workspace {

struct BindPlan {
    std::string label;  // what the Bind panel calls it
    std::string name;   // bind.args.name
    std::string object; // bind.args.object (uuid; a placeholder derived from the hostname)
    json args() const { return json{{"name", name}, {"object", object}}; }
};

std::string hostName();
std::string placeholderUuid(const std::string& seed); // deterministic uuid-shaped id

BindPlan localFsBind(); // name "workspace/<host>"

} // namespace ke::workspace
