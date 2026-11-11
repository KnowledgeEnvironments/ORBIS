// Attach / reattach bookkeeping (docs/protocol.md "Session" and "Subscriptions").
//
//   open  -> attach {token principal? session? subs{sub: acked} client-kind}
//   ok    -> resumed=true : subs are restored by the world at our acked seq, nothing to send
//            resumed=false: re-subscribe every sub with after = acked
//   push  -> dedupe by seq (drop seq <= cursor), land rows in the EventStore, then ack
//
// The client's acked seq is authoritative, so it is persisted after every ack. No network here:
// frames go out through a callback and come in through onFrame(), which keeps this testable.
#pragma once

#include "model/EventStore.hpp"
#include "net/Protocol.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>

namespace ke {

struct SessionConfig {
    std::string token;
    std::optional<std::string> principal;
    std::vector<std::string> subs{"history"}; // one unfiltered cursor over the log (sprint 0)
    // Subs whose acked seq is not persisted: a new process subscribes them from seq 0 (the fold
    // over the whole log), a reattach within the process resumes them at acked like any other.
    std::vector<std::string> transientSubs;
    // When set, a transient sub with nothing acked is not subscribed at attach: the owner decides
    // where it starts (a snapshot boot subscribes it `after snapshot.seq`, a replay from 0) with
    // subscribeTransient(). A reattach with an acked seq resumes it like any other sub.
    bool deferTransient = false;
    Seq window = 256;
    // A persisted sub more than this far behind the head starts this far behind it: the older
    // rows are paged on demand (EventStore::prepend), so an attach costs the same at any age of a
    // world. 0 keeps the whole tail. Only a process's first subscribe jumps: a reattach it already
    // holds rows for stays contiguous, acked+1 .. head.
    Seq historyWindow = 0;
    std::string clientKind = "listener";
    // Ask the world who it is before attaching (`hello`, a pre-attach op): its id names the saved
    // session's world, so a different world under the same URL (a `--fresh` data dir, another
    // machine) starts a fresh session instead of resuming a stranger's acked seq. A world without
    // the op answers err and the attach goes ahead as before.
    bool hello = false;
};

// What `hello` says about the world behind the socket (sprint 2: several worlds).
struct WorldInfo {
    std::string id;
    std::string name;
    std::string url; // the URL the world believes it has (world/url), "" when it names none
    Seq head = 0;
    json formats;    // format-versions as sent
    bool operator==(const WorldInfo&) const = default;
};

// What survives a process restart: {session, principal, acked per sub}.
struct Persisted {
    std::string session;
    std::optional<std::string> principal;
    std::map<std::string, Seq> acked;
    // The world this entry belongs to, as `hello` named it, for the world picker and the fresh
    // start when the id changes under a URL.
    std::string world;        // world id
    std::string worldName;    // world/name
    std::string lastAttached; // ISO-8601 of the last successful attach

    json toJson() const;
    static Persisted fromJson(const json& j);
    bool operator==(const Persisted&) const = default;
};

class SessionStore {
  public:
    virtual ~SessionStore() = default;
    virtual std::optional<Persisted> load() = 0;
    virtual void save(const Persisted& p) = 0;
};

class MemorySessionStore : public SessionStore {
  public:
    std::optional<Persisted> load() override { return value; }
    void save(const Persisted& p) override {
        value = p;
        ++saves;
    }
    std::optional<Persisted> value;
    int saves = 0;
};

// ~/Library/Application Support/ORBIS/session.json by default. One file holds one entry per
// world: {"worlds": {"<url without query>": {session principal acked}}}, so attaching to a second
// world never resumes with the first one's acked seq. A file in the older single-entry shape
// {session principal acked} is read as the entry of the current world and rewritten in the new
// shape on the next save. With an empty world key the store keeps the single-entry shape.
class FileSessionStore : public SessionStore {
  public:
    explicit FileSessionStore(std::filesystem::path path = defaultPath(), std::string worldUrl = "");
    static std::filesystem::path defaultPath();
    // The key an url is stored under: scheme, host, port and path; the query (format=) dropped.
    static std::string worldKey(const std::string& url);
    std::optional<Persisted> load() override;
    void save(const Persisted& p) override;
    const std::filesystem::path& path() const { return path_; }
    const std::string& key() const { return key_; }
    // Every world the file knows: {url key, its entry}, most recently attached first.
    struct KnownWorld {
        std::string url;
        Persisted entry;
    };
    static std::vector<KnownWorld> listWorlds(const std::filesystem::path& path);

  private:
    json readFile() const; // the whole file, or an empty object
    std::filesystem::path path_;
    std::string key_;
};

// Per-subscription window accounting. cursor = last seq landed, acked = last seq acknowledged,
// asOf = world head as of the last push. unacked = cursor - acked, lag = asOf - cursor.
struct SubState {
    std::optional<Seq> cursor;
    std::optional<Seq> acked;
    Seq asOf = 0;
    Seq window = 256;
    bool subscribed = false;
    bool lagging = false;
    std::uint64_t received = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t acksSent = 0;
    bool transient = false; // not persisted; subscribed with after 0 when nothing is acked yet
    std::optional<Seq> startAfter; // a deferred transient sub's first `after` (subscribeTransient)
    json filter;                   // merged into its subscribe: {stream, kinds}

    Seq unacked() const { return cursor && acked ? *cursor - *acked : (cursor ? *cursor + 1 : 0); }
    Seq lag() const { return cursor ? asOf - *cursor : 0; }
};

class Session {
  public:
    using Send = std::function<void(const Frame&)>;
    using ResponseHandler = std::function<void(const Response&)>;

    Session(SessionConfig config, SessionStore& store, EventStore& events, Send send);

    // Off-the-record pushes go here and nowhere else.
    std::function<void(const Push&)> scratchPush;
    // A sub's cursor fell below the first seq the world still holds (`gap` on a push): the world
    // cannot send what was in between, so the owner folds again from a snapshot.
    std::function<void(const std::string& sub, Seq firstHeld)> logGap;
    // A pane's screen bytes (docs/panes.md): off the log, so they bypass every sub and ack.
    std::function<void(const PaneData&)> paneData;

    // Transport hooks, called on the UI thread.
    void onOpen();
    void onClose();
    void onFrame(const Frame& frame);

    // Any other op (bind, read, ...); the handler runs when the response arrives. `quiet` marks a
    // request whose err is expected (the sprint-0 bind stubs): it is handed to the handler but not
    // promoted to lastError() nor logged as a warning.
    std::string request(const std::string& op, json args, ResponseHandler handler = {}, bool quiet = false);
    // Start a deferred transient sub at `after` (0 for the whole log). No-op unless attached and
    // the sub is one of config.transientSubs that is not subscribed yet.
    void subscribeTransient(const std::string& sub, Seq after);
    // A sub of one narrow interest, from `after`, its filter merged into the subscribe. Not kept
    // across a restart; call it on each attach.
    void subscribeFiltered(const std::string& sub, json filter, Seq after);
    // True when a deferred transient sub still waits for subscribeTransient() on this attach.
    bool transientPending(const std::string& sub) const;

    // Observable state for the Session pane.
    bool attached() const { return attached_; }
    bool attaching() const { return attaching_; }
    const std::string& sessionId() const { return persisted_.session; }
    const std::optional<std::string>& principal() const { return persisted_.principal; }
    Seq head() const { return head_; }
    const std::map<std::string, SubState>& subs() const { return subs_; }
    const std::string& lastError() const { return lastError_; }
    // The last attach that the world refused: {code, message}; empty code when the last attach
    // succeeded. attachRefused() is true for a reason that will not change by retrying
    // (unauthenticated: the token; unresolved: the principal; invalid: this client's frame).
    const std::pair<std::string, std::string>& attachError() const { return attachError_; }
    bool attachRefused() const;
    int attachFailures() const { return attachFailures_; }
    int attaches() const { return attaches_; }
    int resumes() const { return resumes_; }
    const Persisted& persisted() const { return persisted_; }
    const SessionConfig& config() const { return config_; }
    // The world as `hello` described it on this socket; empty id until it answered (or never,
    // for a world without the op).
    const WorldInfo& world() const { return world_; }
    // Non-empty when the saved session was dropped because the world's id changed under the URL.
    const std::string& freshReason() const { return freshReason_; }

  private:
    struct Pending {
        std::string op;
        std::string sub; // for subscribe
        ResponseHandler handler;
        bool quiet = false;
    };

    void onResponse(const Response& r);
    bool farBehind(const SubState& state) const;
    void onPush(const Push& p);
    void onAttached(const Response& r);
    void onHello(const Response& r);
    void sendAttach();
    void subscribe(const std::string& sub);
    void noteHead(Seq seq);
    std::string nextId();

    SessionConfig config_;
    SessionStore& store_;
    EventStore& events_;
    Send send_;
    Persisted persisted_;
    std::map<std::string, SubState> subs_;
    std::set<std::string> unsubscribedUnknown_; // stale subs of earlier processes, dropped once
    std::map<std::string, Pending> pending_;
    std::string lastError_;
    std::pair<std::string, std::string> attachError_;
    WorldInfo world_;
    std::string freshReason_;
    int attachFailures_ = 0;
    Seq head_ = 0;
    bool attaching_ = false;
    bool attached_ = false;
    int attaches_ = 0;
    int resumes_ = 0;
    unsigned counter_ = 0;
};

} // namespace ke
