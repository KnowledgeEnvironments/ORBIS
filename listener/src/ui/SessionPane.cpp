#include "ui/SessionPane.hpp"

#include "util/Log.hpp"

#include <string>

namespace ke::ui {

namespace {

void row(const char* label, const std::string& value, ImU32 color = Ink::primary) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    dimText("%s", label);
    ImGui::TableNextColumn();
    text(color, value);
}

std::string seqText(const std::optional<Seq>& s) {
    return s ? std::to_string(*s) : "-";
}

} // namespace

void SessionPane::draw(const Session& session, const WsClient& client, const EventStore& store,
                       const chat::Chat& chat, ImFont* mono) {
    beginPane("SESSION");
    drawBody(session, client, store, chat, mono);
    endPane();
}

void SessionPane::drawBody(const Session& session, const WsClient& client, const EventStore& store,
                           const chat::Chat& chat, ImFont* mono) {
    touchDragScroll();
    ImGui::PushFont(mono, 0.0f);
    const ConnState st = client.state();
    const bool attached = st == ConnState::Attached;
    const bool connecting = st == ConnState::Connecting || st == ConnState::Attaching;
    if (ImGui::BeginTable("session", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("PRINCIPAL ").x);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        // CARRIER: red while the connection is down after having been up, phosphor once back,
        // with the acked seq the session resumed from.
        std::string carrier = upper(name(st));
        ImU32 carrierColor = stateColor(attached, connecting);
        {
            if (chat.carrierLost() && !attached) {
                carrier = "CARRIER LOST // " + carrier;
                carrierColor = Ink::fatal;
            } else if (attached) {
                carrier = "CARRIER";
                if (chat.resumedFrom()) {
                    carrier += " // RESUMED FROM " + std::to_string(*chat.resumedFrom());
                }
            }
        }
        row("STATE", carrier, carrierColor);
        row("URL", client.url(), Ink::secondary);
        if (!session.world().id.empty()) {
            row("WORLD", (session.world().name.empty() ? std::string("(unnamed)") : session.world().name) + "  " +
                             session.world().id.substr(0, 8),
                Ink::strong);
        }
        if (!session.freshReason().empty()) {
            row("FRESH", session.freshReason(), Ink::inspected);
        }
        {
            const std::string p = session.principal().value_or("-");
            row("PRINCIPAL", names_ && session.principal() ? names_->nameOf(p) + "  " + p : p);
        }
        row("SESSION", session.sessionId().empty() ? "-" : session.sessionId());
        row("HEAD", std::to_string(session.head()), Ink::strong);
        if (bootLine_) {
            row("BOOT", *bootLine_, bootLine_->rfind("snapshot", 0) == 0 ? Ink::nav : Ink::secondary);
        }
        if (linkLine_ && !linkLine_->empty()) {
            row("LINK", *linkLine_, linkStopped_ ? Ink::fatal : Ink::secondary);
        }
        row("ATTACHES",
            std::to_string(session.attaches()) + " (" + std::to_string(session.resumes()) + " resumed)");
        row("FRAMES", std::to_string(client.framesIn()) + " in / " + std::to_string(client.framesOut()) + " out");
        row("ROWS", std::to_string(store.rows().size()) +
                        (store.holes().empty() ? std::string() : " \u00b7 filtered " + std::to_string(store.holes().size())),
            Ink::primary);
        if (ImGui::IsItemHovered() && !store.holes().empty()) {
            ImGui::SetTooltip("%zu run(s) of seqs the world skipped: Events this Principal may not read (resolution filters "
                              "every push). Expected for a narrow Principal; not a loss.",
                              store.holes().size());
        }
        if (!store.gaps().empty()) {
            row("GAPS", std::to_string(store.gaps().size()) + " (the world's head fell below our cursor)", Ink::fatal);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("a different or truncated log under this URL (a --fresh world, a lost tail): the rows "
                                  "before seq %lld may not match it; forget the session to start clean",
                                  static_cast<long long>(store.gaps().back().got));
            }
        }
        std::string routeText = upper(name(chat.routeState()));
        if (chat.routeState() == chat::RouteState::Found) {
            routeText = names_ ? names_->nameOf(chat.route()) + "  " + chat.route() : chat.route();
        } else if (chat.routeState() == chat::RouteState::None) {
            routeText = "NO ROUTE (retrying)";
        }
        row("ROUTE", routeText, chat.routeState() == chat::RouteState::Found ? Ink::nav : Ink::secondary);
        row("ERROR", session.lastError().empty() ? "-" : session.lastError(),
            session.lastError().empty() ? Ink::secondary : Ink::inspected);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    dimText("SUBSCRIPTIONS");
    for (const auto& [subName, s] : session.subs()) {
        text(s.subscribed ? Ink::nav : Ink::secondary, subName);
        ImGui::SameLine();
        dimText("window %lld as-of %lld", static_cast<long long>(s.window), static_cast<long long>(s.asOf));
        ImGui::Indent();
        const float window = s.window > 0 ? static_cast<float>(s.window) : 1.0f;
        dimText("CURSOR ");
        ImGui::SameLine(0.0f, 0.0f);
        text(Ink::primary, seqText(s.cursor));
        ImGui::SameLine();
        dimText("ACKED ");
        ImGui::SameLine(0.0f, 0.0f);
        text(Ink::primary, seqText(s.acked));
        dimText("UNACKED ");
        ImGui::SameLine(0.0f, 0.0f);
        slantBar(static_cast<float>(s.unacked()) / window, 12);
        ImGui::SameLine();
        text(Ink::primary, std::to_string(s.unacked()));
        dimText("LAG     ");
        ImGui::SameLine(0.0f, 0.0f);
        slantBar(static_cast<float>(s.lag()) / window, 12, s.lagging ? Ink::inspected : Ink::primary);
        ImGui::SameLine();
        text(s.lagging ? Ink::inspected : (s.lag() > 0 ? Ink::primary : Ink::secondary),
             std::to_string(s.lag()) + (s.lagging ? " LAGGING" : ""));
        ImGui::Unindent();
    }
    // The last 200 listener log lines to the clipboard, for a bug report.
    if (bracketButtonNamed("copy log", "copy-log")) {
        ImGui::SetClipboardText(log::recent(200).c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("copy the last 200 listener log lines (what stderr printed) to the clipboard");
    }
    ImGui::PopFont();
}

void SessionPane::drawBinds(const std::vector<BindRow>& rows, ImFont* mono, bool canSend, bool& sendRequested) {
    beginPane("BIND");
    drawBindsBody(rows, mono, canSend, sendRequested);
    endPane();
}

void SessionPane::drawBindsBody(const std::vector<BindRow>& rows, ImFont* mono, bool canSend,
                                bool& sendRequested) {
    touchDragScroll();
    ImGui::PushFont(mono, 0.0f);
    dimText("sprint 0 stubs: the host as a workspace and a shell (spec 15.6); opt-in, `unresolved` is the expected answer");
    if (bracketButton("send bind ops", canSend)) {
        sendRequested = true;
    }
    if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("NAME");
        ImGui::TableSetupColumn("OBJECT");
        ImGui::TableSetupColumn("RESULT");
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < 3; ++c) {
            ImGui::TableSetColumnIndex(c);
            dimText("%s", ImGui::TableGetColumnName(c));
        }
        for (const BindRow& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.plan.name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", r.plan.label.c_str());
            }
            ImGui::TableNextColumn();
            text(Ink::nav, names_ ? names_->nameOf(r.plan.object) : r.plan.object);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", r.plan.object.c_str());
            }
            ImGui::TableNextColumn();
            if (!r.sent) {
                dimText("not sent");
            } else if (!r.response) {
                dimText("waiting");
            } else if (r.response->ok) {
                text(Ink::primary,
                     "OK SEQ " + std::to_string(r.response->result.value("seq", r.response->seq)));
            } else {
                // An err here is the expected sprint-0 answer, shown dim, never red.
                text(Ink::secondary, "err " + (r.response->err ? r.response->err->code : std::string("?")));
                if (ImGui::IsItemHovered() && r.response->err) {
                    ImGui::SetTooltip("%s", r.response->err->message.c_str());
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
}

} // namespace ke::ui
