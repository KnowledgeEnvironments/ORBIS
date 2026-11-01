// The input line: `>` prompt in white, a blinking block cursor, Enter submits, up/down recall
// the last 50 lines. Escape, precisely: with text in the line it clears the line (one kill slot,
// Up or Cmd-Z brings it back, like a shell); on an empty line it reports `escapedEmpty` so the
// pane deselects; the line keeps keyboard focus either way (ImGui's default Esc-defocus is
// undone). A second Escape within 400 ms while a loop runs is the pane's stop gesture. The target decides where a line goes (a Principal's terminal submits to that
// Principal; the phone's strip submits to the session's own Chat). A label at the right end
// shows the route state (`NO ROUTE` in amber while the target has no inbox). The ImGui text
// field does the editing; the block cursor is drawn over its caret cell.
#pragma once

#include "chat/Chat.hpp"
#include "chat/InputLine.hpp"
#include "ui/Theme.hpp"

#include <functional>
#include <imgui.h>
#include <string>
#include <vector>

namespace ke::ui {

struct LineTarget {
    chat::InputLine* history = nullptr;             // recall buffer (per terminal)
    std::function<void(const std::string&)> submit; // Enter
    std::string label;                              // right end: ROUTE xxxx / NO ROUTE
    ImU32 labelColor = Ink::secondary;
    std::string labelTooltip;
    std::function<void()> onLabelClick;             // the label is a control when set (cycle recipients)
    bool enabled = true;
    bool connected = true;                          // off, the line still takes typing and Return keeps it
    std::vector<std::string>* tags = nullptr;       // set: `@` completes names and picks objects into it
    bool working = false;                           // the Principal is working: Return queues
    bool allowEmpty = false;                        // an attachment says enough: Return sends an empty line
    std::function<void(const std::string&)> steer;  // Shift-Return and [ STEER ] while working
    std::function<void()> attach;                   // set where a file can go with a line: the `+` beside it
    std::function<void(const std::string&)> talk;   // set when a Model can hear: a recording, as WAV
};

class InputPane {
  public:
    // The session principal's own line (the phone's strip, the smoke's shape).
    void draw(chat::Chat& chat, const Anim& anim, ImFont* mono);
    void drawBody(chat::Chat& chat, const Anim& anim, ImFont* mono);
    // Any target.
    void drawLine(LineTarget& target, const Anim& anim, ImFont* mono);

    bool active() const { return active_; } // the field has keyboard focus (the cursor blinks)
    void requestFocus() { focusRequested_ = true; }
    // The field's rectangle on screen (for the phone: SDL_SetTextInputArea keeps it above the
    // keyboard). Valid after drawBody.
    ImVec2 fieldMin() const { return fieldMin_; }
    ImVec2 fieldMax() const { return fieldMax_; }
    const std::string& buffer() const { return text_; }
    void clear() {
        text_.clear();
        restore_.clear();
    }
    // A line the world refused comes back, unless a new one has been started.
    void putBack(const std::string& line) {
        if (text_.empty() && !line.empty()) {
            text_ = line;
            restore_ = line;
        }
    }
    // True once after an Escape on an empty line (the pane clears its selection).
    bool takeEscapedEmpty() {
        const bool v = escapedEmpty_;
        escapedEmpty_ = false;
        return v;
    }
    const std::string& killed() const { return killed_; } // the last line Escape cleared
    // What was said goes into the line to read and edit; it is never sent from here.
    void addTranscript(const std::string& said) {
        const std::string joined = text_.empty() || said.empty() ? text_ + said : text_ + " " + said;
        text_ = joined;
        restore_ = joined;
        focusRequested_ = true;
    }
    void setVoiceNote(std::string note) { voiceNote_ = std::move(note); }

  private:
    static int callback(ImGuiInputTextCallbackData* data);
    void drawTalk(LineTarget& target);
    bool holding_ = false;     // TALK is down and the microphone is open
    bool cutOff_ = false;      // the limit ended it: nothing more until TALK is let go
    double heldSince_ = 0.0;
    std::string voiceNote_;

    std::string text_;
    std::string killed_;        // the kill slot
    std::string restore_;       // applied by the callback on the next frame (Cmd-Z)
    bool escapedEmpty_ = false;
    bool active_ = false;
    bool focusRequested_ = false;
    bool atHover_ = false;      // the pointer is on the `@` list
    std::vector<std::string> atNames_; // the `@` completion list on show, with its ids
    std::vector<std::string> atIds_;
    std::size_t atFrom_ = std::string::npos; // where the `@` is in the line
    int atIndex_ = 0;
    int atKey_ = 0; // set by the callback: 1 accept
    ImVec2 fieldMin_, fieldMax_;
};

} // namespace ke::ui
