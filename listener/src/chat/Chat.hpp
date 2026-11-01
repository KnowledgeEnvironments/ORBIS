// The chat side of the listener, no UI and no SDL (testable, shared by the window and --smoke):
//   - route discovery: after attach, `read {object <principal>}` and take the first id in
//     `principal/routes`; when absent, read again every 5 s (the pane shows NO ROUTE meanwhile)
//   - the input line: plain text -> `send {to #{principal} route text tags}`; `who am i` ->
//     `read` of the principal, its fields printed as [WHO] rows
//   - message rows: a `sent` / `delivered` event names a Message; it is read once (MessageCache)
//     so the history can show `<from> > text`
//   - notes: local lines merged into the history (boot sequence, send errors, WHO rows), and the
//     CARRIER state for the session pane
//   - a streaming hook: rows whose Evaluation is streaming carry the spinner (nothing streams in
//     the world yet, so the set stays empty)
// Deadlines (boot lines appearing, the route retry) are exposed so the app can sleep until the
// next one instead of polling.
#pragma once

#include "chat/Attach.hpp"
#include "chat/InputLine.hpp"
#include "chat/MessageCache.hpp"
#include "chat/Views.hpp"
#include "model/EventStore.hpp"
#include "net/Session.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ke::chat {

using Clock = std::chrono::steady_clock;

enum class NoteKind { Boot, Echo, Who, Error, Dim };

struct Note {
    NoteKind kind = NoteKind::Dim;
    std::string chip; // bracketed token shown before the text, "" for none
    std::string text;
    std::size_t afterRow = 0;   // event rows present when the note was made (merge order)
    Clock::time_point visibleAt; // boot lines appear one by one
    bool cursor = false;        // READY. carries the blinking block
};

enum class RouteState { Unknown, Searching, None, Found };
const char* name(RouteState s);

struct ChatStats {
    int sendsOk = 0;
    int sendsErr = 0;
    std::string lastSendCode;
    std::string lastMessageId;
    int whoRows = 0;
    int routeReads = 0;
};

// The offset of the next page of a byte-range read, or nothing when that was the last.
std::optional<long> nextBytePage(const nlohmann::json& result, const std::string& field);

class Chat {
  public:
    static constexpr std::chrono::seconds kRouteRetry{5};

    Chat(Session& session, EventStore& events);


    // Transport hooks from the Runtime, in this order around the Session's own.
    void onOpen();   // socket open, before attach: remembers the acked seq for CARRIER
    void onClosed(); // socket closed: pending reads are lost, CARRIER LOST
    // After every drain: notices a new attach, scans new rows, retries the route read.
    void tick(Clock::time_point now = Clock::now());
    // Enter on the input line (the session principal's own terminal).
    void submit(const std::string& line, Clock::time_point now = Clock::now());
    // Enter on another Principal's terminal: `who am i` stays a self query; anything else is a
    // `send {to #{principal} route text tags}` on that Principal's inbox route. `onResult` gets
    // "" on ok or the err code.
    using SendResult = std::function<void(const std::string& errCode, const std::string& errMessage)>;
    void submitTo(const std::string& principal, const std::string& route, const std::string& line,
                  SendResult onResult = {}, Clock::time_point now = Clock::now(), std::vector<std::string> tags = {});
    // Who a steer reaches: `send {steer true}` interrupts the running Evaluation; `err not-allowed`
    // says it could not, and the text stays with the caller.
    // The same with several recipients (the attached principal's own tab addresses the Route's
    // other members, see model::recipientsFor); `who am i` is a self query when `to` is only self.
    void submitTo(const std::vector<std::string>& to, const std::string& route, const std::string& line,
                  SendResult onResult = {}, Clock::time_point now = Clock::now(), std::vector<std::string> tags = {},
                  bool steer = false);
    // Who the session principal's own line (submit / probeSend) addresses on a Route: set by the
    // runtime from the Principals fold; without it, or when it answers nothing, the line goes to
    // self as in sprint 0 (the world delivers it to the sender, which is a loop poke for root).
    using RecipientResolver = std::function<std::vector<std::string>(const std::string& route)>;
    void setRecipientResolver(RecipientResolver r) { recipients_ = std::move(r); }
    // The text of a line the world refused (submit), once, for the input to take back.
    std::string takeFailedSend() {
        std::string t;
        t.swap(failedSend_);
        return t;
    }
    // `read {object}` of any Object; the handler gets the raw response (used to fill a card whose
    // Task has not reached the fold yet).
    std::string readObject(const std::string& object, Session::ResponseHandler handler);
    // `read {name}`: a mounted name materializes its file as an Object.
    std::string readNamed(const std::string& name, Session::ResponseHandler handler);
    // One Field whole, blobs included (`read {object field range}` pages by line; a million lines is all of it).
    std::string readField(const std::string& object, const std::string& field, Session::ResponseHandler handler);
    // A whole text Field, page by page in bytes: a Serialization is one line, so lines cannot page it.
    // done(text, ""), or done(what arrived, the error).
    void readWhole(const std::string& object, const std::string& field,
                   std::function<void(std::string, std::string)> done);
    // `set-field {object field type value reader}`; the handler gets the raw response.
    std::string setField(const std::string& object, const std::string& field, const json& type, const json& value,
                         const std::string& reader, Session::ResponseHandler handler);
    // `act {class action args}` (ADR 0008: creation is the create action on a class); the handler
    // gets the raw response, whose result carries `object` and `seq` on ok.
    std::string act(const std::string& cls, const std::string& action, json args, Session::ResponseHandler handler);
    // `act` on an existing Object: {object action args} (cancel an Evaluation, run a shell Adapter, ...).
    std::string actOn(const std::string& object, const std::string& action, json args, Session::ResponseHandler handler);
    // A named action (sprint 1 W3): `act {action "<name>" args}` with no class or object; the
    // world resolves `action/<name>`, validates the args against its schema and dispatches.
    std::string actNamed(const std::string& action, json args, Session::ResponseHandler handler);
    // An image for the next send: bytes in pieces, or a file the world can read. `done` gets the
    // attachment, or an empty object and why not.
    using ImageDone = std::function<void(json attachment, std::string error)>;
    void putImage(std::vector<unsigned char> bytes, const std::string& name, ImageDone done);
    void putImageFile(const std::string& mount, const std::string& path, ImageDone done);
    // What the next send carries, then forgets.
    void setNextImages(json images) { nextImages_ = std::move(images); }
    // What a typed `@` mention is called in the text, so a send carries its exact span.
    std::function<std::string(const std::string&)> mentionName;
    // An event from the fold sub (the whole log): Messages seen there are read like the rows'.
    void noteFoldEvent(const Event& e);
    // `history {...}` with any args (a thread's Messages, since-fork, a page); the handler gets the
    // raw response. `ingestHistory` feeds a history result back into the folds this chat owns:
    // its `events` are noted like fold events (their Messages get read), its `messages` (state
    // maps, when a world answers them) land in the cache directly. Returns how many it took.
    std::string history(json args, Session::ResponseHandler handler);
    std::size_t ingestHistory(const json& result);
    // A Message the snapshot seeded into the cache without a read: follow its view and tags.
    void messageSeeded(const std::string& id);
    // Views (sprint 1 L10): the view Objects behind Messages of kind view, and tagged Objects
    // that turn out to carry `view/kind`; read once, cached here, drawn by the terminal.
    ViewCache& views() { return views_; }
    const ViewCache& views() const { return views_; }
    // Attach a local file to a Principal's conversation (sprint 1 L8): `act create object` with
    // the file Fields, `set-field` of its content, then `send` on `route` tagging the Object.
    // `progress` is called with a short stage line, whether it is an error, and whether done.
    using Progress = std::function<void(const std::string& stage, bool err, bool done)>;
    void attach(const std::string& principal, const std::string& route, Attachment a, Progress progress);
    // The same three ops for an Object built from `fields` (a folder listing as a tree view).
    void attachFields(const std::string& principal, const std::string& route, json fields, const std::string& text,
                      Progress progress);
    // --smoke only: a send with the principal standing in for a route when none was found, so
    // the op is exercised against a world whose root has no inbox yet.
    void probeSend(const std::string& text, Clock::time_point now = Clock::now());
    // The earliest moment something changes without a network event (a boot line, the retry).
    std::optional<Clock::time_point> nextDeadline(Clock::time_point now) const;

    RouteState routeState() const { return routeState_; }
    const std::string& route() const { return route_; }
    const std::vector<Note>& notes() const { return notes_; }
    MessageCache& messages() { return messages_; }
    const MessageCache& messages() const { return messages_; }
    InputLine& input() { return input_; }
    bool carrierLost() const { return carrierLost_; }
    std::optional<Seq> resumedFrom() const { return resumedFrom_; }
    const std::set<Seq>& streamingSeqs() const { return streaming_; }
    const ChatStats& stats() const { return stats_; }
    bool sendNotBuilt() const { return sendNotBuilt_; }
    // The id of an object seen in the store whose id starts with `prefix`, or "".
    std::string resolveRef(std::string_view prefix) const;
    // A connection note from the Runtime (a refused attach, a reconnect backoff): a red line in
    // the transcript with the CARRIER chip.
    void noteCarrier(const std::string& text, Clock::time_point now = Clock::now());

  private:
    void onAttached(Clock::time_point now);
    void readRoute();
    void readMessage(const std::string& id);
    void followViews(const MessageRow& m); // request the view Object and any tagged Object once
    void readView(const std::string& id);
    void readBlob(const std::string& id);
    std::string failedSend_;
    void sendText(const Command& cmd, const std::string& route, Clock::time_point now);
    void whoAmI();
    void note(NoteKind kind, std::string chip, std::string text, Clock::time_point visibleAt,
              bool cursor = false);
    void noteStreaming(const Event& e);

    Session& session_;
    EventStore& events_;
    MessageCache messages_;
    ViewCache views_;
    std::vector<std::string> viewRereads_;
    std::optional<ContentField> pendingContent_; // the content set-field between create and send
    InputLine input_;
    std::vector<Note> notes_;
    int seenAttaches_ = 0;
    int seenResumes_ = 0;
    std::size_t scanned_ = 0;
    std::vector<std::string> rereads_;
    RouteState routeState_ = RouteState::Unknown;
    std::string route_;
    RecipientResolver recipients_;
    std::optional<Clock::time_point> nextRouteRead_;
    bool everAttached_ = false;
    bool carrierLost_ = false;
    std::optional<Seq> ackedAtOpen_;
    std::optional<Seq> resumedFrom_;
    bool sendNotBuilt_ = false;
    json nextImages_;
    ChatStats stats_;
    std::set<Seq> streaming_;
    std::map<std::string, Seq> streamingByEvaluation_;
};

} // namespace ke::chat
