// The world's symbols in running text. A pure scan over the listener's own fold: what the fold
// holds is what this Principal may read, so a Grant decides what links and nothing can leak.
// The model is never asked to escape anything; it already mistypes ids. Rules (listener-style.md,
// "Symbols"):
//   root.route  grid.fs/README.md  task.run   dotted or slashed bound names always link
//   a full id                                 links and displays by name
//   88969202                                  an 8+ hex prefix links when unique; matching nothing
//                                             where an id belongs, it is `unresolved` (a mistyped id)
//   [[id|name|class]]                         displays as the name
//   loop/stalled                              a Field of the object in context
//   seq 680                                   the row in HISTORY
//   root  person  harness                     single words link only in code spans or tags:
//                                             prose stays prose
#pragma once

#include "model/Actions.hpp"
#include "model/Evaluations.hpp"
#include "model/Principals.hpp"
#include "model/Tasks.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ke::chat {

enum class SymbolKind { Object, Path, Field, Seq, Unresolved };

struct Symbol {
    std::size_t start = 0; // bytes in the scanned text, [start, end)
    std::size_t end = 0;
    SymbolKind kind = SymbolKind::Object;
    std::string object;  // Object, Path, Field
    std::string field;   // Field: the Field's name
    std::string path;    // Path: below the mount (`README.md` of `grid.fs/README.md`)
    int line = 0;        // Path: the line it cites (`README.md:7-15` is line 7), 0 for none
    Seq seq = 0;         // Seq
    std::string display; // what to show: the source text, or the name for an id or a tag
    bool operator==(const Symbol&) const = default;
};

struct ScanContext {
    const model::Principals* fold = nullptr; // where Field names are checked
    std::string object;                      // the object in context (a terminal's Principal, a row's stream)
    bool code = false;                       // inside a code span: single-word names link too
};

class SymbolTable {
  public:
    struct Entry {
        std::string id;
        std::string name; // "" for what has no name (a Message, an Evaluation)
        std::string cls;  // principal, adapter, model, group, route, action, task, evaluation, message, ""
    };

    // Rebuilds only when a name or an object appeared; returns true then. Cheap to call per frame.
    bool refresh(const model::Principals& fold, const model::Actions* actions = nullptr,
                 const model::Tasks* tasks = nullptr, const model::Evaluations* evals = nullptr);
    // Bumped by each rebuild: a key for documents built from this table.
    unsigned long long version() const { return version_; }

    const Entry* byId(std::string_view id) const;
    const Entry* byName(std::string_view name) const;
    // The one entry whose id starts with `prefix`; nullptr for none or several.
    const Entry* byPrefix(std::string_view prefix) const;
    std::size_t size() const { return entries_.size(); }

    std::vector<Symbol> scan(std::string_view text, const ScanContext& ctx = {}) const;
    // Named entries starting with `prefix`, case-insensitive: Principals first, then by name.
    std::vector<const Entry*> complete(std::string_view prefix, std::size_t limit = 8) const;

  private:
    using Key = std::tuple<unsigned long long, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t>;
    std::vector<Entry> entries_;                         // sorted by id
    std::unordered_map<std::string, std::size_t> names_; // name -> index in entries_
    Key key_{};
    bool built_ = false;
    unsigned long long version_ = 0;
};

} // namespace ke::chat
