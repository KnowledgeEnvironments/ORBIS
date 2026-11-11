// One mark per agent, for one person: what is worth looking at. A pure read of the fold, so two
// clients of one person agree: `view/seen` on the person's Principal says how far they have looked.
//   NeedsYou  a card or a loud notification from this agent waits on this person
//   Failed    its loop stalled on work this person asked for, and they have not seen it
//   Finished  it said something to this person they have not seen: one job, one mark
//   Working   its loop is running
//   Idle      nothing new
//   Inactive  it has never been woken
// A subagent answering its parent says nothing to the person, so it raises no mark.
#pragma once

#include "model/Principals.hpp"
#include "model/Tasks.hpp"

#include <map>
#include <string>
#include <vector>

namespace ke::model {

enum class Activity { None, Inactive, Idle, Working, Finished, Failed, NeedsYou };

const char* word(Activity a); // `needs you`, for a tooltip

struct ActivityBoard {
    std::map<std::string, Activity> marks; // by agent
    std::map<std::string, Seq> latest;     // what opening that agent's chat has seen
    std::vector<std::string> next;         // what wants a visit, the most pressing first
    int needYou = 0;
    int failed = 0;
    int done = 0;
    int working = 0;
};

// `loud`: unread notifications at attention or above, by sender. `seen` overlays view/seen with what
// this client has written and the world has not confirmed yet.
ActivityBoard activity(const Principals& principals, const Tasks& tasks, const std::string& self,
                       const std::map<std::string, int>& loud, const std::map<std::string, Seq>& seen);

// `2 need you · 1 done`, "" when nothing wants a person.
std::string rollup(const ActivityBoard& board);

// A person's view/seen Field as agent -> seq.
std::map<std::string, Seq> seenOf(const PrincipalInfo* person);

} // namespace ke::model
