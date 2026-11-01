// OFF THE RECORD: an in-memory fork of a Principal, kept apart from everything that persists. It
// is a window of its own rather than a terminal tab, so nothing reaches session.json, prefs, the
// event store, search or the log. Its text lives in this object and is gone at END.
#pragma once

#include "chat/Chat.hpp"
#include "model/ScratchFold.hpp"
#include "net/Protocol.hpp"

#include <chrono>
#include <string>

#include <imgui.h>

namespace ke::ui {

class ScratchPane {
  public:
    void ask(const std::string& principal) { asked_ = principal; }
    void push(const Push& p);
    void draw(const model::Principals& principals, chat::Chat& chat, ImFont* mono, bool& showReasoning);
    // Ends a running session, for quitting and for a change of world.
    void end(chat::Chat& chat);
    bool active() const { return !scratch_.empty(); }
    int count() const { return active() ? 1 : 0; }

  private:
    void drawConfirm(const model::Principals& principals, chat::Chat& chat);
    void wipe();
    void send(chat::Chat& chat);

    std::string asked_;
    std::string endpoint_, endpointOf_; // the Model and where its calls go, as the world says
    std::string scratch_;
    std::string principal_;
    std::string error_;
    std::string line_;
    model::ScratchFold fold_;
    std::chrono::steady_clock::time_point began_{};
    bool beginning_ = false;
    bool closing_ = false;
    std::string reasoning_ = "off"; // this session's, never the Principal's
    bool stick_ = true;
};

} // namespace ke::ui
