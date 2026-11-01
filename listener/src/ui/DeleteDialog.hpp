// DELETE on any object: says in plain words what it does (removed from view, kept in the log,
// restorable), shows the dependents when the world refuses, and offers FORCE then.
#pragma once

#include "chat/Chat.hpp"
#include "model/Principals.hpp"

#include <imgui.h>
#include <string>

namespace ke::ui {

class DeleteDialog {
  public:
    void open(const std::string& object);
    void draw(const model::Principals& fold, chat::Chat& chat);
    bool isOpen() const { return open_; }

  private:
    std::string object_;
    std::string reason_;
    std::string error_;
    bool conflict_ = false;
    bool busy_ = false;
    bool open_ = false;
    bool justOpened_ = false;
};

} // namespace ke::ui
