#include "ui/ManualPane.hpp"

#include <cmath>


#include "drive/Commands.hpp"
#include "ui/Chrome.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <imgui_stdlib.h>

namespace ke::ui {

using nlohmann::json;

namespace {

// actNamed has already lifted the action's answer to the top.
const json& resultOf(const Response& r) { return r.result; }

constexpr const char* kBookmarks = "view/bookmarks";

} // namespace

ManualPane::Row ManualPane::rowOf(const json& j) {
    return Row{j.value("topic", std::string()), j.value("title", std::string()), j.value("type", std::string()),
               j.value("snippet", std::string())};
}

void ManualPane::show(const std::string& topic) {
    open_ = true;
    wanted_ = topic;
}

void ManualPane::go(chat::Chat& chat, const std::string& topic) {
    status_ = "reading";
    chat.actNamed("doc.show", json{{"topic", topic}}, [this, topic](const Response& r) {
        if (!r.ok) {
            status_ = "err " + (r.err ? r.err->code + " " + r.err->message : std::string());
            return;
        }
        const json& j = resultOf(r);
        if (j.contains("found") && !j["found"].get<bool>()) {
            // No such topic: what is closest, the way Show Documentation offers candidates.
            status_ = "no topic " + topic;
            query_ = topic;
            searched_ = topic;
            candidates_.clear();
            for (const json& c : j.value("candidates", json::array())) {
                candidates_.push_back(rowOf(c));
            }
            total_ = static_cast<int>(candidates_.size());
            return;
        }
        if (!topic_.empty() && topic_ != j.value("topic", std::string())) {
            back_.push_back(topic_);
        }
        topic_ = j.value("topic", std::string());
        title_ = j.value("title", std::string());
        type_ = j.value("type", std::string());
        format_ = j.value("format", std::string("markdown"));
        body_ = j.value("body", std::string());
        previous_ = j.value("previous", std::string());
        next_ = j.value("next", std::string());
        status_.clear();
        doc_.invalidate();
    });
}

void ManualPane::search(chat::Chat& chat) {
    const unsigned long long mine = ++asked_;
    const std::string text = query_;
    if (text.empty()) {
        searched_.clear();
        candidates_.clear();
        total_ = 0;
        return;
    }
    chat.actNamed("doc.candidates", json{{"text", text}, {"limit", 30}}, [this, mine, text](const Response& r) {
        if (mine != asked_) {
            return; // a newer line is on its way
        }
        searched_ = text;
        candidates_.clear();
        total_ = 0;
        if (r.ok) {
            const json& j = resultOf(r);
            total_ = j.value("total", 0);
            for (const json& c : j.value("candidates", json::array())) {
                candidates_.push_back(rowOf(c));
            }
        }
    });
}

void ManualPane::askBooks(chat::Chat& chat) {
    booksAsked_ = true;
    chat.actNamed("doc.overview", json::object(), [this](const Response& r) {
        if (!r.ok) {
            booksAsked_ = false;
            status_ = "this world has no manual";
            return;
        }
        books_.clear();
        for (const json& b : resultOf(r).value("books", json::array())) {
            books_.push_back(Book{b.value("book", std::string()), b.value("title", std::string()), b.value("topics", 0)});
        }
    });
}

void ManualPane::askBook(chat::Chat& chat, const std::string& book) {
    chat.actNamed("doc.contents", json{{"book", book}}, [this, book](const Response& r) {
        if (r.ok) {
            std::vector<Row> rows;
            for (const json& t : resultOf(r).value("topics", json::array())) {
                rows.push_back(rowOf(t));
            }
            contents_[book] = std::move(rows);
        }
    });
}

std::vector<std::string> ManualPane::bookmarks(const model::Principals& principals, const std::string& self) const {
    std::vector<std::string> out;
    if (const model::PrincipalInfo* me = principals.get(self)) {
        if (auto it = me->fields.find(kBookmarks); it != me->fields.end() && it->second.value.is_array()) {
            for (const json& v : it->second.value) {
                if (v.is_string()) {
                    out.push_back(v.get<std::string>());
                }
            }
        }
    }
    return out;
}

bool ManualPane::rowButton(chat::Chat& chat, const Row& r, int index) {
    ImGui::PushID(index);
    const bool hit = ImGui::Selectable(r.title.empty() ? r.topic.c_str() : r.title.c_str(), r.topic == topic_);
    drive::hit("doc:" + r.topic, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered() && !r.snippet.empty()) {
        wrappedTooltip(r.topic + "\n" + r.snippet);
    }
    if (hit) {
        go(chat, r.topic);
    }
    ImGui::PopID();
    return hit;
}

void ManualPane::drawNav(chat::Chat& chat, const model::Principals& principals, const std::string& self) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##manual-search", "search the manual", &query_)) {
        search(chat);
    }
    drive::hit("input:manual-search", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    int row = 0;
    const std::vector<std::string> marks = bookmarks(principals, self);
    if (!marks.empty()) {
        dimText("bookmarks");
        for (const std::string& m : marks) {
            rowButton(chat, Row{m, m, "", ""}, row++);
        }
        ImGui::Separator();
    }
    if (!query_.empty()) {
        dimText("%d candidate%s", total_, total_ == 1 ? "" : "s");
        for (const Row& r : std::vector<Row>(candidates_)) { // a click below can change the list
            rowButton(chat, r, row++);
            ImGui::SameLine();
            dimText("%s", r.type.c_str());
        }
        return;
    }
    dimText("contents");
    for (const Book& b : std::vector<Book>(books_)) {
        const bool isOpen = openBooks_.count(b.book) != 0;
        ImGui::PushID(b.book.c_str());
        if (ImGui::Selectable((std::string(isOpen ? "- " : "+ ") + b.title).c_str())) {
            if (isOpen) {
                openBooks_.erase(b.book);
            } else {
                openBooks_.insert(b.book);
                if (contents_.count(b.book) == 0) {
                    askBook(chat, b.book);
                }
            }
        }
        drive::hit("book:" + b.book, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::PopID();
        if (isOpen) {
            if (auto it = contents_.find(b.book); it != contents_.end()) {
                ImGui::Indent();
                for (const Row& r : std::vector<Row>(it->second)) {
                    rowButton(chat, r, row++);
                }
                ImGui::Unindent();
            }
        }
    }
}

void ManualPane::drawTopic(chat::Chat& chat, const model::Principals& principals, const std::string& self) {
    if (status_ == "reading") { // a tape at the right of the button row, drawn over nothing: the page never moves
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float right = at.x + ImGui::GetContentRegionAvail().x;
        const float tall = ImGui::GetFrameHeight();
        hazardTape(ImGui::GetWindowDrawList(), ImVec2(right - ImGui::CalcTextSize("0").x * 14.0f, at.y + tall * 0.25f),
                   ImVec2(right, at.y + tall * 0.75f), ImGui::GetTime());
    } else if (!status_.empty()) {
        text(Ink::secondary, status_);
    }
    if (topic_.empty()) {
        dimText("search on the left, or open a book");
        return;
    }
    if (bracketButtonNamed("back", "manual-back", !back_.empty())) {
        const std::string to = back_.back();
        back_.pop_back();
        topic_.clear(); // going back is not a step forward: nothing to come back to
        go(chat, to);
    }
    ImGui::SameLine();
    if (bracketButtonNamed("previous", "manual-previous", !previous_.empty())) {
        go(chat, previous_);
    }
    ImGui::SameLine();
    if (bracketButtonNamed("next", "manual-next", !next_.empty())) {
        go(chat, next_);
    }
    ImGui::SameLine();
    std::vector<std::string> marks = bookmarks(principals, self);
    const bool marked = std::find(marks.begin(), marks.end(), topic_) != marks.end();
    if (bracketButtonNamed(marked ? "unbookmark" : "bookmark", "manual-bookmark", !self.empty(), marked)) {
        if (marked) {
            marks.erase(std::remove(marks.begin(), marks.end(), topic_), marks.end());
        } else {
            marks.push_back(topic_);
        }
        chat.setField(self, kBookmarks, json::array({"list", "string"}), marks, "both", [](const Response&) {});
    }
    text(Ink::strong, title_);
    ImGui::SameLine();
    dimText("%s // %s", type_.c_str(), topic_.c_str());
    ImGui::Separator();
    ImGui::BeginChild("manual-body", ImVec2(0, 0), ImGuiChildFlags_None);
    touchDragScroll();
    // A LaTeX topic is math in text, which the document already typesets.
    doc_.draw(body_, true, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize, "manual-doc");
    ImGui::EndChild();
}

void ManualPane::draw(chat::Chat& chat, const model::Principals& principals, const std::string& self, ImFont* mono) {
    if (!open_) {
        return;
    }
    if (!booksAsked_) {
        askBooks(chat);
    }
    if (!wanted_.empty()) {
        go(chat, wanted_);
        wanted_.clear();
    }
    const float ch = ImGui::CalcTextSize("0").x;
    ImGui::SetNextWindowSize(ImVec2(ch * 120, ImGui::GetTextLineHeight() * 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("DOCUMENTATION###manual", &open_, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(mono, 0.0f);
    drive::hit("pane:manual", ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                                                            ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
    ImGui::BeginChild("manual-nav", ImVec2(ch * 38, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_ResizeX);
    touchDragScroll();
    drawNav(chat, principals, self);
    ImGui::EndChild();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::BeginChild("manual-topic", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    drawTopic(chat, principals, self);
    ImGui::EndChild();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopFont();
    ImGui::End();
}

json ManualPane::driveState() const {
    json rows = json::array();
    for (const Row& r : candidates_) {
        rows.push_back(r.topic);
    }
    return json{{"open", open_}, {"topic", topic_}, {"title", title_}, {"type", type_}, {"format", format_},
                {"chars", body_.size()}, {"candidates", rows}, {"books", books_.size()}, {"status", status_}};
}

} // namespace ke::ui
