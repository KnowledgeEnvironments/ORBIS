#include "model/EventStore.hpp"

#include "util/Log.hpp"

#include <iterator>

namespace ke {

bool EventStore::append(const std::string& sub, Event event) {
    auto it = lastSeq_.find(sub);
    if (it != lastSeq_.end()) {
        if (event.seq <= it->second) {
            ++duplicates_;
            return false;
        }
        if (event.seq != it->second + 1) {
            GapReport hole{sub, it->second + 1, event.seq, pushStart_.count(sub) == 0};
            holes_.push_back(hole);
            if (onHole) {
                onHole(hole);
            } else {
                log::info("store: sub {} skipped seq {}..{} ({}): filtered by resolution", hole.sub, hole.expected,
                           hole.got - 1, hole.withinPush ? "within a push" : "across pushes");
            }
        }
        it->second = event.seq;
    } else {
        lastSeq_.emplace(sub, event.seq);
    }
    pushStart_.erase(sub);
    if (onEvent) {
        onEvent(sub, event);
    }
    if (!rowSubs.empty() && rowSubs.count(sub) == 0) {
        return true;
    }
    if (event.seq > maxSeq_) {
        maxSeq_ = event.seq;
    }
    rows_.push_back(std::move(event));
    return true;
}

void EventStore::notePush(const std::string& sub) {
    pushStart_.insert(sub);
}

void EventStore::noteRegression(const std::string& sub, Seq cursor, Seq asOf) {
    // Once per distinct regression: a world that stays behind our cursor keeps pushing.
    if (!gaps_.empty() && gaps_.back().sub == sub && gaps_.back().got == asOf) {
        return;
    }
    GapReport gap{sub, cursor, asOf, false};
    gaps_.push_back(gap);
    if (onGap) {
        onGap(gap);
    } else {
        log::warn("store: sub {}: the world's head {} fell below our cursor {}: a different or truncated log", sub, asOf,
                  cursor);
    }
}

std::optional<Seq> EventStore::lastSeq(const std::string& sub) const {
    auto it = lastSeq_.find(sub);
    if (it == lastSeq_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void EventStore::prepend(std::vector<Event> older) {
    const Seq below = oldestSeq();
    while (!older.empty() && older.back().seq >= below) {
        older.pop_back();
    }
    if (older.empty()) {
        return;
    }
    rows_.insert(rows_.begin(), std::make_move_iterator(older.begin()), std::make_move_iterator(older.end()));
    ++prepends_;
}

void EventStore::clear() {
    prepends_ = 0;
    complete_ = false;
    rows_.clear();
    lastSeq_.clear();
    gaps_.clear();
    holes_.clear();
    pushStart_.clear();
    duplicates_ = 0;
    maxSeq_ = -1;
}

} // namespace ke
