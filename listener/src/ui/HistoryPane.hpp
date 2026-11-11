// The interaction history: event rows as they land, merged with the chat's local notes (boot
// sequence, echoes, WHO rows, send errors). `sent` / `delivered` rows whose Message has been read
// show as chat lines (`<from> > text`); every other row keeps the raw form
// seq | at | [KIND] | stream | actor | body. Selection is inverse video; follows the tail until
// you scroll up.
#pragma once

#include "chat/Chat.hpp"
#include "chat/Search.hpp"
#include "model/EventStore.hpp"
#include "model/Principals.hpp"
#include "ui/Theme.hpp"

#include <functional>
#include <imgui.h>
#include <optional>
#include <vector>

namespace ke::ui {

class HistoryPane {
  public:
    void draw(const EventStore& store, const chat::Chat& chat, const Anim& anim, ImFont* mono);
    void drawBody(const EventStore& store, const chat::Chat& chat, const Anim& anim,
                  ImFont* mono);
    // The current Principal (the terminal's); the THIS PRINCIPAL toggle narrows the rows to its
    // streams (stream, streams, actor) and keeps the local notes only when it is our own.
    void setFocus(const std::string& principal, const std::string& self);
    // The fold that resolves ids to names (one rule everywhere: model::Principals::nameOf).
    void setNames(const model::Principals* names) { names_ = names; }

    // Search (sprint 1 MVP item): a one-line box above the rows ([ SEARCH ] in the menu bar and the
    // HISTORY header, Cmd-F); Return asks `onSearch` (the app issues `search {text limit 50}` and
    // falls back to chat::localSearch when the world has no such op); the hits list under the box
    // shows kind, name, field, the snippet with the match in inverse video and the seq; a click
    // opens a Principal's FIELDS (`onOpenObject`) or scrolls the rows to the seq; Escape closes.
    void openSearch();
    void closeSearch();
    bool searchOpen() const { return searchOpen_; }
    // Desktop: one line, the newest event, until asked for. The phone's tab is always whole.
    bool collapsed = false;
    void search(const std::string& text); // sets the query and asks onSearch
    void setQuery(const std::string& text) { query_ = text; } // offered in the box without searching
    void setHits(std::vector<chat::SearchHit> hits, bool local, std::string status);
    void scrollToSeq(Seq seq); // bring the row with that seq into view and select it
    std::function<void(const std::string&)> onSearch;
    std::function<void()> onNeedOlder; // the top of the rows is near: page in what precedes them
    std::function<void(const std::string&)> onOpenObject;
    std::function<void(const std::string&, const std::string&)> onOpenMessage; // Message id, event actor
    const std::vector<chat::SearchHit>& hits() const { return hits_; }
    bool hitsLocal() const { return local_; }
    const std::string& query() const { return query_; }
    const std::string& searchStatus() const { return status_; }

  private:
    struct Item {
        bool note = false;
        std::size_t index = 0;
    };
    void merge(const EventStore& store, const chat::Chat& chat, chat::Clock::time_point now);
    void drawEvent(const Event& e, const chat::Chat& chat, const Anim& anim, bool inverse);
    void drawNote(const chat::Note& n, const Anim& anim, bool inverse);

    bool matches(const Item& item, const EventStore& store) const;

    std::string nameOf(const std::string& id) const;
    const model::Principals* names_ = nullptr;
    std::vector<Item> items_;
    std::set<std::string> sentSeen_; // Messages with a `sent` row: their `delivered` row is one short line
    std::vector<std::size_t> view_; // indices into items_ when the filter is on
    std::size_t viewScanned_ = 0;
    std::string focus_;
    std::string self_;
    bool onlyFocus_ = false;
    bool showReads_ = false; // field-read and resolved: what every client does just by looking
    bool showRoutine_ = false; // a chat turn's bookkeeping (isRoutine)
    std::size_t rowsMerged_ = 0;
    std::uint64_t prependsSeen_ = 0;
    std::size_t shownBeforePrepend_ = 0; // set when rows arrive above: the scroll keeps its place
    std::size_t lastShown_ = 0;
    std::size_t notesMerged_ = 0;
    std::size_t lastCount_ = 0;
    bool stickToBottom_ = true;
    // Selection: anchor..selected_ (either order) over the shown rows; shift-click extends.
    int selected_ = -1;
    int selAnchor_ = -1;
    std::string rowText(const Item& item, const EventStore& store, const chat::Chat& chat) const;
    std::string selectionText(const EventStore& store, const chat::Chat& chat, bool filtered) const;
    void drawSearch();
    bool searchOpen_ = false;
    bool focusSearch_ = false;
    bool local_ = false;
    std::string query_;
    std::string status_;
    std::vector<chat::SearchHit> hits_;
    std::optional<Seq> scrollTo_;
};

} // namespace ke::ui
