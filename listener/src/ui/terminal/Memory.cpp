#include "ui/terminal/Internal.hpp"

namespace ke::ui {

using namespace term;

// The world's defaults for a Principal's memory (sprint 1 item W5; the world applies the same
// four when it creates a Principal). Shown verbatim so a person sees what CREATE DEFAULTS writes.
constexpr const char* kMemoryPolicyDefault =
    "At the end of a wake, if you learned something durable about this world, the people in it, or a project, "
    "update memory/notes; keep it under memory/budget characters; prefer facts with seqs.";
constexpr double kMemoryBudgetDefault = 4000;
constexpr double kMemoryRetentionDefault = 30;

void TerminalPane::drawMemory(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals,
                              chat::Chat& chat, ImFont* mono) {
    ImGui::BeginChild("memory", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    touchDragScroll();
    if (!info) {
        dimText("not in the fold yet");
        ImGui::EndChild();
        return;
    }
    const float lineH = ImGui::GetTextLineHeight();
    const float ch = ImGui::CalcTextSize("0").x;
    std::vector<const model::FieldState*> entries;
    for (const auto& [name, f] : info->fields) {
        if (name.rfind("memory/", 0) == 0 && !name.ends_with("/hint") && !name.ends_with("/comment")) {
            entries.push_back(&f);
        }
    }
    // what is forgotten sinks to the end; it stays listed, with its history on the log
    std::stable_sort(entries.begin(), entries.end(), [](const model::FieldState* a, const model::FieldState* b) {
        const auto blank = [](const model::FieldState* f) {
            return f->value.is_null() || (f->value.is_string() && f->value.get<std::string>().empty());
        };
        return !blank(a) && blank(b);
    });
    auto sizeOf = [](const json& v) -> std::size_t {
        return v.is_string() ? v.get<std::string>().size() : v.is_null() ? 0 : v.dump().size();
    };
    if (entries.empty()) {
        tab.newMemory.open = true; // nothing here yet: the form is the page
        dimText("no memory Fields yet on %s", principals.nameOf(tab.principal).c_str());
        dimText("memory/* is what a Principal keeps between wakes; the serializer always includes all of it");
        FieldEdit& fresh = tab.edits["##memory-defaults"];
        if (bracketButton("create defaults")) {
            setField(tab, chat, "memory/notes", json("string"), "both", json(""), fresh);
            setField(tab, chat, "memory/policy", json("string"), "both", json(kMemoryPolicyDefault), fresh);
            setField(tab, chat, "memory/budget", json("number"), "both", json(kMemoryBudgetDefault), fresh);
            setField(tab, chat, "memory/retention-days", json("number"), "both", json(kMemoryRetentionDefault), fresh);
        }
        drive::hit("button:create", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("set-field memory/notes \"\", memory/policy (the world's default text), memory/budget %g, "
                              "memory/retention-days %g, reader both, as you",
                              kMemoryBudgetDefault, kMemoryRetentionDefault);
        }
        if (!fresh.result.empty()) {
            ImGui::SameLine();
            text(fresh.resultErr ? Ink::inspected : Ink::connected, fresh.result);
        }
    } else {
        // Everything counts: the serializer includes every memory/* Field, so the budget is theirs together.
        const model::FieldState* budget = info->field("memory/budget");
        const double budgetChars = budget && budget->value.is_number() ? budget->value.get<double>() : kMemoryBudgetDefault;
        std::size_t used = 0;
        for (const model::FieldState* f : entries) {
            used += sizeOf(f->value);
        }
        dimText("%zu fields // %zu / %g chars ", entries.size(), used, budgetChars);
        ImGui::SameLine(0.0f, 0.0f);
        const float frac = budgetChars > 0 ? static_cast<float>(static_cast<double>(used) / budgetChars) : 0.0f;
        slantBar(std::min(frac, 1.0f), 24, frac > 1.0f ? Ink::inspected : (frac > 0.8f ? Ink::inspected : Ink::primary));
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("every memory/* Field against memory/budget; the prompt always carries all of them");
        }
        ImGui::SameLine(contentRight() - squareSide());
        if (squareButton(tab.newMemory.open ? "x" : "+", "new-memory", true, tab.newMemory.open)) {
            tab.newMemory.open = !tab.newMemory.open;
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("add a memory Field");
        }
        ImGui::Separator();
    }
    drawNewField(tab, tab.newMemory, info, chat, "memory/", "new-memory");
    ImGui::Separator();
    for (const model::FieldState* fp : entries) {
        const model::FieldState& f = *fp;
        const std::string& name = f.name;
        const std::string value = editText(f.value);
        FieldEdit& edit = tab.edits[name];
        const bool empty = f.value.is_null() || (f.value.is_string() && f.value.get<std::string>().empty());
        ImGui::PushID(name.c_str());
        if (edit.basedOn != f.seq && !edit.editing) {
            edit.buffer = value;
            edit.basedOn = f.seq;
        }
        text(Ink::inspected, name);
        ImGui::SameLine();
        text(Ink::secondary, "// " + std::string(typeName(f.type)) + " // " + std::to_string(sizeOf(f.value)) + " chars // by " +
                               principals.nameOf(f.by) + " // " + clockPart(f.at) + " // seq " + std::to_string(f.seq));
        const bool hoveredMeta = ImGui::IsItemHovered();
        drawFieldNews(*info, f, principals);
        if (hoveredMeta && !f.previous.is_null() && detailShown(name + " has a previous value")) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ch * 72);
            const std::string before = editText(f.previous);
            text(Ink::secondary, "previous");
            ImGui::TextUnformatted(before.empty() ? "(empty)" : before.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        // The same row as FIELDS and INSTRUCTIONS: controls at the right, the value below.
        const bool tall = tallField(f);
        const auto forget = [&] {
            const json type = f.type.is_null() ? json("string") : f.type;
            const std::string kind = scalarName(type);
            const json blank = kind == "number"    ? json(0)
                               : kind == "boolean" ? json(false)
                               : kind == "list"    ? json::array()
                               : kind == "map"     ? json::object()
                                                   : json("");
            setField(tab, chat, name, type, f.reader.empty() ? "both" : f.reader, blank, edit);
        };
        if (!edit.editing) {
            rowAtRight(tall && !empty ? bracketRowWidth({"edit", "more", "clear"})
                       : tall         ? bracketRowWidth({"edit", "more"})
                       : !empty       ? bracketRowWidth({"edit", "clear"})
                                      : bracketRowWidth({"edit"}));
            const bool pressed = bracketButton("edit");
            drive::hit("button:edit:" + name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (pressed && tall) {
                editInPane(tab, name);
            } else if (pressed) {
                edit.editing = true;
                edit.buffer = value;
                edit.basedOn = f.seq;
                edit.result.clear();
            }
            if (tall) {
                ImGui::SameLine();
                if (bracketButtonNamed(edit.expanded ? "less" : "more", "more:" + name)) {
                    edit.expanded = !edit.expanded;
                }
            }
            if (!empty) {
                ImGui::SameLine();
                if (bracketButtonNamed("clear", "forget:" + name)) {
                    if (ImGui::GetIO().KeyShift) {
                        forget();
                    } else {
                        tab.forgetting = name;
                        tab.confirmFocus = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ke::ui::tip("clears the value with a set-field; the history stays on the log\nasks first; shift-click clears at once");
                }
            }
        }
        if (edit.editing) {
            ImGui::PushStyleColor(ImGuiCol_Border, Ink::secondary);
            ImGui::InputTextMultiline("##v", &edit.buffer, ImVec2(-FLT_MIN, lineH * 6), ImGuiInputTextFlags_AllowTabInput);
            drive::hit("input:" + name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::PopStyleColor();
            dialogGap();
            if (bracketButton("set")) {
                const json type = f.type.is_null() ? json("string") : f.type;
                setField(tab, chat, name, type, f.reader.empty() ? "both" : f.reader, valueFor(type, edit.buffer), edit);
                edit.editing = false;
            }
            ImGui::SameLine();
            if (bracketButton("revert")) {
                edit.buffer = value;
                edit.editing = false;
                edit.result.clear();
            }
        } else {
            if (empty) {
                dimText("(empty)");
            } else if (tall) {
                const std::size_t cut = edit.expanded ? value.size() : foldAt(value, ImGui::GetContentRegionAvail().x / ImGui::CalcTextSize("0").x);
                drawRendered(edit, f.seq + (edit.expanded ? 0 : 1) * 1000000000LL,
                             cut < value.size() ? value.substr(0, cut) + " \xe2\x80\xa6" : value,
                             ImGui::GetContentRegionAvail().x, mono, name.c_str());
                dimText("%s", sizeLine(value).c_str());
            } else if (f.value.is_string()) {
                drawRendered(edit, f.seq, value, ImGui::GetContentRegionAvail().x, mono, name.c_str());
            } else {
                text(Ink::primary, value);
            }
            if (tab.forgetting == name) {
                text(Ink::inspected, "clear " + name + " of " + principals.nameOf(tab.principal) + "? " +
                                         std::to_string(sizeOf(f.value)) + " chars; the log keeps it");
                ImGui::SameLine();
                const Confirmed c = confirmRow("clear", "confirm-forget:" + name, "cancel-forget:" + name, tab.confirmFocus);
                if (c == Confirmed::Yes) {
                    forget();
                }
                if (c != Confirmed::Waiting) {
                    tab.forgetting.clear();
                }
            }
        }
        if (!edit.result.empty()) {
            ImGui::SameLine();
            text(edit.resultErr ? Ink::inspected : Ink::connected, edit.result);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    insetRim(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace ke::ui
