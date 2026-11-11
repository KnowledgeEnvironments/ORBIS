// What is waiting for a Principal's loop: `loop/queue` from the fold, with this listener's own
// just-sent lines on the end until the world lists them, so nothing typed ever looks lost. Pure.
#pragma once

#include "chat/MessageCache.hpp"

#include <string>
#include <vector>

namespace ke::chat {

struct QueueRow {
    std::string message; // "" for a line still on its way
    std::string from;
    std::string at;
    std::string kind;
    std::string text;
    bool pending = false;
    bool operator==(const QueueRow&) const = default;
};

// `pending` loses the lines the world now lists (matched by text); the rows come back in queue order.
std::vector<QueueRow> queueRows(const json& loopQueue, const MessageCache& cache, std::vector<std::string>& pending);

// The first line of `text`, at most `limit` bytes, for a one-line row.
std::string firstLine(const std::string& text, std::size_t limit = 72);

} // namespace ke::chat
