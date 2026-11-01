// ImGui's item hooks, built into the imgui target. Every text field reports itself here as it is
// laid out, so the right-click menu covers each one, present and future, with no call at the site.
#include "ui/ImguiHooks.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace ke::ui {

InputSeen g_seen;

const InputSeen& inputUnderMouse() { return g_seen; }
void clearInputSeen() { g_seen = InputSeen{}; }
void enableItemHooks() { ImGui::GetCurrentContext()->TestEngineHookItems = true; }

} // namespace ke::ui

namespace ke::ui {
extern InputSeen g_seen;
}

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* item) {
    if (item == nullptr || (item->ItemFlags & ImGuiItemFlags_Inputable) == 0) {
        return;
    }
    if (bb.Contains(ctx->IO.MousePos) && ctx->CurrentWindow != nullptr) {
        ke::ui::InputSeen seen;
        seen.id = id;
        seen.window = ctx->CurrentWindow->ID;
        ke::ui::g_seen = seen;
    }
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID, const char*, ImGuiItemStatusFlags) {}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
