// Text drawn as a document: Markdown or plain linked text, selectable, with its symbols live.
// Shared by the viewers and by an editor's rendered view, so they read alike.
#pragma once

#include "ui/Markdown.hpp"
#include "ui/SelectableText.hpp"
#include "ui/SymbolUi.hpp"
#include "ui/Theme.hpp"
#include "ui/terminal/Internal.hpp"

#include <string>

namespace ke::modules {

class RenderedDoc {
  public:
    void invalidate() { built_ = false; }
    bool stale() const { return !built_ || themeBuilt_ != ui::themeVersion(); }

    // Draws at the cursor, `markdown` or plain, wrapped at `wrap`. `key` scopes its ImGui ids.
    void draw(const std::string& text, bool markdown, float wrap, const char* key, ImU32 ink = 0) {
        const ImU32 base = ink != 0 ? ink : ui::Ink::primary;
        if (stale() || base != inkBuilt_) {
            built_ = true;
            inkBuilt_ = base;
            themeBuilt_ = ui::themeVersion();
            doc_ = ui::SelDoc{};
            doc_.version = ++builds_;
            md_ = ui::MdExtras{};
            scan_.table = ui::symbolUi().table();
            scan_.ctx.fold = ui::symbolUi().fold();
            md_.scan = &scan_;
            if (markdown) {
                ui::appendMarkdown(doc_, text, base, 0, md_);
            } else {
                ui::appendLinked(doc_, text, base, 0, md_);
            }
            doc_.finish();
            sel_.clear();
        }
        ImFont* font = ImGui::GetFont();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImDrawListSplitter split;
        split.Split(dl, 3);
        split.SetCurrentChannel(dl, 1);
        const ui::SelResult sr = ui::drawSelectableDoc(doc_, sel_, wrap, font, ImGui::GetFontSize(), true, &layout_);
        split.SetCurrentChannel(dl, 2);
        ui::term::drawMarkdownExtras(md_, doc_, layout_, origin, wrap, sr.hoveredPara, key, &split);
        split.Merge(dl);
        ui::symbolUi().handleDoc(md_, doc_, sr, sel_.dragging);
        ui::term::registerSymbolHits(md_, doc_, layout_, font, origin);
    }

    std::string selection() const { return ui::selectedText(doc_, sel_); }
    std::size_t mathCount() const { return md_.math.size(); }

  private:
    bool built_ = false;
    ImU32 inkBuilt_ = 0;
    int themeBuilt_ = -1;
    unsigned long long builds_ = 0;
    ui::SelDoc doc_;
    ui::SelState sel_;
    ui::SelLayout layout_;
    ui::MdExtras md_;
    ui::SymbolScan scan_;
};

} // namespace ke::modules
