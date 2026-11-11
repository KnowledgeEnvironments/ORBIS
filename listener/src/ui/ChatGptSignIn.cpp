#include "ui/ChatGptSignIn.hpp"

#include "ui/Theme.hpp"

#include <imgui.h>
#include <map>
#include <nlohmann/json.hpp>

namespace ke::ui {

namespace {
constexpr int kPort = 1455; // OpenAI's documented example; the flow lets the port vary
constexpr const char* kWelcome = "You're using your ChatGPT plan";
} // namespace

ChatGptSignIn& chatGptSignIn() {
    static ChatGptSignIn s;
    return s;
}

void ChatGptSignIn::say(std::string text, bool failed) {
    status_ = std::move(text);
    failed_ = failed;
    saidAt_ = SDL_GetTicks();
}

void ChatGptSignIn::start(chat::Chat& chat, const std::string& model) {
    if (busy_) {
        return;
    }
    if (!loopback_.start(kPort)) {
        say("sign in from a desktop client: this one cannot hold a browser's callback", true);
        return;
    }
    busy_ = true;
    say("opening your browser", false);
    nlohmann::json args{{"port", loopback_.port()}};
    if (!model.empty()) {
        args["model"] = model;
    }
    chat.actNamed("model.signin.begin", std::move(args), [this](const Response& r) {
        if (!r.ok || !r.result.is_object()) {
            cancel();
            say("could not start: " + (r.err ? r.err->message : std::string("no answer")), true);
            return;
        }
        const nlohmann::json& out = r.result.contains("result") ? r.result["result"] : r.result;
        attempt_ = out.value("attempt", std::string());
        url_ = out.value("url", std::string());
        if (platform::openUrl(url_)) {
            say("finish in your browser", false);
        } else {
            cancel();
            say("could not open a browser", true);
        }
    });
}

void ChatGptSignIn::cancel() {
    loopback_.stop();
    busy_ = false;
    url_.clear();
    say("", false);
}

void ChatGptSignIn::poll(chat::Chat& chat) {
    if (!busy_) {
        // good news fades; a failure stays until the next attempt
        if (!failed_ && !status_.empty() && SDL_GetTicks() - saidAt_ > 8000) {
            status_.clear();
        }
        return;
    }
    std::map<std::string, std::string> q;
    if (!loopback_.poll(q)) {
        return;
    }
    loopback_.stop();
    say("signing in", false);
    nlohmann::json args{{"attempt", attempt_}, {"state", q["state"]}};
    if (q.count("code") != 0) {
        args["code"] = q["code"];
    }
    if (q.count("client_id") != 0) {
        args["client-id"] = q["client_id"];
    }
    if (q.count("error") != 0) {
        args["error"] = q["error"];
    }
    chat.actNamed("model.signin.finish", std::move(args), [this](const Response& r) {
        busy_ = false;
        url_.clear();
        if (!r.ok) {
            say(r.err ? r.err->message : std::string("sign-in failed"), true);
            return;
        }
        const nlohmann::json& out = r.result.contains("result") ? r.result["result"] : r.result;
        welcome_ = out.contains("first") && out["first"].is_boolean() && out["first"].get<bool>();
        // absent or null when the plan names no usage page
        usage_ = out.contains("usage-url") && out["usage-url"].is_string() ? out["usage-url"].get<std::string>() : std::string();
        say("signed in", false);
    });
}

void ChatGptSignIn::drawWelcome() {
    if (welcome_) {
        ImGui::OpenPopup(kWelcome);
        welcome_ = false;
    }
    if (ImGui::BeginPopupModal(kWelcome, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        text(Ink::strong, kWelcome);
        text(Ink::secondary, "Eligible requests here use your ChatGPT plan. Manage usage in ChatGPT settings.");
        dialogGap();
        if (bracketButtonNamed("got it", "signin-got-it") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (bracketButtonNamed("manage usage", "signin-usage") && !usage_.empty()) {
            platform::openUrl(usage_);
        }
        ImGui::EndPopup();
    }
}

} // namespace ke::ui
