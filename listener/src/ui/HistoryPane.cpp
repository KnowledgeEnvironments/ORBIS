#include "ui/HistoryPane.hpp"

#include "drive/Commands.hpp"
#include "ui/SymbolUi.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ke::ui {

namespace {

std::string shortId(const std::string& id) {
    return id.size() > 8 ? id.substr(0, 8) : id;
}

// "2026-10-29T06:00:01.000Z" -> "06:00:01.000"
std::string clockPart(const std::string& at) {
    return at.size() >= 23 ? at.substr(11, 12) : at;
}

std::string oneLine(const json& body, std::size_t max = 160) {
    std::string s = body.dump();
    if (s.size() > max) {
        s.resize(max - 3);
        s += "...";
    }
    return s;
}

ImU32 ink(bool inverse, ImU32 color) {
    return inverse ? Ink::onSelection : color;
}

bool copyChord() {
    return ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_C) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C);
}

} // namespace

std::string HistoryPane::nameOf(const std::string& id) const {
    return names_ ? names_->nameOf(id) : shortId(id);
}

// One row as tab-separated seq, at, kind, stream, actor, body (a note: `-` and its text).
std::string HistoryPane::rowText(const Item& item, const EventStore& store, const chat::Chat& chat) const {
    if (item.note) {
        const chat::Note& n = chat.notes()[item.index];
        return "-\t\t" + (n.chip.empty() ? std::string() : n.chip) + "\t\t\t" + n.text;
    }
    const Event& e = store.rows()[item.index];
    return std::to_string(e.seq) + "\t" + e.at + "\t" + e.kind + "\t" + e.stream + "\t" + e.actor + "\t" + e.body.dump();
}

std::string HistoryPane::selectionText(const EventStore& store, const chat::Chat& chat, bool filtered) const {
    if (selected_ < 0 || selAnchor_ < 0) {
        return "";
    }
    const std::size_t shown = filtered ? view_.size() : items_.size();
    const int lo = std::min(selected_, selAnchor_);
    const int hi = std::min(std::max(selected_, selAnchor_), static_cast<int>(shown) - 1);
    std::string out;
    for (int i = lo; i <= hi; ++i) {
        const Item& item = items_[filtered ? view_[static_cast<std::size_t>(i)] : static_cast<std::size_t>(i)];
        out += rowText(item, store, chat) + "\n";
    }
    return out;
}

void HistoryPane::merge(const EventStore& store, const chat::Chat& chat, chat::Clock::time_point now) {
    const auto& rows = store.rows();
    const auto& notes = chat.notes();
    const bool grewAbove = store.prepends() != prependsSeen_;
    if (rows.size() < rowsMerged_ || notes.size() < notesMerged_ || grewAbove) {
        if (grewAbove) {
            shownBeforePrepend_ = lastShown_;
        }
        prependsSeen_ = store.prepends();
        items_.clear();
        view_.clear();
        viewScanned_ = 0;
        sentSeen_.clear();
        rowsMerged_ = notesMerged_ = 0;
        selected_ = -1;
    }
    // Both lists are append-only, so the merge continues where it stopped. A note that is not
    // visible yet (a boot line still due) holds the merge until it shows.
    while (true) {
        if (notesMerged_ < notes.size() && notes[notesMerged_].afterRow <= rowsMerged_) {
            if (notes[notesMerged_].visibleAt > now) {
                break;
            }
            items_.push_back(Item{true, notesMerged_++});
        } else if (rowsMerged_ < rows.size()) {
            if (const Event& row = rows[rowsMerged_]; row.kind == "sent") {
                if (auto id = chat::MessageCache::messageIdOf(row)) {
                    sentSeen_.insert(*id);
                }
            }
            items_.push_back(Item{false, rowsMerged_++});
        } else {
            break;
        }
    }
}

void HistoryPane::setFocus(const std::string& principal, const std::string& self) {
    if (principal != focus_) {
        focus_ = principal;
        view_.clear();
        viewScanned_ = 0;
    }
    self_ = self;
}

bool HistoryPane::matches(const Item& item, const EventStore& store) const {
    if (item.note) {
        return !onlyFocus_ || focus_ == self_;
    }
    const Event& e = store.rows()[item.index];
    if (!showReads_ && (e.kind == "field-read" || e.kind == "resolved")) {
        return false;
    }
    if (!showRoutine_ && isRoutine(e)) {
        return false;
    }
    if (!onlyFocus_ || focus_.empty()) {
        return true;
    }
    if (e.stream == focus_ || e.actor == focus_) {
        return true;
    }
    for (const std::string& s : e.streams) {
        if (s == focus_) {
            return true;
        }
    }
    if (e.body.is_object()) {
        for (const char* key : {"recipient", "object", "grantee"}) {
            if (auto it = e.body.find(key); it != e.body.end() && it->is_string() && it->get<std::string>() == focus_) {
                return true;
            }
        }
    }
    return false;
}

void HistoryPane::draw(const EventStore& store, const chat::Chat& chat, const Anim& anim,
                       ImFont* mono) {
    if (!collapsed) {
        if (beginPane("HISTORY")) {
            drawBody(store, chat, anim, mono);
        }
        endPane();
        return;
    }
    if (!beginPane("HISTORY", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        endPane();
        return;
    }
    ImGui::PushFont(mono, 0.0f);
    ImGui::SameLine();
    const auto& rows = store.rows();
    for (std::size_t i = rows.size(); i-- > 0;) {
        if (rows[i].kind != "field-read" && rows[i].kind != "resolved") { // the newest thing that happened
            text(Ink::secondary, std::to_string(rows[i].seq));
            ImGui::SameLine();
            kindChip(rows[i].kind);
            ImGui::SameLine();
            text(Ink::secondary, nameOf(rows[i].stream));
            break;
        }
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("[ EXPAND ]").x));
    if (bracketButtonNamed("expand", "history-expand")) {
        collapsed = false;
    }
    ImGui::PopFont();
    endPane();
}

void HistoryPane::drawBody(const EventStore& store, const chat::Chat& chat, const Anim& anim,
                           ImFont* mono) {
    const auto& rows = store.rows();
    merge(store, chat, chat::Clock::now());

    ImGui::PushFont(mono, 0.0f);
    dimText("%zu rows // head %lld // filtered %zu", rows.size(), static_cast<long long>(store.maxSeq()),
            store.holes().size());
    if (ImGui::IsItemHovered() && !store.holes().empty()) {
        ImGui::SetTooltip("filtered: runs of seqs the world skipped because this Principal may not read them; expected, not a loss");
    }
    if (!store.gaps().empty()) {
        ImGui::SameLine();
        text(Ink::fatal, "// " + std::to_string(store.gaps().size()) + " gap(s): the world's head fell below our cursor");
    }
    // The controls sit at the right of the status line, and wrap rather than run off a narrow pane.
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* const kControls[] = {"[ COLLAPSE ]", "[ SEARCH ]", "[ THIS PRINCIPAL ]", "[ READS ]", "[ ROUTINE ]", "[ FOLLOW ]"};
    const auto wide = [&](const char* word) { return ImGui::CalcTextSize(word).x; };
    float row = 5.0f * style.ItemSpacing.x;
    for (const char* word : kControls) {
        row += wide(word);
    }
    const float statusEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + style.ItemSpacing.x;
    const float rowStart = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - row;
    if (rowStart >= statusEnd) {
        ImGui::SameLine(rowStart);
    }
    const auto nextControl = [&](const char* word) {
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < wide(word)) {
            ImGui::NewLine();
        }
    };
    if (bracketButtonNamed("collapse", "history-collapse")) {
        collapsed = true;
    }
    nextControl("[ SEARCH ]");
    if (bracketButtonNamed("search", "search", true, searchOpen_)) {
        if (searchOpen_) {
            closeSearch();
        } else {
            openSearch();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("search Fields, names and Messages (Cmd-F); the world answers what you may read");
    }
    nextControl("[ THIS PRINCIPAL ]");
    if (bracketButton("this principal", !focus_.empty(), onlyFocus_ && !focus_.empty())) {
        onlyFocus_ = !onlyFocus_;
        selected_ = selAnchor_ = -1; // indices change meaning with the filter
    }
    if (ImGui::IsItemHovered() && !focus_.empty()) {
        ImGui::SetTooltip("only rows touching %s\n%s", nameOf(focus_).c_str(), focus_.c_str());
    }
    nextControl("[ READS ]");
    if (bracketButtonNamed("reads", "history-reads", true, showReads_)) {
        showReads_ = !showReads_;
        selected_ = selAnchor_ = -1;
        view_.clear();
        viewScanned_ = 0;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("show field-read and resolved rows: what clients and agents do just by looking");
    }
    nextControl("[ ROUTINE ]");
    if (bracketButtonNamed("routine", "history-routine", true, showRoutine_)) {
        showRoutine_ = !showRoutine_;
        selected_ = selAnchor_ = -1;
        view_.clear();
        viewScanned_ = 0;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("show each chat turn's bookkeeping: streaming, costs and iterations\nMessages, ops and failures always show");
    }
    nextControl("[ FOLLOW ]");
    if (bracketButton("follow", true, stickToBottom_)) {
        stickToBottom_ = !stickToBottom_;
    }
    if (searchOpen_) {
        drawSearch();
    }
    const bool filtered = true; // reads are hidden unless asked for, so the view is always a filter
    if (filtered) {
        if (viewScanned_ > items_.size()) {
            view_.clear();
            viewScanned_ = 0;
        }
        for (; viewScanned_ < items_.size(); ++viewScanned_) {
            if (matches(items_[viewScanned_], store)) {
                view_.push_back(viewScanned_);
            }
        }
    }
    const std::size_t shown = filtered ? view_.size() : items_.size();

    const ImGuiTableFlags flags = ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("history", 4, flags)) {
        const float ch = ImGui::CalcTextSize("0").x;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("SEQ", ImGuiTableColumnFlags_WidthFixed, ch * 6);
        ImGui::TableSetupColumn("AT", ImGuiTableColumnFlags_WidthFixed, ch * 13);
        ImGui::TableSetupColumn("KIND", ImGuiTableColumnFlags_WidthFixed, ch * 13);
        ImGui::TableSetupColumn("STREAM | ACTOR | BODY", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < 4; ++c) {
            ImGui::TableSetColumnIndex(c);
            dimText("%s", ImGui::TableGetColumnName(c));
        }

        // Leaving the bottom turns following off; scrolling back to it turns it on again.
        if (ImGui::GetScrollMaxY() > 0.0f) {
            const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
            if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
                stickToBottom_ = atBottom && ImGui::GetIO().MouseWheel < 0.0f;
            }
            if (touchDragScroll()) { // touch: a finger drag scrolls the rows (iOS)
                stickToBottom_ =
                    ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y >= ImGui::GetScrollMaxY() - 2.0f;
            }
        }

        // A search hit asked for a seq: find its row among the shown ones, make the clipper submit
        // it, scroll there and select it; following stops so the row stays in view.
        int scrollRow = -1;
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        if (shownBeforePrepend_ != 0 && shown >= shownBeforePrepend_) {
            ImGui::SetScrollY(ImGui::GetScrollY() + static_cast<float>(shown - shownBeforePrepend_) * rowHeight);
        }
        shownBeforePrepend_ = 0;
        lastShown_ = shown;
        if (scrollTo_) {
            for (std::size_t i = 0; i < shown; ++i) {
                const Item& item = items_[filtered ? view_[i] : i];
                if (!item.note && rows[item.index].seq == *scrollTo_) {
                    scrollRow = static_cast<int>(i);
                    break;
                }
            }
            if (scrollRow >= 0) {
                stickToBottom_ = false;
                selected_ = selAnchor_ = scrollRow;
            }
            // a row older than the window: keep asking until the pages reach it
            const bool older = !rows.empty() && *scrollTo_ < rows.front().seq && !store.complete();
            if (!older) {
                scrollTo_.reset();
            } else if (onNeedOlder) {
                onNeedOlder();
            }
        }
        if (!store.complete() && !stickToBottom_ && onNeedOlder && ImGui::GetScrollY() < rowHeight * 30.0f) {
            onNeedOlder();
        }
        drive::hit("history-rows", ImGui::GetWindowPos(),
                   ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown));
        if (scrollRow >= 0) {
            clipper.IncludeItemByIndex(scrollRow);
        }
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Item& item = items_[filtered ? view_[static_cast<std::size_t>(i)] : static_cast<std::size_t>(i)];
                const int selLo = std::min(selected_, selAnchor_);
                const int selHi = std::max(selected_, selAnchor_);
                const bool inverse = selected_ >= 0 && selAnchor_ >= 0 && i >= selLo && i <= selHi;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                // The whole row is one selectable; inverse video when selected. Shift-click extends
                // the range from the anchor; a click on the single selected row clears it.
                // A row about a Message is a way to that Message: a plain click opens it in its chat.
                std::optional<std::string> message;
                if (!item.note && onOpenMessage) {
                    const Event& e = rows[item.index];
                    if (e.kind == "sent" || e.kind == "delivered") {
                        message = chat::MessageCache::messageIdOf(e);
                    }
                }
                if (ImGui::Selectable("##row", inverse,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    if (ImGui::GetIO().KeyShift && selAnchor_ >= 0) {
                        selected_ = i;
                    } else if (message) {
                        selected_ = selAnchor_ = i;
                        onOpenMessage(*message, rows[item.index].actor);
                    } else if (inverse && selLo == selHi) {
                        selected_ = selAnchor_ = -1;
                    } else {
                        selected_ = selAnchor_ = i;
                    }
                }
                if (message) {
                    drive::hit("history-message:" + std::to_string(rows[item.index].seq), ImGui::GetItemRectMin(),
                               ImGui::GetItemRectMax());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("open this Message in its chat; shift-click selects rows");
                    }
                }
                if (ImGui::BeginPopupContextItem("row-menu")) {
                    if (!inverse) {
                        selected_ = selAnchor_ = i;
                    }
                    if (menuItem("COPY ROW(S)", "Cmd-C")) {
                        ImGui::SetClipboardText(selectionText(store, chat, filtered).c_str());
                    }
                    if (!item.note) {
                        const Event& e = rows[item.index];
                        if (menuItem("COPY BODY")) {
                            ImGui::SetClipboardText(e.body.dump(2).c_str());
                        }
                        if (menuItem("COPY EVENT ID")) {
                            ImGui::SetClipboardText(e.id.c_str());
                        }
                        if (menuItem("COPY STREAM ID")) {
                            ImGui::SetClipboardText(e.stream.c_str());
                        }
                    }
                    ImGui::EndPopup();
                }
                if (i == scrollRow) {
                    ImGui::SetScrollHereY(0.3f);
                }
                ImGui::SameLine(0.0f, 0.0f);
                if (item.note) {
                    drawNote(chat.notes()[item.index], anim, inverse);
                } else {
                    drawEvent(rows[item.index], chat, anim, inverse);
                }
                ImGui::PopID();
            }
        }
        if (stickToBottom_ && shown != lastCount_) {
            ImGui::SetScrollHereY(1.0f);
        }
        lastCount_ = shown;
        ImGui::EndTable();
        insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    if (selected_ >= 0 && copyChord() && !ImGui::GetIO().WantTextInput &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        ImGui::SetClipboardText(selectionText(store, chat, filtered).c_str());
    }
    // Deselect: Escape, or a click on the pane that lands on no row.
    if (selected_ >= 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        selected_ = selAnchor_ = -1;
    }
    if (selected_ >= 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        selected_ = selAnchor_ = -1;
    }
    ImGui::PopFont();
}

void HistoryPane::drawNote(const chat::Note& n, const Anim& anim, bool inverse) {
    ImU32 color = Ink::primary;
    switch (n.kind) {
    case chat::NoteKind::Boot:
        color = Ink::strong;
        break;
    case chat::NoteKind::Echo:
    case chat::NoteKind::Dim:
        color = Ink::secondary;
        break;
    case chat::NoteKind::Who:
        color = Ink::primary;
        break;
    case chat::NoteKind::Error:
        color = Ink::inspected;
        break;
    }
    text(ink(inverse, Ink::secondary), "-");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    if (!n.chip.empty()) {
        chip(n.chip, ink(inverse, Ink::inspected));
    }
    ImGui::TableNextColumn();
    text(ink(inverse, color), n.text);
    if (n.cursor) {
        ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 cell(ImGui::CalcTextSize("0").x, ImGui::GetTextLineHeight());
        blockCursor(ImGui::GetWindowDrawList(), ImVec2(pos.x + cell.x / 2, pos.y), cell, anim.blinkOn, true, nullptr);
        ImGui::Dummy(ImVec2(cell.x * 1.5f, cell.y));
    }
}

void HistoryPane::drawEvent(const Event& e, const chat::Chat& chat, const Anim& anim,
                            bool inverse) {
    text(ink(inverse, Ink::primary), std::to_string(e.seq));
    ImGui::TableNextColumn();
    text(ink(inverse, Ink::secondary), clockPart(e.at));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", e.at.c_str());
    }
    ImGui::TableNextColumn();
    const auto messageId = chat::MessageCache::messageIdOf(e);
    if (messageId) {
        chip("MESSAGE", ink(inverse, Ink::nav));
    } else {
        chip(e.kind, ink(inverse, kindColor(e.kind)));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\nevent %s", e.kind.c_str(), e.id.c_str());
    }
    ImGui::TableNextColumn();

    if (messageId && e.kind == "delivered" && sentSeen_.count(*messageId) != 0) {
        // the `sent` row above already shows the text; a narrow Principal that only sees the
        // delivery keeps the full row below
        std::string line = "delivered";
        if (e.body.contains("recipient") && e.body["recipient"].is_string()) {
            line += " to " + nameOf(e.body["recipient"].get<std::string>());
        }
        text(ink(inverse, Ink::secondary), line);
    } else if (messageId) {
        const chat::MessageCache& cache = chat.messages();
        if (const chat::MessageRow* m = cache.get(*messageId)) {
            text(ink(inverse, Ink::nav), nameOf(m->from));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("from %s\nmessage %s", m->from.c_str(), m->id.c_str());
            }
            ImGui::SameLine(0.0f, 0.0f);
            text(ink(inverse, Ink::strong), " > ");
            ImGui::SameLine(0.0f, 0.0f);
            symbolUi().text(m->text, ink(inverse, Ink::primary), m->from, inverse);
            if (m->inReplyTo) {
                ImGui::SameLine();
                text(ink(inverse, Ink::nav), "<- " + shortId(*m->inReplyTo));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("in reply to %s", m->inReplyTo->c_str());
                }
            }
            ImGui::SameLine();
            std::string tail = "// " + e.kind;
            if (e.kind == "delivered" && e.body.contains("recipient") && e.body["recipient"].is_string()) {
                tail += " to " + nameOf(e.body["recipient"].get<std::string>());
            }
            text(ink(inverse, Ink::secondary), tail);
        } else if (const std::string* code = cache.failure(*messageId)) {
            text(ink(inverse, Ink::inspected), "<message " + shortId(*messageId) + ": " + *code + ">");
        } else {
            text(ink(inverse, Ink::secondary), "<reading message " + shortId(*messageId) + ">");
        }
    } else {
        text(ink(inverse, Ink::primary), nameOf(e.stream));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("stream %s\n%s", nameOf(e.stream).c_str(), e.stream.c_str());
        }
        ImGui::SameLine(0.0f, 0.0f);
        text(ink(inverse, Ink::secondary), " | ");
        ImGui::SameLine(0.0f, 0.0f);
        text(ink(inverse, Ink::primary), nameOf(e.actor));
        if (ImGui::IsItemHovered()) {
            std::string tip = "actor " + e.actor;
            if (e.via) {
                tip += "\nvia " + *e.via;
            }
            if (e.delegation) {
                tip += "\ndelegation " + *e.delegation;
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::SameLine(0.0f, 0.0f);
        text(ink(inverse, Ink::secondary), " | ");
        ImGui::SameLine(0.0f, 0.0f);
        // A snapshot read is one `resolved` for the whole page: name `*`, a count, no object.
        if (e.kind == "resolved" && e.body.is_object() && e.body.contains("count") &&
            (!e.body.contains("object") || e.body["object"].is_null())) {
            const long long n = e.body["count"].is_number() ? e.body["count"].get<long long>() : 0;
            text(ink(inverse, Ink::unresolved), "* \u00b7 " + std::to_string(n) + " objects");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("a snapshot read: every object this principal resolves, in one event\n%s", oneLine(e.body).c_str());
            }
        } else {
            const float bodyX = ImGui::GetCursorScreenPos().x;
            symbolUi().text(oneLine(e.body), ink(inverse, Ink::primary), e.stream, inverse);
            // the whole event on the rest of the row, unless a link is under the mouse
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (ImGui::IsWindowHovered() && mouse.x >= bodyX && mouse.y >= ImGui::GetItemRectMin().y &&
                mouse.y < ImGui::GetItemRectMax().y && ImGui::GetMouseCursor() != ImGuiMouseCursor_Hand) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(e.body.dump(2).c_str());
                ImGui::EndTooltip();
            }
        }
    }
    if (chat.streamingSeqs().count(e.seq) != 0) {
        ImGui::SameLine();
        text(ink(inverse, Ink::primary), anim.spinner);
    }
}

void HistoryPane::openSearch() {
    collapsed = false;
    searchOpen_ = true;
    focusSearch_ = true;
}

void HistoryPane::closeSearch() {
    searchOpen_ = false;
    focusSearch_ = false;
}

void HistoryPane::search(const std::string& text) {
    query_ = text;
    hits_.clear();
    local_ = false;
    status_ = text.empty() ? "" : "searching\xe2\x80\xa6";
    if (onSearch && !text.empty()) {
        onSearch(text);
    }
}

void HistoryPane::setHits(std::vector<chat::SearchHit> hits, bool local, std::string status) {
    hits_ = std::move(hits);
    local_ = local;
    status_ = std::move(status);
}

void HistoryPane::scrollToSeq(Seq seq) {
    scrollTo_ = seq;
}

void HistoryPane::drawSearch() {
    ImGui::PushID("search");
    dimText("SEARCH");
    ImGui::SameLine();
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", query_.c_str());
    if (focusSearch_) {
        ImGui::SetKeyboardFocusHere();
        focusSearch_ = false;
    }
    const float closeW = ImGui::CalcTextSize("[ X ]").x + 2 * ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - closeW));
    const bool entered = ImGui::InputText("##q", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue);
    drive::hit("input:search", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
    if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        closeSearch();
    }
    if (entered) {
        search(buf);
        focusSearch_ = true; // Return keeps the box focused for the next query
    } else {
        query_ = buf;
    }
    if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        closeSearch();
    }
    ImGui::SameLine();
    if (bracketButtonNamed("x", "search-close")) {
        closeSearch();
    }
    if (!status_.empty()) {
        text(Ink::secondary, status_);
        if (local_) {
            ImGui::SameLine();
            text(Ink::secondary, "local");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("the world has no search op; this searched what the listener already holds (Fields, names, read Messages)");
            }
        }
    }
    if (!hits_.empty()) {
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float h = std::min<std::size_t>(hits_.size(), 8) * lineH + ImGui::GetStyle().WindowPadding.y * 2;
        if (ImGui::BeginChild("hits", ImVec2(0.0f, h), ImGuiChildFlags_Borders)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (std::size_t i = 0; i < hits_.size(); ++i) {
                const chat::SearchHit& hit = hits_[i];
                ImGui::PushID(static_cast<int>(i));
                const bool clicked = ImGui::Selectable("##hit", false, ImGuiSelectableFlags_AllowOverlap);
                drive::hit("search:hit:" + std::to_string(i), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s%s\nseq %lld  score %.1f\nclick: open the object (a Principal's FIELDS) or scroll the rows to the seq",
                                      hit.object.c_str(), hit.field.empty() ? "" : ("  " + hit.field).c_str(),
                                      static_cast<long long>(hit.seq), hit.score);
                }
                if (clicked) {
                    if (onOpenObject && names_ && names_->get(hit.object)) {
                        onOpenObject(hit.object);
                    } else if (hit.seq > 0) {
                        scrollToSeq(hit.seq);
                    }
                }
                ImGui::SameLine(0.0f, 0.0f);
                const ImU32 kindColor = hit.kind == "message" ? Ink::primary : (hit.kind == "object" ? Ink::nav : Ink::inspected);
                chip(upper(hit.kind.empty() ? std::string("hit") : hit.kind), kindColor);
                ImGui::SameLine();
                text(Ink::nav, hit.name.empty() ? nameOf(hit.object) : hit.name);
                if (!hit.field.empty()) {
                    ImGui::SameLine();
                    text(Ink::secondary, hit.field);
                }
                ImGui::SameLine();
                // The snippet, its match in inverse video (phosphor box, black text).
                if (hit.matchStart != std::string::npos && hit.matchStart + hit.matchLen <= hit.snippet.size()) {
                    const std::string pre = hit.snippet.substr(0, hit.matchStart);
                    const std::string match = hit.snippet.substr(hit.matchStart, hit.matchLen);
                    const std::string post = hit.snippet.substr(hit.matchStart + hit.matchLen);
                    if (!pre.empty()) {
                        text(Ink::primary, pre);
                        ImGui::SameLine(0.0f, 0.0f);
                    }
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    const ImVec2 size = ImGui::CalcTextSize(match.c_str());
                    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Surface::selection);
                    dl->AddText(pos, Ink::onSelection, match.c_str());
                    ImGui::Dummy(size);
                    if (!post.empty()) {
                        ImGui::SameLine(0.0f, 0.0f);
                        text(Ink::primary, post);
                    }
                } else {
                    text(Ink::primary, hit.snippet);
                }
                if (hit.seq > 0) {
                    ImGui::SameLine();
                    text(Ink::secondary, "seq " + std::to_string(hit.seq));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::PopID();
}

} // namespace ke::ui
