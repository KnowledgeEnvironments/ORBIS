// A module is what a pane shows. It draws into a rect, reads the keyboard while focused, answers
// to its verbs and picks up from `pane/*` state. Everything it does to the world goes through the
// Host as a `pane.*` action, so a remote client and a model do exactly what a person does. A
// module knows no other module and no platform; adding one touches modules/Registry.cpp only.
#pragma once

#include "model/PaneFold.hpp"
#include "net/Protocol.hpp"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ke::modules {

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// A file somewhere: a mount or file Object and a path below it. What "open this" carries.
struct FileRef {
    std::string target; // the mount or file Object
    std::string path;
    std::string kind;    // "" lets the registry decide
    bool newPane = false; // a split, rather than a tab of the focused group
    std::string message;  // an image a Message carries, with `index`, instead of a file
    int index = 0;
    std::string fileObject; // a file Object: the world finds its mount; `path` only names it
    std::string tex;      // a typeset source's first page, instead
    int line = 0;         // the line to show, 0 for the top
    std::string field;    // a Field of `target` instead of a file: `path` is empty, kind is editor
};

// What a module may ask of the client, and nothing else.
class Host {
  public:
    using Reply = std::function<void(const Response&)>;
    virtual ~Host() = default;
    virtual void act(const std::string& action, nlohmann::json args, Reply reply) = 0;
    virtual void open(const FileRef& file) = 0;
    virtual void inspect(const std::string& object) = 0;
    virtual void copy(const std::string& text) = 0;
    virtual std::string paste() = 0;
    // The on-screen keyboard, on platforms that have one: while true, typed text reaches the module.
    virtual void wantKeyboard(bool on) = 0;
    virtual std::string nameOf(const std::string& object) const = 0;
    virtual std::string self() const = 0; // this client's Principal
    // A binary Field of a pane (pane/image, pane/pdf), fetched in chunks; `err` says why not.
    virtual void readBytes(const std::string& pane, const std::string& field,
                           std::function<void(std::vector<unsigned char> bytes, std::string err)> done) = 0;
};

class Module {
  public:
    virtual ~Module() = default;
    // Called on attach and whenever the fold's view of the pane changes.
    virtual void restore(const model::PaneInfo& pane) = 0;
    virtual void draw(Rect rect, bool focused) = 0;
    // Before draw, while focused and no text field holds the keyboard.
    virtual void keyboard() {}
    // A push from `pane.subscribe`.
    virtual void data(const PaneData&) {}
    // Someone else drove this pane: re-read what they changed. `who` is a display name.
    virtual void driven(const model::PaneInput&, const std::string& who) { (void)who; }
    // A dirty document asks before it closes; the text says what would be lost.
    virtual std::string closeWarning() const { return ""; }
    // Rendered or source, for the modules that have both.
    virtual bool hasSource() const { return false; }
    virtual bool showingSource() const { return false; }
    virtual void toggleSource() {}
    // Show this line, 1-based, as soon as the text is here.
    virtual void goToLine(int) {}
    // What the drive API and the status line read.
    virtual nlohmann::json describe() const { return nlohmann::json::object(); }
    // An error this pane is showing, for the modeline when nobody is looking at the pane.
    virtual std::string heldError() const { return ""; }
    // The text of the current selection, for Copy from the pane's menu.
    virtual std::string selection() const { return ""; }
    virtual void pasteText(const std::string&) {}
};

} // namespace ke::modules
