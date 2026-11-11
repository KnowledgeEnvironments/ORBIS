// Notifications as this client knows them (docs/protocol.md, Notifications). `notify.list` once on
// each attach, then the collection's stream. A notification carries no authority, so nothing here
// ever acts on one: it is a line for a person to read, and a link to follow if they choose to.
#pragma once

#include "model/Event.hpp"
#include "model/Principals.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ke::chat {

struct Notification {
    std::string id;
    std::string level = "info"; // info, attention, urgent
    std::string text;
    std::string from;
    std::string at;
    std::string object; // optional: what it is about
    int dropped = 0;    // a summary of how many were over the limit
    bool read = false;
    Seq seq = 0;
};

class Notifications {
  public:
    // The answer of `notify.list`: replaces what is held, and everything in it counts as already seen.
    void seed(const nlohmann::json& result);
    // An event on the collection's stream, as `self` sees it. True when the picture changed.
    bool apply(const Event& e, const std::string& self);
    // This client said so; the world's field-set confirms it later.
    void markRead(const std::vector<std::string>& ids);
    void markAllRead();

    const std::string& collection() const { return collection_; }
    const std::vector<Notification>& rows() const { return rows_; } // newest first
    int unread() const;
    int unreadFrom(std::string_view principal) const;
    bool any() const { return !rows_.empty(); }
    std::optional<std::string> worstUnread() const; // the highest level unread, if any
    // Fresh, loud ones the OS should hear about, once each. Never what was already there at attach.
    std::vector<Notification> takeLoud();
    void clear();

  private:
    std::string collection_;
    std::vector<Notification> rows_;
    std::vector<Notification> loud_;
};

// Messages to this person that arrived while they were elsewhere, by sender. Starts at the head of
// the attach, so what was said before does not come back as new; reading a tab clears its sender.
class MessageUnread {
  public:
    void update(const std::vector<model::MessageRef>& messages, const std::string& self, Seq floor, const std::string& reading);
    int from(const std::string& principal) const;
    const std::map<std::string, int>& counts() const { return counts_; }

  private:
    std::size_t cursor_ = 0;
    std::map<std::string, int> counts_;
};

// Worth an OS notification: `attention` and `urgent`.
bool loud(std::string_view level);

} // namespace ke::chat
