#include "ui/SymbolUi.hpp"

#include "modules/Registry.hpp"

#include "chat/MessageCache.hpp"
#include "drive/Commands.hpp"
#include "model/ObjectTree.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <cctype>
#include <imgui_internal.h>

namespace ke::ui {

namespace {

constexpr const char* kMenu = "orbis-symbol-menu";

std::string upperOf(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

} // namespace

SymbolUi& symbolUi() {
    static SymbolUi ui;
    return ui;
}

json actionPrefill(const std::string& action, const std::string& object, const std::string& cls, bool& enabled) {
    const bool principal = cls == "principal";
    const bool adapter = cls == "adapter";
    json args = json::object();
    enabled = true;
    if (action == "workspace.mount") {
        enabled = principal;
        args["principal"] = object;
    } else if (action == "task.run") {
        if (adapter) {
            args["adapter"] = object;
        }
    } else if (action == "route.create") {
        enabled = principal;
        args["members"] = json::array({object});
    } else if (action == "route.join") {
        enabled = principal;
        args["principal"] = object;
    }
    return args;
}

namespace {

bool isFileClass(const chat::SymbolTable::Entry* e) { return e != nullptr && (e->cls == "file" || e->cls == "document"); }

} // namespace

void SymbolUi::hover(const chat::Symbol& s) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (!detailShown(s.display)) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::CalcTextSize("0").x * 72);
    if (s.kind == chat::SymbolKind::Unresolved) {
        ui::text(Ink::secondary, "unresolved: nothing you can read has this id");
    } else if (s.kind == chat::SymbolKind::Seq) {
        ui::text(Ink::strong, "seq " + std::to_string(s.seq));
        ui::text(Ink::secondary, "click: its row in HISTORY");
    } else {
        const chat::SymbolTable::Entry* e = table_ ? table_->byId(s.object) : nullptr;
        const model::PrincipalInfo* p = fold_ ? fold_->get(s.object) : nullptr;
        const std::string name = fold_ ? fold_->nameOf(s.object) : s.display;
        ui::text(Ink::strong, name);
        if (fold_ != nullptr && !s.object.empty()) {
            ui::text(Ink::secondary, "bound name " + fold_->boundName(s.object) + "  id " + s.object);
        }
        std::string line = e && !e->cls.empty() ? e->cls : "object";
        if (p && !p->drivenBy.empty()) {
            line += " / " + p->drivenBy;
        }
        if (p) {
            const model::LoopState loop = model::loopState(*p);
            if (loop != model::LoopState::None) {
                line += std::string(" / ") + model::name(loop);
            }
        }
        ui::text(Ink::secondary, line);
        if (fold_) {
            const std::string what = fold_->describe(s.object, e ? e->cls : std::string());
            if (!what.empty()) {
                ui::text(Ink::primary, what);
            }
        }
        if (s.kind == chat::SymbolKind::Path) {
            ui::text(Ink::inspected, s.path);
        }
        if (s.kind == chat::SymbolKind::Path || isFileClass(e)) {
            ui::text(Ink::secondary, modules::kFileHint);
        }
        if (s.kind == chat::SymbolKind::Field) {
            ui::text(Ink::inspected, s.field);
            if (fold_) {
                if (const model::FieldState* f = fold_->fieldOf(s.object, s.field)) {
                    const std::string v = f->value.is_string() ? f->value.get<std::string>() : f->value.dump();
                    ui::text(Ink::primary, v.size() > 240 ? v.substr(0, 240) + "…" : v);
                }
            }
        }
        if (fold_) {
            if (const model::FieldState* h = fold_->fieldOf(s.object, "object/hint"); h && h->value.is_string()) {
                ui::text(Ink::primary, h->value.get<std::string>());
            }
        }
        ui::text(Ink::secondary, s.object);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void SymbolUi::click(const chat::Symbol& s) {
    LinkRequest r;
    r.object = s.object;
    const chat::SymbolTable::Entry* entry = table_ != nullptr ? table_->byId(s.object) : nullptr;
    // A file read through a mount is an Object named `mount/path`: a click on it is a click on the file.
    const chat::SymbolTable::Entry* mount = nullptr;
    std::string relative;
    if (entry != nullptr && table_ != nullptr) {
        if (const std::size_t slash = entry->name.find('/'); slash != std::string::npos) {
            const chat::SymbolTable::Entry* m = table_->byName(entry->name.substr(0, slash));
            if (m != nullptr && m->cls == "adapter") {
                mount = m;
                relative = entry->name.substr(slash + 1);
            }
        }
    }
    // Any other name with a folder and an extension is a file Object under some mount: the world knows which.
    bool fileObject = false;
    if (entry != nullptr && mount == nullptr && s.kind != chat::SymbolKind::Path) {
        const std::size_t slash = entry->name.rfind('/');
        fileObject = slash != std::string::npos && entry->name.find('.', slash) != std::string::npos;
    }
    const bool file = s.kind == chat::SymbolKind::Path || isFileClass(entry) || mount != nullptr || fileObject;
    switch (modules::clickAction(file, ImGui::GetIO().KeyShift)) {
    case modules::ClickAction::OpenModule:
        r.kind = LinkRequest::OpenFile;
        r.line = s.line;
        r.path = s.kind == chat::SymbolKind::Path ? s.path : mount != nullptr ? relative : entry->name;
        // a citation carries its line: `harness/docs/spec.md:1130` opens the file
        if (const std::size_t colon = r.path.rfind(':'); colon != std::string::npos && colon + 1 < r.path.size() &&
            r.path.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
            r.path.resize(colon);
        }
        if (mount != nullptr) {
            r.object = mount->id;
        } else if (fileObject) {
            r.fileObject = s.object;
        }
        request_ = std::move(r);
        return;
    case modules::ClickAction::Inspect:
        if (s.kind != chat::SymbolKind::Seq && s.kind != chat::SymbolKind::Unresolved) {
            r.kind = LinkRequest::Inspect;
            r.object = s.kind == chat::SymbolKind::Path ? s.display : s.object;
            r.byName = s.kind == chat::SymbolKind::Path;
            request_ = std::move(r);
            return;
        }
        break;
    case modules::ClickAction::Default:
        break;
    }
    switch (s.kind) {
    case chat::SymbolKind::Unresolved:
        return;
    case chat::SymbolKind::Seq:
        r.kind = LinkRequest::History;
        r.seq = s.seq;
        break;
    case chat::SymbolKind::Field:
        r.kind = LinkRequest::Field;
        r.field = s.field;
        break;
    case chat::SymbolKind::Path:
        r.kind = LinkRequest::Inspect;
        r.object = s.display;
        r.byName = true;
        break;
    case chat::SymbolKind::Object: {
        const model::PrincipalInfo* p = fold_ ? fold_->get(s.object) : nullptr;
        if (p && p->cls == "principal") {
            r.kind = LinkRequest::Terminal;
        } else if (p) {
            r.kind = LinkRequest::Fields;
        } else {
            r.kind = LinkRequest::Inspect;
        }
        break;
    }
    }
    request_ = std::move(r);
}

void SymbolUi::openMenu(const chat::Symbol& s) {
    menu_ = s;
    openPending_ = true;
}

bool SymbolUi::handleDoc(const MdExtras& md, const SelDoc& doc, const SelResult& r, bool dragging) {
    if (r.clickedOffset >= 0) {
        if (const chat::Symbol* s = symbolAt(md, doc, r.clickedOffset)) {
            click(*s);
        }
    }
    if (r.rightClickOffset >= 0) {
        if (const chat::Symbol* s = symbolAt(md, doc, r.rightClickOffset)) {
            openMenu(*s);
        }
    }
    if (r.hoveredOffset < 0 || dragging) {
        return false;
    }
    const chat::Symbol* s = symbolAt(md, doc, r.hoveredOffset);
    if (!s) {
        return false;
    }
    hover(*s);
    return true;
}

void SymbolUi::text(const std::string& s, ImU32 color, const std::string& context, bool plain) {
    std::vector<chat::Symbol> spans;
    if (!plain && table_) {
        chat::ScanContext ctx;
        ctx.fold = fold_;
        ctx.object = context;
        spans = table_->scan(s, ctx);
    }
    if (spans.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(s.c_str());
        ImGui::PopStyleColor();
        return;
    }
    std::size_t at = 0;
    bool first = true;
    auto piece = [&](const char* b, const char* e, ImU32 c) {
        if (!first) {
            ImGui::SameLine(0.0f, 0.0f);
        }
        first = false;
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextUnformatted(b, e);
        ImGui::PopStyleColor();
    };
    for (const chat::Symbol& sym : spans) {
        if (sym.start > at) {
            piece(s.data() + at, s.data() + sym.start, color);
        }
        const bool dead = sym.kind == chat::SymbolKind::Unresolved;
        piece(sym.display.data(), sym.display.data() + sym.display.size(), dead ? Ink::secondary : Ink::nav);
        // by rectangle: inside a selectable row the row is the active item the moment it is pressed
        if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            hover(sym);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                click(sym);
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                openMenu(sym);
            }
        }
        drive::hit("link:" + sym.display, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        at = sym.end;
    }
    if (at < s.size()) {
        piece(s.data() + at, s.data() + s.size(), color);
    }
}

void SymbolUi::drawMenu() {
    const ImGuiID id = ImHashStr(kMenu);
    if (openPending_) {
        ImGui::OpenPopup(id);
        openPending_ = false;
    }
    if (!menu_ || !ImGui::BeginPopupEx(id, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                               ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    const chat::Symbol s = *menu_;
    auto want = [this, &s](LinkRequest::Kind kind) {
        LinkRequest r;
        r.kind = kind;
        r.object = s.object;
        r.seq = s.seq;
        r.field = s.field;
        request_ = std::move(r);
    };
    if (s.kind == chat::SymbolKind::Seq) {
        ui::text(Ink::strong, s.display);
        ImGui::Separator();
        if (menuItem("HISTORY")) {
            want(LinkRequest::History);
        }
        if (menuItem("COPY SEQ")) {
            ImGui::SetClipboardText(std::to_string(s.seq).c_str());
        }
    } else if (s.kind == chat::SymbolKind::Unresolved) {
        ui::text(Ink::secondary, "unresolved: " + s.display);
        ImGui::Separator();
        if (menuItem("COPY TEXT")) {
            ImGui::SetClipboardText(s.display.c_str());
        }
    } else if (s.kind == chat::SymbolKind::Path || isFileClass(table_ ? table_->byId(s.object) : nullptr)) {
        const chat::SymbolTable::Entry* e = table_ ? table_->byId(s.object) : nullptr;
        const std::string path = s.kind == chat::SymbolKind::Path ? s.path : (e != nullptr ? e->name : s.display);
        const auto open = [&](const std::string& kind, bool newPane) {
            LinkRequest r;
            r.kind = LinkRequest::OpenFile;
            r.object = s.object;
            r.path = path;
            r.openKind = kind;
            r.newPane = newPane;
            request_ = std::move(r);
        };
        ui::text(Ink::strong, s.display);
        ImGui::Separator();
        if (menuItem("OPEN")) {
            open("", false);
        }
        if (beginMenu("OPEN WITH")) {
            for (const modules::ModuleSpec* m : modules::openWith(path)) {
                if (menuItem(m->title)) {
                    open(m->kind, false);
                }
            }
            ImGui::EndMenu();
        }
        if (menuItem("OPEN IN NEW PANE")) {
            open("", true);
        }
        if (menuItem("INSPECT")) {
            LinkRequest r;
            r.kind = LinkRequest::Inspect;
            r.object = s.kind == chat::SymbolKind::Path ? s.display : s.object;
            r.byName = s.kind == chat::SymbolKind::Path;
            request_ = std::move(r);
        }
        ImGui::Separator();
        if (menuItem("COPY PATH")) {
            ImGui::SetClipboardText(path.c_str());
        }
        if (menuItem("COPY ID", nullptr, false, !s.object.empty())) {
            ImGui::SetClipboardText(s.object.c_str());
        }
    } else {
        const model::PrincipalInfo* p = fold_ ? fold_->get(s.object) : nullptr;
        const chat::SymbolTable::Entry* e = table_ ? table_->byId(s.object) : nullptr;
        const std::string cls = e ? e->cls : std::string();
        ui::text(Ink::strong, fold_ ? fold_->nameOf(s.object) : s.display);
        ImGui::Separator();
        if (menuItem("CHAT", nullptr, false, cls == "principal")) {
            want(LinkRequest::Terminal);
        }
        if (menuItem("INSPECT")) {
            want(LinkRequest::Inspect);
        }
        if (menuItem("GRAPH", nullptr, false, p != nullptr)) {
            want(LinkRequest::Graph);
        }
        {
            // An action, a Field or a class has a topic in the manual, whatever its name here.
            const std::string bound = fold_ ? fold_->boundName(s.object) : std::string();
            std::string topic = s.kind == chat::SymbolKind::Field ? "field/" + s.field
                                : bound.rfind("action/", 0) == 0  ? bound
                                : !cls.empty()                    ? "class/" + cls
                                                                  : std::string();
            if (menuItem("SHOW DOCUMENTATION", nullptr, false, !topic.empty())) {
                LinkRequest r;
                r.kind = LinkRequest::Doc;
                r.field = std::move(topic);
                request_ = std::move(r);
            }
            drive::hit("menu:show-documentation", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        if (actions_ && actions_->size() > 0) {
            ImGui::Separator();
            ui::text(Ink::secondary, "actions");
            for (const model::ActionInfo* a : actions_->ordered()) {
                bool enabled = true;
                json args = actionPrefill(a->name, s.object, cls, enabled);
                const std::string label = a->name == "object.rename" ? "RENAME BOUND NAME (what agents resolve)"
                                                                     : upperOf(a->name) + (a->gated ? "  (gated)" : "");
                if (menuItem(label.c_str(), nullptr, false, enabled)) {
                    LinkRequest r;
                    r.kind = LinkRequest::Action;
                    r.action = a->name;
                    r.args = std::move(args);
                    request_ = std::move(r);
                }
                if (ImGui::IsItemHovered() && !a->help.empty()) {
                    wrappedTooltip(a->help);
                }
            }
        }
        ImGui::Separator();
        if (menuItem("RENAME LABEL...")) {
            want(LinkRequest::Label);
        }
        drive::hit("menu:rename-label", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("a pretty name for people, on this object; agents keep resolving the bound name");
        }
        if (menuItem("COPY ID")) {
            ImGui::SetClipboardText(s.object.c_str());
        }
        const std::string blocked = fold_ ? model::deleteBlocked(*fold_, s.object) : std::string();
        if (fold_) {
            const model::PrincipalInfo* who = fold_->get(s.object);
            if (who && who->cls == "principal" && who->drivenBy != "person" && menuItem("OFF THE RECORD")) {
                want(LinkRequest::Scratch);
            }
        }
        const bool del = menuItem("DELETE", nullptr, false, blocked.empty());
        if (blocked.empty()) {
            drive::hit("menu:delete", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        if (del) {
            want(LinkRequest::Delete);
        }
        if (!blocked.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", blocked.c_str());
        }
        if (menuItem("COPY NAME")) {
            ImGui::SetClipboardText((fold_ ? fold_->nameOf(s.object) : s.display).c_str());
        }
    }
    ImGui::EndPopup();
}

std::optional<LinkRequest> SymbolUi::take() {
    std::optional<LinkRequest> r;
    r.swap(request_);
    return r;
}

} // namespace ke::ui
