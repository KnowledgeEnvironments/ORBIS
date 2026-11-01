// The Model control beside the input: what this agent or conversation runs on, every Model in the
// world with what each can do, a fallback, and Sign in with ChatGPT. The world decides what a wake
// uses (model.chosen); nothing here guesses it, so two clients never disagree.
#include "ui/ChatGptSignIn.hpp"
#include "ui/terminal/Internal.hpp"

namespace ke::ui {

namespace {
const json* fieldOf(const model::PrincipalInfo* e, const char* name) {
    const model::FieldState* f = e ? e->field(name) : nullptr;
    return f ? &f->value : nullptr;
}

std::string stringOf(const json* v) { return v && v->is_string() ? v->get<std::string>() : std::string(); }

std::string contextOf(const model::PrincipalInfo* e) {
    const json* c = fieldOf(e, "model/context-tokens");
    if (!c || !c->is_number()) {
        return "\xe2\x80\x93";
    }
    const long long n = c->get<long long>();
    return n >= 1000 ? std::to_string(n / 1000) + "k" : std::to_string(n);
}

std::string priceOf(const model::PrincipalInfo* e) {
    if (stringOf(fieldOf(e, "model/api")) == "chatgpt-plan") {
        return "plan";
    }
    const json* pin = fieldOf(e, "model/price-in");
    const json* pout = fieldOf(e, "model/price-out");
    const double in = pin && pin->is_number() ? pin->get<double>() : 0.0;
    const double out = pout && pout->is_number() ? pout->get<double>() : 0.0;
    if (in <= 0.0 && out <= 0.0) {
        return "\xe2\x80\x93";
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f / %.2f", in, out);
    return buf;
}

// A Model's offerings: what the server lists, else its own id. Each row keeps the slug and its name.
std::vector<std::pair<std::string, std::string>> offersOf(const model::PrincipalInfo* e) {
    std::vector<std::pair<std::string, std::string>> out;
    std::map<std::string, std::string> named;
    if (const json* c = fieldOf(e, "model/catalogue"); c && c->is_array()) {
        for (const json& row : *c) {
            if (row.is_object()) {
                named[row.value("slug", std::string())] = row.value("name", std::string());
            }
        }
    }
    if (const json* a = fieldOf(e, "model/available"); a && a->is_array()) {
        for (const json& s : *a) {
            if (s.is_string()) {
                const std::string slug = s.get<std::string>();
                out.emplace_back(slug, named.count(slug) != 0 ? named[slug] : slug);
            }
        }
    }
    if (out.empty()) {
        const std::string id = stringOf(fieldOf(e, "model/id"));
        out.emplace_back(id, id.empty() ? std::string("default") : id);
    }
    return out;
}
} // namespace

void TerminalPane::drawModelPicker(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals,
                                   chat::Chat& chat) {
    std::vector<const model::PrincipalInfo*> models;
    std::string stamp = tab.principal + "|" + tab.lastThread + "|" + std::to_string(tab.chosenAsked);
    for (const model::PrincipalInfo* p : principals.ordered()) {
        if (p->cls == "model") {
            models.push_back(p);
            stamp += stringOf(fieldOf(p, "model/status"));
        }
    }
    const json* choice = fieldOf(info, "principal/model-choice");
    const json* fallback = fieldOf(info, "principal/model-fallback");
    stamp += (choice ? choice->dump() : "") + (fallback ? fallback->dump() : "") + stringOf(fieldOf(info, "loop/model-note"));
    if (stamp != tab.chosenStamp) {
        // a change in the fold, or a choice made here: ask once, never on a timer
        tab.chosenStamp = stamp;
        json args{{"principal", tab.principal}};
        if (!tab.lastThread.empty()) {
            args["thread"] = tab.lastThread;
        }
        json* chosen = &tab.chosen;
        chat.actNamed("model.chosen", std::move(args), [chosen](const Response& r) {
            if (r.ok && r.result.is_object()) {
                *chosen = r.result.contains("result") ? r.result["result"] : r.result;
            }
        });
    }
    const std::string id = tab.chosen.is_object() ? tab.chosen.value("id", std::string()) : std::string();
    const std::string name = tab.chosen.is_object() ? tab.chosen.value("name", std::string()) : std::string();
    const std::string whole = id.empty() ? (name.empty() ? std::string("none") : name) : id;
    const std::string label = "model " + whole.substr(whole.rfind('/') == std::string::npos ? 0 : whole.rfind('/') + 1);
    if (bracketButtonNamed(label.c_str(), "model", true, false)) {
        ImGui::OpenPopup("terminal-model");
    }
    const ImVec2 controlAt = ImGui::GetItemRectMin();
    if (ImGui::IsItemHovered()) {
        const std::string source = tab.chosen.is_object() ? tab.chosen.value("source", std::string()) : std::string();
        ke::ui::tip("%s", (name + (source.empty() ? "" : "\nchosen for: " + source) + "\nclick to choose").c_str());
    }
    const bool plan = tab.chosen.is_object() && tab.chosen.value("plan", false);
    const std::string status = tab.chosen.is_object() ? tab.chosen.value("status", std::string()) : std::string();
    const std::string usage = tab.chosen.is_object() ? tab.chosen.value("usage-url", std::string()) : std::string();
    if (plan) {
        ImGui::SameLine();
        if (status == "limit") {
            text(Ink::inspected, "Usage limit reached");
        } else if (!tab.chosen.value("signed-in", true)) {
            text(Ink::inspected, "Sign in to ChatGPT again");
        } else {
            text(Ink::secondary, "Using ChatGPT plan");
        }
        ImGui::SameLine();
        if (bracketButtonNamed("manage usage", "manage-usage") && !usage.empty()) {
            platform::openUrl(usage);
        }
    }
    if (const std::string note = stringOf(fieldOf(info, "loop/model-note")); !note.empty()) {
        ImGui::SameLine();
        text(Ink::inspected, note);
    }
    if (!tab.pickerResult.empty()) {
        ImGui::SameLine();
        text(Ink::inspected, tab.pickerResult);
    }
    ChatGptSignIn& signin = chatGptSignIn();
    if (!signin.status().empty()) {
        ImGui::SameLine();
        text(signin.failed() ? Ink::inspected : Ink::secondary, signin.status());
    }

    // Ctrl-M from anywhere in a chat. Command-M is the Mac's own minimize, so the physical Control key
    // counts too, which ImGui reports as Super on macOS.
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        ImGui::OpenPopup("terminal-model");
    }
    // above the control: the line it sits on is what a person is about to use
    ImGui::SetNextWindowPos(ImVec2(controlAt.x, controlAt.y - ImGui::GetStyle().ItemSpacing.y), ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
    if (!ImGui::BeginPopup("terminal-model")) {
        return;
    }
    const auto choose = [&](json args) {
        args["principal"] = tab.principal;
        std::string* out = &tab.pickerResult;
        int* asked = &tab.chosenAsked;
        out->clear();
        chat.actNamed("model.choose", std::move(args), [out, asked](const Response& r) {
            if (!r.ok) {
                *out = r.err ? r.err->message : std::string("refused");
            }
            ++*asked;
        });
    };
    // One click switches. What is not choosing a model lives one level in, under SETTINGS.
    std::vector<std::pair<const model::PrincipalInfo*, std::pair<std::string, std::string>>> rows;
    for (const model::PrincipalInfo* e : models) {
        for (const auto& offer : offersOf(e)) {
            rows.push_back({e, offer});
        }
    }
    const auto chooseRow = [&](std::size_t i) {
        json args{{"model", rows[i].first->id}};
        if (!rows[i].second.first.empty()) {
            args["id"] = rows[i].second.first;
        }
        if (tab.pickerConversation && !tab.lastThread.empty()) {
            args["thread"] = tab.lastThread;
        }
        choose(std::move(args));
    };
    if (ImGui::IsWindowAppearing()) {
        tab.pickerCursor = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (tab.chosen.is_object() && tab.chosen.value("model", std::string()) == rows[i].first->id && id == rows[i].second.first) {
                tab.pickerCursor = static_cast<int>(i);
            }
        }
    }
    if (!rows.empty()) {
        const int n = static_cast<int>(rows.size());
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            tab.pickerCursor = (tab.pickerCursor + 1) % n;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            tab.pickerCursor = (tab.pickerCursor + n - 1) % n;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            chooseRow(static_cast<std::size_t>(std::clamp(tab.pickerCursor, 0, n - 1)));
            ImGui::CloseCurrentPopup();
        }
    }
    if (rows.empty()) {
        text(Ink::secondary, "no Model objects in the fold yet");
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const model::PrincipalInfo* e = rows[i].first;
        const std::string& slug = rows[i].second.first;
        const std::string& title = rows[i].second.second;
        const std::string ename = principals.nameOf(e->id);
        const std::string st = [&] {
            const std::string s = stringOf(fieldOf(e, "model/status"));
            return s.empty() ? std::string("unknown") : s;
        }();
        const bool current = tab.chosen.is_object() && tab.chosen.value("model", std::string()) == e->id && id == slug;
        const std::string word = title.substr(title.rfind('/') == std::string::npos ? 0 : title.rfind('/') + 1);
        ImGui::PushID(static_cast<int>(i));
        const std::string row = std::string(current ? "\xe2\x80\xa2 " : "  ") + word + "##row";
        if (ImGui::Selectable(row.c_str(), static_cast<int>(i) == tab.pickerCursor, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(ImGui::CalcTextSize("0").x * 40.0f, 0.0f))) {
            chooseRow(i);
            ImGui::CloseCurrentPopup();
        }
        drive::hit("menu:model:" + ename + ":" + slug, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            tab.pickerCursor = static_cast<int>(i);
            ke::ui::tip("%s\n%s\ncontext %s // price %s", ename.c_str(), title.c_str(), contextOf(e).c_str(), priceOf(e).c_str());
        }
        ImGui::SameLine();
        text(st == "ok" ? Ink::secondary : Ink::inspected, st);
        ImGui::PopID();
    }
    ImGui::Separator();
    const bool settings = beginMenu("SETTINGS");
    drive::hit("menu:model-settings", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!settings) {
        ImGui::EndPopup();
        return;
    }
    text(Ink::strong, "model for " + principals.nameOf(tab.principal));
    if (bracketButtonNamed("this agent", "model-scope-agent", true, !tab.pickerConversation)) {
        tab.pickerConversation = false;
    }
    ImGui::SameLine();
    if (bracketButtonNamed("this conversation", "model-scope-conversation", !tab.lastThread.empty(), tab.pickerConversation)) {
        tab.pickerConversation = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ke::ui::tip("%s", tab.lastThread.empty() ? "no conversation yet" : "only this conversation; the agent's choice stays");
    }
    ImGui::Separator();
    if (models.empty()) {
        text(Ink::secondary, "no Model objects in the fold yet");
    }
    if (!models.empty() && ImGui::BeginTable("models", 7, ImGuiTableFlags_SizingFixedFit)) {
        for (const char* h : {"model", "offers", "reasoning", "context", "price", "status", ""}) {
            ImGui::TableSetupColumn(h);
        }
        ImGui::TableHeadersRow();
        for (const model::PrincipalInfo* e : models) {
            const std::string ename = principals.nameOf(e->id);
            const std::string shown = [&] {
                const std::string l = stringOf(fieldOf(e, "model/label"));
                return l.empty() ? ename : l;
            }();
            const std::string st = [&] {
                const std::string s = stringOf(fieldOf(e, "model/status"));
                return s.empty() ? std::string("unknown") : s;
            }();
            for (const auto& [slug, title] : offersOf(e)) {
                ImGui::PushID((e->id + slug).c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool current = tab.chosen.is_object() && tab.chosen.value("model", std::string()) == e->id && id == slug;
                if (ImGui::Selectable(shown.c_str(), current, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    json args{{"model", e->id}};
                    if (!slug.empty()) {
                        args["id"] = slug;
                    }
                    if (tab.pickerConversation && !tab.lastThread.empty()) {
                        args["thread"] = tab.lastThread;
                    }
                    choose(std::move(args));
                }
                drive::hit("menu:model:" + ename + ":" + slug, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                ImGui::TableNextColumn();
                text(Ink::secondary, title);
                ImGui::TableNextColumn();
                const std::string thinks = stringOf(fieldOf(e, "model/reasoning"));
                text(Ink::secondary, thinks.empty() ? std::string("auto") : thinks);
                ImGui::TableNextColumn();
                text(Ink::secondary, contextOf(e));
                ImGui::TableNextColumn();
                text(Ink::secondary, priceOf(e));
                ImGui::TableNextColumn();
                chip(upper(st), st == "ok" ? Ink::primary : st == "unknown" ? Ink::secondary : Ink::inspected);
                ImGui::TableNextColumn();
                if (bracketButtonNamed("probe", "probe:" + ename + ":" + slug)) {
                    probeResults_[e->id] = {"probing…", false};
                    const std::string mid = e->id;
                    chat.actNamed("model.probe", json{{"model", mid}}, [this, mid](const Response& r) {
                        probeResults_[mid] = {r.ok ? "done" : "err " + (r.err ? r.err->code : std::string("?")), !r.ok};
                    });
                }
                if (auto it = probeResults_.find(e->id); it != probeResults_.end()) {
                    ImGui::SameLine();
                    text(it->second.second ? Ink::inspected : Ink::secondary, it->second.first);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    const std::string fallbackId = fallback && fallback->is_object() ? fallback->value("model", std::string()) : std::string();
    if (beginMenu(("fallback: " + (fallbackId.empty() ? std::string("none") : principals.nameOf(fallbackId))).c_str())) {
        if (menuItem("NONE", nullptr, fallbackId.empty(), !fallbackId.empty())) {
            choose(json{{"fallback", true}, {"clear", true}});
        }
        drive::hit("menu:fallback:none", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        for (const model::PrincipalInfo* e : models) {
            const std::string ename = principals.nameOf(e->id);
            if (menuItem(ename.c_str(), nullptr, e->id == fallbackId)) {
                choose(json{{"model", e->id}, {"fallback", true}});
            }
            drive::hit("menu:fallback:" + ename, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ImGui::EndMenu();
    }
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("used when the chosen Model is down or at its limit; the chat says which one answered");
    }
    const bool canCreate = actions_ && actions_->get("model.create") != nullptr;
    if (menuItem("NEW MODEL…", nullptr, false, canCreate)) {
        actionRequest_ = "model.create";
        actionArgs_ = json::object();
    }
    drive::hit("menu:model.create", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::Separator();
    // OpenAI's own wording, from its guidelines for this flow
    if (signin.busy()) {
        if (bracketButtonNamed("cancel sign-in", "signin-cancel")) {
            signin.cancel();
        }
    } else {
        const std::string renew = plan && !tab.chosen.value("signed-in", true) ? tab.chosen.value("model", std::string()) : std::string();
        if (bracketButtonNamed("continue with chatgpt", "signin")) {
            signin.start(chat, renew);
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("use your ChatGPT plan here: opens your browser, and no key is needed");
        }
    }
    if (plan && tab.chosen.value("signed-in", false)) {
        ImGui::SameLine();
        if (bracketButtonNamed("sign out", "signout")) {
            std::string* out = &tab.pickerResult;
            int* asked = &tab.chosenAsked;
            chat.actNamed("model.signout", json{{"model", tab.chosen.value("model", std::string())}},
                          [out, asked](const Response& r) {
                              if (r.ok && r.result.is_object()) {
                                  const json& v = r.result.contains("result") ? r.result["result"] : r.result;
                                  if (!v.value("revoked", false)) {
                                      *out = "signed out here; disconnect the app in ChatGPT settings to be sure";
                                  }
                              }
                              ++*asked;
                          });
        }
    }
    ImGui::EndMenu();
    ImGui::EndPopup();
}

} // namespace ke::ui
