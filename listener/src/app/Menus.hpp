// Everything the app can do, as one list: the desktop draws it as the menu bar, the phone in its
// Settings tab. Each item shows its shortcut, says why it is off, and runs the same function the
// rest of the app does.
#pragma once

#include "app/Layout.hpp"

#include <string>

namespace ke::app {

struct DesktopFrame; // desktop builds only

struct MenuHost {
    FrameInputs& in;
    DesktopFrame* desktop; // null on the phone
};

// The bar's menus, after the mark. Also the shortcuts: they fire with the menus closed.
void drawMenus(MenuHost host);
// The same list as sections, for a touch screen.
void drawMenusFlat(MenuHost host);
// Popups the items opened (open file, shortcuts, about). Once per frame, outside the bar.
void drawMenuPopups(MenuHost host);
// The three bars of the mark, sized to `height`, at the cursor.
void brandMark();

} // namespace ke::app
