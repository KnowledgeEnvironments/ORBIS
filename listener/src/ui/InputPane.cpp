#include "ui/InputPane.hpp"

#include "ui/Chrome.hpp"

#include "platform/Platform.hpp"

#include "chat/MessageCache.hpp"
#include "drive/Commands.hpp"
#include "chat/Conversations.hpp"
#include "ui/SymbolUi.hpp"

#include <algorithm>
#include <cmath>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace ke::ui {

namespace {

struct CallbackCtx {
    chat::InputLine* history = nullptr;
    std::string* killed = nullptr;  // Up on an empty line restores the kill slot first
    std::string* restore = nullptr; // Cmd-Z: text to put back, consumed here
    int atCount = 0;                // the `@` list is open: Up, Down and Tab belong to it
    int* atIndex = nullptr;
    int* atKey = nullptr;
};

} // namespace

int InputPane::callback(ImGuiInputTextCallbackData* data) {
    auto* ctx = static_cast<CallbackCtx*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (ctx->restore && !ctx->restore->empty()) {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, ctx->restore->c_str());
            data->SelectAll();
            data->ClearSelection();
            ctx->restore->clear();
        }
        return 0;
    }
    if (ctx->atCount > 0 && data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        *ctx->atKey = 1;
        return 0;
    }
    if (ctx->atCount > 0 && data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        const int step = data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
        *ctx->atIndex = (*ctx->atIndex + step + ctx->atCount) % ctx->atCount;
        return 0;
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || ctx->history == nullptr) {
        return 0;
    }
    std::optional<std::string> line;
    if (data->EventKey == ImGuiKey_UpArrow) {
        if (data->BufTextLen == 0 && ctx->killed && !ctx->killed->empty()) {
            line = *ctx->killed; // the kill slot comes back before the history
            ctx->killed->clear();
        } else {
            line = ctx->history->up(std::string(data->Buf, static_cast<std::size_t>(data->BufTextLen)));
        }
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        line = ctx->history->down();
    }
    if (line) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, line->c_str());
        data->SelectAll();
        data->ClearSelection();
    }
    return 0;
}

void InputPane::draw(chat::Chat& chat, const Anim& anim, ImFont* mono) {
    beginPane("INPUT", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawBody(chat, anim, mono);
    endPane();
}

void InputPane::drawBody(chat::Chat& chat, const Anim& anim, ImFont* mono) {
    putBack(chat.takeFailedSend());
    LineTarget t;
    t.history = &chat.input();
    t.submit = [&chat](const std::string& line) { chat.submit(line); };
    switch (chat.routeState()) {
    case chat::RouteState::Found:
        t.label = "ROUTE " + chat::idPrefix(chat.route());
        t.labelTooltip = "route " + chat.route();
        break;
    case chat::RouteState::Searching:
        t.label = "ROUTE ?";
        break;
    case chat::RouteState::None:
    case chat::RouteState::Unknown:
        t.label = "NO ROUTE";
        t.labelColor = Ink::inspected;
        break;
    }
    drawLine(t, anim, mono);
}

// Held, the microphone is open, and only then: let go and the recording leaves for a transcript the
// person reads in the line before sending anything. Two minutes is what one frame to the world holds.
void InputPane::talkStep(bool down, const std::function<void(const std::string&)>& talk) {
    constexpr double kMaxSeconds = 120.0;
    const double now = ImGui::GetTime();
    const auto finish = [&] {
        holding_ = false;
        const std::string wav = platform::stopRecording();
        if (wav.empty()) {
            voiceNote_ = "nothing recorded";
        } else {
            talk(wav);
        }
    };
    if (down && !holding_ && !cutOff_) {
        holding_ = platform::startRecording();
        heldSince_ = now;
        voiceNote_ = holding_ ? "listening" : "no microphone";
    } else if (holding_ && (!down || now - heldSince_ >= kMaxSeconds)) {
        cutOff_ = down;
        finish();
    } else if (!down) {
        cutOff_ = false;
    }
}

void InputPane::drawTalk(LineTarget& target) {
    bracketButtonNamed("talk", "talk", true, holding_);
    talkStep(ImGui::IsItemActive(), target.talk);
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("hold to speak; the words land in the line to read before sending");
    }
    // the note sits after the bracket when the row has room for it; a narrow row (a phone) keeps
    // STEER and the label on screen instead (seen on the iPhone Duo, 2026-11-11)
    if (!voiceNote_.empty() && ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(voiceNote_.c_str()).x + ImGui::CalcTextSize("[ STEER ] ROUTE 00000000").x) {
        ImGui::SameLine();
        text(Ink::secondary, voiceNote_);
    }
}

// The same hold, as a bar a thumb finds without looking: the whole width, as tall as asked, the
// word in the middle saying what it is doing. The transcript lands in the line above it, to read
// before sending, like the bracket's.
void InputPane::drawTalkBar(const std::function<void(const std::string&)>& talk, float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (width <= 0.0f) {
        width = ImGui::GetContentRegionAvail().x;
    }
    ImGui::InvisibleButton("talk-bar", ImVec2(width, height));
    drive::hit("talk-bar", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    // The on-screen keyboard may close when the bar takes focus, which relays out the phone while
    // the finger is still down.  Once recording starts, follow the physical hold rather than the
    // item's moving rectangle; otherwise that relayout looks like an early release.
    const bool down = ImGui::IsItemActive() || (holding_ && ImGui::IsMouseDown(ImGuiMouseButton_Left));
    talkStep(down, talk);
    const bool busy = !holding_ && voiceNote_ == "transcribing";
    chrome(dl, pos, ImVec2(pos.x + width, pos.y + height), holding_ ? ChromeRole::PressedControl : ChromeRole::RaisedControl);
    if (busy) {
        // the recording is on its way to the Model: the same tape as a thinking agent, along the bar's foot
        hazardTape(dl, ImVec2(pos.x + 2.0f, pos.y + height - 6.0f), ImVec2(pos.x + width - 2.0f, pos.y + height - 2.0f), ImGui::GetTime());
    }
    const std::string word = holding_ ? "LISTENING" : busy ? "TRANSCRIBING" : voiceNote_.empty() || voiceNote_ == "listening" ? "HOLD TO TALK" : upper(voiceNote_);
    const ImVec2 size = ImGui::CalcTextSize(word.c_str());
    const ImVec2 at(pos.x + std::floor((width - size.x) / 2), pos.y + std::floor((height - size.y) / 2));
    dl->AddText(at, holding_ ? Ink::onSelection : Ink::nav, word.c_str());
    if (holding_) {
        // a dot that breathes while the microphone is open, so a held bar is seen to be listening
        const float r = 3.0f + 2.0f * static_cast<float>(0.5 + 0.5 * std::sin(ImGui::GetTime() * 6.0));
        dl->AddCircleFilled(ImVec2(at.x - 14.0f, at.y + size.y / 2), r, Ink::onSelection);
    }
}

void InputPane::drawLine(LineTarget& target, const Anim& anim, ImFont* mono) {
    ImGui::PushFont(mono, 0.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float cw = ImGui::CalcTextSize("0").x;
    const float labelWidth = target.label.empty() ? 0.0f : ImGui::CalcTextSize(target.label.c_str()).x;

    // Chips: what `@` picked for the next line, above it. A click drops one.
    if (target.tags && !target.tags->empty()) {
        for (std::size_t i = 0; i < target.tags->size(); ++i) {
            const std::string id = (*target.tags)[i];
            const chat::SymbolTable::Entry* e = ui::symbolUi().table() ? ui::symbolUi().table()->byId(id) : nullptr;
            const std::string label = "@" + (e && !e->name.empty() ? e->name : chat::idPrefix(id));
            ImGui::PushID(static_cast<int>(i));
            if (i > 0) {
                ImGui::SameLine();
            }
            if (bracketButton(label.c_str(), true, true)) {
                target.tags->erase(target.tags->begin() + static_cast<std::ptrdiff_t>(i));
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    if (target.attach) {
        if (squareButton("+", "attach-file")) {
            target.attach();
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("send a file with your next line; you can also paste or drop one");
        }
    } else {
        text(Ink::strong, ">");
    }
    ImGui::SameLine();
    // what sits beside the field is measured out of it: a row wider than the pane scrolls the whole
    // pane sideways when the field takes focus, and the pane's left edge slides under the left bar
    const float steerWidth = (target.working && target.steer != nullptr)
                                 ? ImGui::CalcTextSize("[ STEER ]").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x
                                 : 0.0f;
    const float noteWidth = voiceNote_.empty() ? 0.0f : ImGui::CalcTextSize(voiceNote_.c_str()).x + style.ItemSpacing.x;
    const float talkWidth = target.talk ? ImGui::CalcTextSize("[ TALK ]").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x + noteWidth : 0.0f;
    const float fieldWidth =
        ImGui::GetContentRegionAvail().x - labelWidth - (labelWidth > 0 ? style.ItemSpacing.x : 0) - steerWidth - talkWidth;
    ImGui::SetNextItemWidth(fieldWidth > cw * 8 ? fieldWidth : cw * 8);
    if (focusRequested_) {
        ImGui::SetKeyboardFocusHere();
        focusRequested_ = false;
    }
    // Cmd-Z on an empty, focused line brings the kill slot back (applied inside the callback,
    // which is the only place that may edit the active buffer).
    const bool wasActive = active_;
    if (wasActive && text_.empty() && !killed_.empty() && ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_Z)) {
        restore_ = killed_;
        killed_.clear();
    }
    const std::string before = text_;
    const bool escNow = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    CallbackCtx ctx{target.history, &killed_, &restore_, static_cast<int>(atNames_.size()), &atIndex_, &atKey_};
    ImGui::PushStyleColor(ImGuiCol_Border, Ink::secondary);
    // this field shows the block alone: ImGui's caret would stand beside it on its own blink clock
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImGui::GetColorU32(ImGuiCol_FrameBg));
    ImGui::BeginDisabled(!target.enabled);
    const bool entered = ImGui::InputText("##line", &text_,
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                                              ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_CallbackAlways |
                                              ImGuiInputTextFlags_CallbackCompletion,
                                          &InputPane::callback, &ctx);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    fieldMin_ = ImGui::GetItemRectMin();
    fieldMax_ = ImGui::GetItemRectMax();
    active_ = ImGui::IsItemActive();
    if (active_) {
        ImGui::SetNavCursorVisible(false); // its own frame shows the focus; the ring would cross the row above
    }
    const ImGuiID id = ImGui::GetItemID();
    if (escNow && wasActive) {
        if (!before.empty()) {
            killed_ = before; // EscapeClearsAll emptied the line; keep it for Up / Cmd-Z
        } else {
            escapedEmpty_ = true; // the pane deselects
        }
        if (!active_) {
            ImGui::SetKeyboardFocusHere(-1); // ImGui's Escape also drops focus; take it back
            active_ = true;
        }
    }

    // The block cursor over the caret cell: inverse when on, hollow when the field is idle.
    {
        std::size_t cursor = text_.size();
        float scrollX = 0.0f;
        if (active_) {
            if (ImGuiInputTextState* state = ImGui::GetInputTextState(id)) {
                cursor = static_cast<std::size_t>(
                    std::max(0, std::min(state->GetCursorPos(), static_cast<int>(text_.size()))));
                scrollX = state->Scroll.x;
            }
        }
        const float x = fieldMin_.x + style.FramePadding.x +
                        ImGui::CalcTextSize(text_.c_str(), text_.c_str() + cursor).x - scrollX;
        const ImVec2 cell(cw, ImGui::GetTextLineHeight());
        const ImVec2 pos(x, fieldMin_.y + style.FramePadding.y);
        if (pos.x + cell.x <= fieldMax_.x - style.FramePadding.x + 1.0f && pos.x >= fieldMin_.x) {
            const char* under = cursor < text_.size() ? text_.c_str() + cursor : nullptr;
            blockCursor(ImGui::GetWindowDrawList(), pos, cell, anim.blinkOn, active_, under);
        }
    }

    if (target.talk) {
        ImGui::SameLine();
        drawTalk(target);
    }

    if (!target.label.empty()) {
        ImGui::SameLine();
        text(target.labelColor, target.label);
        drive::hit("line-label", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (target.onLabelClick) {
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (ImGui::IsItemClicked()) {
                target.onLabelClick();
            }
        }
        if (ImGui::IsItemHovered() && !target.labelTooltip.empty()) {
            ke::ui::tip("%s", target.labelTooltip.c_str());
        }
    }

    // `@` completion: the word being typed at the end of the line. Picked with Tab, Enter or a
    // click; the name goes into the line and the object into the Message's tags.
    if (target.tags) {
        const std::size_t at = text_.rfind('@');
        const bool word = at != std::string::npos && (at == 0 || text_[at - 1] == ' ') &&
                          text_.find(' ', at) == std::string::npos;
        std::string prefix = word ? text_.substr(at + 1) : std::string();
        atNames_.clear();
        atIds_.clear();
        if (word && ui::symbolUi().table()) {
            for (const auto* e : ui::symbolUi().table()->complete(prefix, 8)) {
                atNames_.push_back(e->name);
                atIds_.push_back(e->id);
            }
        }
        atFrom_ = word ? at : std::string::npos;
        // `/` directives, the same list: a whole word at the start of the line. Once typed out, Return sends it.
        if (!word) {
            for (const chat::Directive& d : chat::directiveMatches(text_)) {
                if (text_ != d.word) {
                    atNames_.push_back(d.word);
                    atIds_.emplace_back();
                    atFrom_ = 0;
                }
            }
        }
        atIndex_ = atNames_.empty() ? 0 : std::min(atIndex_, static_cast<int>(atNames_.size()) - 1);
        int clicked = -1;
        // A click on the list takes focus from the field before this frame draws: the pointer
        // being over the list keeps it up long enough to pick.
        if (!atNames_.empty() && (active_ || atHover_)) {
            const float rowH = ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetNextWindowPos(ImVec2(fieldMin_.x, fieldMin_.y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(cw * 40, rowH * static_cast<float>(atNames_.size()) + style.WindowPadding.y * 2));
            ImGui::Begin("##at-complete", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoSavedSettings);
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            for (std::size_t i = 0; i < atNames_.size(); ++i) {
                const bool on = static_cast<int>(i) == atIndex_;
                // On the press: the field loses focus to this click, and the list closes with it
                // before a release could arrive.
                if (ImGui::Selectable(atNames_[i].c_str(), on, ImGuiSelectableFlags_SelectOnClick)) {
                    clicked = static_cast<int>(i);
                }
                drive::hit("at:" + atNames_[i], ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
            atHover_ = ImGui::IsWindowHovered();
            ImGui::End();
        } else {
            atHover_ = false;
        }
        const bool accept = !atNames_.empty() && (atKey_ == 1 || entered || clicked >= 0);
        atKey_ = 0;
        if (accept) {
            const int pick = clicked >= 0 ? clicked : atIndex_;
            const std::string object = atIds_[static_cast<std::size_t>(pick)];
            const std::string line = text_.substr(0, atFrom_) + atNames_[static_cast<std::size_t>(pick)] + " ";
            if (!object.empty() && std::find(target.tags->begin(), target.tags->end(), object) == target.tags->end()) {
                target.tags->push_back(object);
            }
            text_ = line;
            restore_ = line;
            atNames_.clear();
            atIds_.clear();
            atIndex_ = 0;
            if (clicked >= 0) {
                focusRequested_ = true; // the click holds the active item this frame: take it back on the next
            } else {
                ImGui::ActivateItemByID(id);
            }
            ImGui::PopFont();
            return;
        }
    }

    // Plain Return never interrupts: while the Principal works it queues. Shift-Return, or
    // [ STEER ] where there is no Shift, is the one way in; idle it is just Return.
    const bool wantSteer = target.working && target.steer != nullptr;
    bool steerNow = false;
    if (wantSteer) {
        ImGui::SameLine();
        steerNow = bracketButtonNamed("steer", "steer", !text_.empty());
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("interrupt the running call with this line (Shift-Return)");
        }
    }
    if (entered && ImGui::GetIO().KeyAlt) { // Option-Return: a newline, the line stays open
        text_ += '\n';
        restore_ = text_;
        ImGui::ActivateItemByID(id);
    } else if ((entered || steerNow) && !target.connected) {
        ImGui::ActivateItemByID(id); // nothing to send it to yet: the line stays, and stays open
    } else if ((entered || steerNow) && target.enabled) {
        const bool steering = steerNow || (wantSteer && ImGui::GetIO().KeyShift);
        if (target.history && !text_.empty()) {
            target.history->push(text_);
        }
        if (steering) {
            target.steer(text_);
        } else if (target.submit && (target.allowEmpty || !text_.empty())) {
            target.submit(text_);
        }
        text_.clear();
        ImGui::SetKeyboardFocusHere(-1); // keep typing
    }
    ImGui::PopFont();
}

} // namespace ke::ui
