#include "ui/UnsafeView.hpp"

#include "drive/Commands.hpp"
#include "ui/Theme.hpp"

#include <algorithm>

namespace ke::ui {

namespace {

std::vector<chat::UnsafeEntry> g_entries;
std::string g_asked; // the Principal the confirm is about
int g_minutes = 30;
bool g_openConfirm = false;
bool g_openBox = false;
std::string g_askedAdapter;
std::string g_result;
std::string g_askedPane;
std::string g_shareAgent;
std::string g_shareMode = "read";
bool g_openShare = false;

std::string labelNow(const model::Principals& principals, double nowEpoch) {
    return chat::unsafeLabel(g_entries, [&principals](const std::string& id) { return principals.nameOf(id); }, nowEpoch);
}

void off(chat::Chat& chat, const std::string& principal) {
    chat.actNamed("approval.unsafe.off", nlohmann::json{{"principal", principal}}, [](const Response& r) {
        g_result = r.ok ? std::string() : "err " + (r.err ? r.err->code + " " + r.err->message : std::string());
    });
}

} // namespace

void setUnsafe(const std::vector<chat::UnsafeEntry>& entries) { g_entries = entries; }

bool isUnsafe(const std::string& principal) {
    return std::any_of(g_entries.begin(), g_entries.end(), [&](const chat::UnsafeEntry& e) { return e.principal == principal; });
}

float unsafeIndicatorWidth(const model::Principals& principals, double nowEpoch) {
    const std::string label = labelNow(principals, nowEpoch);
    return label.empty() ? 0.0f : ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
}

void unsafeIndicator(const model::Principals& principals, chat::Chat& chat, double nowEpoch) {
    const std::string label = labelNow(principals, nowEpoch);
    if (label.empty()) {
        return;
    }
    const ImVec2 size = ImGui::CalcTextSize(label.c_str());
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##unsafe", size)) {
        ImGui::OpenPopup("unsafe-list");
    }
    drive::hit("unsafe", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), Ink::unsafe);
    dl->AddText(at, Surface::background, label.c_str());
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("approvals are off for this Principal; click to list and end it");
    }
    if (ImGui::BeginPopup("unsafe-list")) {
        for (const chat::UnsafeEntry& e : g_entries) {
            ImGui::PushID(e.principal.c_str());
            const std::string who = e.kind == "shared-terminal" ? "terminal shared with " + principals.nameOf(e.agent)
                                                                : principals.nameOf(e.principal);
            const std::string line = who + "  " + std::to_string(chat::minutesLeft(e.until, nowEpoch)) +
                                     "m left, armed by " + principals.nameOf(e.by);
            text(Ink::unsafe, line);
            ImGui::SameLine();
            if (bracketButtonNamed("OFF", "unsafe-off:" + principals.nameOf(e.principal))) {
                off(chat, e.principal);
            }
            ImGui::PopID();
        }
        if (!g_result.empty()) {
            text(Ink::fatal, g_result);
        }
        ImGui::EndPopup();
    }
}

void unsafeToggle(const model::PrincipalInfo& principal) {
    if (principal.drivenBy != "process") {
        return; // a person has no gate
    }
    if (bracketButtonNamed("UNSAFE MODE (disable approvals)", "unsafe:" + principal.id, true, isUnsafe(principal.id), Ink::unsafe)) {
        g_asked = principal.id;
        g_openConfirm = true;
    }
}

void unconfineToggle(const model::PrincipalInfo& adapter) {
    const auto it = adapter.fields.find("adapter/binds");
    if (it == adapter.fields.end() || !it->second.value.is_string() || it->second.value.get<std::string>() != "host") {
        return; // only a shell runs in a box
    }
    if (bracketButtonNamed(isUnsafe(adapter.id) ? "OUT OF THE BOX" : "LEAVE THE BOX", "unconfine:" + adapter.id, true,
                           isUnsafe(adapter.id), Ink::unsafe)) {
        g_askedAdapter = adapter.id;
        g_openBox = true;
    }
    if (ImGui::IsItemHovered()) {
        wrappedTooltip("this Adapter's commands and terminals run in a sandbox; a person can lift it for a while");
    }
}

namespace {

// Lifting the sandbox for a while, named plainly. There is no way to leave it off for good.
void boxDialog(const model::Principals& principals, chat::Chat& chat) {
    if (g_openBox) {
        g_openBox = false;
        g_result.clear();
        ImGui::OpenPopup("LEAVE THE BOX");
    }
    if (!ImGui::BeginPopupModal("LEAVE THE BOX", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const std::string name = principals.nameOf(g_askedAdapter);
    if (isUnsafe(g_askedAdapter)) {
        text(Ink::unsafe, name + " is out of its box.");
        if (bracketButtonNamed("BACK IN THE BOX", "box-end")) {
            off(chat, g_askedAdapter);
            ImGui::CloseCurrentPopup();
        }
    } else {
        text(Ink::unsafe, "Let " + name + " run without its sandbox?");
        ImGui::PushTextWrapPos(ImGui::CalcTextSize("0").x * 64.0f);
        ImGui::TextUnformatted(("Commands and terminals on " + name + " will run as the world's own user. They can read this world's "
                                "tokens, its Model key and its log, read your home directory, write anywhere that user can, and "
                                "reach any network.")
                                   .c_str());
        ImGui::TextUnformatted("Anything an agent runs there can take your session. The red indicator stays on until it ends.");
        ImGui::PopTextWrapPos();
        for (const auto& [minutes, word] : {std::pair<int, const char*>{30, "30 MIN"}, {120, "2 H"}, {480, "8 H"}}) {
            if (bracketButtonNamed(word, "box-" + std::to_string(minutes), true, false, Ink::unsafe)) {
                chat.actNamed("adapter.unconfine", nlohmann::json{{"adapter", g_askedAdapter}, {"minutes", minutes}}, [](const Response& r) {
                    g_result = r.ok ? std::string() : "err " + (r.err ? r.err->code + " " + r.err->message : std::string());
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }
    dialogGap();
    if (bracketButtonNamed("CANCEL", "box-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Handing over a shell that runs as you, named plainly. There is no way to share with everyone.
void shareDialog(const model::Principals& principals, chat::Chat& chat) {
    if (g_openShare) {
        g_openShare = false;
        g_result.clear();
        g_shareAgent.clear();
        g_shareMode = "read";
        ImGui::OpenPopup("SHARE TERMINAL");
    }
    if (!ImGui::BeginPopupModal("SHARE TERMINAL", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::PushTextWrapPos(ImGui::CalcTextSize("0").x * 64.0f);
    text(Ink::unsafe, "Share this terminal with one agent?");
    ImGui::TextUnformatted("It runs as you, outside the box: an unconfined shell. READ lets the agent see the screen. "
                           "TYPE also lets it ask to type, with a card each time, unless a rule of yours admits it.");
    ImGui::PopTextWrapPos();
    bool first = true;
    for (const model::PrincipalInfo* a : principals.ordered()) {
        if (a->cls != "principal" || a->drivenBy != "process") {
            continue;
        }
        if (!first) {
            ImGui::SameLine();
        }
        first = false;
        if (bracketButtonNamed(principals.nameOf(a->id).c_str(), "share-agent:" + principals.nameOf(a->id), true, g_shareAgent == a->id)) {
            g_shareAgent = a->id;
        }
    }
    for (const char* mode : {"read", "type"}) {
        if (bracketButtonNamed(mode, std::string("share-mode:") + mode, true, g_shareMode == mode)) {
            g_shareMode = mode;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    for (const auto& [minutes, word] : {std::pair<int, const char*>{30, "30 MIN"}, {120, "2 H"}, {480, "8 H"}}) {
        if (bracketButtonNamed(word, "share-" + std::to_string(minutes), !g_shareAgent.empty(), false, Ink::unsafe)) {
            chat.actNamed("pane.share",
                          nlohmann::json{{"pane", g_askedPane}, {"principal", g_shareAgent}, {"mode", g_shareMode}, {"minutes", minutes}},
                          [](const Response& r) {
                              g_result = r.ok ? std::string() : "err " + (r.err ? r.err->code + " " + r.err->message : std::string());
                          });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (bracketButtonNamed("CANCEL", "share-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

void askShareTerminal(const std::string& pane) {
    g_askedPane = pane;
    g_openShare = true;
}

void unsafeDialog(const model::Principals& principals, chat::Chat& chat) {
    boxDialog(principals, chat);
    shareDialog(principals, chat);
    if (g_openConfirm) {
        g_openConfirm = false;
        g_result.clear();
        ImGui::OpenPopup("UNSAFE MODE");
    }
    if (!ImGui::BeginPopupModal("UNSAFE MODE", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const std::string name = principals.nameOf(g_asked);
    if (isUnsafe(g_asked)) {
        text(Ink::unsafe, name + " is in unsafe mode.");
        if (bracketButtonNamed("END IT", "unsafe-end")) {
            off(chat, g_asked);
            ImGui::CloseCurrentPopup();
        }
    } else {
        text(Ink::unsafe, "Disable approvals for " + name + "?");
        ImGui::PushTextWrapPos(ImGui::CalcTextSize("0").x * 64.0f);
        ImGui::TextUnformatted(("Every gated op " + name + " makes runs without a card: shell commands, file writes, terminal input. "
                                "Each still records a Task, and they are listed in APPROVALS.")
                                   .c_str());
        ImGui::TextUnformatted("Still raises a card: Grants, retiring an object, creating a Principal, tokens, and policy.");
        ImGui::PopTextWrapPos();
        for (const auto& [minutes, word] : {std::pair<int, const char*>{30, "30 MIN"}, {120, "2 H"}, {480, "8 H"}}) {
            if (bracketButtonNamed(word, "unsafe-minutes:" + std::to_string(minutes), true, g_minutes == minutes)) {
                g_minutes = minutes;
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
        dialogGap();
        if (bracketButtonNamed("DISABLE APPROVALS", "unsafe-arm", true, false, Ink::unsafe)) {
            chat.actNamed("approval.unsafe", nlohmann::json{{"principal", g_asked}, {"minutes", g_minutes}}, [](const Response& r) {
                g_result = r.ok ? std::string() : "err " + (r.err ? r.err->code + " " + r.err->message : std::string());
            });
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (bracketButtonNamed("CANCEL", "unsafe-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void unsafeMark(ImDrawList* dl, ImVec2 at, float size) { dl->AddRectFilled(at, ImVec2(at.x + size, at.y + size), Ink::unsafe); }

} // namespace ke::ui
