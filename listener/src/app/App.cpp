#include "platform/Platform.hpp"
#include "ui/TextMenu.hpp"
#include "app/App.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "ui/SymbolUi.hpp"

#include "app/DebugHooks.hpp"
#include "app/Layout.hpp"
#include "ui/Themes.hpp"
#include "app/Lifecycle.hpp"
#include "app/Prefs.hpp"
#include "app/Runtime.hpp"
#include "app/Worlds.hpp"
#include "platform/Keyboard.hpp"

#include <string_view>
#include "drive/Drive.hpp"
#include "ui/Theme.hpp"
#include "util/Log.hpp"
#include "util/Timing.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <imgui_impl_sdl3.h>
#include <memory>

namespace ke {

using namespace ke::app;

namespace {
using Clock = std::chrono::steady_clock;
}

int runApp(const Options& optsIn) {
    Options opts = optsIn; // iOS Settings may change url/token at runtime
    Gpu gpu;
    if (const int rc = bringUp(gpu, opts); rc != 0) {
        return rc;
    }
    ImFont* mono = gpu.fonts.mono;
    ImFont* uiFont = gpu.fonts.ui;
    std::string themeApplied; // what SETTINGS last chose; a change lands between frames

    // Prefs: the whimsy switches on both platforms (url/token too on iOS, see main.cpp).
    Prefs prefs = loadPrefs(prefsFile(opts)).value_or(Prefs{});
    auto saveWhimsy = [&] {
        Prefs p = loadPrefs(prefsFile(opts)).value_or(prefs);
        p.thinking = prefs.thinking;
        p.filter = prefs.filter;
        p.treeFlat = prefs.treeFlat;
        p.treeExpanded = prefs.treeExpanded;
        p.pinned = prefs.pinned;
        p.theme = prefs.theme;
        p.labelCase = prefs.labelCase;
        p.osNotices = prefs.osNotices;
        p.mapTwilight = prefs.mapTwilight;
        p.touch = prefs.touch;
        p.touchWide = prefs.touchWide;
        p.tooltips = prefs.tooltips; // missing, these two snapped back to the file's value on every save
        p.focusFollowsMouse = prefs.focusFollowsMouse;
        p.historyOpen = prefs.historyOpen;
        p.fontUi = prefs.fontUi;
        p.fontMono = prefs.fontMono;
        p.fontUiSize = prefs.fontUiSize;
        p.fontMonoSize = prefs.fontMonoSize;
        prefs = p;
        savePrefs(prefsFile(opts), p);
    };

    // Network stack; its thread wakes the loop with a user event when frames arrive. It is
    // rebuilt when the iOS Settings tab changes the url or token.
    const Uint32 wakeEvent = SDL_RegisterEvents(1);
    Panes panes;
    std::unique_ptr<Runtime> rt;
    auto makeRuntime = [&] {
        if (rt) {
            panes.scratch.end(rt->chat); // a session does not follow a change of world
        }
        rt.reset(); // stops the old socket before the new one starts
        rt = std::make_unique<Runtime>(opts, std::make_unique<FileSessionStore>(sessionFile(opts), opts.url));
        rt->session.scratchPush = [&panes](const Push& p) { panes.scratch.push(p); };
        log::info("session file: {}", static_cast<FileSessionStore*>(rt->store.get())->path().string());
        rt->client.setWake([wakeEvent] {
            SDL_Event ev{};
            ev.type = wakeEvent;
            SDL_PushEvent(&ev);
        });
        rt->start();
    };
    makeRuntime();

#if LISTENER_PHONE
    Lifecycle lifecycle;
    lifecycle.runtime = &rt;
    lifecycle.install();
    platform::deliverPendingLink();
    PhoneFrame frame;
    frame.settings.load(opts);
#else
    DesktopFrame frame;
#endif
    panes.terminal.showThinking = prefs.thinking;
    panes.terminal.filter = prefs.filter;
    panes.principals.flat = prefs.treeFlat;
    panes.principals.expanded = std::set<std::string>(prefs.treeExpanded.begin(), prefs.treeExpanded.end());
    panes.principals.pinnedIds = std::set<std::string>(prefs.pinned.begin(), prefs.pinned.end());
    for (const std::string& id : panes.principals.pinnedIds) {
        rt->principals.pin(id); // the fold sorts pinned rows first, from boot and re-attach
    }
    panes.history.setQuery(prefs.searchLast);
#if !LISTENER_PHONE
    panes.history.collapsed = !prefs.historyOpen;
#endif
    bool ownTerminalOpened = false; // the session's own terminal opens on the first attach
    std::string bootLine;           // the SESSION pane's BOOT row, refreshed every frame
    std::string linkLine;           // the SESSION pane's LINK row: reconnects, backoff, or why we stopped
    int framesToRender = 3;         // a few frames after any event lets hover/focus settle, then idle
    bool done = false;
    long long pinsSeenSeq = -1; // the `view/pinned` write last applied to the fold and Prefs
    int exitCode = 0;
    const auto started = Clock::now();
    auto seconds = [&](Clock::time_point t) { return std::chrono::duration<double>(t - started).count(); };
    DebugHooks debug(started);
    drive::Drive drive; // --drive: the UX drive API (the desktop, and a phone build on the simulator)
#if !LISTENER_PHONE || (defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR)
    if (!opts.driveSocket.empty() && !drive.start(opts.driveSocket, gpu.window)) {
        log::error("drive: could not open {}", opts.driveSocket);
    }
#endif

    // Idle rule: block in SDL_WaitEventTimeout until the next thing that changes the picture
    // (a frame from the socket wakes it; the cursor blink, the spinner, a boot line, the route
    // retry and the progress line's seconds are computed deadlines). 250 ms is the safety tick,
    // not a frame rate.
    auto nextWakeMs = [&](bool paused) -> Sint32 {
        const auto now = Clock::now();
        std::optional<Clock::time_point> deadline = rt->nextDeadline(now);
        auto consider = [&](double periodSeconds) {
            const double t = seconds(now);
            const double next = (std::floor(t / periodSeconds) + 1.0) * periodSeconds;
            const auto tp = started + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(next));
            if (!deadline || tp < *deadline) {
                deadline = tp;
            }
        };
        if (!paused) {
            const bool bootCursor = !rt->chat.notes().empty() && rt->chat.notes().back().cursor;
            // every text field's insertion point blinks, not only the terminal's line
            if (ImGui::GetIO().WantTextInput || ((panes.input.active() || panes.terminal.inputActive() || bootCursor))) {
                consider(ui::kBlinkPeriod / 2.0);
            }
            const bool loopRunning = panes.terminal.spinning(rt->principals) || panes.graph.spinning(rt->principals);
            if (loopRunning || panes.graph.pulsing()) {
                consider(1.0 / 24.0); // the hazard tape marches while a model works
            } else if (!rt->chat.streamingSeqs().empty()) {
                consider(ui::kSpinnerPeriod);
            }
            if (loopRunning || panes.terminal.countdown()) {
                consider(1.0); // the progress line's elapsed seconds, the pairing countdown
            }
        }
        if (!deadline) {
            return 250;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count();
        return static_cast<Sint32>(std::clamp<long long>(ms + 1, 1, 250));
    };

    ui::themeLibrary().loadDirectory(sessionFile(opts).parent_path() / "themes");
    while (!done) {
        // Theme and fonts apply between frames, live. The interface size scales the whole layout:
        // spacing, bevels and the tree are all measured from it.
        const std::string lookKey = prefs.theme + "|" + prefs.fontUi + "|" + prefs.fontMono + "|" +
                                    std::to_string(prefs.fontUiSize) + "|" + std::to_string(prefs.fontMonoSize) + "|" + prefs.labelCase;
        if (themeApplied != lookKey) {
            themeApplied = lookKey;
            ui::setTheme(ui::themeLibrary().named(prefs.theme), gpu.uiScale * prefs.fontUiSize / 14.0f);
            ui::setLabelCase(prefs.labelCase);
            ui::useFonts(gpu.fonts, prefs.fontUi, prefs.fontMono, prefs.fontUiSize, prefs.fontMonoSize, ui::theme(), opts.assetsDir);
            mono = gpu.fonts.mono;
            uiFont = gpu.fonts.ui;
            framesToRender = std::max(framesToRender, 3);
        }
        // Drive input belongs to this window, not the desktop focus. Covered test windows
        // still need frames to consume commands and answer waits and captures.
        bool paused = !drive.enabled() && (SDL_GetWindowFlags(gpu.window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) != 0;
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
        paused = false; // a headless simulator counts as occluded, and the drive still needs frames
#endif
        if (!paused && (debug.keepDrawing(Clock::now()) || timing::spin() || drive.wantsFrames())) {
            framesToRender = 3;
        }
#if LISTENER_PHONE
        // A one-shot answers on its own thread and its result is read in a frame: nothing else
        // would draw one, and the claim would sit at "claiming..." until the next touch.
        if (frame.settings.claimPending || frame.settings.worlds.hello.running()) {
            framesToRender = 3;
        }
        paused = paused || lifecycle.background.load(); // no Metal work while backgrounded
#endif
        SDL_Event ev;
        bool got = false;
        // Hidden, nothing draws, so frames owed stay owed: polling for them would spin a core.
        if (framesToRender > 0 && !paused) {
            got = SDL_PollEvent(&ev);
        } else {
            const Sint32 waitMs = nextWakeMs(paused);
            got = SDL_WaitEventTimeout(&ev, waitMs);
            if (!got && waitMs < 250) {
                framesToRender = 1; // a deadline passed: one frame to show it
            }
        }
        while (got) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
#if LISTENER_PHONE
            // A fold, an unfold or a rotation: the screen changes under the window. Logged so a
            // phone's console says what SDL saw (iPhone Duo, 2026-11-11).
            if (ev.type == SDL_EVENT_WINDOW_RESIZED || ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                ev.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED || ev.type == SDL_EVENT_DISPLAY_ORIENTATION ||
                ev.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                int w = 0, h = 0, pw = 0, ph = 0;
                SDL_GetWindowSize(gpu.window, &w, &h);
                SDL_GetWindowSizeInPixels(gpu.window, &pw, &ph);
                log::info("screen: event {} window {}x{} ({}x{} px)", static_cast<int>(ev.type), w, h, pw, ph);
            }
#endif
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(gpu.window))) {
                done = true;
            }
            // A file or folder from Finder, or dropped text (sprint 1 L8): the current terminal
            // attaches it; the pane decides between a file, a folder prompt and text.
            if ((ev.type == SDL_EVENT_DROP_FILE || ev.type == SDL_EVENT_DROP_TEXT) && ev.drop.data != nullptr &&
                std::string_view(ev.drop.data).rfind("orbis://pair?", 0) == 0) {
                // a pairing link (the phone's URL scheme, or a drop anywhere): open the pairing form
                const app::PairLink link = app::parsePairLink(ev.drop.data);
                log::info("pair: link received for {}", link.url);
#if LISTENER_PHONE
                frame.settings.prefill(link);
                frame.activeTab = kPhoneTabCount - 1;
#else
                platform::setClipboard(ev.drop.data); // the desktop has no claim path; keep the link at hand
#endif
            } else if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data != nullptr) {
                panes.terminal.drop(ev.drop.data, "", ImVec2(ev.drop.x, ev.drop.y));
            } else if (ev.type == SDL_EVENT_DROP_TEXT && ev.drop.data != nullptr) {
                panes.terminal.drop("", ev.drop.data, ImVec2(ev.drop.x, ev.drop.y));
            }
            framesToRender = 3;
            got = SDL_PollEvent(&ev);
        }
#if LISTENER_PHONE
        lifecycle.settle();
#endif
        if (rt->drain()) {
            framesToRender = 3;
        }
        panes.history.setNames(&rt->principals);
        panes.session.setNames(&rt->principals);
        bootLine = rt->boot.line();
        panes.session.setBoot(&bootLine);
        {
            linkLine.clear();
            if (!rt->stopReason().empty()) {
                linkLine = "stopped: " + rt->stopReason();
            } else if (auto until = rt->backoffUntil()) {
                const auto left = std::chrono::duration_cast<std::chrono::seconds>(*until - Clock::now()).count();
                linkLine = "backing off, retry in " + std::to_string(std::max<long long>(0, left)) + " s (" +
                           std::to_string(rt->reconnects()) + " reconnects)";
            } else if (rt->reconnects() > 0) {
                linkLine = std::to_string(rt->reconnects()) + " reconnect(s) this run";
            }
            panes.session.setLink(&linkLine, !rt->stopReason().empty());
        }
        rt->symbols.refresh(rt->principals, &rt->actions, &rt->tasks, &rt->evaluations);
        ui::symbolUi().bind(&rt->symbols, &rt->principals, &rt->actions);
        panes.principals.setEvaluations(&rt->evaluations);
        panes.principals.setTasks(&rt->tasks);
        for (ui::TerminalPane* t : {&panes.terminal, &panes.inspector}) {
            t->setTasks(&rt->tasks);
            t->setTokens(&rt->tokens);
            t->setTokenDir(sessionFile(opts).parent_path());
            t->setSocketUrl(opts.url);
            t->setActions(&rt->actions);
        }
        panes.graph.setActions(&rt->actions);
        if (rt->session.attached() && !ownTerminalOpened && rt->session.principal()) {
            ownTerminalOpened = true; // nothing opens by itself: the map says who is here
            framesToRender = 3;
        }
        if (framesToRender <= 0 || paused) {
            continue;
        }
        --framesToRender;

        timing::Scope frameScope("frame");
#if !LISTENER_PHONE
        const int sweepPass = debug.sweepBegin(gpu, *rt, Clock::now());
        if (sweepPass >= 0 && sweepPass < 3) {
            panes.terminal.setView(sweepPass);
        }
#endif
        ui::textMenuPump();
        if (panes.labelCaseRequest) {
            prefs.labelCase = *panes.labelCaseRequest;
            panes.labelCaseRequest.reset();
            saveWhimsy();
        }
        drive.beforeFrame(); // queued drive input goes to ImGui's io before NewFrame
        {
            timing::Scope s("newFrame");
            newFrame(gpu);
        }
        FrameInputs in{*rt, panes, prefs, mono, uiFont, ui::animAt(seconds(Clock::now())), gpu.uiScale, saveWhimsy,
                       opts.assetsDir, gpu.window};
#if LISTENER_PHONE
        if (frame.draw(in, opts, gpu.window)) {
            makeRuntime(); // Settings changed url/token or forgot the session
            ownTerminalOpened = false;
            framesToRender = 3;
        }
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
        if (drive.enabled()) { // the simulator's drive answers after the frame, as the desktop's does
            drive::FrameContext dctx{gpu, *rt, panes, gpu.uiScale};
            if (drive.afterFrame(dctx)) {
                done = true;
                continue;
            }
        }
#endif
#else
        frame.graphInFront = debug.graphInFront(Clock::now()) || sweepPass == 3;
        frame.opts = &opts;
        {
            timing::Scope s("draw");
            frame.draw(in);
        }
        if (auto attach = frame.worlds.takeAttach()) { // [ WORLD ] picker: another world
            opts.url = attach->first;
            opts.token = attach->second;
            makeRuntime();
            frame.worlds.noteRuntime();
            ownTerminalOpened = false;
            framesToRender = 3;
        }
        if (drive.enabled()) {
            drive::FrameContext dctx{gpu, *rt, panes, gpu.uiScale};
            if (drive.afterFrame(dctx)) {
                done = true; // `quit`
                continue;
            }
            if (auto attach = drive.takeAttach()) {
                opts.url = attach->url;
                opts.token = attach->token;
                makeRuntime();
                ownTerminalOpened = false;
                framesToRender = 3;
            }
        }
        if (debug.sweepEnd()) {
            exitCode = debug.sweepExitCode();
            done = true;
            continue;
        }
        if (debug.shotDue(*rt, Clock::now())) {
            dumpFrame(gpu, debug.shotFile);
            done = true;
            continue;
        }
#endif
        if (panes.principals.takeRetiredRequested()) {
            rt->fetchRetired();
        }
        // Pins live on the person's own Principal, in `view/pinned`, so every client of theirs shows the
        // same list; Prefs is the copy for before attach. A pick writes the Field; the fold's echo of it
        // (and another client's change) comes back through the same sync.
        constexpr const char* kPinned = "view/pinned";
        const std::string self = rt->session.principal().value_or("");
        if (const model::PrincipalInfo* me = self.empty() ? nullptr : rt->principals.get(self)) {
            if (auto it = me->fields.find(kPinned);
                it != me->fields.end() && it->second.value.is_array() && it->second.seq != pinsSeenSeq) {
                pinsSeenSeq = it->second.seq;
                std::set<std::string> ids;
                for (const json& v : it->second.value) {
                    if (v.is_string()) {
                        ids.insert(v.get<std::string>());
                    }
                }
                if (ids != panes.principals.pinnedIds) {
                    panes.principals.pinnedIds = ids;
                    rt->principals.setPins(ids);
                    prefs.pinned.assign(ids.begin(), ids.end());
                    saveWhimsy();
                }
            }
        }
        std::string pinnedId, unpinnedId;
        bool pinsChanged = false;
        if (panes.principals.takePinned(pinnedId)) {
            rt->principals.pin(pinnedId);
            pinsChanged = true;
        }
        if (panes.principals.takeUnpinned(unpinnedId)) {
            rt->principals.unpin(unpinnedId);
            pinsChanged = true;
        }
        if (pinsChanged && !self.empty() && rt->session.attached()) {
            json list = json::array();
            for (const std::string& id : panes.principals.pinnedIds) {
                list.push_back(id);
            }
            rt->chat.setField(self, kPinned, json::array({"list", "string"}), list, "human", [](const Response&) {});
        }
        if (panes.principals.takePrefsDirty()) {
            prefs.treeFlat = panes.principals.flat;
            prefs.treeExpanded.assign(panes.principals.expanded.begin(), panes.principals.expanded.end());
            prefs.pinned.assign(panes.principals.pinnedIds.begin(), panes.principals.pinnedIds.end());
            saveWhimsy();
        }
        if (panes.terminal.showThinking != prefs.thinking || panes.terminal.filter != prefs.filter) { // the status bar's switches
            prefs.thinking = panes.terminal.showThinking;
            prefs.filter = panes.terminal.filter;
            saveWhimsy();
        }
        {
            timing::Scope s("render");
            renderFrame(gpu);
        }
        timing::flush();
    }

#if LISTENER_PHONE
    lifecycle.remove();
#endif
    panes.scratch.end(rt->chat); // quitting ends it; the world sweeps it anyway if this frame is lost
    rt->client.stop();
    tearDown(gpu);
    if (exitCode == 0 && std::getenv("LISTENER_STRICT") && imguiErrorCount() > 0) {
        log::error("{} recoverable imgui error(s) during the run", imguiErrorCount());
        exitCode = 3;
    }
    return exitCode;
}

} // namespace ke
