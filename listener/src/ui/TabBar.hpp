// The tab bar: one component for the terminal area and every pane group. Tabs join the content
// they own: the active one is open into it, a baseline runs under the rest. Each tab carries a
// kind mark, its name and a close `x`, shown on hover and on the active tab; a middle click also
// closes. When tabs do not fit, the strip scrolls and a list menu at the end names them all.
#pragma once

#include "model/Activity.hpp"

#include <imgui.h>
#include <string>
#include <vector>

namespace ke::ui {

struct TabItem {
    std::string id;    // stable across frames; the drive target is `tab:<label>`, `close:<label>`
    std::string glyph; // a short ASCII kind mark: `>_`, `md`, `(i)`
    std::string label;
    std::string note;      // a dim word after the name, saying what kind of tab this is: `inspector`
    std::string target;    // the drive target when the label alone would not be unique
    bool dirty = false;    // `*` beside the name
    bool closable = true;
    bool pinned = false;    // leading tabs that never scroll away: a view stays in reach however many panes
    bool attention = false; // something happened here: a model typed
    bool alarm = false;     // a small red mark: unsafe mode
    int badge = 0;          // unread here: a count beside the name
    model::Activity activity = model::Activity::None; // an agent's mark, before the name
};

struct TabBarState {
    float scroll = 0.0f;   // pixels the strip is scrolled left
    int lastActive = -1;   // scrolled into view when it changes
};

struct TabBarResult {
    int picked = -1; // clicked this frame
    int closed = -1; // `x` or middle click
    int dragged = -1; // being dragged out this frame; the caller starts the drag and drop
    int menu = -1;   // right clicked
};

float tabBarHeight();
// One row, `width` wide, at the cursor. The cursor ends just under the baseline, so the content
// that follows reads as the active tab's body.
TabBarResult tabBar(const char* id, const std::vector<TabItem>& tabs, int active, float width, TabBarState& state);

// The strip subordinate to the bar: small words on the content's own surface, under its tab.
int subTabs(const char* id, const char* const* names, int count, int active);

} // namespace ke::ui
