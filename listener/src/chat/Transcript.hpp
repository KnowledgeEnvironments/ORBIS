// A Principal's conversation, assembled from the fold's MessageRefs (`sent` / `delivered`
// events) and the MessageCache (the Message objects, read once). Pure: no network, no UI.
// One line per Message that the Principal sent or received, in log order; a Message seen as
// both `sent` and `delivered` appears once, at its first seq.
#pragma once

#include "chat/MessageCache.hpp"
#include "model/Principals.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ke::chat {

struct TranscriptLine {
    std::string message;
    std::string from;
    std::string fromName;
    std::string at;
    Seq seq = 0;
    std::optional<std::string> via;       // the Evaluation that produced it (a loop's reply)
    std::optional<std::string> inReplyTo; // from the Message once read
    std::string text;                     // "" until the Message is read
    json images;                          // message/images once read, null when it has none
    json refs;                            // message/refs once read, null when it has none
    std::string kind;                     // message/kind once read: "" text, "approval", "approval-result"
    std::string task;                     // message/task once read: the Task an approval carries
    std::string view;                     // message/view once read: the view Object a view Message renders
    std::vector<std::string> tags;        // message/tags once read (an attached file, a tagged view)
    std::string thread;                   // message/thread once read: the conversation's root Message
    std::string askedBy;                  // message/asked-by once read: who started the thread
    std::string askedByName;
    int hop = 0;                          // message/hop once read (≥ 3: a courtesy reply between loops)
    std::string steerApplied;             // message/steer-applied once set: the Evaluation that took a steer in
    std::string summary;                  // conversation/summary, on the line of a thread's root
    Seq summaryFrom = 0;
    Seq summaryThrough = 0;
    std::string contradiction;            // reply/contradiction once set: the prose contradicts the kernel record
    bool ready = false;                   // text is known
    std::string failure;                  // the read's err code when it failed
    bool mine = false;                    // typed by this listener (our principal, no via)
    bool operator==(const TranscriptLine&) const = default;
};

using NameOf = std::function<std::string(const std::string& id)>;

// The plain text of lines[first..last] (inclusive, clamped, either order) for the clipboard:
// one head line `FROM  hh:mm:ss  [via xxxxxxxx]` then the body, a blank line between messages.
// Unread bodies copy as `<reading>`; failed ones as `<message unreadable: code>`.
std::string transcriptText(const std::vector<TranscriptLine>& lines, std::size_t first, std::size_t last);

// Threads (sprint 1, world pass 8): the lines that share a `message/thread`, in the order they
// appear. Singletons are not groups. `depth` is 1 for a line inside a thread that neither the
// asker nor the terminal's principal wrote (grid answering root inside the person's thread, seen
// from root's terminal): it draws indented under the question with `↳ grid`.
struct ThreadGroup {
    std::string thread;
    std::string askedBy;
    std::string askedByName;
    std::vector<std::size_t> lines; // indices into the transcript, ascending
};
struct ThreadPlace {
    int group = -1; // index into the groups, -1 when the line is not in one
    int depth = 0;
    bool first = false;
    bool last = false;
};
// The whole conversation of one thread (the OPEN THREAD pane): every Message any Principal sent
// or received whose message/thread is `thread` (or which is the thread root itself), each once,
// in log order, from the fold's refs and the message cache. `self` marks our own lines.
std::vector<TranscriptLine> assembleThread(const std::string& thread, const std::vector<model::MessageRef>& refs,
                                           const MessageCache& cache, const std::string& self, const NameOf& nameOf);

std::vector<ThreadGroup> threadGroups(const std::vector<TranscriptLine>& lines);
std::vector<ThreadPlace> threadPlaces(const std::vector<TranscriptLine>& lines, const std::vector<ThreadGroup>& groups,
                                      const std::string& principal);

// A compressed stretch of a conversation: the thread, what the summary says, and how many of the
// lines here it stands for. Nothing is gone: the Messages stay on the log and one click away.
struct SummarySpan {
    std::string thread;
    std::string text;
    Seq from = 0;
    Seq through = 0;
    int messages = 0;
};

std::map<std::string, SummarySpan> summarySpans(const std::vector<TranscriptLine>& lines);

// The span a line falls under, or null.
const SummarySpan* spanOf(const TranscriptLine& line, const std::map<std::string, SummarySpan>& spans);

// Lines for `principal`; `self` marks the lines typed by this listener's principal.
std::vector<TranscriptLine> assembleTranscript(const std::vector<model::MessageRef>& refs, const MessageCache& cache,
                                               const std::string& principal, const std::string& self,
                                               const NameOf& nameOf);

} // namespace ke::chat
