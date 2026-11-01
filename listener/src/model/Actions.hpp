// The world's named actions as a pure fold over Events (sprint 1 W3/L6): an action is an Object
// bound in the world namespace as `action/<name>` with the Fields action/name, action/schema (the
// Malli form of its args as EDN text), action/handler, action/gated and instructions/own (one
// paragraph for whoever calls it). The listener builds its dialogs and menus from these, so a
// new action in the world appears in the client without a release; nothing here is hardcoded.
//
//   created {class object fields {action/name ...}} -> an action
//   bound {name "action/<n>" object}                -> an action by name (Fields may follow)
//   field-set on its stream                          -> any Field
#pragma once

#include "model/Event.hpp"
#include "model/Principals.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ke::model {

struct ActionInfo {
    std::string id;
    std::string name;    // action/name, or the bound name after `action/`
    std::string schema;  // action/schema, EDN text of a Malli vector form
    std::string handler; // action/handler
    std::string help;    // instructions/own
    bool gated = false;  // action/gated
    std::map<std::string, FieldState> fields;
    Seq created = 0;
    const FieldState* field(std::string_view name) const;
};

struct SnapshotObject;

class Actions {
  public:
    bool apply(const Event& e);
    void seed(const SnapshotObject& o);
    void setApplied(Seq seq) { applied_ = std::max(applied_, seq); }
    const ActionInfo* get(std::string_view name) const;
    const ActionInfo* byId(std::string_view id) const;
    // Every action by name.
    std::vector<const ActionInfo*> ordered() const;
    std::size_t size() const { return actions_.size(); }
    Seq applied() const { return applied_; }
    void clear();

  private:
    ActionInfo& ensure(const std::string& id);
    void setField(ActionInfo& a, const std::string& name, const FieldState& f);
    std::map<std::string, ActionInfo> actions_; // by object id
    std::map<std::string, std::string> byName_;  // name -> id
    Seq applied_ = -1;
};

} // namespace ke::model
