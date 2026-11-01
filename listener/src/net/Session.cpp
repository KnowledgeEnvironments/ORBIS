#include "net/Session.hpp"

#include "util/Log.hpp"
#include "util/Product.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace ke {

// ---- Persisted ------------------------------------------------------------------------------

json Persisted::toJson() const {
    json j = {{"session", session}, {"acked", acked}};
    if (principal) {
        j["principal"] = *principal;
    }
    if (!world.empty()) {
        j["world"] = world;
    }
    if (!worldName.empty()) {
        j["world-name"] = worldName;
    }
    if (!lastAttached.empty()) {
        j["last-attached"] = lastAttached;
    }
    return j;
}

Persisted Persisted::fromJson(const json& j) {
    Persisted p;
    p.session = j.value("session", "");
    if (j.contains("principal") && j["principal"].is_string()) {
        p.principal = j["principal"].get<std::string>();
    }
    if (j.contains("acked") && j["acked"].is_object()) {
        for (auto& [k, v] : j["acked"].items()) {
            if (v.is_number_integer()) {
                p.acked[k] = v.get<Seq>();
            }
        }
    }
    p.world = j.value("world", "");
    p.worldName = j.value("world-name", "");
    p.lastAttached = j.value("last-attached", "");
    return p;
}

FileSessionStore::FileSessionStore(std::filesystem::path path, std::string worldUrl)
    : path_(std::move(path)), key_(worldKey(worldUrl)) {}

std::filesystem::path FileSessionStore::defaultPath() {
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home ? home : ".";
    return base / "Library" / "Application Support" / kProductName / "session.json";
}

std::string FileSessionStore::worldKey(const std::string& url) {
    const std::size_t q = url.find('?');
    std::string k = q == std::string::npos ? url : url.substr(0, q);
    while (!k.empty() && k.back() == '/') {
        k.pop_back();
    }
    return k;
}

json FileSessionStore::readFile() const {
    std::ifstream in(path_);
    if (!in) {
        return json::object();
    }
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        log::warn("session: ignoring unreadable {}", path_.string());
        return json::object();
    }
    return j;
}

std::optional<Persisted> FileSessionStore::load() {
    const json j = readFile();
    if (j.empty()) {
        return std::nullopt;
    }
    if (key_.empty()) {
        return j.contains("session") ? std::optional<Persisted>(Persisted::fromJson(j)) : std::nullopt;
    }
    if (j.contains("worlds") && j["worlds"].is_object()) {
        const json& worlds = j["worlds"];
        if (worlds.contains(key_) && worlds[key_].is_object()) {
            return Persisted::fromJson(worlds[key_]);
        }
        return std::nullopt;
    }
    if (j.contains("session")) { // the single-entry shape: it was this machine's one world
        log::info("session: adopting the single-entry session file for {}", key_);
        Persisted p = Persisted::fromJson(j);
        save(p); // rewritten per world at once, so another world does not adopt it too
        return p;
    }
    return std::nullopt;
}

void FileSessionStore::save(const Persisted& p) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    json out;
    if (key_.empty()) {
        out = p.toJson();
    } else {
        const json old = readFile();
        out = json::object();
        if (old.contains("worlds") && old["worlds"].is_object()) {
            out["worlds"] = old["worlds"];
        } else {
            out["worlds"] = json::object(); // a single-entry file is replaced; its entry is `p` now
        }
        out["worlds"][key_] = p.toJson();
    }
    std::filesystem::path tmp = path_;
    tmp += ".tmp";
    {
        std::ofstream out_(tmp, std::ios::trunc);
        out_ << out.dump(2) << '\n';
    }
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
        log::warn("session: could not write {}: {}", path_.string(), ec.message());
    }
}

std::vector<FileSessionStore::KnownWorld> FileSessionStore::listWorlds(const std::filesystem::path& path) {
    std::vector<KnownWorld> out;
    std::ifstream in(path);
    if (!in) {
        return out;
    }
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("worlds") || !j["worlds"].is_object()) {
        return out;
    }
    for (auto& [url, entry] : j["worlds"].items()) {
        if (entry.is_object()) {
            out.push_back(KnownWorld{url, Persisted::fromJson(entry)});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const KnownWorld& a, const KnownWorld& b) {
        return a.entry.lastAttached > b.entry.lastAttached; // ISO strings sort by time; "" last
    });
    return out;
}

// ---- Session --------------------------------------------------------------------------------

Session::Session(SessionConfig config, SessionStore& store, EventStore& events, Send send)
    : config_(std::move(config)), store_(store), events_(events), send_(std::move(send)) {
    if (auto p = store_.load()) {
        persisted_ = *p;
        log::info("session: loaded persisted session {} with {} acked cursor(s)", persisted_.session,
                  persisted_.acked.size());
    }
    for (const std::string& sub : config_.subs) {
        SubState s;
        s.window = config_.window;
        if (auto it = persisted_.acked.find(sub); it != persisted_.acked.end()) {
            s.acked = it->second;
            s.cursor = it->second; // everything up to acked has landed in some earlier run
        }
        subs_[sub] = s;
    }
    for (const std::string& sub : config_.transientSubs) {
        SubState s;
        s.window = config_.window;
        s.transient = true;
        subs_[sub] = s; // never from persisted_: this process has not seen anything yet
    }
}

std::string Session::nextId() {
    return "r" + std::to_string(++counter_);
}

void Session::noteHead(Seq seq) {
    if (seq > head_) {
        head_ = seq;
    }
}

// protocol.md "Session": attach {token principal? session? subs? client-kind?}; the reattach form
// carries the known session and {sub: acked}.
void Session::onOpen() {
    attaching_ = true;
    attached_ = false;
    pending_.clear();
    if (config_.hello) {
        const std::string id = nextId();
        pending_[id] = Pending{"hello", "", {}, true};
        send_(Request{id, "hello", json::object()});
        return; // attach follows the answer, whatever it is
    }
    sendAttach();
}

// `hello` answered: remember the world; a different id under the same URL means the saved
// session belongs to another world, so it is dropped before attaching (a fresh start, said so).
void Session::onHello(const Response& r) {
    if (r.ok && r.result.is_object()) {
        const json& res = r.result;
        world_.id = res.contains("world") && res["world"].is_string() ? res["world"].get<std::string>()
                    : res.contains("id") && res["id"].is_string()     ? res["id"].get<std::string>()
                                                                        : std::string();
        world_.name = res.value("name", "");
        world_.url = res.value("url", "");
        world_.head = res.contains("head") && res["head"].is_number_integer() ? res["head"].get<Seq>() : r.seq;
        world_.formats = res.contains("format-versions") ? res["format-versions"] : json();
        noteHead(world_.head);
        if (!world_.id.empty()) {
            if (!persisted_.world.empty() && persisted_.world != world_.id && !persisted_.session.empty()) {
                freshReason_ = "the world under " + (world_.url.empty() ? std::string("this URL") : world_.url) +
                               " changed (" + persisted_.world.substr(0, 8) + " -> " + world_.id.substr(0, 8) +
                               "): starting a fresh session";
                log::warn("session: {}", freshReason_);
                persisted_ = Persisted{};
                for (auto& [sub, state] : subs_) {
                    state.cursor.reset();
                    state.acked.reset();
                    state.subscribed = false;
                }
            }
            persisted_.world = world_.id;
            persisted_.worldName = world_.name;
        }
        log::info("session: hello from world {} '{}' head {}", world_.id.substr(0, 8), world_.name, world_.head);
    } else {
        log::info("session: no hello from this world ({}); attaching anyway",
                  r.err ? r.err->code : std::string("no answer"));
    }
    sendAttach();
}

void Session::sendAttach() {
    json args = {{"token", config_.token}, {"client-kind", config_.clientKind}};
    if (config_.principal) {
        args["principal"] = *config_.principal;
    }
    if (!persisted_.session.empty()) {
        args["session"] = persisted_.session;
        json subs = json::object();
        for (const auto& [sub, state] : subs_) {
            if (state.acked) {
                subs[sub] = *state.acked;
            }
        }
        if (!subs.empty()) {
            args["subs"] = subs;
        }
    }
    const std::string id = nextId();
    pending_[id] = Pending{"attach", "", {}};
    log::info("session: attach ({})",
              persisted_.session.empty() ? "fresh" : "reattach " + persisted_.session);
    send_(Request{id, "attach", args});
}

void Session::onClose() {
    attached_ = false;
    attaching_ = false;
    for (auto& [sub, state] : subs_) {
        state.subscribed = false;
    }
    if (!pending_.empty()) {
        log::warn("session: {} request(s) lost with the connection", pending_.size());
        pending_.clear();
    }
}

void Session::onFrame(const Frame& frame) {
    switch (kindOf(frame)) {
    case FrameKind::Response:
        onResponse(std::get<Response>(frame));
        break;
    case FrameKind::Push:
        onPush(std::get<Push>(frame));
        break;
    case FrameKind::Pane:
        if (paneData) {
            paneData(std::get<PaneData>(frame));
        }
        break;
    case FrameKind::Request:
    case FrameKind::Ack:
        // Client-bound only (protocol.md "Frames"): a world never sends these.
        log::warn("session: ignoring {} frame from the world", name(kindOf(frame)));
        break;
    }
}

std::string Session::request(const std::string& op, json args, ResponseHandler handler, bool quiet) {
    const std::string id = nextId();
    pending_[id] = Pending{op, "", std::move(handler), quiet};
    send_(Request{id, op, std::move(args)});
    return id;
}

void Session::onResponse(const Response& r) {
    noteHead(r.seq);
    auto it = pending_.find(r.id);
    if (it == pending_.end()) {
        log::warn("session: response for unknown request id {}", r.id);
        return;
    }
    Pending pending = std::move(it->second);
    pending_.erase(it);

    if (!r.ok) {
        const Err& e = r.err.value_or(Err{"invalid", "missing err", std::nullopt});
        if (pending.quiet) {
            log::info("session: {} answered {} {} (expected)", pending.op, e.code, e.message);
        } else {
            lastError_ = pending.op + ": " + e.code + " (" + e.message + ")";
            log::warn("session: {} failed: {} {}", pending.op, e.code, e.message);
        }
    }

    if (pending.op == "hello") {
        onHello(r);
    } else if (pending.op == "attach") {
        attaching_ = false;
        if (r.ok) {
            onAttached(r);
            attachError_ = {};
            attachFailures_ = 0;
        } else {
            const Err& e = r.err.value_or(Err{"invalid", "missing err", std::nullopt});
            attachError_ = {e.code, e.message};
            ++attachFailures_;
        }
    } else if (pending.op == "subscribe") {
        auto sub = subs_.find(pending.sub);
        if (sub != subs_.end() && r.ok) {
            sub->second.subscribed = true;
            sub->second.asOf = r.result.value("as-of", r.seq);
            noteHead(sub->second.asOf);
        }
    }
    if (pending.handler) {
        // a reply of an unexpected shape costs that one action, never the client
        try {
            pending.handler(r);
        } catch (const std::exception& e) {
            log::error("response to {} not understood: {}", pending.op, e.what());
        }
    }
}

void Session::onAttached(const Response& r) {
    const json& res = r.result;
    const bool resumed = res.value("resumed", false);
    persisted_.session = res.value("session", "");
    if (res.contains("principal") && res["principal"].is_string()) {
        persisted_.principal = res["principal"].get<std::string>();
    }
    attached_ = true;
    ++attaches_;
    if (resumed) {
        ++resumes_;
    }
    noteHead(res.value("seq", r.seq));
    {
        char buf[32];
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&t, &tm);
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
        persisted_.lastAttached = buf;
    }
    log::info("session: attached as {} session {} resumed={} head={}", persisted_.principal.value_or("?"),
              persisted_.session, resumed, head_);
    store_.save(persisted_);

    for (auto& [sub, state] : subs_) {
        if (resumed && state.acked && !farBehind(state)) {
            // Known session: the world restored the sub with its cursor at our acked seq.
            state.subscribed = true;
        } else if (state.transient && config_.deferTransient && !state.acked) {
            // The owner starts it after deciding between a snapshot boot and a replay from 0.
            state.startAfter.reset();
        } else {
            // Unknown session (or a sub the world never saw): subscribe with after = acked.
            subscribe(sub);
        }
    }
}

bool Session::attachRefused() const {
    const std::string& c = attachError_.first;
    return c == "unauthenticated" || c == "unresolved" || c == "invalid";
}

void Session::subscribeTransient(const std::string& sub, Seq after) {
    auto it = subs_.find(sub);
    if (!attached_ || it == subs_.end() || !it->second.transient || it->second.subscribed) {
        return;
    }
    if (it->second.acked) {
        subscribe(sub); // an in-process reattach: resume at acked, never earlier
        return;
    }
    if (it->second.startAfter) {
        return; // already asked for on this attach
    }
    it->second.startAfter = after;
    subscribe(sub);
}

bool Session::transientPending(const std::string& sub) const {
    auto it = subs_.find(sub);
    return attached_ && it != subs_.end() && it->second.transient && !it->second.subscribed && !it->second.acked &&
           config_.deferTransient;
}

// protocol.md "Subscriptions": subscribe {sub after? window?}; after = acked keeps delivery at
// exactly acked+1 .. head on both reattach paths.
void Session::subscribeFiltered(const std::string& sub, json filter, Seq after) {
    if (!attached_) {
        return;
    }
    SubState& state = subs_[sub];
    state.transient = true;
    state.startAfter = after;
    state.filter = std::move(filter);
    subscribe(sub);
}

bool Session::farBehind(const SubState& state) const {
    return config_.historyWindow > 0 && !state.transient && !state.filter.is_object() && state.received == 0 &&
           head_ - state.acked.value_or(0) > config_.historyWindow;
}

void Session::subscribe(const std::string& sub) {
    SubState& state = subs_[sub];
    json args = {{"sub", sub}, {"window", state.window}};
    if (state.filter.is_object()) {
        args.update(state.filter);
    }
    if (farBehind(state)) {
        state.acked = head_ - config_.historyWindow;
        state.cursor = state.acked;
    }
    if (state.acked) {
        args["after"] = *state.acked;
    } else if (state.transient) {
        args["after"] = state.startAfter.value_or(0); // the whole log for a replay, the tail after a snapshot
    }
    const std::string id = nextId();
    pending_[id] = Pending{"subscribe", sub, {}};
    send_(Request{id, "subscribe", args});
}

void Session::onPush(const Push& p) {
    // Off the record: a fork's own seqs, never acked, never kept, and never a stale sub.
    if (p.sub.rfind("scratch-", 0) == 0) {
        if (scratchPush) {
            scratchPush(p);
        }
        return;
    }
    noteHead(p.asOf);
    auto it = subs_.find(p.sub);
    if (it == subs_.end()) {
        // A sub this process never made: the resumed world session still carries a previous
        // process's transient sub. Drop it on the world side once, then ignore it.
        if (unsubscribedUnknown_.insert(p.sub).second) {
            log::info("session: unsubscribing stale sub {} left by a previous process ({} events)", p.sub,
                      p.events.size());
            request("unsubscribe", json{{"sub", p.sub}}, {}, true);
        }
        return;
    }
    SubState& state = it->second;
    if (state.cursor && p.asOf < *state.cursor) {
        events_.noteRegression(p.sub, *state.cursor, p.asOf);
    }
    // p.from above our cursor is not a loss: the world advances its cursor over events its filter
    // dropped without pushing them (pump-sub keeps the last seq READ), so the range cursor+1..from
    // is the same filtered hole that append() counts when the push's first event lands.
    events_.notePush(p.sub);
    state.asOf = p.asOf;
    state.lagging = p.lagging;
    Seq last = -1;
    for (const Event& e : p.events) {
        ++state.received;
        if (state.cursor && e.seq <= *state.cursor) {
            ++state.duplicates;
            continue;
        }
        events_.append(p.sub, e);
        state.cursor = e.seq;
        last = e.seq;
    }
    if (p.events.empty()) {
        return;
    }
    // Rows have landed: ack the highest seq we hold so the world advances its window.
    const Seq ackSeq = state.cursor.value_or(last);
    if (ackSeq < 0) {
        return;
    }
    if (!state.acked || ackSeq > *state.acked) {
        state.acked = ackSeq;
        if (!state.transient) {
            persisted_.acked[p.sub] = ackSeq;
            store_.save(persisted_);
        }
    }
    ++state.acksSent;
    send_(Ack{p.sub, ackSeq});
}

} // namespace ke
