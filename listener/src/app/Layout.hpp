// Where things go: the desktop dock layout (PRINCIPALS over SESSION over BIND on the left,
// CHAT | GRAPH over HISTORY on the right), the phone's tab strip and safe area, the Settings
// tab the phone has instead of a command line.
#pragma once

#include "model/Activity.hpp"
#include "app/Options.hpp"
#include "app/Prefs.hpp"
#include "app/Worlds.hpp"
#include "modules/Multiplexer.hpp"
#include "net/OneShot.hpp"
#include "platform/Platform.hpp"
#include "ui/Theme.hpp"

#include "ui/ActionDialog.hpp"
#include "ui/ApprovalsPane.hpp"
#include "ui/ManualPane.hpp"
#include "ui/NotificationsPane.hpp"
#include "ui/OpenFileDialog.hpp"
#include "ui/DeleteDialog.hpp"
#include "ui/ScratchPane.hpp"
#include "ui/GraphPane.hpp"
#include "ui/HistoryPane.hpp"
#include "ui/InputPane.hpp"
#include "ui/PrincipalsPane.hpp"
#include "ui/SessionPane.hpp"
#include "ui/TerminalPane.hpp"

#include <SDL3/SDL.h>
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <string>

namespace ke {
class Runtime;
}

namespace ke::app {

std::filesystem::path sessionFile(const Options& opts);
std::filesystem::path prefsFile(const Options& opts);

// Everything a frame draws, so the desktop and phone frames read the same.
// Where a followed symbol leads, so a frame can bring that pane forward.
enum class LinkTarget { None, Terminal, Graph, History };

struct FrameInputs;

struct Panes {
    ui::HistoryPane history;
    ui::SessionPane session;
    ui::InputPane input;
    ui::PrincipalsPane principals;
    ui::TerminalPane terminal;
    ui::TerminalPane inspector; // the desktop's inspectors: a view of their own, beside the chats
    ui::GraphPane graph;
    modules::Multiplexer mux; // the PANES area
    ui::DeleteDialog removal; // DELETE on any object
    ui::ApprovalsPane approvals; // [ APPROVALS n ]
    ui::NotificationsPane notices; // [ NOTICES n ]
    ui::ManualPane manual;         // HELP, SHOW DOCUMENTATION
    ui::OpenFileDialog openFile;   // FILE, OPEN FILE
    model::ActivityBoard activity;        // an agent's mark, for this person
    std::string activityKey;              // what activity was built from
    std::map<std::string, Seq> seenSent;  // written to view/seen, not yet back from the world
    std::set<std::string> needsTold;      // agents the OS has been told about
    int badge = 0;
    std::map<std::string, int> unread; // by actor: Messages and notifications, for tabs and the map
    int osNotices = 0; // sent to the OS, or would have been under the drive API
    ui::ScratchPane scratch; // OFF THE RECORD
    ui::ActionDialog dialog; // NEW PRINCIPAL and the generic action forms (modal)
    std::optional<std::string> labelCaseRequest; // the drive API's way to VIEW, LABELS
    bool labelClearing = false, labelConfirmFocus = false; // RENAME LABEL: an empty SET asks first
    std::string labelObject, labelText; // RENAME LABEL: the object/label Field of one object
    // Open a Principal's terminal, select its node, focus HISTORY on it.
    // True when it opened in the chats, false in an inspector.
    bool open(Runtime& rt, const std::string& id, int view = -1);
    void inspect(const std::string& id, int view);
    // Route a pane's request for an action to the dialog: principal.create opens the two-step
    // flow, workspace.mount the mount step, anything else the generic form with `defaults`.
    void openAction(Runtime& rt, const std::string& action, json defaults = json::object());
    // Once per frame after the panes: the symbol menu, and what a clicked symbol asked for.
    LinkTarget follow(Runtime& rt, ImFont* mono);
    void symbols();
    // The one line of status along the bottom of the window. Takes its height off the work area.
    void drawModeline(FrameInputs& in);
    std::string modelineText = "idle";
    std::string modelineError;        // a refusal or a held error, shown in the error ink for a while
    double modelineErrorSince = -100.0;
    std::pair<Seq, std::size_t> modelineVersion{-1, 0};
    // Once per frame: what is unread now, given what the person is reading.
    void noteUnread(Runtime& rt, const std::string& reading);
    // Once per frame after noteUnread: the marks, and opening a chat counting as seeing it.
    void noteActivity(Runtime& rt, const std::string& reading, bool looking, SDL_Window* window, bool tellOs);
    // Loud notifications go to the OS while this window is not in front.
    void noteLoud(Runtime& rt, SDL_Window* window, bool enabled);
    ui::GraphInput graphInput(Runtime& rt, const ui::Anim& anim, ImFont* mono);
};

// The world picker (sprint 2): the worlds this machine knows (session.json entries + worlds.json
// tokens), [ ADD WORLD… ] with url and token and a `hello` probe, and the attach request the App
// executes by rebuilding the runtime. Shared by the desktop menu bar and the phone's Settings.
struct WorldPicker {
    std::string addUrl = "ws://";
    std::string addToken;
    std::string addTokenFile;
    bool adding = false;
    std::string status;
    bool statusErr = false;
    OneShot hello;        // the probe for the url being added
    std::string helloUrl; // the url the probe was sent to
    std::string helloLine; // what it answered, for the form
    std::optional<std::pair<std::string, std::string>> request; // {url, token} to attach to
    bool rememberedCurrent = false; // the running world's token saved once per runtime

    // The body: rows of known worlds with [ ATTACH ] / [ FORGET ], then the add form.
    void drawBody(Runtime& rt, const Options& opts);
    std::optional<std::pair<std::string, std::string>> takeAttach();
    void noteRuntime() { rememberedCurrent = false; }
};

// What one frame needs from the app, by reference: nothing here owns anything.
struct FrameInputs {
    Runtime& rt;
    Panes& panes;
    Prefs& prefs;
    ImFont* mono;
    ImFont* ui;
    ui::Anim anim;
    float uiScale;
    std::function<void()> saveWhimsy; // persists prefs
    std::string assetsDir;            // bundled fonts
    SDL_Window* window = nullptr;     // for the native folder picker
};

#if !LISTENER_PHONE
// Desktop layout: one tree of views and panes, `view/layout` on the person (modules/Multiplexer).
// Bring a view (CHAT, GRAPH, PANES, HISTORY) forward, opening it if it was closed, and focus it.
void bringDockWindowFront(const char* name);

// The desktop frame: menu bar (ORBIS // DEBUG SETTINGS, state and head on the right), the dock
// layout built once, then every pane.
struct DesktopFrame {
    bool layoutBuilt = false;
    bool graphInFront = false; // set by the debug hooks for the first two seconds
    // Responsive desktop: the left column (PRINCIPALS) takes 30% of the width,
    // never less than 28 columns of the mono font; under 820 px it hides and the terminal takes
    // the full width, a [ PANES ] item in the menu bar brings it back at its minimum width.
    bool showSession = false; // DEBUG, SESSION
    bool showBind = false;    // DEBUG, BIND
    bool viewsSet = false;
    bool landed = false; // the first attach looks at who is here, whatever window a saved layout had in front
    std::string wantFront = "GRAPH"; // what comes forward once its window has joined the dock
    bool leftWanted = true;   // VIEW, LEFT COLUMN
    bool worldPickerWanted = false; // WORLD, SWITCH WORLD: opens beside its button
    bool leftHidden = false;  // what the current layout has
    WorldPicker worlds;       // [ WORLD <name> ] in the menu bar
    std::string titledWorld;  // the world name the window title carries
    bool titled = false;
    const Options* opts = nullptr; // set by the App each frame (session file, current url/token)
    void draw(FrameInputs& in);
};
#else
inline constexpr const char* const kPhoneTabs[] = {"CHAT", "GRAPH", "PRINCIPALS", "HISTORY", "SESSION", "PANES", "SETTINGS"};
inline constexpr int kPhoneTabCount = 7;

// The status bar and home indicator overlap a fullscreen window; keep ImGui's work area inside
// the safe area. Set before the main window each frame.
void applySafeArea(SDL_Window* window);

// Settings tab: the world URL, token and principal (prefs.json), the connection buttons that the
// desktop keeps in the Debug menu, and the files involved. Apply saves and
// restarts the stack.
struct SettingsPane {
    std::string url, token, principal;
    std::string status;
    WorldPicker worlds; // WORLDS rows: the worlds this phone knows
    // PAIR WITH CODE: url + 6-digit code -> pair.claim on a fresh socket -> the token in prefs
    bool pairOpen = false;
    std::string pairUrl;
    std::string pairCode;
    std::string pairLabel;
    std::string pairStatus;
    bool pairErr = false;
    OneShot claim;
    bool claimPending = false;
    // A orbis://pair link arrived (URL scheme or a drop): prefill and open the form.
    void prefill(const PairLink& link);

    void load(const Options& o);
    // Returns true when the runtime must be rebuilt with the new options.
    bool drawBody(Options& opts, Runtime& rt);
};

// The phone frame: one window, parallelogram tabs, the active pane's body, the INPUT strip at
// the bottom (SDL keeps the strip above the keyboard through SDL_SetTextInputArea).
struct PhoneFrame {
    SettingsPane settings;
    int activeTab = 1; // the map first: who is here, what they are doing
    // Returns true when the runtime must be rebuilt (Settings changed url/token or forgot the session).
    bool draw(FrameInputs& in, Options& opts, SDL_Window* window);
};
#endif

} // namespace ke::app
