// Has a Principal seen an edit? `loop/seen-seq` marks what its last wake took in, so a person can be told
// plainly: applies at its next wake, then seen. The rules are world/changes.clj's.
#pragma once

#include "model/Principals.hpp"

namespace ke::model {

enum class FieldNews { None, Waiting, Seen };

// Is this Field something a Principal hears about? Not its running figures.
bool aboutThePrincipal(const std::string& field);
FieldNews fieldNews(const Principals& fold, const PrincipalInfo& info, const FieldState& f);
// Edits made since its last wake: the count on its tab.
int waitingEdits(const Principals& fold, const PrincipalInfo& info);

} // namespace ke::model
