// Messages behind `sent` / `delivered` rows: the event body names the Message id; the text lives
// on the Message object and is fetched once with `read {object <id>}` and cached here. The pane
// then shows the row as a chat line (`<from-prefix> > text`). No network: Chat issues the reads.
#pragma once

#include "model/Event.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ke::chat {

struct MessageRow {
    std::string id;
    std::string from;
    std::vector<std::string> to;
    std::string route;
    std::string text;
    json images; // message/images: {sha256 size mime name}; null when it carries none
    json refs; // message/refs: spans of `text` (chat/RichText); null when the Message has none
    std::vector<std::string> tags;
    std::optional<std::string> inReplyTo;
    std::string kind; // message/kind: "" (text), "approval", "approval-result", "view"
    std::string task; // message/task: the Task an approval Message carries
    std::string view; // message/view: the view Object a Message of kind view renders (sprint 1 L10)
    std::string thread;  // message/thread: the root Message of the conversation this one belongs to
    std::string askedBy; // message/asked-by: who started that thread
    int hop = 0;         // message/hop: how many loop-to-loop hops from the person (0 for a person's own line)
    std::string device; // message/device: the label of the token a session sent this with ("hosttwo")
    std::string steerApplied; // message/steer-applied: the Evaluation that took this steer in
    std::string summary;      // conversation/summary on a thread root: its older Messages, compressed
    Seq summaryFrom = 0;      // conversation/from and /through: the seqs that summary covers
    Seq summaryThrough = 0;
    std::string contradiction; // reply/contradiction (world pass 13): the prose claimed what the kernel record denies
    Seq seq = 0;
    bool operator==(const MessageRow&) const = default;
};

enum class MessageState { Unknown, Pending, Ready, Failed };

// A value from a `read` state map: top-level `key`, or `object/fields.<key>.field/value` (the
// world folds class attributes into Fields; the schema lists them top-level). Null when absent.
json stateValue(const json& state, std::string_view key);

// The first 8 characters of an id (the prefix shown in chat lines and ref tags).
std::string idPrefix(std::string_view id);

class MessageCache {
  public:
    // The Message id a `sent` / `delivered` event refers to; nullopt for every other kind.
    static std::optional<std::string> messageIdOf(const Event& e);
    // Parse a Message object's state map as `read` returns it.
    static MessageRow parseMessage(const std::string& id, const json& state);

    // Note an event. Returns the Message id when it must be read now (first sight of it), so
    // `sent` and `delivered` of the same Message cost one read.
    std::optional<std::string> noteEvent(const Event& e);
    // Mark an id as being read (Unknown -> Pending); false when it is already known.
    bool request(const std::string& id);
    void onRead(const std::string& id, const json& state);
    void onReadFailed(const std::string& id, const std::string& code);
    // A Field set on a Message after it was read (`reply/contradiction` lands after the reply is
    // sent): updated in place when the Message is known. Returns true when a row changed.
    bool noteFieldSet(const std::string& id, const std::string& name, const json& value);
    // The connection dropped: every pending read is lost. Returns the ids to read again.
    std::vector<std::string> resetPending();

    MessageState state(const std::string& id) const;
    const MessageRow* get(const std::string& id) const;
    const std::string* failure(const std::string& id) const;
    std::size_t ready() const { return ready_; }
    std::size_t pending() const { return pending_; }
    std::size_t size() const { return entries_.size(); }
    // Every Message read so far (state Ready), in no particular order (search, exports).
    template <class F> void forEachReady(F&& f) const {
        for (const auto& [id, e] : entries_) {
            if (e.state == MessageState::Ready) {
                f(e.row);
            }
        }
    }
    // Bumped on every change (a read landing, a failure, a reset): a cache key for views built from it.
    unsigned long long version() const { return version_; }

  private:
    struct Entry {
        MessageState state = MessageState::Unknown;
        MessageRow row;
        std::string error;
    };
    std::unordered_map<std::string, Entry> entries_;
    std::size_t ready_ = 0;
    std::size_t pending_ = 0;
    unsigned long long version_ = 0;
};

} // namespace ke::chat
