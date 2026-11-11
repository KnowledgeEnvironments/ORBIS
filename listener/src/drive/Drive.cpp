#include "ui/MathView.hpp"
#include "ui/ChatGptSignIn.hpp"
#include "drive/Drive.hpp"

#include "app/DebugHooks.hpp"
#include "app/Layout.hpp"
#include "app/Lifecycle.hpp"
#include "app/Runtime.hpp"
#include "chat/KernelRecord.hpp"
#include "chat/Transcript.hpp"
#include "drive/Commands.hpp"
#include "model/Evaluations.hpp"
#include "platform/Platform.hpp"
#include "util/Log.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <imgui_internal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ke::drive {

namespace {

// What the test is looking at: the inspector once it has the focus, else the chats.
ui::TerminalPane& conversation(FrameContext& ctx) {
#if !LISTENER_PHONE
    if (ctx.panes.mux.focused() == "view:inspect" && !ctx.panes.inspector.current().empty()) {
        return ctx.panes.inspector;
    }
#endif
    return ctx.panes.terminal;
}


// --- the hit registry (UI thread) ------------------------------------------------------------
std::atomic<bool> g_active{false};
std::map<std::string, ImRect>& registry() {
    static std::map<std::string, ImRect> r;
    return r;
}
unsigned long long g_registryFrame = 0;

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

const std::map<std::string, ImGuiKey>& keyTable() {
    static const std::map<std::string, ImGuiKey> t = {
        {"return", ImGuiKey_Enter},     {"enter", ImGuiKey_Enter},      {"escape", ImGuiKey_Escape},
        {"esc", ImGuiKey_Escape},       {"up", ImGuiKey_UpArrow},       {"down", ImGuiKey_DownArrow},
        {"left", ImGuiKey_LeftArrow},   {"right", ImGuiKey_RightArrow}, {"tab", ImGuiKey_Tab},
        {"backspace", ImGuiKey_Backspace}, {"delete", ImGuiKey_Delete}, {"space", ImGuiKey_Space},
        {"home", ImGuiKey_Home},        {"end", ImGuiKey_End},          {"pageup", ImGuiKey_PageUp},
        {"pagedown", ImGuiKey_PageDown}, {"period", ImGuiKey_Period},   {".", ImGuiKey_Period},
        {"comma", ImGuiKey_Comma},      {",", ImGuiKey_Comma},          {"minus", ImGuiKey_Minus},
        {"-", ImGuiKey_Minus},          {"equal", ImGuiKey_Equal},      {"=", ImGuiKey_Equal},
        {"slash", ImGuiKey_Slash},      {"/", ImGuiKey_Slash},
    };
    return t;
}

ImGuiKey keyFromName(const std::string& name) {
    const std::string n = lower(name);
    if (const auto it = keyTable().find(n); it != keyTable().end()) {
        return it->second;
    }
    if (n.size() == 1) {
        const char c = n[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'a'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
        }
    }
    if (n.size() == 2 && n[0] == 'f' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (n[1] - '1'));
    }
    return ImGuiKey_None;
}

ImGuiKey modFromName(const std::string& name) {
    const std::string n = lower(name);
    if (n == "cmd" || n == "super" || n == "meta") {
        return ImGuiMod_Super;
    }
    if (n == "shift") {
        return ImGuiMod_Shift;
    }
    if (n == "alt" || n == "option") {
        return ImGuiMod_Alt;
    }
    if (n == "ctrl" || n == "control") {
        return ImGuiMod_Ctrl;
    }
    return ImGuiKey_None;
}

json rectJson(const ImRect& r) {
    return json{{"x", r.Min.x}, {"y", r.Min.y}, {"w", r.GetWidth()}, {"h", r.GetHeight()}};
}

std::string readToken(const std::string& file) {
    std::ifstream in(file);
    std::string t;
    if (in) {
        std::getline(in, t);
    }
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) {
        t.pop_back();
    }
    return t;
}

} // namespace

bool active() {
    return g_active.load(std::memory_order_relaxed);
}

void hit(const std::string& name, ImVec2 min, ImVec2 max) {
    if (!active()) {
        return;
    }
    if (!ImGui::IsRectVisible(min, max)) {
        return; // a row clipped by its pane is not a place a click can reach
    }
    registry()[lower(name)] = ImRect(min, max);
}

// --- lifecycle ------------------------------------------------------------------------------

Drive::Drive() = default;

Drive::~Drive() {
    stop();
}

bool Drive::start(const std::string& socketPath, SDL_Window* window) {
    if (socketPath.empty()) {
        return true;
    }
    window_ = window;
    path_ = socketPath;
    sockaddr_un addr{};
    if (path_.size() >= sizeof(addr.sun_path)) {
        log::error("drive: socket path too long: {}", path_);
        return false;
    }
    ::unlink(path_.c_str());
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        log::error("drive: socket: {}", std::strerror(errno));
        return false;
    }
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(listenFd_, 1) < 0) {
        log::error("drive: bind/listen {}: {}", path_, std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    wakeEvent_ = SDL_RegisterEvents(1);
    g_active.store(true);
    thread_ = std::thread([this] { serve(); });
    log::info("drive: listening on {}", path_);
    return true;
}

void Drive::stop() {
    if (listenFd_ < 0) {
        return;
    }
    stopping_.store(true);
    g_active.store(false);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (clientFd_ >= 0) {
            ::shutdown(clientFd_, SHUT_RDWR);
        }
    }
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    if (thread_.joinable()) {
        thread_.join();
    }
    listenFd_ = -1;
    ::unlink(path_.c_str());
}

void Drive::serve() {
    while (!stopping_.load()) {
        pollfd p{listenFd_, POLLIN, 0};
        const int r = ::poll(&p, 1, 200);
        if (r <= 0) {
            continue;
        }
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (stopping_.load()) {
                break;
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            clientFd_ = fd;
        }
        log::info("drive: client connected");
        handleClient(fd);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (clientFd_ == fd) {
                clientFd_ = -1;
            }
        }
        ::close(fd);
        log::info("drive: client left");
    }
}

void Drive::handleClient(int fd) {
    std::string buf;
    char chunk[4096];
    while (!stopping_.load()) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 200);
        if (r < 0) {
            break;
        }
        if (r == 0) {
            continue;
        }
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) {
            break;
        }
        buf.append(chunk, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.find_first_not_of(" \t\r") == std::string::npos) {
                continue;
            }
            json j = json::parse(line, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("cmd")) {
                reply(j.is_object() && j.contains("id") ? j["id"] : json(nullptr),
                      json{{"ok", false}, {"err", "bad request: expected {id, cmd, ...}"}});
                continue;
            }
            Command c{j.value("id", json(nullptr)), j["cmd"].get<std::string>(), j};
            {
                std::lock_guard<std::mutex> lock(mu_);
                pending_.push_back(std::move(c));
            }
            SDL_Event ev{};
            ev.type = wakeEvent_;
            SDL_PushEvent(&ev);
        }
    }
}

void Drive::reply(const json& id, json body) {
    body["id"] = id;
    const std::string line = body.dump() + "\n";
    std::lock_guard<std::mutex> lock(mu_);
    if (clientFd_ < 0) {
        return;
    }
    std::size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = ::send(clientFd_, line.data() + off, line.size() - off, 0);
        if (n <= 0) {
            break;
        }
        off += static_cast<std::size_t>(n);
    }
}

void Drive::ok(const json& id, json body) {
    body["ok"] = true;
    reply(id, std::move(body));
}

void Drive::fail(const json& id, const std::string& err, json extra) {
    extra["ok"] = false;
    extra["err"] = err;
    reply(id, std::move(extra));
}

// --- per frame ------------------------------------------------------------------------------

bool Drive::wantsFrames() const {
    if (listenFd_ < 0) {
        return false;
    }
    if (!steps_.empty() || wait_ || settleFrames_ > 0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mu_);
    return !pending_.empty();
}

void Drive::beforeFrame() {
    if (listenFd_ < 0) {
        return;
    }
    ++frame_;
    if (g_registryFrame != frame_) {
        registry().clear();
        g_registryFrame = frame_;
    }
    if (steps_.empty()) {
        if (settleFrames_ > 0) {
            --settleFrames_;
        }
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    Step& s = steps_.front();
    switch (s.kind) {
    case Step::MousePos:
        // The SDL backend re-reads the OS cursor every frame while the window is focused and no
        // button is down, which would undo an injected position: move the real cursor too.
        if (window_) {
            SDL_WarpMouseInWindow(window_, s.x, s.y);
        }
        io.AddMousePosEvent(s.x, s.y);
        break;
    case Step::MouseDown:
        io.AddMouseButtonEvent(s.button, true);
        break;
    case Step::MouseUp:
        io.AddMouseButtonEvent(s.button, false);
        break;
    case Step::KeyDown:
        io.AddKeyEvent(s.key, true);
        break;
    case Step::KeyUp:
        io.AddKeyEvent(s.key, false);
        break;
    case Step::Text:
        io.AddInputCharactersUTF8(s.text.c_str());
        break;
    case Step::Wheel:
        io.AddMouseWheelEvent(s.x, s.y);
        break;
    case Step::Hold:
        if (--s.holdFrames > 0) {
            return; // stay on this step
        }
        break;
    }
    steps_.pop_front();
    settleFrames_ = 4;
}

void Drive::queueClick(ImVec2 at, int button, int holdFrames) {
    steps_.push_back(Step::pos(at.x, at.y));
    steps_.push_back(Step::hold(1));
    steps_.push_back(Step::down(button));
    steps_.push_back(Step::hold(std::max(1, holdFrames))); // a popup opens on the release, one frame after the press
    steps_.push_back(Step::up(button));
}

void Drive::queueKey(const std::string& name, const std::vector<std::string>& mods, std::string& err) {
    const ImGuiKey key = keyFromName(name);
    if (key == ImGuiKey_None) {
        err = "unknown key: " + name;
        return;
    }
    std::vector<ImGuiKey> modKeys;
    for (const std::string& m : mods) {
        const ImGuiKey mk = modFromName(m);
        if (mk == ImGuiKey_None) {
            err = "unknown modifier: " + m;
            return;
        }
        modKeys.push_back(mk);
    }
    for (ImGuiKey mk : modKeys) {
        steps_.push_back(Step::keyDown(mk));
    }
    steps_.push_back(Step::keyDown(key));
    steps_.push_back(Step::keyUp(key));
    for (auto it = modKeys.rbegin(); it != modKeys.rend(); ++it) {
        steps_.push_back(Step::keyUp(*it));
    }
}

std::vector<std::string> Drive::targetNames() const {
    std::vector<std::string> names;
    for (const auto& [k, v] : registry()) {
        names.push_back(k);
    }
    names.emplace_back("input");
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiWindow* w : g.Windows) {
        if (w->Active && (w->DockIsActive || w->ParentWindow == nullptr) && w->Name[0] != '#' &&
            std::string(w->Name).rfind("Debug", 0) != 0 && std::string(w->Name).rfind("WindowOverViewport", 0) != 0) {
            names.push_back("pane:" + lower(w->Name));
        }
    }
    return names;
}

std::optional<ImRect> Drive::resolveTarget(const std::string& target, FrameContext& ctx, std::string& why) const {
    const std::string t = lower(target);
    // x,y
    if (const auto comma = t.find(','); comma != std::string::npos && !t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-')) {
        const float x = std::strtof(t.c_str(), nullptr);
        const float y = std::strtof(t.c_str() + comma + 1, nullptr);
        return ImRect(ImVec2(x, y), ImVec2(x, y));
    }
    if (t == "input") {
        const ImVec2 a = ctx.panes.terminal.inputMin();
        const ImVec2 b = ctx.panes.terminal.inputMax();
        if (b.x > a.x && b.y > a.y) {
            return ImRect(a, b);
        }
        const ImVec2 c = ctx.panes.input.fieldMin();
        const ImVec2 d = ctx.panes.input.fieldMax();
        if (d.x > c.x && d.y > c.y) {
            return ImRect(c, d);
        }
        why = "no input line on screen";
        return std::nullopt;
    }
    if (t.rfind("pane:", 0) == 0) {
        const std::string name = t.substr(5);
        ImGuiWindow* w = ImGui::FindWindowByName(upper(name).c_str());
        if (!w) {
            w = ImGui::FindWindowByName(name.c_str());
        }
        if (!w || !w->Active) {
            why = "no window named " + name;
            return std::nullopt;
        }
        return ImRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y));
    }
    if (const auto it = registry().find(t); it != registry().end()) {
        return it->second;
    }
    // principal:<id-prefix> also works by id
    if (t.rfind("principal:", 0) == 0) {
        const std::string who = t.substr(10);
        for (const auto& [k, r] : registry()) {
            if (k.rfind("principal:", 0) == 0 && k.size() > 10 && who.size() >= 4 && k.compare(10, who.size(), who) == 0) {
                return r;
            }
        }
    }
    why = "unknown target " + target;
    return std::nullopt;
}

// The highest seq of any line in the current terminal's transcript (0 when empty): a reply that
// arrives after a wait began has a seq above it, while an older Message whose text is read late
// does not.
static Seq transcriptMaxSeq(FrameContext& ctx) {
    const std::string who = conversation(ctx).current();
    if (who.empty()) {
        return 0;
    }
    const std::string self = ctx.rt.session.principal().value_or("");
    Seq m = 0;
    for (const auto& l : chat::assembleTranscript(ctx.rt.principals.messages(), ctx.rt.chat.messages(), who, self,
                                                  [&](const std::string& id) { return ctx.rt.principals.nameOf(id); })) {
        m = std::max(m, l.seq);
    }
    return m;
}

// A reply that answers a line this listener sent after the wait began (in-reply-to one of our
// Messages with seq > after); when no such line exists yet, any new reply block counts, so a
// `wait reply` issued before typing still works.
static bool replyAfter(FrameContext& ctx, Seq after) {
    const std::string who = conversation(ctx).current();
    if (who.empty()) {
        return false;
    }
    const std::string self = ctx.rt.session.principal().value_or("");
    const auto lines = chat::assembleTranscript(ctx.rt.principals.messages(), ctx.rt.chat.messages(), who, self,
                                                [&](const std::string& id) { return ctx.rt.principals.nameOf(id); });
    std::vector<std::string> ours;
    for (const auto& l : lines) {
        if (l.mine && l.seq > after) {
            ours.push_back(l.message);
        }
    }
    for (const auto& l : lines) {
        if (l.mine || !l.ready || l.text.empty() || l.seq <= after) {
            continue;
        }
        if (ours.empty()) {
            return true;
        }
        if (l.inReplyTo && std::find(ours.begin(), ours.end(), *l.inReplyTo) != ours.end()) {
            return true;
        }
    }
    return false;
}

std::size_t Drive::replyCount(FrameContext& ctx) const {
    const std::string who = conversation(ctx).current();
    if (who.empty()) {
        return 0;
    }
    const std::string self = ctx.rt.session.principal().value_or("");
    const auto lines = chat::assembleTranscript(ctx.rt.principals.messages(), ctx.rt.chat.messages(), who, self,
                                                [&](const std::string& id) { return ctx.rt.principals.nameOf(id); });
    std::size_t n = 0;
    for (const auto& l : lines) {
        if (!l.mine && l.ready && !l.text.empty()) {
            ++n;
        }
    }
    return n;
}

bool Drive::waitSatisfied(Wait& w, FrameContext& ctx) const {
    if (w.until == "attached") {
        return ctx.rt.session.attached();
    }
    if (w.until == "settle") {
        return --w.framesLeft <= 0 && steps_.empty();
    }
    if (w.until == "route") {
        return ctx.rt.chat.routeState() == chat::RouteState::Found;
    }
    const std::string who = conversation(ctx).current();
    if (w.until == "idle") {
        if (who.empty()) {
            return false;
        }
        const model::PrincipalInfo* info = ctx.rt.principals.get(who);
        return ctx.rt.evaluations.running(conversation(ctx).currentWorker()) == nullptr && (!info || info->loopIteration() == 0);
    }
    if (w.until == "reply") {
        return replyAfter(ctx, w.seqBefore);
    }
    if (w.until == "text") {
        if (who.empty()) {
            return false;
        }
        const std::string self = ctx.rt.session.principal().value_or("");
        const auto lines = chat::assembleTranscript(ctx.rt.principals.messages(), ctx.rt.chat.messages(), who, self,
                                                    [&](const std::string& id) { return ctx.rt.principals.nameOf(id); });
        for (const auto& l : lines) {
            if (l.text.find(w.text) != std::string::npos) {
                return true;
            }
        }
        for (const auto& n : ctx.rt.chat.notes()) {
            if (n.text.find(w.text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    return true;
}

json Drive::stateJson(FrameContext& ctx) const {
    json s;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(ctx.gpu.window, &ww, &wh);
    s["window"] = json{{"w", ww}, {"h", wh}, {"scale", ctx.uiScale}};
    s["attached"] = ctx.rt.session.attached();
    {
        const WorldInfo& w = ctx.rt.session.world();
        s["world"] = json{{"id", w.id}, {"name", w.name}, {"url", w.url}, {"head", w.head},
                          {"fresh_reason", ctx.rt.session.freshReason()}};
    }
    s["head"] = ctx.rt.session.head();
    s["modeline"] = ctx.panes.modelineText;
    s["last_jump"] = ctx.panes.terminal.lastJump();
    s["modeline_error"] = ctx.panes.modelineError;
    s["session_principal"] = ctx.rt.session.principal().value_or("");
    {
        const auto [ready, failed] = ui::typesetCounts();
        json shown = json::array();
        for (const ui::TypesetSeen& t : ui::typesetsSeen()) {
            shown.push_back(json{{"source", t.source}, {"reason", t.reason}, {"failed", t.failed}, {"pages", t.pages},
                                 {"page", t.page}, {"log", t.log}, {"left", t.left}, {"top", t.top}, {"right", t.right},
                                 {"bottom", t.bottom}, {"note_top", t.noteTop}, {"note_bottom", t.noteBottom}});
        }
        s["typeset"] = json{{"ready", ready}, {"failed", failed}, {"shown", shown}};
    }
    s["route"] = json{{"state", chat::name(ctx.rt.chat.routeState())}, {"id", ctx.rt.chat.route()}};
    const std::string who = conversation(ctx).current();
    s["principal"] = who;
    s["principal_name"] = who.empty() ? "" : ctx.rt.principals.nameOf(who);
    {
        // whom the current tab's line addresses (model::recipientsFor): ids and names
        const std::string self = ctx.rt.session.principal().value_or("");
        std::vector<std::string> routes;
        if (const model::PrincipalInfo* pi = who.empty() ? nullptr : ctx.rt.principals.get(who)) {
            routes = pi->routes();
        }
        if (routes.empty() && who == self && ctx.rt.chat.routeState() == chat::RouteState::Found) {
            routes.push_back(ctx.rt.chat.route());
        }
        json to = json::array();
        if (!routes.empty() && !who.empty()) {
            for (const std::string& id : model::recipientsFor(ctx.rt.principals, who, routes.front(), self)) {
                to.push_back(json{{"id", id}, {"name", ctx.rt.principals.nameOf(id)}});
            }
        }
        s["to"] = to;
        s["to_route"] = routes.empty() ? "" : routes.front();
    }
    // windows
    json wins = json::array();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiWindow* w : g.Windows) {
        if (w->Active && (w->DockIsActive || w->ParentWindow == nullptr) && w->Name[0] != '#' &&
            std::string(w->Name).rfind("Debug", 0) != 0 && std::string(w->Name).rfind("WindowOverViewport", 0) != 0) {
            wins.push_back(json{{"name", w->Name}, {"x", w->Pos.x}, {"y", w->Pos.y}, {"w", w->Size.x}, {"h", w->Size.y},
                                {"visible", !w->Hidden}, {"front", !w->Hidden}});
        }
    }
#if !LISTENER_PHONE
    if (ctx.panes.mux.panesInView()) { // the host's own area, not a window of its own
        wins.push_back(json{{"name", "PANES"}, {"visible", true}, {"front", true}});
    }
#endif
    s["windows"] = wins;
    s["nav_window"] = g.NavWindow ? g.NavWindow->Name : "";
    s["hovered_window"] = g.HoveredWindow ? g.HoveredWindow->Name : "";
    s["active_id"] = g.ActiveId;
    s["mouse"] = json{{"x", g.IO.MousePos.x}, {"y", g.IO.MousePos.y}, {"down", g.IO.MouseDown[0]}};
    s["app_focus_lost"] = g.IO.AppFocusLost;
    {
        const BootInfo& b = ctx.rt.boot;
        s["boot"] = json{{"mode", b.mode == BootInfo::Mode::Snapshot ? "snapshot"
                                  : b.mode == BootInfo::Mode::Replay ? "replay"
                                                                      : "pending"},
                         {"seq", b.seq},
                         {"objects", b.objects},
                         {"pages", b.pages},
                         {"ms", b.ms},
                         {"fold_ready_ms", b.foldReadyMs},
                         {"head_at_attach", b.headAtAttach},
                         {"reason", b.reason},
                         {"line", b.line()}};
        s["fold_applied"] = json{{"principals", ctx.rt.principals.applied()}, {"evaluations", ctx.rt.evaluations.applied()},
                                 {"principal_count", ctx.rt.principals.size()}};
        const auto until = ctx.rt.backoffUntil();
        s["link"] = json{{"reconnects", ctx.rt.reconnects()},
                         {"backoff_ms", until ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    *until - std::chrono::steady_clock::now())
                                                    .count()
                                              : 0},
                         {"stop_reason", ctx.rt.stopReason()},
                         {"attach_failures", ctx.rt.session.attachFailures()},
                         {"attach_error", ctx.rt.session.attachError().first},
                         {"frames_in", ctx.rt.client.framesIn()},
                         {"frames_out", ctx.rt.client.framesOut()},
                         {"socket_opens", ctx.rt.client.opens()},
                         {"attaches", ctx.rt.session.attaches()},
                         {"conn_state", name(ctx.rt.client.state())}};
    }
    json targets = json::object();
    for (const auto& [k, r] : registry()) {
        targets[k] = rectJson(r);
    }
    {
        std::string why;
        if (auto r = resolveTarget("input", ctx, why)) {
            targets["input"] = rectJson(*r);
        }
    }
    s["targets"] = targets;
    // transcript
    json lines = json::array();
    if (!who.empty()) {
        const std::string self = ctx.rt.session.principal().value_or("");
        for (const auto& l : chat::assembleTranscript(ctx.rt.principals.messages(), ctx.rt.chat.messages(), who, self,
                                                      [&](const std::string& id) { return ctx.rt.principals.nameOf(id); })) {
            json j{{"message", l.message}, {"from", l.from},     {"from_name", l.fromName}, {"at", l.at},
                   {"seq", l.seq},         {"text", l.text},     {"ready", l.ready},        {"mine", l.mine}};
            if (l.via) {
                j["via"] = *l.via;
                if (const model::EvaluationInfo* e = ctx.rt.evaluations.get(*l.via)) {
                    j["cost"] = json{{"state", e->state},       {"tokens_in", e->tokensIn}, {"tokens_out", e->tokensOut},
                                     {"ctx_bytes", e->ctxBytes}, {"ctx_in", e->ctxIn},      {"ctx_out", e->ctxOut},
                                     {"ctx_objects", e->ctxObjects}};
                }
            }
            if (l.inReplyTo) {
                j["in_reply_to"] = *l.inReplyTo;
            }
            if (!l.failure.empty()) {
                j["failure"] = l.failure;
            }
            if (!l.kind.empty()) {
                j["kind"] = l.kind;
            }
            if (!l.tags.empty()) {
                j["tags"] = l.tags;
            }
            if (!l.thread.empty()) {
                j["thread"] = l.thread;
            }
            if (!l.askedBy.empty()) {
                j["asked_by"] = l.askedBy;
                j["asked_by_name"] = l.askedByName;
            }
            if (l.hop > 0) {
                j["hop"] = l.hop;
            }
            {
                chat::ScanContext sc;
                sc.fold = &ctx.rt.principals;
                sc.object = who;
                json links = json::array();
                for (const chat::Symbol& sym : ctx.rt.symbols.scan(l.text, sc)) {
                    links.push_back(json{{"text", sym.display}, {"object", sym.object},
                                         {"kind", static_cast<int>(sym.kind)}, {"unresolved", sym.kind == chat::SymbolKind::Unresolved}});
                }
                if (!links.empty()) {
                    j["links"] = std::move(links);
                }
            }
            if (!l.contradiction.empty()) {
                j["contradiction"] = l.contradiction;
            }
            if (l.ready && !l.mine) { // the kernel record a loop's reply ends with (world pass 13)
                const chat::KernelRecord rec = chat::splitKernelRecord(l.text);
                if (rec.present) {
                    json runs = json::array();
                    for (const chat::RunRecord& r : rec.runs) {
                        json rj{{"cmd", r.cmd}, {"tail", r.tail}};
                        if (r.exit) { rj["exit"] = *r.exit; }
                        if (r.durationS) { rj["duration_s"] = *r.durationS; }
                        runs.push_back(std::move(rj));
                    }
                    j["runs"] = std::move(runs);
                }
            }
            // the view behind the line (sprint 1 L10): message/view or a tagged view Object
            {
                const chat::ViewSpec* spec = nullptr;
                if (!l.view.empty()) {
                    spec = ctx.rt.chat.views().get(l.view);
                }
                for (std::size_t ti = 0; !spec && ti < l.tags.size(); ++ti) {
                    spec = ctx.rt.chat.views().get(l.tags[ti]);
                }
                if (spec) {
                    j["view"] = json{{"id", spec->id}, {"kind", spec->kind}, {"title", spec->title},
                                     {"about", spec->about}, {"truncated", spec->truncated}, {"ready", spec->dataReady}};
                } else if (!l.view.empty()) {
                    j["view"] = json{{"id", l.view}, {"ready", false}};
                }
            }
            lines.push_back(std::move(j));
        }
        s["replies"] = replyCount(ctx);
        // evaluation state of the current Principal
        json ev;
        const model::PrincipalInfo* info = ctx.rt.principals.get(who);
        const model::EvaluationInfo* cur = ctx.rt.evaluations.latest(conversation(ctx).currentWorker());
        ev["iteration"] = info ? info->loopIteration() : 0;
        ev["stalled"] = info ? info->loopStalled() : "";
        if (cur) {
            ev["id"] = cur->id;
            ev["state"] = cur->state;
            ev["tokens_in"] = cur->tokensIn;
            ev["tokens_out"] = cur->tokensOut;
            ev["ctx_bytes"] = cur->ctxBytes;
            ev["partial_chars"] = cur->partial.size();
            ev["thinking_chars"] = cur->thinkingChars;
        }
        const double nowEpoch = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        ev["progress"] = model::progressLine(ctx.rt.evaluations, who, ctx.rt.principals.nameOf(who),
                                             info ? info->loopIteration() : 0, nowEpoch);
        ev["status"] = model::statusLine(ctx.rt.evaluations, who);
        ev["totals"] = model::totalsLine(ctx.rt.evaluations.totals(who));
        s["evaluation"] = ev;
        // the Principal's Fields with their hint facets, values clipped (the smoke asserts on hints)
        json fields = json::object();
        if (info) {
            for (const auto& [name, f] : info->fields) {
                std::string value = f.value.is_string() ? f.value.get<std::string>() : f.value.dump();
                if (value.size() > 300) {
                    value.resize(300);
                }
                json fj{{"value", value}, {"type", f.type}, {"by", f.by}, {"seq", f.seq}, {"reader", f.reader}};
                if (const model::FieldState* h = info->field(name + "/hint"); h && h->value.is_string()) {
                    fj["hint"] = h->value.get<std::string>();
                }
                if (const model::FieldState* c = info->field(name + "/comment"); c && c->value.is_array()) {
                    fj["comments"] = c->value.size();
                }
                fields[name] = std::move(fj);
            }
        }
        s["fields"] = fields;
    }
    s["transcript"] = lines;
    {
        json q = json::array();
        const model::PrincipalInfo* worker = ctx.rt.principals.get(conversation(ctx).currentWorker());
        if (const model::FieldState* f = worker ? worker->field("loop/queue") : nullptr) {
            q = f->value;
        }
        s["queue"] = q;
    }
    {
        const ui::TerminalPane::AttachStatus as = conversation(ctx).attachStatus();
        s["attach"] = json{{"stage", as.stage}, {"err", as.err}, {"done", as.done}};
        s["graph_overlay"] = ctx.panes.graph.hasOverlay();
    }
    // the focused text field (ImGui's own state; the terminal's line while it has focus)
    {
        const ImGuiInputTextState& st = g.InputTextState;
        s["input_focused"] = st.ID != 0 && g.ActiveId == st.ID;
        if (st.ID != 0 && g.ActiveId == st.ID && st.TextA.Data) {
            s["input_text"] = std::string(st.TextA.Data, static_cast<std::size_t>(std::max(0, st.TextLen)));
        } else {
            s["input_text"] = ctx.panes.input.buffer();
        }
    }
    json notes = json::array();
    const auto& all = ctx.rt.chat.notes();
    for (std::size_t i = all.size() > 8 ? all.size() - 8 : 0; i < all.size(); ++i) {
        notes.push_back(json{{"kind", static_cast<int>(all[i].kind)}, {"chip", all[i].chip}, {"text", all[i].text}});
    }
    s["notes"] = notes;
    s["scratch"] = ctx.panes.scratch.count(); // a count, never its text
    {
        // The newest events the world sent, so a test can assert an op reached the log.
        json log = json::array();
        // Reads and resolutions are noise here: one tab click is dozens of them.
        const auto& rows = ctx.rt.events.rows();
        std::size_t kept = 0;
        for (std::size_t i = rows.size(); i-- > 0 && kept < 200;) {
            if (rows[i].kind == "field-read" || rows[i].kind == "resolved") {
                continue;
            }
            log.insert(log.begin(), json{{"seq", rows[i].seq}, {"kind", rows[i].kind}, {"stream", rows[i].stream}, {"body", rows[i].body}});
            ++kept;
        }
        s["head_seq"] = rows.empty() ? 0 : rows.back().seq;
        s["history_rows"] = rows.size();
        s["oldest_seq"] = rows.empty() ? 0 : rows.front().seq;
        s["history_complete"] = ctx.rt.events.complete();
        s["log"] = std::move(log);
    }
    s["carrier_lost"] = ctx.rt.chat.carrierLost();
    s["principals"] = ctx.rt.principals.size();
    // pending approvals (sprint 1 L5) and the world's named actions (L6)
    {
        json tasks = json::array();
        for (const model::TaskInfo* t : ctx.rt.tasks.pending()) {
            json approvers = json::array();
            for (const std::string& a : t->approvers) {
                approvers.push_back(ctx.rt.principals.nameOf(a));
            }
            tasks.push_back(json{{"id", t->id},
                                 {"short", chat::idPrefix(t->id)},
                                 {"state", t->state},
                                 {"summary", t->summary},
                                 {"requested_by", ctx.rt.principals.nameOf(t->requestedBy)},
                                 {"approvers", approvers},
                                 {"expires_at", t->expiresAt},
                                 {"op", t->op}});
        }
        s["tasks"] = tasks;
        {
            const model::WorldMap& map = ctx.panes.graph.map();
            json actors = json::array();
            for (const model::MapActor& a : map.actors) {
                json ports = json::array();
                for (const model::MapPort& p : a.ports) {
                    ports.push_back(p.name);
                }
                actors.push_back(json{{"name", a.name}, {"kind", model::name(a.kind)}, {"x", a.x}, {"y", a.y}, {"w", a.w}, {"h", a.h},
                                      {"depth", a.depth}, {"running", a.loop == model::LoopState::Running}, {"unsafe", a.unsafe}, {"unread", a.unread},
                                      {"waiting", a.waiting}, {"can_decide", a.canDecide}, {"ports", ports}});
            }
            json links = json::array();
            for (const model::MapLink& l : map.links) {
                links.push_back(json{{"from", ctx.rt.principals.nameOf(l.from)}, {"to", ctx.rt.principals.nameOf(l.to)},
                                     {"kind", l.kind == model::LinkKind::Route ? "route" : "model"},
                                     {"traffic", l.traffic}, {"messages", l.messages}});
            }
            s["map"] = json{{"actors", actors}, {"links", links}, {"stacked", map.stacked}, {"pulsing", ctx.panes.graph.pulsing()},
                            {"status", ctx.panes.graph.statusLines()}};
            json rows = json::array();
            for (const chat::Notification& n : ctx.rt.notifications.rows()) {
                rows.push_back(json{{"id", n.id}, {"level", n.level}, {"text", n.text}, {"read", n.read},
                                    {"from", ctx.rt.principals.nameOf(n.from)}});
            }
            s["notices"] = json{{"unread", ctx.rt.notifications.unread()}, {"rows", rows}, {"os", ctx.panes.osNotices}};
            json unread = json::object();
            for (const auto& [id, count] : ctx.panes.unread) {
                unread[ctx.rt.principals.nameOf(id)] = count;
            }
            s["unread"] = unread;
            json marks = json::object();
            for (const auto& [id, mark] : ctx.panes.activity.marks) {
                marks[ctx.rt.principals.nameOf(id)] = model::word(mark);
            }
            s["activity"] = json{{"marks", marks}, {"rollup", model::rollup(ctx.panes.activity)}};
            s["signin"] = json{{"busy", ui::chatGptSignIn().busy()}, {"url", ui::chatGptSignIn().url()},
                               {"status", ui::chatGptSignIn().status()}};
            s["manual"] = ctx.panes.manual.driveState();
            s["open_file"] = ctx.panes.openFile.driveState();
        }
        s["tasks_total"] = ctx.rt.tasks.size();
        json acts = json::array();
        for (const model::ActionInfo* a : ctx.rt.actions.ordered()) {
            acts.push_back(json{{"name", a->name}, {"gated", a->gated}, {"help", a->help}, {"schema", a->schema}});
        }
        s["actions"] = acts;
        json models = json::array();
        for (const model::PrincipalInfo* e : ctx.rt.principals.ordered()) {
            if (e->cls != "model") {
                continue;
            }
            auto str = [&](const char* name) {
                const model::FieldState* f = e->field(name);
                return f && f->value.is_string() ? f->value.get<std::string>() : std::string();
            };
            auto num = [&](const char* name) -> json {
                const model::FieldState* f = e->field(name);
                return f && f->value.is_number() ? f->value : json(nullptr);
            };
            models.push_back(json{{"id", e->id}, {"name", ctx.rt.principals.nameOf(e->id)}, {"label", str("model/label")},
                                   {"model", str("model/id")}, {"status", str("model/status")},
                                   {"price_in", num("model/price-in")}, {"price_out", num("model/price-out")}});
        }
        s["models"] = models;
        {
            json hits = json::array();
            for (const chat::SearchHit& h : ctx.panes.history.hits()) {
                hits.push_back(json{{"object", h.object}, {"name", h.name}, {"field", h.field}, {"kind", h.kind},
                                    {"seq", h.seq}, {"snippet", h.snippet}, {"score", h.score}});
            }
            s["search"] = json{{"open", ctx.panes.history.searchOpen()}, {"text", ctx.panes.history.query()},
                               {"local", ctx.panes.history.hitsLocal()}, {"status", ctx.panes.history.searchStatus()},
                               {"hits", hits}};
        }
        const model::PrincipalInfo* cur = who.empty() ? nullptr : ctx.rt.principals.get(who);
        s["model"] = cur && !cur->models().empty() ? cur->models().front() : std::string();
    }
    s["frame"] = frame_;
    return s;
}

bool Drive::handle(const Command& c, FrameContext& ctx) {
    try {
        return handleInner(c, ctx);
    } catch (const std::exception& e) {
        fail(c.id, std::string("command failed: ") + e.what());
        return false;
    }
}

bool Drive::handleInner(const Command& c, FrameContext& ctx) {
    const json& a = c.args;
    const std::string& cmd = c.cmd;
    if (cmd == "ping") {
        ok(c.id, json{{"pong", true}, {"frame", frame_}});
    } else if (cmd == "quit") {
        ok(c.id);
        return true;
    } else if (cmd == "state") {
        ok(c.id, json{{"state", stateJson(ctx)}});
    } else if (cmd == "targets") {
        ok(c.id, json{{"targets", targetNames()}});
    } else if (cmd == "clipboard") {
        if (a.contains("text")) {
            platform::setClipboard(a.value("text", std::string()));
        }
        ok(c.id, json{{"text", platform::clipboard()}});
    } else if (cmd == "label-case") {
        ctx.panes.labelCaseRequest = a.value("case", std::string());
        ok(c.id);
    } else if (cmd == "panes") {
        ok(c.id, json{{"panes", ctx.panes.mux.describe()}});
    } else if (cmd == "pane-open") {
        modules::FileRef ref{a.value("target", std::string()), a.value("path", std::string()),
                             a.value("kind", std::string()), a.value("new-pane", false)};
        ref.field = a.value("field", std::string());
        ctx.panes.mux.open(ref);
        ok(c.id);
    } else if (cmd == "pane-act") {
        // What a remote client or a model does: the same action, by name.
        ctx.panes.mux.act(a.value("action", std::string()), a.value("args", json::object()), [](const Response&) {});
        ok(c.id);
    } else if (ctx.panes.scratch.active() && (cmd == "screenshot" || cmd == "type" || cmd == "key" || cmd == "drop")) {
        fail(c.id, "ui-only: off the record is not driven");
    } else if (cmd == "screenshot") {
        const std::string path = a.value("path", "");
        if (path.empty()) {
            fail(c.id, "screenshot needs path");
        } else if (!app::dumpFrame(ctx.gpu, path.c_str())) {
            fail(c.id, "capture failed");
        } else {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(ctx.gpu.window, &ww, &wh);
            ok(c.id, json{{"path", path}, {"w", ww}, {"h", wh}});
        }
    } else if (cmd == "type") {
        const std::string text = a.value("text", "");
        if (text.empty()) {
            fail(c.id, "type needs text");
        } else {
            steps_.push_back(Step::typed(text));
            ok(c.id, json{{"chars", text.size()}});
        }
    } else if (cmd == "key") {
        std::string err;
        std::vector<std::string> mods;
        if (a.contains("mods") && a["mods"].is_array()) {
            for (const auto& m : a["mods"]) {
                mods.push_back(m.get<std::string>());
            }
        }
        queueKey(a.value("key", ""), mods, err);
        if (!err.empty()) {
            fail(c.id, err);
        } else {
            ok(c.id);
        }
    } else if (cmd == "click" || cmd == "scroll" || cmd == "drag") {
        std::string why;
        std::optional<ImRect> rect;
        if (a.contains("x") && a.contains("y")) {
            const float x = a["x"].get<float>(), y = a["y"].get<float>();
            rect = ImRect(ImVec2(x, y), ImVec2(x, y));
        } else {
            rect = resolveTarget(a.value(cmd == "drag" ? "from" : "target", ""), ctx, why);
        }
        if (!rect) {
            fail(c.id, why, json{{"targets", targetNames()}});
        } else if (cmd == "click") {
            const ImVec2 at = rect->GetCenter();
            const int times = std::max(1, a.value("times", 1));
            for (int i = 0; i < times; ++i) {
                const bool shift = a.value("shift", false);
                if (shift) {
                    steps_.push_back(Step::keyDown(ImGuiMod_Shift));
                }
                queueClick(at, a.value("button", std::string()) == "right" ? 1 : 0, a.value("hold", 0));
                if (shift) {
                    steps_.push_back(Step::keyUp(ImGuiMod_Shift));
                }
            }
            ok(c.id, json{{"at", json{{"x", at.x}, {"y", at.y}}}, {"rect", rectJson(*rect)}});
        } else if (cmd == "scroll") {
            const ImVec2 at = rect->GetCenter();
            steps_.push_back(Step::pos(at.x, at.y));
            steps_.push_back(Step::hold(1));
            steps_.push_back(Step::wheel(a.value("dx", 0.0f), a.value("dy", 0.0f)));
            ok(c.id, json{{"at", json{{"x", at.x}, {"y", at.y}}}});
        } else {
            std::optional<ImRect> to = a.contains("to") ? resolveTarget(a["to"].get<std::string>(), ctx, why)
                                                        : std::optional<ImRect>();
            if (a.contains("to_x") && a.contains("to_y")) {
                const float x = a["to_x"].get<float>(), y = a["to_y"].get<float>();
                to = ImRect(ImVec2(x, y), ImVec2(x, y));
            }
            if (!to) {
                fail(c.id, why.empty() ? "drag needs to or to_x/to_y" : why);
            } else {
                const ImVec2 f = rect->GetCenter(), t = to->GetCenter();
                steps_.push_back(Step::pos(f.x, f.y));
                steps_.push_back(Step::hold(1));
                steps_.push_back(Step::down());
                steps_.push_back(Step::hold(1));
                for (int i = 1; i <= 6; ++i) {
                    const float k = static_cast<float>(i) / 6.0f;
                    steps_.push_back(Step::pos(f.x + (t.x - f.x) * k, f.y + (t.y - f.y) * k));
                }
                steps_.push_back(Step::hold(1));
                steps_.push_back(Step::up());
                ok(c.id, json{{"from", json{{"x", f.x}, {"y", f.y}}}, {"to", json{{"x", t.x}, {"y", t.y}}}});
            }
        }
    } else if (cmd == "drop") {
        // A file / folder / text drop onto the current terminal, as Finder would do it.
        const std::string path = a.value("path", "");
        const std::string text = a.value("text", "");
        if (path.empty() && text.empty()) {
            fail(c.id, "drop needs path or text");
        } else {
            ImVec2 at(0.0f, 0.0f);
            std::string why;
            if (auto r = resolveTarget(a.value("target", "input"), ctx, why)) {
                at = r->GetCenter();
            }
            ctx.panes.terminal.drop(path, text, at);
            ok(c.id, json{{"path", path}, {"text_chars", text.size()}, {"at", json{{"x", at.x}, {"y", at.y}}}});
        }
    } else if (cmd == "wait") {
        Wait w;
        w.id = c.id;
        w.until = a.value("until", "settle");
        w.text = a.value("text", "");
        w.started = Clock::now();
        w.deadline = w.started + std::chrono::milliseconds(a.value("timeout_ms", 30000));
        w.repliesBefore = replyCount(ctx);
        w.seqBefore = transcriptMaxSeq(ctx);
        w.framesLeft = a.value("frames", 3);
        static const std::vector<std::string> kinds{"idle", "attached", "reply", "text", "settle", "route"};
        if (std::find(kinds.begin(), kinds.end(), w.until) == kinds.end()) {
            fail(c.id, "wait until must be one of idle|attached|reply|text|settle|route");
        } else if (w.until == "text" && w.text.empty()) {
            fail(c.id, "wait until text needs text");
        } else {
            wait_ = w;
        }
    } else if (cmd == "search") {
        const std::string text = a.value("text", "");
        ctx.panes.history.openSearch();
        ctx.panes.history.search(text);
        ok(c.id, json{{"text", text}});
    } else if (cmd == "attach") {
        AttachRequest r;
        r.url = a.value("url", "");
        r.token = a.value("token", "");
        if (r.token.empty() && a.contains("token_file")) {
            r.token = readToken(a["token_file"].get<std::string>());
        }
        if (r.url.empty() || r.token.empty()) {
            fail(c.id, "attach needs url and token or token_file");
        } else {
            attach_ = r;
            ok(c.id);
        }
    } else if (cmd == "focus") {
        ctx.panes.terminal.requestFocus();
        ok(c.id);
    } else if (cmd == "minimize") {
        // answered first: a minimized window draws no frame to answer from
        ok(c.id, json{{"minimized", true}});
        SDL_MinimizeWindow(window_);
    } else if (cmd == "resize") {
        // The window at a size (UX tests at phone width on the desktop build).
        const int w = a.value("w", 0), h = a.value("h", 0);
        if (w < 200 || h < 200) {
            fail(c.id, "resize needs w and h of at least 200");
        } else {
            SDL_SetWindowSize(ctx.gpu.window, w, h);
            ok(c.id, json{{"w", w}, {"h", h}});
        }
    } else {
        fail(c.id, "unknown cmd " + cmd,
             json{{"commands", {"ping", "state", "targets", "screenshot", "type", "key", "click", "drag", "scroll", "drop",
                                "wait", "attach", "focus", "resize", "search", "quit"}}});
    }
    return false;
}

bool Drive::afterFrame(FrameContext& ctx) {
    if (listenFd_ < 0) {
        return false;
    }
    // an active wait resolves before anything queued behind it runs
    if (wait_) {
        Wait& w = *wait_;
        if (waitSatisfied(w, ctx)) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - w.started).count();
            ok(w.id, json{{"until", w.until}, {"elapsed_ms", ms}});
            wait_.reset();
        } else if (Clock::now() >= w.deadline) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - w.started).count();
            fail(w.id, "timeout", json{{"until", w.until}, {"elapsed_ms", ms}, {"state", stateJson(ctx)}});
            wait_.reset();
        } else {
            return false;
        }
    }
    // input steps still pending: let them play out before observing or queueing more
    if (!steps_.empty()) {
        return false;
    }
    for (;;) {
        std::optional<Command> next;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!pending_.empty()) {
                next = std::move(pending_.front());
                pending_.pop_front();
            }
        }
        if (!next) {
            return false;
        }
        if (handle(*next, ctx)) {
            return true;
        }
        if (wait_ || !steps_.empty()) {
            return false; // the rest waits for the next frame(s)
        }
    }
}

std::optional<AttachRequest> Drive::takeAttach() {
    std::optional<AttachRequest> r;
    r.swap(attach_);
    return r;
}

} // namespace ke::drive
