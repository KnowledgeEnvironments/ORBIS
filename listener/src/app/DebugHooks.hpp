// Desktop debug aids, all driven by environment variables so a build never changes for them:
//   LISTENER_SHOT=<file.bmp>        render the frame to a BMP and quit (checks the look on a
//                                   machine whose screen cannot be captured)
//   LISTENER_SHOT_AFTER_MS=<ms>     when to take it (default 4000)
//   LISTENER_SHOT_WHEN=thinking|streaming|done
//                                   instead of the timer: when the attached Principal's latest
//                                   Evaluation is running with nothing received / streaming
//                                   content / finished after a typed line (the timer stays as
//                                   the deadline)
//   LISTENER_SHOT_TYPE=<text>       a line typed into the own terminal once the route is known
//   LISTENER_START_PANE=GRAPH       the pane in front for the first two seconds
//   LISTENER_IDCHECK=sweep[:step]   walk a synthetic mouse over the whole window in `step` px
//                                   (default 12) through the transcript, prompt, fields and
//                                   graph views, record every ImGui id conflict and every
//                                   recoverable ImGui error, print them, exit 3 if any
//   LISTENER_IDCHECK_AFTER_MS=<ms>  when the sweep starts (default 3000, after attach)
// On iOS none of this exists; the struct compiles to nothing.
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

namespace ke {
class Runtime;
}
namespace ke::ui {
class GraphPane;
}

namespace ke::app {

struct Gpu;

struct DebugHooks {
    using Clock = std::chrono::steady_clock;

    explicit DebugHooks(Clock::time_point started);

    bool active() const { return shotFile != nullptr; }
    // Keep drawing in the run-up to the shot (the loop idles otherwise).
    bool keepDrawing(Clock::time_point now) const;
    // LISTENER_START_PANE=GRAPH during its first two seconds.
    bool graphInFront(Clock::time_point now) const;
    // After the panes drew: type the line, move the node, decide whether this frame is the shot.
    bool shotDue(Runtime& rt, Clock::time_point now);

    // The id sweep. sweeping() while it runs; sweepBegin() before NewFrame places the synthetic
    // mouse and returns the view pass (0 transcript, 1 prompt, 2 fields, 3 graph, -1 not yet);
    // sweepEnd() after the frame records what ImGui saw and returns true once the sweep is over.
    bool sweeping() const { return idCheck != nullptr && !sweepDone; }
    int sweepBegin(Gpu& gpu, Runtime& rt, Clock::time_point now);
    bool sweepEnd();
    int sweepExitCode() const;

    const char* shotFile = nullptr;
    const char* shotType = nullptr;
    const char* shotWhen = nullptr;
    const char* startPane = nullptr;
    Clock::time_point shotAt;
    Clock::time_point startPaneUntil;
    bool shotTyped = false;
    bool typedSeen = false; // the typed line's reply condition may only trigger after typing

    const char* idCheck = nullptr;
    Clock::time_point sweepAt;
    int sweepStep = 12;
    int sweepPass = -1;
    int sweepX = 0, sweepY = 0;
    int prevX = -1, prevY = -1, prevPass = -1;
    int sweepW = 0, sweepH = 0;
    bool sweepDone = false;
    bool sweepPresentSet = false;
    unsigned sweepFrames = 0;
    std::vector<std::string> conflicts;
    std::string lastConflictKey;
};

// Render the current ImGui frame into an offscreen texture of the window's size, download it
// and write a BMP. Returns false when anything in the GPU path failed.
bool dumpFrame(Gpu& gpu, const char* file);

} // namespace ke::app
