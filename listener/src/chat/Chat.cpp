#include "chat/RichText.hpp"
#include "chat/Chat.hpp"

#include "util/Base64.hpp"
#include "util/Log.hpp"
#include "util/Product.hpp"

#include <algorithm>
#include <cctype>

namespace ke::chat {

const char* name(RouteState s) {
    switch (s) {
    case RouteState::Unknown:
        return "unknown";
    case RouteState::Searching:
        return "searching";
    case RouteState::None:
        return "none";
    case RouteState::Found:
        return "found";
    }
    return "?";
}

namespace {

std::string oneLine(const json& v, std::size_t max = 120) {
    std::string s = v.is_string() ? v.get<std::string>() : v.dump();
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    if (s.size() > max) {
        s.resize(max - 3);
        s += "...";
    }
    return s;
}

bool looksLikeId(const std::string& s) {
    if (s.size() < 8) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) || c == '-'; });
}

} // namespace

Chat::Chat(Session& session, EventStore& events) : session_(session), events_(events) {}

void Chat::noteCarrier(const std::string& text, Clock::time_point now) {
    note(NoteKind::Error, "CARRIER", text, now);
}

void Chat::note(NoteKind kind, std::string chip, std::string text, Clock::time_point visibleAt, bool cursor) {
    Note n;
    n.kind = kind;
    n.chip = std::move(chip);
    n.text = std::move(text);
    n.afterRow = events_.rows().size();
    n.visibleAt = visibleAt;
    n.cursor = cursor;
    // Only the newest READY. keeps the cursor; other notes leave it where it is.
    if (cursor) {
        for (Note& old : notes_) {
            old.cursor = false;
        }
    }
    notes_.push_back(std::move(n));
}

void Chat::onOpen() {
    auto it = session_.subs().find("history");
    ackedAtOpen_ = it != session_.subs().end() ? it->second.acked : std::nullopt;
}

void Chat::onClosed() {
    carrierLost_ = everAttached_;
    for (std::string& id : messages_.resetPending()) {
        rereads_.push_back(std::move(id));
    }
    for (std::string& id : views_.resetPending()) {
        viewRereads_.push_back(std::move(id));
    }
    if (routeState_ == RouteState::Searching) {
        routeState_ = RouteState::Unknown;
    }
    nextRouteRead_.reset();
}

void Chat::tick(Clock::time_point now) {
    if (session_.attaches() != seenAttaches_) {
        seenAttaches_ = session_.attaches();
        onAttached(now);
    }
    const auto& rows = events_.rows();
    if (rows.size() < scanned_) {
        scanned_ = 0; // the store was cleared
    }
    for (; scanned_ < rows.size(); ++scanned_) {
        const Event& e = rows[scanned_];
        if (auto id = messages_.noteEvent(e)) {
            readMessage(*id);
        }
        noteStreaming(e);
    }
    if (session_.attached() && routeState_ == RouteState::None && nextRouteRead_ && now >= *nextRouteRead_) {
        readRoute();
    }
}

void Chat::onAttached(Clock::time_point now) {
    everAttached_ = true;
    carrierLost_ = false;
    if (session_.resumes() != seenResumes_) {
        seenResumes_ = session_.resumes();
        resumedFrom_ = ackedAtOpen_;
    }
    const std::string principal = session_.principal().value_or("?");
    const std::string head = std::to_string(session_.head());
    note(NoteKind::Boot, "", std::string(kProductName) + " // ATTACHED AS " + principal + " // HEAD " + head, now, true);
    if (routeState_ != RouteState::Found) {
        readRoute();
    }
    std::vector<std::string> again;
    again.swap(rereads_);
    for (const std::string& id : again) {
        if (messages_.request(id)) {
            readMessage(id);
        }
    }
    std::vector<std::string> views;
    views.swap(viewRereads_);
    for (const std::string& id : views) {
        if (views_.request(id)) {
            readView(id);
        }
    }
}

void Chat::readRoute() {
    if (!session_.attached() || !session_.principal()) {
        return;
    }
    routeState_ = RouteState::Searching;
    nextRouteRead_.reset();
    ++stats_.routeReads;
    session_.request("read", json{{"object", *session_.principal()}}, [this](const Response& r) {
        const json routes = r.ok ? stateValue(r.result, "principal/routes") : json();
        if (routes.is_array() && !routes.empty() && routes.front().is_string()) {
            route_ = routes.front().get<std::string>();
            routeState_ = RouteState::Found;
            log::info("chat: route {} (of {})", route_, routes.size());
            return;
        }
        if (routes.is_string()) {
            route_ = routes.get<std::string>();
            routeState_ = RouteState::Found;
            return;
        }
        routeState_ = RouteState::None;
        nextRouteRead_ = Clock::now() + kRouteRetry;
        if (!r.ok) {
            log::warn("chat: route read failed: {}", r.err ? r.err->code : "?");
        }
    });
}

void Chat::readMessage(const std::string& id) {
    if (!session_.attached()) {
        rereads_.push_back(id);
        return;
    }
    session_.request("read", json{{"object", id}}, [this, id](const Response& r) {
        if (r.ok) {
            messages_.onRead(id, r.result);
            if (const MessageRow* m = messages_.get(id)) {
                followViews(*m);
            }
        } else {
            messages_.onReadFailed(id, r.err ? r.err->code : "?");
        }
    });
}

void Chat::messageSeeded(const std::string& id) {
    if (const MessageRow* m = messages_.get(id)) {
        followViews(*m);
    }
}

void Chat::followViews(const MessageRow& m) {
    // The view a Message of kind view names, and (tolerantly) any tagged Object: a tag that
    // turns out to carry `view/kind` renders as a view too, which is how a client attaches a
    // folder listing without the world's `present`. Tags are few (an attached file, a view);
    // an Object read once as not-a-view is never read again.
    if (!m.view.empty() && views_.request(m.view)) {
        readView(m.view);
    }
    if (m.tags.size() <= 8) {
        for (const std::string& t : m.tags) {
            if (!views_.knownNotView(t) && views_.request(t)) {
                readView(t);
            }
        }
    }
}

void Chat::readView(const std::string& id) {
    if (!session_.attached()) {
        viewRereads_.push_back(id);
        return;
    }
    session_.request("read", json{{"object", id}}, [this, id](const Response& r) {
        if (r.ok) {
            const std::string blob = views_.onRead(id, r.result);
            if (!blob.empty()) {
                readBlob(blob);
            }
        } else {
            views_.onReadFailed(id, r.err ? r.err->code : "?");
        }
    }, true);
}

std::string Chat::readObject(const std::string& object, Session::ResponseHandler handler) {
    if (!session_.attached()) {
        return "";
    }
    return session_.request("read", json{{"object", object}}, std::move(handler), true);
}

std::string Chat::readField(const std::string& object, const std::string& field, Session::ResponseHandler handler) {
    if (!session_.attached()) {
        return "";
    }
    return session_.request("read", json{{"object", object}, {"field", field}, {"range", json::array({1, 1000000})}},
                            std::move(handler), true);
}

namespace {
constexpr long kBytePage = 64 * 1024;

struct WholeRead {
    std::string object, field, text;
    std::function<void(std::string, std::string)> done;
};
} // namespace

std::optional<long> nextBytePage(const json& result, const std::string& field) {
    const json range = stateValue(result, field + "-byte-range");
    const json size = stateValue(result, field + "-size");
    if (!range.is_array() || range.size() != 2 || !range[1].is_number() || !size.is_number()) {
        return std::nullopt;
    }
    const long to = range[1].get<long>();
    return to < size.get<long>() ? std::optional<long>(to) : std::nullopt;
}

void Chat::readWhole(const std::string& object, const std::string& field,
                     std::function<void(std::string, std::string)> done) {
    auto state = std::make_shared<WholeRead>(WholeRead{object, field, "", std::move(done)});
    auto page = std::make_shared<std::function<void(long)>>();
    *page = [this, state, page](long from) {
        const std::string id = session_.request(
            "read", json{{"object", state->object}, {"field", state->field}, {"byte-range", json::array({from, from + kBytePage})}},
            [this, state, page](const Response& r) {
                if (!r.ok) {
                    state->done(state->text, r.err ? r.err->code + ": " + r.err->message : std::string("unreadable"));
                    *page = nullptr;
                    return;
                }
                const json v = stateValue(r.result, state->field);
                state->text += v.is_string() ? v.get<std::string>() : v.dump(2);
                if (const std::optional<long> next = nextBytePage(r.result, state->field)) {
                    (*page)(*next);
                    return;
                }
                state->done(state->text, "");
                *page = nullptr;
            },
            true);
        if (id.empty()) {
            state->done(state->text, "not attached");
            *page = nullptr;
        }
    };
    (*page)(0);
}

std::string Chat::readNamed(const std::string& name, Session::ResponseHandler handler) {
    if (!session_.attached()) {
        return "";
    }
    return session_.request("read", json{{"name", name}}, std::move(handler), true);
}

void Chat::readBlob(const std::string& id) {
    if (!session_.attached()) {
        viewRereads_.push_back(id);
        return;
    }
    session_.request("read", json{{"object", id}}, [this, id](const Response& r) {
        if (r.ok) {
            views_.onBlob(id, r.result);
        } else {
            views_.onBlobFailed(id, r.err ? r.err->code : "?");
        }
    }, true);
}

void Chat::attach(const std::string& principal, const std::string& route, Attachment a, Progress progress) {
    if (!a.refusal.empty()) {
        progress("not attached: " + a.refusal, true, true);
        return;
    }
    const std::string text = "attached: " + a.name + " (" + a.mime + ", " + std::to_string(a.size) + " B, sha256 " +
                             a.sha256.substr(0, 12) + ")";
    // attachFields creates the Object and sends; the content set-field goes in between and is
    // handed over through pendingContent_ (consumed at the start of attachFields).
    pendingContent_ = contentField(a);
    attachFields(principal, route, createFields(a), text, std::move(progress));
}

void Chat::attachFields(const std::string& principal, const std::string& route, json fields, const std::string& text,
                        Progress progress) {
    if (!session_.attached() || !session_.principal()) {
        progress("not attached: no carrier", true, true);
        return;
    }
    if (route.empty()) {
        progress("not attached: this Principal has no route", true, true);
        return;
    }
    progress("creating the Object…", false, false);
    std::optional<ContentField> content;
    content.swap(pendingContent_);
    session_.request("act", json{{"class", "object"}, {"action", "create"}, {"args", json{{"fields", std::move(fields)}}}},
                     [this, principal, route, text, progress, content](const Response& r) {
                         if (!r.ok) {
                             progress("not attached: create " + (r.err ? r.err->code + " " + r.err->message : std::string("?")),
                                      true, true);
                             return;
                         }
                         const std::string object = r.result.value("object", "");
                         if (object.empty()) {
                             progress("not attached: create answered without an object id", true, true);
                             return;
                         }
                         auto sendIt = [this, principal, route, text, progress, object]() {
                             progress("sending the Message…", false, false);
                             json args = {{"to", json::array({principal})}, {"route", route}, {"text", text},
                                          {"tags", json::array({object})}};
                             session_.request("send", args, [progress, object](const Response& sr) {
                                 if (sr.ok) {
                                     progress("attached as " + idPrefix(object) + " (seq " +
                                                  std::to_string(sr.result.value("seq", sr.seq)) + ")",
                                              false, true);
                                 } else {
                                     progress("not attached: send " +
                                                  (sr.err ? sr.err->code + " " + sr.err->message : std::string("?")),
                                              true, true);
                                 }
                             });
                         };
                         if (!content) {
                             sendIt();
                             return;
                         }
                         progress("uploading " + content->field + "…", false, false);
                         json sf = {{"object", object}, {"field", content->field}, {"type", content->type},
                                    {"value", content->value}, {"reader", "both"}};
                         session_.request("set-field", sf, [progress, sendIt](const Response& fr) {
                             if (!fr.ok) {
                                 progress("not attached: upload " +
                                              (fr.err ? fr.err->code + " " + fr.err->message : std::string("?")),
                                          true, true);
                                 return;
                             }
                             sendIt();
                         });
                     });
}

std::string Chat::resolveRef(std::string_view prefix) const {
    const auto& rows = events_.rows();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        for (const std::string* cand : {&it->id, &it->stream, &it->actor}) {
            if (idHasPrefix(*cand, prefix)) {
                return *cand;
            }
        }
        if (it->body.is_object()) {
            for (const auto& [k, v] : it->body.items()) {
                if (v.is_string() && looksLikeId(v.get<std::string>()) && idHasPrefix(v.get<std::string>(), prefix)) {
                    return v.get<std::string>();
                }
            }
        }
    }
    return "";
}

void Chat::submit(const std::string& line, Clock::time_point now) {
    const Command cmd = parseLine(line, [this](std::string_view p) { return resolveRef(p); });
    if (cmd.kind == CommandKind::Empty) {
        return;
    }
    input_.push(line);
    if (cmd.kind == CommandKind::WhoAmI) {
        whoAmI();
        return;
    }
    if (routeState_ != RouteState::Found) {
        note(NoteKind::Dim, "", "[ SEND: NO ROUTE ] " + cmd.text, now);
        return;
    }
    sendText(cmd, route_, now);
}

void Chat::submitTo(const std::string& principal, const std::string& route, const std::string& line,
                    SendResult onResult, Clock::time_point now, std::vector<std::string> tags) {
    submitTo(std::vector<std::string>{principal}, route, line, std::move(onResult), now, std::move(tags));
}

void Chat::submitTo(const std::vector<std::string>& to, const std::string& route, const std::string& line,
                    SendResult onResult, Clock::time_point now, std::vector<std::string> tags, bool steer) {
    Command cmd = parseLine(line, [this](std::string_view p) { return resolveRef(p); });
    for (std::string& t : tags) { // objects picked with `@`, which enter the recipient's working set
        if (std::find(cmd.tags.begin(), cmd.tags.end(), t) == cmd.tags.end()) {
            cmd.tags.push_back(std::move(t));
        }
    }
    if (cmd.kind == CommandKind::Empty) {
        return;
    }
    json images;
    images.swap(nextImages_);
    const bool onlySelf = session_.principal() && to.size() == 1 && to.front() == *session_.principal();
    if (cmd.kind == CommandKind::WhoAmI && onlySelf) {
        input_.push(line);
        whoAmI();
        return;
    }
    if (!session_.attached()) {
        if (onResult) {
            onResult("no-carrier", "not attached");
        }
        return;
    }
    if (route.empty()) {
        if (onResult) {
            onResult("no-route", "the principal has no inbox route");
        }
        return;
    }
    if (to.empty()) {
        if (onResult) {
            onResult("no-recipient", "nobody to address on this route yet");
        }
        return;
    }
    json args = {{"to", json(to)}, {"route", route}, {"text", cmd.text}, {"tags", json(cmd.tags)}};
    if (steer) {
        args["steer"] = true;
    }
    if (images.is_array() && !images.empty()) {
        args["images"] = std::move(images);
    }
    if (mentionName) {
        std::vector<std::pair<std::string, std::string>> picked;
        for (const std::string& t : cmd.tags) {
            picked.emplace_back(t, mentionName(t));
        }
        if (json refs = mentionRefs(cmd.text, picked); !refs.empty()) {
            args["refs"] = std::move(refs);
        }
    }
    session_.request("send", args, [this, onResult](const Response& r) {
        if (r.ok) {
            ++stats_.sendsOk;
            stats_.lastMessageId = r.result.value("message", "");
            stats_.lastSendCode.clear();
            if (onResult) {
                onResult("", "");
            }
            return;
        }
        ++stats_.sendsErr;
        stats_.lastSendCode = r.err ? r.err->code : "?";
        if (onResult) {
            onResult(stats_.lastSendCode, r.err ? r.err->message : "");
        }
    });
}

std::string Chat::setField(const std::string& object, const std::string& field, const json& type, const json& value,
                           const std::string& reader, Session::ResponseHandler handler) {
    json args = {{"object", object}, {"field", field}, {"type", type}, {"value", value}};
    if (!reader.empty()) {
        args["reader"] = reader;
    }
    return session_.request("set-field", args, std::move(handler));
}

std::string Chat::act(const std::string& cls, const std::string& action, json args, Session::ResponseHandler handler) {
    return session_.request("act", json{{"class", cls}, {"action", action}, {"args", std::move(args)}}, std::move(handler));
}

std::string Chat::actOn(const std::string& object, const std::string& action, json args, Session::ResponseHandler handler) {
    // `args` is required by the frame, even for an action that takes none (STOP never worked
    // without it: the world refused every cancel as an invalid frame).
    json req = {{"object", object}, {"action", action}, {"args", args.is_null() ? json::object() : std::move(args)}};
    return session_.request("act", std::move(req), std::move(handler));
}

std::string Chat::actNamed(const std::string& action, json args, Session::ResponseHandler handler) {
    json req = {{"action", action}};
    req["args"] = args.is_null() ? json::object() : std::move(args);
    // A named action answers {result {...} object? seq?}: the handler's own keys sit one level
    // down. Flatten once here so every caller reads `token`, `secret`, `pairing-code`, `route`
    // at the top (found live: the issued secret and the pairing code were never seen).
    auto flat = [handler = std::move(handler)](const Response& r) {
        if (r.ok && r.result.is_object() && r.result.contains("result") && r.result["result"].is_object()) {
            Response f = r;
            json inner = r.result["result"];
            for (const auto& [k, v] : r.result.items()) {
                if (k != "result" && !inner.contains(k)) {
                    inner[k] = v;
                }
            }
            f.result = std::move(inner);
            handler(f);
            return;
        }
        handler(r);
    };
    return session_.request("act", std::move(req), std::move(flat));
}

std::string Chat::history(json args, Session::ResponseHandler handler) {
    if (!session_.attached()) {
        return "";
    }
    return session_.request("history", std::move(args), std::move(handler), true);
}

std::size_t Chat::ingestHistory(const json& result) {
    std::size_t n = 0;
    if (!result.is_object()) {
        return 0;
    }
    if (result.contains("events") && result["events"].is_array()) {
        for (const json& ej : result["events"]) {
            if (ej.is_object()) {
                noteFoldEvent(Event::fromJson(ej));
                ++n;
            }
        }
    }
    if (result.contains("messages") && result["messages"].is_array()) {
        for (const json& mj : result["messages"]) {
            const json idj = mj.is_object() ? stateValue(mj, "object/id") : json();
            const std::string id = idj.is_string() ? idj.get<std::string>() : (mj.is_object() && mj.contains("id") && mj["id"].is_string() ? mj["id"].get<std::string>() : std::string());
            if (id.empty()) {
                continue;
            }
            messages_.request(id);
            messages_.onRead(id, mj);
            ++n;
        }
    }
    return n;
}

void Chat::noteFoldEvent(const Event& e) {
    if (auto id = messages_.noteEvent(e)) {
        readMessage(*id);
    }
    // a Field set on a Message the cache already holds (reply/contradiction lands after the send)
    if (e.kind == "field-set" && e.body.is_object() && e.body.contains("name") && e.body["name"].is_string()) {
        const std::string object = e.body.contains("object") && e.body["object"].is_string() ? e.body["object"].get<std::string>() : e.stream;
        messages_.noteFieldSet(object, e.body["name"].get<std::string>(), e.body.contains("value") ? e.body["value"] : json());
    }
}

void Chat::probeSend(const std::string& text, Clock::time_point now) {
    const Command cmd = parseLine(text);
    const std::string route = routeState_ == RouteState::Found ? route_ : session_.principal().value_or("");
    if (cmd.kind != CommandKind::Text || route.empty()) {
        return;
    }
    sendText(cmd, route, now);
}

namespace {

constexpr std::size_t kImagePiece = 512 * 1024; // under the socket's frame limit, as base64

void putPiece(Chat& chat, std::shared_ptr<std::vector<unsigned char>> bytes, std::size_t from, const std::string& upload,
              const std::string& name, Chat::ImageDone done) {
    const std::size_t n = std::min(kImagePiece, bytes->size() - from);
    const bool more = from + n < bytes->size();
    json args = {{"base64", util::base64Encode(bytes->data() + from, n)}, {"upload", upload}, {"more", more}, {"name", name}};
    chat.actNamed("image.put", std::move(args), [&chat, bytes, from, n, more, upload, name, done = std::move(done)](const Response& r) mutable {
        if (!r.ok) {
            done(json::object(), r.err ? r.err->message : std::string("refused"));
            return;
        }
        if (more) {
            putPiece(chat, bytes, from + n, upload, name, std::move(done));
            return;
        }
        const json& result = r.result.is_object() && r.result.contains("result") ? r.result["result"] : r.result;
        done(result, "");
    });
}

} // namespace

void Chat::putImage(std::vector<unsigned char> bytes, const std::string& name, ImageDone done) {
    if (bytes.empty()) {
        done(json::object(), "empty");
        return;
    }
    static unsigned long long uploads = 0;
    auto shared = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    putPiece(*this, std::move(shared), 0, "up-" + std::to_string(++uploads), name, std::move(done));
}

void Chat::putImageFile(const std::string& mount, const std::string& path, ImageDone done) {
    json args = {{"path", path}};
    if (!mount.empty()) {
        args["mount"] = mount;
    }
    actNamed("image.put", std::move(args), [done = std::move(done)](const Response& r) {
        if (!r.ok) {
            done(json::object(), r.err ? r.err->message : std::string("refused"));
            return;
        }
        done(r.result.is_object() && r.result.contains("result") ? r.result["result"] : r.result, "");
    });
}

void Chat::sendText(const Command& cmd, const std::string& route, Clock::time_point now) {
    if (!session_.attached() || !session_.principal()) {
        note(NoteKind::Error, "", "[ SEND: NO CARRIER ] " + cmd.text, now);
        return;
    }
    note(NoteKind::Echo, "", "> " + cmd.text, now);
    std::vector<std::string> to = recipients_ ? recipients_(route) : std::vector<std::string>{};
    if (to.empty()) {
        to.push_back(*session_.principal());
    }
    json args = {{"to", json(to)},
                 {"route", route},
                 {"text", cmd.text},
                 {"tags", json(cmd.tags)}};
    session_.request("send", args, [this, text = cmd.text](const Response& r) {
        if (r.ok) {
            ++stats_.sendsOk;
            stats_.lastMessageId = r.result.value("message", "");
            stats_.lastSendCode.clear();
            return;
        }
        ++stats_.sendsErr;
        const std::string code = r.err ? r.err->code : "?";
        stats_.lastSendCode = code;
        failedSend_ = text; // never reported as sent: the input line takes it back
        if (code == "not-implemented") {
            if (!sendNotBuilt_) {
                sendNotBuilt_ = true;
                note(NoteKind::Error, "", "[ SEND: NOT BUILT ]", Clock::now());
            }
            return;
        }
        std::string label = code;
        std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        note(NoteKind::Error, "", "[ SEND: " + label + " ] " + (r.err ? r.err->message : ""), Clock::now());
    });
}

void Chat::whoAmI() {
    if (!session_.attached() || !session_.principal()) {
        note(NoteKind::Error, "WHO", "no carrier", Clock::now());
        return;
    }
    session_.request("read", json{{"object", *session_.principal()}}, [this](const Response& r) {
        const auto now = Clock::now();
        if (!r.ok) {
            note(NoteKind::Error, "WHO", "err " + (r.err ? r.err->code : std::string("?")), now);
            return;
        }
        int rows = 0;
        for (const char* key : {"object/id", "object/class", "object/stream", "object/names"}) {
            if (r.result.contains(key)) {
                note(NoteKind::Who, "WHO", std::string(key) + " " + oneLine(r.result[key]), now);
                ++rows;
            }
        }
        for (const auto& [k, v] : r.result.items()) {
            if (k.rfind("object/", 0) == 0) {
                continue;
            }
            note(NoteKind::Who, "WHO", k + " = " + oneLine(v), now);
            ++rows;
        }
        if (auto f = r.result.find("object/fields"); f != r.result.end() && f->is_object()) {
            for (const auto& [k, v] : f->items()) {
                const json value = v.is_object() && v.contains("field/value") ? v["field/value"] : v;
                note(NoteKind::Who, "WHO", k + " = " + oneLine(value), now);
                ++rows;
            }
        }
        stats_.whoRows += rows;
    });
}

// Streaming hook: an `evaluated` event whose body says the Evaluation is streaming keeps its
// row spinning until a later `evaluated` for the same Evaluation settles it. The world does not
// stream yet, so this stays empty in practice.
void Chat::noteStreaming(const Event& e) {
    if (e.kind != "evaluated" || !e.body.is_object()) {
        return;
    }
    const std::string evaluation = e.body.value("evaluation", "");
    const std::string state = e.body.value("state", "");
    if (auto it = streamingByEvaluation_.find(evaluation); it != streamingByEvaluation_.end()) {
        streaming_.erase(it->second);
        streamingByEvaluation_.erase(it);
    }
    if (state == "streaming" && !evaluation.empty()) {
        streaming_.insert(e.seq);
        streamingByEvaluation_[evaluation] = e.seq;
    }
}

std::optional<Clock::time_point> Chat::nextDeadline(Clock::time_point now) const {
    std::optional<Clock::time_point> next;
    auto consider = [&](Clock::time_point t) {
        if (t > now && (!next || t < *next)) {
            next = t;
        }
    };
    for (const Note& n : notes_) {
        consider(n.visibleAt);
    }
    if (nextRouteRead_) {
        consider(*nextRouteRead_);
    }
    return next;
}

} // namespace ke::chat
