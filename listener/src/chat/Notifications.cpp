#include "chat/Notifications.hpp"

#include <algorithm>

namespace ke::chat {

using nlohmann::json;

namespace {

std::string str(const json& j, const char* key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

int count(const json& j, const char* key) {
    return j.contains(key) && j[key].is_number_integer() ? j[key].get<int>() : 0;
}

bool lists(const json& list, const std::string& who) {
    return list.is_array() && std::find(list.begin(), list.end(), json(who)) != list.end();
}

// The answer may carry its rows directly or under `result`, as named actions do.
const json& inner(const json& r) { return r.contains("result") && r["result"].is_object() ? r["result"] : r; }

} // namespace

bool loud(std::string_view level) { return level == "attention" || level == "urgent"; }

void Notifications::seed(const json& result) {
    const json& r = inner(result);
    rows_.clear();
    loud_.clear();
    collection_ = str(r, "collection");
    if (!r.contains("rows") || !r["rows"].is_array()) {
        return;
    }
    for (const json& row : r["rows"]) {
        Notification n;
        n.id = str(row, "notification");
        n.level = str(row, "level");
        n.text = str(row, "text");
        n.from = str(row, "from");
        n.at = str(row, "at");
        n.object = str(row, "object");
        n.dropped = count(row, "dropped");
        n.read = row.contains("read") && row["read"].is_boolean() && row["read"].get<bool>();
        if (!n.id.empty()) {
            rows_.push_back(std::move(n));
        }
    }
}

bool Notifications::apply(const Event& e, const std::string& self) {
    if (e.kind == "created") {
        if (!e.body.is_object() || !e.body.contains("fields") || !e.body["fields"].is_object()) {
            return false;
        }
        const json& f = e.body["fields"];
        if (!f.contains("notification/text") || !lists(f.value("notification/to", json()), self)) {
            return false;
        }
        for (const Notification& have : rows_) {
            if (have.id == e.stream) {
                return false;
            }
        }
        Notification n;
        n.id = e.stream;
        n.level = str(f, "notification/level");
        n.text = str(f, "notification/text");
        n.from = str(f, "notification/from");
        n.at = str(f, "notification/at");
        n.object = str(f, "notification/object");
        n.dropped = count(f, "notification/dropped");
        n.seq = e.seq;
        if (loud(n.level)) {
            loud_.push_back(n);
        }
        rows_.insert(rows_.begin(), std::move(n));
        return true;
    }
    if (e.kind == "field-set" && e.body.is_object() && e.body.value("name", std::string()) == "notification/read-by" &&
        lists(e.body.value("value", json()), self)) {
        for (Notification& n : rows_) {
            if (n.id == e.stream && !n.read) {
                n.read = true;
                return true;
            }
        }
    }
    return false;
}

void Notifications::markRead(const std::vector<std::string>& ids) {
    for (Notification& n : rows_) {
        if (std::find(ids.begin(), ids.end(), n.id) != ids.end()) {
            n.read = true;
        }
    }
}

void Notifications::markAllRead() {
    for (Notification& n : rows_) {
        n.read = true;
    }
}

int Notifications::unread() const {
    return static_cast<int>(std::count_if(rows_.begin(), rows_.end(), [](const Notification& n) { return !n.read; }));
}

int Notifications::unreadFrom(std::string_view principal) const {
    return static_cast<int>(std::count_if(rows_.begin(), rows_.end(),
                                          [&](const Notification& n) { return !n.read && n.from == principal; }));
}

std::optional<std::string> Notifications::worstUnread() const {
    std::optional<std::string> worst;
    for (const Notification& n : rows_) {
        if (n.read) {
            continue;
        }
        if (n.level == "urgent") {
            return n.level;
        }
        if (!worst || n.level == "attention") {
            worst = n.level;
        }
    }
    return worst;
}

std::vector<Notification> Notifications::takeLoud() {
    std::vector<Notification> out;
    out.swap(loud_);
    return out;
}

void MessageUnread::update(const std::vector<model::MessageRef>& messages, const std::string& self, Seq floor,
                           const std::string& reading) {
    if (floor < 0 || self.empty()) {
        return; // the folds are still coming up
    }
    for (; cursor_ < messages.size(); ++cursor_) {
        const model::MessageRef& m = messages[cursor_];
        if (m.seq > floor && m.recipient == self && m.from != self) {
            ++counts_[m.from];
        }
    }
    if (!reading.empty()) {
        counts_.erase(reading);
    }
}

int MessageUnread::from(const std::string& principal) const {
    auto it = counts_.find(principal);
    return it != counts_.end() ? it->second : 0;
}

void Notifications::clear() {
    rows_.clear();
    loud_.clear();
    collection_.clear();
}

} // namespace ke::chat
