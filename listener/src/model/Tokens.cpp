#include "model/Tokens.hpp"

#include "model/Evaluations.hpp" // epochSeconds
#include "model/Snapshot.hpp"

#include <algorithm>

namespace ke::model {

namespace {

std::string str(const json& j) {
    return j.is_string() ? j.get<std::string>() : std::string();
}

bool isToken(const std::map<std::string, FieldState>& fields) {
    return fields.count("token/principal") != 0 || fields.count("token/hash") != 0;
}

} // namespace

std::string TokenInfo::state(double nowEpoch) const {
    if (revoked()) {
        return "revoked";
    }
    if (!expiresAt.empty()) {
        const double exp = epochSeconds(expiresAt);
        if (exp > 0 && exp < nowEpoch) {
            return "expired";
        }
    }
    return "live";
}

void Tokens::setField(TokenInfo& t, const FieldState& f) {
    t.fields[f.name] = f;
    const std::string& n = f.name;
    if (n == "token/principal") {
        t.principal = str(f.value);
    } else if (n == "token/label") {
        t.label = str(f.value);
        if (onLabel && !t.label.empty()) {
            onLabel(t.id, t.label);
        }
    } else if (n == "token/issued-by") {
        t.issuedBy = str(f.value);
    } else if (n == "token/expires-at") {
        t.expiresAt = str(f.value);
    } else if (n == "token/last-used") {
        t.lastUsed = str(f.value);
    } else if (n == "token/revoked-at") {
        t.revokedAt = str(f.value);
    } else if (n == "token/revoke-reason" || n == "token/reason") {
        t.reason = str(f.value);
    }
}

bool Tokens::apply(const Event& e) {
    if (e.seq <= applied_) {
        return false;
    }
    applied_ = e.seq;
    if (e.kind == "created" && e.body.is_object()) {
        const json& fields = e.body.contains("fields") ? e.body["fields"] : json::object();
        if (!fields.is_object() || !(fields.contains("token/principal") || fields.contains("token/hash"))) {
            return false;
        }
        TokenInfo& t = tokens_[e.stream];
        t.id = e.stream;
        t.created = e.seq;
        t.createdAt = e.at;
        t.last = e.seq;
        for (auto& [name, value] : fields.items()) {
            FieldState f;
            f.name = name;
            f.value = value;
            f.by = e.actor;
            f.at = e.at;
            f.seq = e.seq;
            setField(t, f);
        }
        return true;
    }
    if (e.kind == "field-set" && e.body.is_object()) {
        auto it = tokens_.find(e.stream);
        if (it == tokens_.end()) {
            // a token Object whose creation the fold never saw (a Grant came later): the first
            // token/* Field starts it
            const std::string name = str(e.body.value("name", json()));
            if (name.rfind("token/", 0) != 0) {
                return false;
            }
            TokenInfo& t = tokens_[e.stream];
            t.id = e.stream;
            t.created = e.seq;
            t.createdAt = e.at;
            it = tokens_.find(e.stream);
        }
        TokenInfo& t = it->second;
        FieldState f;
        f.name = str(e.body.value("name", json()));
        f.type = e.body.contains("type") ? e.body["type"] : json();
        f.value = e.body.contains("value") ? e.body["value"] : json();
        f.reader = str(e.body.value("reader", json()));
        f.by = e.body.contains("by") && e.body["by"].is_string() ? e.body["by"].get<std::string>() : e.actor;
        f.at = e.at;
        f.seq = e.seq;
        if (f.name.empty()) {
            return false;
        }
        setField(t, f);
        t.last = e.seq;
        return true;
    }
    return false;
}

void Tokens::seed(const SnapshotObject& o) {
    if (!isToken(o.fields)) {
        return;
    }
    TokenInfo& t = tokens_[o.id];
    t.id = o.id;
    t.created = o.created;
    t.createdAt = o.createdAt;
    for (const auto& [name, f] : o.fields) {
        setField(t, f);
        t.last = std::max(t.last, f.seq);
    }
}

const TokenInfo* Tokens::get(std::string_view id) const {
    auto it = tokens_.find(std::string(id));
    return it == tokens_.end() ? nullptr : &it->second;
}

std::vector<const TokenInfo*> Tokens::forPrincipal(std::string_view principal) const {
    std::vector<const TokenInfo*> out;
    for (const auto& [id, t] : tokens_) {
        if (t.principal == principal) {
            out.push_back(&t);
        }
    }
    std::sort(out.begin(), out.end(), [](const TokenInfo* a, const TokenInfo* b) { return a->created < b->created; });
    return out;
}

} // namespace ke::model
