// A world snapshot as the client boots from it (sprint 1 item W7/L7): `history {snapshot true}`
// answers {snapshot {seq objects} events next?} where every object is in the per-object shape
// `read` returns:
//
//   object/id, object/class, object/stream, object/names [[namespace name] ...],
//   object/fields {name {field/value field/type field/reader field/author field/set-seq field/set-at}},
//   object/created-seq?, object/created-at?   (else the earliest field set)
//
// Seeding a fold from a snapshot must leave it as the equivalent events would (tests/snapshot_test):
// the snapshot is a derived view of the log, never a second source of truth (ground rule 3), so
// the live tail is subscribed `after snapshot.seq` and applies on top.
#pragma once

#include "model/Event.hpp"
#include "model/Principals.hpp"

#include <functional>
#include <map>
#include <string>

namespace ke::model {

struct SnapshotObject {
    std::string id;
    std::string cls;
    std::string stream;
    std::string name; // the latest binding, "" when unbound
    std::map<std::string, FieldState> fields;
    Seq created = 0;
    std::string createdAt;
    std::string actor; // object/actor when the world records who created it
    json raw;          // the object as received, for consumers that parse it themselves (MessageCache)

    const FieldState* field(std::string_view name) const;
    static SnapshotObject parse(const json& object);
};

struct SnapshotStats {
    std::size_t objects = 0;
    std::size_t principals = 0;
    std::size_t routes = 0;
    std::size_t evaluations = 0;
    std::size_t messages = 0;
    std::size_t others = 0;
};

class Evaluations;
class Tasks;
class Actions;
class Tokens;

// Seed the folds from one page of snapshot objects. `onMessage` receives Message objects (class
// message) so the chat's MessageCache can take their text without a `read`. Tasks and Actions
// are seeded too when given. Call setApplied(seq) on the folds once every page is in.
SnapshotStats seedSnapshot(const json& objects, Principals& principals, Evaluations& evaluations,
                           const std::function<void(const SnapshotObject&)>& onMessage = {}, Tasks* tasks = nullptr,
                           Actions* actions = nullptr, Tokens* tokens = nullptr);

} // namespace ke::model
