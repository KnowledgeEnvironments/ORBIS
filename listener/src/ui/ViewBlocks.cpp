#include "ui/ViewBlocks.hpp"

#include "chat/MessageCache.hpp"
#include "drive/Commands.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace ke::ui {

namespace {

constexpr float kPad = 4.0f;

float titleHeight(float lineH) {
    return lineH + 6.0f;
}

void chevronAt(ImDrawList* dl, ImVec2 from, ImVec2 tip, ImU32 color, float size) {
    const float dx = tip.x - from.x, dy = tip.y - from.y;
    const float l = std::sqrt(dx * dx + dy * dy);
    if (l < 0.5f) {
        return;
    }
    const ImVec2 d(dx / l, dy / l);
    const ImVec2 n(-d.y, d.x);
    dl->AddLine(tip, ImVec2(tip.x - d.x * size + n.x * size, tip.y - d.y * size + n.y * size), color, 1.0f);
    dl->AddLine(tip, ImVec2(tip.x - d.x * size - n.x * size, tip.y - d.y * size - n.y * size), color, 1.0f);
}

std::string shortName(const chat::NameOf& nameOf, const std::string& ref) {
    if (nameOf) {
        return nameOf(ref);
    }
    return chat::idPrefix(ref);
}

// Rows the tree shows with the current fold state.
std::size_t visibleTreeRows(const chat::TreeNode& n, const std::set<std::string>& collapsed, const std::string& path, int depth) {
    std::size_t rows = depth == 0 && n.label.empty() ? 0 : 1;
    if (depth == 0 || !collapsed.count(path)) {
        std::size_t i = 0;
        for (const chat::TreeNode& c : n.children) {
            rows += visibleTreeRows(c, collapsed, path + "/" + std::to_string(i++), depth + 1);
        }
    }
    return rows;
}

// ---- title ------------------------------------------------------------------------------------

void drawTitle(ImDrawList* dl, const chat::ViewSpec& v, ViewUiState& st, ImVec2 min, float wrap, ImFont* mono, float lineH,
               const std::string& scope, const chat::NameOf& nameOf, const std::string& via, ViewActions& acts,
               bool expandable) {
    const float h = titleHeight(lineH);
    const float slant = h * 0.268f;
    const ImVec2 pts[4] = {ImVec2(min.x, min.y), ImVec2(min.x + wrap - 1.0f, min.y),
                           ImVec2(min.x + wrap - 1.0f - slant, min.y + h), ImVec2(min.x, min.y + h)};
    dl->AddConvexPolyFilled(pts, 4, Surface::hover);
    const float fs = ImGui::GetFontSize();
    float x = min.x + kPad;
    const float ty = min.y + 3.0f;
    // kind chip, inverse on the bar: black on amber
    const std::string chipText = "[" + upper(v.kind.empty() ? "view" : v.kind) + "]";
    const ImVec2 cs = mono->CalcTextSizeA(fs, FLT_MAX, 0.0f, chipText.c_str());
    dl->AddRectFilled(ImVec2(x - 1.0f, ty - 1.0f), ImVec2(x + cs.x + 1.0f, ty + cs.y + 1.0f), viewKindColor(v.kind));
    dl->AddText(mono, fs, ImVec2(x, ty), Ink::onSelection, chipText.c_str());
    x += cs.x + 2.0f * kPad;
    const std::string title = v.title.empty() ? "untitled view" : v.title;
    dl->AddText(mono, fs, ImVec2(x, ty), Ink::strong, title.c_str());
    x += mono->CalcTextSizeA(fs, FLT_MAX, 0.0f, title.c_str()).x + 2.0f * kPad;
    if (v.truncated) {
        dl->AddText(mono, fs, ImVec2(x, ty), Ink::inspected, "(truncated)");
        x += mono->CalcTextSizeA(fs, FLT_MAX, 0.0f, "(truncated)").x + 2.0f * kPad;
    }
    // about chips and [ EXPAND ] are real items on the bar (clickable), placed by cursor. A bar
    // scrolled out of the visible window gets no items: ImGui cannot hover them and the drive
    // would otherwise be handed an off-screen target.
    if (!ImGui::IsRectVisible(min, ImVec2(min.x + wrap, min.y + h))) {
        return;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, min.y + 1.0f));
    ImGui::PushID(scope.c_str());
    if (!v.about.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Ink::onSelection);
        ImGui::TextUnformatted("about:");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, kPad);
        int i = 0;
        for (const std::string& a : v.about) {
            ImGui::PushID(i++);
            const std::string label = "[" + shortName(nameOf, a) + "]";
            ImGui::PushStyleColor(ImGuiCol_Text, Ink::nav);
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\nclick: open", a.c_str());
            }
            if (ImGui::IsItemClicked()) {
                acts.openRef = a;
            }
            ImGui::PopID();
            ImGui::SameLine(0.0f, kPad);
        }
    }
    if (!via.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Ink::onSelection);
        ImGui::TextUnformatted(("[via " + chat::idPrefix(via) + "]").c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, kPad);
    }
    if (expandable) {
        const float bw = ImGui::CalcTextSize("[ EXPAND ]").x;
        ImGui::SetCursorScreenPos(ImVec2(min.x + wrap - slant - bw - 2.0f * kPad, min.y + 1.0f));
        if (bracketButtonNamed("expand", "expand:" + chat::idPrefix(v.id), true, false, Ink::onSelection)) {
            if (v.kind == "graph") {
                acts.expandGraph = true;
            } else {
                st.expanded = !st.expanded;
            }
            acts.changed = true;
        }
    } else {
        ImGui::NewLine();
    }
    ImGui::PopID();
}

// ---- table ------------------------------------------------------------------------------------

void drawTableBody(const chat::ViewSpec& v, ViewUiState& st, ImVec2 min, ImVec2 max, ImFont* mono, const std::string& scope,
                   const chat::NameOf& nameOf, ViewActions& acts, bool ownWindow) {
    chat::TableModel& t = st.table;
    if (t.columns.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(min.x + kPad, min.y + kPad));
        text(Ink::secondary, "empty table");
        return;
    }
    if (!ownWindow) {
        ImGui::SetCursorScreenPos(min);
        ImGui::BeginChild((scope + "/tbl").c_str(), ImVec2(max.x - min.x, max.y - min.y), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar * 0);
        drive::hit("view:table:" + chat::idPrefix(v.id), min, max);
    }
    const int ncols = static_cast<int>(std::min<std::size_t>(t.columns.size(), 64));
    ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                            ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SortTristate;
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Surface::background);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Surface::hover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Surface::selection);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, Ink::secondary);
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, Ink::secondary);
    if (ImGui::BeginTable((scope + "/t").c_str(), ncols, flags)) {
        // Column widths from the content: the header and the widest cell of the first rows, so a
        // header is never clipped and a column does not jump as rows scroll in.
        for (int c = 0; c < ncols; ++c) {
            const std::size_t ci = static_cast<std::size_t>(c);
            float w = ImGui::CalcTextSize((t.columns[ci] + " v").c_str()).x;
            const std::size_t sample = std::min<std::size_t>(t.order.size(), 200);
            for (std::size_t i = 0; i < sample; ++i) {
                const json& cell = t.rows[t.order[i]][ci];
                const std::string ref = chat::TableModel::cellRef(cell);
                const std::string txt = ref.empty() ? chat::TableModel::cellText(cell)
                                        : (cell.is_object() && cell.contains("name") && cell["name"].is_string() ? cell["name"].get<std::string>()
                                                                                                                : shortName(nameOf, ref));
                w = std::max(w, ImGui::CalcTextSize(txt.c_str()).x);
            }
            w = std::min(w, ImGui::CalcTextSize("0").x * 48.0f) + 2.0f * kPad;
            ImGui::TableSetupColumn(t.columns[ci].c_str(), ImGuiTableColumnFlags_WidthFixed, w, static_cast<ImGuiID>(c + 1));
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        // Header row drawn by hand so the hovered header reads inverse (black on phosphor).
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < ncols; ++c) {
            ImGui::TableSetColumnIndex(c);
            ImGui::PushID(c);
            const char* name = t.columns[static_cast<std::size_t>(c)].c_str();
            const bool sorted = t.sortCol == c;
            const std::string label = std::string(name) + (sorted ? (t.sortAsc ? " ^" : " v") : "");
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 sz(std::max(ImGui::GetContentRegionAvail().x, ImGui::CalcTextSize(label.c_str()).x),
                            ImGui::GetTextLineHeight());
            ImGui::InvisibleButton("##h", ImVec2(std::max(sz.x, 8.0f), sz.y));
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (hov) {
                dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), Surface::selection);
            }
            dl->AddText(p0, hov ? Ink::onSelection : Ink::strong, label.c_str());
            if (ImGui::IsItemClicked()) {
                if (sorted && t.sortAsc) {
                    t.sortBy(c, false);
                } else if (sorted) {
                    t.sortBy(-1, true);
                } else {
                    t.sortBy(c, true);
                }
                acts.changed = true;
            }
            ImGui::PopID();
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(t.order.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& row = t.rows[t.order[static_cast<std::size_t>(i)]];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                for (int c = 0; c < ncols; ++c) {
                    ImGui::TableSetColumnIndex(c);
                    const json& cell = row[static_cast<std::size_t>(c)];
                    const std::string ref = chat::TableModel::cellRef(cell);
                    if (!ref.empty()) {
                        ImGui::PushID(c);
                        const std::string label = cell.is_object() && cell.contains("name") && cell["name"].is_string()
                                                      ? cell["name"].get<std::string>()
                                                      : shortName(nameOf, ref);
                        ImGui::PushStyleColor(ImGuiCol_Text, Ink::nav);
                        ImGui::TextUnformatted(label.c_str());
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s\nclick: open", ref.c_str());
                        }
                        if (ImGui::IsItemClicked()) {
                            acts.openRef = ref;
                        }
                        ImGui::PopID();
                    } else {
                        const std::string s = chat::TableModel::cellText(cell);
                        // the view's `align right` (and numbers by default) sit flush with the column's right edge
                        const std::string& al = static_cast<std::size_t>(c) < t.aligns.size() ? t.aligns[static_cast<std::size_t>(c)] : std::string();
                        const bool right = al == "right" || (al.empty() && cell.is_number());
                        if (right) {
                            const float w = ImGui::CalcTextSize(s.c_str()).x;
                            const float avail = ImGui::GetContentRegionAvail().x;
                            if (avail > w) {
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
                            }
                        } else if (al == "center") {
                            const float w = ImGui::CalcTextSize(s.c_str()).x;
                            const float avail = ImGui::GetContentRegionAvail().x;
                            if (avail > w) {
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
                            }
                        }
                        ImGui::PushStyleColor(ImGuiCol_Text, cell.is_number() ? Ink::inspected : Ink::primary);
                        if (cell.is_number()) {
                            ImGui::TextUnformatted(s.c_str());
                        } else {
                            symbolUi().text(s, Ink::primary, std::string());
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered() && s.size() > 24 && ImGui::GetMouseCursor() != ImGuiMouseCursor_Hand) {
                            ImGui::SetTooltip("%s", s.c_str());
                        }
                    }
                    if (ImGui::BeginPopupContextItem("cell-menu")) {
                        if (menuItem("COPY CELL")) {
                            ImGui::SetClipboardText((ref.empty() ? chat::TableModel::cellText(cell) : ref).c_str());
                        }
                        if (menuItem("COPY TABLE AS TSV")) {
                            ImGui::SetClipboardText(t.tsv().c_str());
                        }
                        ImGui::EndPopup();
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(5);
    if (ImGui::BeginPopupContextWindow("table-menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (menuItem("COPY TABLE AS TSV")) {
            ImGui::SetClipboardText(t.tsv().c_str());
        }
        ImGui::EndPopup();
    }
    if (!ownWindow) {
        ImGui::EndChild();
    }
}

// ---- tree -------------------------------------------------------------------------------------

void drawTreeNode(const chat::TreeNode& n, ViewUiState& st, const std::string& path, int depth, float cell,
                  const chat::NameOf& nameOf, ViewActions& acts) {
    const bool hasKids = !n.children.empty();
    const bool rootless = depth == 0 && n.label.empty();
    if (!rootless) {
        ImGui::PushID(path.c_str());
        const float x0 = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x0 + cell * 2.0f * static_cast<float>(std::max(0, depth - 1)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float box = ImGui::GetTextLineHeight() - 2.0f;
        if (hasKids) {
            const bool closed = st.collapsed.count(path) != 0;
            ImGui::InvisibleButton("##x", ImVec2(box + cell, box + 2.0f));
            const bool hov = ImGui::IsItemHovered();
            outlineRect(dl, ImVec2(p.x, p.y + 1.0f), ImVec2(p.x + box, p.y + 1.0f + box), hov ? Ink::strong : Ink::primary);
            dl->AddText(ImVec2(p.x + box * 0.5f - ImGui::CalcTextSize("+").x * 0.5f, p.y + 1.0f), Ink::primary,
                        closed ? "+" : "-");
            if (ImGui::IsItemClicked()) {
                if (closed) {
                    st.collapsed.erase(path);
                } else {
                    st.collapsed.insert(path);
                }
                acts.changed = true;
            }
            ImGui::SameLine(0.0f, 0.0f);
        } else {
            ImGui::Dummy(ImVec2(box + cell, box + 2.0f));
            ImGui::SameLine(0.0f, 0.0f);
            dl->AddText(ImVec2(p.x + box * 0.5f - 2.0f, p.y + 1.0f), Ink::secondary, "·");
        }
        if (!n.ref.empty()) {
            const std::string label = n.label.empty() || n.label == "·" ? shortName(nameOf, n.ref) : n.label;
            ImGui::PushStyleColor(ImGuiCol_Text, Ink::nav);
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\nclick: open", n.ref.c_str());
            }
            if (ImGui::IsItemClicked()) {
                acts.openRef = n.ref;
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, hasKids ? Ink::strong : Ink::primary);
            ImGui::TextUnformatted(n.label.c_str());
            ImGui::PopStyleColor();
        }
        if (!n.value.empty() && n.ref.empty()) {
            ImGui::SameLine(0.0f, cell);
            ImGui::PushStyleColor(ImGuiCol_Text, Ink::inspected);
            ImGui::TextUnformatted(n.value.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
        if (hasKids && st.collapsed.count(path)) {
            return;
        }
    }
    std::size_t i = 0;
    for (const chat::TreeNode& c : n.children) {
        drawTreeNode(c, st, path + "/" + std::to_string(i++), depth + 1, cell, nameOf, acts);
    }
}

void drawTreeBody(const chat::ViewSpec& v, ViewUiState& st, ImVec2 min, ImVec2 max, const std::string& scope,
                  const chat::NameOf& nameOf, ViewActions& acts) {
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginChild((scope + "/tree").c_str(), ImVec2(max.x - min.x, max.y - min.y));
    drive::hit("view:tree:" + chat::idPrefix(v.id), min, max);
    const float cell = ImGui::CalcTextSize("0").x;
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    drawTreeNode(st.tree, st, "", 0, cell, nameOf, acts);
    ImGui::EndChild();
}

// ---- graph ------------------------------------------------------------------------------------

void fitGraph(ViewUiState& st, ImVec2 canvas) {
    const model::LayoutMetrics m;
    if (st.graph.nodes.empty()) {
        st.zoom = 1.0f;
        st.pan = ImVec2(0.0f, 0.0f);
        return;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const model::GraphNode& n : st.graph.nodes) {
        x0 = std::min(x0, n.x);
        y0 = std::min(y0, n.y);
        x1 = std::max(x1, n.x + m.nodeW);
        y1 = std::max(y1, n.y + m.nodeH);
    }
    const float w = std::max(1.0f, x1 - x0), h = std::max(1.0f, y1 - y0);
    st.zoom = std::clamp(std::min((canvas.x - 16.0f) / w, (canvas.y - 16.0f) / h), 0.2f, 1.0f);
    st.pan = ImVec2((canvas.x - w * st.zoom) / 2.0f - x0 * st.zoom, (canvas.y - h * st.zoom) / 2.0f - y0 * st.zoom);
    st.fitted = true;
}

void drawGraphBody(const chat::ViewSpec& v, ViewUiState& st, ImVec2 min, ImVec2 max, ImFont* mono, const std::string& scope,
                   const chat::NameOf& nameOf, ViewActions& acts) {
    (void)nameOf;
    const ImVec2 size(max.x - min.x, max.y - min.y);
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginChild((scope + "/graph").c_str(), size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    drive::hit("view:graph:" + chat::idPrefix(v.id), min, max);
    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (!st.fitted) {
        fitGraph(st, size);
    }
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0.0f) {
        const ImVec2 m(io.MousePos.x - min.x, io.MousePos.y - min.y);
        const float before = st.zoom;
        st.zoom = std::clamp(st.zoom * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.15f, 3.0f);
        st.pan.x = m.x - (m.x - st.pan.x) * (st.zoom / before);
        st.pan.y = m.y - (m.y - st.pan.y) * (st.zoom / before);
        acts.changed = true;
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        st.pan.x += io.MouseDelta.x;
        st.pan.y += io.MouseDelta.y;
        st.panning = true;
        acts.changed = true;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(min, max, true);
    dl->AddRectFilled(min, max, Surface::background);
    const model::LayoutMetrics lm;
    auto toScreen = [&](float gx, float gy) { return ImVec2(min.x + st.pan.x + gx * st.zoom, min.y + st.pan.y + gy * st.zoom); };
    const float fs = ImGui::GetFontSize() * st.zoom;
    for (const model::GraphEdge& e : st.graph.edges) {
        const model::GraphNode* a = st.graph.node(e.from);
        const model::GraphNode* b = st.graph.node(e.to);
        if (!a || !b) {
            continue;
        }
        const std::vector<model::P2> path = model::angularPath(a->x + lm.nodeW, a->y + lm.nodeH / 2, b->x, b->y + lm.nodeH / 2, lm.nodeH);
        std::vector<ImVec2> pts;
        for (const model::P2& p : path) {
            pts.push_back(toScreen(p.x, p.y));
        }
        if (pts.size() >= 2) {
            const ImU32 color = e.kind == model::EdgeKind::Route ? Ink::primary : Ink::secondary;
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), color, 1.0f, ImDrawFlags_None);
            chevronAt(dl, pts[pts.size() - 2], pts.back(), color, 5.0f * std::max(0.6f, st.zoom));
            if (!e.label.empty() && st.zoom >= 0.5f) {
                const std::size_t mid = pts.size() / 2;
                const ImVec2 p0 = pts[mid - 1], p1 = pts[mid];
                dl->AddText(mono, fs * 0.85f, ImVec2((p0.x + p1.x) / 2, (p0.y + p1.y) / 2 - fs), Ink::secondary, e.label.c_str());
            }
        }
    }
    std::string hoveredNode;
    for (const model::GraphNode& n : st.graph.nodes) {
        const ImVec2 a = toScreen(n.x, n.y);
        const ImVec2 b = toScreen(n.x + lm.nodeW, n.y + lm.nodeH);
        const bool hov = hovered && io.MousePos.x >= a.x && io.MousePos.x <= b.x && io.MousePos.y >= a.y && io.MousePos.y <= b.y;
        if (hov) {
            hoveredNode = n.id;
        }
        dl->AddRectFilled(a, b, Surface::background);
        outlineRect(dl, a, b, hov ? Ink::strong : Ink::primary);
        const float pad = 6.0f * st.zoom;
        if (n.drivenBy == "process") {
            const float h = fs + 4.0f * st.zoom;
            const float slant = h * 0.268f;
            const ImVec2 pts[4] = {ImVec2(a.x, a.y + 1), ImVec2(b.x - 1, a.y + 1),
                                   ImVec2(b.x - 1 - slant, a.y + 1 + h), ImVec2(a.x + 1, a.y + 1 + h)};
            dl->AddConvexPolyFilled(pts, 4, Surface::hover);
            dl->AddText(mono, fs, ImVec2(a.x + pad, a.y + 1 + 2.0f * st.zoom), Ink::strong, n.name.c_str());
        } else {
            dl->AddText(mono, fs, ImVec2(a.x + pad, a.y + pad), Ink::strong, n.name.c_str());
        }
        if (!n.cls.empty() || !n.drivenBy.empty()) {
            const std::string sub = n.cls + (n.drivenBy.empty() ? "" : " / " + n.drivenBy);
            dl->AddText(mono, fs * 0.85f, ImVec2(a.x + pad, b.y - pad - fs * 0.85f), Ink::secondary, sub.c_str());
        }
    }
    if (!hoveredNode.empty() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !st.panning) {
        if (chat::looksLikeRef(hoveredNode)) {
            acts.openRef = hoveredNode;
        }
    }
    if (!hoveredNode.empty()) {
        ImGui::SetTooltip("%s", hoveredNode.c_str());
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        st.panning = false;
    }
    dl->PopClipRect();
    // [ FIT ] in the corner
    ImGui::SetCursorScreenPos(ImVec2(max.x - ImGui::CalcTextSize("[ FIT ]").x - 6.0f, min.y + 2.0f));
    if (bracketButtonNamed("fit", "fit:" + chat::idPrefix(v.id))) {
        st.fitted = false;
        acts.changed = true;
    }
    ImGui::EndChild();
}

// ---- image ------------------------------------------------------------------------------------

void ensureTexture(ViewUiState& st) {
    if (st.texTried) {
        return;
    }
    st.texTried = true;
    if (st.image.bytes.empty()) {
        st.texError = st.image.blob.empty() ? "no image bytes" : "image bytes pending (blob " + chat::idPrefix(st.image.blob) + ")";
        return;
    }
    if (!textures::ready()) {
        st.texError = "textures not ready";
        return;
    }
    st.tex = textures::upload(st.image.bytes.data(), st.image.bytes.size(), st.texError);
}

ImVec2 imageFit(const ViewUiState& st, float wrap) {
    if (!st.tex.valid()) {
        return ImVec2(wrap, 0.0f);
    }
    const float w = std::min(wrap - 2.0f * kPad, static_cast<float>(st.tex.w));
    float h = w * static_cast<float>(st.tex.h) / static_cast<float>(st.tex.w);
    if (h > kViewImageMaxH) {
        h = kViewImageMaxH;
    }
    const float ww = h * static_cast<float>(st.tex.w) / static_cast<float>(st.tex.h);
    return ImVec2(std::min(w, ww), h);
}

void drawImageBody(const chat::ViewSpec& v, ViewUiState& st, ImVec2 min, ImVec2 max, ViewActions& acts) {
    ensureTexture(st);
    ImGui::SetCursorScreenPos(ImVec2(min.x + kPad, min.y + kPad));
    if (!st.tex.valid()) {
        text(Ink::inspected, "image: " + st.texError);
        return;
    }
    const ImVec2 fit = imageFit(st, max.x - min.x);
    ImGui::Image(ImTextureRef(st.tex.id), fit);
    drive::hit("view:image:" + chat::idPrefix(v.id), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%dx%d %s\nclick: full size", st.tex.w, st.tex.h, st.image.mime.c_str());
    }
    if (ImGui::IsItemClicked()) {
        st.expanded = true;
        acts.changed = true;
    }
}

} // namespace

ImU32 viewKindColor(const std::string& kind) {
    if (kind == "table") {
        return Ink::inspected;
    }
    if (kind == "graph") {
        return Ink::nav;
    }
    if (kind == "image") {
        return Ink::unresolved;
    }
    if (kind == "tree") {
        return Ink::primary;
    }
    return Ink::strong;
}

void buildViewState(const chat::ViewSpec& v, ViewUiState& st) {
    if (st.built) {
        return;
    }
    st.built = true;
    if (v.kind == "table") {
        st.table = chat::TableModel::from(v.data);
    } else if (v.kind == "tree") {
        st.tree = chat::treeFrom(v.data);
    } else if (v.kind == "graph") {
        st.graph = chat::graphFrom(v.data);
    } else if (v.kind == "image") {
        st.image = chat::imageFrom(v.data);
    }
}

float viewBlockHeight(const chat::ViewSpec& v, ViewUiState& st, float wrap, float lineH) {
    buildViewState(v, st);
    const float title = titleHeight(lineH);
    const float row = lineH + 4.0f;
    if (v.kind == "table") {
        const std::size_t rows = std::min<std::size_t>(st.table.size(), kViewTableRows);
        const bool more = st.table.size() > kViewTableRows || st.table.columns.size() > 6;
        return title + row * static_cast<float>(rows + 1) + 8.0f + (more ? 14.0f : 0.0f);
    }
    if (v.kind == "tree") {
        const std::size_t rows = std::min<std::size_t>(visibleTreeRows(st.tree, st.collapsed, "", 0), kViewTreeRows);
        return title + (lineH + 2.0f) * static_cast<float>(std::max<std::size_t>(rows, 1)) + 10.0f;
    }
    if (v.kind == "graph") {
        return title + lineH * kViewGraphLines;
    }
    if (v.kind == "image") {
        ensureTexture(st);
        if (!st.tex.valid()) {
            return title + lineH + 2.0f * kPad;
        }
        return title + imageFit(st, wrap).y + 2.0f * kPad;
    }
    if (v.kind == "pane") {
        return title + ImGui::GetFrameHeightWithSpacing() + 6.0f;
    }
    return title; // text: the body is in the document
}

ViewActions drawViewBlock(const chat::ViewSpec& v, ViewUiState& st, ImVec2 min, ImVec2 max, ImFont* mono,
                          const std::string& scope, const chat::NameOf& nameOf, const std::string& via, float lineH) {
    ViewActions acts;
    buildViewState(v, st);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float wrap = max.x - min.x;
    const bool expandable = v.kind == "table" || v.kind == "graph" || v.kind == "image";
    const ImVec2 cursorBefore = ImGui::GetCursorScreenPos();
    ImGui::PushID(scope.c_str());
    drawTitle(dl, v, st, min, wrap, mono, lineH, scope, nameOf, via, acts, expandable);
    const ImVec2 bodyMin(min.x, min.y + titleHeight(lineH));
    const ImVec2 bodyMax(max.x - 1.0f, max.y);
    if (bodyMax.y > bodyMin.y + 2.0f) {
        // the body frame: a thin dim chamfer, open at the top where the bar sits
        dl->AddLine(ImVec2(min.x, bodyMin.y), ImVec2(min.x, max.y), Ink::secondary);
        dl->AddLine(ImVec2(min.x, max.y), ImVec2(max.x - 1.0f, max.y), Ink::secondary);
        dl->AddLine(ImVec2(max.x - 1.0f, bodyMin.y), ImVec2(max.x - 1.0f, max.y), Ink::secondary);
        if (v.kind == "table") {
            drawTableBody(v, st, ImVec2(bodyMin.x + 1.0f, bodyMin.y + 2.0f), ImVec2(bodyMax.x - 1.0f, bodyMax.y - 2.0f), mono,
                          scope, nameOf, acts, false);
        } else if (v.kind == "tree") {
            drawTreeBody(v, st, ImVec2(bodyMin.x + 1.0f, bodyMin.y + 2.0f), ImVec2(bodyMax.x - 1.0f, bodyMax.y - 2.0f), scope,
                         nameOf, acts);
        } else if (v.kind == "graph") {
            drawGraphBody(v, st, ImVec2(bodyMin.x + 1.0f, bodyMin.y + 1.0f), ImVec2(bodyMax.x - 1.0f, bodyMax.y - 1.0f), mono,
                          scope, nameOf, acts);
        } else if (v.kind == "image") {
            drawImageBody(v, st, bodyMin, bodyMax, acts);
        } else if (v.kind == "pane") {
            ImGui::SetCursorScreenPos(ImVec2(bodyMin.x + 4.0f, bodyMin.y + 2.0f));
            const std::string pane = v.data.is_object() ? v.data.value("pane", std::string()) : std::string();
            if (bracketButtonNamed("OPEN PANE", "open-pane:" + pane, !pane.empty())) {
                LinkRequest r;
                r.kind = LinkRequest::OpenPane;
                r.object = pane;
                r.openKind = v.data.value("kind", std::string("editor"));
                symbolUi().ask(std::move(r));
            }
        }
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(cursorBefore);
    return acts;
}

ViewActions drawExpandedView(const chat::ViewSpec& v, ViewUiState& st, ImFont* mono, const std::string& scope,
                             const chat::NameOf& nameOf) {
    ViewActions acts;
    if (!st.expanded) {
        return acts;
    }
    buildViewState(v, st);
    const std::string title = "VIEW // " + (v.title.empty() ? v.kind : v.title) + "###" + scope + "/win";
    ImGui::SetNextWindowSize(ImVec2(760.0f, 480.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    // PushFont/PopFont stay inside this window's Begin/End (ImGui 1.92 keeps the font stack per
    // window; a pop after End leaves the next window with a zero font size).
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        (void)mono; // the window uses the default (monospace) font; no font push here
        if (bracketButtonNamed("close", "close:" + chat::idPrefix(v.id))) {
            open = false;
        }
        ImGui::SameLine();
        text(Ink::secondary, v.kind + " · " + std::to_string(v.kind == "table" ? st.table.size() : 0) + (v.kind == "table" ? " rows" : ""));
        if (v.kind == "table") {
            ImGui::BeginChild("expanded-table", ImVec2(0, 0));
            drawTableBody(v, st, ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos(), mono, scope + "/x", nameOf, acts, true);
            ImGui::EndChild();
        } else if (v.kind == "image") {
            ensureTexture(st);
            ImGui::BeginChild("expanded-image", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
            if (st.tex.valid()) {
                ImGui::Image(ImTextureRef(st.tex.id), ImVec2(static_cast<float>(st.tex.w), static_cast<float>(st.tex.h)));
            } else {
                text(Ink::inspected, "image: " + st.texError);
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
    if (!open) {
        st.expanded = false;
        acts.changed = true;
    }
    return acts;
}

} // namespace ke::ui
