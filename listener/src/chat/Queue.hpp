// What is waiting for a Principal's loop: `loop/queue` from the fold, with this listener's own
// just-sent lines on the end until the world lists them, so nothing typed ever looks lost. Pure.
#pragma once

#include "chat/MessageCache.hpp"

#include <cstddef>
#include <limits>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace ke::chat {

struct PendingLine {
    std::string text;
    Seq afterSeq = 0; // only Messages newer than this enqueue watermark can acknowledge it
    bool operator==(const PendingLine&) const = default;
};

struct QueueRow {
    std::string message; // "" for a line still on its way
    std::string from;
    std::string at;
    std::string kind;
    std::string text;
    bool pending = false;
    std::size_t pendingIndex = std::numeric_limits<std::size_t>::max();
    bool operator==(const QueueRow&) const = default;
};

// `pending` loses lines the world lists, or whose Message a newer reply answers; rows come back in queue order.
std::vector<QueueRow> queueRows(const json& loopQueue, const MessageCache& cache, std::vector<PendingLine>& pending);

// Shared geometry for the compact strip; the heading stays outside the scrolling child.
struct QueueStripLayout {
    float targetHeight;
    float listHeight;
    bool scroll;
    bool stop;
    std::string heading;
};
inline QueueStripLayout queueStripLayout(std::size_t count, float frameHeight, float spacing, bool busy) {
    const float target = std::max(44.0f, frameHeight);
    return {target, static_cast<float>(std::min<std::size_t>(3, count)) * (target + spacing),
            count > 3, busy, "QUEUED " + std::to_string(count)};
}
std::string queueStripText(const std::string& text, float room,
                           const std::function<float(const std::string&)>& measure);

// The first line of `text`, at most `limit` bytes, for a one-line row.
std::string firstLine(const std::string& text, std::size_t limit = 72);

} // namespace ke::chat
