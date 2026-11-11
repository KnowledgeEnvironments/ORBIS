#include "chat/Queue.hpp"

#include <algorithm>

namespace ke::chat {

std::vector<QueueRow> queueRows(const json& loopQueue, const MessageCache& cache, std::vector<PendingLine>& pending) {
    std::vector<QueueRow> rows;
    if (loopQueue.is_array()) {
        for (const json& e : loopQueue) {
            if (!e.is_object()) {
                continue;
            }
            QueueRow r;
            auto str = [&e](const char* k) {
                return e.contains(k) && e[k].is_string() ? e[k].get<std::string>() : std::string();
            };
            r.message = str("message");
            r.from = str("from");
            r.at = str("at");
            r.kind = str("kind");
            if (const MessageRow* m = cache.get(r.message)) {
                r.text = m->text;
                if (auto it = std::find_if(pending.begin(), pending.end(), [&](const PendingLine& p) { return p.text == m->text; }); it != pending.end()) {
                    pending.erase(it);
                }
            }
            rows.push_back(std::move(r));
        }
    }
    // Kernel-handled commands need not enter loop/queue. A reply to a Message newer than the
    // enqueue watermark is nevertheless an acknowledgement, so its pending row is finished.
    cache.forEachReady([&](const MessageRow& m) {
        if (!m.inReplyTo) {
            return;
        }
        auto it = std::find_if(pending.begin(), pending.end(), [&](const PendingLine& p) { return m.seq > p.afterSeq; });
        if (it != pending.end()) {
            pending.erase(it);
        }
    });
    for (std::size_t i = 0; i < pending.size(); ++i) {
        QueueRow r;
        r.text = pending[i].text;
        r.pending = true;
        r.pendingIndex = i;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::string firstLine(const std::string& text, std::size_t limit) {
    std::string s = text.substr(0, text.find('\n'));
    if (s.size() > limit) {
        s = s.substr(0, limit) + "…";
    }
    return s;
}

} // namespace ke::chat
