#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

void TerminalPane::sendDirective(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat,
                                 const Session& session, const std::string& line) {
    const std::vector<std::string> routes = info ? info->routes() : std::vector<std::string>{};
    if (routes.empty()) {
        tab.conv.status = "no Route to " + principals.nameOf(tab.principal);
        return;
    }
    std::vector<std::string> to = model::recipientsFor(principals, tab.principal, routes.front(), session.principal().value_or(""));
    if (to.size() > 1) {
        to = {to[static_cast<std::size_t>(tab.recipientIndex) % to.size()]};
    }
    std::string* status = &tab.conv.status;
    chat.submitTo(to, routes.front(), line, [status](const std::string& code, const std::string& message) {
        if (!code.empty()) {
            *status = code + ": " + message;
        }
    });
}

void TerminalPane::askConversations(Tab& tab, chat::Chat& chat, long long before) {
    Tab::ConversationsUi* ui = &tab.conv;
    ui->asked = true;
    ui->busy = true;
    json args = {{"conversations", true}, {"principal", tab.principal}, {"limit", 50}};
    if (before > 0) {
        args["before"] = before;
    } else {
        ui->rows.clear();
    }
    chat.history(std::move(args), [ui](const Response& r) {
        ui->busy = false;
        if (!r.ok) {
            ui->err = r.err ? r.err->code + ": " + r.err->message : std::string("refused");
            return;
        }
        ui->err.clear();
        // A world without the list ignores the key and answers events: say so, do not show nothing.
        ui->supported = r.result.contains("conversations");
        std::vector<chat::ConversationRow> more = chat::parseConversations(r.result);
        ui->rows.insert(ui->rows.end(), more.begin(), more.end());
        ui->more = r.result.contains("next") && !r.result["next"].is_null();
    });
}

void TerminalPane::drawCompression(Tab& tab, const model::PrincipalInfo& info, chat::Chat& chat) {
    const float ch = ImGui::CalcTextSize("0").x;
    // A number of tokens: blank goes back to the world's default.
    const auto number = [&](const char* label, const char* name, const char* unset, const char* tip) {
        FieldEdit& edit = tab.edits[name];
        const auto it = info.fields.find(name);
        const bool have = it != info.fields.end() && it->second.value.is_number();
        const std::string shown = have ? std::to_string(it->second.value.get<long long>()) : std::string();
        if (!edit.editing) {
            edit.buffer = shown;
        }
        ImGui::PushID(name);
        ImGui::AlignTextToFramePadding();
        text(Ink::secondary, label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ch * 8);
        const bool enter = ImGui::InputTextWithHint("##v", unset, &edit.buffer, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        edit.editing = ImGui::IsItemActive() || edit.buffer != shown;
        drive::hit(std::string("input:") + name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            wrappedTooltip(tip);
        }
        ImGui::SameLine();
        const bool changed = edit.buffer != shown;
        if (bracketButtonNamed("set", std::string("set:") + name, changed) || (enter && changed)) {
            edit.editing = false;
            if (edit.buffer.empty()) {
                chat.actNamed("field.clear", json{{"object", tab.principal}, {"field", name}}, [](const Response&) {});
            } else {
                setField(tab, chat, name, "number", "both", json(std::stoll(edit.buffer.substr(0, 9))), edit);
            }
        }
        ImGui::PopID();
    };
    number("compress at", "serialize/compress-at", "auto", "tokens of unsummarised conversation at which a wake compresses first; blank works it out from the budget, 0 is off");
    ImGui::SameLine();
    number("summary under", "serialize/summary-tokens", "400", "the size a conversation's summary should stay under, in tokens");
    ImGui::SameLine();
    const bool own = info.fields.count("serialize/compressor") != 0;
    if (bracketButtonNamed(own ? "edit prompt" : "customise prompt", "compressor")) {
        const std::string object = tab.principal;
        const auto open = [object] {
            LinkRequest r;
            r.kind = LinkRequest::EditField;
            r.object = object;
            r.field = "serialize/compressor";
            symbolUi().ask(std::move(r));
        };
        if (own) {
            open();
        } else {
            chat.actNamed("conversation.compressor", json{{"principal", object}}, [open](const Response& r) {
                if (r.ok) {
                    open();
                }
            });
        }
    }
    if (ImGui::IsItemHovered()) {
        wrappedTooltip(own ? "this Principal's own prompt for writing summaries" : "starts from the shipped prompt, for this Principal only");
    }
}

void TerminalPane::drawConversations(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals,
                                     chat::Chat& chat, const Session& session) {
    Tab::ConversationsUi& c = tab.conv;
    if (!c.asked && session.attached()) {
        askConversations(tab, chat, 0);
    }
    if (info != nullptr && info->drivenBy == "process") {
        drawCompression(tab, *info, chat);
        ImGui::Separator();
    }
    if (bracketButtonNamed("refresh", "conversation-refresh", session.attached() && !c.busy)) {
        askConversations(tab, chat, 0);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-(squareSide() + ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##search", "search titles and people", &c.search);
    drive::hit("input:conversation-search", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (plusAtRight("conversation-new", "add a conversation: this one closes, the next line starts fresh", session.attached())) {
        sendDirective(tab, info, principals, chat, session, "/new");
    }
    if (!c.err.empty()) {
        text(Ink::inspected, c.err);
    }
    if (!c.status.empty()) {
        text(Ink::inspected, c.status);
    }
    if (c.busy && c.rows.empty()) {
        dimText("reading");
        return;
    }
    if (!c.supported) {
        dimText("this world does not list conversations yet");
        return;
    }
    if (c.rows.empty()) {
        dimText("no conversations with %s yet", principals.nameOf(tab.principal).c_str());
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("conversations", 5, flags)) {
        ImGui::TableSetupColumn("TITLE", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("WITH");
        ImGui::TableSetupColumn("LAST");
        ImGui::TableSetupColumn("SIZE");
        ImGui::TableSetupColumn("");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int i = 0; i < 5; ++i) {
            ImGui::TableSetColumnIndex(i);
            dimText("%s", ImGui::TableGetColumnName(i));
        }
        touchDragScroll();
        for (const chat::ConversationRow& r : c.rows) {
            std::string who;
            for (const std::string& p : r.participants) {
                who += (who.empty() ? "" : " ") + principals.nameOf(p);
            }
            chat::ConversationRow named = r;
            named.participants = {who};
            if (!chat::conversationMatches(named, c.search)) {
                continue;
            }
            ImGui::PushID(r.thread.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (c.renaming == r.thread) {
                ImGui::SetNextItemWidth(-1);
                const bool enter = ImGui::InputTextWithHint("##rename", "conversation title", &c.renameTitle,
                                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                drive::hit("input:conversation-rename:" + chat::idPrefix(r.thread), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (enter && !c.renameTitle.empty()) {
                    const std::string thread = r.thread;
                    const std::string title = c.renameTitle;
                    chat.actNamed("conversation.rename", json{{"thread", thread}, {"title", title}}, [&c, thread, title](const Response& res) {
                        if (res.ok) {
                            for (chat::ConversationRow& row : c.rows) {
                                if (row.thread == thread) row.title = title;
                            }
                            c.renaming.clear();
                            c.renameTitle.clear();
                            c.err.clear();
                        } else {
                            c.err = res.err ? res.err->code + ": " + res.err->message : std::string("rename refused");
                        }
                    });
                }
            } else {
                text(Ink::strong, r.title.empty() ? chat::idPrefix(r.thread) : r.title);
                if (r.compressed) {
                    ImGui::SameLine();
                    text(Ink::secondary, "compressed");
                }
            }
            ImGui::TableNextColumn();
            text(Ink::secondary, who);
            ImGui::TableNextColumn();
            text(Ink::secondary, r.lastAt.size() >= 16 ? r.lastAt.substr(0, 16) : r.lastAt);
            ImGui::TableNextColumn();
            text(Ink::secondary, std::to_string(r.messages) + " msgs");
            ImGui::TableNextColumn();
            const std::string shortId = chat::idPrefix(r.thread);
            if (bracketButtonNamed(c.renaming == r.thread ? "cancel" : "rename", "conversation-rename:" + shortId, session.attached())) {
                if (c.renaming == r.thread) {
                    c.renaming.clear();
                    c.renameTitle.clear();
                } else {
                    c.renaming = r.thread;
                    c.renameTitle = r.title;
                }
            }
            ImGui::SameLine();
            if (bracketButtonNamed("open", "conversation-open:" + shortId)) {
                Tab::ThreadUi tu; // the read-only thread window
                tu.thread = r.thread;
                tab.threadUi[r.thread] = std::move(tu);
            }
            ImGui::SameLine();
            if (bracketButtonNamed("resume", "conversation-resume:" + shortId, session.attached())) {
                sendDirective(tab, info, principals, chat, session, "/resume " + r.thread);
                tab.view = 0;
            }
            ImGui::PopID();
        }
        if (c.more) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (bracketButtonNamed("older", "conversation-older", !c.busy)) {
                askConversations(tab, chat, c.rows.back().lastSeq);
            }
        }
        ImGui::EndTable();
        insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
}

} // namespace ke::ui
