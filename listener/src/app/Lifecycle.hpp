// Bring-up and teardown of the window, the GPU device and the ImGui backends, the per-frame
// begin/end, and the iOS background/foreground handling. The screensaver stays enabled (user
// rule): the hint is set before SDL_Init and asserted after the window exists, so it cannot
// regress silently.
#pragma once

#include "app/Options.hpp"
#include "platform/Platform.hpp"
#include "ui/Fonts.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <memory>

#if defined(SDL_PLATFORM_IOS)
#include <TargetConditionals.h>
#endif
// SDL_GPU's Metal backend needs MTLGPUFamilyApple3, which the iOS simulator does not report, so
// simulator builds draw through SDL_Renderer (SDL's own Metal renderer) instead. Android has the
// same fallback for a device without a usable Vulkan (GLES through SDL_Renderer). iOS devices and
// macOS are SDL_GPU only.
#if (defined(SDL_PLATFORM_IOS) && TARGET_OS_SIMULATOR) || defined(SDL_PLATFORM_ANDROID)
#define LISTENER_SDL_RENDERER_FALLBACK 1
#else
#define LISTENER_SDL_RENDERER_FALLBACK 0
#endif

namespace ke {
class Runtime;
}

namespace ke::app {

struct Gpu {
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;
#if LISTENER_SDL_RENDERER_FALLBACK
    SDL_Renderer* renderer = nullptr;
#endif
    float uiScale = 1.0f;
    ui::Fonts fonts;
};

// SDL, the window (screensaver asserted), the GPU device or the simulator's renderer, the ImGui
// context with the theme and fonts, and the backends. Returns the process exit code: 0 on
// success, otherwise the error is logged and nothing is left to tear down.
int bringUp(Gpu& gpu, const Options& opts);
// Recoverable ImGui errors seen so far (routed to the log, never shown by ImGui itself).
int imguiErrorCount();
void newFrame(Gpu& gpu);
void renderFrame(Gpu& gpu);
void tearDown(Gpu& gpu);

#if LISTENER_PHONE
// Phone lifecycle. The OS delivers these while the app is being suspended and the main loop may
// not get another turn before the process is frozen, so the socket is closed (without detach,
// the world keeps the session) right in the event watch (SDL's README-ios). The watch must not
// block: SDL holds its watcher lock while calling it, and the socket thread takes that same lock
// in SDL_PushEvent for the wake event, so joining the thread here deadlocks (seen as a
// 0x8BADF00D watchdog kill). The join and the foreground reconnect, which reattaches with the
// persisted session and acked seqs, therefore run from the main loop (Lifecycle::settle). The
// watch also runs on the socket thread for the wake event, which it ignores.
struct Lifecycle {
    std::unique_ptr<Runtime>* runtime = nullptr;
    std::atomic<bool> background{false};
    std::atomic<bool> joinPending{false};
    std::atomic<bool> reconnectPending{false};

    void install();
    void remove();
    void settle();
};
#endif

} // namespace ke::app
