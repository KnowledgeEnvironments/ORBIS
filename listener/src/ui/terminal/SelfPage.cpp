// A person's own page. A person is the identity and the authority of a human in the world, not
// someone to talk to, so this has no input line: it shows what is waiting on them and what is
// addressed to them, and their Messages stay readable under `messages`.
#include "chat/Notifications.hpp"
#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

namespace {

constexpr std::size_t kRows = 8;

std::string clockOf(const std::string& at) { return at.size() >= 16 ? at.substr(11, 5) : at; }

std::string firstLine(const std::string& s) { return s.substr(0, s.find('\n')); }

void section(const std::string& title) {
    ImGui::Spacing();
    text(Ink::nav, upper(title));
    ImGui::Separator();
}

// The sender column of one section: as wide as its longest name, at most 28 columns, and a gap.
// A fixed column let `bench-orbis-b1-glm5` run under the text beside it (2026-11-05).
float whoColumn(const std::vector<std::string>& names) {
    const float ch = ImGui::CalcTextSize("0").x;
    float w = ch * 4.0f;
    for (const std::string& n : names) {
        w = std::max(w, ImGui::CalcTextSize(n.c_str()).x);
    }
    return std::min(w, ch * 28.0f) + ch * 2.0f;
}

// `s` cut to `width`, ending in an ellipsis when it had to be cut; never inside a UTF-8 sequence.
std::string fitted(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) {
        return s;
    }
    std::string out = s;
    while (!out.empty() && ImGui::CalcTextSize((out + "\xe2\x80\xa6").c_str()).x > width) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            out.pop_back();
        }
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0xC0) {
            out.pop_back(); // a lead byte whose continuation went
        }
    }
    return out + "\xe2\x80\xa6";
}

// One row a person can follow: a clock, who, a line that clips at the pane's edge.
bool row(const std::string& target, const std::string& at, const std::string& who, const std::string& line, float whoW) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::PushID(target.c_str());
    const bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap);
    drive::hit(target, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec2 end = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ch = ImGui::CalcTextSize("0").x;
    const std::string shown = fitted(who, whoW - ch * 2.0f);
    dl->PushClipRect(start, end, true);
    dl->AddText(start, Ink::secondary, clockOf(at).c_str());
    dl->AddText(ImVec2(start.x + ch * 7.0f, start.y), Ink::nav, shown.c_str());
    dl->AddText(ImVec2(start.x + ch * 7.0f + whoW, start.y), Ink::primary, line.c_str());
    dl->PopClipRect();
    if (hovered && shown != who) {
        ke::ui::tip("%s", who.c_str());
    }
    return clicked;
}

struct Row {
    std::string target, at, who, line;
};

} // namespace

void TerminalPane::drawSelfPage(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals,
                                const model::Evaluations& evals, chat::Chat& chat, const Session& session, const Anim& anim,
                                ImFont* mono) {
    if (inlineTabs_) {
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    }
    static const char* const kViews[] = {"you", "messages", "devices", "fields"};
    tab.selfView = subTabs("self-views", kViews, 4, tab.selfView);
    switch (tab.selfView) {
    case 1:
        drawTranscript(tab, info, principals, evals, chat, session, anim, mono);
        break;
    case 2:
        drawDevices(tab, info, principals, chat, session, mono);
        break;
    case 3:
        drawFields(tab, info, principals, chat, session);
        break;
    default:
        drawSelfSummary(tab, info, principals, chat, session);
        break;
    }
}

void TerminalPane::drawSelfSummary(Tab&, const model::PrincipalInfo* info, model::Principals& principals, chat::Chat& chat,
                                   const Session& session) {
    const std::string self = session.principal().value_or("");
    ImGui::BeginChild("self-page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    text(Ink::strong, principals.nameOf(self));
    ImGui::PushTextWrapPos(0.0f);
    dimText("This is you. What you send reaches agents from this name, and approvals, gates and unsafe mode are yours alone.");
    ImGui::PopTextWrapPos();
    for (const auto& [name, f] : info->fields) { // what a person wrote about themselves: Fields of no namespace
        if (name.find('/') == std::string::npos && f.value.is_string()) {
            text(Ink::secondary, name);
            ImGui::SameLine();
            text(Ink::primary, f.value.get<std::string>());
        }
    }

    std::vector<const model::TaskInfo*> waiting;
    if (tasks_) {
        for (const model::TaskInfo* t : tasks_->pending()) {
            if (t->approvers.empty() || std::find(t->approvers.begin(), t->approvers.end(), self) != t->approvers.end()) {
                waiting.push_back(t);
            }
        }
    }
    section("waiting on you");
    if (waiting.empty()) {
        dimText("nothing needs your approval");
    }
    {
        std::vector<Row> rows;
        for (std::size_t i = 0; i < waiting.size() && i < kRows; ++i) {
            const model::TaskInfo& t = *waiting[i];
            rows.push_back({"self-approval:" + chat::idPrefix(t.id), "", principals.nameOf(t.requestedBy), firstLine(t.summary)});
        }
        std::vector<std::string> names;
        for (const Row& r : rows) {
            names.push_back(r.who);
        }
        const float whoW = whoColumn(names);
        for (const Row& r : rows) {
            if (row(r.target, r.at, r.who, r.line, whoW) && onApprovals) {
                onApprovals();
            }
        }
    }
    if (bracketButtonNamed("approvals", "self-approvals") && onApprovals) {
        onApprovals();
    }

    section("addressed to you");
    std::set<std::string> seen;
    std::size_t shown = 0;
    std::vector<Row> addressed;
    std::vector<std::string> senders;
    const std::vector<model::MessageRef>& messages = principals.messages();
    for (auto it = messages.rbegin(); it != messages.rend() && shown < kRows; ++it) {
        if (it->recipient != self || it->from == self || !seen.insert(it->message).second) {
            continue;
        }
        const chat::MessageRow* m = chat.messages().get(it->message);
        if (m == nullptr || !m->kind.empty()) {
            continue; // approval cards are above, and a Message still being read shows next frame
        }
        ++shown;
        addressed.push_back({"self-message:" + chat::idPrefix(it->message), it->at, it->from, firstLine(m->text)});
        senders.push_back(principals.nameOf(it->from));
    }
    {
        const float whoW = whoColumn(senders);
        for (const Row& r : addressed) {
            if (row(r.target, r.at, principals.nameOf(r.who), r.line, whoW)) {
                open(r.who, 0); // `who` holds the sender's id here: a click opens that chat
            }
        }
    }
    if (shown == 0) {
        dimText("nothing yet: what agents say to you lands here");
    }

    section("notices");
    std::size_t notes = 0;
    if (notices_ != nullptr) {
        std::vector<Row> rows;
        std::vector<std::string> names;
        for (const chat::Notification& n : notices_->rows()) {
            if (notes == kRows) {
                break;
            }
            ++notes;
            rows.push_back({"self-notice:" + chat::idPrefix(n.id), n.at, principals.nameOf(n.from), firstLine(n.text)});
            names.push_back(rows.back().who);
        }
        const float whoW = whoColumn(names);
        for (const Row& r : rows) {
            if (row(r.target, r.at, r.who, r.line, whoW) && onNotices) {
                onNotices();
            }
        }
    }
    if (notes == 0) {
        dimText("no notices");
    }
    if (bracketButtonNamed("notices", "self-notices") && onNotices) {
        onNotices();
    }
    ImGui::EndChild();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace ke::ui
