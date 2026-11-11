#include "ui/MathView.hpp"

#include "chat/Chat.hpp"
#include "drive/Commands.hpp"
#include "modules/MathLayout.hpp"
#include "platform/Platform.hpp"
#include "ui/Markdown.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/Textures.hpp"
#include "ui/Theme.hpp"
#include "util/Base64.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ke::ui {

namespace {

using modules::MathBox;

// Layout at a font size is the same every frame; the map stays small because documents do.
const MathBox& boxFor(const std::string& tex, float size, bool display) {
    static std::map<std::tuple<std::string, float, bool, const void*>, MathBox> cache;
    if (cache.size() > 4096) {
        cache.clear();
    }
    ImFont* font = ImGui::GetFont();
    auto [it, fresh] = cache.try_emplace(std::make_tuple(tex, size, display, static_cast<const void*>(font)));
    if (fresh) {
        const auto measure = [font](std::string_view s, float z) {
            return font->CalcTextSizeA(z, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x;
        };
        it->second = modules::layoutMath(tex, size, measure, display);
    }
    return it->second;
}

void paint(ImDrawList* dl, const MathBox& box, ImVec2 baseline, ImU32 ink) {
    ImFont* font = ImGui::GetFont();
    const ImFontBaked* baked = ImGui::GetFontBaked();
    const float ascent = baked != nullptr ? baked->Ascent / ImGui::GetFontSize() : 0.8f;
    for (const modules::MathGlyph& g : box.glyphs) {
        dl->AddText(font, g.size, ImVec2(baseline.x + g.x, baseline.y + g.y - g.size * ascent), ink,
                    g.text.c_str(), g.text.c_str() + g.text.size());
    }
    for (const modules::MathRule& r : box.rules) {
        dl->AddLine(ImVec2(baseline.x + r.x0, baseline.y + r.y0), ImVec2(baseline.x + r.x1, baseline.y + r.y1), ink, 1.0f);
    }
}

std::set<std::string>& sourceShown() {
    static std::set<std::string> shown;
    return shown;
}

ImU32 g_mathInk = 0;
ImU32 mathInk() { return g_mathInk != 0 ? g_mathInk : Ink::primary; }

// ---- what the layout cannot draw: the world typesets it, by source

chat::Chat* g_mathWorld = nullptr;

struct Typeset {
    enum State { Waiting, Asking, Ready, Failed } state = Waiting;
    textures::Texture image;
    float points = 0.0f; // the formula's width, in points
    int pages = 1;
    bool page = false;   // a document's page: paper under it, black ink
    std::string error;
    std::string log;     // what TeX said, fetched when asked for
    ImVec2 min, max;     // where it was last drawn
    float noteTop = 0.0f, noteBottom = 0.0f;
    Uint64 askAt = 0;
};

std::map<std::string, Typeset>& typesets() {
    static std::map<std::string, Typeset> all;
    return all;
}

bool g_asking = false; // one at a time: a reply full of formulas is not a burst of processes

void finishRead(Typeset& t, std::vector<unsigned char> png) {
    g_asking = false;
    std::string err;
    t.image = textures::uploadCoverage(png.data(), png.size(), err);
    t.state = t.image.valid() ? Typeset::Ready : Typeset::Failed;
    t.error = err;
}

void readTypeset(const std::string& source, std::vector<unsigned char> have) {
    const nlohmann::json args = {{"source", source}, {"offset", have.size()}};
    g_mathWorld->actNamed("tex.read", args, [source, have = std::move(have)](const Response& r) mutable {
        Typeset& t = typesets()[source];
        const nlohmann::json& out = r.result.is_object() && r.result.contains("result") ? r.result["result"] : r.result;
        std::vector<unsigned char> chunk;
        if (!r.ok || !out.is_object() || !out.contains("base64") || !out["base64"].is_string() ||
            !util::base64Decode(out["base64"].get<std::string>(), chunk)) {
            g_asking = false;
            t.state = Typeset::Failed;
            t.error = "the image did not arrive";
            return;
        }
        have.insert(have.end(), chunk.begin(), chunk.end());
        if (out.contains("more") && out["more"].is_boolean() && out["more"].get<bool>() && !chunk.empty()) {
            readTypeset(source, std::move(have));
        } else {
            finishRead(t, std::move(have));
        }
    });
}

// What the world made of `source`, asking for it once when it is on screen.
Typeset& typesetOf(const std::string& source) {
    Typeset& t = typesets()[source];
    if (t.state == Typeset::Waiting && !g_asking && g_mathWorld != nullptr && SDL_GetTicks() >= t.askAt) {
        g_asking = true;
        t.state = Typeset::Asking;
        g_mathWorld->actNamed("tex.render", nlohmann::json{{"source", source}}, [source](const Response& r) {
            Typeset& got = typesets()[source];
            if (r.ok) {
                const nlohmann::json& out = r.result.is_object() && r.result.contains("result") ? r.result["result"] : r.result;
                if (out.is_object() && out.contains("width") && out["width"].is_number()) {
                    got.points = out["width"].get<float>();
                }
                if (out.is_object() && out.contains("pages") && out["pages"].is_number()) {
                    got.pages = out["pages"].get<int>();
                }
                got.page = out.is_object() && out.contains("kind") && out["kind"].is_string() &&
                           out["kind"].get<std::string>() == "document";
                readTypeset(source, {});
                return;
            }
            g_asking = false;
            if (r.err && r.err->code == "renderer-preparing") {
                got.state = Typeset::Waiting;
                got.askAt = SDL_GetTicks() + 3000;
                return;
            }
            got.state = Typeset::Failed;
            got.error = r.err ? r.err->message : std::string("no answer");
        });
    }
    return t;
}

} // namespace

void setMathInk(ImU32 ink) { g_mathInk = ink; }

void setMathWorld(chat::Chat* chat) { g_mathWorld = chat; }

std::pair<int, int> typesetCounts() {
    int ready = 0, failed = 0;
    for (const auto& [source, t] : typesets()) {
        ready += t.state == Typeset::Ready ? 1 : 0;
        failed += t.state == Typeset::Failed ? 1 : 0;
    }
    return {ready, failed};
}

std::vector<TypesetSeen> typesetsSeen() {
    std::vector<TypesetSeen> out;
    for (const auto& [source, t] : typesets()) {
        TypesetSeen seen;
        seen.source = source.substr(0, 60);
        seen.failed = t.state == Typeset::Failed;
        seen.reason = seen.failed ? t.error : std::string();
        seen.pages = t.pages;
        seen.page = t.page;
        seen.log = t.log.size();
        seen.left = t.min.x;
        seen.top = t.min.y;
        seen.right = t.max.x;
        seen.bottom = t.max.y;
        seen.noteTop = t.noteTop;
        seen.noteBottom = t.noteBottom;
        out.push_back(std::move(seen));
    }
    return out;
}

void drawInlineMath(const std::string& tex, const std::string& shown, ImVec2 at, float width, float lineHeight) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float size = ImGui::GetFontSize();
    const MathBox& box = boxFor(tex, size, false);
    if (!box.ok) {
        dl->AddText(at, Ink::secondary, tex.c_str()); // no delimiters: they are not what was meant
        if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + width, at.y + lineHeight))) {
            ke::ui::tip("%s", box.error.c_str());
        }
        return;
    }
    // Fit the span the source reserved and the line it sits on.
    const float fit = std::min({1.0f, width / std::max(1.0f, box.width), lineHeight * 1.6f / std::max(1.0f, box.height())});
    const MathBox small = modules::scaled(box, fit);
    const float baseline = at.y + lineHeight * 0.8f;
    paint(dl, small, ImVec2(at.x + (width - small.width) / 2, baseline), mathInk());
    if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + width, at.y + lineHeight))) {
        ke::ui::tip("%s", shown.c_str());
    }
}

void drawDisplayMath(const std::string& tex, ImVec2 min, ImVec2 max, int id) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float size = ImGui::GetFontSize() * kDisplayMathScale;
    // Only math goes through the layout. What it cannot draw, and all text and documents, the world
    // typesets; until then the source, dim.
    static const MathBox kNone;
    const bool math = modules::texKind(tex) == modules::TexKind::Math;
    const MathBox& box = math ? boxFor(tex, size, true) : kNone;
    Typeset* typeset = math && box.ok ? nullptr : &typesetOf(tex);
    const bool imaged = typeset != nullptr && typeset->state == Typeset::Ready;
    const bool failed = typeset != nullptr && typeset->state == Typeset::Failed;
    const bool typesetFine = (math && box.ok) || imaged;
    const bool source = !typesetFine || sourceShown().count(tex) != 0;
    ImGui::PushID(id);
    ImGui::PushID(tex.c_str()); // the same number in another Message is another formula
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    if (typeset != nullptr) {
        typeset->min = min;
        typeset->max = max;
    }
    if (source) {
        // The region is the paragraph's: nothing drawn here reaches the next one. A failure's reason
        // is one line under the source, always inside the region; the source stops short of it.
        const float room = max.x - min.x - 16.0f;
        const float line = ImGui::GetTextLineHeight();
        const float tall = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, room, tex.c_str()).y;
        const float bottom = failed ? std::min(min.y + tall + 6.0f, max.y - line - 2.0f) : max.y;
        dl->PushClipRect(min, ImVec2(max.x, bottom), true);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 8.0f, min.y + 2.0f),
                    typesetFine ? mathInk() : Ink::secondary, tex.c_str(), nullptr, room);
        dl->PopClipRect();
        if (failed) {
            const std::string why = "not typeset: " + typeset->error;
            failedTypesets().insert(tex);
            typeset->noteTop = bottom;
            typeset->noteBottom = bottom + line;
            dl->PushClipRect(ImVec2(min.x, bottom), max, true);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 8.0f, bottom), Ink::inspected, why.c_str());
            dl->PopClipRect();
        }
    } else if (imaged) {
        const float w = static_cast<float>(typeset->image.w);
        const float h = static_cast<float>(typeset->image.h);
        const float roomW = max.x - min.x - 8.0f;
        const float roomH = max.y - min.y;
        if (typeset->page) {
            // A document's first page, on paper, as wide as the chat lets it be.
            const float fit = std::min(roomW / w, roomH / h);
            const ImVec2 a((min.x + max.x - w * fit) / 2, min.y);
            const ImVec2 z(a.x + w * fit, a.y + h * fit);
            dl->AddRectFilled(a, z, Surface::paper);
            dl->AddImage(typeset->image.id, a, z, ImVec2(0, 0), ImVec2(1, 1), Ink::onPaper);
            if (typeset->pages > 1) {
                const std::string more = "page 1 of " + std::to_string(typeset->pages);
                const ImVec2 sz = ImGui::CalcTextSize(more.c_str());
                dl->AddText(ImVec2(z.x - sz.x - 8.0f, z.y - sz.y - 4.0f), Ink::onPaper, more.c_str());
            }
        } else {
            // A point of TeX at the display size, then whatever fits: wide formulas shrink to the width.
            const float natural = typeset->points > 0.0f ? typeset->points * size / 10.0f / w : 1.0f;
            const float fit = std::min({natural, roomW / w, roomH / h});
            // Text starts where text starts; a formula is centred.
            const ImVec2 a(math ? (min.x + max.x - w * fit) / 2 : min.x + 8.0f, (min.y + max.y - h * fit) / 2);
            dl->AddImage(typeset->image.id, a, ImVec2(a.x + w * fit, a.y + h * fit), ImVec2(0, 0), ImVec2(1, 1), mathInk());
        }
    } else {
        const float fit = std::min(1.0f, (max.x - min.x - 8.0f) / std::max(1.0f, box.width));
        const MathBox fitted = modules::scaled(box, fit);
        const ImVec2 at((min.x + max.x - fitted.width) / 2, (min.y + max.y - fitted.height()) / 2 + fitted.ascent);
        paint(dl, fitted, at, mathInk());
    }
    // What TeX said, in full, a click from the short reason.
    if (typeset != nullptr && ImGui::BeginPopup("tex-log")) {
        ImGui::TextUnformatted("what TeX said");
        ImGui::BeginChild("tex-log-text", ImVec2(ImGui::CalcTextSize("0").x * 90.0f, ImGui::GetTextLineHeight() * 24.0f));
        ImGui::TextUnformatted(typeset->log.c_str());
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    // Source is one click away, and copy yields source.
    const float cw = ImGui::CalcTextSize("0").x;
    // Above the formula's own space, not over it: a wide formula reaches the right edge.
    // Held down is still hovered: a button that vanishes under the press never fires.
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        !ImGui::IsMouseHoveringRect(min, max)) {
        ImGui::PopID();
        ImGui::PopID();
        ui::restoreCursor(keep);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        return;
    }
    // A page taller than the pane keeps them in view.
    ImGui::SetCursorScreenPos(ImVec2(max.x - cw * (typeset != nullptr ? 32.0f : 20.0f), std::max(min.y, dl->GetClipRectMin().y + 2.0f)));
    if (typesetFine && bracketButtonNamed(source ? "TYPESET" : "SOURCE", "math-source:" + std::to_string(id), true, source)) {
        if (source) {
            sourceShown().erase(tex);
        } else {
            sourceShown().insert(tex);
        }
    }
    if (typesetFine) {
        ImGui::SameLine();
    }
    if (bracketButtonNamed("COPY", "math-copy:" + std::to_string(id))) {
        platform::setClipboard(tex);
    }
    if (imaged) {
        ImGui::SameLine();
        if (bracketButtonNamed("OPEN", "math-open:" + std::to_string(id))) {
            LinkRequest r;
            r.kind = LinkRequest::OpenFile;
            r.openKind = "image";
            r.path = "typeset";
            r.tex = tex;
            symbolUi().ask(std::move(r));
        }
        if (ImGui::IsItemHovered()) {
            ke::ui::tip("the first page, in a pane of its own");
        }
    }
    if (failed && g_mathWorld != nullptr) {
        ImGui::SameLine();
        if (bracketButtonNamed("LOG", "math-log:" + std::to_string(id))) {
            ImGui::OpenPopup("tex-log");
            if (typeset->log.empty()) {
                const std::string source_ = tex;
                g_mathWorld->actNamed("tex.log", nlohmann::json{{"source", tex}}, [source_](const Response& r) {
                    const nlohmann::json& out = r.result.is_object() && r.result.contains("result") ? r.result["result"] : r.result;
                    typesets()[source_].log = r.ok && out.is_object() && out.contains("log") && out["log"].is_string()
                                                  ? out["log"].get<std::string>()
                                                  : "the log is not there: " + r.result.dump();
                });
            }
        }
    }
    ImGui::PopID();
    ImGui::PopID();
    ui::restoreCursor(keep);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

} // namespace ke::ui
