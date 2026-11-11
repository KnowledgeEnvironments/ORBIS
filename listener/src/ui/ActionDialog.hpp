// Dialogs built from the world's named actions (sprint 1 L6): a form for any action's args from
// its `action/schema` (ui/ActionForm), with a dedicated two-step flow for a new Principal:
//
//   NEW PRINCIPAL   principal.create {name charter model routes-from grant{scope ops} ...}
//                   then [ MOUNT WORKSPACE ] -> workspace.mount {principal path name shell}
//   <any action>    the generic form, prefilled from the context it was opened in (a node, an
//                   Adapter, the current terminal)
//
// Every op result shows inline; nothing is pre-drawn: the new node appears when the world's
// events arrive. When the world has no `action/<name>` Object the dialog says so in dim text and
// offers nothing else (no raw-op fallback: the action is the contract).
#pragma once

#include "chat/Chat.hpp"
#include "model/Actions.hpp"
#include "model/Principals.hpp"
#include "net/Session.hpp"
#include "ui/ActionForm.hpp"
#include "ui/Theme.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <mutex>
#include <string>
#include <vector>

namespace ke::ui {

class ActionDialog {
  public:
    // The two-step flow. `self` is the attached principal (routes-from default).
    void openCreatePrincipal(const std::string& self);
    // The generic form for `action`, prefilled with `defaults` ({arg: value}).
    void openAction(const std::string& action, json defaults = json::object());
    // workspace.mount for an existing Principal (step 2 alone).
    void openMount(const std::string& principal, const std::string& principalName);
    bool isOpen() const { return open_; }
    void close();
    // Draw each frame (a modal popup when open). `window` enables the native folder picker.
    void draw(const model::Principals& principals, const model::Actions& actions, chat::Chat& chat,
              const Session& session, ImFont* mono, SDL_Window* window);

  private:
    struct Step {
        std::string action;             // principal.create, workspace.mount, or any
        std::vector<ArgSpec> specs;     // from the action's schema; empty when unknown/unparsable
        std::string specsErr;
        Seq specsFrom = -2;             // actions.applied() the specs were built at
        FormValues values;
        std::string result;             // the op's answer, shown inline
        bool resultErr = false;
        bool sent = false;              // the request is in flight or done
        std::string createdId;          // principal.create: the new Principal's id from the result
        std::string createdName;
    };
    void rebuildSpecs(Step& s, const model::Actions& actions);
    void submit(Step& s, chat::Chat& chat);
    void drawStep(Step& s, const model::Principals& principals, const model::Actions& actions, chat::Chat& chat,
                  const Session& session, SDL_Window* window, const char* submitWord, const char* driveName);
    FormContext context(const model::Principals& principals, const std::string& scope) const;

    bool open_ = false;
    bool justOpened_ = false;
    std::string title_;
    std::string self_;
    bool twoStep_ = false; // NEW PRINCIPAL: create, then mount
    Step create_;
    Step mount_;
    bool mountShown_ = false;
    std::string prefixFollowed_; // the last name the grant prefix was copied from
    // The native folder picker answers on another thread; the path lands here and is applied on
    // the next frame.
    std::mutex pickMutex_;
    std::string pickedPath_;
    bool pickPending_ = false;
};

} // namespace ke::ui
