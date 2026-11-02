// The Principals of the world as a pure fold over Events (ground rule 3: the log is the only
// state). Fed from an unfiltered subscription that starts at seq 0 (Runtime's "fold" sub) so a
// restarted listener still knows every Principal, its names, its Fields and its Messages.
//
//   created   {class fields}          -> a Principal (class principal, model, adapter, group)
//   bound     {namespace name object} -> a name for any object (the latest binding wins)
//   field-set {name type value by ..} -> a Field on the object the event's stream names
//   sent / delivered {message route recipient?} -> a MessageRef for the transcript
//
// Nothing here is hardcoded for root: whatever Fields the world sets are what the panes show.
#pragma once

#include "model/Event.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ke::model {

struct FieldState {
    std::string name;
    json type;   // "string", "number", ["ref", "message"], ...
    json value;
    std::string reader;
    std::string by; // the Principal that set it (event/actor, or body.by when present)
    std::string at; // event/at
    Seq seq = 0;
    json previous; // the value this write replaced (the event's `previous`), null when unknown
    bool operator==(const FieldState&) const = default;
};

struct PrincipalInfo {
    std::string id;
    std::string cls;      // principal, model, adapter, group
    std::string drivenBy; // person, process, none
    Seq created = 0;
    std::string createdAt;
    std::string createdBy; // actor of the `created` event (root for the Principals it makes)
    std::map<std::string, FieldState> fields;

    const FieldState* field(std::string_view name) const;
    std::vector<std::string> routes() const;  // principal/routes, first is the inbox
    std::vector<std::string> models() const; // principal/models
    std::vector<std::string> workspaces() const; // principal/workspaces
    std::vector<std::string> members() const;    // group/members (groups only)
    int loopIteration() const;                // loop/iteration, 0 when idle
    int loopTurnLimit() const;                // loop/turn-limit, the turns this wake has now, 0 when unknown
    std::string loopStalled() const;          // loop/stalled, "" when clean
};

enum class LoopState { Idle, Running, Stalled, None }; // None: not process-driven
LoopState loopState(const PrincipalInfo& p);
const char* name(LoopState s);

// A Route object (class route): a channel Principals hold in `principal/routes`.
struct RouteInfo {
    std::string id;
    std::string transport; // route/transport, an Adapter, when set
    std::string address;   // route/address
    std::vector<std::string> members; // route/members (Principal refs) when the world records them
    Seq created = 0;
    std::map<std::string, FieldState> fields;
};

struct MessageRef {
    std::string message;
    std::string from;      // the sender (actor of the `sent` / `delivered` event)
    std::string recipient; // "" on `sent`
    std::string route;
    Seq seq = 0;
    std::string at;
    std::optional<std::string> via; // the Evaluation that produced it, when any
    bool operator==(const MessageRef&) const = default;
};

struct SnapshotObject;

class Principals {
  public:
    // Fold one event. Returns true when the picture changed (a frame is due). Events must come in
    // seq order; an event at or below the last applied seq is ignored (idempotent).
    bool apply(const Event& e);
    // Seed from one snapshot object (model/Snapshot): a Principal, Route, or Message as the
    // equivalent created / bound / field-set / sent / delivered events would have left it.
    // Objects of other classes contribute their name only. Call setApplied(snapshot seq) after.
    void seed(const SnapshotObject& o);
    void setApplied(Seq seq) { applied_ = std::max(applied_, seq); }

    const PrincipalInfo* get(std::string_view id) const;
    // The ids the person has pinned to the top of the PRINCIPALS list. A person's own setting, not
    // a fact of the world: it lives in their Prefs, so it survives a restart and is per client.
    void pin(const std::string& id) { pins_.insert(id); ++namesVersion_; }
    void unpin(const std::string& id) { pins_.erase(id); ++namesVersion_; }
    bool pinned(const std::string& id) const { return pins_.count(id) != 0; }
    // All Principals: pinned first (creation order among them), then the rest in creation order.
    std::vector<const PrincipalInfo*> ordered() const;
    std::size_t size() const { return principals_.size(); }
    // The one rule for showing an object: its latest bound name; else an `object/name` or `name`
    // Field it carries; else the first 8 characters of its id. The full id stays in tooltips.
    std::string nameOf(std::string_view id) const;
    // The name agents resolve, ignoring any label: nameOf is `object/label`, then this.
    std::string boundName(std::string_view id) const;
    bool hasName(std::string_view id) const { return names_.count(std::string(id)) != 0; }
    // A name another fold learned for an object that has no binding (a token's label): used by
    // nameOf unless a binding names it.
    void alias(const std::string& id, const std::string& name) {
        if (!name.empty() && names_.count(id) == 0) {
            aliases_[id] = name;
        }
    }
    // Every bound name, for the symbol table; namesVersion moves when one may have changed.
    template <class F> void forEachName(F&& f) const {
        for (const auto& [id, name] : names_) {
            f(id, name);
        }
    }
    unsigned long long namesVersion() const { return namesVersion_; }
    const std::vector<MessageRef>& messages() const { return messages_; }
    // message/kind of a Message: "" for plain talk.
    std::string messageKind(const std::string& message) const;
    // Retired objects (`retired`, undone by `restored`): out of ordered() and every list, kept
    // here with the names they had. A snapshot leaves them out, so SHOW DELETED asks the world.
    struct RetiredInfo {
        std::string name;
        std::string cls;
        Seq seq = 0;
    };
    // The one plain line the world keeps for what this is (`class/description` on the schema
    // Object of its class or Adapter kind), "" when the world has none.
    std::string describe(std::string_view id, const std::string& cls) const;
    bool retired(std::string_view id) const { return retired_.count(std::string(id)) != 0; }
    const std::map<std::string, RetiredInfo>& retiredObjects() const { return retired_; }
    void noteRetired(const std::string& id, RetiredInfo info) {
        retired_[id] = std::move(info);
        ++namesVersion_;
    }
    const RouteInfo* route(std::string_view id) const;
    const std::map<std::string, RouteInfo>& routes() const { return routes_; }
    // Any object's Fields the fold saw (Principals and Routes), for `view/position` and friends.
    const FieldState* fieldOf(std::string_view object, std::string_view name) const;
    Seq applied() const { return applied_; }
    void clear();

  private:
    std::map<std::string, PrincipalInfo> principals_;
    std::map<std::string, RouteInfo> routes_;
    std::map<std::string, std::string> names_;
    std::map<std::string, std::string> aliases_;
    std::vector<MessageRef> messages_;
    std::map<std::string, std::string> messageKinds_;
    std::map<std::string, RetiredInfo> retired_;
    std::map<std::string, std::string> descriptions_; // schema Object -> class/description
    std::set<std::string> pins_;                      // this client's pinned ids, in `ordered()`
    Seq applied_ = -1;
    unsigned long long namesVersion_ = 0;
};

// Whom a terminal line addresses (listener rule, 2026-10-30): a tab for Principal P sends to P,
// except the attached principal's own tab, which is a conversation with the Route's other members:
// `route/members` minus self, else the Route's participants seen in delivered Messages minus self,
// else the Principal that created self (root, for a person); appliances driven by nothing are
// dropped, and when nothing else is known the line goes to self (a poke to one's own loop).
std::vector<std::string> recipientsFor(const Principals& fold, const std::string& principal, const std::string& route,
                                       const std::string& self);

} // namespace ke::model
