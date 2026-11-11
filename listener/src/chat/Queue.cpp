#include "chat/Queue.hpp"

#include <algorithm>

namespace ke::chat {

std::vector<QueueRow> queueRows(const json& loopQueue, const MessageCache& cache, std::vector<std::string>& pending) {
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
                if (auto it = std::find(pending.begin(), pending.end(), m->text); it != pending.end()) {
                    pending.erase(it);
                }
            }
            rows.push_back(std::move(r));
        }
    }
    for (const std::string& line : pending) {
        QueueRow r;
        r.text = line;
        r.pending = true;
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
