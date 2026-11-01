#include "app/Runtime.hpp"

#include "util/Log.hpp"
#include "workspace/LocalFs.hpp"
#include "workspace/Shell.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <filesystem>
#include <fstream>

namespace ke {

namespace {

std::string foldSubName() {
    static const std::string name = [] {
        std::random_device rd;
        char buf[16];
        std::snprintf(buf, sizeof buf, "fold-%08x", rd());
        return std::string(buf);
    }();
    return name;
}

// What a client holds of the log at attach; History pages back from here as it is scrolled.
constexpr Seq kHistoryWindow = 2000;
constexpr int kHistoryPage = 500;

SessionConfig configFrom(const Options& o) {
    SessionConfig c;
    c.token = o.token;
    c.principal = o.principal;
    // The Principals fold needs the whole log. The sub is transient (never persisted) and named
    // per process: a resumed session would otherwise hand back the previous process's "fold"
    // cursor and this process would only see the tail. It is deferred: the boot seeds the folds
    // from a snapshot and subscribes the tail after it, or replays from 0 when the world has none.
    c.transientSubs = {foldSubName()};
    c.deferTransient = true;
    c.historyWindow = kHistoryWindow;
    c.hello = true; // who is this world: a changed id under the URL starts a fresh session
    return c;
}

constexpr int kSnapshotPage = 500;

std::string withFormat(const std::string& url, const std::string& format) {
    if (url.find("format=") != std::string::npos) {
        return url;
    }
    return url + (url.find('?') == std::string::npos ? "?" : "&") + "format=" + format;
}

} // namespace

std::string BootInfo::line() const {
    auto fmtMs = [](double v) { return v < 0 ? std::string("…") : std::to_string(static_cast<long long>(v)) + " ms"; };
    switch (mode) {
    case Mode::Pending:
        return "booting…";
    case Mode::Snapshot:
        return "snapshot @ seq " + std::to_string(seq) + " (" + std::to_string(objects) + " objects, " +
               std::to_string(pages) + (pages == 1 ? " page, " : " pages, ") + fmtMs(ms) + ", fold ready " +
               fmtMs(foldReadyMs) + ")";
    case Mode::Replay:
        return "replay from 0 (" + (reason.empty() ? std::string("no snapshot") : reason) + ", fold ready " +
               fmtMs(foldReadyMs) + ")";
    }
    return "";
}

Runtime::Runtime(const Options& opts, std::unique_ptr<SessionStore> sessionStore)
    : store(std::move(sessionStore)), client(withFormat(opts.url, opts.format)),
      session(configFrom(opts), *store, events, [this](const Frame& f) { client.send(f); }), chat(session, events) {
    trace_ = std::getenv("LISTENER_TRACE") != nullptr;
    noToken_ = opts.token.empty();
    tokenProblem_ = opts.tokenProblem;
    foldSub_ = foldSubName();
    // the session principal's own line addresses the Route's other members, not itself
    chat.setRecipientResolver([this](const std::string& route) {
        const std::string self = session.principal().value_or("");
        return self.empty() ? std::vector<std::string>{} : model::recipientsFor(principals, self, route, self);
    });
    chat.mentionName = [this](const std::string& id) { return principals.boundName(id); };
    tokens.onLabel = [this](const std::string& id, const std::string& label) { principals.alias(id, label); };
    events.rowSubs = {"history"};
    events.onEvent = [this](const std::string& sub, const Event& e) {
        if (sub == "unsafe" && e.kind == "field-set" && e.body.is_object() && e.body.value("name", std::string()) == "world/unsafe") {
            unsafe = chat::parseUnsafe(e.body.value("value", json()));
            foldChanged_ = true;
            return;
        }
        if (sub == "notify") {
            if (notifications.apply(e, session.principal().value_or(""))) {
                foldChanged_ = true;
            }
            return;
        }
        if (sub.rfind("fold-", 0) != 0) {
            return;
        }
        if (principals.apply(e)) {
            foldChanged_ = true;
        }
        if (evaluations.apply(e)) {
            foldChanged_ = true;
        }
        if (tasks.apply(e)) {
            foldChanged_ = true;
        }
        if (actions.apply(e)) {
            foldChanged_ = true;
        }
        if (tokens.apply(e)) {
            foldChanged_ = true;
        }
        if (panes.apply(e)) {
            foldChanged_ = true;
        }
        chat.noteFoldEvent(e);
        // A Grant to this principal that names objects (an approval Task and its wake, say): their
        // creation predates the Grant, so the filtered fold never carried it; read them now and
        // seed what the snapshot would have (the log stays the truth, this is a late catch-up).
        if (e.kind == "granted" && session.principal() && e.body.is_object()) {
            const json& body = e.body;
            const std::string grantee = body.contains("grantee") && body["grantee"].is_string() ? body["grantee"].get<std::string>() : "";
            const json scope = body.contains("scope") ? body["scope"] : json();
            if (grantee == *session.principal() && scope.is_array()) {
                for (const json& id : scope) {
                    if (!id.is_string() || principals.get(id.get<std::string>()) || tasks.get(id.get<std::string>())) {
                        continue;
                    }
                    session.request("read", json{{"object", id.get<std::string>()}}, [this](const Response& r) {
                        if (!r.ok || !r.result.is_object()) {
                            return;
                        }
                        const model::SnapshotObject o = model::SnapshotObject::parse(r.result);
                        if (o.id.empty()) {
                            return;
                        }
                        tasks.seed(o);
                        principals.seed(o);
                        actions.seed(o);
                        tokens.seed(o);
                        panes.seed(o);
                        if (o.cls == "message") {
                            chat.messages().request(o.id);
                            chat.messages().onRead(o.id, o.raw);
                            chat.messageSeeded(o.id);
                        }
                        foldChanged_ = true;
                    }, true);
                }
            }
        }
    };
    binds.push_back(ui::BindRow{workspace::localFsBind(), std::nullopt, false});
    binds.push_back(ui::BindRow{workspace::shellBind(), std::nullopt, false});
}

Runtime::~Runtime() {
    client.stop();
}

bool Runtime::drain() {
    watchUnsafe();
    watchNotifications();
    bool any = false;
    Inbound in;
    while (client.poll(in)) {
        any = true;
        switch (in.type) {
        case Inbound::Type::Opened:
            log::info("ws: open {}", in.text);
            bindsSentThisAttach = false;
            chat.onOpen();
            session.onOpen();
            break;
        case Inbound::Type::Closed: {
            const auto now = std::chrono::steady_clock::now();
            ++reconnects_;
            closes_.push_back(now);
            while (!closes_.empty() && now - closes_.front() > std::chrono::seconds(10)) {
                closes_.pop_front();
            }
            session.onClose();
            chat.onClosed();
            if (closes_.size() >= 3 && !backoffUntil_) {
                const int delay = std::min(30, 1 << std::min(backoffSteps_, 5)); // 1 2 4 8 16 30
                ++backoffSteps_;
                backoffUntil_ = now + std::chrono::seconds(delay);
                client.stop();
                client.setState(ConnState::Disconnected);
                const std::string why = "the world closed the connection " + std::to_string(closes_.size()) +
                                        " times in 10 s (" + in.text + "): another listener may hold session " +
                                        session.sessionId() + "; retrying in " + std::to_string(delay) + " s";
                log::warn("ws: reconnect #{} held back: {}", reconnects_, why);
                chat.noteCarrier("CARRIER LOST // " + why, now);
                closes_.clear();
            } else {
                log::warn("ws: closed ({}); reconnect #{} follows with backoff", in.text, reconnects_);
            }
            break;
        }
        case Inbound::Type::Error:
            log::warn("ws: {}", in.text);
            break;
        case Inbound::Type::Frame:
            if (trace_) {
                if (auto* p = std::get_if<Push>(&*in.frame)) {
                    log::info("push {}: {} event(s) as-of {}{}", p->sub, p->events.size(), p->asOf,
                              p->lagging ? " lagging" : "");
                }
            }
            session.onFrame(*in.frame);
            if (session.attached() && client.state() == ConnState::Attaching) {
                client.setState(ConnState::Attached);
            }
            break;
        }
    }
    // A refused attach (bad token, unknown principal, a frame this world rejects) will not change
    // by retrying: stop and say why. Other attach errors ride the close/backoff path.
    if (session.attachFailures() != attachFailuresSeen_) {
        attachFailuresSeen_ = session.attachFailures();
        if (session.attachFailures() > 0) {
            const auto& [code, message] = session.attachError();
            if (session.attachRefused()) {
                stopReason_ = "attach refused: " + code + " (" + message + ")" +
                              (code == "unauthenticated" ? "; a --fresh world issues new tokens: restart with one" : "");
                log::error("session: {}; not retrying", stopReason_);
                chat.noteCarrier("CARRIER LOST // " + stopReason_);
                client.stop();
                client.setState(ConnState::Disconnected);
            } else {
                log::warn("session: attach failed: {} ({}); reconnecting with backoff", code, message);
                client.drop();
            }
        }
    }
    if (backoffUntil_ && std::chrono::steady_clock::now() >= *backoffUntil_) {
        backoffUntil_.reset();
        log::info("ws: backoff over, connecting again (reconnect #{})", reconnects_ + 1);
        client.start();
    }
    if (session.attached() && backoffSteps_ > 0 && closes_.empty()) {
        backoffSteps_ = 0; // a stable attach resets the ladder
    }
    if (session.attached() && session.attaches() != attachSeen_) {
        attachSeen_ = session.attaches();
        attachAt_ = std::chrono::steady_clock::now();
        if (session.transientPending(foldSub_)) {
            startBoot();
        } else if (boot.mode == BootInfo::Mode::Pending) {
            // An in-process reattach resumed the fold at its acked seq: nothing to seed.
            boot.mode = BootInfo::Mode::Replay;
            boot.reason = "resumed";
        }
    }
    if (boot.foldReadyMs < 0 && boot.headAtAttach >= 0 && boot.mode != BootInfo::Mode::Pending) {
        auto it = session.subs().find(foldSub_);
        if (it != session.subs().end() && it->second.cursor && *it->second.cursor >= boot.headAtAttach) {
            boot.foldReadyMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - attachAt_).count();
            log::info("boot: fold ready at seq {} after {} ms ({})", *it->second.cursor,
                      static_cast<long long>(boot.foldReadyMs), boot.line());
            foldChanged_ = true;
        }
    }
    const std::size_t notesBefore = chat.notes().size();
    chat.tick();
    const bool changed = any || foldChanged_ || chat.notes().size() != notesBefore;
    foldChanged_ = false;
    return changed;
}

std::optional<std::chrono::steady_clock::time_point> Runtime::nextDeadline(std::chrono::steady_clock::time_point now) const {
    std::optional<std::chrono::steady_clock::time_point> d = chat.nextDeadline(now);
    if (backoffUntil_ && (!d || *backoffUntil_ < *d)) {
        d = backoffUntil_;
    }
    return d;
}

// Boot: ask the world for a snapshot of its objects; seed the folds from it and subscribe the
// tail after its seq. A world without the op (err not-implemented, or `invalid` from an older
// schema) gets the sprint-0 replay from seq 0 instead. Any objects of a half-fetched snapshot are
// kept: the tail after their seq brings the rest, nothing is shown twice.
void Runtime::fetchRetired(json next) {
    json args = {{"snapshot", true}, {"retired", true}, {"limit", kSnapshotPage}};
    if (!next.is_null()) {
        args["next"] = std::move(next);
    }
    session.request("history", std::move(args), [this](const Response& r) {
        if (!r.ok) {
            return;
        }
        const json& res = r.result;
        const json& snap = res.contains("snapshot") && res["snapshot"].is_object() ? res["snapshot"] : res;
        if (snap.contains("objects") && snap["objects"].is_array()) {
            for (const json& raw : snap["objects"]) {
                if (raw.value("object/retired", false) || raw.value("object/retired?", false)) {
                    const model::SnapshotObject o = model::SnapshotObject::parse(raw);
                    principals.noteRetired(o.id, model::Principals::RetiredInfo{o.name, o.cls, 0});
                }
            }
        }
        if (res.contains("next") && !res["next"].is_null()) {
            fetchRetired(res["next"]);
        }
    }, true);
}

void Runtime::fetchOlderHistory() {
    if (olderInFlight_ || events.complete() || !session.attached()) {
        return;
    }
    const Seq oldest = events.oldestSeq();
    if (oldest <= 1) {
        events.setComplete();
        return;
    }
    olderInFlight_ = true;
    session.request("history", json{{"before", oldest}, {"last", kHistoryPage}}, [this](const Response& r) {
        olderInFlight_ = false;
        if (!r.ok || !r.result.is_object()) {
            return;
        }
        std::vector<Event> older;
        if (r.result.contains("events") && r.result["events"].is_array()) {
            older.reserve(r.result["events"].size());
            for (const json& e : r.result["events"]) {
                older.push_back(Event::fromJson(e));
            }
        }
        if (older.empty() || !r.result.contains("prev") || r.result["prev"].is_null()) {
            events.setComplete();
        }
        events.prepend(std::move(older));
        foldChanged_ = true;
    }, true);
}

void Runtime::start() {
    if (noToken_) {
        stopReason_ = tokenProblem_.empty() ? "no token" : tokenProblem_;
        chat.noteCarrier("NO CARRIER // " + stopReason_);
        client.setState(ConnState::Disconnected);
        return;
    }
    client.start();
}

void Runtime::startBoot() {
    if (bootInFlight_) {
        return;
    }
    bootInFlight_ = true;
    boot = BootInfo{};
    boot.headAtAttach = session.head();
    principals.clear();
    evaluations.clear();
    tasks.clear();
    actions.clear();
    requestSnapshotPage(json{{"snapshot", true}, {"limit", kSnapshotPage}});
}

void Runtime::requestSnapshotPage(json args) {
    session.request(
        "history", std::move(args),
        [this](const Response& r) {
            if (!session.attached()) {
                bootInFlight_ = false;
                return;
            }
            if (!r.ok) {
                finishBoot(BootInfo::Mode::Replay, r.err ? r.err->code : "err");
                return;
            }
            const json& res = r.result;
            const json& snap = res.contains("snapshot") && res["snapshot"].is_object() ? res["snapshot"] : res;
            const json objects = snap.contains("objects") ? snap["objects"] : json::array();
            if (snap.contains("seq") && snap["seq"].is_number()) {
                boot.seq = std::max<Seq>(boot.seq, snap["seq"].get<Seq>());
            }
            const model::SnapshotStats st = model::seedSnapshot(
                objects, principals, evaluations,
                [this](const model::SnapshotObject& m) {
                    chat.messages().request(m.id);
                    chat.messages().onRead(m.id, m.raw);
                    chat.messageSeeded(m.id);
                },
                &tasks, &actions, &tokens);
            for (const json& raw : objects) {
                panes.seed(model::SnapshotObject::parse(raw));
            }
            boot.objects += st.objects;
            ++boot.pages;
            foldChanged_ = true;
            if (res.contains("next") && !res["next"].is_null()) {
                json next = {{"snapshot", true}, {"limit", kSnapshotPage}, {"next", res["next"]}};
                requestSnapshotPage(std::move(next));
                return;
            }
            if (boot.seq < 0) {
                boot.seq = r.seq; // the head as of the answer, when the world names no snapshot seq
            }
            finishBoot(BootInfo::Mode::Snapshot, "");
        },
        true);
}

// world/unsafe once on each attach, then every change to it. A missing Field means off.
void Runtime::watchUnsafe() {
    const std::string world = session.world().id;
    if (!session.attached() || world.empty() || session.attaches() == unsafeAttach_) {
        return;
    }
    unsafeAttach_ = session.attaches();
    session.request("read", json{{"object", world}, {"field", "world/unsafe"}}, [this](const Response& r) {
        if (!r.ok) {
            unsafe.clear();
            return;
        }
        unsafe = chat::parseUnsafeRead(r.result);
        foldChanged_ = true;
    }, true);
    session.subscribeFiltered("unsafe", json{{"stream", world}, {"kinds", json::array({"field-set"})}}, session.head());
}

// What this person has been told, once on each attach, then the collection's stream. A world that
// has no `notify` yet answers with an error and the chip stays at nought.
void Runtime::watchNotifications() {
    if (!session.attached() || session.attaches() == notifyAttach_ || !session.principal()) {
        return;
    }
    notifyAttach_ = session.attaches();
    chat.actNamed("notify.list", json{{"limit", 200}}, [this](const Response& r) {
        if (!r.ok) {
            return;
        }
        notifications.seed(r.result);
        foldChanged_ = true;
        if (!notifications.collection().empty()) {
            session.subscribeFiltered("notify", json{{"stream", notifications.collection()}, {"kinds", json::array({"created", "field-set"})}},
                                      session.head());
        }
    });
}

void Runtime::finishBoot(BootInfo::Mode mode, const std::string& reason) {
    bootInFlight_ = false;
    boot.mode = mode;
    boot.reason = reason;
    boot.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - attachAt_).count();
    if (mode == BootInfo::Mode::Snapshot) {
        principals.setApplied(boot.seq);
        evaluations.setApplied(boot.seq);
        tasks.setApplied(boot.seq);
        actions.setApplied(boot.seq);
        session.subscribeTransient(foldSub_, boot.seq);
        log::info("boot: {} ", boot.line());
    } else {
        principals.clear();
        evaluations.clear();
        tasks.clear();
        actions.clear();
        session.subscribeTransient(foldSub_, 0);
        log::info("boot: replay from 0 ({})", reason);
    }
    foldChanged_ = true;
}

void Runtime::sendBinds() {
    if (!session.attached()) {
        return;
    }
    for (std::size_t i = 0; i < binds.size(); ++i) {
        ui::BindRow& row = binds[i];
        row.sent = true;
        row.response.reset();
        // quiet: in sprint 0 the world answers these with `unresolved`, which is the expected state
        // and must not become the session's error line.
        session.request(
            "bind", row.plan.args(),
            [this, i](const Response& r) {
            binds[i].response = r;
            if (r.ok) {
                log::info("bind {}: ok seq {}", binds[i].plan.name, r.result.value("seq", r.seq));
            } else {
                log::info("bind {}: err {} ({})", binds[i].plan.name, r.err ? r.err->code : "?",
                          r.err ? r.err->message : "");
            }
            },
            true);
    }
    bindsSentThisAttach = true;
}

std::string readToken(const std::string& token, const std::string& tokenFile, const char* argv0, std::string& problem) {
    if (!token.empty()) {
        return token;
    }
    std::vector<std::filesystem::path> candidates;
    if (!tokenFile.empty()) {
        candidates.emplace_back(tokenFile);
    } else {
        // A person's client is a person. root is the agent: there is no fallback to its token, a
        // client that wants it says --token-file ../data/root.token.
        candidates.emplace_back("../data/person.token"); // run from listener/
        candidates.emplace_back("data/person.token");    // run from the repo root
        std::error_code ec;
        auto exe = std::filesystem::weakly_canonical(std::filesystem::path(argv0), ec);
        if (!ec) {
            // build/<preset>/listener -> listener/ -> repo
            candidates.push_back(exe.parent_path() / ".." / ".." / ".." / "data" / "person.token");
        }
    }
    for (const auto& p : candidates) {
        std::ifstream in(p);
        if (in) {
            std::string t;
            std::getline(in, t);
            while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) {
                t.pop_back();
            }
            if (!t.empty()) {
                log::info("token: read from {}", p.string());
                return t;
            }
        }
    }
    problem = tokenFile.empty() ? "no token: data/person.token not found (a client never falls back to root's)"
                                : "no token: cannot read " + tokenFile;
    log::error("{}", problem);
    return "";
}

} // namespace ke
