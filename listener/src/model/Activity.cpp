#include "model/Activity.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace ke::model {

namespace {

// What a person does not need to be called back for: the card raises its own mark, the rest is plumbing.
bool turnEnd(std::string_view kind) {
    constexpr std::array<std::string_view, 6> kNot = {"approval", "approval-result", "trigger", "request", "directive", "steer"};
    return std::find(kNot.begin(), kNot.end(), kind) == kNot.end();
}

bool neverWoken(const PrincipalInfo& p) {
    const FieldState* f = p.field("cost/evaluations");
    return f == nullptr || !f->value.is_number() || f->value.get<double>() <= 0.0;
}

bool cancelled(const std::string& stalled) { return stalled.rfind("cancelled by", 0) == 0; }

} // namespace

const char* word(Activity a) {
    switch (a) {
    case Activity::NeedsYou:
        return "needs you";
    case Activity::Failed:
        return "failed";
    case Activity::Finished:
        return "finished, not seen";
    case Activity::Working:
        return "working";
    case Activity::Idle:
        return "idle";
    case Activity::Inactive:
        return "inactive";
    case Activity::None:
        break;
    }
    return "";
}

ActivityBoard activity(const Principals& principals, const Tasks& tasks, const std::string& self,
                       const std::map<std::string, int>& loud, const std::map<std::string, Seq>& seen) {
    ActivityBoard board;
    if (self.empty()) {
        return board;
    }
    std::map<std::string, Seq> said;   // the last thing an agent said to this person
    std::map<std::string, bool> asked; // this person has spoken to it
    for (const MessageRef& m : principals.messages()) {
        if (m.from == self) {
            asked[m.recipient] = true;
        } else if (m.recipient == self && turnEnd(principals.messageKind(m.message))) {
            said[m.from] = std::max(said[m.from], m.seq);
        }
    }
    std::map<std::string, int> cards;
    for (const TaskInfo* t : tasks.pending()) {
        if (t->approvers.empty() || std::find(t->approvers.begin(), t->approvers.end(), self) != t->approvers.end()) {
            ++cards[t->requestedBy];
        }
    }
    std::vector<std::pair<Seq, std::string>> needs, failed, done;
    for (const PrincipalInfo* p : principals.ordered()) {
        if (p->drivenBy != "process") {
            continue;
        }
        const std::string& id = p->id;
        const auto seenAt = seen.find(id);
        const Seq looked = seenAt != seen.end() ? seenAt->second : -1;
        const auto saidAt = said.find(id);
        const Seq lastSaid = saidAt != said.end() ? saidAt->second : -1;
        const FieldState* stall = p->field("loop/stalled");
        const bool stalled = !p->loopStalled().empty() && !cancelled(p->loopStalled()) && stall != nullptr;
        const Seq stallAt = stalled ? stall->seq : -1;
        board.latest[id] = std::max(lastSaid, stallAt);
        const auto loudAt = loud.find(id);
        const auto cardsAt = cards.find(id);
        Activity a = Activity::Idle;
        if ((cardsAt != cards.end() && cardsAt->second > 0) || (loudAt != loud.end() && loudAt->second > 0)) {
            a = Activity::NeedsYou;
            needs.emplace_back(board.latest[id], id);
        } else if (stalled && stallAt > looked && asked.count(id) != 0) {
            a = Activity::Failed;
            failed.emplace_back(stallAt, id);
        } else if (lastSaid > looked) {
            a = Activity::Finished;
            done.emplace_back(lastSaid, id);
        } else if (loopState(*p) == LoopState::Running) {
            a = Activity::Working;
            ++board.working;
        } else if (neverWoken(*p)) {
            a = Activity::Inactive;
        }
        board.marks[id] = a;
    }
    board.needYou = static_cast<int>(needs.size());
    board.failed = static_cast<int>(failed.size());
    board.done = static_cast<int>(done.size());
    for (auto* group : {&needs, &failed, &done}) {
        std::sort(group->begin(), group->end()); // oldest first: what has waited longest
        for (const auto& [seq, id] : *group) {
            board.next.push_back(id);
        }
    }
    return board;
}

std::string rollup(const ActivityBoard& board) {
    std::string out;
    const auto add = [&out](int n, const std::string& what) {
        if (n > 0) {
            out += (out.empty() ? "" : " \xc2\xb7 ") + std::to_string(n) + " " + what;
        }
    };
    add(board.needYou, board.needYou == 1 ? "needs you" : "need you");
    add(board.failed, "failed");
    add(board.done, "done");
    return out;
}

std::map<std::string, Seq> seenOf(const PrincipalInfo* person) {
    std::map<std::string, Seq> out;
    if (person == nullptr) {
        return out;
    }
    if (const FieldState* f = person->field("view/seen"); f != nullptr && f->value.is_object()) {
        for (const auto& [id, seq] : f->value.items()) {
            if (seq.is_number()) {
                out[id] = seq.get<Seq>();
            }
        }
    }
    return out;
}

} // namespace ke::model
