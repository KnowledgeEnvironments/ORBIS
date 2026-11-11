#include "app/Layout.hpp"

#include "model/Activity.hpp"
#include "model/Modeline.hpp"
#include "ui/MathView.hpp"
#include "platform/Keyboard.hpp"
#include "platform/Platform.hpp"
#include "app/Menus.hpp"
#include "drive/Commands.hpp"
#include "ui/ChatGptSignIn.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/TextMenu.hpp"
#include "ui/Textures.hpp"
#include "ui/UnsafeView.hpp"

#include "chat/Search.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "app/Runtime.hpp"
#include "ui/Theme.hpp"
#include "ui/Themes.hpp"
#include "util/Timing.hpp"
#include "util/Product.hpp"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <fstream>


namespace {

// Search wiring (sprint 1 MVP item): the HISTORY pane asks, the app issues `search {text limit 50}`
// to the world and, when the world has no such op (`not-implemented`, an unknown op, or no
// attach), searches what the listener already holds and says `local`. Set every frame so the
// callbacks always reach the current Runtime (it is rebuilt on a new attach).
void wireSearch(ke::app::FrameInputs& in) {
    ke::app::Panes& p = in.panes;
    ke::Runtime& rt = in.rt;
    ke::ui::HistoryPane* hp = &p.history;
    hp->onOpenObject = [&p, &rt](const std::string& id) { p.open(rt, id, 2); };
    hp->onOpenMessage = [&rt](const std::string& id, const std::string& actor) {
        // Its chat is the agent's: who sent it, or who a person sent it to.
        const auto agent = [&rt](const std::string& who) {
            const ke::model::PrincipalInfo* info = rt.principals.get(who);
            return info != nullptr && info->drivenBy == "process";
        };
        std::string chatOf;
        if (const ke::chat::MessageRow* row = rt.chat.messages().get(id)) {
            if (agent(row->from)) {
                chatOf = row->from;
            }
            for (const std::string& to : row->to) {
                if (chatOf.empty() && agent(to)) {
                    chatOf = to;
                }
            }
        } else if (agent(actor)) {
            chatOf = actor;
        }
        if (!chatOf.empty()) {
            ke::ui::LinkRequest r;
            r.kind = ke::ui::LinkRequest::Message;
            r.object = chatOf;
            r.message = id;
            ke::ui::symbolUi().ask(std::move(r));
        }
    };
    ke::Prefs* prefs = &in.prefs;
    std::function<void()> save = in.saveWhimsy;
    hp->onNeedOlder = [&rt] { rt.fetchOlderHistory(); };
    hp->onSearch = [hp, &rt, prefs, save](const std::string& text) {
        if (prefs->searchLast != text) {
            prefs->searchLast = text; // remembered for the next launch (prefs.json view.search-last)
            if (save) {
                save();
            }
        }
        auto local = [hp, &rt, text](const std::string& why) {
            std::vector<ke::chat::SearchHit> hits = ke::chat::localSearch(rt.principals, rt.chat.messages(), text, 50);
            const std::size_t n = hits.size();
            hp->setHits(std::move(hits), true, std::to_string(n) + " hit(s), " + why);
        };
        if (!rt.session.attached()) {
            local("not attached");
            return;
        }
        rt.session.request(
            "search", ke::json{{"text", text}, {"limit", 50}},
            [hp, &rt, text, local](const ke::Response& r) {
                if (r.ok) {
                    std::vector<ke::chat::SearchHit> hits = ke::chat::parseWorldHits(r.result, text);
                    const std::size_t n = hits.size();
                    const long long total = r.result.is_object() && r.result.contains("total") && r.result["total"].is_number_integer()
                                                ? r.result["total"].get<long long>()
                                                : static_cast<long long>(n);
                    hp->setHits(std::move(hits), false,
                                std::to_string(n) + " hit(s)" + (total > static_cast<long long>(n) ? " of " + std::to_string(total) : "") +
                                    ", from the world");
                    return;
                }
                const std::string code = r.err ? r.err->code : std::string("?");
                if (code == "not-implemented" || code == "unknown-op" || code == "unknown") {
                    local("the world has no search op");
                } else {
                    hp->setHits({}, false, "err " + code + (r.err && !r.err->message.empty() ? " " + r.err->message : ""));
                }
            },
            true);
    };
}

} // namespace

namespace ke::app {

#if !LISTENER_PHONE
void bringDockWindowFront(const char* name);
#endif

std::filesystem::path sessionFile(const Options& opts) {
    if (opts.dataDir.empty()) {
        return FileSessionStore::defaultPath();
    }
    return std::filesystem::path(opts.dataDir) / "session.json";
}

std::filesystem::path prefsFile(const Options& opts) {
    return sessionFile(opts).parent_path() / "prefs.json";
}

void Panes::noteUnread(Runtime& rt, const std::string& reading) {
    rt.unread.update(rt.principals.messages(), rt.session.principal().value_or(""), rt.boot.headAtAttach, reading);
    unread = rt.unread.counts();
    for (const chat::Notification& n : rt.notifications.rows()) {
        if (!n.read) {
            ++unread[n.from];
        }
    }
}

void Panes::noteActivity(Runtime& rt, const std::string& reading, bool looking, SDL_Window* window, bool tellOs) {
    const std::string self = rt.session.principal().value_or("");
    std::map<std::string, Seq> seen = model::seenOf(rt.principals.get(self));
    for (const auto& [id, seq] : seenSent) {
        seen[id] = std::max(seen[id], seq);
    }
    std::map<std::string, int> loud;
    for (const chat::Notification& n : rt.notifications.rows()) {
        if (!n.read && chat::loud(n.level)) {
            ++loud[n.from];
        }
    }
    std::string key = std::to_string(rt.principals.applied()) + "/" + std::to_string(rt.tasks.applied()) + "/" + self;
    for (const auto& [id, n] : loud) {
        key += "/" + id + ":" + std::to_string(n);
    }
    for (const auto& [id, seq] : seen) {
        key += "/" + id + "@" + std::to_string(seq);
    }
    if (key != activityKey) {
        activityKey = key;
        activity = model::activity(rt.principals, rt.tasks, self, loud, seen);
    }
    // Opening the chat is the look: the world keeps it, so every client of this person clears too.
    if (looking && !reading.empty()) {
        if (auto it = activity.latest.find(reading); it != activity.latest.end() && it->second > seen[reading]) {
            seen[reading] = it->second;
            seenSent[reading] = it->second;
            json value = json::object();
            for (const auto& [id, seq] : seen) {
                value[id] = seq;
            }
            rt.chat.setField(self, "view/seen", "map", value, "both", [](const Response&) {});
        }
    }
    const bool front = window != nullptr && platform::windowFocused(window);
    for (auto it = needsTold.begin(); it != needsTold.end();) {
        auto mark = activity.marks.find(*it);
        it = mark == activity.marks.end() || mark->second != model::Activity::NeedsYou ? needsTold.erase(it) : std::next(it);
    }
    for (const auto& [id, mark] : activity.marks) {
        if (mark == model::Activity::NeedsYou && needsTold.insert(id).second) {
            if (window != nullptr && tellOs && SDL_getenv("ORBIS_TEST") == nullptr && !front) {
                platform::notifySystem("ORBIS: " + rt.principals.nameOf(id), "needs you");
            }
        }
    }
    const int want = window == nullptr || front ? 0 : activity.needYou;
    if (want != badge) {
        badge = want;
        platform::setBadge(want);
    }
}

void Panes::noteLoud(Runtime& rt, SDL_Window* window, bool enabled) {
    ui::chatGptSignIn().poll(rt.chat);
    ui::chatGptSignIn().drawWelcome();
    for (const chat::Notification& n : rt.notifications.takeLoud()) {
        ++osNotices;
        // Under the drive API the count is the test; a run must not fill a person's screen.
        if (enabled && SDL_getenv("ORBIS_TEST") == nullptr && !platform::windowFocused(window)) {
            platform::notifySystem("ORBIS: " + rt.principals.nameOf(n.from), n.text);
        }
    }
}

ui::GraphInput Panes::graphInput(Runtime& rt, const ui::Anim& anim, ImFont* mono) {
    const double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    return ui::GraphInput{rt.principals, rt.evaluations, rt.tasks, rt.panes, rt.unsafe, unread, activity.marks, rt.chat, rt.session, anim, mono, wall};
}

void Panes::inspect(const std::string& id, int view) {
#if LISTENER_PHONE
    terminal.open(id, view);
#else
    inspector.open(id, view);
    if (!mux.viewOpen("view:inspect")) {
        mux.showView("view:inspect", "view:chat", model::Edge::Right);
    } else {
        mux.showView("view:inspect");
    }
#endif
}

bool Panes::open(Runtime& rt, const std::string& id, int view) {
    const model::PrincipalInfo* who = rt.principals.get(id);
    const bool chat = !(view == 2 || (who != nullptr && who->cls != "principal"));
    if (chat) {
        terminal.open(id, view);
    } else {
        inspect(id, view);
    }
    graph.select(id);
    history.setFocus(id, rt.session.principal().value_or(""));
    return chat;
}

LinkTarget Panes::follow(Runtime& rt, ImFont* mono) {
    symbols();
    removal.draw(rt.principals, rt.chat);
    approvals.draw(rt.tasks, rt.principals, rt.chat, mono);
    notices.draw(rt.notifications, rt.principals, rt.chat, mono);
    manual.draw(rt.chat, rt.principals, rt.session.principal().value_or(""), mono);
    scratch.draw(rt.principals, rt.chat, mono, terminal.showThinking);
    if (ImGui::BeginPopupModal("RENAME LABEL", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ui::dimText("bound name %s: agents resolve that, whatever this says", rt.principals.boundName(labelObject).c_str());
        ui::dialogGap();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 40.0f);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool go = ImGui::InputText("##label", &labelText, ImGuiInputTextFlags_EnterReturnsTrue);
        drive::hit("input:label", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const model::PaneInfo* pane = rt.panes.get(labelObject);
        const model::FieldState* had = rt.principals.fieldOf(labelObject, "object/label");
        const bool hadLabel = had != nullptr || (pane != nullptr && !pane->label.empty());
        const auto clearLabel = [&] {
            rt.chat.actNamed("field.clear", json{{"object", labelObject}, {"field", "object/label"}}, [](const Response&) {});
            labelClearing = false;
            ImGui::CloseCurrentPopup();
        };
        if (!labelText.empty()) {
            labelClearing = false;
        }
        if (labelClearing) {
            ui::text(ui::Ink::inspected, "Clear the label of " + rt.principals.boundName(labelObject) + "? The log keeps its history.");
            ui::dialogGap();
            const ui::Confirmed c = ui::confirmRow("clear", "confirm-clear-label", "cancel-clear-label", labelConfirmFocus);
            if (c == ui::Confirmed::Yes) {
                clearLabel();
            } else if (c == ui::Confirmed::No) {
                labelClearing = false;
            }
        } else {
            ui::dialogGap();
            if (ui::bracketButtonNamed("SET", "label-set") || go) {
                if (!labelText.empty()) {
                    rt.chat.setField(labelObject, "object/label", "string", labelText, "both", [](const Response&) {});
                    ImGui::CloseCurrentPopup();
                } else if (!hadLabel) {
                    ImGui::CloseCurrentPopup();
                } else if (ImGui::GetIO().KeyShift) {
                    clearLabel();
                } else {
                    labelClearing = true;
                    labelConfirmFocus = true;
                }
            }
            if (ImGui::IsItemHovered() && labelText.empty() && hadLabel) {
                ke::ui::tip("clears the label: asks first; shift-click clears at once");
            }
            ImGui::SameLine();
            if (ui::bracketButtonNamed("CANCEL", "label-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    auto req = ui::symbolUi().take();
    if (!req) {
        return LinkTarget::None;
    }
    switch (req->kind) {
    case ui::LinkRequest::Terminal:
        open(rt, req->object, 0);
        return LinkTarget::Terminal;
    case ui::LinkRequest::Message:
        open(rt, req->object, 0);
        terminal.jumpTo(req->object, req->message);
        return LinkTarget::Terminal;
    case ui::LinkRequest::Fields:
        open(rt, req->object, 2);
        return LinkTarget::None;
    case ui::LinkRequest::Field:
#if LISTENER_PHONE
        terminal.scrollToField(req->object, req->field);
        graph.select(req->object);
        return LinkTarget::Terminal;
#else
        inspector.scrollToField(req->object, req->field);
        mux.showView("view:inspect", "view:chat", model::Edge::Right);
        graph.select(req->object);
        return LinkTarget::None;
#endif
    case ui::LinkRequest::Inspect:
        if (req->byName) {
#if LISTENER_PHONE
            terminal.openNamed(req->object);
#else
            inspector.openNamed(req->object);
            mux.showView("view:inspect", "view:chat", model::Edge::Right);
#endif
        } else {
            inspect(req->object, 2);
        }
        return LinkTarget::None;
    case ui::LinkRequest::History:
        history.scrollToSeq(req->seq);
        history.collapsed = false;
        return LinkTarget::History;
    case ui::LinkRequest::Graph:
        graph.select(req->object);
        return LinkTarget::Graph;
    case ui::LinkRequest::Action:
        openAction(rt, req->action, req->args);
        return LinkTarget::None;
    case ui::LinkRequest::Delete:
        removal.open(req->object);
        return LinkTarget::None;
    case ui::LinkRequest::Scratch:
        scratch.ask(req->object);
        return LinkTarget::None;
    case ui::LinkRequest::Label:
        labelObject = req->object;
        if (const model::PaneInfo* pane = rt.panes.get(req->object)) {
            labelText = pane->label;
            ImGui::OpenPopup("RENAME LABEL");
            return LinkTarget::None;
        }
        labelText = rt.principals.fieldOf(req->object, "object/label") != nullptr ? rt.principals.nameOf(req->object) : std::string();
        ImGui::OpenPopup("RENAME LABEL");
        return LinkTarget::None;
    case ui::LinkRequest::OpenPane:
        mux.attachPane(req->object, req->openKind);
        return LinkTarget::None;
    case ui::LinkRequest::OpenFile:
        {
            modules::FileRef file{req->object, req->path, req->openKind, req->newPane};
            file.message = req->message;
            file.index = req->index;
            file.tex = req->tex;
            file.line = req->line;
            mux.open(file);
        }
        return LinkTarget::None;
    case ui::LinkRequest::Doc:
        manual.show(req->field);
        return LinkTarget::None;
    case ui::LinkRequest::EditField:
        {
            modules::FileRef ref{req->object, "", "editor", false};
            ref.field = req->field;
            mux.open(ref);
        }
        return LinkTarget::None;
    case ui::LinkRequest::Restore:
        rt.chat.actNamed("object.restore", json{{"object", req->object}}, [](const Response&) {});
        return LinkTarget::None;
    }
    return LinkTarget::None;
}

void Panes::drawModeline(FrameInputs& in) {
    Runtime& rt = in.rt;
    // Rebuilt when the log moves or the connection changes; a frame only draws it.
    const auto version = std::make_pair(rt.session.head(), rt.events.rows().size());
    if (version != modelineVersion) {
        modelineVersion = version;
        modelineText = model::modeline(rt.events.rows(), rt.principals);
    }
    // A refusal has to reach the person wherever they are: the pane that holds it may be out of view.
#if LISTENER_PHONE
    const ImGuiWindow* panes = ImGui::FindWindowByName("PANES");
    const bool panesInView = panes != nullptr && panes->Active && !panes->Hidden;
#else
    const bool panesInView = mux.panesInView();
#endif
    const std::string held = mux.heldError(panesInView);
    if (held != modelineError) {
        modelineError = held;
        modelineErrorSince = ImGui::GetTime();
    }
    constexpr double kErrorShown = 30.0; // then the line goes back to status; the pane still shows it
    const bool showError = !modelineError.empty() && ImGui::GetTime() - modelineErrorSince < kErrorShown;
    ImGui::PushFont(in.ui, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;
    const float height = ImGui::GetTextLineHeight() + 2.0f * ImGui::GetStyle().FramePadding.y;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x, ImGui::GetStyle().FramePadding.y));
    if (ImGui::BeginViewportSideBar("##modeline", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags)) {
        // What wants the person first, and a click goes to it.
        if (const std::string wants = model::rollup(activity); !wants.empty()) {
            ui::text(activity.needYou > 0 ? ui::Ink::inspected : activity.failed > 0 ? ui::Ink::fatal : ui::Ink::nav, wants);
            drive::hit("modeline:next", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ke::ui::tip("click: go to the next one");
            }
            if (ImGui::IsItemClicked() && !activity.next.empty()) {
                open(rt, activity.next.front());
#if !LISTENER_PHONE
                bringDockWindowFront("CHAT");
#endif
            }
            ImGui::SameLine();
        }
        if (showError) {
            ui::text(ui::Ink::fatal, modelineError);
            ImGui::SameLine();
        }
        const std::string hint = ui::takeDetailHint();
#if LISTENER_PHONE
        // A phone has no menu bar: the notices chip and the attach state sit at the right of this
        // line, and the status text is clipped before them rather than run under them.
        const ConnState st = rt.client.state();
        const std::string stateWord = ui::upper(name(st));
        const std::string head = std::to_string(rt.session.head());
        const float chipWidth = ImGui::CalcTextSize("NOTICES 00").x + 2.0f * ImGui::GetStyle().FramePadding.x;
        const float right = chipWidth + ImGui::GetStyle().ItemSpacing.x * 2.0f + ImGui::CalcTextSize((stateWord + " " + head).c_str()).x;
        const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - right;
        const ImVec2 textMin = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(textMin, ImVec2(textMin.x + std::max(0.0f, rightX - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x), textMin.y + ImGui::GetTextLineHeight() * 2.0f), true);
        ui::text(hint.empty() ? ui::Ink::secondary : ui::Ink::nav, hint.empty() ? modelineText : hint);
        ImGui::PopClipRect();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), rightX));
        if (ui::noticesChip(rt.notifications, notices.open())) {
            notices.toggle();
        }
        ImGui::SameLine();
        ui::text(ui::stateColor(st == ConnState::Attached, st == ConnState::Connecting || st == ConnState::Attaching), stateWord);
        ImGui::SameLine();
        ui::text(ui::Ink::strong, head);
#else
        ui::text(hint.empty() ? ui::Ink::secondary : ui::Ink::nav, hint.empty() ? modelineText : hint);
#endif
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

void Panes::symbols() {
    ui::symbolUi().drawMenu();
    ui::textMenu();
}

void Panes::openAction(Runtime& rt, const std::string& action, json defaults) {
    if (action == "principal.create") {
        dialog.openCreatePrincipal(rt.session.principal().value_or(""));
        if (defaults.is_object() && !defaults.empty()) {
            dialog.openAction(action, defaults); // prefilled, single step
        }
        return;
    }
    if (action == "workspace.mount" && defaults.is_object() && defaults.contains("principal") && defaults["principal"].is_string()) {
        const std::string p = defaults["principal"].get<std::string>();
        dialog.openMount(p, defaults.contains("name") && defaults["name"].is_string() ? defaults["name"].get<std::string>()
                                                                                         : rt.principals.nameOf(p));
        return;
    }
    if (action == "route.create" && defaults.is_object() && defaults.contains("members") && defaults["members"].is_array() &&
        defaults["members"].size() == 1 && rt.session.principal() && defaults["members"][0] != *rt.session.principal()) {
        defaults["members"].push_back(*rt.session.principal());
        defaults["name"] = rt.principals.nameOf(*rt.session.principal()) + "." +
                           rt.principals.nameOf(defaults["members"][0].get<std::string>());
    }
    dialog.openAction(action, std::move(defaults));
}

// ---- WorldPicker --------------------------------------------------------------------------------

namespace {

std::string readTokenFile(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    if (in) {
        std::getline(in, s);
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

std::string whenShort(const std::string& iso) {
    // 2026-10-30T21:00:00Z -> 10-30 21:00
    if (iso.size() < 16) {
        return iso.empty() ? "never" : iso;
    }
    return iso.substr(5, 5) + " " + iso.substr(11, 5);
}

} // namespace

void WorldPicker::drawBody(Runtime& rt, const Options& opts) {
    const WorldBook book(sessionFile(opts));
    if (!rememberedCurrent && rt.session.attached() && !opts.token.empty()) {
        // the running world's token is what the picker will need to come back to it
        if (!book.tokenFor(opts.url)) {
            book.remember(opts.url, opts.token);
        }
        rememberedCurrent = true;
    }
    const std::string currentKey = FileSessionStore::worldKey(opts.url);
    ui::dimText("WORLDS");
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("worlds", 5, flags)) {
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("url");
        ImGui::TableSetupColumn("principal");
        ImGui::TableSetupColumn("attached");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const KnownWorld& w : book.list(opts.url)) {
            const bool current = w.url == currentKey;
            ImGui::PushID(w.url.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string name = w.name;
            if (current && !rt.session.world().name.empty()) {
                name = rt.session.world().name;
            }
            ui::textures::assetIcon("server-96.png");
            ui::text(current ? ui::Ink::strong : ui::Ink::primary, name.empty() ? "(unnamed)" : name);
            if (current) {
                ImGui::SameLine();
                ui::chip("CURRENT", ui::Ink::nav);
            }
            if (!w.id.empty() && ImGui::IsItemHovered()) {
                ke::ui::tip("world %s", w.id.c_str());
            }
            ImGui::TableNextColumn();
            ui::text(ui::Ink::secondary, w.url);
            ImGui::TableNextColumn();
            ui::text(ui::Ink::secondary, w.principal.empty() ? "-" : rt.principals.nameOf(w.principal));
            ImGui::TableNextColumn();
            ui::text(ui::Ink::secondary, whenShort(w.lastAttached));
            ImGui::TableNextColumn();
            const bool canAttach = !current && (w.hasToken);
            if (ui::bracketButtonNamed("attach", "world-attach:" + w.url, canAttach)) {
                if (auto tok = book.tokenFor(w.url)) {
                    request = std::make_pair(w.url, *tok);
                    status = "attaching to " + (w.name.empty() ? w.url : w.name);
                    statusErr = false;
                }
            }
            if (ImGui::IsItemHovered()) {
                ke::ui::tip(current ? "this is the world you are attached to"
                                  : canAttach ? "attach with the token saved in worlds.json; the folds reboot from that world's snapshot"
                                              : "no token saved for this world: ADD WORLD with its token");
            }
            if (w.hasToken && !current) {
                ImGui::SameLine();
                if (ui::bracketButtonNamed("forget", "world-forget:" + w.url)) {
                    book.forget(w.url);
                }
                if (ImGui::IsItemHovered()) {
                    ke::ui::tip("drop the saved token (the session entry stays)");
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ui::bracketButtonNamed(adding ? "add world x" : "add world\xe2\x80\xa6", "world-add", true, adding)) {
        adding = !adding;
        status.clear();
    }
    if (adding) {
        ImGui::Indent();
        ui::dimText("URL");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 40);
        ImGui::InputText("##add-url", &addUrl);
        ui::dimText("TOKEN");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 40);
        ImGui::InputText("##add-token", &addToken, ImGuiInputTextFlags_Password);
        ui::dimText("  or token file");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 28);
        ImGui::InputText("##add-token-file", &addTokenFile);
        const bool urlOk = addUrl.rfind("ws://", 0) == 0 || addUrl.rfind("wss://", 0) == 0;
        if (ui::bracketButtonNamed("hello", "world-hello", urlOk && !hello.running())) {
            helloUrl = addUrl;
            helloLine.clear();
            hello.start(addUrl, "hello", json::object(), 6000);
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("ask the world who it is before attaching (no token needed)");
        }
        if (hello.running()) {
            ImGui::SameLine();
            ui::dimText("asking %s\xe2\x80\xa6", helloUrl.c_str());
        } else if (hello.done() && helloLine.empty()) {
            if (hello.ok()) {
                const json r = hello.result();
                const std::string id = r.contains("world") && r["world"].is_string() ? r["world"].get<std::string>()
                                       : r.contains("id") && r["id"].is_string()     ? r["id"].get<std::string>()
                                                                                       : std::string();
                helloLine = (r.value("name", std::string("(unnamed)"))) + "  world " + id.substr(0, 8) + "  head " +
                            std::to_string(r.contains("head") && r["head"].is_number_integer() ? r["head"].get<long long>() : 0LL);
            } else {
                helloLine = "no hello: " + hello.error();
            }
        }
        if (!helloLine.empty()) {
            ImGui::SameLine();
            ui::text(hello.ok() ? ui::Ink::nav : ui::Ink::inspected, helloLine);
        }
        std::string token = addToken;
        if (token.empty() && !addTokenFile.empty()) {
            token = readTokenFile(addTokenFile);
        }
        if (ui::bracketButtonNamed("attach", "world-add-attach", urlOk && !token.empty())) {
            if (book.remember(addUrl, token)) {
                request = std::make_pair(addUrl, token);
                status = "attaching to " + addUrl;
                statusErr = false;
                adding = false;
                addToken.clear();
            } else {
                status = "could not save worlds.json";
                statusErr = true;
            }
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("save the token for this url (worlds.json, 0600) and attach");
        }
        ImGui::Unindent();
    }
    if (!status.empty()) {
        ui::text(statusErr ? ui::Ink::inspected : ui::Ink::connected, status);
    }
    ui::dimText("tokens: %s", book.tokensFile().string().c_str());
}

std::optional<std::pair<std::string, std::string>> WorldPicker::takeAttach() {
    std::optional<std::pair<std::string, std::string>> out;
    out.swap(request);
    return out;
}

#if !LISTENER_PHONE

// Make `name` the window a NoTabBar dock node shows (ImGui shows Windows[0] of such a node) and
// give it focus. A no-op when the window is not docked or already in front.
static modules::Multiplexer* g_layout = nullptr;

// Focus follows the pointer between windows. Not while a button is held, so a drag or a selection
// keeps its window, and not over a menu or a dialog, which has the keys for a reason.
static void focusUnderPointer() {
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.MouseDelta.x == 0.0f && io.MouseDelta.y == 0.0f) || ImGui::IsAnyMouseDown() ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        return;
    }
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* over = g.HoveredWindow;
    if (over == nullptr || (g.NavWindow != nullptr && g.NavWindow->RootWindow == over->RootWindow)) {
        return;
    }
    ImGui::FocusWindow(over);
}

void bringDockWindowFront(const char* name) {
    if (g_layout == nullptr) {
        return;
    }
    std::string view(name);
    std::transform(view.begin(), view.end(), view.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (view == "principals") {
        g_layout->showViewAtEdge("view:principals", model::Edge::Left, 0.3f);
    } else {
        g_layout->showView("view:" + view);
    }
    ImGui::SetWindowFocus(name);
}

// What a person sees before they arrange anything: Principals down the left, Chat, Graph and Panes
// as tabs over History. A narrow window leaves the left column out.
static model::PaneLayout defaultLayout(bool narrow, float leftRatio) {
    const nlohmann::json right = {{"split", "v"},
                                  {"ratio", 0.56f},
                                  {"a", {{"tabs", {"view:chat", "view:graph", "view:panes"}}, {"active", 1}}},
                                  {"b", {{"tabs", {"view:history"}}, {"active", 0}}}};
    if (narrow) {
        return model::PaneLayout::fromJson(right);
    }
    return model::PaneLayout::fromJson({{"split", "h"},
                                        {"ratio", leftRatio},
                                        {"a", {{"tabs", {"view:principals"}}, {"active", 0}}},
                                        {"b", right}});
}

// The left column's share for a window width, in px of the work area: 30 %, never under 28
// columns of the mono font, never over half; 0 when the window is narrower than 820 px (at
// scale 1) and the user has not asked for it.
static float leftRatioFor(float width, float charW, float uiScale, bool forced) {
    if (width <= 0.0f) {
        return 0.26f;
    }
    const bool narrow = width < 820.0f * uiScale;
    if (narrow && !forced) {
        return 0.0f;
    }
    const float minPx = 28.0f * charW + 16.0f * uiScale;
    float px = std::max(0.30f * width, minPx);
    if (narrow) {
        px = minPx; // forced on a narrow window: just enough for the panes to be legible
    }
    return std::min(px / width, 0.5f);
}

void DesktopFrame::draw(FrameInputs& in) {
    Runtime& rt = in.rt;
    Panes& p = in.panes;
    wireSearch(in);
    {
        const std::string wn = rt.session.world().name;
        if (!titled || wn != titledWorld) {
            SDL_SetWindowTitle(in.window, wn.empty() ? kProductName : (std::string(kProductName) + " // " + wn).c_str());
            titledWorld = wn;
            titled = true;
        }
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_F) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
        p.history.openSearch();
        ImGui::SetWindowFocus("HISTORY");
    }
    const ConnState st = rt.client.state();
    const std::string head = std::to_string(rt.session.head());
    if (ImGui::BeginMainMenuBar()) {
        ImGui::PushFont(in.ui, 0.0f);
        brandMark();
        ImGui::SameLine();
        ui::boldText(ui::Ink::strong, kProductName);
        drawMenus(MenuHost{in, this});
        if (ui::beginMenu("DEBUG")) {
            ui::menuItem("SESSION", nullptr, &showSession);
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("the connection, the session and the log's state");
            }
            ui::menuItem("BIND", nullptr, &showBind);
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("the workspace bind rows");
            }
            ImGui::Separator();
            if (ui::menuItem("DROP CONNECTION", nullptr, false, st != ConnState::Disconnected)) {
                rt.client.drop(); // closes the socket without detach; auto-reconnect follows
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ke::ui::tip("close the socket without a detach, to test the reconnect");
            }
            if (ui::menuItem("RECONNECT")) {
                rt.client.reconnect();
            }
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("open the connection again now");
            }
            ImGui::EndMenu();
        }
        {
            const std::string wname = rt.session.world().name;
            ImGui::SameLine();
            if (std::exchange(worldPickerWanted, false)) {
                ImGui::OpenPopup("world-picker");
            }
            if (ui::menuBarItem(ui::upper("world") + (wname.empty() ? std::string() : " " + wname), "world-menu", false, ui::Ink::strong)) {
                ImGui::OpenPopup("world-picker");
            }
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("the worlds this machine knows; attach to another, or add one");
            }
            if (opts && ImGui::BeginPopup("world-picker")) {
                worlds.drawBody(rt, *opts);
                if (worlds.request) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
        {
            // Everything waiting on a decision, amber so it is seen from across the room.
            const std::size_t waiting = rt.tasks.pending().size();
            const std::string label = ui::upper("approvals") + " " + std::to_string(waiting);
            ImGui::SameLine();
            if (ui::menuBarItem(label, "approvals", p.approvals.open(), waiting ? ui::Ink::inspected : ui::Ink::primary)) {
                p.approvals.toggle();
            }
            ImGui::SameLine();
            if (ui::noticesChip(rt.notifications, p.notices.open(), true)) {
                p.notices.toggle();
            }
        }
        ImGui::SameLine();
        if (ui::menuBarItem(ui::upper("search"), "search-menu", p.history.searchOpen())) {
            if (p.history.searchOpen()) {
                p.history.closeSearch();
            } else {
                p.history.openSearch();
                ImGui::SetWindowFocus("HISTORY");
            }
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("search Fields, names and Messages (Cmd-F); the box opens above HISTORY");
        }
        {
            // narrow window: the left column is hidden; [ PANES ] brings it back at its minimum width
            const float width = ImGui::GetMainViewport()->WorkSize.x;
            const bool narrow = width < 820.0f * in.uiScale;
            if (narrow) {
                ImGui::SameLine();
                if (ui::bracketButtonNamed(leftHidden ? "panes" : "panes x", "panes", true, false,
                                           leftHidden ? ui::Ink::secondary : ui::Ink::primary)) {
                    if (leftHidden) {
                        bringDockWindowFront("PRINCIPALS");
                    } else {
                        p.mux.hideView("view:principals");
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ke::ui::tip(leftHidden ? "show PRINCIPALS (the window is narrow)"
                                                 : "hide the left column again");
                }
            }
        }
        ui::setUnsafe(rt.unsafe);
        const double wallClock = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        // SESSION in one line: where this client stands, and anything wrong with the log.
        const std::size_t gaps = rt.events.gaps().size();
        const std::string gapText = gaps ? "GAPS " + std::to_string(gaps) + "  " : std::string();
        const std::string status = gapText + ui::upper(name(st)) + "  " + head;
        const float statusX = ImGui::GetWindowWidth() - ImGui::CalcTextSize(status.c_str()).x - 12.0f * in.uiScale;
        // Red, and it keeps its room on a narrow window: everything else in the bar yields.
        const float unsafeW = ui::unsafeIndicatorWidth(rt.principals, wallClock);
        if (unsafeW > 0.0f) {
            ImGui::SameLine(std::max(0.0f, statusX - unsafeW));
            ui::unsafeIndicator(rt.principals, rt.chat, wallClock);
        }
        ImGui::SameLine(statusX);
        if (gaps) {
            ui::text(ui::Ink::fatal, gapText);
            ImGui::SameLine(0.0f, 0.0f);
        }
        ui::text(ui::stateColor(st == ConnState::Attached, st == ConnState::Connecting || st == ConnState::Attaching),
                 ui::upper(name(st)));
        ImGui::SameLine();
        ui::text(ui::Ink::strong, head);
        ImGui::PopFont();
        ImGui::EndMainMenuBar();
    }

    p.drawModeline(in);
    {
        ImGui::PushFont(in.ui, 0.0f);
        const float charW = ImGui::CalcTextSize("M").x;
        ImGui::PopFont();
        const float width = ImGui::GetMainViewport()->WorkSize.x;
        const bool narrow = width < 820.0f * in.uiScale;
        g_layout = &p.mux;
        if (!viewsSet) {
            viewsSet = true;
            p.mux.setViews({{"view:principals", "PRINCIPALS", "principals", "principals"},
                            {"view:chat", "CHAT", "chat", "chat"},
                            {"view:graph", "GRAPH", "graph", "graph"},
                            {"view:panes", "PANES", "panes", "panes"},
                            {"view:history", "HISTORY", "history", "history"},
                            {"view:inspect", "INSPECT", "inspector", "inspector"}},
                           [charW, width, scale = in.uiScale](bool thin) {
                               return defaultLayout(thin, thin ? 0.0f : leftRatioFor(width, charW, scale, false));
                           });
        }
        // HISTORY collapsed is a strip one tab and one line high; the choice persists.
        const bool collapsed = p.history.collapsed;
        if (in.prefs.historyOpen == collapsed) {
            in.prefs.historyOpen = !collapsed;
            in.saveWhimsy();
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        p.mux.fold("view:history", collapsed ? ui::tabBarHeight() + 2.0f * ImGui::GetTextLineHeight() + 2.0f * style.WindowPadding.y : 0.0f);
    }
    p.terminal.unread = &p.unread;
    p.terminal.activity = &p.activity.marks;
    p.inspector.windowName = "INSPECT";
    p.inspector.unread = &p.unread;
    p.inspector.activity = &p.activity.marks;
    p.inspector.onEmpty = [&p] { p.mux.hideView("view:inspect"); };
    p.principals.setActivity(&p.activity.marks);
    p.terminal.setNotifications(&rt.notifications);
    p.inspector.setNotifications(&rt.notifications);
    p.terminal.onEmpty = [] { bringDockWindowFront("GRAPH"); };
    p.terminal.onApprovals = [&p] {
        if (!p.approvals.open()) {
            p.approvals.toggle();
        }
    };
    p.terminal.onNotices = [&p] {
        if (!p.notices.open()) {
            p.notices.toggle();
        }
    };
    p.graph.twilight = in.prefs.mapTwilight;
    ui::setTooltipsOn(in.prefs.tooltips);
    // the drive holds the pointer still and types by name, so a test keeps focus where it put it
    const bool followMouse = in.prefs.focusFollowsMouse && SDL_getenv("ORBIS_TEST") == nullptr;
    p.mux.followMouse = followMouse;
    p.mux.bind(&rt);
    p.mux.terminalTabs = [&p] { return p.terminal.tabIds(); };
    p.mux.restoreTerminalTab = [&p](const std::string& id) { p.terminal.open(id); };
    p.mux.onInspect = [&p, &rt](const std::string& object) { p.open(rt, object, 2); };
    {
        // One tree holds every view and pane: the host window behind them carries the tabs, the
        // dividers and whatever panes are in front; a view draws in its own window over its place.
        timing::Scope s("pane.panes");
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("ORBIS-LAYOUT", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
        const float pad = std::floor(2.0f * in.uiScale);
        p.mux.drawDesktop(model::LayoutRect{vp->WorkPos.x + pad, vp->WorkPos.y + pad, vp->WorkSize.x - 2.0f * pad, vp->WorkSize.y - 2.0f * pad},
                          vp->WorkSize.x < 820.0f * in.uiScale);
        // a pane just opened takes the keyboard, as its window would
        if (const std::string opened = p.mux.takeBringForward(); !opened.empty() && opened != "!" && opened.rfind("view:", 0) != 0) {
            ImGui::SetWindowFocus();
        }
        ImGui::End();
    }
    if (followMouse) {
        focusUnderPointer();
        // over the chat, its input line takes the keys
        const ImGuiIO& io = ImGui::GetIO();
        if ((io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && !io.WantTextInput && !ImGui::IsAnyMouseDown() &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) && p.mux.viewInFront("view:chat") && p.mux.pointerOverView("view:chat")) {
            p.terminal.requestFocus();
        }
    }
    leftHidden = !p.mux.viewOpen("view:principals");
    leftWanted = !leftHidden;
    {
        const std::string reading = p.mux.viewInFront("view:chat") ? p.terminal.current() : std::string();
        p.noteUnread(rt, reading);
        p.noteLoud(rt, in.window, in.prefs.osNotices);
        p.noteActivity(rt, reading, drive::active() || platform::windowFocused(in.window), in.window, in.prefs.osNotices);
    }
    std::string picked;
    {
        timing::Scope s("pane.principals");
        p.principals.self = rt.session.principal().value_or("");
        picked = p.principals.draw(rt.principals, p.terminal.current(), in.anim, in.mono);
    }
    if (!picked.empty() && p.open(rt, picked)) {
        bringDockWindowFront("CHAT");
    }
    if (p.principals.takeNewRequested()) {
        p.openAction(rt, "principal.create");
    }
    {
        timing::Scope s("pane.terminal");
        ui::setMathWorld(&rt.chat);
        p.terminal.draw(rt.principals, rt.evaluations, rt.chat, rt.session, in.anim, in.mono);
        p.inspector.draw(rt.principals, rt.evaluations, rt.chat, rt.session, in.anim, in.mono);
    }
    if (graphInFront) {
        bringDockWindowFront("GRAPH");
    }
    ui::GraphActions ga;
    {
        timing::Scope s("pane.graph");
        ga = p.graph.draw(p.graphInput(rt, in.anim, in.mono));
    }
    if (!landed && rt.session.attached() && rt.principals.applied() > 0) {
        landed = true;
        if (p.terminal.tabIds().empty()) {
            wantFront = "GRAPH"; // nothing open: a saved layout's front tab would be an empty chat
        }
    }
    if (!wantFront.empty() && p.mux.viewOpen("view:" + std::string(wantFront == "GRAPH" ? "graph" : "chat"))) {
        bringDockWindowFront(wantFront.c_str()); // the first thing a person sees is who is here
        wantFront.clear();
    }
    if (!ga.focus.empty() && p.open(rt, ga.focus, ga.view)) {
        bringDockWindowFront("CHAT");
    }
    if (ga.approvals && !p.approvals.open()) {
        p.approvals.toggle();
    }
    if (!ga.action.empty()) {
        p.openAction(rt, ga.action, ga.actionArgs);
    }
    if (const std::string act = p.terminal.takeActionRequest(); !act.empty()) {
        p.openAction(rt, act, p.terminal.takeActionArgs());
    }
    if (const std::string act = p.inspector.takeActionRequest(); !act.empty()) {
        p.openAction(rt, act, p.inspector.takeActionArgs());
    }
    if (auto ov = p.terminal.takeGraphOverlay()) { // [ EXPAND ] on a graph view: onto the GRAPH pane
        p.graph.setOverlay(std::move(ov->second), ov->first);
        bringDockWindowFront("GRAPH");
    }
    drawMenuPopups(MenuHost{in, this});
    ui::unsafeDialog(rt.principals, rt.chat);
    switch (p.follow(rt, in.mono)) { // a symbol clicked in any pane this frame
    case LinkTarget::Terminal:
        bringDockWindowFront("CHAT");
        break;
    case LinkTarget::Graph:
        bringDockWindowFront("GRAPH");
        break;
    default:
        break;
    }
    p.dialog.draw(rt.principals, rt.actions, rt.chat, rt.session, in.mono, in.window);
    p.history.setFocus(p.terminal.current(), rt.session.principal().value_or(""));
    {
        timing::Scope s("pane.history");
        p.history.draw(rt.events, rt.chat, in.anim, in.mono);
    }
    // Detail for debugging, off the screen until asked for: the menu bar carries the one line.
    if (showSession) {
        timing::Scope s("pane.session");
        ImGui::SetNextWindowSize(ImVec2(52.0f * ImGui::GetFontSize(), 26.0f * ImGui::GetFontSize()), ImGuiCond_FirstUseEver);
        p.session.draw(rt.session, rt.client, rt.events, rt.chat, in.mono);
    }
    if (showBind) {
        ImGui::SetNextWindowSize(ImVec2(52.0f * ImGui::GetFontSize(), 12.0f * ImGui::GetFontSize()), ImGuiCond_FirstUseEver);
        bool sendBinds = false;
        p.session.drawBinds(rt.binds, in.mono, rt.session.attached(), sendBinds);
        if (sendBinds) {
            rt.sendBinds();
        }
    }
}

#else

void applySafeArea(SDL_Window* window) {
    int w = 0, h = 0;
    if (!SDL_GetWindowSize(window, &w, &h)) {
        return;
    }
    const platform::Insets in = platform::safeArea(window);
    static platform::Insets logged{-1, -1, -1, -1};
    if (in.top != logged.top || in.left != logged.left || in.bottom != logged.bottom || in.right != logged.right) {
        logged = in;
        log::info("safe area: window {}x{}, insets top {} left {} bottom {} right {}", w, h, in.top, in.left, in.bottom, in.right);
    }
    auto* vp = static_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
    vp->BuildWorkInsetMin = ImVec2(in.left, in.top);
    // The keyboard takes the bottom of the work area, so the panes shrink rather than leave the screen.
    platform::watchKeyboard(window);
    const float bottom = std::max(in.bottom, static_cast<float>(platform::keyboardOverlap()));
    vp->BuildWorkInsetMax = ImVec2(in.right, bottom);
}

void SettingsPane::load(const Options& o) {
    url = o.url;
    token = o.token;
    principal = o.principal.value_or("");
}

void SettingsPane::prefill(const PairLink& link) {
    pairOpen = true;
    pairUrl = link.url;
    pairCode = link.code;
    if (!link.label.empty()) {
        pairLabel = link.label;
    }
    pairStatus = "link received: check the url and claim";
    pairErr = false;
}

bool SettingsPane::drawBody(Options& opts, Runtime& rt) {
    bool apply = false;
    ui::touchDragScroll();
    if (touch != nullptr) {
        ui::dimText("MODE");
        if (ui::bracketButtonNamed("touch", "mode-touch", true, *touch) && !*touch) {
            *touch = true;
            if (saveView) {
                saveView();
            }
        }
        ImGui::SameLine();
        if (ui::bracketButtonNamed("desk", "mode-desk", true, !*touch) && *touch) {
            *touch = false;
            if (saveView) {
                saveView();
            }
        }
        ImGui::SameLine();
        ui::dimText("touch: one conversation and a bar to hold while you speak; desk: the tabs");
        ImGui::Separator();
    }
    ui::dimText("WORLD URL");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##url", &url, ImGuiInputTextFlags_AutoSelectAll);
    ui::dimText("TOKEN");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##token", &token, ImGuiInputTextFlags_Password);
    ui::dimText("PRINCIPAL (OPTIONAL)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##principal", &principal);
    if (ui::bracketButton("apply and reconnect")) {
        Prefs p = loadPrefs(prefsFile(opts)).value_or(Prefs{});
        p.url = url;
        p.token = token;
        p.principal = principal.empty() ? std::nullopt : std::optional<std::string>(principal);
        const bool saved = savePrefs(prefsFile(opts), p);
        opts.url = url;
        opts.token = token;
        opts.principal = p.principal;
        status = saved ? "saved; reconnecting" : "could not save prefs.json";
        apply = true;
    }
    ImGui::SameLine();
    if (ui::bracketButton("revert")) {
        load(opts);
        status.clear();
    }
    ImGui::Separator();
    // PAIR WITH CODE: the desktop's DEVICES tab showed a code; claiming it on a fresh socket
    // answers a token for this phone alone (labelled, revocable there).
    if (ui::bracketButton(pairOpen ? "pair with code x" : "pair with code", true, pairOpen)) {
        pairOpen = !pairOpen;
        pairStatus.clear();
        if (pairUrl.empty()) {
            pairUrl = url;
        }
        if (pairLabel.empty()) {
            pairLabel = "laptop";
        }
    }
    if (pairOpen) {
        ui::dimText("WORLD URL");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##pair-url", &pairUrl);
        ui::dimText("CODE");
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 12);
        ImGui::InputText("##pair-code", &pairCode, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        ui::dimText("LABEL");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##pair-label", &pairLabel);
        const bool ready = pairCode.size() == 6 && (pairUrl.rfind("ws://", 0) == 0 || pairUrl.rfind("wss://", 0) == 0);
        if (ui::bracketButton("claim", ready && !claim.running())) {
            pairStatus = "claiming...";
            pairErr = false;
            claimPending = true;
            claim.start(pairUrl, "pair.claim", json{{"pairing-code", pairCode}, {"label", pairLabel}}, 8000);
        }
        if (claimPending && claim.done()) {
            claimPending = false;
            if (claim.ok()) {
                const json r = claim.result();
                std::string secret;
                for (const char* k : {"secret", "token", "token-secret"}) {
                    if (r.is_object() && r.contains(k) && r[k].is_string() && r[k].get<std::string>().size() > 16) {
                        secret = r[k].get<std::string>();
                        break;
                    }
                }
                if (secret.empty()) {
                    pairStatus = "the world answered without a token secret";
                    pairErr = true;
                } else {
                    Prefs p = loadPrefs(prefsFile(opts)).value_or(Prefs{});
                    p.url = pairUrl;
                    p.token = secret;
                    p.principal = std::nullopt;
                    const bool saved = savePrefs(prefsFile(opts), p);
                    url = opts.url = pairUrl;
                    token = opts.token = secret;
                    principal.clear();
                    opts.principal = std::nullopt;
                    WorldBook(sessionFile(opts)).remember(pairUrl, secret);
                    pairStatus = saved ? "paired; reconnecting as this device" : "paired, but prefs.json could not be saved";
                    pairErr = !saved;
                    pairOpen = false;
                    pairCode.clear();
                    apply = true;
                }
            } else {
                pairStatus = "claim failed: " + claim.error() +
                             (claim.error() == "could not connect" ? "; is the VPN or Wi-Fi on, and Local Network allowed for ORBIS in Settings?" : "");
                pairErr = true;
            }
        }
        if (!pairStatus.empty()) {
            ui::text(pairErr ? ui::Ink::inspected : ui::Ink::connected, pairStatus);
        }
        ui::dimText("the code comes from DEVICES > PAIR PHONE on a listener that holds the principal; it lasts 5 minutes");
    }
    ImGui::Separator();
    worlds.drawBody(rt, opts);
    if (auto req = worlds.takeAttach()) {
        Prefs p = loadPrefs(prefsFile(opts)).value_or(Prefs{});
        p.url = req->first;
        p.token = req->second;
        savePrefs(prefsFile(opts), p);
        url = opts.url = req->first;
        token = opts.token = req->second;
        status = "switching world; reconnecting";
        apply = true;
    }
    ImGui::Separator();
    ui::dimText("CONNECTION");
    if (ui::bracketButton("drop")) {
        rt.client.drop();
    }
    ImGui::SameLine();
    if (ui::bracketButton("reconnect")) {
        rt.client.reconnect();
    }
    ImGui::SameLine();
    if (ui::bracketButton("forget session")) {
        std::error_code ec;
        std::filesystem::remove(sessionFile(opts), ec);
        status = ec ? "could not remove session.json" : "session.json removed; applies at next attach";
        apply = true; // rebuild so the next attach is fresh and starts at the world's head
    }
    if (!status.empty()) {
        ImGui::TextUnformatted(status.c_str());
    }
    ImGui::Separator();
    ui::dimText("FILES");
    ImGui::TextWrapped("prefs    %s", prefsFile(opts).string().c_str());
    ImGui::TextWrapped("session  %s", sessionFile(opts).string().c_str());
    ImGui::TextWrapped("assets   %s", opts.assetsDir.c_str());
    return apply;
}

bool PhoneFrame::draw(FrameInputs& in, Options& opts, SDL_Window* window) {
    Runtime& rt = in.rt;
    Panes& p = in.panes;
    applySafeArea(window);
    settings.touch = &in.prefs.touch;
    settings.saveView = in.saveWhimsy;
    const bool touch = in.prefs.touch;
    p.terminal.bare = touch;
    p.drawModeline(in);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MAIN", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(in.ui, touch ? ImGui::GetFontSize() * 1.15f : 0.0f);
    if (touch) {
        // TOUCH: no tabs. One line says who you are talking to; [ people ] picks another, [ desk ]
        // returns to the tabs (the way to SETTINGS and the rest).
        activeTab = pickPrincipal ? 2 : 0;
        if (p.terminal.current().empty() && !pickPrincipal) {
            // nothing open yet: root is who a phone talks to first
            const chat::SymbolTable::Entry* root = ui::symbolUi().table() != nullptr ? ui::symbolUi().table()->byName("root") : nullptr;
            if (root != nullptr) {
                p.open(rt, root->id);
            }
        }
        const std::string cur = p.terminal.current();
        const chat::SymbolTable::Entry* who = ui::symbolUi().table() != nullptr ? ui::symbolUi().table()->byId(cur) : nullptr;
        ui::text(ui::Ink::strong, cur.empty() ? std::string("ORBIS") : ui::upper(who != nullptr && !who->name.empty() ? who->name : chat::idPrefix(cur)));
        ui::rowAtRight(ui::bracketRowWidth({"people", "desk"}));
        if (ui::bracketButtonNamed("people", "touch-people", true, pickPrincipal)) {
            pickPrincipal = !pickPrincipal;
        }
        ImGui::SameLine();
        if (ui::bracketButtonNamed("desk", "touch-desk")) {
            in.prefs.touch = false;
            in.saveWhimsy();
        }
    }
    // The tab labels, with the pending approvals waiting on this listener's principal as a badge.
    std::string terminalLabel = kPhoneTabs[0];
    if (const std::size_t n = rt.tasks.pendingFor(rt.session.principal().value_or("")); n > 0) {
        terminalLabel += " (" + std::to_string(n) + ")";
    }
    const char* phoneTabs[kPhoneTabCount] = {terminalLabel.c_str(), kPhoneTabs[1], kPhoneTabs[2], kPhoneTabs[3],
                                             kPhoneTabs[4],         kPhoneTabs[5], kPhoneTabs[6]};
    if (touch) {
        // the strip is not drawn: the header above stands in for it
    } else if (ui::tabStripWidth(phoneTabs, kPhoneTabCount) <= ImGui::GetContentRegionAvail().x) {
        activeTab = ui::tabStrip("phone", phoneTabs, kPhoneTabCount, activeTab);
    } else {
        // Narrower than the strip: two rows, or the last tab (Settings, the way in) is off the screen.
        constexpr int kTopRow = kPhoneTabCount / 2;
        const int active = activeTab;
        const int topActive = active < kTopRow ? active : -1;
        const int bottomActive = active < kTopRow ? -1 : active - kTopRow;
        const int top = ui::tabStrip("phone", phoneTabs, kTopRow, topActive);
        const int bottom = ui::tabStrip("phone-2", phoneTabs + kTopRow, kPhoneTabCount - kTopRow, bottomActive);
        activeTab = top != topActive ? top : bottom != bottomActive ? kTopRow + bottom : active;
    }
    // The notices chip and the attach state live in the modeline at the bottom, where status lives,
    // so the tabs are the only thing above the panes: at phone width the strip already takes two
    // rows, and a third for status pushed the transcript down (iPhone Duo, 2026-11-11).
    {
        // The same red indicator as the desktop's, on a line of its own, always seen.
        ui::setUnsafe(rt.unsafe);
        const double wallClock = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (ui::unsafeIndicatorWidth(rt.principals, wallClock) > 0.0f) {
            ui::unsafeIndicator(rt.principals, rt.chat, wallClock);
        }
    }
    ImGui::PopFont();
    p.noteUnread(rt, activeTab == 0 ? p.terminal.current() : std::string());
    p.noteLoud(rt, nullptr, false); // a phone's window is the screen: push is another job
    p.noteActivity(rt, activeTab == 0 ? p.terminal.current() : std::string(), true, nullptr, false);
    p.terminal.activity = &p.activity.marks;
    p.principals.setActivity(&p.activity.marks);
    p.terminal.setNotifications(&rt.notifications);
    p.terminal.onApprovals = [&p] {
        if (!p.approvals.open()) {
            p.approvals.toggle();
        }
    };
    p.terminal.onNotices = [&p] {
        if (!p.notices.open()) {
            p.notices.toggle();
        }
    };
    p.terminal.unread = &p.unread;
    const bool terminalTab = activeTab == 0; // its own input line; the strip is hidden
    const float inputHeight = terminalTab ? 0.0f : ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    // TOUCH: the bar a thumb holds, under the conversation, as tall as two lines of text
    const float barHeight = touch && terminalTab ? std::floor(ImGui::GetTextLineHeight() * 2.4f) : 0.0f;
    const float barGap = barHeight > 0.0f ? ImGui::GetStyle().ItemSpacing.y : 0.0f;
    ImGui::BeginChild("body", ImVec2(0, ImGui::GetContentRegionAvail().y - inputHeight - barHeight - barGap), ImGuiChildFlags_None);
    bool rebuild = false;
    if (activeTab == 0) {
        ui::setMathWorld(&rt.chat);
        p.terminal.drawBody(rt.principals, rt.evaluations, rt.chat, rt.session, in.anim, in.mono);
        p.terminal.drawExpandedViews(rt.principals, rt.chat, in.mono);
        p.terminal.drawThreadWindows(rt.principals, rt.evaluations, rt.chat, rt.session, in.mono);
    } else if (activeTab == 1) {
        const ui::GraphActions ga = p.graph.drawBody(p.graphInput(rt, in.anim, in.mono));
        if (!ga.focus.empty()) {
            p.open(rt, ga.focus, ga.view);
            activeTab = 0;
        }
        if (ga.approvals && !p.approvals.open()) {
            p.approvals.toggle();
        }
        if (!ga.action.empty()) {
            p.openAction(rt, ga.action, ga.actionArgs);
        }
    } else if (activeTab == 2) {
        p.principals.self = rt.session.principal().value_or("");
        const std::string picked = p.principals.drawBody(rt.principals, p.terminal.current(), in.anim, in.mono);
        if (!picked.empty()) {
            p.open(rt, picked);
            activeTab = 0;
        }
        if (p.principals.takeNewRequested()) {
            p.openAction(rt, "principal.create");
        }
    } else if (activeTab == 3) {
        wireSearch(in);
        p.history.drawBody(rt.events, rt.chat, in.anim, in.mono);
    } else if (activeTab == 5) {
        p.mux.bind(&rt);
        p.mux.terminalTabs = [&p] { return p.terminal.tabIds(); };
        p.mux.restoreTerminalTab = [&p](const std::string& id) { p.terminal.open(id); };
        p.mux.draw();
    } else if (activeTab == 4) {
        p.session.drawBody(rt.session, rt.client, rt.events, rt.chat, in.mono);
        ImGui::Separator();
        ui::paneTitle("BIND");
        bool sendBinds = false;
        p.session.drawBindsBody(rt.binds, in.mono, rt.session.attached(), sendBinds);
        if (sendBinds) {
            rt.sendBinds();
        }
    } else {
        rebuild = settings.drawBody(opts, rt);
        ImGui::Separator();
        drawMenusFlat(MenuHost{in, nullptr});
    }
    ImGui::EndChild();
    ui::outlineRect(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ui::Ink::secondary);
    if (touch) {
        pickPrincipal = activeTab == 2; // a pick in the people list lands on the conversation
    }
    if (barHeight > 0.0f && !p.terminal.drawTalkBar(barHeight)) {
        // no Model can hear: the bar's room says so, in the same voice as the line's notes
        ImGui::Dummy(ImVec2(0.0f, std::floor(barHeight / 4)));
        ui::dimText("no Model here can hear");
    }
    if (!terminalTab) {
        p.input.drawBody(rt.chat, in.anim, in.mono);
    }
    if (const std::string act = p.terminal.takeActionRequest(); !act.empty()) {
        p.openAction(rt, act, p.terminal.takeActionArgs());
    }
    if (auto ov = p.terminal.takeGraphOverlay()) { // [ EXPAND ] on a graph view: onto the GRAPH tab
        p.graph.setOverlay(std::move(ov->second), ov->first);
        activeTab = 1;
    }
    switch (p.follow(rt, in.mono)) { // a symbol tapped in any pane this frame
    case LinkTarget::Terminal:
        activeTab = 0;
        break;
    case LinkTarget::Graph:
        activeTab = 1;
        break;
    case LinkTarget::History:
        activeTab = 3;
        break;
    default:
        break;
    }
    p.dialog.draw(rt.principals, rt.actions, rt.chat, rt.session, in.mono, nullptr);
    ui::unsafeDialog(rt.principals, rt.chat);
    ImGui::End();
    if (p.input.active() || p.terminal.inputActive()) {
        // Keep the active line above the on-screen keyboard: SDL scrolls the view to this area.
        const bool term = p.terminal.inputActive();
        const ImVec2 mn = term ? p.terminal.inputMin() : p.input.fieldMin();
        const ImVec2 mx = term ? p.terminal.inputMax() : p.input.fieldMax();
        SDL_Rect area{static_cast<int>(mn.x), static_cast<int>(mn.y), static_cast<int>(mx.x - mn.x),
                      static_cast<int>(mx.y - mn.y)};
        SDL_SetTextInputArea(window, &area, 0);
    }
    return rebuild;
}

#endif // LISTENER_PHONE

} // namespace ke::app
