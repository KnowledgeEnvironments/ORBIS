#include "model/FieldNews.hpp"
#include "ui/UnsafeView.hpp"
#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

void TerminalPane::setField(Tab& tab, chat::Chat& chat, const std::string& name, const json& type,
                            const std::string& reader, const json& value, FieldEdit& edit) {
    (void)tab;
    edit.result = "...";
    edit.resultErr = false;
    FieldEdit* target = &edit;
    chat.setField(tab.principal, name, type, value, reader.empty() ? "both" : reader, [this, target, id = tab.principal](const Response& r) {
        if (r.ok) {
            objectsAsked_.erase(id); // an inspected object has no fold behind it: read it again
            target->result = "OK seq " + std::to_string(r.result.value("seq", r.seq));
            target->resultErr = false;
            target->editing = false;
        } else {
            target->result = "ERR " + (r.err ? r.err->code + " " + r.err->message : std::string("?"));
            target->resultErr = true;
        }
    });
}

void TerminalPane::drawFieldNews(const model::PrincipalInfo& info, const model::FieldState& f, const model::Principals& principals) {
    const model::FieldNews news = model::fieldNews(principals, info, f);
    if (news == model::FieldNews::None) {
        return;
    }
    const std::string who = principals.nameOf(info.id);
    if (news == model::FieldNews::Waiting) {
        text(Ink::inspected, "applies at " + who + "'s next wake");
    } else {
        text(Ink::secondary, who + " has seen this");
    }
    drive::hit("news:" + f.name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void TerminalPane::drawRendered(FieldEdit& edit, Seq seq, const std::string& value, float wrap, ImFont* mono,
                                const char* key, const nlohmann::json* refs) {
    const unsigned long long symbols = symbolUi().table() ? symbolUi().table()->version() : 0;
    if (edit.docFor != seq || edit.docSymbols != symbols || edit.docTheme != themeVersion()) {
        edit.docTheme = themeVersion();
        edit.doc = SelDoc{};
        edit.doc.version = ++edit.docBuilds;
        edit.md = MdExtras{};
        edit.docSymbols = symbols;
        scan_.table = symbolUi().table();
        scan_.ctx.fold = symbolUi().fold();
        scan_.ctx.object = current();
        edit.md.scan = &scan_;
        std::vector<chat::RefSpan> spans;
        if (refs != nullptr && refs->is_array()) { // `<field>/refs`: the links are given
            spans = chat::parseRefs(*refs, value);
            edit.md.refs = &spans;
        }
        if (looksLikeMarkdown(value)) {
            appendMarkdown(edit.doc, value, Ink::primary, 0, edit.md);
        } else {
            appendLinked(edit.doc, value, Ink::primary, 0, edit.md);
        }
        edit.md.refs = nullptr;
        edit.doc.finish();
        if (edit.docText != value) { // the same text rebuilt keeps a press in progress: a click must land
            edit.docText = value;
            edit.sel.clear();
        }
        edit.docFor = seq;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawListSplitter split;
    split.Split(ImGui::GetWindowDrawList(), 3);
    split.SetCurrentChannel(ImGui::GetWindowDrawList(), 1);
    const SelResult sr = drawSelectableDoc(edit.doc, edit.sel, wrap, mono, ImGui::GetFontSize(), true, &edit.layout);
    split.SetCurrentChannel(ImGui::GetWindowDrawList(), 2);
    drawMarkdownExtras(edit.md, edit.doc, edit.layout, origin, wrap, sr.hoveredPara, key, &split);
    split.Merge(ImGui::GetWindowDrawList());
    symbolUi().handleDoc(edit.md, edit.doc, sr, edit.sel.dragging);
    registerSymbolHits(edit.md, edit.doc, edit.layout, mono, origin);
}

std::size_t TerminalPane::foldAt(const std::string& whole, float charsPerLine) {
    constexpr std::size_t kPreviewChars = 360;
    constexpr std::size_t kPreviewLines = 4;
    std::size_t nl = 0, pos = 0;
    while (pos < whole.size() && nl < kPreviewLines) {
        nl += whole[pos] == '\n' ? 1 : 0;
        ++pos;
    }
    // A row is four lines as drawn, whatever the width: an unbroken string wraps too.
    const std::size_t perLine = static_cast<std::size_t>(std::max(8.0f, charsPerLine));
    std::size_t cut = std::min<std::size_t>({whole.size(), pos, kPreviewChars, perLine * kPreviewLines});
    // Never through the middle of a word short enough to be a path or a name: a link cut in half
    // opens the wrong thing. A truly unbroken string still folds.
    const std::size_t stop = std::min(whole.size(), cut + 256);
    for (std::size_t i = cut; i < stop; ++i) {
        if (whole[i] == ' ' || whole[i] == '\n') {
            cut = i;
            break;
        }
        if (i + 1 == whole.size()) {
            cut = whole.size();
        }
    }
    while (cut > 0 && cut < whole.size() && (static_cast<unsigned char>(whole[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return cut;
}

std::string TerminalPane::sizeLine(const std::string& whole) {
    const std::size_t lines = 1 + static_cast<std::size_t>(std::count(whole.begin(), whole.end(), '\n'));
    return std::to_string(whole.size()) + " characters, " + std::to_string(lines) + (lines == 1 ? " line" : " lines");
}

bool TerminalPane::tallField(const model::FieldState& f) {
    if (!f.value.is_string()) {
        return false; // a list or a map keeps its row: only a string opens in the editor
    }
    const std::string& s = f.value.get_ref<const std::string&>();
    constexpr std::size_t kLongChars = 120;
    return s.size() > kLongChars || s.find('\n') != std::string::npos;
}

void TerminalPane::clearField(const Tab& tab, chat::Chat& chat, const std::string& name, FieldEdit& edit) {
    FieldEdit* target = &edit;
    chat.actNamed("field.clear", json{{"object", tab.principal}, {"field", name}},
                  [this, target, id = tab.principal](const Response& r) {
                      objectsAsked_.erase(id);
                      target->resultErr = !r.ok;
                      target->result = r.ok ? "cleared" : "ERR " + (r.err ? r.err->code + " " + r.err->message : std::string());
                  });
}

void TerminalPane::editInPane(const Tab& tab, const std::string& name) {
    LinkRequest r;
    r.kind = LinkRequest::EditField;
    r.object = tab.principal;
    r.field = name;
    symbolUi().ask(std::move(r));
}

void TerminalPane::drawNewField(Tab& tab, NewField& nf, const model::PrincipalInfo* info, chat::Chat& chat,
                                const char* prefix, const char* scope) {
    static const char* const kReaders[] = {"agent", "human", "both"};
    const float ch = ImGui::CalcTextSize("0").x;
    if (!nf.open) {
        return;
    }
    const float lineH = ImGui::GetTextLineHeight();
    const std::string sc = scope;
    const std::string full = prefix + nf.name;
    FieldEdit& edit = tab.edits["##" + sc];
    ImGui::PushID(scope);
    if (nf.sent && !edit.resultErr && edit.result.rfind("OK", 0) == 0) { // landed: ready for the next one
        nf.sent = false;
        nf.open = false;
        nf.value.clear();
        nf.name.clear();
        if (sc == "new-prompt") {
            nf.name = "instructions/";
        }
    }
    text(Ink::primary, upper(*prefix ? "NEW MEMORY" : "NEW FIELD"));
    ImGui::SameLine();
    if (*prefix) {
        text(Ink::secondary, prefix);
        ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::SetNextItemWidth(ch * 30);
    ImGui::PushStyleColor(ImGuiCol_Border, Ink::secondary);
    ImGui::InputTextWithHint("##name", *prefix ? "project" : "name, like notes/today", &nf.name);
    ImGui::PopStyleColor();
    drive::hit("input:" + sc + ":name", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ch * 12);
    if (ImGui::BeginCombo("##type", chat::kFieldTypes[nf.type], ImGuiComboFlags_NoArrowButton)) {
        for (int t = 0; t < chat::kFieldTypeCount; ++t) {
            if (ImGui::Selectable(chat::kFieldTypes[t], t == nf.type)) {
                nf.type = t;
            }
            drive::hit("menu:" + sc + ":type:" + chat::kFieldTypes[t], ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ImGui::EndCombo();
    }
    drive::hit("menu:" + sc + ":type", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    for (int r = 0; r < 3; ++r) {
        ImGui::SameLine();
        ImGui::PushID(r);
        if (bracketButtonNamed(kReaders[r], "reader:" + sc + ":" + kReaders[r], true, nf.reader == r)) {
            nf.reader = r;
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("who this Field is shown to: the model, people, or both");
        }
    }
    const std::string kind = chat::kFieldTypes[nf.type];
    const bool tall = kind == "string" || kind == "list" || kind == "map";
    ImGui::PushStyleColor(ImGuiCol_Border, Ink::secondary);
    if (tall) {
        ImGui::InputTextMultiline("##value", &nf.value, ImVec2(-FLT_MIN, lineH * 3.4f), ImGuiInputTextFlags_AllowTabInput);
    } else if (kind == "boolean") {
        ImGui::SetNextItemWidth(ch * 12);
        ImGui::InputTextWithHint("##value", "true or false", &nf.value);
    } else {
        ImGui::SetNextItemWidth(ch * 44);
        ImGui::InputTextWithHint("##value", kind == "ref" ? "a name, or @ to look one up" : kind.c_str(), &nf.value);
    }
    ImGui::PopStyleColor();
    drive::hit("input:" + sc + ":value", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (kind == "ref" && symbolUi().table() && !nf.value.empty()) {
        const std::string want = nf.value.front() == '@' ? nf.value.substr(1) : nf.value;
        const auto hits = symbolUi().table()->complete(want, 6);
        if (!(hits.size() == 1 && hits.front()->name == want)) {
            for (std::size_t i = 0; i < hits.size(); ++i) {
                if (i > 0) {
                    ImGui::SameLine();
                }
                ImGui::PushID(static_cast<int>(i));
                if (bracketButtonNamed(hits[i]->name.c_str(), "ref:" + hits[i]->name)) {
                    nf.value = hits[i]->name;
                }
                ImGui::PopID();
            }
        }
    }
    const chat::ParsedField parsed = chat::parseFieldValue(kind, nf.value, symbolUi().table());
    const bool exists = info && !full.empty() && info->field(full) != nullptr;
    const bool named = !nf.name.empty() && nf.name.back() != '/';
    if (bracketButtonNamed("set", *prefix ? "set-memory" : (sc == "new-field" ? "set-field" : "set-prompt-field"),
                           named && parsed.ok && !exists && !nf.sent)) {
        nf.sent = true;
        setField(tab, chat, full, parsed.type, kReaders[nf.reader], parsed.value, edit);
    }
    ImGui::SameLine();
    if (exists) {
        dimText("%s exists: edit its row, this will not overwrite it", full.c_str());
        ImGui::SameLine();
        if (bracketButtonNamed("go", "go:" + sc)) {
            tab.view = 2;
            tab.scrollField = full;
        }
    } else if (!nf.value.empty() && !parsed.ok) {
        dimText("%s", parsed.error.c_str());
    } else if (const std::string hint = chat::fieldNameHint(full); !hint.empty()) {
        dimText("%s", hint.c_str());
    } else if (kind == "string") {
        dimText("bytes: drop a file on a chat");
    }
    if (!edit.result.empty()) {
        ImGui::SameLine();
        text(edit.resultErr ? Ink::inspected : Ink::connected, edit.result);
    }
    if (edit.resultErr) {
        nf.sent = false;
    }
    ImGui::PopID();
}

void TerminalPane::drawHintCell(Tab& tab, const model::PrincipalInfo* info, chat::Chat& chat, const std::string& base,
                                float width) {
    const std::string hintName = base + "/hint";
    const model::FieldState* hf = info ? info->field(hintName) : nullptr;
    const std::string current = hintOf(info, base);
    FieldEdit& h = tab.edits[hintName];
    ImGui::PushID("hint");
    if (h.editing) {
        const float ch = ImGui::CalcTextSize("0").x;
        ImGui::SetNextItemWidth(std::max(ch * 12, width - ch * 34));
        ImGui::PushStyleColor(ImGuiCol_Border, Ink::inspected);
        if (h.focus) {
            ImGui::SetKeyboardFocusHere();
            h.focus = false;
        }
        const bool entered = ImGui::InputText("##hintedit", &h.buffer, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        // who the hint is for: the Field's reader scope
        for (const char* r : {"agent", "human", "both"}) {
            if (bracketButton(r, true, h.reader == r)) {
                h.reader = r;
            }
            ImGui::SameLine();
        }
        if (bracketButton("set") || entered) {
            setField(tab, chat, hintName, hf && !hf->type.is_null() ? hf->type : json("string"), h.reader, json(h.buffer), h);
            h.editing = false;
        }
        ImGui::SameLine();
        if (bracketButton("x")) {
            h.editing = false;
            h.result.clear();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, current.empty() ? Ink::secondary : Ink::inspected);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Surface::hover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, Surface::selection);
        const std::string label = current + "##hintcell";
        bool start = false;
        if (current.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, width - squareSide()));
            start = squareButton("+", "hint-add:" + base);
        } else {
            start = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(width, 0));
        }
        if (start) {
            h.editing = true;
            h.buffer = current;
            h.reader = hf && !hf->reader.empty() ? hf->reader : "both";
            h.focus = true;
            h.result.clear();
        }
        ImGui::PopStyleColor(3);
        drive::hit("hint:" + base, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered() && !current.empty()) {
            ke::ui::tip("%s\nhint by %s, reader %s // click to edit", current.c_str(), hf ? hf->by.c_str() : "?",
                              hf && !hf->reader.empty() ? hf->reader.c_str() : "both");
        } else if (ImGui::IsItemHovered()) {
            ke::ui::tip("a one-line hint about %s for the next reader, person or Principal (%s)", base.c_str(),
                              hintName.c_str());
        }
    }
    if (!h.result.empty()) {
        ImGui::SameLine();
        text(h.resultErr ? Ink::inspected : Ink::connected, h.result);
    }
    ImGui::PopID();
}

void TerminalPane::drawThread(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals,
                              chat::Chat& chat, const Session& session, const std::string& base) {
    const std::string commentName = base + "/comment";
    const model::FieldState* cf = info ? info->field(commentName) : nullptr;
    const json comments = commentsOf(info, base);
    ImGui::PushID("thread");
    ImGui::Indent();
    if (comments.empty()) {
        dimText("no comments on %s yet", base.c_str());
    }
    for (const auto& c : comments) {
        const std::string by = c.is_object() && c.contains("by") && c["by"].is_string() ? c["by"].get<std::string>() : "";
        const std::string when = c.is_object() && c.contains("at") && c["at"].is_string() ? c["at"].get<std::string>() : "";
        const std::string body = c.is_object() && c.contains("text") && c["text"].is_string() ? c["text"].get<std::string>()
                                 : c.is_string()                                               ? c.get<std::string>()
                                                                                               : c.dump();
        text(Ink::strong, by.empty() ? "?" : principals.nameOf(by));
        if (ImGui::IsItemHovered() && !by.empty()) {
            ke::ui::tip("%s", by.c_str());
        }
        ImGui::SameLine();
        text(Ink::secondary, clockPart(when));
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        text(Ink::primary, body);
        ImGui::PopTextWrapPos();
    }
    std::string& reply = tab.replies[base];
    const float ch = ImGui::CalcTextSize("0").x;
    ImGui::SetNextItemWidth(std::max(ch * 20, ImGui::GetContentRegionAvail().x - ch * 14));
    ImGui::PushStyleColor(ImGuiCol_Border, Ink::secondary);
    const bool entered = ImGui::InputText("##reply", &reply, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if ((bracketButton("reply", !reply.empty()) || (entered && !reply.empty())) && info) {
        json next = comments;
        json entry = {{"by", session.principal().value_or("")}, {"at", nowIso()}, {"text", reply}};
        next.push_back(entry);
        const json type = cf && !cf->type.is_null() ? cf->type : json::array({"list", "map"});
        FieldEdit& edit = tab.edits[commentName];
        setField(tab, chat, commentName, type, cf && !cf->reader.empty() ? cf->reader : "both", next, edit);
        reply.clear();
    }
    FieldEdit& edit = tab.edits[commentName];
    if (!edit.result.empty()) {
        ImGui::SameLine();
        text(edit.resultErr ? Ink::inspected : Ink::connected, edit.result);
    }
    ImGui::Unindent();
    ImGui::PopID();
}

void TerminalPane::drawFields(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals,
                              chat::Chat& chat, const Session& session) {
    if (!info) {
        dimText("principal not in the fold yet");
        return;
    }
    const float ch = ImGui::CalcTextSize("0").x;
    {
        const std::string what = principals.describe(tab.principal, info->cls);
        dimText("%s", what.empty() ? (info->cls + ": the world has no description for this kind yet").c_str() : what.c_str());
    }
    if (info->cls == "adapter") {
        unconfineToggle(*info);
    }
    if (info->cls == "principal" && info->drivenBy != "person") {
        const model::EvaluationInfo* last = fieldsEvals_ ? fieldsEvals_->latest(tab.principal) : nullptr;
        if (bracketButtonNamed("view prompt", "view-prompt", last && !last->serialization.empty())) {
            PromptView v;
            v.principal = tab.principal;
            v.serialization = last->serialization;
            v.status = "reading...";
            promptView_ = std::move(v);
            chat.readWhole(last->serialization, "serialization/bytes", [this](std::string whole, std::string err) {
                if (!promptView_) {
                    return;
                }
                promptView_->status = err;
                promptView_->sections = chat::promptSections(whole);
            });
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("the exact text of its latest prompt, what the model was actually told");
        }
    }
    // The object's own hint and comment thread, above the table.
    {
        ImGui::PushID("object");
        text(Ink::secondary, "OBJECT HINT ");
        ImGui::SameLine();
        drawHintCell(tab, info, chat, "object", ImGui::GetContentRegionAvail().x);
        const json oc = commentsOf(info, "object");
        const bool open = tab.openThreads.count("object") != 0;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - squareSide() - ch);
        dimText("COMMENTS %zu // a thread about %s as a whole (object/comment)", oc.size(),
                principals.nameOf(tab.principal).c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(contentRight() - squareSide());
        if (squareButton(open ? "-" : "+", "object-comments", true, open)) {
            if (open) {
                tab.openThreads.erase("object");
            } else {
                tab.openThreads.insert("object");
            }
        }
        if (open) {
            drawThread(tab, info, principals, chat, session, "object");
        }
        ImGui::PopID();
        ImGui::Separator();
    }
    dimText("%zu fields", info->fields.size());
    ImGui::SameLine(contentRight() - squareSide());
    if (squareButton(tab.newField.open ? "x" : "+", "new-field", true, tab.newField.open)) {
        tab.newField.open = !tab.newField.open;
    }
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("add a Field");
    }
    drawNewField(tab, tab.newField, info, chat, "", "new-field");
    if (info->cls == "principal" && info->drivenBy == "process") {
        unsafeToggle(*info);
    }
    ImGui::Separator();
    const ImGuiTableFlags flags = ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    // Columns hold still: the name truncates, the value wraps inside its own column, and the
    // controls keep their place at the row's right edge whatever the value's length.
    if (!ImGui::BeginTable("fields", 7, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    // VALUE gets what is left, so the columns that can wait leave first as the pane narrows.
    const float wide = ImGui::GetContentRegionAvail().x / ch;
    ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthFixed, ch * std::clamp(wide * 0.18f, 14.0f, 24.0f));
    ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed, ch * 8);
    ImGui::TableSetupColumn("VALUE", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, ch * 29);
    ImGui::TableSetupColumn("HINT", ImGuiTableColumnFlags_WidthFixed, ch * 16);
    ImGui::TableSetupColumn("BY", ImGuiTableColumnFlags_WidthFixed, ch * 9);
    ImGui::TableSetupColumn("AT", ImGuiTableColumnFlags_WidthFixed, ch * 9);
    ImGui::TableSetColumnEnabled(1, wide >= 80.0f);
    ImGui::TableSetColumnEnabled(4, wide >= 120.0f);
    ImGui::TableSetColumnEnabled(5, wide >= 100.0f);
    ImGui::TableSetColumnEnabled(6, wide >= 100.0f);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < 7; ++c) {
        ImGui::TableSetColumnIndex(c);
        dimText("%s", ImGui::TableGetColumnName(c));
    }
    touchDragScroll();
    // Rows: every plain Field, plus a facet whose base Field does not exist yet (a hint or a
    // comment left for a Field the Principal has not set), shown as a row with no value.
    std::vector<std::pair<std::string, const model::FieldState*>> rows;
    for (const auto& [name, f] : info->fields) {
        const auto [base, facet] = facetOf(name);
        if (facet.empty()) {
            rows.emplace_back(name, &f);
        } else if (base != "object" && !info->field(base)) {
            if (std::none_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == base; })) {
                rows.emplace_back(base, nullptr);
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const model::FieldState none{};
    for (const auto& [name, fp] : rows) {
        const model::FieldState& f = fp ? *fp : none;
        const json comments = commentsOf(info, name);
        const std::string hint = hintOf(info, name);
        ImGui::PushID(name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (tab.scrollField == name) {
            ImGui::SetScrollHereY(0.3f);
            tab.scrollField.clear();
        }
        const float badgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - squareSide();
        {
            // A long name truncates at the column; the whole of it is on hover.
            const float room = ImGui::GetContentRegionAvail().x - squareSide() - ch;
            std::string shown = name;
            if (ImGui::CalcTextSize(shown.c_str()).x > room) {
                const std::size_t keep = static_cast<std::size_t>(std::max(1.0f, room / ch - 1.0f));
                shown = shown.substr(0, keep) + "\xe2\x80\xa6";
            }
            text(Ink::inspected, shown);
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip(hint.empty() ? "%s" : "%s\nhint: %s", name.c_str(), hint.c_str());
        }
        {
            // the comment badge: [ N ] opens the thread under the row
            const bool open = tab.openThreads.count(name) != 0;
            const std::string badge = comments.empty() ? std::string("+") : std::to_string(comments.size());
            ImGui::SameLine(badgeX);
            if (squareButton(badge.c_str(), "comment:" + name, true, open)) {
                if (open) {
                    tab.openThreads.erase(name);
                } else {
                    tab.openThreads.insert(name);
                }
            }
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("%zu comment(s) on %s (%s/comment) // click to open the thread", comments.size(),
                                  name.c_str(), name.c_str());
            }
        }
        ImGui::TableNextColumn();
        text(Ink::secondary, fp ? typeName(f.type) : "-");
        ImGui::TableNextColumn();
        if (fp) {
            drawFieldNews(*info, f, principals);
        }
        FieldEdit& edit = tab.edits[name];
        if (fp && edit.shownFor != f.seq) {
            edit.shown = editText(f.value);
            edit.shownFor = f.seq;
        }
        // A long value folds to its first lines until MORE, so the table stays scannable and a
        // row does not grow while a value streams in.
        const std::string& whole = edit.shown;
        const std::size_t cut = fp && !edit.expanded ? foldAt(whole, ImGui::GetContentRegionAvail().x / ch) : whole.size();
        const bool clipped = cut < whole.size();
        if (!fp) {
            dimText("(no value yet: only a hint or a comment is set)");
        } else if (edit.editing) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushStyleColor(ImGuiCol_Border, Ink::secondary);
            edit.entered = ImGui::InputText("##edit", &edit.buffer, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopStyleColor();
        } else {
            drawRendered(edit, f.seq + (edit.expanded ? 0 : 1) * 1000000000LL, clipped ? whole.substr(0, cut) + " \xe2\x80\xa6" : whole,
                         ImGui::GetContentRegionAvail().x, ImGui::GetFont(), name.c_str(),
                         info->field(name + "/refs") != nullptr ? &info->field(name + "/refs")->value : nullptr);
            if (clipped) {
                dimText("%s", sizeLine(whole).c_str());
            }
        }
        ImGui::TableNextColumn();
        if (fp && edit.editing) {
            if (bracketButton("set") || edit.entered) {
                setField(tab, chat, name, f.type.is_null() ? json("string") : f.type, f.reader, valueFor(f.type, edit.buffer), edit);
            }
            ImGui::SameLine();
            if (bracketButton("x")) {
                edit.editing = false;
                edit.result.clear();
            }
        } else if (fp) {
            const bool tall = tallField(f);
            if (bracketButtonNamed("edit", "edit:" + name)) {
                if (tall) {
                    editInPane(tab, name);
                } else {
                    edit.editing = true;
                    edit.buffer = editText(f.value);
                    edit.basedOn = f.seq;
                    edit.result.clear();
                }
            }
            if (ImGui::IsItemHovered() && tall) {
                ke::ui::tip("%zu characters: opens in the editor", whole.size());
            }
            ImGui::SameLine();
            if (bracketButtonNamed("clear", "clear:" + name)) {
                if (ImGui::GetIO().KeyShift) {
                    clearField(tab, chat, name, edit);
                } else {
                    tab.clearing = name;
                    tab.confirmFocus = true;
                }
            }
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("removes this Field from reads and prompts; the log keeps its history\nasks first; shift-click clears at once");
            }
            if (clipped || edit.expanded) {
                ImGui::SameLine();
                if (bracketButtonNamed(edit.expanded ? "less" : "more", "more:" + name)) {
                    edit.expanded = !edit.expanded;
                }
                if (ImGui::IsItemHovered()) {
                    ke::ui::tip("%zu characters", whole.size());
                }
            }
        }
        if (!edit.result.empty()) {
            text(edit.resultErr ? Ink::inspected : Ink::connected, edit.result);
        }
        ImGui::TableNextColumn();
        drawHintCell(tab, info, chat, name, ImGui::GetContentRegionAvail().x);
        ImGui::TableNextColumn();
        text(Ink::secondary, fp ? principals.nameOf(f.by) : "-");
        if (fp && ImGui::IsItemHovered()) {
            ke::ui::tip("%s\nreader %s", f.by.c_str(), f.reader.c_str());
        }
        ImGui::TableNextColumn();
        text(Ink::secondary, fp ? clockPart(f.at) : "-");
        if (fp && ImGui::IsItemHovered()) {
            ke::ui::tip("%s\nseq %lld", f.at.c_str(), static_cast<long long>(f.seq));
        }
        if (tab.clearing == name && fp) {
            // Under the row, where the value being cleared is still in view.
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            text(Ink::inspected, "Clear " + name + " on " + principals.nameOf(tab.principal) + "? " + std::to_string(whole.size()) +
                                     " characters. The log keeps its history.");
            ImGui::PopTextWrapPos();
            const Confirmed c = confirmRow("clear", "confirm-clear:" + name, "cancel-clear:" + name, tab.confirmFocus);
            if (c == Confirmed::Yes) {
                clearField(tab, chat, name, edit);
            }
            if (c != Confirmed::Waiting) {
                tab.clearing.clear();
            }
        }
        if (tab.openThreads.count(name)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            drawThread(tab, info, principals, chat, session, name);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace ke::ui
