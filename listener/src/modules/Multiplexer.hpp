// The PANES area: splits and tabs of panes. Panes live in the world; the layout is the person's
// `view/panes` Field, so it follows them across clients. This class owns the Module instances, is
// their Host, routes `pane.subscribe` pushes to them, and draws the who-is-attached line.
#pragma once

#include "model/PaneLayout.hpp"
#include "modules/Module.hpp"
#include "ui/TabBar.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ke {
class Runtime;
}

namespace ke::model {
struct PrincipalInfo;
}

namespace ke::modules {

// A view the desktop's layout can hold alongside panes: Chat, Graph, History and the rest.
struct ViewSpec {
    std::string id;     // `view:chat`
    std::string window; // the ImGui window it draws in, placed by ui::placePane
    std::string label;
    std::string target; // the drive target, `tab:<target>`
    std::string glyph;
};

class Multiplexer final : public Host {
  public:
    // The desktop: one tree holds views and panes both, saved per screen class in `view/layout`.
    void setViews(std::vector<ViewSpec> views, std::function<model::PaneLayout(bool narrow)> defaults);
    void drawDesktop(model::LayoutRect area, bool narrow);
    bool viewOpen(const std::string& view) const;
    const std::string& focused() const { return focus_; }
    bool viewInFront(const std::string& view) const;
    void showView(const std::string& view, const std::string& near = {}, model::Edge edge = model::Edge::Tabs);
    void hideView(const std::string& view);
    // Beside everything on one edge, at the share it had when it was last there.
    void showViewAtEdge(const std::string& view, model::Edge edge, float share);
    bool panesInView() const { return paneInFront_ || bodies_.count("view:panes") != 0; }
    void resetLayout();
    // A view folded to a strip of this height; 0 unfolds it.
    void fold(const std::string& view, float strip);
    // What the modeline shows in the error ink: a refused open, wherever the click came from, then
    // an error held in a pane the person is not looking at. "" when there is none.
    std::string heldError(bool panesInView) const;
    // Each frame, before draw: the Runtime is rebuilt on a new attach.
    void bind(Runtime* rt);
    // Fills the current ImGui window.
    void draw();
    // Asks the world for a pane on `target`, and shows it when it answers.
    void openPane(const std::string& kind, const std::string& target, const std::string& title = {});
    void closeFocused();
    // A pane someone delivered: a view of kind `pane` in a transcript.
    void attachPane(const std::string& pane, const std::string& kind);
    bool hasPanes() const { return !layout_.empty(); }
    nlohmann::json describe() const; // for drive `state`
    // Inspecting an object or path belongs to the frame, which owns FIELDS.
    std::function<void(const std::string&)> onInspect;
    // The CHAT area's tabs ride in the same Field, so they follow the person too.
    std::function<std::vector<std::string>()> terminalTabs;
    std::function<void(const std::string&)> restoreTerminalTab;
    std::string takeBringForward(); // a pane just opened: the frame brings PANES to the front
    void attached(); // a fresh attach: the layout is read again from the Field

    // Host
    void act(const std::string& action, nlohmann::json args, Reply reply) override;
    void open(const FileRef& file) override;
    void inspect(const std::string& object) override;
    void copy(const std::string& text) override;
    std::string paste() override;
    void wantKeyboard(bool on) override;
    std::string nameOf(const std::string& object) const override;
    std::string self() const override;
    void readBytes(const std::string& pane, const std::string& field,
                   std::function<void(std::vector<unsigned char>, std::string)> done) override;

    bool followMouse = true; // the pointer moves focus between panes, with no click
    bool pointerOverView(const std::string& view) const; // the view's body on screen is under the mouse

  private:
    struct Instance {
        std::unique_ptr<Module> module;
        std::string key; // kind|target|path, so a second click reuses the tab
        Seq lastInput = 0;
        double markedAt = -100.0; // a model's input, shown for a moment
        std::string markText;
    };

    Instance* ensure(const std::string& pane);
    const model::PaneInfo* info(const std::string& pane) const;
    void sync();
    void routeData(const PaneData& d);
    void drawLeaf(const model::LayoutLeaf& leaf, int index);
    void drawTree(model::LayoutRect area);
    void drawDividers(model::LayoutRect area);
    void drawCloseConfirm();
    void drawFloats(model::LayoutRect area);
    void drawPanesView(const Rect& body);
    void placeViews();
    void applyFold(model::LayoutRect area);
    void loadDesktop(const model::PrincipalInfo& me);
    void switchClass(bool narrow);
    void saveDesktop(const std::string& self, const nlohmann::json& panes);
    model::PaneLayout saved(bool narrow) const;
    const ViewSpec* view(const std::string& id) const;
    std::string nearNew() const; // where a new pane opens
    void drawAttachLine(const std::string& pane, model::LayoutRect at);
    void drawNewPaneMenu();
    void focusKeys(model::LayoutRect area);
    void requestClose(const std::string& pane);
    void close(const std::string& pane);
    void saveSoon();
    void dropZones(const model::LayoutLeaf& leaf);

    Runtime* rt_ = nullptr;
    model::PaneLayout layout_;
    bool desktop_ = false, narrow_ = false, laid_ = false;
    std::vector<ViewSpec> views_;
    std::function<model::PaneLayout(bool)> defaults_;
    std::map<std::string, Rect> bodies_; // where each view in front sits this frame
    nlohmann::json buckets_ = nlohmann::json::object(); // `view/layout`: "wide" and "narrow"
    std::string lastSavedLayout_;
    std::map<std::string, float> lastShare_; // a view's share of the width when it was closed
    float areaW_ = 1.0f;
    bool paneInFront_ = false;
    std::string foldView_;
    float foldStrip_ = 0.0f, foldRatio_ = 0.0f;
    bool folded_ = false;
    std::map<std::string, Instance> live_;
    std::map<std::string, model::PaneInfo> local_; // what pane.open answered, until the fold catches up
    std::map<std::string, std::string> pendingKeys_;
    std::map<std::string, int> pendingLine_; // a citation's line, until the pane's module is there
    std::string takeKeyboard_;               // a pane just opened, until it is drawn and has the keys
    void takeLine(const std::string& pane, Instance& inst);
    std::map<std::string, ui::TabBarState> tabStates_;
    std::string menuPane_; // the tab a right click opened a menu on
    std::string dragging_, draggingLabel_; // the tab being moved, until the button comes up
    std::string focus_;
    std::string confirmClose_;
    std::string bringForward_;
    std::string openError_;
    bool loaded_ = false, dirty_ = false;
    double dirtyAt_ = 0.0;
    std::string lastSaved_;
    std::vector<std::string> savedTabs_;
};

} // namespace ke::modules
