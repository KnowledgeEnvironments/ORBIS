// The welcome: the world as a map (model/WorldMap), the first thing a person sees on attach. Who is
// here, what each one is doing, what each one holds, and how they are joined. Nodes stay where they
// are; what changes is what they say.
//
//   click an actor        open its tab; a card on the node decides right there
//   shift-click           inspect it, as everywhere else
//   right-click, or hold  its menu
//   drag onto another     connect them, or hand work over, through the normal actions
//   click a port          a pane opens, anything else is inspected
// Idle: nothing animates. A Message crossing a link pulses along it and asks for frames while it does.
#pragma once

#include "model/Activity.hpp"
#include "chat/Chat.hpp"
#include "chat/Notifications.hpp"
#include "chat/Unsafe.hpp"
#include "model/Actions.hpp"
#include "model/Evaluations.hpp"
#include "model/Graph.hpp"
#include "model/PaneFold.hpp"
#include "model/Principals.hpp"
#include "model/Tasks.hpp"
#include "model/WorldMap.hpp"
#include "net/Session.hpp"
#include "ui/Theme.hpp"

#include <functional>
#include <imgui.h>
#include <map>
#include <string>
#include <vector>

namespace ke::ui {

struct GraphActions {
    std::string focus; // a Principal whose tab should open, with its input focused
    int view = -1;     // the terminal view to show: 0 transcript, 2 fields; -1 unchanged
    std::string action; // a named action the menu asked for (the app opens its dialog)
    json actionArgs = json::object(); // prefilled args for it
    bool approvals = false; // several cards: the APPROVALS window has room for them
};

struct GraphInput {
    const model::Principals& principals;
    const model::Evaluations& evals;
    const model::Tasks& tasks;
    const model::PaneFold& panes;
    const std::vector<chat::UnsafeEntry>& unsafe;
    const std::map<std::string, int>& unread; // by actor: Messages and notifications
    const std::map<std::string, model::Activity>& activity; // by agent
    chat::Chat& chat;
    const Session& session;
    const Anim& anim;
    ImFont* mono;
    double wallClock;
};

class GraphPane {
  public:
    bool twilight = false; // the map over PROMETHEUS's twilight field: theme roles, off by default
    // Drawn right after the pane opens (the desktop's CHAT|GRAPH switch lives in Layout).
    std::function<void()> header;
    GraphActions draw(const GraphInput& in);
    GraphActions drawBody(const GraphInput& in);
    const std::string& selected() const { return selected_; }
    void select(const std::string& id) { selected_ = id; }
    // The world's named actions: the node menu lists them, connecting uses route.create.
    void setActions(const model::Actions* actions) { actions_ = actions; }
    // A loop is running, or a pulse is under way: both need frames.
    bool spinning(const model::Principals& principals) const;
    bool pulsing() const { return !pulses_.empty() || pressing_; } // a held press has to be timed
    // What the map holds now, and the op results under it, for the drive API.
    const model::WorldMap& map() const { return map_; }
    std::string statusLines() const {
        std::string out;
        for (const Status& s : status_) {
            out += s.text + "\n";
        }
        return out;
    }
    // A graph view expanded from a terminal: shown in place of the map until dismissed; nothing of
    // it is written to the world.
    void setOverlay(model::GraphModel g, std::string title);
    void clearOverlay();
    bool hasOverlay() const { return !overlayTitle_.empty(); }

  private:
    struct Status {
        std::string text;
        bool err = false;
    };
    struct Pulse {
        std::string from, to;
        double start = 0.0;
    };

    void rebuild(const GraphInput& in, float width);
    void notePulses(const GraphInput& in);
    void drawWelcome(const GraphInput& in, GraphActions& out, ImVec2 origin);
    void drawLinks(ImDrawList* dl, ImVec2 origin, const Anim& anim);
    void drawActor(const GraphInput& in, GraphActions& out, const model::MapActor& a, ImVec2 origin);
    void drawPorts(const model::MapActor& a, ImVec2 origin, float top);
    void drawMenus(const GraphInput& in, GraphActions& out);
    void drawHandOver(const GraphInput& in);
    void drawOverlay(ImFont* mono);
    void tooltip(const GraphInput& in, const model::MapActor& a);
    void connect(const GraphInput& in, const std::string& from, const std::string& to);
    void landed(const std::string& from, const std::string& to);
    void status(std::string text, bool err);
    const model::MapActor* actorAt(ImVec2 mouse, ImVec2 origin) const;
    std::string sharedRoute(const GraphInput& in, const std::string& who) const;

    model::WorldMap map_;
    std::string builtFor_; // what the map was built from
    float laidFor_ = -1.0f;
    float laidLine_ = -1.0f;
    bool welcome_ = false;
    float welcomeH_ = 0.0f;
    std::string selected_;
    std::string hovered_;
    std::string dragFrom_;   // an actor being dragged onto another
    std::string armedFrom_;  // CONNECT TO... from the menu: the next actor clicked is the target
    std::string menuNode_;
    const char* popupWanted_ = nullptr; // opened from the scope its BeginPopup runs in
    std::string dropFrom_, dropTo_;
    std::string handFrom_, handTo_, handText_;
    bool holdFired_ = false; // a long press opened the menu: the release is not a click
    bool pressing_ = false;  // an actor is held down: the frame loop must not sleep through it
    std::size_t messagesSeen_ = 0;
    bool baselined_ = false;
    std::vector<Pulse> pulses_;
    std::vector<Status> status_;
    model::GraphModel overlay_;
    std::string overlayTitle_;
    const model::Actions* actions_ = nullptr;
};

} // namespace ke::ui
