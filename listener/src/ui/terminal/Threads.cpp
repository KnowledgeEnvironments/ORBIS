#include "ui/terminal/Internal.hpp"
#include "ui/Fonts.hpp"

namespace ke::ui {

using namespace term;

void TerminalPane::drawExpandedViews(model::Principals& principals, chat::Chat& chat, ImFont* mono) {
    drawPromptView(principals, mono);
    // Expanded views ([ EXPAND ] on a table, a click on an image): their own top-level windows,
    // begun outside the pane so the pane's draw list and font state are not disturbed.
    if (current_ < 0 || tabs_.empty()) {
        return;
    }
    Tab& tab = tabs_[static_cast<std::size_t>(current_)];
    for (auto& [vid, vst] : tab.viewUi) {
        if (!vst.expanded) {
            continue;
        }
        if (const chat::ViewSpec* spec = chat.views().get(vid)) {
            const ViewActions acts = drawExpandedView(*spec, vst, mono, "xview/" + vid,
                                                      [&principals](const std::string& id) { return principals.nameOf(id); });
            if (!acts.openRef.empty()) {
                openRef(tab, principals, acts.openRef);
            }
            if (acts.changed) {
                ++tab.expandedVersion;
            }
        }
    }
}

void TerminalPane::drawThreadWindows(model::Principals& principals, const model::Evaluations& evals, chat::Chat& chat,
                                     const Session& session, ImFont* mono) {
    if (current_ < 0 || tabs_.empty()) {
        return;
    }
    drawThreadPanes(tabs_[static_cast<std::size_t>(current_)], principals, evals, chat, session, mono);
}

void TerminalPane::rebuildThreadDoc(Tab& tab, Tab::ThreadUi& tu, model::Principals& principals, const model::Evaluations& evals,
                                    chat::Chat& chat, const std::string& self, float wrap) {
    (void)tab;
    (void)wrap;
    tu.lines = chat::assembleThread(tu.thread, principals.messages(), chat.messages(), self,
                                    [&principals](const std::string& id) { return principals.nameOf(id); });
    tu.doc = SelDoc{};
    tu.doc.version = ++tu.builds;
    tu.heads.clear();
    tu.costTips.clear();
    tu.md = MdExtras{};
    scan_.table = symbolUi().table();
    scan_.ctx.fold = &principals;
    scan_.ctx.object = tab.principal;
    tu.md.scan = &scan_;
    SelDoc& doc = tu.doc;
    const float cw = ImGui::CalcTextSize("0").x;
    std::string asker;
    for (const chat::TranscriptLine& l : tu.lines) {
        if (!l.askedBy.empty()) {
            asker = l.askedBy;
            break;
        }
    }
    if (asker.empty() && !tu.lines.empty()) {
        asker = tu.lines.front().from;
    }
    std::unordered_map<std::string, Seq> lastViaSeqFrom;
    std::unordered_map<std::string, Seq> seqOfMessage;
    for (std::size_t i = 0; i < tu.lines.size(); ++i) {
        const chat::TranscriptLine& l = tu.lines[i];
        const int block = static_cast<int>(i);
        // a line by neither the asker nor the thread's first recipient hangs one step in
        const bool side = !asker.empty() && l.from != asker && l.hop >= 2;
        const std::size_t first = doc.paras.size();
        std::vector<const model::EvaluationInfo*> wake;
        // the wake behind a loop's line: its Evaluations since the question it answers and since its
        // previous line (a snapshot-booted session may not know the ref's via, so any line that is
        // not ours is tried by principal and seq window)
        if (l.via || !l.mine) {
            Seq after = 0;
            if (l.inReplyTo) {
                if (auto it = seqOfMessage.find(*l.inReplyTo); it != seqOfMessage.end()) {
                    after = std::max(after, it->second);
                }
            }
            if (auto it = lastViaSeqFrom.find(l.from); it != lastViaSeqFrom.end()) {
                after = std::max(after, it->second);
            }
            if (l.inReplyTo) {
                wake = evals.forMessage(*l.inReplyTo, l.from); // exact: evaluation/message
            }
            if (wake.empty() && l.via) {
                if (const model::EvaluationInfo* ev = evals.get(*l.via)) {
                    wake.push_back(ev);
                }
            }
            if (wake.empty()) {
                wake = evals.between(l.from, after, l.seq);
            }
            if (!wake.empty()) {
                lastViaSeqFrom[l.from] = l.seq;
            }
        }
        seqOfMessage[l.message] = l.seq;
        SelPara head;
        head.block = block;
        head.kind = 1;
        head.runs.push_back(SelRun{(side ? "\u2514> " : std::string()) + (l.mine ? "> " + l.fromName : l.fromName), l.mine ? Ink::strong : Ink::nav});
        head.runs.push_back(SelRun{"  " + clockPart(l.at), Ink::secondary});
        if (l.via) {
            head.runs.push_back(SelRun{"  [via " + chat::idPrefix(*l.via) + "]", Ink::nav});
        }
        if (l.hop > 0) {
            head.runs.push_back(SelRun{"  hop " + std::to_string(l.hop), Ink::secondary});
        }
        if (!l.kind.empty() && l.kind != "approval") {
            head.runs.push_back(SelRun{"  " + l.kind, Ink::secondary});
        }
        if (!l.contradiction.empty()) {
            head.runs.push_back(SelRun{"  CONTRADICTION", Ink::inspected});
        }
        tu.heads[doc.paras.size()] = Head{l.from, l.message, l.via.value_or(""), l.inReplyTo.value_or(""), l.seq, l.contradiction};
        doc.add(std::move(head));
        if (l.ready) {
            const chat::KernelRecord rec = !l.mine ? chat::splitKernelRecord(l.text) : chat::KernelRecord{};
            const std::string& prose = rec.present ? rec.body : l.text;
            const bool systemKind = l.kind == "status" || l.kind == "trigger" || l.kind == "approval-result";
            const ImU32 ink = systemKind ? Ink::secondary : (l.mine ? Ink::strong : Ink::primary);
            if (systemKind) {
                // one short dim line: a decision's result dump or a heartbeat is bookkeeping here
                std::string one = prose;
                for (char& ch : one) {
                    if (ch == '\n') {
                        ch = ' ';
                    }
                }
                if (one.size() > 160) {
                    one = one.substr(0, 157) + "\u2026";
                }
                doc.add(one, ink, block);
            } else if (!prose.empty()) {
                std::vector<chat::RefSpan> spans;
                if (l.refs.is_array()) {
                    spans = chat::spansIn(chat::parseRefs(l.refs, l.text), l.text, prose);
                    tu.md.refs = &spans;
                }
                if (!l.mine && looksLikeMarkdown(prose)) {
                    appendMarkdown(doc, prose, ink, block, tu.md);
                } else {
                    appendLinked(doc, prose, ink, block, tu.md);
                }
                tu.md.refs = nullptr;
            }
            for (const chat::RunRecord& run : rec.runs) {
                SelPara row;
                row.block = block;
                row.indent = 2.0f * cw;
                row.runs.push_back(SelRun{run.cmd, Ink::inspected});
                row.runs.push_back(SelRun{"  \u2192 exit " + (run.exit ? std::to_string(*run.exit) : std::string("?")), run.exit && *run.exit == 0 ? Ink::primary : Ink::inspected});
                if (run.durationS) {
                    row.runs.push_back(SelRun{"  (" + std::to_string(*run.durationS) + " s)", Ink::secondary});
                }
                doc.add(std::move(row));
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
                    tip += "evaluation " + ev->id + " (" + ev->state + ")\n";
                }
                tu.costTips[doc.paras.size()] = tip;
                doc.add("  " + cost, Ink::secondary, block);
            }
        }
        doc.add("", Ink::secondary, block, 3);
        if (side) {
            for (std::size_t pi = first; pi < doc.paras.size(); ++pi) {
                doc.paras[pi].indent += 2.0f * cw;
            }
        }
    }
    doc.finish();
    tu.messagesVersion = chat.messages().version();
    tu.principalsAt = principals.applied();
    tu.evalsAt = evals.applied();
    tu.themeAt = themeVersion();
}

void TerminalPane::drawThreadPanes(Tab& tab, model::Principals& principals, const model::Evaluations& evals, chat::Chat& chat,
                                   const Session& session, ImFont* mono) {
    const std::string self = session.principal().value_or("");
    std::vector<std::string> closed;
    for (auto& [tid, tu] : tab.threadUi) {
        // ask the world once for the whole thread; the fold renders either way
        if (!tu.requested && session.attached()) {
            tu.requested = true;
            Tab::ThreadUi* target = &tu;
            chat::Chat* ch = &chat;
            chat.history(json{{"thread", tid}, {"limit", 500}}, [target, ch](const Response& r) {
                if (r.ok) {
                    target->took = ch->ingestHistory(r.result);
                    target->source = "world";
                } else {
                    target->source = "fold";
                    target->err = r.err ? r.err->code : std::string("?");
                }
                target->messagesVersion = ~0ULL; // rebuild
            });
        }
        const std::string title = "THREAD // " + (tu.lines.empty() ? chat::idPrefix(tid) : "asked by " + (tu.lines.front().askedByName.empty() ? tu.lines.front().fromName : tu.lines.front().askedByName)) +
                                  " \u00b7 " + std::to_string(tu.lines.size()) + " messages###thread/" + tid;
        ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
            if (bracketButtonNamed("close", "close-thread:" + chat::idPrefix(tid))) {
                open = false;
            }
            ImGui::SameLine();
            if (bracketButtonNamed("copy all", "copy-thread:" + chat::idPrefix(tid), !tu.lines.empty())) {
                ImGui::SetClipboardText(chat::transcriptText(tu.lines, 0, tu.lines.empty() ? 0 : tu.lines.size() - 1).c_str());
            }
            ImGui::SameLine();
            text(Ink::secondary, "thread " + chat::idPrefix(tid) + " \u00b7 " + (tu.source == "world" ? "from the world (history thread, " + std::to_string(tu.took) + " taken)" : tu.source == "fold" ? "from the fold (history thread: " + tu.err + ")" : "loading\u2026"));
            ImGui::BeginChild("thread-body", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
            pushMono(); // as in the transcript: one size for the text and everything set in it
            const float wrap = ImGui::GetContentRegionAvail().x;
            if (tu.themeAt != themeVersion() || tu.messagesVersion != chat.messages().version() || tu.principalsAt != principals.applied() || tu.evalsAt != evals.applied()) {
                rebuildThreadDoc(tab, tu, principals, evals, chat, self, wrap);
            }
            if (tu.lines.empty()) {
                dimText("no messages of this thread are in the fold yet");
            }
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawListSplitter split;
            split.Split(ImGui::GetWindowDrawList(), 3);
            split.SetCurrentChannel(ImGui::GetWindowDrawList(), 1);
            const SelResult sr = drawSelectableDoc(tu.doc, tu.sel, wrap, mono, ImGui::GetFontSize(), true, &tu.layout);
            split.SetCurrentChannel(ImGui::GetWindowDrawList(), 2);
            drawMarkdownExtras(tu.md, tu.doc, tu.layout, origin, wrap, sr.hoveredPara, ("thread/" + tid).c_str(), &split);
            split.Merge(ImGui::GetWindowDrawList());
            symbolUi().handleDoc(tu.md, tu.doc, sr, tu.sel.dragging);
            if (sr.hoveredPara >= 0 && !tu.sel.dragging) {
                const std::size_t hp = static_cast<std::size_t>(sr.hoveredPara);
                if (auto it = tu.heads.find(hp); it != tu.heads.end()) {
                    std::string tip = "from " + it->second.from + "\nmessage " + it->second.message + "\nseq " + std::to_string(it->second.seq);
                    if (!it->second.via.empty()) {
                        tip += "\nevaluation " + it->second.via;
                    }
                    if (!it->second.contradiction.empty()) {
                        tip += "\n\nCONTRADICTION: " + it->second.contradiction;
                    }
                    ke::ui::tip("%s", tip.c_str());
                } else if (auto ct = tu.costTips.find(hp); ct != tu.costTips.end()) {
                    ke::ui::tip("%s", ct->second.c_str());
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                tu.sel.clear();
            }
            popMono();
            ImGui::EndChild();
            insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ImGui::End();
        if (!open) {
            closed.push_back(tid);
        }
    }
    for (const std::string& tid : closed) {
        tab.threadUi.erase(tid);
    }
}

void TerminalPane::drawViewBlocks(Tab& tab, model::Principals& principals, chat::Chat& chat, ImVec2 origin, float wrap,
                                  ImFont* mono) {
    View& view = tab.view_;
    const chat::NameOf nameFn = [&principals](const std::string& id) { return principals.nameOf(id); };
    const ImVec2 after = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetFontSize();
    const ImVec2 winMin = ImGui::GetWindowPos();
    const ImVec2 winMax(winMin.x + ImGui::GetWindowSize().x, winMin.y + ImGui::GetWindowSize().y);
    for (const ViewBlock& b : view.views) {
        const chat::ViewSpec* spec = chat.views().get(b.view);
        if (!spec) {
            continue;
        }
        float y0 = 0.0f, y1 = 0.0f;
        if (!paraSpan(view.layout, b.para, b.para, y0, y1)) {
            continue;
        }
        const ImVec2 a(origin.x + 1.0f, origin.y + y0);
        const ImVec2 z(origin.x + wrap - 1.0f, origin.y + y1);
        if (z.y < winMin.y || a.y > winMax.y) {
            continue; // scrolled away: nothing to draw, no child windows to keep alive
        }
        ViewUiState& st = tab.viewUi[spec->id];
        const ViewActions acts = drawViewBlock(*spec, st, a, z, mono, "view/" + spec->id, nameFn, b.via, lineH);
        if (!acts.openRef.empty()) {
            openRef(tab, principals, acts.openRef);
        }
        if (acts.expandGraph) {
            graphOverlay_ = std::make_pair(spec->title.empty() ? std::string("view") : spec->title, st.graph);
        }
        if (acts.changed) {
            ++tab.expandedVersion;
        }
    }
    ui::restoreCursor(after);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

} // namespace ke::ui
