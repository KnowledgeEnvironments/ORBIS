#include "ui/ImguiHooks.hpp"
#include "app/Lifecycle.hpp"

#include "app/FrameFit.hpp"

#include "ui/Textures.hpp"

#include <cstdlib>
#include <imgui_internal.h>

namespace ke::app {
namespace {
int g_imguiErrors = 0;
}
int imguiErrorCount() {
    return g_imguiErrors;
}
} // namespace ke::app

#include "app/Runtime.hpp"
#include "ui/Theme.hpp"
#include "ui/Themes.hpp"
#include "util/Log.hpp"
#include "util/Product.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#if LISTENER_SDL_RENDERER_FALLBACK
#include <imgui_impl_sdlrenderer3.h>
#endif

namespace ke::app {

int bringUp(Gpu& gpu, const Options& opts) {
    // User rule: SDL disables the screensaver by default; the listener must not.
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log::error("SDL_Init: {}", SDL_GetError());
        return 1;
    }
#if defined(SDL_PLATFORM_IOS)
    const SDL_WindowFlags flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
#if defined(SDL_PLATFORM_IOS)
    // The window is the screen, in the orientation the device is held: a request wider than tall
    // (the desktop's 1440x900) made SDL turn a portrait phone on its side, with the safe-area
    // bands of the wrong edges (iPhone Duo simulator, 2026-11-11).
    int ww = 1440, wh = 900;
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &bounds) && bounds.w > 0 && bounds.h > 0) {
        ww = bounds.w;
        wh = bounds.h;
    }
    gpu.window = SDL_CreateWindow(kProductName, ww, wh, flags);
#else
    gpu.window = SDL_CreateWindow(kProductName, 1440, 900, flags);
#endif
    if (!gpu.window) {
        log::error("SDL_CreateWindow: {}", SDL_GetError());
        return 1;
    }
#if !LISTENER_PHONE
    // On iOS the icon comes from the asset catalog; here SDL sets the window's and the Dock's.
    if (SDL_Surface* icon = SDL_LoadPNG((opts.assetsDir + "/appicon.png").c_str())) {
        SDL_SetWindowIcon(gpu.window, icon);
        SDL_DestroySurface(icon);
    } else {
        log::warn("no app icon: {}", SDL_GetError());
    }
#endif
    if (!SDL_ScreenSaverEnabled()) {
        log::error("screensaver check FAILED: SDL reports the screensaver disabled after window creation");
        SDL_DestroyWindow(gpu.window);
        SDL_Quit();
        return 3;
    }
    log::info("screensaver check OK: SDL_ScreenSaverEnabled() is true");

    // SDL_GetWindowDisplayScale is pixel density * content scale; the backend already applies
    // the pixel density through io.DisplayFramebufferScale, so the UI scale is the quotient.
    const float displayScale = SDL_GetWindowDisplayScale(gpu.window);
    const float pixelDensity = SDL_GetWindowPixelDensity(gpu.window);
    gpu.uiScale = pixelDensity > 0.0f ? displayScale / pixelDensity : 1.0f;
#if LISTENER_PHONE
    // Points on a phone are read at arm's length: 14 px rows become ~16.
    gpu.uiScale = (gpu.uiScale > 0.0f ? gpu.uiScale : 1.0f) * 1.15f;
#endif
    log::info("dpi: display scale {:.2f}, pixel density {:.2f}, ui scale {:.2f}", displayScale, pixelDensity,
              gpu.uiScale);

    gpu.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB | SDL_GPU_SHADERFORMAT_SPIRV,
#ifndef NDEBUG
                                     true,
#else
                                     false,
#endif
                                     nullptr);
#if LISTENER_SDL_RENDERER_FALLBACK
    if (gpu.device && !SDL_ClaimWindowForGPUDevice(gpu.device, gpu.window)) {
        log::warn("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
        SDL_DestroyGPUDevice(gpu.device);
        gpu.device = nullptr;
    }
#endif
    if (!gpu.device) {
#if LISTENER_SDL_RENDERER_FALLBACK
        log::warn("no usable SDL_GPU device ({}); falling back to SDL_Renderer", SDL_GetError());
        gpu.renderer = SDL_CreateRenderer(gpu.window, nullptr);
        if (!gpu.renderer) {
            log::error("SDL_CreateRenderer: {}", SDL_GetError());
            return 1;
        }
        SDL_SetRenderVSync(gpu.renderer, 1);
        log::info("gpu: driver {} (SDL_Renderer)", SDL_GetRendererName(gpu.renderer));
#else
        log::error("SDL_CreateGPUDevice: {}", SDL_GetError());
        return 1;
#endif
    } else {
#if !LISTENER_SDL_RENDERER_FALLBACK
        if (!SDL_ClaimWindowForGPUDevice(gpu.device, gpu.window)) {
            log::error("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
            return 1;
        }
#endif
        SDL_SetGPUSwapchainParameters(gpu.device, gpu.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_VSYNC);
        log::info("gpu: driver {}", SDL_GetGPUDeviceDriver(gpu.device));
    }
    SDL_SetWindowPosition(gpu.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(gpu.window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // layout is rebuilt each start; nothing written next to the binary
    io.ConfigInputTextCursorBlink = true; // the block cursor (ui::blockCursor) does the blinking
    // ImGui's error recovery stays on, but it never opens its Debug Log window or an error
    // tooltip in front of the user: recoverable errors go to our log through the callback, are
    // counted (the id sweep and LISTENER_STRICT fail on them), and never assert in production.
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableDebugLog = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    // Id conflict detection (and its "Programmer error" tooltip) only while the id sweep runs.
    io.ConfigDebugHighlightIdConflicts = std::getenv("LISTENER_IDCHECK") != nullptr;
    io.ConfigDebugHighlightIdConflictsShowItemPicker = false;
    ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext*, void*, const char* msg) {
        ++g_imguiErrors;
        log::warn("imgui: {}", msg);
    };
#if LISTENER_PHONE
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges = false;
#endif
    ui::enableItemHooks();
    ui::textures::setAssetsDir(opts.assetsDir);
    ui::setTheme(ui::themeLibrary().named(""), gpu.uiScale); // the client's choice lands once prefs are read
    gpu.fonts = ui::loadFonts(opts.assetsDir);

#if LISTENER_SDL_RENDERER_FALLBACK
    if (gpu.renderer != nullptr) {
        ImGui_ImplSDL3_InitForSDLRenderer(gpu.window, gpu.renderer);
        ImGui_ImplSDLRenderer3_Init(gpu.renderer);
        ui::textures::initRenderer(gpu.renderer); // image views
        return 0;
    }
#endif
    ImGui_ImplSDL3_InitForSDLGPU(gpu.window);
    ImGui_ImplSDLGPU3_InitInfo init{};
    init.Device = gpu.device;
    init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu.device, gpu.window);
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);
    ui::textures::init(gpu.device); // image views
    return 0;
}

void newFrame(Gpu& gpu) {
#if LISTENER_SDL_RENDERER_FALLBACK
    if (gpu.renderer != nullptr) {
        ImGui_ImplSDLRenderer3_NewFrame();
    } else
#endif
    {
        (void)gpu;
        ImGui_ImplSDLGPU3_NewFrame();
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void renderFrame(Gpu& gpu) {
    ui::outlineFloatingWindows();
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
#if LISTENER_SDL_RENDERER_FALLBACK
    if (gpu.renderer != nullptr) {
        const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(ui::Surface::background);
        SDL_SetRenderScale(gpu.renderer, drawData->FramebufferScale.x, drawData->FramebufferScale.y);
        SDL_SetRenderDrawColorFloat(gpu.renderer, bg.x, bg.y, bg.z, 1.0f);
        SDL_RenderClear(gpu.renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(drawData, gpu.renderer);
        SDL_RenderPresent(gpu.renderer);
        return;
    }
#endif
    const bool minimized = drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device);
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 swapW = 0, swapH = 0;
    SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gpu.window, &swapchain, &swapW, &swapH);
    // The swapchain can be a different size than the frame ImGui laid out (a resize or an iOS
    // background transition between NewFrame and here); drawing then trips Metal validation in
    // SDL_SetGPUViewport. Skip such a frame: the resize event schedules another. A difference of a
    // pixel or two is not a resize: it is rounding at a fractional pixel density (the iPhone Duo's
    // inner screen is 2.81), and refusing it left that screen black for good.
    const FrameFit fit = fitFrame(swapW, swapH, drawData->DisplaySize.x, drawData->DisplaySize.y,
                                  drawData->FramebufferScale.x, drawData->FramebufferScale.y);
    const bool sizeMatches = fit.draw;
    if (sizeMatches) {
        drawData->FramebufferScale = ImVec2(fit.scaleX, fit.scaleY);
    } else if (swapchain != nullptr && !minimized) {
        static int reported = 0;
        if (reported++ < 3) {
            log::warn("frame skipped: swapchain {}x{}, layout {}x{} at {}", swapW, swapH, drawData->DisplaySize.x,
                      drawData->DisplaySize.y, drawData->FramebufferScale.x);
        }
    }
    if (swapchain != nullptr && !minimized && sizeMatches) {
        ImGui_ImplSDLGPU3_PrepareDrawData(drawData, cmd);
        SDL_GPUColorTargetInfo target{};
        target.texture = swapchain;
        const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(ui::Surface::background);
        target.clear_color = SDL_FColor{bg.x, bg.y, bg.z, 1.0f};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(drawData, cmd, pass);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

void tearDown(Gpu& gpu) {
#if LISTENER_SDL_RENDERER_FALLBACK
    if (gpu.renderer != nullptr) {
        ui::textures::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(gpu.renderer);
    } else
#endif
    {
        SDL_WaitForGPUIdle(gpu.device);
        ui::textures::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(gpu.device, gpu.window);
        SDL_DestroyGPUDevice(gpu.device);
    }
    SDL_DestroyWindow(gpu.window);
    SDL_Quit();
}

#if LISTENER_PHONE

namespace {

bool lifecycleWatch(void* userdata, SDL_Event* ev) {
    auto* lc = static_cast<Lifecycle*>(userdata);
    // UIKit also reports "did become active" at launch and after system alerts, so only a real
    // background -> foreground transition reconnects.
    if (ev->type == SDL_EVENT_WILL_ENTER_BACKGROUND && !lc->background.exchange(true)) {
        if (*lc->runtime) {
            log::info("lifecycle: entering background, closing the socket (no detach)");
            (*lc->runtime)->client.suspend();
            lc->joinPending.store(true);
        }
        lc->reconnectPending.store(false);
    } else if (ev->type == SDL_EVENT_DID_ENTER_FOREGROUND && lc->background.exchange(false)) {
        lc->reconnectPending.store(true);
    }
    return true;
}

} // namespace

void Lifecycle::install() {
    SDL_AddEventWatch(lifecycleWatch, this);
}

void Lifecycle::remove() {
    SDL_RemoveEventWatch(lifecycleWatch, this);
}

void Lifecycle::settle() {
    if (!*runtime) {
        return;
    }
    if (joinPending.exchange(false)) {
        (*runtime)->client.stop(); // the thread has left or is leaving: a short join
    }
    if (reconnectPending.exchange(false)) {
        log::info("lifecycle: foreground, reconnecting");
        (*runtime)->client.reconnect();
    }
}

#endif // LISTENER_PHONE

} // namespace ke::app
