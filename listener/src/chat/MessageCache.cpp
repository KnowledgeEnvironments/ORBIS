#include "chat/MessageCache.hpp"

#include <algorithm>

namespace ke::chat {

json stateValue(const json& state, std::string_view key) {
    if (!state.is_object()) {
        return nullptr;
    }
    const std::string k(key);
    if (auto it = state.find(k); it != state.end()) {
        return *it;
    }
    if (auto f = state.find("object/fields"); f != state.end() && f->is_object()) {
        if (auto it = f->find(k); it != f->end()) {
            if (it->is_object() && it->contains("field/value")) {
                return (*it)["field/value"];
            }
            return *it;
        }
    }
    return nullptr;
}

std::string idPrefix(std::string_view id) {
    return std::string(id.substr(0, 8));
}

std::optional<std::string> MessageCache::messageIdOf(const Event& e) {
    if (e.kind != "sent" && e.kind != "delivered") {
        return std::nullopt;
    }
    if (e.body.is_object() && e.body.contains("message") && e.body["message"].is_string()) {
        return e.body["message"].get<std::string>();
    }
    return std::nullopt;
}

MessageRow MessageCache::parseMessage(const std::string& id, const json& state) {
    MessageRow m;
    m.id = id;
    auto str = [&](std::string_view key) {
        const json v = stateValue(state, key);
        return v.is_string() ? v.get<std::string>() : std::string();
    };
    auto strs = [&](std::string_view key) {
        std::vector<std::string> out;
        const json v = stateValue(state, key);
        if (v.is_array()) {
            for (const auto& x : v) {
                if (x.is_string()) {
                    out.push_back(x.get<std::string>());
                }
            }
        } else if (v.is_string()) {
            out.push_back(v.get<std::string>());
        }
        return out;
    };
    m.from = str("message/from");
    m.device = str("message/device");
    // A token issued with its Principal has no device worth naming: `person on person (at creation)`.
    if (m.device.find("(at creation)") != std::string::npos) {
        m.device.clear();
    }
    m.steerApplied = str("message/steer-applied");
    m.to = strs("message/to");
    m.route = str("message/route");
    m.text = str("message/text");
    m.refs = stateValue(state, "message/refs");
    m.tags = strs("message/tags");
    if (const std::string r = str("message/in-reply-to"); !r.empty()) {
        m.inReplyTo = r;
    }
    m.kind = str("message/kind");
    m.task = str("message/task");
    m.view = str("message/view");
    if (json v = stateValue(state, "message/images"); v.is_array() && !v.empty()) {
        m.images = std::move(v);
    }
    m.thread = str("message/thread");
    m.askedBy = str("message/asked-by");
    if (const json h = stateValue(state, "message/hop"); h.is_number_integer()) {
        m.hop = h.get<int>();
    }
    if (const json s = stateValue(state, "message/seq"); s.is_number_integer()) {
        m.seq = s.get<Seq>();
    }
    m.contradiction = str("reply/contradiction");
    m.summary = str("conversation/summary");
    auto seqOf = [&](std::string_view key) {
        const json v = stateValue(state, key);
        return v.is_number() ? v.get<Seq>() : Seq{0};
    };
    m.summaryFrom = seqOf("conversation/from");
    m.summaryThrough = seqOf("conversation/through");
    return m;
}

Seq MessageCache::maxReadySeq() const {
    Seq out = 0;
    forEachReady([&](const MessageRow& m) { out = std::max(out, m.seq); });
    return out;
}

bool MessageCache::noteFieldSet(const std::string& id, const std::string& name, const json& value) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.state != MessageState::Ready) {
        return false;
    }
    if (name == "message/steer-applied") {
        it->second.row.steerApplied = value.is_string() ? value.get<std::string>() : std::string();
        ++version_;
        return true;
    }
    if (name == "conversation/summary") {
        it->second.row.summary = value.is_string() ? value.get<std::string>() : std::string();
        ++version_;
        return true;
    }
    if (name == "conversation/from" || name == "conversation/through") {
        (name == "conversation/from" ? it->second.row.summaryFrom : it->second.row.summaryThrough) =
            value.is_number() ? value.get<Seq>() : Seq{0};
        ++version_;
        return true;
    }
    if (name == "reply/contradiction") {
        it->second.row.contradiction = value.is_string() ? value.get<std::string>() : std::string();
        ++version_;
        return true;
    }
    return false;
}

std::optional<std::string> MessageCache::noteEvent(const Event& e) {
    ++version_;
    auto id = messageIdOf(e);
    if (!id) {
        return std::nullopt;
    }
    return request(*id) ? id : std::nullopt;
}

bool MessageCache::request(const std::string& id) {
    ++version_;
    Entry& entry = entries_[id];
    if (entry.state != MessageState::Unknown) {
        return false;
    }
    entry.state = MessageState::Pending;
    ++pending_;
    return true;
}

void MessageCache::onRead(const std::string& id, const json& state) {
    ++version_;
    Entry& entry = entries_[id];
    if (entry.state == MessageState::Pending) {
        --pending_;
    }
    if (entry.state != MessageState::Ready) {
        ++ready_;
    }
    entry.state = MessageState::Ready;
    entry.row = parseMessage(id, state);
    entry.error.clear();
}

void MessageCache::onReadFailed(const std::string& id, const std::string& code) {
    ++version_;
    Entry& entry = entries_[id];
    if (entry.state == MessageState::Pending) {
        --pending_;
    }
    if (entry.state == MessageState::Ready) {
        --ready_;
    }
    entry.state = MessageState::Failed;
    entry.error = code;
}

std::vector<std::string> MessageCache::resetPending() {
    ++version_;
    std::vector<std::string> again;
    for (auto& [id, entry] : entries_) {
        if (entry.state == MessageState::Pending) {
            entry.state = MessageState::Unknown;
            again.push_back(id);
        }
    }
    pending_ = 0;
    return again;
}

MessageState MessageCache::state(const std::string& id) const {
    auto it = entries_.find(id);
    return it == entries_.end() ? MessageState::Unknown : it->second.state;
}

const MessageRow* MessageCache::get(const std::string& id) const {
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.state == MessageState::Ready ? &it->second.row : nullptr;
}

const std::string* MessageCache::failure(const std::string& id) const {
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.state == MessageState::Failed ? &it->second.error : nullptr;
}

} // namespace ke::chat
