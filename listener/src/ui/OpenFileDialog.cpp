#include "ui/OpenFileDialog.hpp"

#include "drive/Commands.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace ke::ui {

using nlohmann::json;

namespace {

std::string fieldText(const model::PrincipalInfo& p, const char* name) {
    const auto it = p.fields.find(name);
    return it != p.fields.end() && it->second.value.is_string() ? it->second.value.get<std::string>() : std::string();
}

} // namespace

// Tab and the arrows arrive inside InputText, which owns the buffer until it returns.
struct OpenFileLine {
    static int callback(ImGuiInputTextCallbackData* data) {
        auto* self = static_cast<OpenFileDialog*>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
            if (self->rewrite_) { // an active field keeps its own buffer: the line changes in here
                self->rewrite_ = false;
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, self->path_.c_str());
            }
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            self->step_ = data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            self->tabPressed_ = true;
        }
        return 0;
    }
};

std::string OpenFileDialog::commonPrefix(const std::vector<std::string>& names) {
    if (names.empty()) {
        return {};
    }
    std::string p = names.front();
    for (const std::string& n : names) {
        std::size_t i = 0;
        while (i < p.size() && i < n.size() && p[i] == n[i]) {
            ++i;
        }
        p.resize(i);
    }
    return p;
}

bool OpenFileDialog::namesItsOwnMount(const std::string& path, const std::vector<std::string>& mounts) {
    if (!path.empty() && path.front() == '/') {
        return true;
    }
    const std::string first = path.substr(0, path.find('/'));
    return path.find('/') != std::string::npos && std::find(mounts.begin(), mounts.end(), first) != mounts.end();
}

std::vector<OpenFileDialog::Mount> OpenFileDialog::mountsOf(const model::Principals& principals) const {
    std::vector<Mount> out;
    for (const model::PrincipalInfo* p : principals.ordered()) {
        if (p->cls == "adapter" && fieldText(*p, "adapter/binds") == "filesystem") {
            out.push_back(Mount{p->id, principals.nameOf(p->id)});
        }
    }
    return out;
}

std::string OpenFileDialog::parentOfLine() const {
    const std::size_t cut = path_.rfind('/');
    return cut == std::string::npos ? std::string() : path_.substr(0, cut + 1);
}

void OpenFileDialog::ask(chat::Chat& chat) {
    const std::string key = mount_ + "\n" + path_;
    if (key == asked_) {
        return;
    }
    asked_ = key;
    const unsigned long long mine = ++seq_;
    chat.actNamed("path.complete", json{{"mount", mount_}, {"path", path_}}, [this, mine](const Response& r) {
        if (mine != seq_) {
            return; // a newer line is on its way
        }
        entries_.clear();
        selected_ = -1;
        if (r.ok) {
            dir_ = r.result.value("dir", std::string());
            for (const json& e : r.result.value("entries", json::array())) {
                if (e.is_string()) {
                    entries_.push_back(e.get<std::string>());
                }
            }
        }
    });
}

void OpenFileDialog::setLine(const std::string& line) {
    path_ = line;
    rewrite_ = true;
    refocus_ = !ImGui::IsAnyItemActive();
}

void OpenFileDialog::choose(const std::string& entry) { setLine(parentOfLine() + entry); }

void OpenFileDialog::finish(const std::string& path) {
    if (onPick_) {
        onPick_(mount_, path);
        onPick_ = nullptr;
        close_ = true;
        return;
    }
    LinkRequest r;
    r.kind = LinkRequest::OpenFile;
    r.object = mount_;
    r.path = path;
    symbolUi().ask(std::move(r));
    close_ = true;
}

void OpenFileDialog::draw(chat::Chat& chat, const model::Principals& principals) {
    if (wanted_) {
        wanted_ = false;
        asked_.clear();
        path_.clear();
        entries_.clear();
        selected_ = -1;
        refocus_ = true;
        ImGui::OpenPopup("OPEN FILE");
    }
    if (!ImGui::BeginPopupModal("OPEN FILE", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const float ch = ImGui::CalcTextSize("0").x;
    const std::vector<Mount> mounts = mountsOf(principals);
    if (std::none_of(mounts.begin(), mounts.end(), [this](const Mount& m) { return m.id == mount_; })) {
        mount_ = mounts.empty() ? std::string() : mounts.front().id;
    }
    std::vector<std::string> names;
    for (const Mount& m : mounts) {
        names.push_back(m.name);
    }
    if (mounts.empty()) {
        dimText("no mounts yet: a whole path in the world still opens");
    }
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        if (i > 0 && ImGui::GetCursorPosX() < ch * 44) {
            ImGui::SameLine();
        }
        if (bracketButtonNamed(mounts[i].name.c_str(), "mount:" + mounts[i].name, true, mounts[i].id == mount_)) {
            mount_ = mounts[i].id;
            refocus_ = true;
        }
    }
    ImGui::SetNextItemWidth(ch * 64.0f);
    if (refocus_ || ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
        refocus_ = false;
    }
    const bool enter = ImGui::InputTextWithHint("##openpath", "a path under this mount, or a whole path", &path_,
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
                                                    ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways,
                                                &OpenFileLine::callback, this);
    drive::hit("input:open-file", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool bare = mount_.empty() || namesItsOwnMount(path_, names);
    if (!bare) {
        ask(chat);
    } else {
        entries_.clear();
        selected_ = -1;
    }
    const int count = static_cast<int>(entries_.size());
    if (step_ != 0 && count > 0) {
        selected_ = selected_ < 0 ? (step_ > 0 ? 0 : count - 1) : (selected_ + step_ + count) % count;
    }
    step_ = 0;
    if (tabPressed_) {
        tabPressed_ = false;
        const std::string common = commonPrefix(entries_);
        const std::string stem = path_.substr(parentOfLine().size());
        if (selected_ >= 0 && selected_ < count) {
            choose(entries_[static_cast<std::size_t>(selected_)]);
        } else if (common.size() >= stem.size() && count > 0) {
            choose(common);
        }
    }
    // Enter on a directory goes in; on a file, or a line with no candidate, it opens.
    if (enter) {
        std::string line = path_;
        if (selected_ >= 0 && selected_ < count) {
            line = parentOfLine() + entries_[static_cast<std::size_t>(selected_)];
        }
        if (!bare && !line.empty() && line.back() == '/') {
            setLine(line);
        } else if (bare && onPick_) {
            onPick_("", line);
            onPick_ = nullptr;
            close_ = true;
        } else if (bare) {
            LinkRequest r;
            r.kind = LinkRequest::OpenFile;
            r.path = line;
            symbolUi().ask(std::move(r));
            close_ = true;
        } else if (!line.empty()) {
            finish(line);
        }
    }
    ImGui::BeginChild("open-candidates", ImVec2(ch * 64.0f, ImGui::GetTextLineHeightWithSpacing() * 10), ImGuiChildFlags_Borders);
    touchDragScroll();
    int row = 0;
    for (const std::string& e : std::vector<std::string>(entries_)) {
        ImGui::PushID(row);
        const bool picked = row == selected_;
        if (ImGui::Selectable(e.c_str(), picked)) {
            const std::string line = parentOfLine() + e;
            if (e.back() == '/') {
                choose(e);
            } else {
                finish(line);
            }
        }
        if (picked) {
            ImGui::SetScrollHereY();
        }
        drive::hit("path:" + e, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::PopID();
        ++row;
    }
    ImGui::EndChild();
    dialogGap();
    if (bracketButtonNamed("OPEN", "open-file", !path_.empty())) {
        if (bare) {
            LinkRequest r;
            r.kind = LinkRequest::OpenFile;
            r.path = path_;
            symbolUi().ask(std::move(r));
            close_ = true;
        } else {
            finish(path_);
        }
    }
    ImGui::SameLine();
    if (bracketButtonNamed("CANCEL", "open-file-cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || close_) {
        close_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

json OpenFileDialog::driveState() const {
    return json{{"mount", mount_}, {"path", path_}, {"entries", entries_}, {"selected", selected_}};
}

} // namespace ke::ui
