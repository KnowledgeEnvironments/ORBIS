#include "ui/terminal/Internal.hpp"
#include "ui/Fonts.hpp"

#include "platform/Platform.hpp"
#include "util/Base64.hpp"

namespace ke::ui {

namespace {

bool anyModelHas(const model::Principals& principals, std::string_view field) {
    for (const model::PrincipalInfo* p : principals.ordered()) {
        if (p->cls == "model" && p->field(field) != nullptr) {
            return true;
        }
    }
    return false;
}

std::string lastReply(const std::vector<chat::TranscriptLine>& lines, const std::string& self) {
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->from != self && it->kind.empty() && !it->text.empty()) {
            return it->text;
        }
    }
    return {};
}

} // namespace

using namespace term;

void TerminalPane::drawTranscript(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals,
                                  const model::Evaluations& evals, chat::Chat& chat, const Session& session,
                                  const Anim& anim, ImFont* mono) {
    const std::string self = session.principal().value_or("");
    {
        const View& cv = tab.view_;
        const float wrapNow = ImGui::GetContentRegionAvail().x;
        if (cv.principalsAt != principals.applied() || cv.evalsAt != evals.applied() ||
            cv.tasksAt != (tasks_ ? tasks_->applied() : -1) || cv.heldCard != tab.heldCard || cv.themeAt != themeVersion() ||
            cv.symbolsVersion != (symbolUi().table() ? symbolUi().table()->version() : 0) ||
            cv.messagesVersion != chat.messages().version() || cv.imagesVersion != messageImages().version() || cv.viewsVersion != chat.views().version() ||
            cv.expandedVersion != tab.expandedVersion || cv.thinking != showThinking || cv.filter != filter || cv.self != self ||
            (!(cv.views.empty() && cv.cards.empty()) && std::fabs(cv.wrap - wrapNow) > 1.0f)) {
            rebuildView(tab, info, principals, evals, chat, self, wrapNow);
        }
    }
    // Dropped files land here, where the Principal's route is known.
    {
        std::vector<std::string> dropRoutes = info ? info->routes() : std::vector<std::string>{};
        if (dropRoutes.empty() && tab.principal == self && chat.routeState() == chat::RouteState::Found) {
            dropRoutes.push_back(chat.route());
        }
        processDrops(tab, chat, session, dropRoutes);
    }
    const auto attachIt = attach_.find(tab.principal);
    const bool attachLine = attachIt != attach_.end() && !attachIt->second.stage.empty();
    View& view = tab.view_;
    const std::vector<chat::TranscriptLine>& lines = view.lines;
    const SelDoc& doc = view.doc;

    const ImGuiStyle& style = ImGui::GetStyle();
    // A person's own tab is a conversation with someone else: progress, STALLED and STOP follow
    // the Principal it addresses, since a person never runs anything itself.
    const model::PrincipalInfo* workerInfo = principals.get(tab.worker);
    const std::string stalled = workerInfo ? workerInfo->loopStalled() : std::string();
    const bool running = (workerInfo && workerInfo->loopIteration() > 0) || evals.running(tab.worker);
    // A cancel that landed (the Evaluation is done) or that nothing runs for any more clears itself.
    if (!tab.cancelling.empty() && !evals.running(tab.worker)) {
        tab.cancelling.clear();
    }
    float footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
    const float wrapW = ImGui::GetContentRegionAvail().x;
    const bool hasReasoning = info != nullptr && info->drivenBy != "person"; // a person's tab has no model
    if (hasReasoning && !bare) {
        footer += reasoningRowHeight(wrapW, true);
        if (wrapW < ImGui::CalcTextSize("0").x * 72.0f) {
            footer += ImGui::GetFrameHeightWithSpacing(); // the model picker's own row at phone width
        }
    }
    if (running) {
        footer += (bare ? std::max(44.0f, ImGui::GetFrameHeight()) + style.ItemSpacing.y
                        : ImGui::GetFrameHeightWithSpacing()); // the progress line with STOP
    } else if (!stalled.empty()) {
        footer += ImGui::GetFrameHeightWithSpacing(); // the RETRY row
        // The whole reason, wrapped: the user has to read it ("evaluation failed: request timed out").
        footer += ImGui::CalcTextSize(("STALLED " + stalled).c_str(), nullptr, false, wrapW).y + style.ItemSpacing.y;
    }
    if (!tab.sendError.empty()) {
        footer += ImGui::GetTextLineHeightWithSpacing();
    }
    if (!tab.pendingTags.empty()) {
        footer += ImGui::GetFrameHeightWithSpacing(); // the `@` chips above the line
    }
    const model::FieldState* queueField = workerInfo ? workerInfo->field("loop/queue") : nullptr;
    const std::vector<chat::QueueRow> queue = chat::queueRows(queueField ? queueField->value : json(), chat.messages(), tab.queuePending);
    if (!running) {
        tab.queuePending.clear(); // nothing is working any more: the world has them or sent them
    }
    if (!queue.empty()) {
        footer += ImGui::GetTextLineHeightWithSpacing() +
                  std::min(3.0f, static_cast<float>(queue.size())) * (std::max(44.0f, ImGui::GetFrameHeight()) + style.ItemSpacing.y);
    }
    if (running) {
        footer += ImGui::GetTextLineHeightWithSpacing(); // RETURN queues · SHIFT-RETURN steers
    }
    if (!tab.steerError.empty()) {
        footer += ImGui::GetFrameHeightWithSpacing();
    }
    if (!tab.cancelResult.empty()) {
        footer += ImGui::GetTextLineHeightWithSpacing();
    }
    if (attachLine) {
        footer += ImGui::GetTextLineHeightWithSpacing();
    }
    if (!tab.folderPrompt.empty()) {
        footer += ImGui::GetFrameHeightWithSpacing();
    }
    const bool routeKnown = (info && !info->routes().empty()) ||
                            (tab.principal == self && chat.routeState() == chat::RouteState::Found);
    if (!routeKnown) {
        footer += ImGui::GetTextLineHeightWithSpacing(); // the NO ROUTE hint line
    }
    // A person's own messages are for reading: there is nobody here to talk to.
    const bool readOnly = tab.principal == self && info != nullptr && info->drivenBy == "person";
    if (!tab.images.empty()) {
        footer += ImGui::GetFrameHeightWithSpacing(); // the files waiting for the next line
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x, ImGui::GetStyle().FramePadding.y * 2.0f));
    ImGui::BeginChild("transcript", ImVec2(0, readOnly ? 0.0f : ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar * 0);
    pushMono(); // one size in here: the text is monospace at its own size, so cards, views and their controls are too
    if (ImGui::GetScrollMaxY() > 0.0f) {
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        // Scrolling up lets go of the bottom, and being at the bottom takes hold of it again, however
        // you got there: a trackpad's last event lands before the scroll does, so a test on the wheel
        // alone left the transcript loose at the very bottom.
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0.0f) {
            tab.stick = false;
        } else if (atBottom) {
            tab.stick = true;
        }
        if (touchDragScroll()) {
            tab.stick = ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y >= ImGui::GetScrollMaxY() - 2.0f;
        }
    }
    if (lines.empty()) {
        const bool agent = info != nullptr && info->drivenBy == "process";
        ImGui::PushTextWrapPos(0.0f);
        dimText("%s", agent ? "Nothing said yet. Type below to start: it wakes this Principal, and its reply lands here."
                            : "Nothing here yet. What people and agents say to you lands here.");
        ImGui::PopTextWrapPos();
    }
    const float wrap = ImGui::GetContentRegionAvail().x;
    const ImVec2 docOrigin = ImGui::GetCursorScreenPos();
    ImDrawListSplitter split;
    split.Split(ImGui::GetWindowDrawList(), 3);
    split.SetCurrentChannel(ImGui::GetWindowDrawList(), 1);
    const SelResult sr = drawSelectableDoc(doc, tab.sel, wrap, mono, ImGui::GetFontSize(), true, &view.layout);
    split.SetCurrentChannel(ImGui::GetWindowDrawList(), 0);
    // Thread rails: a thin dim line down the left edge of each thread, from its header to its
    // last paragraph; the header text sits on it, the lines hang off it.
    for (const View::Rail& r : view.rails) {
        float y0 = 0.0f, y1 = 0.0f, h0 = 0.0f, h1 = 0.0f;
        if (!paraSpan(view.layout, r.headerPara, r.headerPara, h0, h1) || !paraSpan(view.layout, r.firstPara, r.lastPara, y0, y1)) {
            continue;
        }
        const float gutter = 1.5f * ImGui::CalcTextSize("0").x;
        const float x = std::floor(docOrigin.x + gutter * 0.5f) + 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, docOrigin.y + h1), ImVec2(x, docOrigin.y + y1), Ink::secondary, 1.0f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, docOrigin.y + y1), ImVec2(x + gutter * 0.5f, docOrigin.y + y1), Ink::secondary, 1.0f);
    }
    // `open thread` in each rail header is a link run in the line (TranscriptView): the hand and an
    // underline on hover, a drive target, and a click opens the whole conversation in its own window
    // instead of folding the thread, which a click anywhere else on the header still does.
    std::string threadLinkHit;
    for (const View::Rail& r : view.rails) {
        if (r.headerPara >= doc.starts.size() || r.linkEnd <= r.linkBegin) {
            continue;
        }
        const long long a = static_cast<long long>(doc.starts[r.headerPara] + r.linkBegin);
        const long long z = static_cast<long long>(doc.starts[r.headerPara] + r.linkEnd);
        ImVec2 p0, p1;
        float h0 = 0.0f, h1 = 0.0f;
        const bool placed = paraOffsetPos(doc, view.layout, mono, ImGui::GetFontSize(), docOrigin, r.headerPara, r.linkBegin, p0, h0) &&
                            paraOffsetPos(doc, view.layout, mono, ImGui::GetFontSize(), docOrigin, r.headerPara, r.linkEnd, p1, h1) &&
                            p0.y == p1.y; // one visual line; a wrapped link still works, without the underline
        if (placed && ImGui::IsRectVisible(p0, ImVec2(p1.x, p1.y + h1))) {
            drive::hit("button:thread:" + chat::idPrefix(r.thread), p0, ImVec2(p1.x, p1.y + h1));
        }
        if (sr.hoveredOffset >= a && sr.hoveredOffset < z && !tab.sel.dragging) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (placed) {
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, p0.y + h0 - 1.0f), ImVec2(p1.x, p1.y + h1 - 1.0f), Ink::nav);
            }
            ke::ui::tip("the whole thread %s in its own window: every participant's Messages in order, with their cost", r.thread.c_str());
        }
        if (sr.clickedOffset >= a && sr.clickedOffset < z) {
            threadLinkHit = r.thread;
        }
    }
    if (!threadLinkHit.empty() && !tab.threadUi.erase(threadLinkHit)) {
        Tab::ThreadUi tu;
        tu.thread = threadLinkHit;
        tab.threadUi[threadLinkHit] = std::move(tu);
    }
    // Kernel record boxes: a dim frame around a reply's run record, red when the prose contradicted it.
    for (const View::Record& r : view.records) {
        float y0 = 0.0f, y1 = 0.0f;
        if (!paraSpan(view.layout, r.firstPara, r.lastPara, y0, y1)) {
            continue;
        }
        const float inset = doc.paras[r.firstPara].indent;
        outlineRect(ImGui::GetWindowDrawList(), ImVec2(docOrigin.x + inset + 1.0f, docOrigin.y + y0 - 2.0f), ImVec2(docOrigin.x + wrap - 1.0f, docOrigin.y + y1 + 2.0f), r.contradiction ? Ink::inspected : Ink::secondary);
    }
    split.SetCurrentChannel(ImGui::GetWindowDrawList(), 2);
    drawMarkdownExtras(view.md, doc, view.layout, docOrigin, wrap, sr.hoveredPara, "transcript", &split);
    if (!view.cards.empty()) {
        drawCards(tab, principals, chat, session, docOrigin, wrap);
    }
    if (!view.views.empty()) {
        drawViewBlocks(tab, principals, chat, docOrigin, wrap, mono);
    }
    for (const View::ImageRow& row : view.imageRows) {
        float y0 = 0.0f, y1 = 0.0f;
        if (!paraSpan(view.layout, row.para, row.para, y0, y1)) {
            continue;
        }
        const ImVec2 a(docOrigin.x + 1.0f, docOrigin.y + y0);
        const ImVec2 z(docOrigin.x + wrap - 1.0f, docOrigin.y + y1);
        if (!ImGui::IsRectVisible(a, z)) {
            continue;
        }
        if (const int hit = messageImages().draw(chat, row.message, row.images, a, z); hit >= 0) {
            LinkRequest r;
            r.kind = LinkRequest::OpenFile;
            r.openKind = "image";
            r.message = row.message;
            r.index = hit;
            if (row.images.is_array() && static_cast<std::size_t>(hit) < row.images.size()) {
                r.path = row.images[static_cast<std::size_t>(hit)].value("name", std::string("image"));
            }
            symbolUi().ask(std::move(r));
        }
    }
    split.Merge(ImGui::GetWindowDrawList());
    const bool overSymbol = symbolUi().handleDoc(view.md, doc, sr, tab.sel.dragging);
    registerSymbolHits(view.md, doc, view.layout, mono, docOrigin);
    if (sr.clickedPara >= 0) {
        if (auto it = view.toggles.find(static_cast<std::size_t>(sr.clickedPara)); it != view.toggles.end()) {
            if (!tab.expandedThinking.erase(it->second.evaluation)) {
                tab.expandedThinking.insert(it->second.evaluation);
            }
            ++tab.expandedVersion;
        } else if (auto tt = view.threadToggles.find(static_cast<std::size_t>(sr.clickedPara));
                   tt != view.threadToggles.end() && threadLinkHit.empty()) {
            if (!tab.collapsedThreads.erase(tt->second)) {
                tab.collapsedThreads.insert(tt->second);
            }
            ++tab.expandedVersion;
        } else if (auto rt = view.recordToggles.find(static_cast<std::size_t>(sr.clickedPara)); rt != view.recordToggles.end()) {
            if (!tab.collapsedRecords.erase(rt->second)) {
                tab.collapsedRecords.insert(rt->second);
            }
            ++tab.expandedVersion;
        }
    }
    if (sr.hoveredPara >= 0 && !tab.sel.dragging) {
        const std::size_t hp = static_cast<std::size_t>(sr.hoveredPara);
        if (auto it = view.heads.find(hp); it != view.heads.end()) {
            std::string tip = "from " + it->second.from + "\nmessage " + it->second.message + "\nseq " + std::to_string(it->second.seq);
            if (!it->second.via.empty()) {
                tip += "\nevaluation " + it->second.via;
            }
            if (!it->second.inReplyTo.empty()) {
                tip += "\nin reply to " + it->second.inReplyTo;
            }
            if (!it->second.contradiction.empty()) {
                tip += "\n\nCONTRADICTION (reply/contradiction): the prose claimed what the kernel record denies\n" + it->second.contradiction;
            }
            if (detailShown(tip)) {
                ke::ui::tip("%s", tip.c_str());
            }
        } else if (auto ot = view.opTips.find(hp); ot != view.opTips.end()) {
            if (detailShown("this op's arguments and result")) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::CalcTextSize("0").x * 80);
                ImGui::TextUnformatted(ot->second.dump(2).c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        } else if (auto ct = view.costTips.find(hp); ct != view.costTips.end()) {
            ke::ui::tip("%s", ct->second.c_str());
        }
    }
    if (sr.hoveredPara >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !view.heads.empty()) {
        auto at = view.heads.upper_bound(static_cast<std::size_t>(sr.hoveredPara));
        tab.menuSeq = 0;
        tab.menuMessage.clear();
        if (at != view.heads.begin()) {
            --at;
            tab.menuMessage = at->second.message;
            tab.menuSeq = at->second.seq;
        }
    }
    if ((!overSymbol || ImGui::IsPopupOpen("transcript-menu")) &&
        ImGui::BeginPopupContextWindow("transcript-menu", ImGuiPopupFlags_MouseButtonRight)) {
        if (menuItem("COPY SELECTION", "Cmd-C", false, tab.sel.any())) {
            ImGui::SetClipboardText(selectedText(doc, tab.sel).c_str());
        }
        if (menuItem("COPY ALL", nullptr, false, !lines.empty())) {
            ImGui::SetClipboardText(chat::transcriptText(lines, 0, lines.size() - 1).c_str());
        }
        if (menuItem("CLEAR SELECTION", "Esc", false, tab.sel.any())) {
            tab.sel.clear();
        }
        if (menuItem("SHOW IN HISTORY", nullptr, false, tab.menuSeq > 0)) {
            LinkRequest r;
            r.kind = LinkRequest::History;
            r.seq = tab.menuSeq;
            symbolUi().ask(std::move(r));
        }
        drive::hit("menu:show-in-history", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (actions_ != nullptr && actions_->get("speech.say") != nullptr && anyModelHas(principals, "model/tts")) {
            // the selection when there is one, else the last thing the agent said; asked for, never on its own
            const std::string said = tab.sel.any() ? selectedText(doc, tab.sel) : lastReply(lines, session.principal().value_or(""));
            if (menuItem("SPEAK", nullptr, false, !said.empty())) {
                InputPane* line = &tab.line;
                chat.actNamed("speech.say", json{{"text", said}}, [line](const Response& r) {
                    std::vector<unsigned char> wav;
                    const std::string b64 = r.ok ? r.result.value("audio", std::string()) : std::string();
                    const bool ok = !b64.empty() && ke::util::base64Decode(b64, wav) &&
                                    platform::playWav(std::string(wav.begin(), wav.end()));
                    line->setVoiceNote(ok ? "" : "could not speak that");
                });
            }
            drive::hit("menu:speak", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        if (!view.rails.empty()) {
            ImGui::Separator();
            for (const View::Rail& r : view.rails) {
                std::string label = "OPEN THREAD " + chat::idPrefix(r.thread);
                if (r.headerPara < doc.paras.size()) {
                    // the header reads `[-] thread · asked by ...`; the item says THREAD once
                    const std::string header = doc.paras[r.headerPara].text();
                    const std::size_t rest = header.find("asked by");
                    label = "OPEN THREAD: " + (rest == std::string::npos ? header.substr(4) : header.substr(rest));
                }
                if (menuItem(label.c_str(), nullptr, tab.threadUi.count(r.thread) != 0)) {
                    if (!tab.threadUi.erase(r.thread)) {
                        Tab::ThreadUi tu;
                        tu.thread = r.thread;
                        tab.threadUi[r.thread] = std::move(tu);
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
    // A jump from HISTORY: open what hides the Message, then bring it to the top and mark it. The view
    // rebuilds on the next frame after an unfold, so this holds until the head appears or time runs out.
    if (!tab.jumpMessage.empty()) {
        const std::string want = tab.jumpMessage;
        if (const chat::MessageRow* row = chat.messages().get(want); row != nullptr && !row->thread.empty()) {
            const bool unfolded = tab.collapsedThreads.erase(row->thread) != 0;
            const bool summary = tab.expandedThinking.insert("summary:" + row->thread).second;
            if (unfolded || summary) {
                ++tab.expandedVersion;
            }
        }
        float y0 = 0.0f, y1 = 0.0f;
        bool landed = false;
        for (const auto& [para, head] : view.heads) {
            if (head.message == want && paraSpan(view.layout, para, para, y0, y1)) {
                const float top = docOrigin.y - (ImGui::GetWindowPos().y - ImGui::GetScrollY());
                ImGui::SetScrollY(std::max(0.0f, top + y0 - ImGui::GetTextLineHeightWithSpacing()));
                landed = true;
                break;
            }
        }
        if (landed) {
            tab.stick = false;
            tab.flashMessage = want;
            tab.flashUntil = steadySeconds() + 1.8;
            lastJump_ = want;
            tab.jumpMessage.clear();
        } else if (steadySeconds() - tab.jumpSince > 3.0) {
            tab.jumpMessage.clear(); // scrolled out of this chat's record: stay where the person is
        }
    }
    if (!tab.flashMessage.empty()) {
        if (steadySeconds() >= tab.flashUntil) {
            tab.flashMessage.clear();
        } else {
            for (auto it = view.heads.begin(); it != view.heads.end(); ++it) {
                if (it->second.message != tab.flashMessage) {
                    continue;
                }
                const auto next = std::next(it);
                const std::size_t last = next != view.heads.end() ? next->first - 1 : doc.paras.size() - 1;
                float y0 = 0.0f, y1 = 0.0f;
                if (paraSpan(view.layout, it->first, last, y0, y1)) {
                    ImGui::GetWindowDrawList()->AddRect(ImVec2(docOrigin.x, docOrigin.y + y0), ImVec2(docOrigin.x + wrap, docOrigin.y + y1),
                                                        Ink::nav, 0.0f, 1.5f);
                }
                break;
            }
        }
    }
    if (tab.stick && view.shape != tab.lastLines) {
        ImGui::SetScrollHereY(1.0f);
    }
    tab.lastLines = view.shape;
    popMono();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (readOnly) {
        return;
    }

    if (running) {
        // Live progress from the log: the running Evaluation's start time, loop/iteration, and the
        // size of the previous call. The app ticks one frame a second while a loop runs.
        const double nowEpoch = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        const model::EvaluationInfo* currentEval = evals.running(tab.worker);
        const std::string modelName = currentEval ? principals.nameOf(currentEval->model) : std::string();
        std::string line = model::progressLine(evals, tab.worker, principals.nameOf(tab.worker), workerInfo->loopIteration(), nowEpoch,
                                              workerInfo->loopTurnLimit(), workerInfo->loopQueued(), modelName);
        hazardSpinner(ImGui::GetFontSize() * 5.0f, ImGui::GetTime());
        ImGui::SameLine();
        // STOP stays put beside the spinner: after the progress line it moved with every count,
        // out from under the pointer.
        const bool canStop = tab.cancelling.empty() && session.attached();
        bool stopClicked = false;
        if (bare) {
            ImGui::BeginDisabled(!canStop);
            stopClicked = ImGui::Button("STOP", ImVec2(std::max(72.0f, ImGui::CalcTextSize("STOP").x + 16.0f),
                                                     std::max(44.0f, ImGui::GetFrameHeight())));
            drive::hit("button:stop", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::EndDisabled();
        } else {
            stopClicked = bracketButton("stop", canStop);
        }
        if (stopClicked) {
            cancel(tab, evals, chat);
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("stop this Evaluation: act cancel (Esc Esc, Cmd-.)");
        }
        ImGui::SameLine();
        if (!tab.cancelling.empty()) {
            text(Ink::inspected, "cancelling\u2026  " + line);
        } else {
            text(Ink::primary, line);
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("RETURN queues a line for after this call\nSHIFT-RETURN or [ STEER ] interrupts it with a line\nESC ESC or [ STOP ] stops it");
        }
    } else if (!stalled.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
        ImGui::PushStyleColor(ImGuiCol_Text, Ink::inspected);
        ImGui::TextUnformatted(("STALLED " + stalled).c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("loop/stalled of %s\nright-click the FIELDS tab value to copy", principals.nameOf(tab.principal).c_str());
        }
        const std::vector<std::string> rts = info ? info->routes() : std::vector<std::string>{};
        if (bracketButton("retry", !tab.lastSent.empty() && !rts.empty() && session.attached())) {
            tab.sendError.clear();
            std::string* sendError = &tab.sendError;
            const std::string self = session.principal().value_or("");
            std::vector<std::string> to = model::recipientsFor(principals, tab.principal, rts.front(), self);
            if (to.size() > 1) {
                to = {to[static_cast<std::size_t>(tab.recipientIndex) % to.size()]};
            }
            chat.submitTo(to, rts.front(), tab.lastSent, [sendError](const std::string& code, const std::string& message) {
                if (!code.empty()) {
                    *sendError = "[ SEND: " + upper(code) + " ] " + message;
                }
            });
        }
        ImGui::SameLine();
        text(Ink::secondary, tab.lastSent.empty() ? "re-sends the last line typed here" : "re-sends: " + tab.lastSent.substr(0, 60));
    }
    if (!tab.sendError.empty()) {
        text(Ink::inspected, tab.sendError);
    }
    if (!tab.steerError.empty()) {
        text(Ink::inspected, tab.steerError);
        ImGui::SameLine();
        if (bracketButtonNamed("queue instead", "queue-instead")) {
            tab.queueInstead = true;
        }
        ImGui::SameLine();
        if (bracketButton("x")) {
            tab.steerError.clear();
        }
    }
    drawQueue(tab, queue, principals, chat, session.principal().value_or(""));
    if (running) {
        text(Ink::secondary, "RETURN queues \u00b7 SHIFT-RETURN steers");
    }
    if (!tab.cancelResult.empty()) {
        text(Ink::inspected, tab.cancelResult);
        ImGui::SameLine();
        if (bracketButton("x")) {
            tab.cancelResult.clear();
        }
    }
    if (attachLine) {
        AttachStatus& as = attach_[tab.principal];
        text(as.err ? Ink::fatal : (as.done ? Ink::secondary : Ink::inspected), "ATTACH " + as.stage);
        drive::hit("attach-status", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (as.done) {
            ImGui::SameLine();
            if (bracketButtonNamed("x", "attach-dismiss")) {
                as = AttachStatus{};
            }
        }
    }
    if (!tab.folderPrompt.empty()) {
        const std::string folder = tab.folderPrompt;
        std::string base = std::filesystem::path(folder).filename().string();
        if (base.empty()) {
            base = std::filesystem::path(folder).parent_path().filename().string();
        }
        text(Ink::inspected, "DROPPED FOLDER " + base + "/");
        ImGui::SameLine();
        std::vector<std::string> rts = info ? info->routes() : std::vector<std::string>{};
        if (rts.empty() && tab.principal == self && chat.routeState() == chat::RouteState::Found) {
            rts.push_back(chat.route());
        }
        AttachStatus* status = &attach_[tab.principal];
        auto progress = [status](const std::string& stage, bool err, bool done) {
            status->stage = stage;
            status->err = err;
            status->done = done;
        };
        if (bracketButtonNamed("mount", "drop-mount", session.attached())) {
            *status = AttachStatus{"workspace.mount " + base + "\u2026", false, false};
            chat.actNamed("workspace.mount",
                          json{{"principal", tab.principal}, {"path", folder}, {"name", base}, {"shell", true}},
                          [status, base](const Response& r) {
                              if (r.ok) {
                                  *status = AttachStatus{"mounted " + base + " (seq " + std::to_string(r.result.value("seq", r.seq)) + ")",
                                                         false, true};
                              } else {
                                  *status = AttachStatus{"mount failed: " + (r.err ? r.err->code + " " + r.err->message : std::string("?")),
                                                         true, true};
                              }
                          });
            tab.folderPrompt.clear();
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("act workspace.mount {principal %s path %s name %s shell true}", tab.principal.c_str(), folder.c_str(),
                              base.c_str());
        }
        ImGui::SameLine();
        if (bracketButtonNamed("attach listing", "drop-listing", session.attached() && !rts.empty())) {
            bool truncated = false;
            const json tree = chat::folderTree(folder, 40, truncated);
            *status = AttachStatus{"attaching the listing of " + base + "/\u2026", false, false};
            chat.attachFields(tab.principal, rts.front(), chat::folderViewFields(folder, tree, truncated),
                              "attached: listing of " + base + "/ (two levels)", progress);
            tab.folderPrompt.clear();
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("a tree view of the top two levels, as an Object tagged on a Message");
        }
        ImGui::SameLine();
        if (bracketButtonNamed("x", "drop-cancel")) {
            tab.folderPrompt.clear();
        }
    }

    // The line: sends to this Principal on its first inbox route. The fold (seq 0 onwards) can lag
    // the chat's own route read by a few ticks after attach, so our own terminal also accepts the
    // route the chat discovered; anyone else waits for the fold.
    std::vector<std::string> routes = info ? info->routes() : std::vector<std::string>{};
    if (routes.empty() && tab.principal == self && chat.routeState() == chat::RouteState::Found) {
        routes.push_back(chat.route());
    }
    LineTarget t;
    t.history = &tab.history;
    if (routes.empty()) {
        text(Ink::inspected, "NO ROUTE");
        ImGui::SameLine();
        text(Ink::secondary, info && info->drivenBy == "process"
                               ? "// this Principal has no inbox yet: ask root to create a route to it (GRAPH: drag a port onto it)"
                               : "// this Principal has no inbox route; nothing can be sent to it yet");
        t.label = "NO ROUTE";
        t.labelColor = Ink::inspected;
        t.enabled = false;
    }
    // Whom the line addresses: another Principal's tab sends to that Principal; our own tab is a
    // conversation with the Route's other members (route/members minus self, else the Route's
    // participants, else our creator), the first by default, a click on the label cycles.
    // A chat with an agent uses the Routes between the two of them. Its Routes to other agents are
    // not a choice here, even when a person can see them.
    {
        std::vector<std::string> direct;
        for (const std::string& r : routes) {
            const model::RouteInfo* ri = principals.route(r);
            if (ri == nullptr) {
                continue;
            }
            const bool me = std::find(ri->members.begin(), ri->members.end(), self) != ri->members.end();
            const bool others = std::any_of(ri->members.begin(), ri->members.end(), [&](const std::string& m) {
                return m != self && m != tab.principal;
            });
            if (me && !others) {
                direct.push_back(r);
            }
        }
        if (!direct.empty()) {
            routes = std::move(direct);
        }
    }
    if (tab.routeIndex < 0 || static_cast<std::size_t>(tab.routeIndex) >= routes.size()) {
        tab.routeIndex = 0;
    }
    const std::string route = routes.empty() ? std::string() : routes[static_cast<std::size_t>(tab.routeIndex)];
    std::vector<std::string> recipients = routes.empty() ? std::vector<std::string>{}
                                                         : model::recipientsFor(principals, tab.principal, route, self);
    if (!routes.empty()) {
        if (recipients.empty()) {
            t.label = upper("nobody on " + principals.nameOf(route));
            t.labelColor = Ink::inspected;
            t.labelTooltip = "route " + route + "\nnobody else is known on this route yet; the line would reach only you";
            t.enabled = false;
        } else {
            if (tab.recipientIndex < 0 || static_cast<std::size_t>(tab.recipientIndex) >= recipients.size()) {
                tab.recipientIndex = 0;
            }
            const std::string to = recipients[static_cast<std::size_t>(tab.recipientIndex)];
            const bool manyPeople = recipients.size() > 1;
            recipients = {to};
            // One agent on one Route needs no words: the tab says who this is. Only a real choice shows.
            t.labelColor = Ink::secondary;
            if (routes.size() > 1) {
                t.label = upper("via " + principals.nameOf(route));
                t.labelTooltip = "this agent has " + std::to_string(routes.size()) + " routes\nclick for the next one";
                int* idx = &tab.routeIndex;
                t.onLabelClick = [idx] { ++*idx; };
            } else if (manyPeople) {
                t.label = upper("to " + principals.nameOf(to));
                t.labelTooltip = "several are on this route\nclick to address the next one";
                int* idx = &tab.recipientIndex;
                t.onLabelClick = [idx] { ++*idx; };
            }
            // a world restarting is no reason to lose the keyboard: typing goes on, and Return waits
            t.connected = session.attached();
            if (!t.connected) {
                t.label = upper("not connected: this line waits");
                t.labelColor = Ink::inspected;
                t.onLabelClick = nullptr;
            }
        }
    }
    std::string* sendError = &tab.sendError;
    std::string* lastSent = &tab.lastSent;
    t.tags = &tab.pendingTags;
    std::vector<std::string>* pending = &tab.pendingTags;
    InputPane* line_ = &tab.line;
    t.submit = [&chat, recipients, route, sendError, lastSent, pending, line_](const std::string& line) {
        sendError->clear();
        *lastSent = line;
        std::vector<std::string> tags;
        tags.swap(*pending);
        const std::vector<std::string> sentTags = tags;
        chat.submitTo(recipients, route, line, [sendError, line_, line, pending, sentTags](const std::string& code, const std::string& message) {
            if (!code.empty()) {
                // never reported as sent: the text and its tags go back to the input
                *sendError = "[ SEND: " + upper(code) + " ] " + message;
                line_->putBack(line);
                if (pending->empty()) {
                    *pending = sentTags;
                }
            }
        }, chat::Clock::now(), std::move(tags));
    };
    const bool working = principals.get(tab.worker) && principals.get(tab.worker)->loopIteration() > 0;
    t.working = working;
    std::vector<chat::PendingLine>* queued = &tab.queuePending;
    if (working) {
        auto plain = t.submit;
        const chat::MessageCache* messages = &chat.messages();
        t.submit = [plain, queued, messages](const std::string& line) {
            queued->push_back(chat::PendingLine{line, messages->maxReadySeq()}); // shown at once, until listed or answered
            plain(line);
        };
    }
    std::string* steerError = &tab.steerError;
    std::string* steerText = &tab.steerText;
    t.steer = [&chat, recipients, route, steerError, steerText, line_](const std::string& line) {
        steerError->clear();
        chat.submitTo(recipients, route, line, [steerError, steerText, line_, line](const std::string& code, const std::string& message) {
            if (!code.empty()) {
                *steerError = "[ STEER: " + upper(code) + " ] " + message;
                *steerText = line;
                line_->putBack(line); // not interrupted, and not lost
            }
        }, chat::Clock::now(), {}, true);
    };
    tab.talk = nullptr;
    if (actions_ != nullptr && actions_->get("speech.transcribe") != nullptr && anyModelHas(principals, "model/stt")) {
        t.talk = [&chat, line_](const std::string& wav) {
            line_->setVoiceNote("transcribing");
            chat.actNamed("speech.transcribe", json{{"audio", ke::util::base64Encode(wav)}}, [line_](const Response& r) {
                if (!r.ok) {
                    line_->setVoiceNote("not transcribed: " + (r.err ? r.err->code : std::string("no answer")));
                    return;
                }
                line_->setVoiceNote("");
                line_->addTranscript(r.result.value("text", std::string()));
            });
        };
        tab.talk = t.talk;
        if (bare) {
            t.talk = nullptr; // TOUCH: the bar below the line does the holding; no bracket beside it
        }
    }
    tab.speak = nullptr;
    if (actions_ != nullptr && actions_->get("speech.say") != nullptr && anyModelHas(principals, "model/tts")) {
        const std::string said = lastReply(lines, session.principal().value_or(""));
        if (!said.empty()) {
            InputPane* line = &tab.line;
            tab.speak = [&chat, line, said] {
                line->setVoiceNote("reading");
                chat.actNamed("speech.say", json{{"text", said}}, [line](const Response& r) {
                    std::vector<unsigned char> wav;
                    const std::string b64 = r.ok ? r.result.value("audio", std::string()) : std::string();
                    const bool ok = !b64.empty() && ke::util::base64Decode(b64, wav) && platform::playWav(std::string(wav.begin(), wav.end()));
                    line->setVoiceNote(ok ? "" : "could not speak that");
                });
            };
        }
    }
    if (tab.queueInstead) {
        tab.queueInstead = false;
        const std::string line = tab.steerText;
        tab.steerError.clear();
        tab.steerText.clear();
        tab.line.clear();
        if (t.submit && !line.empty()) {
            t.submit(line);
        }
    }
    if (bare) {
        focusRequested_ = false; // TOUCH: the keyboard waits for a tap on the line
    }
    if (focusRequested_) {
        tab.line.requestFocus();
        focusRequested_ = false;
    }
    // A pasted image goes with the next line; text on the clipboard stays the field's own business.
    if (tab.line.active() && ImGui::IsKeyPressed(ImGuiKey_V, false) && (ImGui::GetIO().KeySuper || ImGui::GetIO().KeyCtrl)) {
        if (std::vector<unsigned char> png = platform::clipboardImage(); !png.empty()) {
            addImageBytes(tab, chat, std::move(png), "pasted.png");
        }
    }
    if (hasReasoning && !bare) {
        if (!view.rails.empty()) {
            tab.lastThread = view.rails.back().thread;
        }
        drawReasoning(tab, info, principals, chat);
        // At phone width the picker takes a row of its own, else MODEL sits past the edge and a
        // tap on it lands nowhere (seen on the iPhone Duo, 2026-11-11).
        if (ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() >= ImGui::CalcTextSize("0").x * 72.0f) {
            ImGui::SameLine();
        }
        drawModelPicker(tab, info, principals, chat);
        drawContextBar(tab, evals);
    }
    drawPendingImages(tab);
    if (!tab.images.empty()) {
        t.allowEmpty = true;
        auto plain = t.submit;
        std::vector<std::shared_ptr<Tab::PendingImage>>* pendingImages = &tab.images;
        t.submit = [&chat, plain, pendingImages](const std::string& line) {
            json ready = json::array();
            auto& list = *pendingImages;
            for (auto it = list.begin(); it != list.end();) {
                if ((*it)->attachment.is_object() && (*it)->attachment.contains("sha256")) {
                    ready.push_back((*it)->attachment);
                    it = list.erase(it);
                } else {
                    ++it;
                }
            }
            chat.setNextImages(std::move(ready));
            plain(line.empty() ? std::string("(image)") : line);
        };
    }
    t.attach = [] { symbolUi().attachImageWanted(); };
    tab.line.drawLine(t, anim, mono);
}

void TerminalPane::drawPendingImages(Tab& tab) {
    for (std::size_t i = 0; i < tab.images.size();) {
        const Tab::PendingImage& p = *tab.images[i];
        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        const bool ready = p.attachment.is_object() && p.attachment.contains("sha256");
        const std::string word = p.name + (!p.error.empty() ? " (" + p.error + ")" : ready ? "" : " \xe2\x80\xa6") + " x";
        text(!p.error.empty() ? Ink::fatal : ready ? Ink::primary : Ink::secondary, word);
        const bool drop = ImGui::IsItemClicked();
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ke::ui::tip("click: leave it out");
        }
        drive::hit("pending-image:" + p.name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::PopID();
        if (drop) {
            tab.images.erase(tab.images.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void TerminalPane::drawReasoning(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals,
                                 chat::Chat& chat) {
    const auto word = [](const model::FieldState* f) {
        return f && f->value.is_string() ? f->value.get<std::string>() : std::string();
    };
    const std::string loop = info ? word(info->field("loop/reasoning")) : std::string();
    std::string model;
    if (info) {
        const std::vector<std::string> models = info->models();
        if (const model::PrincipalInfo* e = models.empty() ? nullptr : principals.get(models.front())) {
            model = word(e->field("model/reasoning"));
        }
    }
    ReasoningRow row;
    row.settable = info && info->drivenBy != "person";
    row.mode = chat::reasoningShown(loop, model);
    row.source = loop == "on" || loop == "off" ? "set here (loop/reasoning)"
                                               : "not set here: model/reasoning is " + (model.empty() ? std::string("auto") : model);
    if (const std::optional<std::string> chosen = drawReasoningRow(row, showThinking)) {
        setField(tab, chat, "loop/reasoning", json("keyword"), "both", json(*chosen), tab.reasoningEdit);
    }
    if (!tab.reasoningEdit.result.empty() && tab.reasoningEdit.resultErr) {
        ImGui::SameLine();
        text(Ink::inspected, tab.reasoningEdit.result);
    }
}

void TerminalPane::drawContextBar(Tab& tab, const model::Evaluations& evals) {
    const model::EvaluationInfo* last = evals.latestWithUsage(tab.principal);
    if (last == nullptr || last->ctxIn <= 0) {
        return;
    }
    const auto chosen = [&tab](const char* key) {
        return tab.chosen.is_object() ? tab.chosen.value(key, 0L) : 0L;
    };
    const chat::ContextGauge g = chat::contextGauge(static_cast<long>(last->ctxIn), chosen("compress-at"), chosen("window"));
    const auto thousands = [](long n) { return n >= 1000 ? std::to_string(n / 1000) + "k" : std::to_string(n); };
    const std::string figure = g.known ? std::to_string(g.used * 100 / g.window) + "% of " + thousands(g.window) : thousands(g.used);
    const float cw = ImGui::CalcTextSize("0").x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const char* label = model::newWakeContext(*last) ? "CTX · new wake" : "CTX";
    const float words = ImGui::CalcTextSize(label).x + ImGui::CalcTextSize(figure.c_str()).x + gap * 2.0f;
    // One row, always: the input line's height is reserved below it. The bar gives way, then the whole gauge.
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float barW = g.known ? std::min(cw * 14.0f, avail - words) : 0.0f;
    if (avail < words || (g.known && barW < cw * 4.0f)) {
        // The row is full (a plan adds "Using ChatGPT plan" and "manage usage"): the gauge stays out,
        // and the line is ended here, or the input line lands to the right of the row, off the pane.
        ImGui::NewLine();
        return;
    }
    ImGui::SameLine(ImGui::GetCursorPosX() + avail - words - barW);
    const ImU32 ink = g.near ? Ink::inspected : Ink::secondary;
    text(ink, label);
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("The last prompt%s. A new wake leaves prior turns on the log, not automatically inline; this is not compression.",
                    model::newWakeContext(*last) ? " started a new wake" : " belongs to the current wake");
    }
    if (g.known) {
        ImGui::SameLine();
        const float h = ImGui::GetTextLineHeight();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ctx-bar", ImVec2(barW, h));
        drive::hit("gauge:ctx", at, ImVec2(at.x + barW, at.y + h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float top = at.y + h * 0.2f, bottom = at.y + h * 0.8f;
        dl->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + barW * g.fill, bottom), g.near ? Ink::inspected : Ink::nav);
        dl->AddRect(ImVec2(at.x, top), ImVec2(at.x + barW, bottom), Ink::secondary);
        const float markX = at.x + barW * g.mark;
        dl->AddLine(ImVec2(markX, top - 2.0f), ImVec2(markX, bottom + 2.0f), Ink::strong);
    }
    ImGui::SameLine();
    text(ink, figure);
    if (ImGui::IsItemHovered()) {
        if (g.known) {
            ke::ui::tip("%ld of the Model's %ld token window in the last prompt\nthe mark is where the conversation compresses first, at %ld",
                              g.used, g.window, g.compressAt);
        } else {
            ke::ui::tip("%ld tokens in the last prompt\nthis Model does not report its window: set model/context-tokens on it", g.used);
        }
    }
}

void TerminalPane::drawQueue(Tab& tab, const std::vector<chat::QueueRow>& queue, const model::Principals& principals,
                             chat::Chat& chat, const std::string& self) {
    if (queue.empty()) {
        return;
    }
    const auto strip = chat::queueStripLayout(queue.size(), ImGui::GetFrameHeight(), ImGui::GetStyle().ItemSpacing.y, false);
    text(Ink::inspected, strip.heading);
    const std::string owner = tab.worker;
    const float ch = ImGui::CalcTextSize("0").x;
    const float rowHeight = strip.targetHeight;
    const float listHeight = strip.listHeight;
    drive::hit("queue:heading", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::BeginChild("queue-rows", ImVec2(0, listHeight));
    touchDragScroll();
    const bool compact = ImGui::GetContentRegionAvail().x < ch * 72.0f;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const chat::QueueRow& r = queue[i];
        const std::string id8 = r.pending ? "pending" + std::to_string(i) : chat::idPrefix(r.message);
        ImGui::PushID(static_cast<int>(i));
        if (compact) {
            const bool own = !r.pending && !self.empty() && r.from == self;
            auto button = [&](const char* label, const std::string& target) {
                const bool clicked = ImGui::Button(label, ImVec2(44.0f, rowHeight));
                drive::hit("button:" + target + ":" + id8, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                ImGui::SameLine();
                return clicked;
            };
            if (button("x", r.pending ? "dismiss" : "dequeue")) {
                if (r.pending) {
                    if (r.pendingIndex < tab.queuePending.size()) {
                        tab.queuePending.erase(tab.queuePending.begin() + static_cast<std::ptrdiff_t>(r.pendingIndex));
                    }
                } else {
                    chat.actNamed("loop.dequeue", json{{"principal", owner}, {"message", r.message}}, [](const Response&) {});
                }
            }
            if (own && button("steer", "steer")) {
                std::string* error = &tab.steerError;
                chat.promoteQueued(owner, r.message, [error](const Response& resp) {
                    if (!resp.ok) {
                        *error = "[ STEER: " + (resp.err ? upper(resp.err->code) : std::string("?")) + " ]";
                    }
                });
            }
            const std::string sender = r.pending ? "pending" : principals.nameOf(r.from);
            const std::string tag = chat::firstLine(sender, 8);
            ImGui::AlignTextToFramePadding();
            text(Ink::secondary, tag);
            ImGui::SameLine();
            const float room = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            const std::string line = chat::queueStripText(r.text.empty() ? "<reading>" : r.text, room,
                [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x; });
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("queue-text", ImVec2(room, rowHeight))) {
                ImGui::OpenPopup("queue-detail");
            }
            drive::hit("queue:text:" + id8, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::GetWindowDrawList()->PushClipRect(at, ImVec2(at.x + room, at.y + rowHeight), true);
            ImGui::GetWindowDrawList()->AddText(at, Ink::primary, line.c_str());
            ImGui::GetWindowDrawList()->PopClipRect();
            if (ImGui::BeginPopup("queue-detail")) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(320.0f, ImGui::GetIO().DisplaySize.x - 48.0f));
                ImGui::TextUnformatted(sender.c_str());
                ImGui::Separator();
                ImGui::TextUnformatted(r.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndPopup();
            }
            ImGui::PopID();
            continue;
        }
        if (r.pending) {
            if (bracketButtonNamed("x", "dismiss:" + id8)) {
                if (r.pendingIndex < tab.queuePending.size()) {
                    tab.queuePending.erase(tab.queuePending.begin() + static_cast<std::ptrdiff_t>(r.pendingIndex));
                }
            }
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("dismiss this local pending row");
            }
            ImGui::SameLine();
            dimText("on its way");
            ImGui::SameLine();
        } else {
            const bool canUp = i > 0, canDown = i + 1 < queue.size();
            auto move = [&](long position) {
                chat.actNamed("loop.reorder", json{{"principal", owner}, {"message", r.message}, {"position", position}},
                              [](const Response&) {});
            };
            const bool own = !self.empty() && r.from == self;
            if (own && bracketButtonNamed("^", "up:" + id8, canUp)) {
                move(static_cast<long>(i) - 1);
            }
            ImGui::SameLine();
            if (own && bracketButtonNamed("v", "down:" + id8, canDown)) {
                move(static_cast<long>(i) + 1);
            }
            ImGui::SameLine();
            if (bracketButtonNamed("x", "dequeue:" + id8)) {
                chat.actNamed("loop.dequeue", json{{"principal", owner}, {"message", r.message}}, [](const Response&) {});
            }
            ImGui::SameLine();
            if (own && bracketButtonNamed("steer", "steer:" + id8)) {
                std::string* steerError = &tab.steerError;
                chat.promoteQueued(owner, r.message, [steerError](const Response& resp) {
                    if (!resp.ok) {
                        *steerError = "[ STEER: " + (resp.err ? upper(resp.err->code) : std::string("?")) + " ]";
                    }
                });
            }
            ImGui::SameLine();
        }
        // Who and when keep the right edge: the message gives way, however long it is.
        const std::string who = r.from.empty() ? std::string() : principals.nameOf(r.from) + "  " + clockPart(r.at);
        const float whoW = who.empty() ? 0.0f : ImGui::CalcTextSize(who.c_str()).x + ch;
        const float room = std::max(0.0f, ImGui::GetContentRegionAvail().x - whoW);
        std::string line = chat::firstLine(r.text.empty() ? "<reading>" : r.text);
        if (ImGui::CalcTextSize(line.c_str()).x > room) {
            const std::string ellipsis = "\xe2\x80\xa6";
            while (!line.empty() && ImGui::CalcTextSize((line + ellipsis).c_str()).x > room) {
                line.pop_back();
                while (!line.empty() && (static_cast<unsigned char>(line.back()) & 0xC0) == 0x80) {
                    line.pop_back(); // never half a character
                }
            }
            line += ellipsis;
        }
        text(r.pending ? Ink::secondary : Ink::primary, line);
        if (!who.empty()) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - whoW + ch);
            text(Ink::secondary, who);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace ke::ui
