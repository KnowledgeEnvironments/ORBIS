#include "ui/ScratchPane.hpp"

#include "ui/ReasoningRow.hpp"

#include "drive/Commands.hpp"
#include "ui/Theme.hpp"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace ke::ui {

namespace {

constexpr long kMaxSeconds = 7200; // the world's default :scratch-max-s
constexpr const char* kTools = "it can read and search, and may write Fields that vanish; it cannot run anything";

// `gpt-5.6-luna at OpenAI, on your ChatGPT plan`, or `glm at http://localhost:8000/v1`: the Model and what reads the words.
std::string endpointLine(const std::string& model, const std::string& endpoint) {
    if (endpoint.empty()) {
        return model;
    }
    return model.empty() ? endpoint : model + " at " + endpoint;
}

const json& resultOf(const Response& r) {
    return r.result.contains("result") ? r.result["result"] : r.result;
}

} // namespace

void ScratchPane::push(const Push& p) {
    if (p.sub != "scratch-" + scratch_) {
        return; // a session already ended: nothing of it is kept
    }
    for (const Event& e : p.events) {
        fold_.apply(e);
    }
}

void ScratchPane::wipe() {
    fold_.clear();
    line_.clear();
    scratch_.clear();
    principal_.clear();
    error_.clear();
    closing_ = false;
    reasoning_ = "off";
}

void ScratchPane::end(chat::Chat& chat) {
    if (active()) {
        chat.actNamed("scratch.end", json{{"scratch", scratch_}}, [](const Response&) {});
    }
    ImGui::ClearActiveID();
    wipe();
}

void ScratchPane::send(chat::Chat& chat) {
    if (line_.empty() || fold_.answering()) {
        return;
    }
    fold_.said(line_);
    json args{{"scratch", scratch_}, {"text", line_}, {"reasoning", reasoning_}};
    chat.actNamed("scratch.send", std::move(args), [this](const Response& r) {
        if (!r.ok) {
            error_ = r.err ? r.err->code + ": " + r.err->message : std::string("refused");
        }
    });
    line_.clear();
    stick_ = true;
}

void ScratchPane::drawConfirm(const model::Principals& principals, chat::Chat& chat) {
    if (!ImGui::IsPopupOpen("OFF THE RECORD")) {
        ImGui::OpenPopup("OFF THE RECORD");
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::CalcTextSize("0").x * 72, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, Ink::unresolved);
    if (ImGui::BeginPopupModal("OFF THE RECORD", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        text(Ink::unresolved, "Off the record with " + principals.nameOf(asked_));
        ImGui::PushTextWrapPos(0.0f);
        text(Ink::strong, "You talk to a private copy of it, forked from now. Nothing you say or it answers is written to "
                             "the log, its memory or this client, and it all goes when you end.");
        text(Ink::strong, kTools);
        if (endpointOf_ != asked_) {
            // Asked once for this Principal: what reads the words is the thing to know before saying them.
            endpointOf_ = asked_;
            endpoint_.clear();
            chat.actNamed("model.chosen", json{{"principal", asked_}}, [this](const Response& r) {
                if (r.ok) {
                    const json& v = resultOf(r);
                    endpoint_ = endpointLine(v.value("id", std::string()), v.value("endpoint", std::string()));
                }
            });
        }
        text(Ink::inspected, "Not private from the model server: the Model still receives the text, and whatever it logs is "
                             "outside this world. The log records that a session happened, with no content.");
        text(Ink::unresolved, endpoint_.empty() ? std::string("Goes to: unknown") : "Goes to: " + endpoint_);
        ImGui::PopTextWrapPos();
        if (!error_.empty()) {
            text(Ink::inspected, error_);
        }
        dialogGap();
        if (bracketButtonNamed("go off the record", "scratch-begin", !beginning_, false, Ink::unresolved)) {
            beginning_ = true;
            error_.clear();
            const std::string who = asked_;
            chat.actNamed("scratch.begin", json{{"principal", who}}, [this, who](const Response& r) {
                beginning_ = false;
                if (!r.ok) {
                    error_ = r.err ? r.err->code + ": " + r.err->message : std::string("refused");
                    return;
                }
                const json& v = resultOf(r);
                scratch_ = v.value("scratch", "");
                principal_ = who;
                fold_.setPrincipal(v.value("principal", who));
                endpoint_ = endpointLine(v.value("model-id", std::string()), v.value("endpoint", std::string()));
                asked_.clear();
                began_ = std::chrono::steady_clock::now();
                ImGui::CloseCurrentPopup();
            });
        }
        ImGui::SameLine();
        if (bracketButtonNamed("cancel", "scratch-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            asked_.clear();
            error_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
}

void ScratchPane::draw(const model::Principals& principals, chat::Chat& chat, ImFont* mono, bool& showReasoning) {
    if (!asked_.empty() && !active()) {
        drawConfirm(principals, chat);
    } else if (!asked_.empty()) {
        asked_.clear(); // one session at a time
    }
    if (!active()) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::CalcTextSize("0").x * 90, ImGui::GetTextLineHeight() * 30), ImGuiCond_FirstUseEver);
    ImGui::PushStyleColor(ImGuiCol_Border, Ink::unresolved);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    bool open = true;
    const std::string title = "OFF THE RECORD // " + principals.nameOf(principal_) + " // nothing is kept###scratch";
    const bool shown = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                                              ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    if (!open) {
        closing_ = true;
    }
    if (shown) {
        ImGui::PushFont(mono, 0.0f);
        const long spent = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - began_).count();
        const long left = std::max(0L, kMaxSeconds - spent);
        dimText("%s", kTools);
        text(Ink::unresolved, endpoint_.empty() ? std::string("Goes to: unknown") : "Goes to: " + endpoint_);
        const std::string ctx = fold_.status();
        text(Ink::secondary, ctx.empty() ? std::string("CTX none yet") : ctx);
        dimText("%lld in \xc2\xb7 %lld out this session \xc2\xb7 ends in %ld h %02ld m", fold_.tokensIn(), fold_.tokensOut(), left / 3600, (left % 3600) / 60);
        ImGui::SameLine();
        if (bracketButtonNamed("end", "scratch-end", true, false, Ink::unresolved)) {
            closing_ = true;
        }
        if (!error_.empty()) {
            text(Ink::inspected, error_);
        }
        const float inputH = ImGui::GetFrameHeightWithSpacing();
        const bool working = fold_.answering();
        const float inputRows = working ? 2.0f : 1.0f;
        const float rowH = reasoningRowHeight(ImGui::GetContentRegionAvail().x, true);
        if (ImGui::BeginChild("said", ImVec2(0, -(inputH * inputRows + rowH)), ImGuiChildFlags_AlwaysUseWindowPadding)) {
            ImGui::PushTextWrapPos(0.0f);
            for (const model::ScratchLine& l : fold_.lines()) {
                text(l.note ? Ink::inspected : l.self ? Ink::strong : Ink::primary, (l.self ? "> " : "") + l.text);
                if (const std::string cost = fold_.costLine(l); !cost.empty()) {
                    text(Ink::secondary, cost);
                }
            }
            if (const std::string think = fold_.thinking(); showReasoning && !think.empty()) {
                text(Ink::secondary, think);
            }
            if (const std::string reply = fold_.partial(); !reply.empty()) {
                text(Ink::secondary, reply);
            }
            ImGui::PopTextWrapPos();
            if (stick_ || working) {
                ImGui::SetScrollHereY(1.0f);
                stick_ = false;
            }
        }
        ImGui::EndChild();
        insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (working) {
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            hazardSpinner(ImGui::GetFontSize() * 5.0f, ImGui::GetTime());
            ImGui::SameLine();
            text(Ink::primary, fold_.progress(principals.nameOf(principal_), now));
            ImGui::SameLine();
            if (bracketButtonNamed("stop", "scratch-stop")) {
                chat.actNamed("scratch.stop", json{{"scratch", scratch_}}, [this](const Response& r) {
                    if (!r.ok) {
                        error_ = "this world cannot stop a reply here yet: " + (r.err ? r.err->code : std::string("refused"));
                    }
                });
            }
        }
        // Nothing here touches the real Principal: the mode is this session's, default off.
        ReasoningRow row;
        row.mode = reasoning_;
        row.source = "this session only; the Principal's own setting is not changed";
        row.scope = "scratch-";
        if (const std::optional<std::string> chosen = drawReasoningRow(row, showReasoning)) {
            reasoning_ = *chosen;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##said", fold_.answering() ? "answering" : "say something that will not be kept", &line_,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            send(chat);
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    if (closing_) {
        if (!ImGui::IsPopupOpen("END OFF THE RECORD")) {
            ImGui::OpenPopup("END OFF THE RECORD");
        }
        if (ImGui::BeginPopupModal("END OFF THE RECORD", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            text(Ink::strong, "This conversation goes when it ends. It cannot be brought back.");
            dialogGap();
            if (bracketButtonNamed("end it", "scratch-end-confirm", true, false, Ink::unresolved)) {
                ImGui::CloseCurrentPopup();
                end(chat);
            }
            ImGui::SameLine();
            if (bracketButtonNamed("keep talking", "scratch-end-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                closing_ = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

} // namespace ke::ui
