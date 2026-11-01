#include "chat/Transcript.hpp"

#include <algorithm>
#include <unordered_map>

namespace ke::chat {

std::vector<TranscriptLine> assembleTranscript(const std::vector<model::MessageRef>& refs, const MessageCache& cache,
                                               const std::string& principal, const std::string& self,
                                               const NameOf& nameOf) {
    std::vector<TranscriptLine> lines;
    std::unordered_map<std::string, std::size_t> byMessage;
    for (const model::MessageRef& r : refs) {
        const bool touches = r.from == principal || r.recipient == principal;
        if (!touches) {
            continue;
        }
        auto it = byMessage.find(r.message);
        if (it != byMessage.end()) {
            TranscriptLine& l = lines[it->second];
            if (!l.via && r.via) {
                l.via = r.via;
            }
            continue;
        }
        TranscriptLine l;
        l.message = r.message;
        l.from = r.from;
        l.fromName = nameOf ? nameOf(r.from) : r.from.substr(0, 8);
        l.at = r.at;
        l.seq = r.seq;
        l.via = r.via;
        // Typed by this listener: from our principal and not produced by an Evaluation (a loop
        // answering on our own behalf, as root does to root, carries `via`).
        l.mine = !self.empty() && r.from == self && !r.via;
        if (const MessageRow* m = cache.get(r.message)) {
            l.text = m->text;
            l.refs = m->refs;
            l.images = m->images;
            l.inReplyTo = m->inReplyTo;
            l.kind = m->kind;
            l.task = m->task;
            l.view = m->view;
            l.tags = m->tags;
            l.thread = m->thread;
            l.askedBy = m->askedBy;
            l.askedByName = m->askedBy.empty() ? std::string() : (nameOf ? nameOf(m->askedBy) : m->askedBy.substr(0, 8));
            l.hop = m->hop;
            l.contradiction = m->contradiction;
            l.summary = m->summary;
            l.summaryFrom = m->summaryFrom;
            l.summaryThrough = m->summaryThrough;
            l.steerApplied = m->steerApplied;
            l.ready = true;
            if (!m->from.empty() && m->from != l.from) {
                l.from = m->from; // the Message is the truth about its author
                l.fromName = nameOf ? nameOf(m->from) : m->from.substr(0, 8);
                l.mine = !self.empty() && m->from == self && !l.via;
            }
            if (!m->device.empty()) {
                l.fromName += " on " + m->device; // the token a session sent it with: "person on hosttwo"
            }
        } else if (const std::string* code = cache.failure(r.message)) {
            l.failure = *code;
        }
        byMessage[r.message] = lines.size();
        lines.push_back(std::move(l));
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const TranscriptLine& a, const TranscriptLine& b) { return a.seq < b.seq; });
    return lines;
}

std::vector<TranscriptLine> assembleThread(const std::string& thread, const std::vector<model::MessageRef>& refs,
                                           const MessageCache& cache, const std::string& self, const NameOf& nameOf) {
    std::vector<TranscriptLine> lines;
    std::unordered_map<std::string, std::size_t> byMessage;
    for (const model::MessageRef& r : refs) {
        const MessageRow* m = cache.get(r.message);
        const bool inThread = r.message == thread || (m && (m->thread == thread || m->id == thread));
        if (!inThread) {
            continue;
        }
        auto it = byMessage.find(r.message);
        if (it != byMessage.end()) {
            TranscriptLine& l = lines[it->second];
            if (!l.via && r.via) {
                l.via = r.via;
            }
            continue;
        }
        TranscriptLine l;
        l.message = r.message;
        l.from = r.from;
        l.fromName = nameOf ? nameOf(r.from) : r.from.substr(0, 8);
        l.at = r.at;
        l.seq = r.seq;
        l.via = r.via;
        l.mine = !self.empty() && r.from == self && !r.via;
        if (m) {
            l.text = m->text;
            l.refs = m->refs;
            l.inReplyTo = m->inReplyTo;
            l.kind = m->kind;
            l.task = m->task;
            l.view = m->view;
            l.tags = m->tags;
            l.thread = m->thread.empty() ? thread : m->thread;
            l.askedBy = m->askedBy;
            l.askedByName = m->askedBy.empty() ? std::string() : (nameOf ? nameOf(m->askedBy) : m->askedBy.substr(0, 8));
            l.hop = m->hop;
            l.contradiction = m->contradiction;
            l.summary = m->summary;
            l.summaryFrom = m->summaryFrom;
            l.summaryThrough = m->summaryThrough;
            l.steerApplied = m->steerApplied;
            l.ready = true;
            if (!m->from.empty() && m->from != l.from) {
                l.from = m->from;
                l.fromName = nameOf ? nameOf(m->from) : m->from.substr(0, 8);
                l.mine = !self.empty() && m->from == self && !l.via;
            }
            if (!m->device.empty()) {
                l.fromName += " on " + m->device; // the token a session sent it with: "person on hosttwo"
            }
        } else if (const std::string* code = cache.failure(r.message)) {
            l.failure = *code;
        }
        byMessage[r.message] = lines.size();
        lines.push_back(std::move(l));
    }
    std::stable_sort(lines.begin(), lines.end(), [](const TranscriptLine& a, const TranscriptLine& b) { return a.seq < b.seq; });
    return lines;
}

std::map<std::string, SummarySpan> summarySpans(const std::vector<TranscriptLine>& lines) {
    std::map<std::string, SummarySpan> spans;
    for (const TranscriptLine& l : lines) {
        if (!l.summary.empty() && l.summaryThrough > 0) {
            spans[l.message] = SummarySpan{l.message, l.summary, l.summaryFrom, l.summaryThrough, 0};
        }
    }
    for (const TranscriptLine& l : lines) {
        if (auto it = spans.find(l.thread); it != spans.end() && spanOf(l, spans)) {
            ++it->second.messages;
        }
    }
    return spans;
}

const SummarySpan* spanOf(const TranscriptLine& line, const std::map<std::string, SummarySpan>& spans) {
    auto it = spans.find(line.thread);
    if (it == spans.end() || line.seq < it->second.from || line.seq > it->second.through) {
        return nullptr;
    }
    return &it->second;
}

std::vector<ThreadGroup> threadGroups(const std::vector<TranscriptLine>& lines) {
    std::vector<ThreadGroup> groups;
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const TranscriptLine& l = lines[i];
        // A Message with no thread of its own is its own thread root when others reply into it.
        const std::string key = !l.thread.empty() ? l.thread : l.message;
        auto it = index.find(key);
        if (it == index.end()) {
            ThreadGroup g;
            g.thread = key;
            g.askedBy = l.askedBy.empty() ? l.from : l.askedBy;
            g.askedByName = l.askedBy.empty() ? l.fromName : l.askedByName;
            g.lines.push_back(i);
            index[key] = groups.size();
            groups.push_back(std::move(g));
        } else {
            ThreadGroup& g = groups[it->second];
            if (g.askedBy.empty() && !l.askedBy.empty()) {
                g.askedBy = l.askedBy;
                g.askedByName = l.askedByName;
            }
            g.lines.push_back(i);
        }
    }
    std::vector<ThreadGroup> out;
    for (ThreadGroup& g : groups) {
        if (g.lines.size() >= 2) {
            out.push_back(std::move(g));
        }
    }
    return out;
}

std::vector<ThreadPlace> threadPlaces(const std::vector<TranscriptLine>& lines, const std::vector<ThreadGroup>& groups,
                                      const std::string& principal) {
    std::vector<ThreadPlace> places(lines.size());
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const ThreadGroup& g = groups[gi];
        for (std::size_t k = 0; k < g.lines.size(); ++k) {
            const std::size_t i = g.lines[k];
            if (i >= lines.size()) {
                continue;
            }
            const TranscriptLine& l = lines[i];
            ThreadPlace& p = places[i];
            p.group = static_cast<int>(gi);
            p.first = k == 0;
            p.last = k + 1 == g.lines.size();
            // Another Principal speaking inside a thread this terminal's principal did not ask:
            // indented under the question. The principal's own lines and the asker's stay flush.
            p.depth = (l.from != g.askedBy && l.from != principal) ? 1 : 0;
        }
    }
    return places;
}

std::string transcriptText(const std::vector<TranscriptLine>& lines, std::size_t first, std::size_t last) {
    if (lines.empty()) {
        return "";
    }
    if (first > last) {
        std::swap(first, last);
    }
    last = std::min(last, lines.size() - 1);
    std::string out;
    for (std::size_t i = first; i <= last; ++i) {
        const TranscriptLine& l = lines[i];
        if (!out.empty()) {
            out += "\n";
        }
        out += (l.mine ? "> " : "") + l.fromName + "  " + (l.at.size() >= 19 ? l.at.substr(11, 8) : l.at);
        if (l.via) {
            out += "  [via " + idPrefix(*l.via) + "]";
        }
        out += "\n";
        if (l.ready) {
            out += l.text;
        } else if (!l.failure.empty()) {
            out += "<message unreadable: " + l.failure + ">";
        } else {
            out += "<reading>";
        }
        out += "\n";
    }
    return out;
}

} // namespace ke::chat
