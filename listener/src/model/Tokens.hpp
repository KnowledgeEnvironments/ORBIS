// Tokens as the listener folds them (sprint 2, world pass 11): a token is an Object whose Fields
// name the Principal it attaches, a label for the device, who issued it, when it expires, when it
// was last used and when it was revoked. The secret is never a Field and never on the log; the
// world keeps a hash. Nothing here is special to any Principal: whatever token Objects the world
// shows the fold are the DEVICES rows.
//
//   created {class object fields {token/principal ...}}  -> a token
//   field-set on a token's stream                        -> token/last-used, token/revoked-at, ...
#pragma once

#include "model/Event.hpp"
#include "model/Principals.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ke::model {

struct TokenInfo {
    std::string id;
    std::string principal; // token/principal
    std::string label;     // token/label
    std::string issuedBy;  // token/issued-by
    std::string expiresAt; // token/expires-at (ISO-8601), "" when none
    std::string lastUsed;  // token/last-used
    std::string revokedAt; // token/revoked-at, "" while live
    std::string reason;    // token/revoke-reason when the world records one
    Seq created = 0;
    std::string createdAt;
    Seq last = 0;
    std::map<std::string, FieldState> fields;
    bool revoked() const { return !revokedAt.empty(); }
    // `live`, `revoked`, `expired` at nowEpoch (seconds).
    std::string state(double nowEpoch) const;
};

struct SnapshotObject;

class Tokens {
  public:
    bool apply(const Event& e);
    void seed(const SnapshotObject& o);
    void setApplied(Seq seq) { applied_ = std::max(applied_, seq); }
    const TokenInfo* get(std::string_view id) const;
    // The tokens of a Principal in creation order.
    std::vector<const TokenInfo*> forPrincipal(std::string_view principal) const;
    std::size_t size() const { return tokens_.size(); }
    Seq applied() const { return applied_; }
    // Called with every (id, label) the fold learns, so names resolve through Principals::nameOf.
    std::function<void(const std::string&, const std::string&)> onLabel;

  private:
    void setField(TokenInfo& t, const FieldState& f);
    std::map<std::string, TokenInfo> tokens_;
    Seq applied_ = -1;
};

} // namespace ke::model
