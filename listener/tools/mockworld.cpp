// mockworld: a stand-in for the world on ws://127.0.0.1:7777/ws, speaking the JSON form of
// protocol v0 (docs/protocol.md). It exists so the listener can be built and tested before the
// Clojure world runs. Behaviour:
//   attach     -> {session principal resumed seq}; a known session id resumes (resumed=true) with
//                 each sub's cursor set to the client's acked seq, otherwise a fresh id.
//   subscribe  -> {sub as-of}; cursor = after when given, else the current head (first push is 43
//                 on a fresh world, whose head starts at 42 like attach.response.json).
//   ack        -> advances acked; the window (default 256) reopens.
//   detach, unsubscribe -> ok {}.
//   read       -> the principal (with a `principal/routes` Field naming its inbox Route), that
//                 Route, or a Message created by send; anything else err unresolved.
//   send       -> a Message object readable by id, plus `sent` (on the sender's stream) and one
//                 `delivered` per recipient appended to the log and pushed at once; the route must
//                 be the principal's inbox Route, else err unresolved.
//   anything else -> err not-implemented.
// Every --period-ms (500) the head advances by one synthetic event and every attached sub gets a
// push of cursor+1 .. head, capped by its window. Events are deterministic in seq so a resumed
// client sees exactly acked+1 .. head.
#include "mockpanes.hpp"
#include "net/Protocol.hpp"
#include "util/Log.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace ke;

namespace {

constexpr const char* kRootPrincipal = "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0001";
constexpr const char* kRootRoute = "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0101";

struct SubRec {
    Seq cursor = 0;
    Seq acked = 0;
    Seq window = 256;
    bool lagging = false;
};

struct SessionRec {
    std::string principal;
    std::map<std::string, SubRec> subs;
    ix::WebSocket* socket = nullptr; // null while the connection is down (TTL not modelled)
};

struct World {
    std::mutex mutex;
    Seq head = 42;
    int nextSession = 1;
    std::map<std::string, SessionRec> sessions;             // by session id
    std::map<std::string, std::string> sessionByConnection; // connection id -> session id
    std::map<Seq, Event> appended;                          // events written by ops (send)
    std::map<std::string, json> messages;                   // Message state by id
    int nextMessage = 1;
    std::unique_ptr<mock::Panes> panes;
    mock::PaneHost paneHost;
};

std::string nowIso() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

// Deterministic in seq, so resumed clients get byte-identical events.
Event synthetic(Seq seq, const std::string& principal) {
    Event e;
    e.seq = seq;
    e.id = std::format("0b5a2c3e-1111-4c2a-8f1e-{:012x}", static_cast<unsigned long long>(seq));
    e.stream = principal;
    e.actor = principal;
    e.kind = kKinds[static_cast<std::size_t>(seq) % std::size(kKinds)];
    e.at = nowIso();
    if (e.kind == "field-set") {
        e.body = {{"name", "notes/mock"},
                  {"type", "string"},
                  {"value", std::format("row {}", seq)},
                  {"reader", "both"}};
    } else if (e.kind == "attached") {
        e.body = {{"session", "s-mock"}, {"client-kind", "listener"}, {"format", "json"}};
    } else if (e.kind == "created") {
        e.body = {{"class", "object"}, {"fields", {{"object/name", std::format("mock-{}", seq)}}}};
    } else {
        e.body = {{"mock", true}, {"seq", seq}};
    }
    return e;
}

// The event at a seq: one an op appended, else the synthetic one.
Event eventAt(const World& w, Seq seq, const std::string& principal) {
    if (auto it = w.appended.find(seq); it != w.appended.end()) {
        return it->second;
    }
    return synthetic(seq, principal);
}

json fieldOf(const std::string& name, const std::string& type, json value) {
    return json{{"field/name", name}, {"field/type", type}, {"field/value", std::move(value)}, {"field/reader", "both"}};
}

// The state map `read` answers for the principal: class attributes folded into Fields, as the
// world does it (listener CONTRACT-NEEDS 5).
json principalState(const std::string& id) {
    return json{{"object/id", id},
                {"object/class", "principal"},
                {"object/stream", id},
                {"object/names", json::array({json::array({id, "root"})})},
                {"object/fields",
                 {{"principal/driven-by", fieldOf("principal/driven-by", "keyword", "person")},
                  {"principal/routes", fieldOf("principal/routes", "list", json::array({kRootRoute}))},
                  {"notes/mock", fieldOf("notes/mock", "string", "a mock principal")}}}};
}

json routeState(const std::string& id, const std::string& owner) {
    return json{{"object/id", id},
                {"object/class", "route"},
                {"object/stream", owner},
                {"object/names", json::array({json::array({owner, "inbox"})})},
                {"object/fields",
                 {{"route/transport", fieldOf("route/transport", "keyword", "inbox")},
                  {"route/address", fieldOf("route/address", "string", owner)}}}};
}

void sendPushesLocked(World& w, SessionRec& s) {
    if (!s.socket) {
        return;
    }
    for (auto& [name, sub] : s.subs) {
        Seq room = sub.window - (sub.cursor - sub.acked);
        if (sub.cursor >= w.head) {
            continue;
        }
        if (room <= 0) {
            if (!sub.lagging) {
                sub.lagging = true;
                Push p{name, {}, w.head, true};
                s.socket->sendText(encode(p));
            }
            continue;
        }
        Push p{name, {}, w.head, false};
        while (sub.cursor < w.head && room > 0 && p.events.size() < 256) {
            p.events.push_back(eventAt(w, ++sub.cursor, s.principal));
            --room;
        }
        p.lagging = room <= 0 && sub.cursor < w.head;
        sub.lagging = p.lagging;
        s.socket->sendText(encode(p));
    }
}

Response handleRequest(World& w, const std::string& conn, ix::WebSocket& ws, const Request& r) {
    if (r.op == "attach") {
        const std::string token = r.args.value("token", "");
        if (token.empty()) {
            return errResponse(r.id, "unauthenticated", "attach needs a token", w.head);
        }
        const std::string wanted = r.args.value("session", "");
        auto it = w.sessions.find(wanted);
        const bool resumed = !wanted.empty() && it != w.sessions.end();
        std::string id = wanted;
        if (!resumed) {
            id = std::format("s-{:02}", w.nextSession++);
            SessionRec rec;
            rec.principal = r.args.value("principal", kRootPrincipal);
            it = w.sessions.emplace(id, std::move(rec)).first;
        }
        SessionRec& s = it->second;
        s.socket = &ws;
        w.sessionByConnection[conn] = id;
        ++w.head; // the world appends `attached` to the principal's stream
        if (resumed) {
            // The client's acked seq is authoritative: restore every sub at that cursor.
            if (r.args.contains("subs") && r.args["subs"].is_object()) {
                for (auto& [name, acked] : r.args["subs"].items()) {
                    SubRec& sub = s.subs[name];
                    sub.acked = sub.cursor = acked.get<Seq>();
                    sub.lagging = false;
                }
            }
        }
        log::info("mock: attach {} session {} resumed={} head={}", conn, id, resumed, w.head);
        json result = {{"session", id}, {"principal", s.principal}, {"resumed", resumed}, {"seq", w.head}};
        return okResponse(r.id, result, w.head);
    }

    auto sit = w.sessionByConnection.find(conn);
    if (sit == w.sessionByConnection.end()) {
        return errResponse(r.id, "unauthenticated", "attach first", w.head);
    }
    SessionRec& s = w.sessions[sit->second];

    if (r.op == "subscribe") {
        const std::string name = r.args.value("sub", "");
        if (name.empty()) {
            return errResponse(r.id, "invalid", "subscribe needs a sub", w.head);
        }
        SubRec& sub = s.subs[name];
        sub.window = r.args.value("window", Seq{256});
        sub.cursor = sub.acked = r.args.value("after", w.head);
        sub.lagging = false;
        log::info("mock: subscribe {} after={} window={}", name, sub.cursor, sub.window);
        return okResponse(r.id, json{{"sub", name}, {"as-of", w.head}}, w.head);
    }
    if (r.op == "unsubscribe") {
        s.subs.erase(r.args.value("sub", ""));
        return okResponse(r.id, json::object(), w.head);
    }
    if (r.op == "detach") {
        ++w.head; // `detached`
        w.sessions.erase(sit->second);
        w.sessionByConnection.erase(sit);
        return okResponse(r.id, json::object(), w.head);
    }
    if (r.op == "read") {
        const std::string object = r.args.value("object", r.args.value("name", std::string()));
        if (object == s.principal) {
            return okResponse(r.id, principalState(object), w.head);
        }
        if (object == kRootRoute) {
            return okResponse(r.id, routeState(object, s.principal), w.head);
        }
        if (auto it = w.messages.find(object); it != w.messages.end()) {
            return okResponse(r.id, it->second, w.head);
        }
        if (!object.empty() && object.front() == '/') { // a path reads as a file Object
            const std::string id = std::format("7f0e5a7e-4444-4c2a-8f1e-{:012x}", std::hash<std::string>{}(object) & 0xffffffffffffULL);
            return okResponse(r.id, json{{"object/id", id}, {"object/class", "file"}, {"object/stream", id},
                                         {"object/names", json::array({json::array({id, object})})},
                                         {"object/fields", {{"file/path", fieldOf("file/path", "string", object)}}}}, w.head);
        }
        return errResponse(r.id, "unresolved", "does not resolve", w.head);
    }
    const std::string actName = r.args.value("action", std::string());
    if (r.op == "act" && w.panes && (actName.rfind("pane.", 0) == 0 || actName == "file.open")) {
        json result;
        std::string code, message;
        const std::string action = r.args.value("action", std::string());
        const json args = r.args.value("args", json::object());
        if (!w.panes->act(action, args, sit->second, w.paneHost, result, code, message)) {
            return errResponse(r.id, code, message, w.head);
        }
        for (auto& [id, peer] : w.sessions) {
            sendPushesLocked(w, peer);
        }
        return okResponse(r.id, json{{"result", result}}, w.head);
    }
    if (r.op == "send") {
        const std::string route = r.args.value("route", "");
        const std::string text = r.args.value("text", "");
        const json to = r.args.value("to", json::array());
        if (route.empty() || text.empty() || !to.is_array()) {
            return errResponse(r.id, "invalid", "send needs to, route, text", w.head);
        }
        if (route != kRootRoute) {
            return errResponse(r.id, "unresolved", "does not resolve", w.head);
        }
        const std::string message = std::format("6d0e5a7e-2222-4c2a-8f1e-{:012x}", w.nextMessage++);
        Event sent;
        sent.seq = ++w.head;
        sent.id = std::format("0b5a2c3e-1111-4c2a-8f1e-{:012x}", static_cast<unsigned long long>(sent.seq));
        sent.stream = s.principal;
        sent.streams = {route};
        sent.actor = s.principal;
        sent.kind = "sent";
        sent.at = nowIso();
        sent.body = {{"message", message}, {"route", route}};
        w.appended[sent.seq] = sent;
        json state = {{"object/id", message},
                      {"object/class", "message"},
                      {"object/stream", message},
                      {"object/names", json::array()},
                      {"object/fields", json::object()},
                      {"message/from", s.principal},
                      {"message/to", to},
                      {"message/route", route},
                      {"message/text", text},
                      {"message/tags", r.args.value("tags", json::array())},
                      {"message/seq", sent.seq}};
        if (r.args.contains("in-reply-to")) {
            state["message/in-reply-to"] = r.args["in-reply-to"];
        }
        w.messages[message] = state;
        for (const auto& recipient : to) {
            Event d = sent;
            d.seq = ++w.head;
            d.id = std::format("0b5a2c3e-1111-4c2a-8f1e-{:012x}", static_cast<unsigned long long>(d.seq));
            d.stream = recipient.is_string() ? recipient.get<std::string>() : s.principal;
            d.kind = "delivered";
            d.at = nowIso();
            d.body = {{"message", message}, {"route", route}, {"recipient", d.stream}};
            w.appended[d.seq] = d;
        }
        log::info("mock: send {} -> message {} seq {}", r.args.dump(), message, sent.seq);
        return okResponse(r.id, json{{"message", message}, {"seq", sent.seq}}, w.head);
    }
    return errResponse(r.id, "not-implemented", std::format("op {} is not built yet (mockworld)", r.op),
                       w.head);
}

std::atomic<bool> gStop{false};

} // namespace

int main(int argc, char** argv) {
    int port = 7777;
    int periodMs = 500;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--period-ms" && i + 1 < argc) {
            periodMs = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: listener_mockworld [--port 7777] [--period-ms 500]\n");
            return 2;
        }
    }

    ix::initNetSystem();
    World world;
    ix::WebSocketServer server(port, "127.0.0.1");
    server.setOnClientMessageCallback([&world](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws,
                                               const ix::WebSocketMessagePtr& msg) {
        std::lock_guard<std::mutex> lock(world.mutex);
        const std::string conn = state->getId();
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            log::info("mock: connection {} open on {}", conn, msg->openInfo.uri);
            break;
        case ix::WebSocketMessageType::Close: {
            log::info("mock: connection {} closed", conn);
            auto it = world.sessionByConnection.find(conn);
            if (it != world.sessionByConnection.end()) {
                world.sessions[it->second].socket = nullptr; // session survives for reattach
                world.sessionByConnection.erase(it);
            }
            break;
        }
        case ix::WebSocketMessageType::Message: {
            Frame frame;
            try {
                frame = decode(msg->str);
            } catch (const std::exception& e) {
                log::warn("mock: bad frame from {}: {}", conn, e.what());
                return;
            }
            if (auto* req = std::get_if<Request>(&frame)) {
                Response r = handleRequest(world, conn, ws, *req);
                ws.sendText(encode(r));
                if (req->op == "subscribe" && r.ok) {
                    auto it = world.sessionByConnection.find(conn);
                    if (it != world.sessionByConnection.end()) {
                        sendPushesLocked(world, world.sessions[it->second]);
                    }
                } else if (req->op == "send" && r.ok) {
                    for (auto& [id, s] : world.sessions) { // the new rows reach every cursor now
                        sendPushesLocked(world, s);
                    }
                }
            } else if (auto* ack = std::get_if<Ack>(&frame)) {
                auto it = world.sessionByConnection.find(conn);
                if (it != world.sessionByConnection.end()) {
                    SessionRec& s = world.sessions[it->second];
                    auto sub = s.subs.find(ack->sub);
                    if (sub != s.subs.end()) {
                        sub->second.acked = std::max(sub->second.acked, ack->seq);
                        sendPushesLocked(world, s); // the window reopened
                    }
                }
            } else {
                log::warn("mock: ignoring {} frame from a client", name(kindOf(frame)));
            }
            break;
        }
        default:
            break;
        }
    });

    // What a person sees in a real world's first frames: a named root, a shell, and Fields of the
    // shapes that break a table: 16 KB of text, 5,000 unbroken characters, a very long name.
    {
        Seq seq = 0;
        const auto add = [&world, &seq](const std::string& stream, const std::string& kind, json body) {
            Event e;
            e.seq = ++seq;
            e.id = std::format("0b5a2c3e-1111-4c2a-8f1e-{:012x}", static_cast<unsigned long long>(e.seq));
            e.stream = stream;
            e.actor = kRootPrincipal;
            e.kind = kind;
            e.at = nowIso();
            e.body = std::move(body);
            world.appended[e.seq] = e;
        };
        const std::string shell = "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0002";
        const std::string ivy = "5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0003";
        add(kRootPrincipal, "created", {{"class", "principal"}, {"fields", {{"principal/driven-by", "person"}, {"principal/routes", json::array({kRootRoute})}}}});
        add(kRootPrincipal, "bound", {{"namespace", kRootPrincipal}, {"name", "root"}, {"object", kRootPrincipal}});
        add(shell, "created", {{"class", "adapter"}, {"fields", {{"adapter/binds", "host"}}}});
        add(kRootPrincipal, "bound", {{"namespace", kRootPrincipal}, {"name", "shell"}, {"object", shell}});
        add(ivy, "created", {{"class", "principal"}, {"fields", {{"principal/driven-by", "process"}}}});
        add(kRootPrincipal, "bound", {{"namespace", kRootPrincipal}, {"name", "ivy"}, {"object", ivy}});
        const auto field = [&add](const std::string& stream, const std::string& name, json value) {
            add(stream, "field-set", {{"name", name}, {"type", "string"}, {"value", std::move(value)}, {"reader", "both"}});
        };
        std::string big;
        for (int i = 0; big.size() < 16 * 1024; ++i) {
            big += std::format("line {} of a long Field that a person should not have to scroll past\n", i);
        }
        field(kRootPrincipal, "notes/big", big);
        field(kRootPrincipal, "notes/unbroken", std::string(5000, 'a'));
        field(kRootPrincipal, "notes/a-very-long-field-name-that-does-not-fit-in-any-reasonable-name-column-at-all", "short value");
        const char* path = std::getenv("LISTENER_MOCK_ROOT");
        field(kRootPrincipal, "notes/path", std::string("see ") + (path != nullptr ? path : "/tmp") + "/README.md");
    }

    // Panes: a directory to serve (LISTENER_MOCK_ROOT, the temp dir by default) and a reader per PTY.
    {
        const char* root = std::getenv("LISTENER_MOCK_ROOT");
        world.panes = std::make_unique<mock::Panes>(root != nullptr ? std::filesystem::path(root) : std::filesystem::temp_directory_path());
        world.paneHost.push = [&world](const std::string& session, const PaneData& d) {
            auto it = world.sessions.find(session);
            if (it != world.sessions.end() && it->second.socket != nullptr) {
                it->second.socket->sendText(encode(Frame{d}));
            }
        };
        world.paneHost.event = [&world](const mock::Pane& p, const std::string& kind, json body) {
            Event e;
            e.seq = ++world.head;
            e.id = std::format("0b5a2c3e-1111-4c2a-8f1e-{:012x}", static_cast<unsigned long long>(e.seq));
            e.stream = p.id;
            e.actor = kRootPrincipal;
            e.kind = kind;
            e.at = nowIso();
            e.body = std::move(body);
            world.appended[e.seq] = e;
            if (kind == "pane-opened") { // the Object appears first, as `created` with its Fields
                Event c = e;
                c.seq = ++world.head;
                c.id = std::format("0b5a2c3e-1111-4c2a-8f1e-{:012x}", static_cast<unsigned long long>(c.seq));
                c.kind = "created";
                c.body = {{"class", "object"},
                          {"fields", {{"pane/kind", p.kind}, {"pane/target", p.target}, {"pane/title", p.title}, {"pane/state", "open"}}}};
                world.appended[c.seq] = c;
            }
        };
        world.panes->onTerminal = [&world](const std::string& id, int fd) {
            std::thread([&world, id, fd] {
                char buf[8192];
                while (true) {
                    const ssize_t n = read(fd, buf, sizeof buf);
                    if (n <= 0) {
                        return;
                    }
                    std::lock_guard<std::mutex> lock(world.mutex);
                    world.panes->output(id, std::string(buf, static_cast<std::size_t>(n)), world.paneHost);
                }
            }).detach();
        };
    }

    auto res = server.listen();
    if (!res.first) {
        log::error("mock: cannot listen on {}: {}", port, res.second);
        return 1;
    }
    server.start();
    log::info("mock: world listening on ws://127.0.0.1:{}/ws (head {}, one event every {} ms)", port,
              world.head, periodMs);

    std::signal(SIGINT, [](int) { gStop = true; });
    std::signal(SIGTERM, [](int) { gStop = true; });
    while (!gStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
        std::lock_guard<std::mutex> lock(world.mutex);
        ++world.head;
        for (auto& [id, s] : world.sessions) {
            sendPushesLocked(world, s);
        }
    }
    log::info("mock: stopping");
    server.stop();
    return 0;
}
