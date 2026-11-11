// Drive and capture API for agentic UX testing (`--drive <unix-socket>` or LISTENER_DRIVE=<path>).
// JSON Lines over a Unix domain socket: each request `{"id":n,"cmd":"...",...}` is answered with
// `{"id":n,"ok":true,...}` or `{"id":n,"ok":false,"err":"..."}`. A socket thread accepts one
// client at a time and queues the parsed commands; the UI thread applies them around its frame:
// input injection before ImGui::NewFrame (`type`, `key`, `click`, `drag`, `scroll`, one step per
// frame so ImGui sees a real sequence), observation after the panes drew (`state`, `screenshot`,
// `wait`). Commands run strictly in order; a `wait` blocks the ones behind it until it resolves.
// Off by default; no code here runs without the flag. tools/drive.py is the client.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h> // ImRect

#include "net/Protocol.hpp" // Seq

namespace ke {
class Runtime;
}
namespace ke::app {
struct Gpu;
struct Panes;
} // namespace ke::app

namespace ke::drive {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct FrameContext {
    app::Gpu& gpu;
    Runtime& rt;
    app::Panes& panes;
    float uiScale;
};

struct AttachRequest {
    std::string url;
    std::string token;
};

class Drive {
  public:
    Drive();
    ~Drive();
    Drive(const Drive&) = delete;
    Drive& operator=(const Drive&) = delete;

    // Opens the socket and starts the accept thread; false (logged) on error. Empty path: no-op.
    bool start(const std::string& socketPath, SDL_Window* window = nullptr);
    void stop();
    bool enabled() const { return listenFd_ >= 0; }
    // Pending commands, queued input or an active wait: the loop must keep drawing.
    bool wantsFrames() const;
    // Before ImGui::NewFrame: feed one queued input step into io.
    void beforeFrame();
    // After the panes drew, before the frame renders: answer what can be answered from this
    // frame. Returns true when `quit` was received.
    bool afterFrame(FrameContext& ctx);
    // `attach {url, token_file}` asks the app to rebuild its runtime.
    std::optional<AttachRequest> takeAttach();

  private:
    struct Command {
        json id;
        std::string cmd;
        json args;
    };
    struct Step {
        enum Kind { MousePos, MouseDown, MouseUp, KeyDown, KeyUp, Text, Wheel, Hold } kind = MousePos;
        float x = 0, y = 0;
        ImGuiKey key = ImGuiKey_None;
        std::string text;
        int holdFrames = 0;
        int button = 0;
        static Step pos(float x, float y) { Step s; s.kind = MousePos; s.x = x; s.y = y; return s; }
        static Step down(int button = 0) { Step s; s.kind = MouseDown; s.button = button; return s; }
        static Step up(int button = 0) { Step s; s.kind = MouseUp; s.button = button; return s; }
        static Step keyDown(ImGuiKey k) { Step s; s.kind = KeyDown; s.key = k; return s; }
        static Step keyUp(ImGuiKey k) { Step s; s.kind = KeyUp; s.key = k; return s; }
        static Step typed(std::string t) { Step s; s.kind = Text; s.text = std::move(t); return s; }
        static Step wheel(float dx, float dy) { Step s; s.kind = Wheel; s.x = dx; s.y = dy; return s; }
        static Step hold(int frames) { Step s; s.kind = Hold; s.holdFrames = frames; return s; }
    };
    struct Wait {
        json id;
        std::string until; // idle | attached | reply | text | settle
        std::string text;
        Clock::time_point started;
        Clock::time_point deadline;
        std::size_t repliesBefore = 0;
        Seq seqBefore = 0;
        int framesLeft = 0;
    };

    void serve();
    void handleClient(int fd);
    void reply(const json& id, json body);
    void ok(const json& id, json body = json::object());
    void fail(const json& id, const std::string& err, json extra = json::object());

    bool handle(const Command& c, FrameContext& ctx);      // try/catch around handleInner: a bad command answers err, never kills the app
    bool handleInner(const Command& c, FrameContext& ctx); // true when quit
    json stateJson(FrameContext& ctx) const;
    std::optional<ImRect> resolveTarget(const std::string& target, FrameContext& ctx, std::string& why) const;
    std::vector<std::string> targetNames() const;
    std::size_t replyCount(FrameContext& ctx) const;
    bool waitSatisfied(Wait& w, FrameContext& ctx) const;
    void queueClick(ImVec2 at, int button = 0, int holdFrames = 0);
    void queueKey(const std::string& name, const std::vector<std::string>& mods, std::string& err);

    std::string path_;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex mu_;         // pending_, clientFd_ writes
    std::deque<Command> pending_;
    std::deque<Step> steps_;        // UI thread only
    std::optional<Wait> wait_;      // UI thread only
    std::optional<AttachRequest> attach_;
    int settleFrames_ = 0;          // frames to keep drawing after the last step
    std::uint32_t wakeEvent_ = 0;
    SDL_Window* window_ = nullptr; // for warping the OS cursor along with injected positions
    unsigned long long frame_ = 0;
};

} // namespace ke::drive
