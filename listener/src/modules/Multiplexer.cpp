#include "modules/Multiplexer.hpp"

#include "app/Runtime.hpp"
#include "drive/Commands.hpp"
#include "modules/ModuleUi.hpp"
#include "modules/Registry.hpp"
#include "platform/Platform.hpp"
#include "ui/Fonts.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/Theme.hpp"
#include "ui/UnsafeView.hpp"
#include "util/Base64.hpp"

#include <algorithm>
#include <cmath>
#include <imgui_internal.h>

namespace ke::modules {

namespace {

constexpr float kGap = 3.0f;
constexpr double kMarkSeconds = 3.0;

// A divider sits at origin + ratio * extent. It does not carry its origin, so read it back.
float dividerOffset(const model::LayoutDivider& d) { return d.ratio * d.extent; }

std::string keyOf(const std::string& kind, const std::string& target, const std::string& path) {
    return kind + "|" + target + "|" + path;
}

std::string baseName(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

ImVec2 vec(float x, float y) { return ImVec2(x, y); }

} // namespace

std::string Multiplexer::heldError(bool panesInView) const {
    if (!openError_.empty()) {
        return openError_;
    }
    for (const auto& [id, instance] : live_) {
        if (panesInView && id == focus_) {
            continue; // on screen, in its own status
        }
        if (instance.module) {
            if (std::string held = instance.module->heldError(); !held.empty()) {
                const model::PaneInfo* p = info(id);
                return (p == nullptr ? id.substr(0, 8) : p->shown()) + ": " + held;
            }
        }
    }
    return "";
}

void Multiplexer::bind(Runtime* rt) {
    if (rt_ != rt) {
        rt_ = rt;
        loaded_ = false;
        live_.clear();
        local_.clear();
    }
    if (rt_ != nullptr) {
        rt_->session.paneData = [this](const PaneData& d) { routeData(d); };
    }
}

void Multiplexer::attached() { loaded_ = false; }

void Multiplexer::act(const std::string& action, nlohmann::json args, Reply reply) {
    if (rt_ != nullptr) {
        rt_->chat.actNamed(action, std::move(args), std::move(reply));
    }
}

void Multiplexer::copy(const std::string& text) { platform::setClipboard(text); }
std::string Multiplexer::paste() { return platform::clipboard(); }
void Multiplexer::wantKeyboard(bool on) {
    const ImVec2 pos = ImGui::GetIO().MousePos;
    platform::showKeyboard(on, pos.x, pos.y, 1.0f, 1.0f);
}
std::string Multiplexer::nameOf(const std::string& object) const {
    return rt_ != nullptr ? rt_->principals.nameOf(object) : object;
}

std::string Multiplexer::self() const { return rt_ != nullptr ? rt_->session.principal().value_or("") : std::string(); }

namespace {

// One chunk, then the next from where it ended, until the world says there is no more.
void readChunk(chat::Chat& chat, const std::string& pane, const std::string& field, std::vector<unsigned char> have,
               std::function<void(std::vector<unsigned char>, std::string)> done) {
    json args = {{"pane", pane}, {"what", "bytes"}, {"field", field}, {"offset", have.size()}};
    chat.actNamed("pane.read", args, [&chat, pane, field, have = std::move(have), done = std::move(done)](const Response& r) mutable {
        if (!r.ok) {
            done({}, errLine(r));
            return;
        }
        const json& result = r.result.is_object() && r.result.contains("result") ? r.result["result"] : r.result;
        std::vector<unsigned char> chunk;
        if (!result.is_object() || !result.contains("base64") || !util::base64Decode(result["base64"].get<std::string>(), chunk)) {
            done({}, "the world sent no bytes");
            return;
        }
        have.insert(have.end(), chunk.begin(), chunk.end());
        if (result.value("more", false) && !chunk.empty()) {
            readChunk(chat, pane, field, std::move(have), std::move(done));
        } else {
            done(std::move(have), "");
        }
    });
}

} // namespace

void Multiplexer::readBytes(const std::string& pane, const std::string& field,
                            std::function<void(std::vector<unsigned char>, std::string)> done) {
    if (rt_ != nullptr) {
        readChunk(rt_->chat, pane, field, {}, std::move(done));
    }
}

void Multiplexer::inspect(const std::string& object) {
    if (onInspect) {
        onInspect(object);
    }
}

const model::PaneInfo* Multiplexer::info(const std::string& pane) const {
    if (rt_ != nullptr) {
        if (const model::PaneInfo* p = rt_->panes.get(pane)) {
            return p;
        }
    }
    auto it = local_.find(pane);
    return it == local_.end() ? nullptr : &it->second;
}

Multiplexer::Instance* Multiplexer::ensure(const std::string& pane) {
    auto it = live_.find(pane);
    if (it != live_.end()) {
        return &it->second;
    }
    const model::PaneInfo* p = info(pane);
    if (p == nullptr) {
        return nullptr;
    }
    const ModuleSpec* spec = specFor(p->kind);
    if (spec == nullptr || spec->make == nullptr) {
        return nullptr;
    }
    Instance inst;
    inst.module = spec->make(*this, *p);
    if (auto k = pendingKeys_.find(pane); k != pendingKeys_.end()) {
        inst.key = k->second;
    } else {
        inst.key = keyOf(p->kind, p->target, p->field);
    }
    inst.lastInput = p->lastInput.seq;
    inst.module->restore(*p);
    return &live_.emplace(pane, std::move(inst)).first->second;
}

// The layout from the person's Field, once per attach; then the fold's changes into the modules.
void Multiplexer::sync() {
    if (rt_ == nullptr || !rt_->session.attached()) {
        return;
    }
    const std::string self = rt_->session.principal().value_or("");
    if (!loaded_ && !self.empty()) {
        if (const model::PrincipalInfo* me = rt_->principals.get(self)) {
            loaded_ = true;
            if (auto f = me->fields.find("view/panes"); f != me->fields.end()) {
                if (!desktop_) {
                    layout_ = model::PaneLayout::fromJson(f->second.value);
                }
                lastSaved_ = f->second.value.dump();
                if (f->second.value.is_object() && f->second.value.contains("terminal-tabs") && restoreTerminalTab) {
                    for (const nlohmann::json& id : f->second.value["terminal-tabs"]) {
                        if (id.is_string()) {
                            restoreTerminalTab(id.get<std::string>());
                        }
                    }
                }
            }
            if (desktop_) {
                loadDesktop(*me);
            }
        }
    }
    for (auto& [id, inst] : live_) {
        const model::PaneInfo* p = rt_->panes.get(id);
        if (p == nullptr) {
            continue;
        }
        inst.module->restore(*p);
        if (p->lastInput.seq > inst.lastInput) {
            inst.lastInput = p->lastInput.seq;
            if (p->lastInput.who != self) {
                const std::string who = nameOf(p->lastInput.who);
                inst.module->driven(p->lastInput, who);
                if (p->lastInput.model) {
                    inst.markedAt = ImGui::GetTime();
                    inst.markText = who + ": " + p->lastInput.text;
                }
            }
        }
    }
    const std::vector<std::string> tabs = terminalTabs ? terminalTabs() : std::vector<std::string>();
    if (tabs != savedTabs_) {
        savedTabs_ = tabs;
        saveSoon();
    }
    if (dirty_ && ImGui::GetTime() - dirtyAt_ > 0.6) {
        dirty_ = false;
        // A phone reads `view/panes`, panes only; the desktop's whole window is `view/layout`.
        model::PaneLayout panesOnly = layout_;
        if (desktop_) {
            std::vector<std::string> keep;
            for (const std::string& id : layout_.panes()) {
                if (view(id) == nullptr) {
                    keep.push_back(id);
                }
            }
            panesOnly.retain(keep);
            for (const model::LayoutFloat& f : layout_.floats()) {
                const std::string id = f.pane;
                panesOnly.dock(id, {});
                if (view(id) != nullptr) {
                    panesOnly.remove(id);
                }
            }
        }
        nlohmann::json j = panesOnly.toJson();
        if (!j.is_object()) {
            j = nlohmann::json::object();
        }
        j["terminal-tabs"] = tabs;
        if (const std::string s = j.dump(); s != lastSaved_ && !self.empty()) {
            lastSaved_ = s;
            rt_->chat.setField(self, "view/panes", "map", j, "both", [](const Response&) {});
        }
        if (desktop_ && !self.empty()) {
            saveDesktop(self, j);
        }
    }
}

void Multiplexer::saveSoon() {
    dirty_ = true;
    dirtyAt_ = ImGui::GetTime();
}

void Multiplexer::routeData(const PaneData& d) {
    auto it = live_.find(d.pane);
    if (it != live_.end()) {
        it->second.module->data(d);
    }
}

void Multiplexer::openPane(const std::string& kind, const std::string& target, const std::string& title) {
    open(FileRef{target, "", kind, false});
    (void)title;
}

void Multiplexer::takeLine(const std::string& pane, Instance& inst) {
    if (auto it = pendingLine_.find(pane); it != pendingLine_.end()) {
        inst.module->goToLine(it->second);
        pendingLine_.erase(it);
    }
}

void Multiplexer::open(const FileRef& file) {
    const std::string kind = !file.kind.empty() ? file.kind : !file.path.empty() ? kindForPath(file.path) : "terminal";
    const std::string key = keyOf(kind, file.target, !file.message.empty() ? file.message + "#" + std::to_string(file.index)
                                                     : file.field.empty()  ? file.path
                                                                           : file.field);
    for (const auto& [id, inst] : live_) {
        if (inst.key == key && layout_.contains(id)) {
            layout_.activate(id);
            focus_ = id;
            bringForward_ = id;
            takeKeyboard_ = id;
            if (file.line > 0) {
                pendingLine_[id] = file.line;
            }
            saveSoon();
            return;
        }
    }
    // With no mount the world resolves the path and picks the kind; a mount names its own pane.
    const bool bare = file.target.empty() && !file.path.empty();
    nlohmann::json args = bare ? nlohmann::json{{"path", file.path}} : nlohmann::json{{"kind", kind}, {"target", file.target}};
    if (!file.field.empty()) {
        args = nlohmann::json{{"kind", kind}, {"object", file.target}, {"field", file.field}};
    } else if (bare && !file.kind.empty()) {
        args["as"] = file.kind;
    } else if (!file.path.empty() && !bare) {
        args["path"] = file.path;
    }
    const bool byObject = !file.fileObject.empty();
    if (byObject) {
        args = nlohmann::json{{"name", file.fileObject}};
    }
    if (!file.path.empty()) {
        args["title"] = baseName(file.path);
    }
    if (!file.message.empty()) {
        args = nlohmann::json{{"message", file.message}, {"index", file.index}};
    }
    if (!file.tex.empty()) {
        args = nlohmann::json{{"source", file.tex}, {"title", "typeset"}};
    }
    const bool split = file.newPane;
    const std::string title = !file.field.empty() ? nameOf(file.target) + " \xc2\xb7 " + file.field
                              : file.path.empty() ? std::string()
                                                  : baseName(file.path);
    act(!file.tex.empty() ? "tex.open" : !file.message.empty() ? "image.open" : bare || byObject ? "file.open" : "pane.open", std::move(args), [this, kind, key, file, split, title](const Response& r) {
        if (!r.ok) {
            openError_ = errLine(r);
            bringForward_ = "!";
            return;
        }
        openError_.clear();
        const std::string id = r.result.is_object() ? r.result.value("pane", std::string()) : std::string();
        if (id.empty()) {
            return;
        }
        model::PaneInfo p;
        p.id = id;
        p.kind = r.result.value("kind", kind);
        p.target = file.target;
        p.title = title.empty() ? kind : title;
        p.field = file.field;
        p.state = "open";
        local_[id] = p;
        pendingKeys_[id] = key;
        takeKeyboard_ = id;
        if (file.line > 0) {
            pendingLine_[id] = file.line;
        }
        if (split && !focus_.empty()) {
            layout_.place(id, focus_, model::Edge::Right);
        } else {
            layout_.add(id, nearNew());
        }
        focus_ = id;
        bringForward_ = id;
        saveSoon();
    });
}

bool Multiplexer::pointerOverView(const std::string& view) const {
    // Called between windows, where ImGui's own hover test clips to nothing: plain arithmetic.
    const auto it = bodies_.find(view);
    if (it == bodies_.end()) {
        return false;
    }
    const ImVec2 m = ImGui::GetIO().MousePos;
    const Rect& b = it->second;
    return m.x >= b.x && m.x < b.x + b.w && m.y >= b.y && m.y < b.y + b.h;
}

std::string Multiplexer::takeBringForward() {
    std::string out;
    out.swap(bringForward_);
    return out;
}

void Multiplexer::requestClose(const std::string& pane) {
    if (view(pane) != nullptr) {
        hideView(pane);
        return;
    }
    auto it = live_.find(pane);
    if (it != live_.end() && !it->second.module->closeWarning().empty()) {
        confirmClose_ = pane;
        return;
    }
    close(pane);
}

// Closing a tab ends the pane. Closing the client does not: that is what keeps a pane running.
void Multiplexer::close(const std::string& pane) {
    layout_.remove(pane);
    live_.erase(pane);
    local_.erase(pane);
    if (focus_ == pane) {
        const std::vector<std::string> left = layout_.panes();
        focus_ = left.empty() ? std::string() : left.back();
    }
    saveSoon();
    act("pane.close", {{"pane", pane}}, [](const Response&) {});
}

void Multiplexer::attachPane(const std::string& pane, const std::string& kind) {
    if (!layout_.contains(pane)) {
        if (info(pane) == nullptr) {
            model::PaneInfo p;
            p.id = pane;
            p.kind = kind;
            p.title = kind;
            p.state = "open";
            local_[pane] = p;
        }
        layout_.add(pane, nearNew());
    }
    layout_.activate(pane);
    focus_ = pane;
    bringForward_ = pane;
    saveSoon();
}

void Multiplexer::closeFocused() {
    if (!focus_.empty() && view(focus_) == nullptr) {
        requestClose(focus_);
    }
}

void Multiplexer::focusKeys(model::LayoutRect area) {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (physicalCtrl() || io.KeyCtrl) {
        const struct {
            ImGuiKey key;
            model::Direction dir;
        } kDirs[] = {{ImGuiKey_LeftArrow, model::Direction::Left},
                     {ImGuiKey_RightArrow, model::Direction::Right},
                     {ImGuiKey_UpArrow, model::Direction::Up},
                     {ImGuiKey_DownArrow, model::Direction::Down}};
        for (const auto& d : kDirs) {
            if (physicalCtrl() && ImGui::IsKeyPressed(d.key, false) && !focus_.empty()) {
                if (auto next = layout_.neighbour(focus_, d.dir, area)) {
                    focus_ = *next;
                    layout_.activate(focus_);
                    ImGui::GetIO().ClearInputKeys(); // the module must not see this chord
                    return;
                }
            }
        }
    }
    if (shortcut(ImGuiKey_W)) {
        closeFocused();
    }
}

void Multiplexer::drawAttachLine(const std::string& pane, model::LayoutRect at) {
    const model::PaneInfo* p = info(pane);
    if (p == nullptr) {
        return;
    }
    const std::string self = rt_->session.principal().value_or("");
    std::string line;
    for (const std::string& a : p->attached) {
        line += (line.empty() ? "attached: " : ", ") + (a == self ? std::string("you") : nameOf(a));
    }
    if (line.empty()) {
        line = "attached: you";
    }
    bool driver = false;
    if (!p->lastInput.who.empty() && p->lastInput.model) {
        line += "  driven by " + nameOf(p->lastInput.who);
        driver = true;
    }
    if (!p->owner.empty()) {
        line += "  owner " + nameOf(p->owner);
    }
    ImGui::GetWindowDrawList()->AddText(vec(at.x + 4.0f, at.y), driver ? ui::Ink::inspected : ui::Ink::secondary, line.c_str());
}

void Multiplexer::dropZones(const model::LayoutLeaf& leaf) {
    if (dragging_.empty() || leaf.tabs.empty()) {
        return;
    }
    const std::string dragged = dragging_;
    const model::LayoutRect r = leaf.rect;
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (m.x < r.x || m.y < r.y || m.x > r.x + r.w || m.y > r.y + r.h) {
        return;
    }
    const float third = 0.28f;
    model::Edge edge = model::Edge::Tabs;
    ImVec2 a = vec(r.x, r.y), b = vec(r.x + r.w, r.y + r.h);
    if (m.x < r.x + r.w * third) {
        edge = model::Edge::Left;
        b.x = r.x + r.w / 2;
    } else if (m.x > r.x + r.w * (1 - third)) {
        edge = model::Edge::Right;
        a.x = r.x + r.w / 2;
    } else if (m.y < r.y + r.h * third) {
        edge = model::Edge::Top;
        b.y = r.y + r.h / 2;
    } else if (m.y > r.y + r.h * (1 - third)) {
        edge = model::Edge::Bottom;
        a.y = r.y + r.h / 2;
    }
    ImDrawList* dl = desktop_ ? ImGui::GetForegroundDrawList() : ImGui::GetWindowDrawList(); // a view's window sits on the leaf
    dl->AddRectFilled(a, b, ui::Surface::hover);
    dl->AddRect(a, b, ui::Ink::nav);
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (!(edge == model::Edge::Tabs && std::find(leaf.tabs.begin(), leaf.tabs.end(), dragged) != leaf.tabs.end())) {
            layout_.move(dragged, *leaf.activePane() == dragged && leaf.tabs.size() > 1 ? leaf.tabs.front() : *leaf.activePane(), edge);
            focus_ = dragged;
            saveSoon();
        }
    }
}

void Multiplexer::drawLeaf(const model::LayoutLeaf& leaf, int index) {
    ImGui::PushID(index);
    std::vector<ui::TabItem> tabs;
    for (const std::string& id : leaf.tabs) {
        const model::PaneInfo* p = info(id);
        ui::TabItem t;
        t.id = id;
        if (const ViewSpec* v = view(id)) {
            t.label = ui::upper(v->label); // a view's name is a label; a pane's name below is a name, as typed
            t.target = v->target;
            t.pinned = true;
            tabs.push_back(std::move(t));
            continue;
        }
        const ModuleSpec* spec = p != nullptr ? specFor(p->kind) : nullptr;
        t.glyph = spec != nullptr ? spec->glyph : "?";
        t.label = p == nullptr ? id.substr(0, 8) : p->shown();
        t.alarm = ui::isUnsafe(id); // shared with an agent
        if (auto it = live_.find(id); it != live_.end()) {
            t.dirty = !it->second.module->closeWarning().empty();
            t.attention = ImGui::GetTime() - it->second.markedAt < kMarkSeconds;
        }
        tabs.push_back(std::move(t));
    }
    const model::LayoutRect r = leaf.rect;
    ImGui::SetCursorScreenPos(vec(r.x, r.y));
    const ui::TabBarResult tb = ui::tabBar("leaf", tabs, leaf.active, r.w, tabStates_[leaf.tabs.empty() ? std::string() : leaf.tabs.front()]);
    if (tb.picked >= 0) {
        focus_ = leaf.tabs[static_cast<std::size_t>(tb.picked)];
        layout_.activate(focus_);
        saveSoon();
    }
    if (tb.closed >= 0) {
        requestClose(leaf.tabs[static_cast<std::size_t>(tb.closed)]);
        ImGui::PopID();
        return;
    }
    if (tb.menu >= 0) {
        menuPane_ = leaf.tabs[static_cast<std::size_t>(tb.menu)];
        ImGui::OpenPopup("pane-tab-menu");
    }
    if (ImGui::BeginPopup("pane-tab-menu")) {
        if (view(menuPane_) == nullptr) {
            const bool rename = ui::menuItem("RENAME...");
            drive::hit("menu:rename-pane", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (rename) {
                ui::LinkRequest q;
                q.kind = ui::LinkRequest::Label;
                q.object = menuPane_;
                ui::symbolUi().ask(std::move(q));
            }
        }
        if (desktop_) {
            const bool floated = ui::menuItem("FLOAT");
            drive::hit("menu:float-pane", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (floated) {
                const ImVec2 size(std::floor(r.w * 0.8f), std::floor(r.h * 0.8f));
                layout_.floatOut(menuPane_, model::LayoutRect{r.x + 24.0f, r.y + 24.0f, std::max(size.x, 360.0f), std::max(size.y, 240.0f)});
                saveSoon();
            }
        }
        const model::PaneInfo* shareable = info(menuPane_);
        if (shareable != nullptr && shareable->kind == "terminal") {
            const bool share = ui::menuItem("SHARE WITH...");
            drive::hit("menu:share-pane", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (share) {
                ui::askShareTerminal(menuPane_);
            }
        }
        if (ui::menuItem("CLOSE")) {
            requestClose(menuPane_);
        }
        ImGui::EndPopup();
    }
    if (tb.dragged >= 0) {
        dragging_ = leaf.tabs[static_cast<std::size_t>(tb.dragged)];
        draggingLabel_ = tabs[static_cast<std::size_t>(tb.dragged)].label;
    }
    const float tabH = ui::tabBarHeight() + 2.0f;
    const float lineH = ImGui::GetTextLineHeight() + 2.0f;
    const std::string* activeId = leaf.activePane();
    if (activeId != nullptr) {
        const std::string active = *activeId;
        if (desktop_ && hoveredRect(Rect{r.x, r.y, r.w, r.h}) &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            focus_ = active;
        }
        if (view(active) != nullptr) {
            const Rect body{r.x, r.y + tabH, r.w, std::max(1.0f, r.h - tabH)};
            bodies_[active] = body;
            if (active == "view:panes") {
                drawPanesView(body);
            }
            dropZones(leaf);
            ImGui::PopID();
            return;
        }
        paneInFront_ = true;
        Instance* inst = ensure(active);
        const bool focused = active == focus_;
        Rect body{r.x, r.y + tabH, r.w, r.h - tabH - lineH};
        if (focused) {
            ImGui::GetWindowDrawList()->AddRect(vec(r.x, r.y + tabH - 1.0f), vec(r.x + r.w, r.y + r.h), ui::Ink::nav);
        }
        if (inst != nullptr) {
            ImGui::PushID(active.c_str());
            const ImGuiIO& io = ImGui::GetIO();
            const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
            if (hoveredRect(Rect{r.x, r.y, r.w, r.h}) &&
                (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                 (followMouse && moved && !ImGui::IsAnyMouseDown() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)))) {
                focus_ = active;
                if (followMouse && moved && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                    ImGui::SetWindowFocus();
                }
                if (followMouse && moved && io.WantTextInput) {
                    ImGui::ClearActiveID(); // a field elsewhere still holds the keys
                }
            }
            // A pane just opened takes the keys from whatever had them: nothing should need a click first.
            if (takeKeyboard_ == active) {
                takeKeyboard_.clear();
                ImGui::SetWindowFocus();
                ImGui::ClearActiveID();
            }
            const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (focused && windowFocused && !ImGui::GetIO().WantTextInput) {
                inst->module->keyboard();
            }
            takeLine(active, *inst);
            ui::pushMono();
            inst->module->draw(body, focused && windowFocused);
            ui::popMono();
            if (ImGui::GetTime() - inst->markedAt < kMarkSeconds) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRect(topLeft(body), bottomRight(body), ui::Ink::inspected);
                const std::string mark = "MODEL " + inst->markText;
                const ImVec2 size = ImGui::CalcTextSize(mark.c_str());
                dl->AddRectFilled(vec(body.x + 2.0f, body.y + 2.0f), vec(body.x + 8.0f + size.x, body.y + 4.0f + size.y), ui::Surface::selection);
                dl->AddText(vec(body.x + 5.0f, body.y + 3.0f), ui::Ink::inspected, mark.c_str());
            }
            ImGui::PopID();
            drive::hit("panebody:" + (tabs[static_cast<std::size_t>(leaf.active)].label), topLeft(body), bottomRight(body));
        }
        drawAttachLine(active, model::LayoutRect{r.x, r.y + r.h - lineH + 1.0f, r.w, lineH});
    }
    dropZones(leaf);
    ImGui::PopID();
}

void Multiplexer::drawNewPaneMenu() {
    if (ui::plusAtRight("new-pane", "add a pane: a terminal or a file manager on one of your Adapters")) {
        ImGui::OpenPopup("newpane");
    }
    if (ImGui::BeginPopup("newpane")) {
        if (rt_ != nullptr) {
            for (const model::PrincipalInfo* a : rt_->principals.ordered()) {
                if (a->cls != "adapter") {
                    continue;
                }
                // What an Adapter binds says what it can be: a shell a terminal, a mount a file manager.
                const auto binds = a->fields.find("adapter/binds");
                const std::string kind = binds != a->fields.end() && binds->second.value.is_string()
                                             ? binds->second.value.get<std::string>()
                                             : std::string();
                const std::string name = nameOf(a->id);
                if (kind == "host" || kind == "shell") {
                    const std::string label = "TERMINAL ON " + name;
                    if (ui::menuItem(label.c_str())) {
                        open(FileRef{a->id, "", "terminal", false});
                    }
                } else if (kind == "filesystem") {
                    const std::string files = "FILES ON " + name;
                    if (ui::menuItem(files.c_str())) {
                        open(FileRef{a->id, "", "files", false});
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
}

void Multiplexer::drawTree(model::LayoutRect area) {
    focusKeys(area);
    const std::vector<model::LayoutLeaf> leaves = layout_.leaves(area, kGap);
    int i = 0;
    for (const model::LayoutLeaf& leaf : leaves) {
        drawLeaf(leaf, i++);
    }
    drawDividers(area);
    if (!dragging_.empty()) {
        // A tab being moved: its name at the pointer, over everything, until the button comes up.
        const ImVec2 m = ImGui::GetIO().MousePos;
        ImGui::GetForegroundDrawList()->AddText(vec(m.x + 12.0f, m.y + 8.0f), ui::Ink::nav, draggingLabel_.c_str());
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            dragging_.clear();
        }
    }
}

void Multiplexer::drawDividers(model::LayoutRect area) {
    for (const model::LayoutDivider& d : layout_.dividers(area, kGap)) {
        const float pad = 2.0f;
        const ImVec2 a(d.rect.x - pad, d.rect.y - pad), b(d.rect.x + d.rect.w + pad, d.rect.y + d.rect.h + pad);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(d.path.c_str());
        ImGui::InvisibleButton("div", vec(std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y)));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) {
            ImGui::SetMouseCursor(d.horizontal ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            ImGui::GetWindowDrawList()->AddRectFilled(a, b, ui::Ink::nav);
        }
        drive::hit("divider:" + d.path, a, b);
        if (ImGui::IsItemActive()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float origin = d.horizontal ? d.rect.x + d.rect.w / 2 - dividerOffset(d) : d.rect.y + d.rect.h / 2 - dividerOffset(d);
            const float pos = d.horizontal ? m.x : m.y;
            layout_.setRatio(d.path, std::clamp((pos - origin) / std::max(1.0f, d.extent), 0.1f, 0.9f));
            saveSoon();
        }
        ImGui::PopID();
    }
}

void Multiplexer::drawCloseConfirm() {
    if (!confirmClose_.empty()) {
        ImGui::OpenPopup("confirm-close");
    }
    if (ImGui::BeginPopupModal("confirm-close", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto it = live_.find(confirmClose_);
        ui::text(ui::Ink::inspected, it == live_.end() ? std::string("close?") : it->second.module->closeWarning());
        ui::dialogGap();
        if (ui::bracketButtonNamed("CLOSE ANYWAY", "close-anyway")) {
            close(confirmClose_);
            confirmClose_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::bracketButtonNamed("KEEP", "close-keep") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            confirmClose_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Multiplexer::draw() {
    sync();
    ImGui::PushID("mux");
    drawNewPaneMenu();
    if (!openError_.empty()) {
        ImGui::SameLine();
        ui::text(ui::Ink::fatal, openError_);
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const model::LayoutRect area{at.x, at.y, avail.x, avail.y};
    if (layout_.empty()) {
        ui::dimText("no panes open: right-click a shell or directory object, or +");
        ImGui::PopID();
        return;
    }
    drawTree(area);
    drawCloseConfirm();
    ImGui::PopID();
}

// ---------------------------------------------------------------- the desktop

void Multiplexer::setViews(std::vector<ViewSpec> views, std::function<model::PaneLayout(bool)> defaults) {
    views_ = std::move(views);
    defaults_ = std::move(defaults);
    desktop_ = true;
}

const ViewSpec* Multiplexer::view(const std::string& id) const {
    for (const ViewSpec& v : views_) {
        if (v.id == id) {
            return &v;
        }
    }
    return nullptr;
}

std::string Multiplexer::nearNew() const {
    return desktop_ && layout_.contains("view:panes") ? std::string("view:panes") : focus_;
}

bool Multiplexer::viewOpen(const std::string& v) const { return layout_.contains(v) || layout_.floating(v); }

bool Multiplexer::viewInFront(const std::string& v) const { return bodies_.count(v) != 0 || layout_.floating(v); }

void Multiplexer::showView(const std::string& v, const std::string& near, model::Edge edge) {
    // Asked for by name, a floated view comes home: this is how to get back anything that went astray.
    if (layout_.floating(v)) {
        const std::string by = !near.empty() && layout_.contains(near) ? near : layout_.contains("view:chat") ? "view:chat" : focus_;
        layout_.dock(v, by);
        saveSoon();
    }
    if (!viewOpen(v)) {
        const std::string by = !near.empty() && layout_.contains(near) ? near : layout_.contains("view:chat") ? "view:chat" : focus_;
        layout_.place(v, by, edge);
        saveSoon();
    }
    if (layout_.contains(v)) {
        layout_.activate(v);
    }
    focus_ = v;
    bringForward_ = v;
}

void Multiplexer::showViewAtEdge(const std::string& v, model::Edge edge, float share) {
    if (!viewOpen(v)) {
        const auto it = lastShare_.find(v);
        layout_.wrap(v, edge, it != lastShare_.end() ? it->second : share);
        saveSoon();
    }
    showView(v);
}

void Multiplexer::hideView(const std::string& v) {
    if (const auto it = bodies_.find(v); it != bodies_.end()) {
        lastShare_[v] = (it->second.w + kGap) / std::max(1.0f, areaW_);
    }
    layout_.remove(v);
    if (layout_.floating(v)) {
        layout_.dock(v, {});
        layout_.remove(v);
    }
    bodies_.erase(v);
    if (focus_ == v) {
        const std::vector<std::string> left = layout_.panes();
        focus_ = left.empty() ? std::string() : left.back();
    }
    saveSoon();
}

void Multiplexer::resetLayout() {
    model::PaneLayout fresh = defaults_(narrow_);
    for (const std::string& id : layout_.panes()) {
        if (view(id) == nullptr) {
            fresh.add(id, "view:panes");
        }
    }
    for (const model::LayoutFloat& f : layout_.floats()) {
        if (view(f.pane) == nullptr) {
            fresh.add(f.pane, "view:panes");
        }
    }
    layout_ = std::move(fresh);
    folded_ = false;
    focus_.clear();
    saveSoon();
}

model::PaneLayout Multiplexer::saved(bool narrow) const {
    const char* key = narrow ? "narrow" : "wide";
    if (buckets_.is_object() && buckets_.contains(key)) {
        model::PaneLayout l = model::PaneLayout::fromJson(buckets_[key]);
        if (!l.empty() || !l.floats().empty()) {
            return l;
        }
    }
    // the other screen's arrangement is a better start than none, once it has been made
    return defaults_(narrow);
}

// One person's window from the Field: this screen's arrangement, with the panes they hold today.
void Multiplexer::loadDesktop(const model::PrincipalInfo& me) {
    std::vector<std::string> held;
    if (auto f = me.fields.find("view/panes"); f != me.fields.end()) {
        held = model::PaneLayout::fromJson(f->second.value).panes();
    }
    if (auto f = me.fields.find("view/layout"); f != me.fields.end() && f->second.value.is_object()) {
        buckets_ = f->second.value;
        lastSavedLayout_ = buckets_.dump();
    }
    model::PaneLayout l = saved(narrow_);
    const bool heldKnown = me.fields.count("view/panes") != 0;
    for (const std::string& id : l.panes()) {
        if (view(id) == nullptr && heldKnown && std::find(held.begin(), held.end(), id) == held.end()) {
            l.remove(id); // closed from another client
        }
    }
    l.remove("view:inspect"); // inspectors are a moment's look: none survives a restart
    if (l.floating("view:inspect")) {
        l.dock("view:inspect", {});
        l.remove("view:inspect");
    }
    for (const std::string& id : held) {
        if (!l.contains(id) && !l.floating(id)) {
            l.add(id, l.contains("view:panes") ? "view:panes" : "");
        }
    }
    layout_ = std::move(l);
    folded_ = false;
}

void Multiplexer::switchClass(bool narrow) {
    buckets_[narrow_ ? "narrow" : "wide"] = layout_.toJson();
    std::vector<std::string> mine;
    for (const std::string& id : layout_.panes()) {
        if (view(id) == nullptr) {
            mine.push_back(id);
        }
    }
    narrow_ = narrow;
    model::PaneLayout l = saved(narrow);
    for (const std::string& id : mine) {
        if (!l.contains(id) && !l.floating(id)) {
            l.add(id, l.contains("view:panes") ? "view:panes" : "");
        }
    }
    layout_ = std::move(l);
    folded_ = false;
    saveSoon();
}

void Multiplexer::saveDesktop(const std::string& self, const nlohmann::json&) {
    model::PaneLayout shown = layout_;
    if (folded_) {
        if (auto side = layout_.sideOf(foldView_)) {
            shown.setRatio(side->path, foldRatio_); // a fold is the moment's; the height comes back
        }
    }
    buckets_[narrow_ ? "narrow" : "wide"] = shown.toJson();
    if (const std::string s = buckets_.dump(); s != lastSavedLayout_) {
        lastSavedLayout_ = s;
        rt_->chat.setField(self, "view/layout", "map", buckets_, "both", [](const Response&) {});
    }
}

void Multiplexer::fold(const std::string& v, float strip) {
    foldView_ = v;
    foldStrip_ = strip;
}

void Multiplexer::applyFold(model::LayoutRect area) {
    const auto side = layout_.sideOf(foldView_);
    if (!side || side->horizontal || !side->alone) {
        folded_ = false;
        return;
    }
    if (foldStrip_ <= 0.0f) {
        if (folded_) {
            layout_.setRatio(side->path, foldRatio_);
            folded_ = false;
        }
        return;
    }
    float extent = area.h;
    for (const model::LayoutDivider& d : layout_.dividers(area, kGap)) {
        if (d.path == side->path) {
            extent = d.extent;
        }
    }
    if (!folded_) {
        foldRatio_ = layout_.ratioAt(side->path);
        folded_ = true;
    }
    const float share = foldStrip_ / std::max(1.0f, extent);
    layout_.setRatio(side->path, side->second ? 1.0f - share : share);
}

void Multiplexer::drawPanesView(const Rect& body) {
    ImGui::SetCursorScreenPos(topLeft(body));
    if (ImGui::BeginChild("panes-view", ImVec2(body.w, body.h), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ui::dimText("terminals, files and viewers open as tabs here: [+], or right-click a shell or directory object");
        drawNewPaneMenu();
        if (!openError_.empty()) {
            ui::text(ui::Ink::fatal, openError_);
        }
    }
    ImGui::EndChild();
}

void Multiplexer::drawFloats(model::LayoutRect area) {
    const std::vector<model::LayoutFloat> floats = layout_.floats();
    const std::string home = layout_.contains("view:panes") ? "view:panes" : layout_.contains("view:chat") ? "view:chat" : focus_;
    for (const model::LayoutFloat& f : floats) {
        // The other views have windows of their own, placed below. PANES has none: it lives in its tab.
        if (view(f.pane) != nullptr && f.pane != "view:panes") {
            continue;
        }
        ImGui::SetNextWindowPos(vec(f.rect.x, f.rect.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(vec(f.rect.w, f.rect.h), ImGuiCond_Appearing);
        bool open = true;
        const model::PaneInfo* p = info(f.pane);
        const std::string title = (f.pane == "view:panes" ? std::string("panes") : p == nullptr ? f.pane.substr(0, 8) : p->shown()) +
                                  "###float" + f.pane;
        if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            if (f.pane == "view:panes") {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                drawPanesView(Rect{at.x, at.y, avail.x, avail.y});
            } else if (Instance* inst = ensure(f.pane)) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
                if (focused && !ImGui::GetIO().WantTextInput) {
                    inst->module->keyboard();
                }
                takeLine(f.pane, *inst);
                ImGui::PushID(f.pane.c_str());
                ui::pushMono();
                inst->module->draw(Rect{at.x, at.y, avail.x, avail.y}, focused);
                ui::popMono();
                ImGui::PopID();
            }
            const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            if (pos.x != f.rect.x || pos.y != f.rect.y || size.x != f.rect.w || size.y != f.rect.h) {
                layout_.setFloatRect(f.pane, model::LayoutRect{pos.x, pos.y, size.x, size.y});
                saveSoon();
            }
        }
        ImGui::End();
        if (!open) {
            layout_.dock(f.pane, home);
            saveSoon();
        }
    }
    (void)area;
}

void Multiplexer::placeViews() {
    const std::string home = layout_.contains("view:chat") ? "view:chat" : layout_.contains("view:panes") ? "view:panes" : focus_;
    for (const ViewSpec& v : views_) {
        ui::PanePlace at;
        for (const model::LayoutFloat& f : layout_.floats()) {
            if (f.pane == v.id) {
                if (ui::takePaneFloatClosed(v.window)) {
                    layout_.dock(v.id, home);
                    saveSoon();
                    break;
                }
                const ImVec4 now = ui::paneFloatRect(v.window);
                if (now.z > 0.0f && (now.x != f.rect.x || now.y != f.rect.y || now.z != f.rect.w || now.w != f.rect.h)) {
                    layout_.setFloatRect(v.id, model::LayoutRect{now.x, now.y, now.z, now.w});
                    saveSoon();
                }
                at.shown = true;
                at.floating = true;
                at.pos = vec(f.rect.x, f.rect.y);
                at.size = vec(f.rect.w, f.rect.h);
                break;
            }
        }
        if (const auto it = bodies_.find(v.id); it != bodies_.end() && !at.floating) {
            at.shown = true;
            at.pos = topLeft(it->second);
            at.size = vec(it->second.w, it->second.h);
        }
        ui::placePane(v.window, at);
    }
}

void Multiplexer::drawDesktop(model::LayoutRect area, bool narrow) {
    if (!laid_) {
        laid_ = true;
        narrow_ = narrow;
        layout_ = defaults_(narrow);
    } else if (narrow != narrow_) {
        switchClass(narrow);
    }
    sync();
    ImGui::PushID("mux");
    applyFold(area);
    areaW_ = area.w;
    bodies_.clear();
    paneInFront_ = false;
    if (layout_.empty()) {
        ImGui::SetCursorScreenPos(vec(area.x + 12.0f, area.y + 12.0f));
        ui::dimText("every view is closed: VIEW brings them back, or VIEW, RESET LAYOUT");
    } else {
        drawTree(area);
    }
    drawFloats(area);
    drawCloseConfirm();
    placeViews();
    ImGui::PopID();
}

nlohmann::json Multiplexer::describe() const {
    nlohmann::json panes = nlohmann::json::array();
    for (const auto& [id, inst] : live_) {
        nlohmann::json j = inst.module->describe();
        j["pane"] = id;
        j["key"] = inst.key;
        if (const model::PaneInfo* p = info(id)) {
            j["title"] = p->title;
            j["state"] = p->state;
            j["field"] = p->field;
        }
        panes.push_back(std::move(j));
    }
    nlohmann::json front = nlohmann::json::array();
    for (const auto& [id, body] : bodies_) {
        front.push_back({{"view", id}, {"x", body.x}, {"y", body.y}, {"w", body.w}, {"h", body.h}});
    }
    return {{"focus", focus_}, {"layout", layout_.toJson()}, {"live", panes}, {"error", openError_}, {"front", front}};
}

} // namespace ke::modules
