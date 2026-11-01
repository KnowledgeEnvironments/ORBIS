#include "ui/TextMenu.hpp"

#include "drive/Commands.hpp"
#include "platform/Platform.hpp"
#include "ui/ImguiHooks.hpp"
#include "ui/Theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace ke::ui {

namespace {

enum class Op { None, Cut, Copy, Paste, SelectAll };

struct Pending {
    Op op = Op::None;
    ImGuiID id = 0;
    int stage = 0;
};

Pending g_pending;
ImGuiKey g_shortcut = ImGuiKey_None;
int g_shortcutStage = 0;
ImGuiID g_menuField = 0;

ImGuiKey keyOf(Op op) {
    switch (op) {
    case Op::Cut:
        return ImGuiKey_X;
    case Op::Copy:
        return ImGuiKey_C;
    case Op::Paste:
        return ImGuiKey_V;
    case Op::SelectAll:
        return ImGuiKey_A;
    case Op::None:
        break;
    }
    return ImGuiKey_None;
}

} // namespace

void sendShortcut(ImGuiKey key) {
    g_shortcut = key;
    g_shortcutStage = 0;
}

void textMenuPump() {
    if (g_shortcut != ImGuiKey_None) {
        ImGuiIO& io = ImGui::GetIO();
        const ImGuiKey mod = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        if (g_shortcutStage++ == 0) {
            io.AddKeyEvent(mod, true);
            io.AddKeyEvent(g_shortcut, true);
        } else {
            io.AddKeyEvent(g_shortcut, false);
            io.AddKeyEvent(mod, false);
            g_shortcut = ImGuiKey_None;
        }
    }
    if (g_pending.op == Op::None) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiKey mod = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    switch (g_pending.stage++) {
    case 0:
        ImGui::ActivateItemByID(g_pending.id); // the menu took the keyboard; give it back
        break;
    case 1:
        break; // the field is active from the next frame
    case 2:
        io.AddKeyEvent(mod, true);
        io.AddKeyEvent(keyOf(g_pending.op), true);
        break;
    default:
        io.AddKeyEvent(keyOf(g_pending.op), false);
        io.AddKeyEvent(mod, false);
        g_pending = Pending{};
        break;
    }
}

void textMenu() {
    const InputSeen seen = inputUnderMouse();
    clearInputSeen();
    const ImGuiIO& io = ImGui::GetIO();
    const bool longPress = io.MouseSource == ImGuiMouseSource_TouchScreen && io.MouseDownDuration[0] > 0.5f &&
                           !ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    if (seen.id != 0 && (ImGui::IsMouseReleased(ImGuiMouseButton_Right) || longPress) && !ImGui::IsPopupOpen("##textmenu")) {
        g_menuField = seen.id;
        ImGui::OpenPopup("##textmenu");
        if (longPress) {
            platform::haptic(platform::Haptic::Tap);
        }
    }
    if (!ImGui::BeginPopup("##textmenu", ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    const auto item = [](const char* word, const char* target, Op op) {
        if (menuItem(word)) {
            g_pending = Pending{op, g_menuField, 0};
        }
        drive::hit(target, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    };
    item("CUT", "menu:cut", Op::Cut);
    item("COPY", "menu:copy", Op::Copy);
    item("PASTE", "menu:paste", Op::Paste);
    ImGui::Separator();
    item("SELECT ALL", "menu:select-all", Op::SelectAll);
    ImGui::EndPopup();
}

} // namespace ke::ui
