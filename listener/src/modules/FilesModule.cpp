// The file manager: two lists side by side on directories the world holds. Every listing and
// every change is a `pane.*` call; the status line shows the world's answer, and an outward verb
// says it is waiting while the approval gate holds it.
#include "modules/FileListing.hpp"
#include "modules/Module.hpp"
#include "modules/ModuleUi.hpp"
#include "modules/Registry.hpp"

#include "drive/Commands.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <cmath>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace ke::modules {

namespace {

class FilesModule final : public Module {
  public:
    FilesModule(Host& host, const model::PaneInfo& p) : host_(host), id_(p.id), target_(p.target) {}

    void restore(const model::PaneInfo&) override {
        if (!loaded_) {
            loaded_ = true;
            list(0, "");
            list(1, "");
        }
    }

    void driven(const model::PaneInput&, const std::string&) override { refresh(); }

    void keyboard() override {
        if (ImGui::GetIO().WantTextInput || prompting()) {
            return;
        }
        FilePanel& p = panel_[static_cast<std::size_t>(side_)];
        const auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
        if (pressed(ImGuiKey_DownArrow)) {
            p.move(1);
        } else if (pressed(ImGuiKey_UpArrow)) {
            p.move(-1);
        } else if (pressed(ImGuiKey_PageDown)) {
            p.move(page_);
        } else if (pressed(ImGuiKey_PageUp)) {
            p.move(-page_);
        } else if (pressed(ImGuiKey_Home)) {
            p.cursor = 0;
        } else if (pressed(ImGuiKey_End)) {
            p.move(static_cast<int>(p.entries.size()));
        } else if (pressed(ImGuiKey_Tab) || pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_RightArrow)) {
            side_ = 1 - side_;
        } else if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
            activate();
        } else if (pressed(ImGuiKey_Backspace)) {
            up();
        } else if (pressed(ImGuiKey_F5)) {
            ask("copy");
        } else if (pressed(ImGuiKey_F6)) {
            ask("move");
        } else if (pressed(ImGuiKey_F2)) {
            ask("rename");
        } else if (pressed(ImGuiKey_F7)) {
            ask("mkdir");
        } else if (pressed(ImGuiKey_Delete)) {
            ask("delete");
        }
    }

    void draw(Rect r, bool focused) override {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float lh = std::floor(ImGui::GetTextLineHeight() + 2.0f);
        dl->AddRectFilled(topLeft(r), bottomRight(r), ui::Surface::background);
        // One row of verbs when they fit, two when a pane is narrow: nothing runs off the edge.
        const float row = ImGui::GetFrameHeightWithSpacing();
        const bool twoRows = barWidth() > r.w - 8.0f;
        const float barH = row * (twoRows ? 2.0f : 1.0f) + 2.0f;
        const float statusH = lh + 2.0f + (prompting() ? ImGui::GetFrameHeightWithSpacing() : 0.0f);
        const float mid = std::floor(r.w / 2);
        const float h = r.h - barH - statusH;
        for (int s = 0; s < 2; ++s) {
            drawPanel(Rect{r.x + (s == 0 ? 0.0f : mid + 1.0f), r.y, mid - 1.0f, h}, s, lh, focused);
        }
        fileMenu();  // once: both panels share the popup, a call from each drew its items twice
        dl->AddLine(ImVec2(r.x + mid, r.y), ImVec2(r.x + mid, r.y + h), ui::Surface::codeBorder);
        dl->AddLine(ImVec2(r.x, r.y + h), ImVec2(r.x + r.w, r.y + h), ui::Surface::codeBorder);
        ImGui::SetCursorScreenPos(ImVec2(r.x + 4.0f, r.y + h + 3.0f));
        ImGui::PushID(id_.c_str());
        const bool any = panel_[static_cast<std::size_t>(side_)].current() != nullptr;
        if (ui::bracketButtonNamed("OPEN", "files-open", any)) {
            activate();
        }
        for (const char* verb : {"copy", "move", "rename", "mkdir", "delete"}) {
            if (twoRows && std::string_view(verb) == "rename") {
                ImGui::SetCursorScreenPos(ImVec2(r.x + 4.0f, r.y + h + 3.0f + row));
            } else {
                ImGui::SameLine();
            }
            const std::string word = ui::upper(verb);
            if (ui::bracketButtonNamed(word.c_str(), std::string("files-") + verb, any || std::string_view(verb) == "mkdir")) {
                ask(verb);
            }
        }
        ImGui::SameLine();
        if (ui::bracketButtonNamed("REFRESH", "files-refresh")) {
            refresh();
        }
        if (prompting()) {
            drawPrompt(r.x + 4.0f, r.y + r.h - statusH + 2.0f, r.w);
        }
        ImGui::PopID();
        const float sy = r.y + r.h - lh - 1.0f;
        dl->AddText(ImVec2(r.x + 4.0f, sy), statusBad_ ? ui::Ink::fatal : waiting_ ? ui::Ink::inspected : ui::Ink::secondary, status_.c_str());
    }

    nlohmann::json describe() const override {
        nlohmann::json sides = nlohmann::json::array();
        for (const FilePanel& p : panel_) {
            nlohmann::json names = nlohmann::json::array();
            for (const FileEntry& e : p.entries) {
                names.push_back(e.name);
            }
            sides.push_back({{"path", p.path}, {"entries", names}, {"cursor", p.cursor}, {"error", p.error}});
        }
        return {{"kind", "files"}, {"panels", sides}, {"side", side_}, {"status", status_}};
    }

    std::string selection() const override {
        const FileEntry* e = panel_[static_cast<std::size_t>(side_)].current();
        return e ? joinPath(panel_[static_cast<std::size_t>(side_)].path, e->name) : "";
    }

    std::string heldError() const override { return statusBad_ ? status_ : std::string(); }

  private:
    bool prompting() const { return !prompt_.empty(); }

    void list(int side, const std::string& path) {
        FilePanel& p = panel_[static_cast<std::size_t>(side)];
        p.loading = true;
        p.path = path;
        nlohmann::json args = nlohmann::json::object();
        if (!path.empty()) {
            args["path"] = path;
        }
        host_.act("pane.frob", {{"pane", id_}, {"verb", "list"}, {"args", std::move(args)}}, [this, side](const Response& r) {
            FilePanel& panel = panel_[static_cast<std::size_t>(side)];
            panel.loading = false;
            if (!r.ok) {
                panel.error = errLine(r);
                panel.entries.clear();
                return;
            }
            panel.set(parseListing(r.result));
        });
    }

    void refresh() {
        list(0, panel_[0].path);
        list(1, panel_[1].path);
    }

    void up() {
        FilePanel& p = panel_[static_cast<std::size_t>(side_)];
        list(side_, parentPath(p.path));
        p.cursor = 0;
    }

    void activate() {
        FilePanel& p = panel_[static_cast<std::size_t>(side_)];
        const FileEntry* e = p.current();
        if (e == nullptr) {
            return;
        }
        const std::string full = joinPath(p.path, e->name);
        if (e->directory()) {
            p.cursor = 0;
            list(side_, full);
        } else {
            host_.open(FileRef{target_, full, "", false});
        }
    }

    void ask(const char* verb) {
        const FilePanel& p = panel_[static_cast<std::size_t>(side_)];
        const FileEntry* e = p.current();
        const std::string v = verb;
        prompt_ = v;
        if (v == "mkdir") {
            answer_.clear();
        } else if (v == "rename") {
            answer_ = e ? e->name : "";
        } else if (v == "copy" || v == "move") {
            answer_ = joinPath(panel_[static_cast<std::size_t>(1 - side_)].path, e ? e->name : "");
        } else {
            answer_.clear();
        }
        focusPrompt_ = true;
    }

    void run() {
        FilePanel& p = panel_[static_cast<std::size_t>(side_)];
        const FileEntry* e = p.current();
        const std::string verb = prompt_;
        const std::string from = e ? joinPath(p.path, e->name) : "";
        nlohmann::json args;
        if (verb == "mkdir") {
            args = {{"name", joinPath(p.path, answer_)}};
        } else if (verb == "rename") {
            args = {{"from", from}, {"to", joinPath(p.path, answer_)}};
        } else if (verb == "delete") {
            args = {{"name", from}};
        } else {
            args = {{"from", from}, {"to", answer_}};
        }
        prompt_.clear();
        waiting_ = true;
        statusBad_ = false;
        status_ = verb + ": waiting on the world";
        host_.act("pane.frob", {{"pane", id_}, {"verb", verb}, {"args", std::move(args)}}, [this, verb](const Response& r) {
            waiting_ = false;
            statusBad_ = !r.ok;
            if (!r.ok) {
                status_ = verb + ": " + errLine(r);
                return;
            }
            const bool held = r.result.is_object() && r.result.contains("state") && r.result["state"] == "waiting";
            status_ = verb + (held ? ": waiting on approval" : ": done");
            waiting_ = held;
            refresh();
        });
    }

    static float barWidth() {
        const ImGuiStyle& style = ImGui::GetStyle();
        float w = 0.0f;
        for (const char* word : {"OPEN", "COPY", "MOVE", "RENAME", "MKDIR", "DELETE", "REFRESH"}) {
            w += ImGui::CalcTextSize(word).x + ImGui::CalcTextSize("[  ]").x + style.FramePadding.x * 2 + style.ItemSpacing.x;
        }
        return w;
    }

    void drawPrompt(float x, float y, float width) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        const std::string label = ui::upper(prompt_);
        ui::dimText("%s", label.c_str());
        ImGui::SameLine();
        bool go = false;
        if (prompt_ == "delete") {
            const FileEntry* e = panel_[static_cast<std::size_t>(side_)].current();
            ui::text(ui::Ink::inspected, e ? e->name : "");
        } else {
            if (focusPrompt_) {
                ImGui::SetKeyboardFocusHere();
                focusPrompt_ = false;
            }
            ImGui::SetNextItemWidth(width * 0.5f);
            go = ImGui::InputText("##answer", &answer_, ImGuiInputTextFlags_EnterReturnsTrue);
            drive::hit("input:files", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ImGui::SameLine();
        go = ui::bracketButtonNamed("GO", "files-go") || go;
        ImGui::SameLine();
        if (ui::bracketButtonNamed("CANCEL", "files-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            prompt_.clear();
            return;
        }
        if (go) {
            run();
        }
    }

    void drawPanel(Rect r, int s, float lh, bool focused) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        FilePanel& p = panel_[static_cast<std::size_t>(s)];
        const bool active = s == side_;
        const float cw = ImGui::CalcTextSize("0").x;
        // Columns from the right: kind, date, size; the name takes the rest and clips.
        // The name always shows: kind goes first, then the date, then the size.
        const float nameMin = cw * 18;
        float kindW = cw * 9, dateW = cw * 12, sizeW = cw * 7;
        if (r.w - kindW - dateW - sizeW < nameMin) {
            kindW = 0.0f;
        }
        if (r.w - kindW - dateW - sizeW < nameMin) {
            dateW = 0.0f;
        }
        if (r.w - kindW - dateW - sizeW < nameMin) {
            sizeW = 0.0f;
        }
        const float dateX = r.x + r.w - kindW - dateW, sizeX = dateX - sizeW, kindX = r.x + r.w - kindW;
        const std::string title = p.path.empty() ? "/" : p.path;
        dl->PushClipRect(topLeft(r), bottomRight(r), true);
        dl->AddText(ImVec2(r.x + 4.0f, r.y + 1.0f), active && focused ? ui::Ink::strong : ui::Ink::secondary, title.c_str());
        const float headY = r.y + lh;
        dl->AddText(ImVec2(r.x + 4.0f, headY), ui::Ink::secondary, ui::upper("NAME").c_str());
        if (sizeW > 0.0f) {
            dl->AddText(ImVec2(sizeX, headY), ui::Ink::secondary, ui::upper("SIZE").c_str());
        }
        if (dateW > 0.0f) {
            dl->AddText(ImVec2(dateX, headY), ui::Ink::secondary, ui::upper("DATE").c_str());
        }
        if (kindW > 0.0f) {
            dl->AddText(ImVec2(kindX, headY), ui::Ink::secondary, ui::upper("KIND").c_str());
        }
        const float listY = headY + lh;
        const int rows = std::max(1, static_cast<int>((r.y + r.h - listY) / lh));
        page_ = std::max(1, rows - 1);
        const ImGuiIO& io = ImGui::GetIO();
        Rect listRect{r.x, listY, r.w, r.y + r.h - listY};
        const bool over = hoveredRect(listRect);
        float& offset = offset_[static_cast<std::size_t>(s)];
        if (over) {
            offset -= io.MouseWheel * 3.0f;
            if (io.MouseSource == ImGuiMouseSource_TouchScreen && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8.0f)) {
                offset -= io.MouseDelta.y / lh;
            }
        }
        const int count = static_cast<int>(p.entries.size());
        if (p.cursor < static_cast<int>(offset)) {
            offset = static_cast<float>(p.cursor);
        } else if (p.cursor >= static_cast<int>(offset) + rows) {
            offset = static_cast<float>(p.cursor - rows + 1);
        }
        offset = std::clamp(offset, 0.0f, static_cast<float>(std::max(0, count - rows)));
        const int first = static_cast<int>(offset);
        if (!p.error.empty()) {
            dl->AddText(ImVec2(r.x + 4.0f, listY), ui::Ink::fatal, p.error.c_str());
        } else if (p.loading && p.entries.empty()) {
            dl->AddText(ImVec2(r.x + 4.0f, listY), ui::Ink::secondary, "listing");
        }
        for (int i = first; i < count && i < first + rows; ++i) {
            const FileEntry& e = p.entries[static_cast<std::size_t>(i)];
            const float y = listY + static_cast<float>(i - first) * lh;
            const bool cur = i == p.cursor;
            const ImVec2 a(r.x, y), b(r.x + r.w, y + lh);
            const bool hover = ImGui::IsMouseHoveringRect(a, b, true) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
            if (cur) {
                dl->AddRectFilled(a, b, active ? ui::Surface::selection : ui::Surface::hover);
            } else if (hover) {
                dl->AddRectFilled(a, b, ui::Surface::hover);
            }
            const ImU32 ink = cur && active ? ui::Ink::onSelection : e.directory() ? ui::Ink::nav : ui::Ink::primary;
            dl->PushClipRect(ImVec2(r.x, y), ImVec2(sizeX - cw, y + lh), true);
            const std::string name = e.name + (e.directory() ? "/" : "");
            dl->AddText(ImVec2(r.x + 4.0f, y + 1.0f), ink, name.c_str());
            dl->PopClipRect();
            const ImU32 facts = cur && active ? ink : ui::Ink::secondary;
            if (sizeW > 0.0f) {
                dl->AddText(ImVec2(sizeX, y + 1.0f), facts, formatSize(e.size).c_str());
            }
            if (dateW > 0.0f) {
                dl->AddText(ImVec2(dateX, y + 1.0f), facts, shortDate(e.modified).c_str());
            }
            if (kindW > 0.0f) {
                dl->AddText(ImVec2(kindX, y + 1.0f), facts, kindLabel(e).c_str());
            }
            drive::hit("file:" + e.name, a, b);
            if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                side_ = s;
                p.cursor = i;
                const FileEntry entry = e;
                if (clickAction(!entry.directory(), io.KeyShift) == ClickAction::Inspect) {
                    host_.inspect(joinPath(p.path, entry.name));
                } else if (entry.directory() || ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) >= 1) {
                    activate();
                    dl->PopClipRect();
                    return;
                }
            } else if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                side_ = s;
                p.cursor = i;
                menuPath_ = joinPath(p.path, e.name);
                ImGui::OpenPopup("filemenu");
            }
            if (hover && !e.directory()) {
                ImGui::SetTooltip("%s\n%s", joinPath(p.path, e.name).c_str(), kFileHint);
            }
        }
        dl->PopClipRect();
        if (hoveredRect(r) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            side_ = s;
        }
    }

    void fileMenu() {
        if (!ImGui::BeginPopup("filemenu")) {
            return;
        }
        if (ui::menuItem("OPEN")) {
            host_.open(FileRef{target_, menuPath_, "", false});
        }
        if (ui::beginMenu("OPEN WITH")) {
            for (const ModuleSpec* m : openWith(menuPath_)) {
                if (ui::menuItem(m->title)) {
                    host_.open(FileRef{target_, menuPath_, m->kind, false});
                }
            }
            ImGui::EndMenu();
        }
        if (ui::menuItem("OPEN IN NEW PANE")) {
            host_.open(FileRef{target_, menuPath_, "", true});
        }
        if (ui::menuItem("INSPECT")) {
            host_.inspect(menuPath_);
        }
        if (ui::menuItem("COPY PATH")) {
            host_.copy(menuPath_);
        }
        ImGui::Separator();
        // a click opens a file, so the verbs on the row under the pointer live here as well
        for (const char* verb : {"copy", "move", "rename", "delete"}) {
            if (ui::menuItem(ui::upper(verb).c_str())) {
                ask(verb);
            }
        }
        ImGui::EndPopup();
    }

    Host& host_;
    std::string id_, target_;
    FilePanel panel_[2];
    float offset_[2] = {0.0f, 0.0f};
    int side_ = 0, page_ = 10;
    bool loaded_ = false, statusBad_ = false, waiting_ = false, focusPrompt_ = false;
    std::string status_, prompt_, answer_, menuPath_;
};

} // namespace

std::unique_ptr<Module> makeFiles(Host& host, const model::PaneInfo& pane) {
    return std::make_unique<FilesModule>(host, pane);
}

} // namespace ke::modules
