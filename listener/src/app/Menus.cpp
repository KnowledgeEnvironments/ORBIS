#include "app/Menus.hpp"

#include "app/Runtime.hpp"
#include "drive/Commands.hpp"
#include "modules/Registry.hpp"
#include "platform/Platform.hpp"
#include "ui/Fonts.hpp"
#include "ui/TextMenu.hpp"
#include "ui/Themes.hpp"
#include "ui/Theme.hpp"
#include "util/Product.hpp"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace ke::app {

namespace {

constexpr float kFontStep = 1.0f, kFontMin = 9.0f, kFontMax = 28.0f;
constexpr float kFontSizes[] = {11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 18.0f, 20.0f, 24.0f};

struct Item {
    const char* section;
    const char* label;
    const char* shortcut;
    const char* hover;
    std::function<void()> run;
    std::string disabledWhy; // empty: enabled
    const bool* checked = nullptr;
};

// The popups' state: one of each, whichever frame draws them.
struct Popups {
    bool openFile = false;
    bool shortcuts = false;
    bool things = false;
    bool about = false;
    std::string path;
} g_popups;

// A shortcut's effect on whatever has the keyboard: the same event a key press makes.
void chord(ImGuiKey key) { ui::sendShortcut(key); }

void bumpFonts(FrameInputs& in, float delta) {
    in.prefs.fontUiSize = std::clamp(in.prefs.fontUiSize + delta, kFontMin, kFontMax);
    in.prefs.fontMonoSize = std::clamp(in.prefs.fontMonoSize + delta, kFontMin, kFontMax);
    in.saveWhimsy();
}

void resetFonts(FrameInputs& in) {
    in.prefs.fontUiSize = 14.0f;
    in.prefs.fontMonoSize = 14.0f;
    in.saveWhimsy();
}

std::vector<Item> items(MenuHost h) {
    FrameInputs& in = h.in;
    Runtime& rt = in.rt;
    Panes& p = in.panes;
    const bool attached = rt.session.attached();
    const std::string notAttached = attached ? "" : "not attached to a world";
    const std::string principal = p.terminal.current();
    const std::string noTerminal = principal.empty() ? "no chat is open" : "";
    const auto view = [&p](int v) {
        return [&p, v] {
#if !LISTENER_PHONE
            bringDockWindowFront("CHAT");
#endif
            p.terminal.setView(v);
        };
    };
    std::vector<Item> out;
    const auto add = [&out](const char* section, const char* label, const char* keys, const char* hover, std::function<void()> run,
                            std::string why = {}, const bool* checked = nullptr) {
        out.push_back(Item{section, label, keys, hover, std::move(run), std::move(why), checked});
    };
    // FILE
    add("FILE", "NEW PRINCIPAL", "", "create a Principal in this world", [&] { p.openAction(rt, "principal.create"); }, notAttached);
    add("FILE", "NEW PANE", "", "a terminal or file manager on a shell object", [&] {
#if !LISTENER_PHONE
        bringDockWindowFront("PANES");
#endif
    });
    add("FILE", "OPEN FILE", "Ctrl-O", "open a path in the right module", [] { g_popups.openFile = true; }, notAttached);
    add("FILE", "CLOSE TAB", "Ctrl-W", "close the focused pane's tab", [&] { p.mux.closeFocused(); });
    add("FILE", "OFF THE RECORD", "", "a conversation with the current Principal that the log never keeps",
        [&p, principal] { p.scratch.ask(principal); }, noTerminal); // by value: the menu item runs after this frame's locals
    // EDIT
    add("EDIT", "CUT", "Ctrl-X", "cut the selection", [] { chord(ImGuiKey_X); });
    add("EDIT", "COPY", "Ctrl-C", "copy the selection", [] { chord(ImGuiKey_C); });
    add("EDIT", "PASTE", "Ctrl-V", "paste from the clipboard", [] { chord(ImGuiKey_V); });
    add("EDIT", "SELECT ALL", "Ctrl-A", "select everything in the focused text", [] { chord(ImGuiKey_A); });
    add("EDIT", "FIND", "Ctrl-F", "search Fields, names and Messages", [&] { p.history.openSearch(); });
    // VIEW
#if !LISTENER_PHONE
    add("VIEW", "CHAT", "", "the conversation area", [] { bringDockWindowFront("CHAT"); });
    add("VIEW", "GRAPH", "", "the world as a graph", [] { bringDockWindowFront("GRAPH"); });
    add("VIEW", "PANES", "", "terminals, files, editors and viewers", [] { bringDockWindowFront("PANES"); });
    add("VIEW", "TWILIGHT MAP", "", "the map over a starfield and a low dusk", [&] {
        in.prefs.mapTwilight = !in.prefs.mapTwilight;
        in.saveWhimsy();
    }, {}, &in.prefs.mapTwilight);
    if (h.desktop != nullptr) {
        DesktopFrame* d = h.desktop;
        add("VIEW", "LEFT COLUMN", "", "show or hide PRINCIPALS", [&p, d] {
            if (d->leftWanted) {
                p.mux.hideView("view:principals");
            } else {
                bringDockWindowFront("PRINCIPALS");
            }
        }, {}, &d->leftWanted);
        add("VIEW", "RESET LAYOUT", "", "every view back where it started; your panes stay open", [&p] {
            p.mux.resetLayout();
            p.history.collapsed = true;
        });
    }
#endif
    add("VIEW", "HISTORY", "", "fold the event log away, or bring it back", [&] {
#if !LISTENER_PHONE
        if (!p.mux.viewOpen("view:history")) {
            p.history.collapsed = false;
            in.prefs.historyOpen = true;
            in.saveWhimsy();
            bringDockWindowFront("HISTORY");
            return;
        }
#endif
        p.history.collapsed = !p.history.collapsed;
        in.prefs.historyOpen = !p.history.collapsed;
        in.saveWhimsy();
    });
    add("VIEW", "SHOW REASONING", "", "open each reply with the model's reasoning above it", [&] {
        in.prefs.thinking = !in.prefs.thinking;
        p.terminal.showThinking = in.prefs.thinking;
        in.saveWhimsy();
    }, {}, &in.prefs.thinking);
    add("VIEW", "TEXT LARGER", "Ctrl-+", "larger text everywhere", [&] { bumpFonts(in, kFontStep); });
    add("VIEW", "TEXT SMALLER", "Ctrl--", "smaller text everywhere", [&] { bumpFonts(in, -kFontStep); });
    add("VIEW", "TEXT DEFAULT", "Ctrl-0", "back to the default size", [&] { resetFonts(in); });
    // WORLD
#if !LISTENER_PHONE
    if (h.desktop != nullptr) {
        DesktopFrame* d = h.desktop;
        add("WORLD", "SWITCH WORLD", "", "attach to another world, or add one", [d] { d->worldPickerWanted = true; });
    }
#endif
    add("WORLD", "APPROVALS", "", "everything waiting on a decision", [&] { p.approvals.toggle(); });
    add("WORLD", "NOTIFICATIONS", "", "what the world's Principals have told you", [&] { p.notices.toggle(); });
#if !LISTENER_PHONE
    add("WORLD", "OS NOTIFICATIONS", "", "attention and urgent notices reach the desktop while this window is behind", [&] {
        in.prefs.osNotices = !in.prefs.osNotices;
        in.saveWhimsy();
    }, {}, &in.prefs.osNotices);
#endif
    add("WORLD", "RULES", "", "the current Principal's standing rules", view(5), noTerminal);
    add("WORLD", "CONVERSATIONS", "", "the current Principal's conversations", view(6), noTerminal);
    add("WORLD", "DEVICES", "", "tokens and devices", view(4), noTerminal);
    add("WORLD", "PAIR A DEVICE", "", "a code or link for a phone", view(4), noTerminal);
    add("WORLD", "COMPRESS", "", "compress the current conversation now", [&] { p.terminal.compressCurrent(rt.principals, rt.chat, rt.session); },
        noTerminal.empty() ? notAttached : noTerminal);
    // HELP
    add("HELP", "SHOW DOCUMENTATION", "", "the world's manual: search it, read it, bookmark it", [&] { p.manual.toggle(); });
    add("HELP", "KEYBOARD SHORTCUTS", "", "every key the listener answers to", [] { g_popups.shortcuts = true; });
    add("HELP", "WHAT THINGS ARE", "", "Principals, Fields, panes, Grants, in a line each", [] { g_popups.things = true; });
    add("HELP", "ABOUT", "", "version and world", [] { g_popups.about = true; });
    return out;
}

void menuItem(const Item& i) {
    const bool enabled = i.disabledWhy.empty();
    const bool picked = ui::menuItem(i.label, i.shortcut[0] != 0 ? i.shortcut : nullptr, i.checked != nullptr && *i.checked, enabled);
    drive::hit(std::string("menu:") + i.label, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", enabled ? i.hover : i.disabledWhy.c_str());
    }
    if (picked) {
        i.run();
    }
}

void fontMenu(FrameInputs& in) {
    const std::vector<ui::FontOption> fonts = ui::fontOptions(in.assetsDir);
    // every font is offered for both: a person's own proportional face is theirs to pick for code too
    const auto choose = [&](const char* title, const char* drive, std::string& family, float& size) {
        if (!ui::beginMenu(title)) {
            return;
        }
        drive::hit(drive, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ui::menuItem("THEME DEFAULT", nullptr, family.empty())) {
            family.clear();
            in.saveWhimsy();
        }
        for (const ui::FontOption& f : fonts) {
            if (ui::menuItem(f.name.c_str(), nullptr, family == f.name)) {
                family = f.name;
                in.saveWhimsy();
            }
            drive::hit("font:" + f.name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ImGui::Separator();
        // a size is picked, not dragged: the menu resizes with the font under the pointer
        for (const float s : kFontSizes) {
            const std::string word = std::to_string(static_cast<int>(s));
            if (ui::menuItem(word.c_str(), nullptr, size == s)) {
                size = s;
                in.saveWhimsy();
            }
            drive::hit(std::string(drive) + ":" + word, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ImGui::EndMenu();
    };
    choose("INTERFACE FONT", "menu:interface-font", in.prefs.fontUi, in.prefs.fontUiSize);
    choose("MONOSPACE FONT", "menu:monospace-font", in.prefs.fontMono, in.prefs.fontMonoSize);
    if (ui::beginMenu("LABELS")) {
        for (const auto& [word, value] : {std::pair<const char*, const char*>{"THEME DEFAULT", ""}, {"CAPS", "caps"}, {"SENTENCE", "sentence"}}) {
            if (ui::menuItem(word, nullptr, in.prefs.labelCase == value)) {
                in.prefs.labelCase = value;
                in.saveWhimsy();
            }
            drive::hit(std::string("labels:") + word, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ImGui::EndMenu();
    }
    // The theme, only when there is a choice: a theme file in the data dir.
    const auto& themes = ui::themeLibrary().all();
    if (themes.size() > 1 && ui::beginMenu("THEME")) {
        for (const ui::ThemeSpec& t : themes) {
            if (ui::menuItem(t.title.c_str(), nullptr, in.prefs.theme == t.name)) {
                in.prefs.theme = t.name;
                in.saveWhimsy();
            }
        }
        ImGui::EndMenu();
    }
}

void shortcutsFire(MenuHost h) {
    FrameInputs& in = h.in;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Equal) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd)) {
        bumpFonts(in, kFontStep);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Minus) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract)) {
        bumpFonts(in, -kFontStep);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0)) {
        resetFonts(in);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
        g_popups.openFile = true;
    }
}

} // namespace

void brandMark() {
    // edge to edge of the row, cut by it: no padding above or below the bars
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 win = ImGui::GetWindowPos();
    const float top = std::floor(win.y);
    const float h = std::ceil(ImGui::GetWindowHeight());
    const float slant = std::floor(h * 0.45f);
    const float bar = std::max(2.0f, std::floor(h * 0.26f));
    const float gap = std::max(1.0f, std::floor(h * 0.14f));
    float x = std::floor(at.x);
    dl->PushClipRect(ImVec2(win.x, top), ImVec2(win.x + ImGui::GetWindowWidth(), top + h), false);
    for (int i = 0; i < 3; ++i) {
        const ImVec2 quad[4] = {ImVec2(x + slant, top), ImVec2(x + slant + bar, top), ImVec2(x + bar, top + h), ImVec2(x, top + h)};
        dl->AddConvexPolyFilled(quad, 4, ui::theme().brand[i]);
        x += bar + gap;
    }
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(x - at.x + slant, ImGui::GetTextLineHeight()));
}

void drawMenus(MenuHost h) {
    const std::vector<Item> all = items(h);
    for (const char* section : {"FILE", "EDIT", "VIEW", "WORLD", "HELP"}) {
        if (!ui::beginMenu(section)) {
            drive::hit(std::string("menu:") + section, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            continue;
        }
        drive::hit(std::string("menu:") + section, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        for (const Item& i : all) {
            if (std::string_view(i.section) == section) {
                menuItem(i);
            }
        }
        if (std::string_view(section) == "VIEW") {
            ImGui::Separator();
            fontMenu(h.in);
        }
        ImGui::EndMenu();
    }
    shortcutsFire(h);
}

void drawMenusFlat(MenuHost h) {
    const std::vector<Item> all = items(h);
    for (const char* section : {"FILE", "EDIT", "VIEW", "WORLD", "HELP"}) {
        ui::dimText("%s", section);
        for (const Item& i : all) {
            if (std::string_view(i.section) != section) {
                continue;
            }
            const bool enabled = i.disabledWhy.empty();
            if (ui::bracketButtonNamed(i.label, std::string("menu-") + i.label, enabled, i.checked != nullptr && *i.checked)) {
                i.run();
            }
            if (!enabled) {
                ImGui::SameLine();
                ui::dimText("%s", i.disabledWhy.c_str());
            }
        }
    }
}

void drawMenuPopups(MenuHost h) {
    FrameInputs& in = h.in;
    if (g_popups.openFile) {
        g_popups.openFile = false;
        in.panes.openFile.open();
    }
    if (ui::symbolUi().takeAttachImage()) {
        chat::Chat* chat = &in.rt.chat;
        ui::TerminalPane* terminal = &in.panes.terminal;
        in.panes.openFile.pick([chat, terminal](const std::string& mount, const std::string& path) {
            terminal->addImageAt(*chat, mount, path);
        });
    }
    in.panes.openFile.draw(in.rt.chat, in.rt.principals);
    const auto textPopup = [](bool& flag, const char* title, const char* body) {
        if (flag) {
            flag = false;
            ImGui::OpenPopup(title);
        }
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(body);
            ui::dialogGap();
            if (ui::bracketButtonNamed("CLOSE", std::string("close-") + title) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    };
    textPopup(g_popups.shortcuts, "KEYBOARD SHORTCUTS",
              "Ctrl is Cmd on a Mac, except Ctrl-arrows.\n\n"
              "Ctrl-O        open a file\n"
              "Ctrl-W        close the focused tab\n"
              "Ctrl-F        search\n"
              "Ctrl-+ - 0    text size\n"
              "Ctrl-arrows   move between panes\n"
              "Ctrl-C X V A  copy, cut, paste, select all\n"
              "click         open a file; an agent's chat\n"
              "shift-click   inspect\n"
              "F2 F5 F6 F7   rename, copy, move, mkdir in FILES\n");
    textPopup(g_popups.things, "WHAT THINGS ARE",
              "A Principal acts in the world: a person, a model, a shell.\n"
              "A Field is a named value on an Object. An Object is anything with an id.\n"
              "A Grant says who may do what to what.\n"
              "A pane is a live surface on an Object; it lives in the world, not in this client.\n"
              "HISTORY is the event log; CTX is how much of the model's context a conversation uses.\n");
    textPopup(g_popups.about, "ABOUT", (std::string(kProductName) + ": " + kProductExpansion + ".\n" + kTagline + "\n").c_str());
}

} // namespace ke::app
