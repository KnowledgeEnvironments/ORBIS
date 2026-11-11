// The network + session + store + chat stack with no SDL in it: the App wraps it in a window, --smoke
// runs it bare. Inbound frames are drained on the caller's thread by drain().
#pragma once

#include "chat/Symbols.hpp"
#include "chat/Notifications.hpp"
#include "chat/Unsafe.hpp"
#include "app/Options.hpp"
#include "chat/Chat.hpp"
#include "model/Actions.hpp"
#include "model/EventStore.hpp"
#include "model/Evaluations.hpp"
#include "model/PaneFold.hpp"
#include "model/Principals.hpp"
#include "model/Snapshot.hpp"
#include "model/Tasks.hpp"
#include "model/Tokens.hpp"
#include "net/Session.hpp"
#include "net/WsClient.hpp"
#include "ui/SessionPane.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace ke {

// How the folds were brought up on this attach (SESSION pane's BOOT row, drive `state.boot`):
// a snapshot (`history {snapshot true}` paged through `next`, then the tail subscribed after its
// seq) or a replay of the whole log from seq 0 when the world has no snapshot op.
struct BootInfo {
    enum class Mode { Pending, Snapshot, Replay };
    Mode mode = Mode::Pending;
    Seq seq = -1;             // the snapshot's seq (the tail starts after it)
    std::size_t objects = 0;  // objects seeded
    int pages = 0;            // snapshot pages fetched
    double ms = -1;           // attach -> folds seeded (snapshot) or -> subscribed (replay)
    double foldReadyMs = -1;  // attach -> the fold sub's cursor reached the head seen at attach
    Seq headAtAttach = -1;
    std::string reason;       // why a replay: the op's err code, or "" for a snapshot
    std::string line() const; // `snapshot @ seq 8014 (312 objects, 2 pages, 140 ms)` / `replay from 0 (…)`
};

class Runtime {
  public:
    Runtime(const Options& opts, std::unique_ptr<SessionStore> store);
    ~Runtime();

    // Starts the client; with no token nobody attaches, and SESSION says why.
    void start();
    // SHOW DELETED: a snapshot leaves retired objects out, so ask the world for them.
    void fetchRetired(json next = nullptr);
    void fetchOlderHistory(); // one page of rows above the oldest one held
    // Returns true when anything was processed (a frame, a state change), i.e. a redraw is due.
    bool drain();
    // Sprint-0 stubs (spec 15.6): send the two bind ops once per call.
    void sendBinds();

    EventStore events;           // rows: the "history" sub (recent, resumable, persisted acked)
    model::Principals principals; // folded from the "fold" sub: the whole log from seq 0
    model::Evaluations evaluations; // same source: Evaluations, Serializations, their cost and ops
    model::Tasks tasks;             // same source: Tasks (approval gates, shell runs)
    chat::SymbolTable symbols;      // every name and id the folds know, for links and `@` completion
    model::Actions actions;         // same source: the world's named actions (dialogs and menus)
    model::Tokens tokens;           // same source: token Objects (the DEVICES tab)
    model::PaneFold panes;          // same source: panes (pane/* Fields, who drove them)
    std::vector<chat::UnsafeEntry> unsafe; // world/unsafe, the red indicator's only source
    chat::Notifications notifications;     // what the world's Principals have told this person
    chat::MessageUnread unread;            // Messages that arrived while the person was elsewhere
    std::unique_ptr<SessionStore> store;
    WsClient client;
    Session session;
    std::vector<ui::BindRow> binds;
    chat::Chat chat; // after session: route discovery, the input line, message rows, notes
    bool bindsSentThisAttach = false;
    BootInfo boot;
    const std::string& foldSub() const { return foldSub_; }
    // The earliest moment something changes without a network event: a chat deadline or the end
    // of a reconnect backoff (the App sleeps until then).
    std::optional<std::chrono::steady_clock::time_point> nextDeadline(std::chrono::steady_clock::time_point now) const;
    // Reconnect accounting for the SESSION pane: closes seen, the current backoff, why we stopped.
    int reconnects() const { return reconnects_; }
    std::optional<std::chrono::steady_clock::time_point> backoffUntil() const { return backoffUntil_; }
    const std::string& stopReason() const { return stopReason_; }

  private:
    void startBoot();
    void requestSnapshotPage(json args);
    void onLogGap(const std::string& sub, Seq firstHeld);
    void finishBoot(BootInfo::Mode mode, const std::string& reason);
    void watchUnsafe();
    void watchNotifications();
    int notifyAttach_ = -1;
    std::string unsafeWatched_; // the world this attach watches
    int unsafeAttach_ = -1;
    bool trace_ = false; // LISTENER_TRACE=1 logs every push (headless verification)
    bool foldChanged_ = false;
    std::string foldSub_;
    bool bootInFlight_ = false;
    bool olderInFlight_ = false;
    int attachSeen_ = 0;
    std::chrono::steady_clock::time_point attachAt_;
    // Closes within the last 10 s. Three of them mean the world keeps closing us (another
    // listener holding the same session id, a world restarting in a loop): stop, back off
    // 1 s → 2 s → 4 s … 30 s, then start again. A refused attach stops for good.
    std::deque<std::chrono::steady_clock::time_point> closes_;
    int reconnects_ = 0;
    int backoffSteps_ = 0;
    std::optional<std::chrono::steady_clock::time_point> backoffUntil_;
    std::string stopReason_;
    bool noToken_ = false;
    std::string tokenProblem_;
    int attachFailuresSeen_ = 0;
};

// Resolve --token / --token-file / default ../data/person.token (relative to the repo). Empty
// with `problem` set when there is none: the client stays unattached rather than fall back.
std::string readToken(const std::string& token, const std::string& tokenFile, const char* argv0, std::string& problem);

} // namespace ke
