#include "workspace/Shell.hpp"

namespace ke::workspace {

BindPlan shellBind() {
    const std::string host = hostName();
    return BindPlan{"shell (local)", "shell/" + host, placeholderUuid("shell/" + host)};
}

} // namespace ke::workspace
