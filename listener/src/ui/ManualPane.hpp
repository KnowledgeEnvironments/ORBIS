// The manual, as the Document Examiner has it: a reading pane, candidates, contents and bookmarks. The
// world holds the topics and answers doc.show, doc.candidates, doc.overview and doc.contents, the same
// commands an agent uses, so what a person reads and what an agent reads cannot differ.
#pragma once

#include "chat/Chat.hpp"
#include "model/Principals.hpp"
#include "modules/RenderedDoc.hpp"

#include <imgui.h>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace ke::ui {

class ManualPane {
  public:
    void toggle() { open_ = !open_; }
    bool open() const { return open_; }
    // Opens the pane at a topic, from a symbol's menu or the HELP menu.
    void show(const std::string& topic);
    void draw(chat::Chat& chat, const model::Principals& principals, const std::string& self, ImFont* mono);
    nlohmann::json driveState() const;

  private:
    struct Row {
        std::string topic, title, type, snippet;
    };
    struct Book {
        std::string book, title;
        int topics = 0;
    };
    static Row rowOf(const nlohmann::json& j);
    void go(chat::Chat& chat, const std::string& topic);
    void search(chat::Chat& chat);
    void askBooks(chat::Chat& chat);
    void askBook(chat::Chat& chat, const std::string& book);
    void drawNav(chat::Chat& chat, const model::Principals& principals, const std::string& self);
    void drawTopic(chat::Chat& chat, const model::Principals& principals, const std::string& self);
    bool rowButton(chat::Chat& chat, const Row& r, int index);
    std::vector<std::string> bookmarks(const model::Principals& principals, const std::string& self) const;

    bool open_ = false;
    bool booksAsked_ = false;
    std::string wanted_;      // a topic asked for from outside the pane
    std::string query_;       // the search line
    std::string searched_;    // what `candidates_` answers
    unsigned long long asked_ = 0; // the newest search: an older answer is dropped
    std::vector<Row> candidates_;
    int total_ = 0;
    std::vector<Book> books_;
    std::map<std::string, std::vector<Row>> contents_; // by book, once opened
    std::set<std::string> openBooks_;

    // the topic on show
    std::string topic_, title_, type_, format_, body_, previous_, next_, status_;
    std::vector<std::string> back_; // where BACK goes
    modules::RenderedDoc doc_;
};

} // namespace ke::ui
