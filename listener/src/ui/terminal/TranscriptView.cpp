#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

void TerminalPane::rebuildView(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals,
                               const model::Evaluations& evals, chat::Chat& chat, const std::string& self, float wrap) {
    (void)info;
    View& v = tab.view_;
    const float lineH = ImGui::GetFontSize();
    // The view behind a line (sprint 1 L10): its message/view, else the first tagged Object that
    // read as a view (a client-attached listing). nullptr while unread or when there is none.
    auto viewOf = [&chat](const chat::TranscriptLine& l) -> const chat::ViewSpec* {
        if (!l.view.empty()) {
            if (const chat::ViewSpec* sp = chat.views().get(l.view)) {
                return sp;
            }
        }
        for (const std::string& t : l.tags) {
            if (const chat::ViewSpec* sp = chat.views().get(t)) {
                return sp;
            }
        }
        return nullptr;
    };
    v.lines = chat::assembleTranscript(principals.messages(), chat.messages(), tab.principal, self,
                                       [&principals](const std::string& id) { return principals.nameOf(id); });
    const std::vector<chat::TranscriptLine>& lines = v.lines;
    v.doc = SelDoc{};
    v.doc.version = ++v.builds;
    v.heads.clear();
    v.toggles.clear();
    v.opTips.clear();
    v.costTips.clear();
    v.cards.clear();
    v.views.clear();
    v.imageRows.clear();
    v.rails.clear();
    v.threadToggles.clear();
    v.records.clear();
    v.recordToggles.clear();
    v.md = MdExtras{};
    scan_.table = symbolUi().table();
    scan_.ctx.fold = &principals;
    scan_.ctx.object = tab.principal;
    v.md.scan = &scan_;
    SelDoc& doc = v.doc;
    // Threads: the groups of lines sharing a message/thread, and each line's place in its group.
    const std::vector<chat::ThreadGroup> groups = chat::threadGroups(lines);
    const std::vector<chat::ThreadPlace> places = chat::threadPlaces(lines, groups, tab.principal);
    const float cw = ImGui::CalcTextSize("0").x;
    const float railGutter = 1.5f * cw; // every line of a thread hangs right of the rail
    const float railIndent = 2.0f * cw; // a depth-1 line (└> another Principal inside the thread) two columns further
    const model::NameFn nameFn = [&principals](const std::string& id) { return principals.nameOf(id); };

    // Runs a standing rule admitted, by the Evaluation that asked: they have no card.
    std::unordered_map<std::string, std::vector<const model::TaskInfo*>> autoRuns;
    if (tasks_) {
        for (const model::TaskInfo* t : tasks_->ordered()) {
            if (t->field("task/rule") && !t->evaluation.empty()) {
                autoRuns[t->evaluation].push_back(t);
            }
        }
    }
    auto ruleNote = [&principals](const model::TaskInfo& t) {
        const model::PrincipalInfo* who = principals.get(t.requestedBy);
        const model::FieldState* rules = who ? who->field("policy/approval-rules") : nullptr;
        const model::FieldState* id = t.field("task/rule");
        if (rules && id && id->value.is_string()) {
            for (const chat::ApprovalRule& r : chat::parseRules(rules->value)) {
                if (r.id == id->value.get<std::string>()) {
                    return r.note;
                }
            }
        }
        return std::string();
    };
    // The transcript as one selectable document (ui/SelectableText): per Message a block of
    // paragraphs: the ops its wake ran, the thinking toggles, the head, the body, the cost line.
    auto addEvaluation = [&](const model::EvaluationInfo* ev, int block) {
        if (showThinking && (!ev->thinking.is_null() || !ev->thinkingPartial.empty())) {
            const std::string think = ev->thinking.is_string() ? ev->thinking.get<std::string>()
                                      : !ev->thinking.is_null() ? "blob " + ev->thinking.dump()
                                                                : ev->thinkingPartial;
            const bool open = tab.expandedThinking.count(ev->id) != 0;
            v.toggles[doc.paras.size()] = Toggle{ev->id};
            doc.add(std::string(open ? "  \xe2\x96\xbe thinking (" : "  \xe2\x96\xb8 thinking (") + std::to_string(think.size()) +
                        " chars) // evaluation " + chat::idPrefix(ev->id),
                    Ink::secondary, block, 2);
            if (open) {
                doc.add(think, Ink::secondary, block);
            }
        }
        if (ev->results.is_array() && !ev->results.empty()) {
            for (const json& r : ev->results) {
                const std::string op = r.is_object() && r.contains("op") && r["op"].is_string() ? r["op"].get<std::string>() : "op";
                const bool ok = r.is_object() && r.contains("ok") && r["ok"].is_boolean() && r["ok"].get<bool>();
                std::string line = "  " + op + " -> ";
                if (ok) {
                    line += "ok";
                    if (r.contains("seq") && r["seq"].is_number()) {
                        line += " seq " + std::to_string(r["seq"].get<long long>());
                    }
                } else {
                    const json err = r.is_object() && r.contains("err") ? r["err"] : json();
                    line += "err " + (err.is_object() && err.contains("code") && err["code"].is_string()
                                           ? err["code"].get<std::string>()
                                           : (err.is_string() ? err.get<std::string>() : std::string("?")));
                }
                v.opTips[doc.paras.size()] = r;
                doc.add(line, ok ? Ink::inspected : Ink::inspected, block);
            }
        } else if (ev->ops.is_array() && !ev->ops.empty()) {
            doc.add("  " + std::to_string(ev->ops.size()) + " op(s) asked, no result recorded", Ink::inspected, block);
        }
        if (auto runs = autoRuns.find(ev->id); runs != autoRuns.end()) {
            for (const model::TaskInfo* t : runs->second) {
                doc.add("  " + chat::autoRunLine(*t, ruleNote(*t)), Ink::secondary, block);
            }
        }
        if (ev->cancelled()) {
            doc.add(std::string(ev->reason == "steer" ? "  steered" : "  cancelled") + " // evaluation " + chat::idPrefix(ev->id) +
                        (ev->partial.empty() ? std::string() : " (" + std::to_string(ev->partial.size()) + " chars kept)"),
                    Ink::secondary, block);
            if (!ev->partial.empty()) {
                doc.add(ev->partial, Ink::secondary, block);
            }
        }
    };
    // The wake behind a loop's reply: its Evaluations since the request it answers and since
    // this Principal's previous reply, whichever is later (loops run one wake at a time).
    std::unordered_map<std::string, Seq> seqOfMessage;
    std::unordered_map<std::string, Seq> lastViaSeqFrom;
    const std::map<std::string, chat::SummarySpan> spans = chat::summarySpans(lines);
    std::set<std::string> spanShown;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const chat::TranscriptLine& l = lines[i];
        const int block = static_cast<int>(i);
        // A compressed stretch reads as one dim line; opening it shows the summary and the originals.
        if (const chat::SummarySpan* span = chat::spanOf(l, spans)) {
            const std::string key = "summary:" + span->thread;
            const bool open = tab.expandedThinking.count(key) != 0;
            if (spanShown.insert(span->thread).second) {
                v.toggles[doc.paras.size()] = Toggle{key};
                doc.add("summary of " + std::to_string(span->messages) + " messages [ " + (open ? "HIDE" : "SHOW") + " ]",
                        Ink::secondary, block, 2);
                if (open) {
                    doc.add(span->text, Ink::secondary, block);
                }
            }
            if (!open) {
                seqOfMessage[l.message] = l.seq;
                continue;
            }
        }
        const chat::ThreadPlace& place = places[i];
        const chat::ThreadGroup* group = place.group >= 0 ? &groups[static_cast<std::size_t>(place.group)] : nullptr;
        const bool collapsed = group && tab.collapsedThreads.count(group->thread) != 0;
        if (group && place.first) {
            // The rail header: `── thread · asked by person · N messages ──`, click to collapse.
            View::Rail rail;
            rail.thread = group->thread;
            rail.collapsed = collapsed;
            rail.headerPara = doc.paras.size();
            v.threadToggles[doc.paras.size()] = group->thread;
            // `open thread` is a link in the line itself, like a name in running text: same font, same baseline
            SelPara hp;
            hp.block = block;
            hp.kind = 2;
            const std::string head = std::string(collapsed ? "\xe2\x96\xb8 " : "\xe2\x96\xbe ") + "thread · asked by " + group->askedByName +
                                     " · " + std::to_string(group->lines.size()) + " messages" + (collapsed ? " (root and last shown)" : "") + " · ";
            hp.runs.push_back(SelRun{head, Ink::secondary});
            hp.runs.push_back(SelRun{"open thread", Ink::nav});
            rail.linkBegin = head.size();
            rail.linkEnd = head.size() + std::string("open thread").size();
            doc.add(std::move(hp));
            rail.firstPara = doc.paras.size();
            v.rails.push_back(rail);
        }
        if (collapsed && !place.first && !place.last) {
            seqOfMessage[l.message] = l.seq; // keep the bookkeeping even when the line is folded away
            continue;
        }
        // Noise control: a Trigger's firing (a heartbeat) and an approval decision are system
        // lines: one dim line each under `all`, hidden under `chat`, the only lines under `system`.
        // A status (an interim line a loop sent while an op awaits approval, world pass 13) is a
        // system line too: it is not the answer, the answer comes after the decision.
        const bool system = l.ready && (l.kind == "trigger" || l.kind == "approval-result" || l.kind == "status");
        if ((filter == "chat" && system) || (filter == "system" && !system)) {
            seqOfMessage[l.message] = l.seq;
            continue;
        }
        if (system) {
            SelPara one;
            one.block = block;
            one.kind = 1;
            std::string body = l.text;
            for (char& ch : body) {
                if (ch == '\n') {
                    ch = ' ';
                }
            }
            if (body.size() > 80) {
                body = body.substr(0, 77) + "\u2026";
            }
            one.runs.push_back(SelRun{(place.depth > 0 ? "\u2514> " : std::string()) + (l.kind == "trigger" ? "heartbeat" : l.kind == "status" ? "status" : "approval-result"), Ink::secondary});
            one.runs.push_back(SelRun{"  " + clockPart(l.at) + "  " + l.fromName + "  " + body, Ink::secondary});
            v.heads[doc.paras.size()] = Head{l.from, l.message, l.via.value_or(""), l.inReplyTo.value_or(""), l.seq, ""};
            doc.add(std::move(one));
            seqOfMessage[l.message] = l.seq;
            if (group) {
                doc.paras.back().indent += railGutter + (place.depth > 0 ? railIndent : 0.0f);
                for (View::Rail& r : v.rails) {
                    if (r.thread == group->thread) {
                        r.lastPara = doc.paras.size() - 1;
                    }
                }
            }
            continue;
        }
        // A courtesy reply between loops (hop 3 and beyond) is noise: drawn dim, still there.
        const bool courtesy = l.ready && l.hop >= 3 && l.via.has_value();
        const ImU32 bodyInk = courtesy ? Ink::secondary : (l.mine ? Ink::strong : Ink::primary);
        const std::size_t firstParaOfLine = doc.paras.size();
        std::vector<const model::EvaluationInfo*> wake;
        Seq wakeAfter = 0; // the seq this reply's wake began after (its Tasks were created past it)
        if (l.via) {
            Seq after = 0;
            if (l.inReplyTo) {
                if (auto it = seqOfMessage.find(*l.inReplyTo); it != seqOfMessage.end()) {
                    after = std::max(after, it->second);
                }
            }
            if (auto it = lastViaSeqFrom.find(l.from); it != lastViaSeqFrom.end()) {
                after = std::max(after, it->second);
            }
            wakeAfter = after;
            // exact first: the Evaluations the world recorded against the question this answers
            // (evaluation/message); then this reply's via; then the seq window (older worlds)
            if (l.inReplyTo) {
                wake = evals.forMessage(*l.inReplyTo, l.from);
            }
            if (wake.empty()) {
                if (const model::EvaluationInfo* ev = evals.get(*l.via)) {
                    wake.push_back(ev);
                }
            }
            if (wake.empty()) {
                wake = evals.between(l.from, after, l.seq);
            }
            lastViaSeqFrom[l.from] = l.seq;
        }
        seqOfMessage[l.message] = l.seq;
        for (const model::EvaluationInfo* ev : wake) {
            addEvaluation(ev, block);
        }
        // Head: FROM  hh:mm:ss  [via xxxxxxxx]  <- xxxxxxxx
        SelPara head;
        head.block = block;
        head.kind = 1;
        head.runs.push_back(SelRun{(place.depth > 0 ? "\u2514> " : std::string()) + (l.mine ? "> " + l.fromName : l.fromName),
                                   courtesy ? Ink::secondary : (l.mine ? Ink::strong : Ink::nav)});
        head.runs.push_back(SelRun{"  " + clockPart(l.at), Ink::secondary});
        if (l.via) {
            head.runs.push_back(SelRun{"  [via " + chat::idPrefix(*l.via) + "]", Ink::nav});
        }
        if (l.inReplyTo) {
            head.runs.push_back(SelRun{"  <- " + chat::idPrefix(*l.inReplyTo), Ink::secondary});
        }
        if (!l.contradiction.empty()) {
            head.runs.push_back(SelRun{"  CONTRADICTION", Ink::inspected}); // the prose claimed what the kernel record denies
        }
        v.heads[doc.paras.size()] = Head{l.from, l.message, l.via.value_or(""), l.inReplyTo.value_or(""), l.seq, l.contradiction};
        doc.add(std::move(head));
        // Body: an approval Message is a card (sprint 1 L5): the Task's summary, who asked, the op
        // spelled out, the Evaluation behind it, and the controls slot; once decided, one line.
        const model::TaskInfo* task = (l.ready && tasks_ && !l.task.empty()) ? tasks_->get(l.task) : nullptr;
        // The Task may not be in this listener's fold yet (its creation predates the Grant that
        // lets us see it): the approval Message alone still makes a pending card, from its text.
        model::TaskInfo fromMessage;
        if (l.ready && l.kind == "approval" && !l.task.empty() && (!task || task->kind.empty() || task->summary.empty())) {
            // fill what the Message knows into what the fold has (which may be nothing yet)
            if (task) {
                fromMessage = *task;
            }
            fromMessage.id = l.task;
            fromMessage.kind = "approval";
            if (fromMessage.state.empty()) {
                fromMessage.state = "pending";
            }
            if (fromMessage.summary.empty()) {
                fromMessage.summary = l.text;
            }
            if (fromMessage.requestedBy.empty()) {
                fromMessage.requestedBy = l.from;
            }
            task = &fromMessage;
        }
        if (l.ready && l.kind == "approval" && task) {
            Card card;
            card.task = task->id;
            // A decided card keeps its height while the mouse is on it: nothing moves under the cursor.
            card.pending = task->pending() || tab.heldCard == task->id;
            card.firstPara = doc.paras.size();
            const std::vector<model::OpLine> detail = model::opDetail(task->op, nameFn);
            const std::string asker = principals.nameOf(task->requestedBy.empty() ? l.from : task->requestedBy);
            // One statement of what is asked: the op's detail when there is one, else its summary.
            doc.add(asker + (card.pending ? " needs your approval" : " asked"), Ink::strong, block);
            if (detail.empty() || !card.pending) {
                doc.add(task->summary.empty() ? model::opSummary(task->op, nameFn) : task->summary, Ink::strong, block);
            }
            if (task == &fromMessage && !(tasks_ && tasks_->get(l.task))) {
                doc.add("task " + chat::idPrefix(l.task) + " is still arriving", Ink::secondary, block);
            }
            if (card.pending) {
                for (const model::OpLine& ol : detail) {
                    if (ol.code) {
                        appendMarkdown(doc, "```\n" + ol.text + "\n```", Ink::inspected, block, v.md);
                    } else {
                        doc.add("  " + ol.text, Ink::inspected, block);
                    }
                }
                // The controls slot(s): reserved rows the card draws its items over (drawCards).
                // Under 72 columns the row would wrap and hide DENY behind the next block, so the
                // reason box and the time left get a row of their own.
                card.rows = wrap < 72.0f * cw ? 2 : 1;
                card.controlsPara = doc.paras.size();
                for (int r = 0; r < card.rows; ++r) {
                    SelPara slot;
                    slot.block = block;
                    slot.kind = 5;
                    slot.reserve = ImGui::GetFrameHeight() + 2.0f;
                    slot.source = r == 0 ? "[ APPROVE ] [ DENY ]" : "reason";
                    doc.add(std::move(slot));
                }
            } else {
                doc.add("  " + model::collapsedLine(*task, nameFn, tasks_),
                        task->state == "denied" ? Ink::inspected : task->state == "expired" ? Ink::inspected : Ink::secondary, block);
                card.controlsPara = doc.paras.size() - 1;
            }
            card.lastPara = doc.paras.size() - 1;
            v.cards.push_back(card);
        } else if (l.ready && l.kind == "approval-result" && task) {
            doc.add("  " + model::collapsedLine(*task, nameFn, tasks_), task->state == "denied" ? Ink::inspected : Ink::primary, block);
            if (!l.text.empty()) {
                doc.add(l.text, Ink::secondary, block);
            }
        } else if (l.ready && viewOf(l) != nullptr) {
            // A view (sprint 1 L10): text views render as markdown in the document; the other kinds
            // reserve their height here and are drawn over the slot (drawViewBlocks).
            const chat::ViewSpec* spec = viewOf(l);
            ViewUiState& st = tab.viewUi[spec->id];
            if (spec->kind == "text" || spec->kind.empty()) {
                doc.add("[" + upper(spec->kind.empty() ? std::string("view") : spec->kind) + "] " +
                            (spec->title.empty() ? std::string("untitled view") : spec->title) +
                            (spec->truncated ? " (truncated)" : ""),
                        Ink::strong, block);
                appendMarkdown(doc, chat::textFrom(spec->data), Ink::primary, block, v.md);
            } else if (spec->kind == "tex") {
                // {source}: a ```tex fence is display math, and a whole document stays source.
                doc.add("[TEX] " + (spec->title.empty() ? std::string("untitled view") : spec->title), Ink::strong, block);
                const std::string source = spec->data.is_object() ? spec->data.value("source", std::string()) : chat::textFrom(spec->data);
                appendMarkdown(doc, "```tex\n" + source + "\n```", Ink::primary, block, v.md);
            } else {
                SelPara slot;
                slot.block = block;
                slot.kind = 5;
                slot.reserve = viewBlockHeight(*spec, st, wrap, lineH);
                slot.source = "[" + spec->kind + "] " + spec->title;
                v.views.push_back(ViewBlock{spec->id, doc.paras.size(), block, l.via.value_or("")});
                doc.add(std::move(slot));
            }
            if (!l.text.empty() && l.text != spec->title && l.kind != "view") {
                doc.add(l.text, l.mine ? Ink::strong : Ink::secondary, block); // an attach line keeps its text
            }
        } else if (l.ready && l.kind == "view" && !l.view.empty()) {
            if (const std::string* code = chat.views().failure(l.view)) {
                doc.add("<view unreadable: " + *code + ">", Ink::inspected, block);
            } else {
                doc.add("<view " + chat::idPrefix(l.view) + " loading…>", Ink::secondary, block);
            }
        } else if (l.ready) {
            // A loop's reply may end with the kernel record of the commands its wake ran (world
            // pass 13): the prose renders as usual, the record as a framed box whose numbers come
            // from the Task Fields when the fold has them, else from the text the world wrote.
            // (the heading is the world's own fixed text; a person never types it, so no `via` is required:
            // a session booted from a snapshot may not know the ref's via)
            const chat::KernelRecord rec = !l.mine ? chat::splitKernelRecord(l.text) : chat::KernelRecord{};
            const std::string& prose = rec.present ? rec.body : l.text;
            if (l.kind == "steer") {
                std::string status = "STEER \u00b7 steering\u2026";
                if (!l.steerApplied.empty()) {
                    const model::EvaluationInfo* ev = evals.get(l.steerApplied);
                    status = "STEER \u00b7 applied" + (ev ? " at iteration " + std::to_string(ev->iteration) : std::string());
                }
                doc.add(status, Ink::unresolved, block);
            }
            if (!prose.empty()) {
                // A Message with refs carries its links; the matcher is for text that has none.
                std::vector<chat::RefSpan> spans;
                if (l.refs.is_array()) {
                    spans = chat::spansIn(chat::parseRefs(l.refs, l.text), l.text, prose);
                    v.md.refs = &spans;
                }
                if (!l.mine && looksLikeMarkdown(prose)) {
                    appendMarkdown(doc, prose, bodyInk, block, v.md);
                } else {
                    appendLinked(doc, prose, bodyInk, block, v.md);
                }
                v.md.refs = nullptr;
            }
            if (l.images.is_array() && !l.images.empty()) {
                SelPara slot;
                slot.block = block;
                slot.kind = 5;
                slot.reserve = messageImages().rowHeight(l.message, l.images, lineH);
                slot.source = "[" + std::to_string(l.images.size()) + " image(s)]";
                v.imageRows.push_back(View::ImageRow{l.message, doc.paras.size(), l.images});
                doc.add(std::move(slot));
            }
            if (rec.present) {
                std::vector<chat::RunRecord> runs = rec.runs;
                if (tasks_) {
                    // the run Tasks this wake created: same command, same owner, created inside the wake
                    for (chat::RunRecord& run : runs) {
                        for (const model::TaskInfo* t : tasks_->ordered()) {
                            if (t->approval() || t->created <= wakeAfter || t->created > l.seq) {
                                continue;
                            }
                            const model::FieldState* cmdF = t->field("task/command");
                            const std::string cmd = cmdF && cmdF->value.is_string() ? cmdF->value.get<std::string>() : t->summary;
                            const std::string owner = !t->owner.empty() ? t->owner : t->requestedBy;
                            if (cmd != run.cmd || (!owner.empty() && owner != l.from)) {
                                continue;
                            }
                            run.task = t->id;
                            if (const model::FieldState* ex = t->field("task/exit"); ex && ex->value.is_number()) {
                                run.exit = ex->value.get<long long>();
                            }
                            if (const model::FieldState* du = t->field("task/duration-ms"); du && du->value.is_number()) {
                                run.durationS = du->value.get<long long>() / 1000;
                            }
                            if (const model::FieldState* so = t->field("task/stdout"); so && so->value.is_string()) {
                                std::vector<std::string> tail;
                                std::string out = chat::stripAnsi(so->value.get<std::string>()); // a test runner's colours
                                while (!out.empty() && out.back() == '\n') {
                                    out.pop_back();
                                }
                                std::size_t end = out.size();
                                while (tail.size() < 3 && end > 0) {
                                    const std::size_t nl = out.rfind('\n', end - 1);
                                    tail.insert(tail.begin(), out.substr(nl == std::string::npos ? 0 : nl + 1, end - (nl == std::string::npos ? 0 : nl + 1)));
                                    if (nl == std::string::npos) {
                                        break;
                                    }
                                    end = nl;
                                }
                                if (!tail.empty()) {
                                    run.tail = std::move(tail);
                                }
                            }
                            break;
                        }
                    }
                }
                const bool contra = !l.contradiction.empty();
                const bool folded = !contra && tab.collapsedRecords.count(l.message) != 0; // a contradiction keeps it open
                View::Record record;
                record.message = l.message;
                record.contradiction = contra;
                record.firstPara = doc.paras.size();
                std::string title = std::string(folded ? "\xe2\x96\xb8 " : "\xe2\x96\xbe ") + "KERNEL RECORD \u00b7 " + std::to_string(runs.size()) + (runs.size() == 1 ? " run" : " runs");
                if (folded) {
                    title += " \u00b7 exit";
                    for (const chat::RunRecord& run : runs) {
                        title += " " + (run.exit ? std::to_string(*run.exit) : std::string("?"));
                    }
                }
                v.recordToggles[doc.paras.size()] = l.message;
                doc.add(title, contra ? Ink::inspected : Ink::secondary, block, 2);
                if (!folded) {
                    for (const chat::RunRecord& run : runs) {
                        SelPara row;
                        row.block = block;
                        row.indent = 2.0f * cw;
                        row.runs.push_back(SelRun{run.cmd, Ink::inspected});
                        if (run.exit) {
                            row.runs.push_back(SelRun{"  \u2192 exit " + std::to_string(*run.exit), *run.exit == 0 ? Ink::primary : Ink::inspected});
                        } else {
                            row.runs.push_back(SelRun{"  \u2192 exit ?", Ink::secondary});
                        }
                        if (run.durationS) {
                            row.runs.push_back(SelRun{"  (" + std::to_string(*run.durationS) + " s)", Ink::secondary});
                        }
                        if (!run.task.empty()) {
                            row.runs.push_back(SelRun{"  task " + chat::idPrefix(run.task), Ink::secondary});
                        }
                        json tip{{"cmd", run.cmd}, {"tail", run.tail}};
                        if (run.exit) { tip["exit"] = *run.exit; }
                        if (run.durationS) { tip["duration_s"] = *run.durationS; }
                        if (!run.task.empty()) { tip["task"] = run.task; tip["source"] = "task fields"; } else { tip["source"] = "the world's record text"; }
                        v.opTips[doc.paras.size()] = tip;
                        doc.add(std::move(row));
                        for (const std::string& t : run.tail) {
                            SelPara tl;
                            tl.block = block;
                            tl.indent = 4.0f * cw;
                            addLinkedRuns(tl, doc.paras.size(), SelRun{t, Ink::secondary}, false, v.md);
                            doc.add(std::move(tl));
                        }
                    }
                    if (contra) {
                        SelPara c;
                        c.block = block;
                        c.indent = 2.0f * cw;
                        c.runs.push_back(SelRun{"contradiction: " + l.contradiction, Ink::inspected});
                        doc.add(std::move(c));
                    }
                }
                record.lastPara = doc.paras.size() - 1;
                v.records.push_back(record);
            }
        } else if (!l.failure.empty()) {
            doc.add("<message unreadable: " + l.failure + ">", Ink::inspected, block);
        } else {
            doc.add("<reading>", Ink::secondary, block);
        }
        if (!wake.empty()) {
            const std::string cost = model::costLine(wake, evals);
            if (!cost.empty()) {
                std::string tip;
                for (const model::EvaluationInfo* ev : wake) {
                    tip += "evaluation " + ev->id + " (" + ev->state + ")";
                    if (!ev->serialization.empty()) {
                        tip += "\n  serialization " + ev->serialization;
                    }
                    tip += "\n";
                }
                v.costTips[doc.paras.size()] = tip;
                doc.add("  " + cost, Ink::secondary, block);
            }
        }
        doc.add("", Ink::secondary, block, 3); // spacer
        if (group) {
            const float in = railGutter + (place.depth > 0 ? railIndent : 0.0f);
            for (std::size_t pi = firstParaOfLine; pi < doc.paras.size(); ++pi) {
                doc.paras[pi].indent += in;
            }
        }
        if (group) {
            for (View::Rail& r : v.rails) {
                if (r.thread == group->thread) {
                    r.lastPara = doc.paras.size() - 2; // up to the line's last paragraph, before its spacer
                }
            }
        }
    }
    // A line from outside the thread can land inside its span, such as the world saying it restarted:
    // it hangs right of the rail like the rest, or the rail runs through its words.
    for (const View::Rail& r : v.rails) {
        for (std::size_t pi = r.firstPara; pi <= r.lastPara && pi < doc.paras.size(); ++pi) {
            doc.paras[pi].indent = std::max(doc.paras[pi].indent, railGutter);
        }
    }
    int extraBlock = static_cast<int>(lines.size());
    // Cancelled wakes no reply followed: a dim block each, with whatever text had arrived.
    const Seq lastLineSeq = lines.empty() ? 0 : lines.back().seq;
    for (const model::EvaluationInfo* ev : evals.cancelledAfter(tab.principal, lastLineSeq)) {
        const int block = extraBlock++;
        SelPara head;
        head.block = block;
        head.kind = 1;
        head.runs.push_back(SelRun{principals.nameOf(tab.principal), Ink::secondary});
        head.runs.push_back(SelRun{std::string(ev->reason == "steer" ? "  steered  [via " : "  cancelled  [via ") + chat::idPrefix(ev->id) + "]", Ink::secondary});
        v.heads[doc.paras.size()] = Head{tab.principal, "", ev->id, "", ev->last, ""};
        doc.add(std::move(head));
        addEvaluation(ev, block);
        doc.add("", Ink::secondary, block, 3);
    }
    // A reply being streamed: one provisional block, dim until the final event lands and the
    // Message itself arrives (the real block then replaces it; it is never shown twice). While
    // only reasoning has arrived the head says `thinking`; with SHOW REASONING on, the reasoning
    // text streams into the same collapsed block the finished Evaluation will have.
    if (const model::EvaluationInfo* sv = evals.streaming(tab.principal)) {
        const int block = extraBlock++;
        SelPara head;
        head.block = block;
        head.kind = 1;
        head.runs.push_back(SelRun{principals.nameOf(tab.principal), Ink::nav});
        head.runs.push_back(SelRun{std::string(sv->replying() ? "  streaming  [via " : "  thinking  [via ") +
                                       chat::idPrefix(sv->id) + "]",
                                   Ink::secondary});
        v.heads[doc.paras.size()] = Head{tab.principal, "", sv->id, "", sv->last, ""};
        doc.add(std::move(head));
        if (showThinking && !sv->thinkingPartial.empty()) {
            const bool open = tab.expandedThinking.count(sv->id) != 0;
            v.toggles[doc.paras.size()] = Toggle{sv->id};
            doc.add(std::string(open ? "  \xe2\x96\xbe thinking (" : "  \xe2\x96\xb8 thinking (") +
                        std::to_string(sv->thinkingPartial.size()) + " chars, live) // evaluation " +
                        chat::idPrefix(sv->id),
                    Ink::secondary, block, 2);
            if (open) {
                doc.add(sv->thinkingPartial, Ink::secondary, block);
            }
        }
        if (sv->replying()) {
            doc.add(sv->partial, Ink::secondary, block);
        }
        doc.add("", Ink::secondary, block, 3);
    }
    doc.finish();
    // Follow the tail when a line arrives or when one fills in (its text lands after the
    // Message is read, which changes the height without changing the count).
    std::size_t shape = lines.size();
    for (const chat::TranscriptLine& l : lines) {
        shape += l.ready ? 1 + l.text.size() : 0;
    }
    for (const View::ImageRow& r : v.imageRows) { // an image row appearing or fitting moves the document
        shape += static_cast<std::size_t>(doc.paras[r.para].reserve);
    }
    for (const ViewBlock& b : v.views) { // a view landing or resizing grows the document too
        shape += static_cast<std::size_t>(doc.paras[b.para].reserve);
    }
    if (const model::EvaluationInfo* sv = evals.streaming(tab.principal)) {
        shape += sv->partial.size() + sv->thinkingPartial.size();
    }
    v.shape = shape;
    v.principalsAt = principals.applied();
    v.symbolsVersion = scan_.table ? scan_.table->version() : 0;
    v.evalsAt = evals.applied();
    v.tasksAt = tasks_ ? tasks_->applied() : -1;
    v.heldCard = tab.heldCard;
    v.themeAt = themeVersion();
    v.messagesVersion = chat.messages().version();
    v.viewsVersion = chat.views().version();
    v.imagesVersion = messageImages().version();
    v.wrap = wrap;
    v.expandedVersion = tab.expandedVersion;
    v.thinking = showThinking;
    v.filter = filter;
    v.self = self;
}

} // namespace ke::ui
