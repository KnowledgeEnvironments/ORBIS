#include "model/FieldNews.hpp"

#include <algorithm>
#include <cctype>

#include "ui/UnsafeView.hpp"
#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

void TerminalPane::jumpTo(const std::string& principal, const std::string& message) {
    open(principal, 0);
    if (current_ >= 0 && static_cast<std::size_t>(current_) < tabs_.size() &&
        tabs_[static_cast<std::size_t>(current_)].principal == principal) {
        Tab& tab = tabs_[static_cast<std::size_t>(current_)];
        tab.jumpMessage = message;
        tab.jumpSince = steadySeconds();
    }
}

void TerminalPane::open(const std::string& principal, int view) {
    const bool inspect = view == 2;
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].principal == principal && (tabs_[i].object || tabs_[i].inspector == inspect)) {
            current_ = static_cast<int>(i);
            focusRequested_ = true;
            if (view >= 0) {
                tabs_[i].view = view;
            }
            return;
        }
    }
    Tab t;
    t.principal = principal;
    t.inspector = inspect;
    if (view >= 0) {
        t.view = view;
    }
    tabs_.push_back(std::move(t));
    current_ = static_cast<int>(tabs_.size()) - 1;
    focusRequested_ = true;
}

void TerminalPane::scrollToField(const std::string& object, const std::string& field) {
    open(object, 2);
    tabs_[static_cast<std::size_t>(current_)].scrollField = field;
}

const model::PrincipalInfo* TerminalPane::objectInfo(const std::string& id, model::Principals& principals, chat::Chat& chat) {
    if (objectsAsked_.insert(id).second) {
        chat.readObject(id, [this, id, &principals](const Response& r) {
            ObjectEntry& e = objects_[id];
            if (!r.ok) {
                e.err = r.err ? r.err->code : "?";
                return;
            }
            const model::SnapshotObject o = model::SnapshotObject::parse(r.result);
            e.err.clear();
            e.ready = true;
            e.info.id = id;
            e.info.cls = o.cls;
            e.info.fields = o.fields;
            e.info.created = o.created;
            e.info.createdAt = o.createdAt;
            if (!o.name.empty()) {
                principals.alias(id, o.name);
            }
        });
    }
    auto it = objects_.find(id);
    return it != objects_.end() && it->second.ready ? &it->second.info : nullptr;
}

std::string TerminalPane::currentWorker() const {
    if (current_ < 0 || current_ >= static_cast<int>(tabs_.size())) {
        return "";
    }
    const Tab& t = tabs_[static_cast<std::size_t>(current_)];
    return t.worker.empty() ? t.principal : t.worker;
}

void TerminalPane::setView(int view) {
    if (current_ >= 0 && current_ < static_cast<int>(tabs_.size())) {
        tabs_[static_cast<std::size_t>(current_)].view = std::clamp(view, 0, 6);
    }
}

void TerminalPane::compressCurrent(model::Principals& principals, chat::Chat& chat, const Session& session) {
    if (current_ >= 0 && current_ < static_cast<int>(tabs_.size())) {
        Tab& tab = tabs_[static_cast<std::size_t>(current_)];
        sendDirective(tab, principals.get(tab.principal), principals, chat, session, "/compress");
    }
}

void TerminalPane::closeCurrent() {
    if (current_ < 0 || current_ >= static_cast<int>(tabs_.size())) {
        return;
    }
    tabs_.erase(tabs_.begin() + current_);
    if (tabs_.empty()) {
        current_ = -1;
    } else {
        current_ = std::min(current_, static_cast<int>(tabs_.size()) - 1);
    }
}

std::vector<std::string> TerminalPane::tabIds() const {
    std::vector<std::string> out;
    for (const Tab& t : tabs_) {
        if (t.object || !t.inspector) { // an agent's inspector is a moment's look, not part of the layout
            out.push_back(t.principal);
        }
    }
    return out;
}

const std::string& TerminalPane::current() const {
    return current_ >= 0 && current_ < static_cast<int>(tabs_.size()) ? tabs_[static_cast<std::size_t>(current_)].principal
                                                                      : kNone;
}

bool TerminalPane::inputActive() const {
    return current_ >= 0 && tabs_[static_cast<std::size_t>(current_)].line.active();
}

ImVec2 TerminalPane::inputMin() const {
    return current_ >= 0 ? tabs_[static_cast<std::size_t>(current_)].line.fieldMin() : ImVec2();
}

ImVec2 TerminalPane::inputMax() const {
    return current_ >= 0 ? tabs_[static_cast<std::size_t>(current_)].line.fieldMax() : ImVec2();
}

void TerminalPane::requestFocus() {
    focusRequested_ = true;
}

bool TerminalPane::spinning(const model::Principals& principals) const {
    for (const Tab& t : tabs_) {
        if (const model::PrincipalInfo* p = principals.get(t.principal); p && p->loopIteration() > 0) {
            return true;
        }
    }
    return false;
}

void TerminalPane::draw(model::Principals& principals, const model::Evaluations& evals, chat::Chat& chat,
                        const Session& session, const Anim& anim, ImFont* mono) {
    // A hidden tab (GRAPH in front) draws nothing, so its input line cannot pull focus back.
    // With a header (the desktop's CHAT|GRAPH switch) the pane title would repeat it: the
    // switch and the per-Principal tabs share row 1 instead, the view tabs are row 2.
    // This pane never scrolls as a whole, its transcript does: a page too wide for a narrow pane is
    // clipped at the right, and neither focus nor a wide inspector may slide the tab row. Set before
    // Begin, because inside it the scroll would only land a frame late.
    ImGui::SetNextWindowScroll(ImVec2(0.0f, -1.0f));
    if (beginPane(windowName, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, !header)) {
        inlineTabs_ = static_cast<bool>(header);
        if (header) {
            header();
        }
        drawBody(principals, evals, chat, session, anim, mono);
        inlineTabs_ = false;
    }
    endPane();
    drawExpandedViews(principals, chat, mono);
    drawThreadWindows(principals, evals, chat, session, mono);
}

void TerminalPane::drawBody(model::Principals& principals, const model::Evaluations& evals, chat::Chat& chat,
                            const Session& session, const Anim& anim, ImFont* mono) {
    ImGui::PushFont(mono, 0.0f);
    for (const std::string& name : pendingNames_) {
        chat.readNamed(name, [this, name, &principals](const Response& r) {
            if (r.ok) {
                const model::SnapshotObject o = model::SnapshotObject::parse(r.result);
                if (!o.id.empty()) {
                    if (name.find('/') != std::string::npos) { // a file's tab reads as the file
                        principals.alias(o.id, name.substr(name.rfind('/') + 1));
                    }
                    open(o.id, 2);
                }
            }
        });
    }
    pendingNames_.clear();
    if (tabs_.empty()) {
        dimText("no chat open: click an agent");
        ImGui::PopFont();
        return;
    }
    // Tabs: one per open object, named from the fold; a chat and an inspector read differently.
    std::vector<TabItem> items;
    for (const Tab& t : tabs_) {
        TabItem item;
        item.id = t.principal;
        const model::PrincipalInfo* p = principals.get(t.principal);
        const bool isPrincipal = p != nullptr && p->cls == "principal";
        const bool inspecting = t.inspector || (p != nullptr && !isPrincipal) || t.object;
        const bool me = !inspecting && t.principal == session.principal().value_or("");
        item.glyph = inspecting ? "(i)" : me ? "@" : ">";
        item.label = principals.nameOf(t.principal);
        if (inspecting) {
            item.note = "inspector";
            if (isPrincipal) {
                item.target = "inspect:" + item.label; // beside the same Principal's chat
            }
        }
        item.alarm = isUnsafe(t.principal);
        if (const model::PrincipalInfo* who = principals.get(t.principal); who != nullptr) {
            if (const int waiting = model::waitingEdits(principals, *who); waiting > 0) {
                item.note = std::to_string(waiting) + (waiting == 1 ? " edit waits" : " edits wait");
            }
        }
        if (unread != nullptr) {
            if (auto it = unread->find(t.principal); it != unread->end()) {
                item.badge = it->second;
            }
        }
        if (activity != nullptr) {
            if (auto it = activity->find(t.principal); it != activity->end()) {
                item.activity = it->second;
            }
        }
        items.push_back(std::move(item));
    }
    const TabBarResult tabsPicked = tabBar("principal-tabs", items, current_, ImGui::GetContentRegionAvail().x, tabBarState_);
    if (tabsPicked.picked >= 0) {
        current_ = tabsPicked.picked;
        focusRequested_ = true;
    }
    if (tabsPicked.closed >= 0) {
        current_ = tabsPicked.closed;
        closeCurrent();
    } else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
               ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W)) {
        closeCurrent();
    }
    if (tabs_.empty()) {
        // Closing the last tab goes back to who is here.
        if (onEmpty) {
            onEmpty();
        }
        ImGui::Dummy(ImVec2(0, 0)); // the tab row left the cursor set
        ImGui::PopFont();
        return;
    }
    Tab& tab = tabs_[static_cast<std::size_t>(current_)];
    const model::PrincipalInfo* info = principals.get(tab.principal);
    if (!info) {
        info = objectInfo(tab.principal, principals, chat);
    }
    tab.worker = tab.principal;
    if (info) {
        tab.object = info->cls != "principal";
    }
    // Anything that is not a Principal has no conversation: its inspector tab is its Fields, and
    // its memory when it has any.
    fieldsEvals_ = &evals;
    if (!info || info->cls != "principal" || tab.inspector) {
        bool hasMemory = false;
        if (info) {
            for (const auto& [name, f] : info->fields) {
                hasMemory = hasMemory || name.rfind("memory/", 0) == 0;
            }
        }
        if (inlineTabs_) {
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
        }
        static const char* const kObjectViews[] = {"fields", "memory"};
        const int shown = tab.view == 3 && hasMemory ? 1 : 0;
        const int chosen = subTabs("object-views", kObjectViews, hasMemory ? 2 : 1, shown);
        if (info) { // while it is still unknown (the fold is booting) the tab keeps its view
            tab.view = chosen == 1 ? 3 : 2;
        }
        if (!info) {
            auto it = objects_.find(tab.principal);
            if (it != objects_.end() && !it->second.err.empty()) {
                text(Ink::inspected, "unreadable: " + it->second.err);
            } else {
                dimText("reading...");
            }
        } else if (tab.view == 3) {
            drawMemory(tab, info, principals, chat, mono);
        } else {
            drawFields(tab, info, principals, chat, session);
        }
        ImGui::PopFont();
        return;
    }

    // Yourself: a page, not a conversation.
    if (tab.principal == session.principal().value_or("") && info->drivenBy == "person") {
        drawSelfPage(tab, info, principals, evals, chat, session, anim, mono);
        ImGui::PopFont();
        return;
    }

    // View switch: slanted tabs, inverse video for the active one (one tab style everywhere).
    // Shown in this order; tab.view keeps its number, so CONVERSATIONS sits beside TRANSCRIPT.
    static const char* const kViews[] = {"transcript", "conversations", "instructions", "fields", "memory", "devices", "rules"};
    static const int kViewIds[] = {0, 6, 1, 2, 3, 4, 5};
    int shownView = 0;
    for (int i = 0; i < 7; ++i) {
        if (kViewIds[i] == tab.view) {
            shownView = i;
        }
    }
    if (inlineTabs_) {
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x); // row 2 starts at the pane's left edge
    }
    const ImVec2 rowAt = ImGui::GetCursorScreenPos();
    tab.view = kViewIds[subTabs("views", kViews, 7, shownView)];
    if (tab.view == 0) {
        // What the transcript shows: a choice in the sub-strip's own words, right-aligned on the views'
        // row when there is room, else on its own row. It changes the view, so it is not a button.
        static const char* const kShowNames[] = {"all", "messages", "activity"};
        static const char* const kShowIds[] = {"all", "chat", "system"};
        static const char* const kShowTargets[] = {"filter:all", "filter:chat", "filter:system"};
        float words = 4.0f;
        for (const char* v : kViews) {
            words += ImGui::CalcTextSize(upper(v).c_str()).x + 12.0f;
        }
        const float want = subChoiceWidth("show", kShowNames, 3);
        const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const bool fits = rowAt.x + words + want + 16.0f <= rightEdge;
        const ImVec2 at(std::max(rowAt.x, rightEdge - want), fits ? rowAt.y : after.y);
        ImGui::SetCursorScreenPos(at);
        int shown = 0;
        for (int i = 0; i < 3; ++i) {
            if (this->filter == kShowIds[i]) {
                shown = i;
            }
        }
        this->filter = kShowIds[subChoice("show", "show", kShowNames, kShowTargets, 3, shown)];
        if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + want, at.y + ImGui::GetTextLineHeight() + 2.0f))) {
            ke::ui::tip("all: everything. messages: what people and agents said. activity: heartbeats, approvals and status lines");
        }
        if (fits) {
            restoreCursor(after);
        }
    }

    drawStatusBar(tab, info, principals, evals, chat, session);
    // Esc twice within 400 ms (or Cmd-.) stops the running Evaluation; a single Esc keeps its
    // meaning (deselect, leave the line). The hint above makes the gesture discoverable.
    if (tab.line.takeEscapedEmpty()) {
        tab.sel.clear(); // Esc on an empty line: deselect in the transcript
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        const double now = steadySeconds();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (tab.escAt >= 0 && now - tab.escAt <= kEscEscSeconds) {
                cancel(tab, evals, chat);
                tab.escAt = -1.0;
            } else {
                tab.escAt = now;
            }
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_Period)) {
            cancel(tab, evals, chat);
        }
    }

    switch (tab.view) {
    case 0:
        drawTranscript(tab, info, principals, evals, chat, session, anim, mono);
        break;
    case 1:
        drawPrompt(tab, info, principals, chat);
        break;
    case 3:
        drawMemory(tab, info, principals, chat, mono);
        break;
    case 4:
        drawDevices(tab, info, principals, chat, session, mono);
        break;
    case 5:
        drawPolicy(tab, info, principals, chat);
        break;
    case 6:
        drawConversations(tab, info, principals, chat, session);
        break;
    default:
        drawFields(tab, info, principals, chat, session);
        break;
    }
    ImGui::PopFont();
}

void TerminalPane::drop(const std::string& path, const std::string& text, ImVec2 at) {
    (void)at;
    if (current_ < 0 || tabs_.empty()) {
        return;
    }
    tabs_[static_cast<std::size_t>(current_)].pendingDrops.emplace_back(path, text);
}

std::optional<std::pair<std::string, model::GraphModel>> TerminalPane::takeGraphOverlay() {
    std::optional<std::pair<std::string, model::GraphModel>> out;
    out.swap(graphOverlay_);
    return out;
}

TerminalPane::AttachStatus TerminalPane::attachStatus() const {
    if (current_ < 0 || tabs_.empty()) {
        return {};
    }
    auto it = attach_.find(tabs_[static_cast<std::size_t>(current_)].principal);
    return it == attach_.end() ? AttachStatus{} : it->second;
}

void TerminalPane::openRef(Tab& tab, model::Principals& principals, const std::string& ref) {
    if (principals.get(ref)) {
        open(ref, 2);
        return;
    }
    ImGui::SetClipboardText(ref.c_str());
    attach_[tab.principal] = AttachStatus{"ref " + chat::idPrefix(ref) + " is not a Principal; its id was copied", false, true};
}

namespace {
bool imagePath(const std::string& path) {
    std::string low = path;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* ext : {".png", ".jpg", ".jpeg", ".gif", ".webp"}) {
        const std::string e = ext;
        if (low.size() > e.size() && low.compare(low.size() - e.size(), e.size(), e) == 0) {
            return true;
        }
    }
    return false;
}
std::string baseOf(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
} // namespace

void TerminalPane::addImageBytes(Tab& tab, chat::Chat& chat, std::vector<unsigned char> bytes, const std::string& name) {
    auto item = std::make_shared<Tab::PendingImage>();
    item->name = name;
    tab.images.push_back(item);
    chat.putImage(std::move(bytes), name, [item](nlohmann::json a, std::string err) {
        item->attachment = std::move(a);
        item->error = std::move(err);
    });
}

void TerminalPane::addImageFile(Tab& tab, chat::Chat& chat, const std::string& path) {
    std::size_t n = 0;
    void* data = SDL_LoadFile(path.c_str(), &n);
    if (data == nullptr) {
        auto item = std::make_shared<Tab::PendingImage>();
        item->name = baseOf(path);
        item->error = "cannot read it here";
        tab.images.push_back(item);
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    addImageBytes(tab, chat, std::vector<unsigned char>(bytes, bytes + n), baseOf(path));
    SDL_free(data);
}

void TerminalPane::addImageAt(chat::Chat& chat, const std::string& mount, const std::string& path) {
    if (current_ < 0 || tabs_.empty()) {
        return;
    }
    // An image travels with the line; any other file goes as its path, for the agent to read.
    std::string ext = path.substr(std::min(path.rfind('.'), path.size()));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".gif" && ext != ".webp") {
        tabs_[static_cast<std::size_t>(current_)].line.addTranscript(mount.empty() ? path : mount + "/" + path);
        return;
    }
    auto item = std::make_shared<Tab::PendingImage>();
    item->name = baseOf(path);
    tabs_[static_cast<std::size_t>(current_)].images.push_back(item);
    chat.putImageFile(mount, path, [item](nlohmann::json a, std::string err) {
        item->attachment = std::move(a);
        item->error = std::move(err);
    });
}

void TerminalPane::processDrops(Tab& tab, chat::Chat& chat, const Session& session, const std::vector<std::string>& routes) {
    if (tab.pendingDrops.empty()) {
        return;
    }
    std::vector<std::pair<std::string, std::string>> drops;
    drops.swap(tab.pendingDrops);
    const std::string principal = tab.principal;
    const std::string route = routes.empty() ? std::string() : routes.front();
    AttachStatus* status = &attach_[principal];
    auto progress = [status](const std::string& stage, bool err, bool done) {
        status->stage = stage;
        status->err = err;
        status->done = done;
    };
    for (auto& [path, text] : drops) {
        if (!session.attached()) {
            *status = AttachStatus{"not attached: no carrier", true, true};
            continue;
        }
        if (!text.empty()) {
            chat::Attachment a;
            a.name = "dropped-text.txt";
            a.mime = "text/plain";
            a.content = text;
            a.size = text.size();
            a.sha256 = util::Sha256::hex(text);
            a.text = true;
            *status = AttachStatus{"attaching dropped text\u2026", false, false};
            chat.attach(principal, route, std::move(a), progress);
            continue;
        }
        if (imagePath(path)) {
            addImageFile(tab, chat, path);
            continue;
        }
        chat::Attachment a = chat::classify(path);
        if (a.directory) {
            tab.folderPrompt = path;
            *status = AttachStatus{};
            continue;
        }
        if (!a.refusal.empty()) {
            *status = AttachStatus{"not attached: " + a.name + " " + a.refusal, true, true};
            continue;
        }
        *status = AttachStatus{"attaching " + a.name + " (" + std::to_string(a.size) + " B)\u2026", false, false};
        chat.attach(principal, route, std::move(a), progress);
    }
}

void TerminalPane::cancel(Tab& tab, const model::Evaluations& evals, chat::Chat& chat) {
    const std::string& who = tab.worker.empty() ? tab.principal : tab.worker;
    const model::EvaluationInfo* r = evals.running(who);
    if (!r && !tab.cancelling.empty()) {
        return; // already asked
    }
    const std::string target = r ? r->id : who;
    tab.cancelling = target;
    tab.cancelResult.clear();
    std::string* result = &tab.cancelResult;
    std::string* cancelling = &tab.cancelling;
    chat.actOn(target, "cancel", json(), [result, cancelling, target](const Response& resp) {
        if (!resp.ok) {
            *result = "CANCEL: " + (resp.err ? upper(resp.err->code) + " " + resp.err->message : std::string("?"));
            if (*cancelling == target) {
                cancelling->clear();
            }
        }
    });
}

void TerminalPane::cancelCurrent(const model::Evaluations& evals, chat::Chat& chat) {
    if (current_ >= 0) {
        cancel(tabs_[static_cast<std::size_t>(current_)], evals, chat);
    }
}

} // namespace ke::ui
