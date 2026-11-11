// The input line's model, no UI: the last 50 submitted lines with up/down recall, and the parse
// of one submitted line into a command (plain text to send, or `who am i`). Ref tags: a token
// `@<8+ hex chars>` becomes a tag when a resolver maps the prefix to an object id seen in the
// store (docs/listener-style.md "Whimsy": `who am i` is a real query, not a trick).
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ke::chat {

enum class CommandKind { Empty, Text, WhoAmI };

struct Command {
    CommandKind kind = CommandKind::Empty;
    std::string text;              // the line, trimmed
    std::vector<std::string> tags; // resolved ref tags (full ids), in order of appearance
    bool operator==(const Command&) const = default;
};

// Maps an 8+ hex-char prefix (as typed after `@`, dashes stripped) to a full id, or "" when no
// object seen in the store starts with it.
using RefResolver = std::function<std::string(std::string_view prefix)>;

Command parseLine(std::string_view line, const RefResolver& resolve = {});

// Hex prefixes typed as `@abcdef12` in a line, in order, dashes removed, lower-cased.
std::vector<std::string> refPrefixes(std::string_view line);

// Does `id` (a uuid, dashes allowed) start with `prefix` (hex, no dashes)?
bool idHasPrefix(std::string_view id, std::string_view prefix);

class InputLine {
  public:
    static constexpr std::size_t kMax = 50;

    // Remember a submitted line (empty lines and immediate repeats are not kept); resets recall.
    void push(std::string line);
    // Up arrow: the previous line, or nullopt at the oldest. `draft` is kept for the way down.
    std::optional<std::string> up(std::string_view draft);
    // Down arrow: the next line, or the draft once past the newest; nullopt when not recalling.
    std::optional<std::string> down();
    void resetRecall();

    const std::deque<std::string>& history() const { return history_; }
    bool recalling() const { return pos_ < history_.size(); }

  private:
    std::deque<std::string> history_; // oldest first
    std::size_t pos_ = 0;             // == history_.size() when not recalling
    std::string draft_;
};

} // namespace ke::chat
