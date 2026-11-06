// What the world's Principals have told this person (`notify`). A line to read and a link to follow:
// a notification carries no authority, so nothing here acts on one's say-so.
#pragma once

#include "chat/Chat.hpp"
#include "chat/Notifications.hpp"
#include "model/Principals.hpp"

#include <imgui.h>

namespace ke::ui {

class NotificationsPane {
  public:
    void toggle() { open_ = !open_; }
    bool open() const { return open_; }
    void draw(chat::Notifications& notifications, const model::Principals& principals, chat::Chat& chat, ImFont* mono);

  private:
    bool open_ = false;
};

// The menu bar's chip: `notices 3`, amber while something loud is unread. True when clicked.
bool noticesChip(const chat::Notifications& notifications, bool open, bool menuBar = false); // a menu-bar word on the desktop, a bracket on the phone

} // namespace ke::ui
