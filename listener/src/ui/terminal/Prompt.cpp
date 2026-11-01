#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

void TerminalPane::drawPromptView(const model::Principals& principals, ImFont* mono) {
    if (!promptView_) {
        return;
    }
    PromptView& v = *promptView_;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(ImGui::CalcTextSize("0").x * 100, ImGui::GetTextLineHeight() * 40), ImGuiCond_FirstUseEver);
    const std::string title = "PROMPT // " + principals.nameOf(v.principal) + "###prompt-view";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
        ImGui::PushFont(mono, 0.0f);
        drive::hit("pane:prompt-view", ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
        std::size_t total = 0;
        for (const chat::PromptSection& s : v.sections) {
            total += s.bytes();
        }
        dimText("serialization %s // %zu bytes // exactly what the model was sent", chat::idPrefix(v.serialization).c_str(), total);
        if (!v.status.empty()) {
            text(Ink::inspected, v.status);
        }
        int i = 0;
        for (chat::PromptSection& s : v.sections) {
            ImGui::PushID(i++);
            const std::string head = s.name + " // " + s.role + " // " + std::to_string(s.bytes()) + " bytes###section";
            ImGui::PushStyleColor(ImGuiCol_Text, Ink::inspected);
            const bool show = ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleColor();
            if (show) {
                const float h = ImGui::CalcTextSize(s.text.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x).y + ImGui::GetTextLineHeight();
                ImGui::InputTextMultiline("##text", &s.text, ImVec2(-FLT_MIN, std::min(h, ImGui::GetTextLineHeight() * 24)),
                                          ImGuiInputTextFlags_ReadOnly);
            }
            ImGui::PopID();
        }
        ImGui::PopFont();
    }
    ImGui::End();
    if (!open) {
        promptView_.reset();
    }
}

void TerminalPane::drawPrompt(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals,
                              chat::Chat& chat) {
    ImGui::BeginChild("prompt", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    touchDragScroll();
    if (!info) {
        dimText("principal not in the fold yet");
        ImGui::EndChild();
        return;
    }
    dimText("instruction fields of %s; [ EDIT ] opens a long one in the editor, [ SET ] writes a short one back",
            principals.nameOf(tab.principal).c_str());
    const float lineH = ImGui::GetTextLineHeight();
    int shown = 0;
    for (const auto& [name, f] : info->fields) {
        if (name.rfind("instructions/", 0) != 0) {
            continue;
        }
        ++shown;
        FieldEdit& edit = tab.edits[name];
        if (edit.basedOn != f.seq && !edit.editing) {
            edit.buffer = editText(f.value);
            edit.basedOn = f.seq;
        }
        ImGui::PushID(name.c_str());
        text(Ink::inspected, name);
        ImGui::SameLine();
        text(Ink::secondary, "// by " + principals.nameOf(f.by) + " // " + clockPart(f.at) + " // seq " +
                               std::to_string(f.seq) + (edit.basedOn != f.seq ? " (changed in the world)" : ""));
        if (info != nullptr) {
            drawFieldNews(*info, f, principals);
        }
        // Every Field row reads the same: what it is on one line, its controls at the right of that
        // line, its value below.
        const bool tall = tallField(f) && !edit.editing;
        if (!edit.editing) {
            rowAtRight(tall ? bracketRowWidth({"edit", "more"}) : bracketRowWidth({"edit"}));
            if (bracketButtonNamed("edit", "edit:" + name)) {
                if (tall) {
                    editInPane(tab, name);
                } else {
                    edit.editing = true;
                    edit.buffer = editText(f.value);
                    edit.basedOn = f.seq;
                }
            }
            if (tall) {
                ImGui::SameLine();
                if (bracketButtonNamed(edit.expanded ? "less" : "more", "more:" + name)) {
                    edit.expanded = !edit.expanded;
                }
            }
        }
        if (tall) {
            // The text belongs in the editor: here, enough to know which one it is.
            const std::string whole = f.value.get<std::string>();
            const std::size_t cut = edit.expanded ? whole.size() : foldAt(whole, ImGui::GetContentRegionAvail().x / ImGui::CalcTextSize("0").x);
            drawRendered(edit, f.seq + (edit.expanded ? 0 : 1) * 1000000000LL, cut < whole.size() ? whole.substr(0, cut) + " \xe2\x80\xa6" : whole,
                         ImGui::GetContentRegionAvail().x, ImGui::GetFont(), name.c_str());
            dimText("%s", sizeLine(whole).c_str());
        } else if (edit.editing) {
            ImGui::PushStyleColor(ImGuiCol_Border, Ink::secondary);
            ImGui::InputTextMultiline("##value", &edit.buffer, ImVec2(-FLT_MIN, lineH * 8), ImGuiInputTextFlags_AllowTabInput);
            ImGui::PopStyleColor();
            dialogGap();
            if (bracketButton("set")) {
                setField(tab, chat, name, f.type.is_null() ? json("string") : f.type, f.reader, valueFor(f.type, edit.buffer), edit);
                edit.basedOn = f.seq;
            }
            ImGui::SameLine();
            if (bracketButton("revert")) {
                edit.buffer = editText(f.value);
                edit.basedOn = f.seq;
                edit.editing = false;
                edit.result.clear();
            }
        } else {
            // Read-only: the instruction rendered as markdown, selectable; [ EDIT ] opens the raw text.
            drawRendered(edit, f.seq, f.value.is_string() ? f.value.get<std::string>() : f.value.dump(2),
                         ImGui::GetContentRegionAvail().x, ImGui::GetFont(), name.c_str());
        }
        if (!edit.result.empty()) {
            text(edit.resultErr ? Ink::inspected : Ink::connected, edit.result);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (shown == 0) {
        dimText("no instructions/* fields yet");
        ImGui::Separator();
    }
    if (!tab.newPrompt.seeded) {
        tab.newPrompt.seeded = true;
        tab.newPrompt.name = "instructions/";
    }
    dimText("add an instruction Field");
    ImGui::SameLine(contentRight() - squareSide());
    if (squareButton(tab.newPrompt.open ? "x" : "+", "new-prompt", true, tab.newPrompt.open)) {
        tab.newPrompt.open = !tab.newPrompt.open;
    }
    drawNewField(tab, tab.newPrompt, info, chat, "", "new-prompt");
    ImGui::EndChild();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace ke::ui
