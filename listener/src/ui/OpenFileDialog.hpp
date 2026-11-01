// OPEN FILE: a mount, and a path that completes as you type from the world's own listing. Tab extends the
// line, Up and Down walk the candidates, Enter opens, Escape cancels, and on a phone a tap does it all.
#pragma once

#include "chat/Chat.hpp"
#include "model/Principals.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ke::ui {

class OpenFileDialog {
  public:
    void open() { wanted_ = true; onPick_ = nullptr; }
    // The same dialog, handing back the mount and path instead of opening them.
    void pick(std::function<void(const std::string& mount, const std::string& path)> onPick) {
        wanted_ = true;
        onPick_ = std::move(onPick);
    }
    void draw(chat::Chat& chat, const model::Principals& principals);
    nlohmann::json driveState() const;

    // The longest start every name shares, the way a shell completes.
    static std::string commonPrefix(const std::vector<std::string>& names);
    // Does this line name its own way there: an absolute path, or one that starts at a mount?
    static bool namesItsOwnMount(const std::string& path, const std::vector<std::string>& mounts);

  private:
    struct Mount {
        std::string id, name;
    };
    std::vector<Mount> mountsOf(const model::Principals& principals) const;
    void ask(chat::Chat& chat);
    void choose(const std::string& entry); // moves the line onto a candidate
    void setLine(const std::string& line);
    void finish(const std::string& path);
    std::string parentOfLine() const;

    bool wanted_ = false;
    std::function<void(const std::string&, const std::string&)> onPick_;
    std::string mount_;   // the chosen Adapter's id
    std::string path_;    // the line
    std::string asked_;   // what `entries_` answers: mount and line
    unsigned long long seq_ = 0;
    std::string dir_;     // the directory `entries_` lists, as the world read it
    std::vector<std::string> entries_;
    int selected_ = -1;
    bool refocus_ = false;
    bool close_ = false;
    bool tabPressed_ = false;
    bool rewrite_ = false; // the line changed while its field held the buffer
    int step_ = 0;        // Up or Down, from the line's callback
    friend struct OpenFileLine;
};

} // namespace ke::ui
