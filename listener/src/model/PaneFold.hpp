// Panes as the listener folds them (docs/panes.md). A pane is an Object with pane/* Fields, so it
// arrives like any other Object: created, field-set, snapshot. It lives in the world; this is what
// one client knows about it. The events pane-opened, pane-input and pane-closed say who drove it.
#pragma once

#include "model/Event.hpp"
#include "model/Principals.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ke::model {

struct SnapshotObject;

// The last input a pane took: who, from where, and the text when a model sent it.
struct PaneInput {
    std::string who;
    std::string from;
    std::string text;
    bool model = false;
    Seq seq = 0;
};

struct PaneInfo {
    std::string id;
    std::string kind;   // pane/kind: terminal, files, editor, markdown, latex
    std::string target; // pane/target
    std::string title;  // pane/title
    std::string label;  // object/label: a pretty name a person gave it
    std::string field;  // pane/field: an editor on a Field of the target Object, not a file
    std::string state;  // pane/state: open, exited
    std::string owner;  // pane/owner
    int cols = 0;
    int rows = 0;
    bool hasExit = false;
    int exitCode = 0; // pane/exit
    PaneInput lastInput;
    std::vector<std::string> attached; // sessions or names the world reports, when it does
    Seq created = 0;
    bool open() const { return state != "exited"; }
    const std::string& shown() const { return !label.empty() ? label : !title.empty() ? title : kind; }
};

class PaneFold {
  public:
    bool apply(const Event& e);
    void seed(const SnapshotObject& o);
    const PaneInfo* get(std::string_view id) const;
    std::vector<const PaneInfo*> ordered() const; // in creation order
    std::size_t size() const { return panes_.size(); }

  private:
    void setField(PaneInfo& p, const std::string& name, const json& value);
    std::map<std::string, PaneInfo, std::less<>> panes_;
};

} // namespace ke::model
