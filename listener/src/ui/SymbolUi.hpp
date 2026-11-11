// What a linked symbol does, in one place, so a name behaves the same in the transcript, HISTORY,
// FIELDS, kernel records and view tables. Hover: name, class, driven-by, loop state, hint, full
// id. Click: a Principal opens its terminal, another Object its Fields, a Field scrolls to it, a
// seq scrolls HISTORY. Right-click: terminal, fields, graph, copy id, the world's actions. The
// panes only say what is under the mouse; the frame carries out the request (it owns the tabs).
#pragma once

#include "chat/Symbols.hpp"
#include "ui/Markdown.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace ke::ui {

struct LinkRequest {
    enum Kind { Doc, Terminal, Message, Fields, Field, History, Graph, Action, Inspect, Delete, Restore, Scratch, OpenFile, OpenPane, Label, EditField } kind = Terminal;
    std::string object; // Inspect: the id, or the name when `byName`
    bool byName = false;
    std::string field;    // Field, and EditField: a Field of `object` to open in the editor
    Seq seq = 0;
    std::string message;  // OpenFile: an image this Message carries, at `index`; Message: the one to show
    int index = 0;
    std::string tex;      // OpenFile: a typeset source's first page, instead
    std::string path;     // OpenFile: below `object`, or absolute with no object
    int line = 0;         // OpenFile: the line a citation names, 0 for the top
    std::string fileObject; // OpenFile: a file Object, which knows its own mount
    std::string openKind; // OpenFile: a module named by OPEN WITH; "" lets the registry decide
    bool newPane = false; // OpenFile: OPEN IN NEW PANE
    std::string action;
    nlohmann::json args = nlohmann::json::object();
};

class SymbolUi {
  public:
    void attachImageWanted() { attachImage_ = true; }
    bool takeAttachImage() {
        const bool v = attachImage_;
        attachImage_ = false;
        return v;
    }
    void bind(const chat::SymbolTable* table, const model::Principals* fold, const model::Actions* actions) {
        table_ = table;
        fold_ = fold;
        actions_ = actions;
    }
    const chat::SymbolTable* table() const { return table_; }
    const model::Principals* fold() const { return fold_; }

    // The tooltip and the hand cursor for the symbol under the mouse.
    void hover(const chat::Symbol& s);
    void click(const chat::Symbol& s);
    void openMenu(const chat::Symbol& s);
    // Any pane can ask; the frame carries it out.
    void ask(LinkRequest r) { request_ = std::move(r); }
    // A drawn document's symbols: returns true while one is under the mouse.
    bool handleDoc(const MdExtras& md, const SelDoc& doc, const SelResult& r, bool dragging);
    // One line of text with its symbols linked, for tables and rows. `context` is the object whose
    // Fields count; `plain` draws without links (a selected row is inverse video).
    void text(const std::string& s, ImU32 color, const std::string& context, bool plain = false);
    // The menu, once per frame after the panes.
    void drawMenu();
    std::optional<LinkRequest> take();

  private:
    bool attachImage_ = false;
    const chat::SymbolTable* table_ = nullptr;
    const model::Principals* fold_ = nullptr;
    const model::Actions* actions_ = nullptr;
    std::optional<chat::Symbol> menu_;
    bool openPending_ = false;
    std::optional<LinkRequest> request_;
};

SymbolUi& symbolUi();

// What a named action's form starts with when invoked on an object; `enabled` false when the
// action makes no sense there. Shared with the graph's node menu.
nlohmann::json actionPrefill(const std::string& action, const std::string& object, const std::string& cls,
                             bool& enabled);

} // namespace ke::ui
