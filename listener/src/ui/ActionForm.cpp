#include "ui/ActionForm.hpp"

#include "drive/Commands.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <imgui_stdlib.h>

namespace ke::ui {

namespace {

std::string joinPath(const std::string& path, const std::string& name) {
    return path.empty() ? name : path + "." + name;
}

} // namespace

// ---- widgets -------------------------------------------------------------------------------------

namespace {

void label(const std::string& name, bool optional, float width) {
    const float x0 = ImGui::GetCursorPosX(); // the row's indent: the widget column starts `width` right of it
    text(optional ? Ink::secondary : Ink::primary, name);
    ImGui::SameLine(x0 + width);
}

} // namespace

bool drawForm(const std::vector<ArgSpec>& specs, FormValues& v, const FormContext& ctx, const std::string& path) {
    bool changed = false;
    float labelW = ctx.labelWidth;
    if (labelW <= 0.0f) {
        for (const ArgSpec& a : specs) {
            labelW = std::max(labelW, ImGui::CalcTextSize(a.name.c_str()).x);
        }
        labelW += 3.0f * ImGui::CalcTextSize("0").x;
    }
    const float cw = ImGui::CalcTextSize("0").x;
    for (const ArgSpec& a : specs) {
        const std::string p = joinPath(path, a.name);
        ImGui::PushID(p.c_str());
        auto hit = [&](const std::string& suffix = "") {
            if (!ctx.driveScope.empty()) {
                drive::hit(ctx.driveScope + p + suffix, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
        };
        switch (a.kind) {
        case ArgSpec::Text:
        case ArgSpec::Id: {
            label(a.name, a.optional, labelW);
            json* s = valueSlot(v, p);
            std::string val = s->is_string() ? s->get<std::string>() : "";
            auto choices = ctx.idChoices.find(a.name);
            if (a.kind == ArgSpec::Id && choices != ctx.idChoices.end() && !choices->second.empty()) {
                std::string shown = val;
                for (const auto& [id, lbl] : choices->second) {
                    if (id == val) {
                        shown = lbl;
                    }
                }
                ImGui::SetNextItemWidth(std::max(cw * 16.0f, ImGui::GetContentRegionAvail().x - cw * 2.0f));
                if (ImGui::BeginCombo("##combo", shown.empty() ? "(none)" : shown.c_str())) {
                    if (a.optional && ImGui::Selectable("(none)", val.empty())) {
                        *s = "";
                        changed = true;
                    }
                    for (const auto& [id, lbl] : choices->second) {
                        if (ImGui::Selectable(lbl.c_str(), id == val)) {
                            *s = id;
                            changed = true;
                        }
                        if (ImGui::IsItemHovered()) {
                            ke::ui::tip("%s", id.c_str());
                        }
                    }
                    ImGui::EndCombo();
                }
                hit();
            } else {
                ImGui::SetNextItemWidth(std::max(cw * 16.0f, ImGui::GetContentRegionAvail().x - cw * 2.0f));
                if (ImGui::InputTextWithHint("##t", a.kind == ArgSpec::Id ? "object id" : a.hint.c_str(), &val)) {
                    *s = val;
                    changed = true;
                }
                hit();
            }
            break;
        }
        case ArgSpec::Multiline: {
            label(a.name, a.optional, labelW);
            json* s = valueSlot(v, p);
            std::string val = s->is_string() ? s->get<std::string>() : "";
            const float h = ImGui::GetTextLineHeight() * 6.0f + 8.0f;
            if (ImGui::InputTextMultiline("##m", &val, ImVec2(std::max(cw * 16.0f, ImGui::GetContentRegionAvail().x - cw * 2.0f), h))) {
                *s = val;
                changed = true;
            }
            hit();
            break;
        }
        case ArgSpec::Int: {
            label(a.name, a.optional, labelW);
            std::string& t = v.intText[p];
            ImGui::SetNextItemWidth(cw * 12.0f);
            if (ImGui::InputTextWithHint("##i", "number", &t, ImGuiInputTextFlags_CharsDecimal)) {
                changed = true;
            }
            hit();
            break;
        }
        case ArgSpec::Bool: {
            label(a.name, a.optional, labelW);
            json* s = valueSlot(v, p);
            bool b = s->is_boolean() && s->get<bool>();
            if (ImGui::Checkbox("##b", &b)) {
                *s = b;
                changed = true;
            }
            hit();
            break;
        }
        case ArgSpec::Enum: {
            label(a.name, a.optional, labelW);
            json* s = valueSlot(v, p);
            std::string val = s->is_string() ? s->get<std::string>() : "";
            ImGui::SetNextItemWidth(cw * 20.0f);
            if (ImGui::BeginCombo("##e", val.empty() ? "(default)" : val.c_str())) {
                if (a.optional && ImGui::Selectable("(default)", val.empty())) {
                    *s = "";
                    changed = true;
                }
                for (const std::string& opt : a.values) {
                    if (ImGui::Selectable(opt.c_str(), opt == val)) {
                        *s = opt;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            hit();
            break;
        }
        case ArgSpec::EnumList: {
            label(a.name, a.optional, labelW);
            json* s = valueSlot(v, p);
            if (!s->is_array()) {
                *s = json::array();
            }
            ImGui::BeginGroup();
            for (const std::string& opt : a.values) {
                bool on = std::find(s->begin(), s->end(), json(opt)) != s->end();
                if (ImGui::Checkbox(opt.c_str(), &on)) {
                    if (on) {
                        s->push_back(opt);
                    } else {
                        s->erase(std::remove(s->begin(), s->end(), json(opt)), s->end());
                    }
                    changed = true;
                }
                hit(":" + opt);
                ImGui::SameLine();
            }
            ImGui::NewLine();
            ImGui::EndGroup();
            break;
        }
        case ArgSpec::IdList:
        case ArgSpec::StringList: {
            label(a.name, a.optional, labelW);
            std::string& t = v.listText[p];
            ImGui::SetNextItemWidth(std::max(cw * 16.0f, ImGui::GetContentRegionAvail().x - cw * 2.0f));
            if (ImGui::InputTextWithHint("##l", a.kind == ArgSpec::IdList ? "ids, comma separated" : "values, comma separated", &t)) {
                changed = true;
            }
            hit();
            break;
        }
        case ArgSpec::Nested: {
            text(a.optional ? Ink::secondary : Ink::primary, a.name);
            ImGui::Indent(cw * 2.0f);
            changed |= drawForm(a.children, v, ctx, p);
            ImGui::Unindent(cw * 2.0f);
            break;
        }
        case ArgSpec::Choice: {
            label(a.name, a.optional, labelW);
            int& idx = v.choice[p];
            if (idx < 0 || static_cast<std::size_t>(idx) >= a.children.size()) {
                idx = 0;
            }
            for (std::size_t i = 0; i < a.children.size(); ++i) {
                if (i > 0) {
                    ImGui::SameLine();
                }
                if (ImGui::RadioButton(a.children[i].name.c_str(), idx == static_cast<int>(i))) {
                    idx = static_cast<int>(i);
                    changed = true;
                }
                hit(":" + a.children[i].name);
            }
            const ArgSpec& variant = a.children[static_cast<std::size_t>(idx)];
            if (!(variant.kind == ArgSpec::Enum && variant.values.size() == 1)) {
                ImGui::Indent(cw * 2.0f);
                if (variant.kind == ArgSpec::Nested) {
                    changed |= drawForm(variant.children, v, ctx, p + "#" + std::to_string(idx));
                } else {
                    // a leaf variant (a list of ids, a string): one widget stored under <path>#<idx>
                    changed |= drawForm(std::vector<ArgSpec>{variant}, v, ctx, p + "#" + std::to_string(idx));
                }
                ImGui::Unindent(cw * 2.0f);
            }
            break;
        }
        case ArgSpec::Unknown: {
            label(a.name, a.optional, labelW);
            json* s = valueSlot(v, p);
            std::string val = s->is_string() ? s->get<std::string>() : (s->is_null() ? "" : s->dump());
            ImGui::SetNextItemWidth(std::max(cw * 16.0f, ImGui::GetContentRegionAvail().x - cw * 2.0f));
            if (ImGui::InputTextWithHint("##u", "value (JSON)", &val)) {
                json parsed = json::parse(val, nullptr, false);
                *s = parsed.is_discarded() ? json(val) : parsed;
                changed = true;
            }
            hit();
            break;
        }
        }
        ImGui::PopID();
    }
    return changed;
}

} // namespace ke::ui
