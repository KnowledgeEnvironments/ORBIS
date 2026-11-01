// View blocks in the transcript (sprint 1 L10): a Message of kind view renders as a titled block
// instead of text. The title bar is the slanted header of the style guide (white title, a kind
// chip, the `about:` provenance refs as chips, `[ EXPAND ]`), then the body by kind:
//   text   the markdown renderer (the terminal puts it in the document itself)
//   table  sortable, column-resizable, clipped; a cell holding a ref opens that object; right-click
//          copies as TSV; more than 12 rows get [ EXPAND ] into their own window
//   tree   indented, chamfered expanders, refs clickable
//   graph  the patcher's node and edge drawing in a bounded canvas (pan, zoom, [ FIT ]); [ EXPAND ]
//          puts it on the GRAPH pane as a labelled overlay
//   image  decoded once (stb_image), fit to width; click opens it full size
// The terminal reserves the block's height in its selectable document (a kind-5 paragraph) and
// draws the block over that slot, so selection and scrolling keep working around it. Everything
// here is per view id state kept by the tab; nothing is written to the world.
#pragma once

#include "chat/Transcript.hpp"
#include "chat/Views.hpp"
#include "model/Graph.hpp"
#include "ui/Textures.hpp"

#include <imgui.h>
#include <set>
#include <string>

namespace ke::ui {

struct ViewUiState {
    bool built = false; // the models below were built from the spec (once; a view never changes)
    chat::TableModel table;
    chat::TreeNode tree;
    model::GraphModel graph;
    chat::ImageBytes image;
    std::set<std::string> collapsed; // tree node paths folded by the user
    // graph canvas
    ImVec2 pan{0.0f, 0.0f};
    float zoom = 1.0f;
    bool fitted = false;
    bool panning = false;
    // image
    textures::Texture tex;
    bool texTried = false;
    std::string texError;
    // own window ([ EXPAND ] for tables and images)
    bool expanded = false;
};

struct ViewActions {
    std::string openRef;      // a ref the user clicked (a cell, a tree node, a graph node, an about chip)
    bool expandGraph = false; // put this view's graph on the GRAPH pane
    bool changed = false;     // state changed (a frame is due)
};

constexpr int kViewTableRows = 12;   // rows shown inline before the table scrolls / expands
constexpr int kViewTreeRows = 24;    // tree rows shown inline before it scrolls
constexpr float kViewGraphLines = 14.0f; // graph canvas height in text lines
constexpr float kViewImageMaxH = 600.0f;

// Build the per-kind model from the spec (idempotent).
void buildViewState(const chat::ViewSpec& v, ViewUiState& st);
// The block's total height (title bar + body) at `wrap` width; text views answer the title only
// (their body is in the document).
float viewBlockHeight(const chat::ViewSpec& v, ViewUiState& st, float wrap, float lineH);
// Draw the block into [min, max] (what viewBlockHeight asked for). `scope` makes the ImGui ids
// unique per block; `via` is the Evaluation behind the Message ("" when none).
ViewActions drawViewBlock(const chat::ViewSpec& v, ViewUiState& st, ImVec2 min, ImVec2 max, ImFont* mono,
                          const std::string& scope, const chat::NameOf& nameOf, const std::string& via, float lineH);
// The expanded window for a table or an image (st.expanded); draws nothing when not expanded.
ViewActions drawExpandedView(const chat::ViewSpec& v, ViewUiState& st, ImFont* mono, const std::string& scope,
                             const chat::NameOf& nameOf);
// The kind chip's colour.
ImU32 viewKindColor(const std::string& kind);

} // namespace ke::ui
