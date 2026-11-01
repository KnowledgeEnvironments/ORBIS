#include "net/Protocol.hpp"

#include "util/Base64.hpp"

#include <utility>

namespace ke {

namespace {

template <class T> T require(const json& j, const char* key, const char* frame) {
    auto it = j.find(key);
    if (it == j.end()) {
        throw ProtocolError(std::string(frame) + " frame is missing \"" + key + "\"");
    }
    try {
        return it->get<T>();
    } catch (const json::exception& e) {
        throw ProtocolError(std::string(frame) + " frame has a bad \"" + key + "\": " + e.what());
    }
}

template <class T> std::optional<T> optional(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return std::nullopt;
    }
    return it->get<T>();
}

} // namespace

// ---- Event ----------------------------------------------------------------------------------

Event Event::fromJson(const json& j) {
    if (!j.is_object()) {
        throw ProtocolError("event is not an object");
    }
    Event e;
    e.id = require<std::string>(j, "event/id", "event");
    e.stream = require<std::string>(j, "event/stream", "event");
    e.streams = optional<std::vector<std::string>>(j, "event/streams").value_or(std::vector<std::string>{});
    e.seq = require<Seq>(j, "event/seq", "event");
    e.kind = require<std::string>(j, "event/kind", "event");
    e.actor = require<std::string>(j, "event/actor", "event");
    e.via = optional<std::string>(j, "event/via");
    e.delegation = optional<std::string>(j, "event/delegation");
    e.at = require<std::string>(j, "event/at", "event");
    e.body = require<json>(j, "event/body", "event");
    return e;
}

json Event::toJson() const {
    json j = {{"event/id", id},       {"event/stream", stream}, {"event/seq", seq},  {"event/kind", kind},
              {"event/actor", actor}, {"event/at", at},         {"event/body", body}};
    if (!streams.empty()) {
        j["event/streams"] = streams;
    }
    if (via) {
        j["event/via"] = *via;
    }
    if (delegation) {
        j["event/delegation"] = *delegation;
    }
    return j;
}

// ---- Err ------------------------------------------------------------------------------------

Err errFromJson(const json& j) {
    Err e;
    e.code = require<std::string>(j, "code", "err");
    e.message = require<std::string>(j, "message", "err");
    e.data = optional<json>(j, "data");
    return e;
}

json errToJson(const Err& e) {
    json j = {{"code", e.code}, {"message", e.message}};
    if (e.data) {
        j["data"] = *e.data;
    }
    return j;
}

// ---- Frames ---------------------------------------------------------------------------------

const char* name(FrameKind k) {
    switch (k) {
    case FrameKind::Request:
        return "request";
    case FrameKind::Response:
        return "response";
    case FrameKind::Push:
        return "push";
    case FrameKind::Ack:
        return "ack";
    case FrameKind::Pane:
        return "pane";
    }
    return "?";
}

// docs/protocol.md "Frames": kinds are told apart by key presence.
FrameKind kindOf(const json& j) {
    if (!j.is_object()) {
        throw ProtocolError("frame is not a JSON object");
    }
    if (j.contains("ack")) {
        return FrameKind::Ack;
    }
    if (j.contains("pane") && j.contains("seq") && !j.contains("id") && !j.contains("op")) {
        return FrameKind::Pane;
    }
    if (j.contains("sub") && j.contains("events")) {
        return FrameKind::Push;
    }
    if (j.contains("op")) {
        return FrameKind::Request;
    }
    if (j.contains("ok")) {
        return FrameKind::Response;
    }
    throw ProtocolError("frame has none of ack/sub/op/ok");
}

FrameKind kindOf(const Frame& f) {
    return static_cast<FrameKind>(f.index());
}

Frame parseFrame(const json& j) {
    switch (kindOf(j)) {
    case FrameKind::Pane: {
        PaneData p;
        p.pane = require<std::string>(j, "pane", "pane");
        p.seq = j.value("seq", Seq{0});
        if (j.contains("base") && j["base"].is_number_integer()) {
            p.base = j["base"].get<Seq>();
        }
        p.full = j.value("full", false);
        p.exited = j.value("exited", false);
        if (j.contains("exit") && j["exit"].is_number_integer()) {
            p.exitCode = j["exit"].get<int>();
        }
        p.cols = j.value("cols", 0);
        p.rows = j.value("rows", 0);
        if (j.contains("data") && j["data"].is_string()) {
            p.data = j["data"].get<std::string>();
        }
        if (j.contains("state")) {
            p.state = j["state"];
        }
        if (j.contains("edit")) {
            p.edit = j["edit"];
        }
        return p;
    }
    case FrameKind::Ack: {
        Ack a;
        a.sub = require<std::string>(j, "ack", "ack");
        a.seq = require<Seq>(j, "seq", "ack");
        return a;
    }
    case FrameKind::Push: {
        Push p;
        p.sub = require<std::string>(j, "sub", "push");
        p.asOf = require<Seq>(j, "as-of", "push");
        p.lagging = optional<bool>(j, "lagging").value_or(false);
        p.from = optional<Seq>(j, "from");
        const json& events = j.at("events");
        if (!events.is_array()) {
            throw ProtocolError("push frame \"events\" is not an array");
        }
        p.events.reserve(events.size());
        for (const json& e : events) {
            p.events.push_back(Event::fromJson(e));
        }
        return p;
    }
    case FrameKind::Request: {
        Request r;
        r.id = require<std::string>(j, "id", "request");
        r.op = require<std::string>(j, "op", "request");
        r.args = require<json>(j, "args", "request");
        if (!r.args.is_object()) {
            throw ProtocolError("request frame \"args\" is not an object");
        }
        return r;
    }
    case FrameKind::Response: {
        Response r;
        r.id = require<std::string>(j, "id", "response");
        r.ok = require<bool>(j, "ok", "response");
        r.seq = require<Seq>(j, "seq", "response");
        if (r.ok) {
            r.result = require<json>(j, "result", "response");
        } else {
            r.err = errFromJson(j.at("err").is_object() ? j.at("err") : json::object());
        }
        return r;
    }
    }
    throw ProtocolError("unreachable");
}

json toJson(const Frame& f) {
    return std::visit(
        [](const auto& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Request>) {
                return json{{"id", v.id}, {"op", v.op}, {"args", v.args}};
            } else if constexpr (std::is_same_v<T, Response>) {
                if (v.ok) {
                    return json{{"id", v.id}, {"ok", true}, {"result", v.result}, {"seq", v.seq}};
                }
                return json{{"id", v.id},
                            {"ok", false},
                            {"err", errToJson(v.err.value_or(Err{"invalid", "no err given", std::nullopt}))},
                            {"seq", v.seq}};
            } else if constexpr (std::is_same_v<T, Push>) {
                json events = json::array();
                for (const Event& e : v.events) {
                    events.push_back(e.toJson());
                }
                json j = {{"sub", v.sub}, {"events", std::move(events)}, {"as-of", v.asOf}};
                if (v.lagging) {
                    j["lagging"] = true;
                }
                if (v.from) {
                    j["from"] = *v.from;
                }
                return j;
            } else if constexpr (std::is_same_v<T, PaneData>) {
                json j = {{"pane", v.pane}, {"seq", v.seq}, {"data", v.data}};
                if (v.base) {
                    j["base"] = *v.base;
                }
                if (v.full) {
                    j["full"] = true;
                }
                if (v.exited) {
                    j["exited"] = true;
                }
                if (v.exitCode) {
                    j["exit"] = *v.exitCode;
                }
                if (v.cols > 0) {
                    j["cols"] = v.cols;
                    j["rows"] = v.rows;
                }
                if (!v.state.is_null()) {
                    j["state"] = v.state;
                }
                if (!v.edit.is_null()) {
                    j["edit"] = v.edit;
                }
                return j;
            } else {
                return json{{"ack", v.sub}, {"seq", v.seq}};
            }
        },
        f);
}

Frame decode(std::string_view text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        throw ProtocolError("frame is not valid JSON");
    }
    return parseFrame(j);
}

std::string encode(const Frame& f) {
    return toJson(f).dump();
}

Request makeRequest(std::string id, std::string op, json args) {
    return Request{std::move(id), std::move(op), std::move(args)};
}

Response okResponse(std::string id, json result, Seq seq) {
    Response r;
    r.id = std::move(id);
    r.ok = true;
    r.result = std::move(result);
    r.seq = seq;
    return r;
}

Response errResponse(std::string id, std::string code, std::string message, Seq seq) {
    Response r;
    r.id = std::move(id);
    r.ok = false;
    r.err = Err{std::move(code), std::move(message), std::nullopt};
    r.seq = seq;
    return r;
}

} // namespace ke
