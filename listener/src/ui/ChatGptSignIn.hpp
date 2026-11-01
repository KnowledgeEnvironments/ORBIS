// Sign in with ChatGPT from this client. The browser comes back here, so the loopback callback lives
// here; the world holds everything else, and no token ever reaches a client (world/.../chatgpt.clj).
#pragma once

#include "chat/Chat.hpp"
#include "platform/Platform.hpp"

#include <SDL3/SDL.h>
#include <string>

namespace ke::ui {

class ChatGptSignIn {
public:
    // model: "" for a new Model, else the one to renew.
    void start(chat::Chat& chat, const std::string& model);
    void cancel();
    void poll(chat::Chat& chat); // once a frame
    void drawWelcome();          // once a frame: the first sign-in's confirmation
    bool busy() const { return busy_; }
    const std::string& status() const { return status_; }
    bool failed() const { return failed_; }
    const std::string& url() const { return url_; }

private:
    void say(std::string text, bool failed);

    platform::LoopbackCallback loopback_;
    bool busy_ = false;
    bool failed_ = false;
    bool welcome_ = false;
    std::string attempt_, status_, url_, usage_;
    Uint64 saidAt_ = 0;
};

ChatGptSignIn& chatGptSignIn();

} // namespace ke::ui
