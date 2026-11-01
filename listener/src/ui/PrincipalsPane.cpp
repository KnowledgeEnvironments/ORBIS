#include "ui/Textures.hpp"
#include "ui/UnsafeView.hpp"
#include "ui/PrincipalsPane.hpp"

#include "ui/ActivityMark.hpp"

#include "drive/Commands.hpp"
#include "ui/SymbolUi.hpp"

namespace ke::ui {

namespace {
ImU32 ink(bool inverse, ImU32 color) {
    return inverse ? Ink::onSelection : color;
}
} // namespace

std::string PrincipalsPane::draw(const model::Principals& principals, const std::string& current, const Anim& anim, ImFont* mono) {
    std::string clicked;
    if (beginPane("PRINCIPALS")) {
        clicked = drawBody(principals, current, anim, mono);
    }
    endPane();
    return clicked;
}

void PrincipalsPane::drawMenu(const model::Principals& principals, const std::string& id, const std::string& route, std::string& clicked) {
    if (ImGui::BeginPopupContextItem("principal-menu")) {
        const bool inspect = menuItem("INSPECT");
        drive::hit("menu:inspect", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (inspect) {
            LinkRequest r;
            r.kind = LinkRequest::Inspect;
            r.object = id;
            symbolUi().ask(std::move(r));
        }
        ImGui::Separator();
        if (menuItem("COPY ID")) {
            ImGui::SetClipboardText(id.c_str());
        }
        if (menuItem("COPY NAME")) {
            ImGui::SetClipboardText(principals.nameOf(id).c_str());
        }
        if (const model::PrincipalInfo* who = principals.get(id); who && who->cls == "principal" && who->drivenBy != "person") {
            const bool go = menuItem("OFF THE RECORD");
            drive::hit("menu:off-the-record", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (go) {
                LinkRequest r;
                r.kind = LinkRequest::Scratch;
                r.object = id;
                symbolUi().ask(std::move(r));
            }
        }
        if (!route.empty()) {
            if (menuItem(("OPEN ROUTE " + principals.nameOf(route)).c_str())) {
                clicked = route;
            }
            drive::hit("menu:open-route", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        const std::string blocked = model::deleteBlocked(principals, id);
        const bool del = menuItem("DELETE", nullptr, false, blocked.empty());
        if (blocked.empty()) {
            drive::hit("menu:delete", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        if (del) {
            LinkRequest r;
            r.kind = LinkRequest::Delete;
            r.object = id;
            symbolUi().ask(std::move(r));
        }
        if (!blocked.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ke::ui::tip("%s", blocked.c_str());
        }
        ImGui::EndPopup();
    }
}

void PrincipalsPane::drawActor(const model::Principals& principals, const model::TreeRoot& root, const std::string& current,
                               const Anim& anim, std::string& clicked, int& index) {
    const model::PrincipalInfo* p = principals.get(root.id);
    const std::string name = principals.nameOf(root.id);
    const bool inverse = root.id == current;
    const bool open = expanded.count(root.id) != 0;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(index++);
    if (ImGui::Selectable("##row", inverse, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
        clicked = root.id;
    }
    drive::hit("principal:" + name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        LinkRequest r;
        r.kind = LinkRequest::Label;
        r.object = root.id;
        symbolUi().ask(std::move(r));
    }
    if (ImGui::IsItemHovered()) {
        std::string tip = name + "\nbound name " + principals.boundName(root.id) + "  id " + root.id;
        if (const std::string what = principals.describe(root.id, p ? p->cls : std::string()); !what.empty()) {
            tip += "\n" + what;
        }
        if (p && evals_) {
            if (const std::string totals = model::totalsLine(evals_->totals(root.id)); !totals.empty()) {
                tip += "\n" + totals;
            }
        }
        ke::ui::tip("%s", tip.c_str());
    }
    drawMenu(principals, root.id, "", clicked);
    ImGui::SameLine(0.0f, 0.0f);
    if (!root.lines.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ink(inverse, Ink::secondary));
        if (disclosure(open, "fold")) {
            if (open) {
                expanded.erase(root.id);
            } else {
                expanded.insert(root.id);
            }
            prefsDirty_ = true;
        }
        ImGui::PopStyleColor();
        drive::hit("tree-toggle:" + name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
    }
    if (p != nullptr && p->cls == "adapter") {
        textures::assetIcon("server-96.png");
    }
    text(ink(inverse, Ink::primary), name);
    if (p != nullptr && p->field("principal/ephemeral") != nullptr) {
        ImGui::SameLine();
        text(ink(inverse, Ink::secondary), "temp");
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("temporary: retired when its Task ends or its time runs out; its record stays on the log");
        }
    }
    if (isUnsafe(root.id)) {
        const float mark = std::floor(ImGui::GetTextLineHeight() * 0.4f);
        const ImVec2 end = ImGui::GetItemRectMax();
        unsafeMark(ImGui::GetWindowDrawList(), ImVec2(end.x + 4.0f, end.y - mark * 1.8f), mark);
    }
    ImGui::TableNextColumn();
    text(ink(inverse, Ink::secondary), root.role);
    ImGui::TableNextColumn();
    if (p) {
        drawLoopChips(principals, *p, inverse, anim);
    }
    ImGui::PopID();
    if (!open) {
        return;
    }
    const float ch = ImGui::CalcTextSize("0").x;
    for (const model::TreeLine& line : root.lines) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(index++);
        {
            // The connector, from the parent's expand glyph to this row's label.
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float pad = ImGui::GetStyle().CellPadding.y;
            treeGuide(ImGui::GetWindowDrawList(), at.x + ch * 0.5f, at.y - pad, at.y + ImGui::GetTextLineHeight() + pad, at.x + ch * 1.7f,
                      &line == &root.lines.back());
        }
        const float rowRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x; // the cell clips past here
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ch * 2.0f);
        const float labelEnd = ImGui::GetCursorPosX() + ch * 10.0f;
        text(Ink::secondary, line.label);
        ImGui::SameLine(labelEnd);
        bool first = true;
        for (const model::TreeItem& item : line.items) {
            const std::string itemName = principals.nameOf(item.id);
            const float w = ImGui::CalcTextSize((itemName + ",").c_str()).x;
            if (!first) {
                text(Ink::secondary, ",");
                // A long line wraps under its names, never under the label.
                if (ImGui::GetItemRectMax().x + ch + w < rowRight) {
                    ImGui::SameLine();
                } else {
                    ImGui::SetCursorPosX(labelEnd);
                }
            }
            first = false;
            ImGui::PushID(item.id.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, Ink::nav);
            if (ImGui::Selectable(itemName.c_str(), false, 0, ImGui::CalcTextSize(itemName.c_str()))) {
                clicked = item.id;
            }
            ImGui::PopStyleColor();
            drive::hit("tree:" + name + ":" + itemName, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::IsItemHovered()) {
                std::string tip = principals.describe(item.id, "");
                if (!item.route.empty()) {
                    tip += (tip.empty() ? "" : "\n") + ("through " + principals.nameOf(item.route));
                }
                if (!item.others.empty()) {
                    tip += (tip.empty() ? std::string() : "\n") + "also used by";
                    for (const std::string& o : item.others) {
                        tip += " " + principals.nameOf(o);
                    }
                }
                if (!tip.empty()) {
                    ke::ui::tip("%s", tip.c_str());
                }
            }
            drawMenu(principals, item.id, item.route, clicked);
            ImGui::PopID();
            ImGui::SameLine(0.0f, 0.0f);
        }
        ImGui::NewLine();
        ImGui::PopID();
    }
}

void PrincipalsPane::drawLoopChips(const model::Principals& principals, const model::PrincipalInfo& p, bool inverse, const Anim& anim) {
    const std::string& id = p.id;
    const model::LoopState ls = model::loopState(p);
    const model::FieldState* queued = p.field("loop/queue");
    const std::size_t waiting = queued && queued->value.is_array() ? queued->value.size() : 0;
    // The mark is the whole state: what the person should do about this agent, if anything.
    model::Activity mark = model::Activity::None;
    if (activity_ != nullptr) {
        if (auto it = activity_->find(id); it != activity_->end()) {
            mark = it->second;
        }
    }
    if (mark != model::Activity::None) {
        activityMarkItem(mark, ImGui::GetTime());
    }
    if (waiting > 0) {
        if (mark != model::Activity::None) {
            ImGui::SameLine();
        }
        chip(std::to_string(waiting) + " QUEUED", ink(inverse, Ink::inspected));
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("%zu message(s) queued for %s", waiting, principals.nameOf(id).c_str());
        }
    }
    if (tasks_) {
        if (const std::size_t n = tasks_->pendingRequestedBy(id); n > 0) {
            if (ls != model::LoopState::None) {
                ImGui::SameLine();
            }
            chip(std::to_string(n) + " TO APPROVE", ink(inverse, Ink::inspected));
            if (ImGui::IsItemHovered()) {
                ke::ui::tip("%zu op(s) of %s wait for approval; open its chat for the cards", n,
                                  principals.nameOf(id).c_str());
            }
        }
    }
}

void PrincipalsPane::drawRow(const model::Principals& principals, const std::string& id, const std::string& current,
                             const Anim& anim, std::string& clicked, int& index) {
    const model::PrincipalInfo* p = principals.get(id);
    const model::RouteInfo* route = p ? nullptr : principals.route(id);
    const std::string cls = p ? p->cls : route ? std::string("route") : std::string();
    const bool inverse = id == current;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(index++);
    if (ImGui::Selectable("##row", inverse, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
        clicked = id;
    }
    drive::hit("principal:" + principals.nameOf(id), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        std::string tip = principals.nameOf(id) + "\n" + id;
        if (const std::string what = principals.describe(id, cls); !what.empty()) {
            tip += "\n" + what;
        } else if (!cls.empty()) {
            tip += "\n" + cls;
        }
        if (p) {
            if (evals_) {
                const std::string totals = model::totalsLine(evals_->totals(id));
                if (!totals.empty()) {
                    tip += "\n" + totals;
                }
            }
            if (const std::string stalled = p->loopStalled(); !stalled.empty()) {
                tip += "\nstalled: " + stalled;
            }
        }
        ke::ui::tip("%s", tip.c_str());
    }
    drawMenu(principals, id, "", clicked);
    ImGui::SameLine(0.0f, 0.0f);
    text(ink(inverse, principals.hasName(id) ? Ink::primary : Ink::secondary), principals.nameOf(id));
    ImGui::TableNextColumn();
    text(ink(inverse, Ink::secondary), cls);
    ImGui::TableNextColumn();
    if (p) {
        text(ink(inverse, p->drivenBy == "process" ? Ink::nav : Ink::secondary),
             p->field("principal/ephemeral") != nullptr ? p->drivenBy + " temp" : p->drivenBy);
    }
    ImGui::TableNextColumn();
    if (p) {
        text(ink(inverse, Ink::secondary), std::to_string(p->routes().size()));
    }
    ImGui::TableNextColumn();
    if (p) {
        drawLoopChips(principals, *p, inverse, anim);
    }
    ImGui::TableNextColumn();
    if (p && evals_) {
        const model::Totals t = evals_->totals(id);
        const std::string total = model::totalsShort(t);
        // right-aligned: numbers line up down the column
        const float w = ImGui::CalcTextSize(total.c_str()).x;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (w < avail) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
        }
        text(ink(inverse, Ink::secondary), total);
        if (ImGui::IsItemHovered() && t.evaluations > 0) {
            ke::ui::tip("%s", model::totalsLine(t).c_str());
        }
    }
    ImGui::PopID();
}

std::string PrincipalsPane::drawBody(const model::Principals& principals, const std::string& current,
                                     const Anim& anim, ImFont* mono) {
    std::string clicked;
    ImGui::PushFont(mono, 0.0f);
    const auto rows = principals.ordered();
    const model::ObjectTree tree = flat ? model::ObjectTree{} : model::buildObjectTree(principals, self);
    dimText("%zu objects // fold at %lld", rows.size(), static_cast<long long>(principals.applied()));
    if (plusAtRight("new-principal", "add a Principal: charter, Route, Model and a narrower Grant")) {
        newRequested_ = true;
    }
    if (bracketButtonNamed("flat", "tree-flat", true, flat)) {
        flat = !flat;
        prefsDirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
        ke::ui::tip("one flat table instead of the tree");
    }
    ImGui::SameLine();
    if (bracketButtonNamed("show deleted", "show-deleted", true, showDeleted_)) {
        showDeleted_ = !showDeleted_;
        if (showDeleted_) {
            retiredRequested_ = true;
        }
    }
    ImGui::SameLine();
    bracketButtonNamed("?", "tree-legend");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        for (const auto& [word, meaning] : model::treeLegend()) {
            text(Ink::strong, word);
            ImGui::SameLine(ImGui::CalcTextSize("0").x * 12.0f);
            text(Ink::secondary, meaning);
        }
        ImGui::EndTooltip();
    }
    // FLAT: fixed columns with horizontal scroll, so the name is never squeezed out by the counters.
    const float ch = ImGui::CalcTextSize("0").x;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  (flat ? ImGuiTableFlags_ScrollX : 0) |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("principals", flat ? 6 : 3, flags)) {
        if (flat) {
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthFixed, ch * 24);
            ImGui::TableSetupColumn("CLASS", ImGuiTableColumnFlags_WidthFixed, ch * 9);
            ImGui::TableSetupColumn("DRIVEN", ImGuiTableColumnFlags_WidthFixed, ch * 7);
            ImGui::TableSetupColumn("RT", ImGuiTableColumnFlags_WidthFixed, ch * 2);
            ImGui::TableSetupColumn("LOOP", ImGuiTableColumnFlags_WidthFixed, ch * 22);
            ImGui::TableSetupColumn("TOTAL", ImGuiTableColumnFlags_WidthFixed, ch * 24);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int c = 0; c < 6; ++c) {
                ImGui::TableSetColumnIndex(c);
                dimText("%s", ImGui::TableGetColumnName(c));
            }
        } else {
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ROLE", ImGuiTableColumnFlags_WidthFixed, ch * 6);
            ImGui::TableSetupColumn("LOOP", ImGuiTableColumnFlags_WidthFixed, ch * 22);
        }
        touchDragScroll();
        int index = 0;
        // First while it is on: asking for the deleted means wanting to see them.
        if (showDeleted_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            dimText("DELETED %zu", principals.retiredObjects().size());
            for (const auto& [id, r] : principals.retiredObjects()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(index++);
                text(Ink::secondary, "  " + (r.name.empty() ? id.substr(0, 8) : r.name));
                ImGui::TableNextColumn();
                text(Ink::secondary, r.cls);
                ImGui::TableNextColumn();
                if (bracketButtonNamed("restore", "restore:" + (r.name.empty() ? id.substr(0, 8) : r.name))) {
                    LinkRequest q;
                    q.kind = LinkRequest::Restore;
                    q.object = id;
                    symbolUi().ask(std::move(q));
                }
                ImGui::PopID();
            }
        }
        if (flat) {
            for (const model::PrincipalInfo* p : rows) {
                drawRow(principals, p->id, current, anim, clicked, index);
            }
        } else {
            for (const model::TreeRoot& root : tree.roots) {
                drawActor(principals, root, current, anim, clicked, index);
            }
            if (!tree.unattached.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool open = expanded.count("unattached") != 0;
                ImGui::PushStyleColor(ImGuiCol_Text, Ink::secondary);
                if (disclosure(open, "unattached")) {
                    if (open) {
                        expanded.erase("unattached");
                    } else {
                        expanded.insert("unattached");
                    }
                    prefsDirty_ = true;
                }
                ImGui::PopStyleColor();
                drive::hit("tree-toggle:unattached", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                ImGui::SameLine();
                dimText("UNATTACHED %zu", tree.unattached.size());
                if (open) {
                    for (const std::string& id : tree.unattached) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(index++);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ch * 2.0f);
                        const std::string itemName = principals.nameOf(id);
                        ImGui::PushStyleColor(ImGuiCol_Text, Ink::nav);
                        if (ImGui::Selectable(itemName.c_str(), false, 0, ImGui::CalcTextSize(itemName.c_str()))) {
                            clicked = id;
                        }
                        ImGui::PopStyleColor();
                        drive::hit("tree:unattached:" + itemName, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                        drawMenu(principals, id, "", clicked);
                        ImGui::PopID();
                    }
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    return clicked;
}

} // namespace ke::ui
