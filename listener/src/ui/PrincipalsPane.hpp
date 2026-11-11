// Every Principal the fold knows, one row each: name (from its binding), class, driven-by,
// route count, and the loop chip (CARRIER while a cycle runs, STALLED in red with the reason on
// hover, IDLE otherwise; nothing for Principals that are not process-driven). Selecting a row
// makes that Principal the current one and opens its terminal.
#pragma once

#include "model/Activity.hpp"
#include "model/Evaluations.hpp"
#include "model/ObjectTree.hpp"
#include "model/Principals.hpp"
#include "model/Tasks.hpp"
#include "ui/Theme.hpp"

#include <imgui.h>
#include <set>
#include <string>

namespace ke::ui {

class PrincipalsPane {
  public:
    // Returns the id clicked this frame, or "".
    std::string draw(const model::Principals& principals, const std::string& current, const Anim& anim, ImFont* mono);
    std::string drawBody(const model::Principals& principals, const std::string& current, const Anim& anim, ImFont* mono);
    // The running cost per Principal (evaluations, tokens, context) for the row's hover.
    void setEvaluations(const model::Evaluations* evals) { evals_ = evals; }
    // Pending approvals per Principal (a PENDING badge on the requester's row).
    void setTasks(const model::Tasks* tasks) { tasks_ = tasks; }
    void setActivity(const std::map<std::string, model::Activity>* marks) { activity_ = marks; }
    // [ NEW PRINCIPAL ] was pressed this frame (the app opens the create dialog).
    bool takeNewRequested() { const bool r = newRequested_; newRequested_ = false; return r; }
    // [ SHOW DELETED ] was turned on (a snapshot leaves retired objects out: the app asks the world).
    bool takeRetiredRequested() { const bool r = retiredRequested_; retiredRequested_ = false; return r; }
    std::string takeRestoreRequested() { std::string r; r.swap(restoreRequested_); return r; }
    // PIN TO TOP / UNPIN was chosen this frame: the app applies it to the fold and saves prefs.
    bool takePinned(std::string& out) { out = pinned_; const bool r = !pinned_.empty(); pinned_.clear(); return r; }
    bool takeUnpinned(std::string& out) { out = unpinned_; const bool r = !unpinned_.empty(); unpinned_.clear(); return r; }
    // Persisted by the app.
    bool flat = false;
    std::set<std::string> expanded; // everything starts folded
    std::set<std::string> pinnedIds; // rows pinned to the top of the list
    std::string self;               // who this client is: their row says `you`
    bool takePrefsDirty() { const bool r = prefsDirty_; prefsDirty_ = false; return r; }

  private:
    void drawRow(const model::Principals& principals, const std::string& id, const std::string& current, const Anim& anim, std::string& clicked, int& index);
    void drawActor(const model::Principals& principals, const model::TreeRoot& root, const std::string& current, const Anim& anim, std::string& clicked, int& index);
    void drawLoopChips(const model::Principals& principals, const model::PrincipalInfo& p, bool inverse, const Anim& anim);
    void drawMenu(const model::Principals& principals, const std::string& id, const std::string& route, std::string& clicked);
    bool showDeleted_ = false;
    bool retiredRequested_ = false;
    std::string restoreRequested_;
    bool prefsDirty_ = false;
    std::string pinned_, unpinned_; // the menu's pick this frame, taken by takePinned/takeUnpinned
    const model::Evaluations* evals_ = nullptr;
    const model::Tasks* tasks_ = nullptr;
    const std::map<std::string, model::Activity>* activity_ = nullptr;
    bool newRequested_ = false;
};

} // namespace ke::ui
