#include "app/DebugHooks.hpp"

#include "app/Lifecycle.hpp"
#include "app/Runtime.hpp"
#include "util/Log.hpp"

#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>
#include <imgui_internal.h>

namespace ke::app {

DebugHooks::DebugHooks(Clock::time_point started) {
#if !LISTENER_PHONE
    shotFile = std::getenv("LISTENER_SHOT");
    const char* after = std::getenv("LISTENER_SHOT_AFTER_MS");
    shotAt = started + std::chrono::milliseconds(after ? std::atoi(after) : 4000);
    shotType = std::getenv("LISTENER_SHOT_TYPE");
    shotWhen = std::getenv("LISTENER_SHOT_WHEN");
    startPane = std::getenv("LISTENER_START_PANE");
    // The tab is selected for the first two seconds: attach opens the own terminal with a focus
    // request, which would otherwise pull CHAT back in front.
    startPaneUntil = started + std::chrono::milliseconds(startPane ? 2000 : 0);
    idCheck = std::getenv("LISTENER_IDCHECK");
    if (idCheck) {
        const std::string spec = idCheck;
        if (const auto colon = spec.find(':'); colon != std::string::npos) {
            sweepStep = std::max(2, std::atoi(spec.c_str() + colon + 1));
        }
        const char* sweepAfter = std::getenv("LISTENER_IDCHECK_AFTER_MS");
        sweepAt = started + std::chrono::milliseconds(sweepAfter ? std::atoi(sweepAfter) : 3000);
    }
#else
    (void)started;
#endif
}

bool DebugHooks::keepDrawing(Clock::time_point now) const {
    if (sweeping()) {
        return true;
    }
    if (!shotFile) {
        return false;
    }
    if (shotWhen) {
        return true; // a condition is watched every frame until it holds or the deadline passes
    }
    return now >= shotAt - std::chrono::milliseconds(200);
}

bool DebugHooks::graphInFront(Clock::time_point now) const {
    return startPane && std::string(startPane) == "GRAPH" && now < startPaneUntil;
}

bool DebugHooks::shotDue(Runtime& rt, Clock::time_point now) {
    if (!shotFile) {
        return false;
    }
    if (shotType && !shotTyped && rt.session.attached() && rt.chat.routeState() == chat::RouteState::Found) {
        shotTyped = true;
        rt.chat.submit(shotType);
    }
    if (shotWhen && rt.session.principal()) {
        const std::string when = shotWhen;
        const std::string& me = *rt.session.principal();
        const model::EvaluationInfo* cur = rt.evaluations.latest(me);
        // Only Evaluations that began after the typed line count, so a stale one cannot trigger.
        if (shotType && !shotTyped) {
            return now >= shotAt;
        }
        if (cur && !cur->done() && !cur->runningAt.empty()) {
            typedSeen = true;
        }
        const bool live = cur && !cur->done() && !cur->runningAt.empty();
        if (when == "thinking" && live && !cur->replying()) {
            // give the counter a few seconds so the line shows elapsed time
            return now >= shotAt;
        }
        if (when == "streaming" && live && cur->replying()) {
            return true;
        }
        if (when == "done" && typedSeen && cur && cur->done()) {
            return true;
        }
        // the deadline is the fallback for every condition
        return now >= shotAt + std::chrono::milliseconds(60000);
    }
    return now >= shotAt;
}

int DebugHooks::sweepBegin(Gpu& gpu, Runtime& rt, Clock::time_point now) {
    if (!sweeping() || now < sweepAt || !rt.session.attached()) {
        return -1;
    }
    if (sweepPass < 0) {
        SDL_GetWindowSize(gpu.window, &sweepW, &sweepH);
        sweepPass = 0;
        sweepX = sweepY = 0;
        // Vsync would make the sweep a frame per point; present immediately when the driver can.
        if (gpu.device && !sweepPresentSet) {
            sweepPresentSet = true;
            for (SDL_GPUPresentMode mode : {SDL_GPU_PRESENTMODE_IMMEDIATE, SDL_GPU_PRESENTMODE_MAILBOX}) {
                if (SDL_WindowSupportsGPUPresentMode(gpu.device, gpu.window, mode) &&
                    SDL_SetGPUSwapchainParameters(gpu.device, gpu.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
                    break;
                }
            }
        }
        log::info("idsweep: {}x{} step {} ({} points per pass, 4 passes)", sweepW, sweepH, sweepStep,
                  (sweepW / sweepStep + 1) * (sweepH / sweepStep + 1));
    }
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(sweepX) + 0.5f, static_cast<float>(sweepY) + 0.5f);
    return sweepPass;
}

bool DebugHooks::sweepEnd() {
    if (!sweeping() || sweepPass < 0) {
        return false;
    }
    ++sweepFrames;
    // ImGui flags a conflict in the frame after the hover, so it belongs to the previous point.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.DebugDrawIdConflictsId != 0 && prevX >= 0) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "pass %d (%s) at %d,%d: %d items share id %08x", prevPass,
                      prevPass == 0 ? "transcript" : prevPass == 1 ? "prompt" : prevPass == 2 ? "fields" : "graph",
                      prevX, prevY, g.HoveredIdPreviousFrameItemCount, g.DebugDrawIdConflictsId);
        const std::string key = std::to_string(prevPass) + ":" + std::to_string(g.DebugDrawIdConflictsId);
        if (key != lastConflictKey) { // one line per run of the same conflict
            conflicts.emplace_back(buf);
            log::warn("idsweep: {}", buf);
        }
        lastConflictKey = key;
    } else {
        lastConflictKey.clear();
    }
    prevX = sweepX;
    prevY = sweepY;
    prevPass = sweepPass;
    sweepX += sweepStep;
    if (sweepX > sweepW) {
        sweepX = 0;
        sweepY += sweepStep;
    }
    if (sweepY > sweepH) {
        sweepY = 0;
        prevX = -1;
        ++sweepPass;
        if (sweepPass > 3) {
            sweepDone = true;
            log::info("idsweep: done, {} frames, {} id conflict(s), {} recoverable imgui error(s)", sweepFrames,
                      conflicts.size(), imguiErrorCount());
            return true;
        }
    }
    return false;
}

int DebugHooks::sweepExitCode() const {
    return conflicts.empty() && imguiErrorCount() == 0 ? 0 : 3;
}

bool dumpFrame(Gpu& gpu, const char* file) {
    ui::outlineFloatingWindows();
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    const Uint32 w = static_cast<Uint32>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const Uint32 h = static_cast<Uint32>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (w == 0 || h == 0) {
        return false;
    }
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GetGPUSwapchainTextureFormat(gpu.device, gpu.window);
    tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tci.width = w;
    tci.height = h;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(gpu.device, &tci);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(gpu.device, &tbi);
    if (!tex || !tb) {
        log::error("shot: {}", SDL_GetError());
        return false;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device);
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, cmd);
    SDL_GPUColorTargetInfo target{};
    target.texture = tex;
    target.clear_color = SDL_FColor{0, 0, 0, 1};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(drawData, cmd, pass);
    SDL_EndGPURenderPass(pass);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = tex;
    src.w = w;
    src.h = h;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
    SDL_ReleaseGPUFence(gpu.device, fence);
    void* pixels = SDL_MapGPUTransferBuffer(gpu.device, tb, false);
    bool ok = false;
    if (pixels) {
        const bool bgra = tci.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                          tci.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
        SDL_Surface* surf = SDL_CreateSurfaceFrom(static_cast<int>(w), static_cast<int>(h),
                                                  bgra ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32, pixels,
                                                  static_cast<int>(w * 4));
        ok = surf && SDL_SaveBMP(surf, file);
        SDL_DestroySurface(surf);
        SDL_UnmapGPUTransferBuffer(gpu.device, tb);
    }
    SDL_ReleaseGPUTransferBuffer(gpu.device, tb);
    SDL_ReleaseGPUTexture(gpu.device, tex);
    log::info("shot: {} {}x{} -> {}", ok ? "wrote" : "failed", w, h, file);
    return ok;
}

} // namespace ke::app
