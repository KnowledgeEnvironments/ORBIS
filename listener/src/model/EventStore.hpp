// Ordered rows as they land, per-sub last seq, and gap detection. Derived state only: the log
// is the only state (ground rule 3); this can be dropped and rebuilt from a subscription.
#pragma once

#include "model/Event.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ke {

struct GapReport {
    std::string sub;
    Seq expected = 0;
    Seq got = 0;
    bool withinPush = false; // the hole sits inside one push (certainly filtered, not lost)
};

class EventStore {
  public:
    // Appends unless seq <= last seq seen on that sub (a duplicate). A seq that is not last+1 is
    // a hole: the world's cursor skipped seqs this Principal may not read (resolution filters
    // every push), so holes are expected for a narrow Principal and are counted, not warned.
    // A push's `from` is the world's cursor before the page, and the world advances that cursor
    // over events its filter dropped without pushing (subscription.clj pump-sub: cursor' is the
    // last seq READ, matched or not). So `from` above our last seq is a hole across pushes, the
    // same filtered hole as one inside a push, never a loss (noteFiltered). Deliveries ride one
    // ordered socket; the one fault the client can see is a regression (noteRegression).
    // Rows are kept for the subs in rowSubs (all when empty); every accepted event also goes to onEvent.
    bool append(const std::string& sub, Event event);
    // The next append on `sub` is the first event of a push (Session::onPush says so): a hole
    // there is `across pushes`, a later one `within a push`.
    void notePush(const std::string& sub);
    // The world's as-of head fell below our cursor on `sub`: a different or truncated log (a
    // `--fresh` world under the same URL, a lost tail). That is a real gap.
    void noteRegression(const std::string& sub, Seq cursor, Seq asOf);

    const std::vector<Event>& rows() const { return rows_; }
    // Older rows paged in on demand, ascending, all below rows().front(). They bypass append:
    // nothing folds them, the fold is current already. prepends() counts it, as row indices move.
    void prepend(std::vector<Event> older);
    std::uint64_t prepends() const { return prepends_; }
    Seq oldestSeq() const { return rows_.empty() ? maxSeq_ + 1 : rows_.front().seq; }
    // Nothing older exists: the first page came back short, or the rows reach the log's start.
    bool complete() const { return complete_; }
    void setComplete() { complete_ = true; }
    std::optional<Seq> lastSeq(const std::string& sub) const;
    Seq maxSeq() const { return maxSeq_; }
    // Seqs skipped by the world's filter (dim `filtered N` in the panes).
    const std::vector<GapReport>& holes() const { return holes_; }
    // Real inconsistencies (red): regressions of the world's head below our cursor.
    const std::vector<GapReport>& gaps() const { return gaps_; }
    std::size_t duplicates() const { return duplicates_; }
    void clear();

    // Where real gaps go; defaults to the stderr logger. Holes are logged at debug level only.
    std::function<void(const GapReport&)> onGap;
    std::function<void(const GapReport&)> onHole;
    // Every accepted event, with its sub, before it is (or is not) kept as a row. The fold over
    // the "fold" sub hangs here.
    std::function<void(const std::string& sub, const Event&)> onEvent;
    // Subs whose events are kept in rows(); empty means every sub.
    std::set<std::string> rowSubs;

  private:
    std::vector<Event> rows_;
    std::map<std::string, Seq> lastSeq_;
    std::vector<GapReport> gaps_;
    std::vector<GapReport> holes_;
    std::set<std::string> pushStart_; // subs whose next append opens a push
    std::size_t duplicates_ = 0;
    Seq maxSeq_ = -1;
    std::uint64_t prepends_ = 0;
    bool complete_ = false;
};

} // namespace ke
