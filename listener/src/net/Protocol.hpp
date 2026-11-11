// Wire frames of protocol v0 in the JSON form (docs/protocol.md "Frames", ADR 0002).
// Four kinds, told apart by key presence: ack -> Ack, sub -> Push, op -> Request, ok -> Response.
#pragma once

#include "model/Event.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ke {

struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// request {id op args}, client to world.
struct Request {
    std::string id;
    std::string op;
    json args = json::object();
    bool operator==(const Request&) const = default;
};

// err {code message data?}; codes are enumerated in docs/protocol.md "Frames".
struct Err {
    std::string code;
    std::string message;
    std::optional<json> data;
    bool operator==(const Err&) const = default;
};

// response {id ok=true result seq} or {id ok=false err seq}; seq is the log head at reply time.
struct Response {
    std::string id;
    bool ok = false;
    json result;            // meaningful when ok
    std::optional<Err> err; // meaningful when !ok
    Seq seq = 0;
    bool operator==(const Response&) const = default;
};

// push {sub events as-of}, world to client. `lagging` is tolerated as an optional marker
// (protocol.md "Subscriptions" says the sub is marked lagging; the schema has no key yet, see
// CONTRACT-NEEDS.md).
struct Push {
    std::string sub;
    std::vector<Event> events;
    Seq asOf = 0;
    bool lagging = false;
    // The cursor the page was read from (world pass 11): seqs between `from` and the first event
    // were filtered by resolution, not lost; a `from` above our cursor is a lost delivery.
    std::optional<Seq> from;
    // The first seq the world still holds, when this sub's cursor was below it: retention dropped
    // the seqs in between. Unlike `from`, this is a real loss: what was folded from them is stale.
    std::optional<Seq> gap;
    bool operator==(const Push&) const = default;
};

// ack {ack=<sub> seq}, client to world, no response.
struct Ack {
    std::string sub;
    Seq seq = 0;
    bool operator==(const Ack&) const = default;
};

// pane {pane seq data reset?}, world to client (docs/panes.md "Streams"): screen bytes for one pane,
// coalesced. data is bytes in the JSON form, so base64; decoded here. `reset` marks the full screen
// a pane sends first on attach. Not on the log, so not a Push and never acked.
// One pane's stream to this session (docs/panes.md, PaneFrame). A terminal's `data` is escape
// sequences; `full` means start clean. Another kind's `data` is its text, and `state` or `edit`
// say what moved. `base` is the seq this delta applies on, so a gap is seen and resumed with `after`.
struct PaneData {
    std::string pane;
    Seq seq = 0;
    std::optional<Seq> base;
    bool full = false;
    bool exited = false;
    std::optional<int> exitCode;
    int cols = 0, rows = 0;
    std::string data;
    nlohmann::json state; // null when absent
    nlohmann::json edit;  // {at del text who version}, null when absent
    bool operator==(const PaneData&) const = default;
};

using Frame = std::variant<Request, Response, Push, Ack, PaneData>;

enum class FrameKind { Request, Response, Push, Ack, Pane };
const char* name(FrameKind k);

FrameKind kindOf(const json& j); // throws ProtocolError when no discriminating key is present
Frame parseFrame(const json& j); // throws ProtocolError on a malformed frame
json toJson(const Frame& f);
FrameKind kindOf(const Frame& f);

Frame decode(std::string_view text); // JSON text -> Frame
std::string encode(const Frame& f);  // Frame -> compact JSON text

Err errFromJson(const json& j);
json errToJson(const Err& e);

// Small helpers for building the frames the listener sends.
Request makeRequest(std::string id, std::string op, json args = json::object());
Response okResponse(std::string id, json result, Seq seq);
Response errResponse(std::string id, std::string code, std::string message, Seq seq);

} // namespace ke
