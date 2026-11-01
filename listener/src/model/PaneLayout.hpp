// How one person arranges panes: a tree of splits whose leaves are tab groups of pane ids. It is
// the value of the Field view/panes, so it follows the person across clients. Pure: no ImGui, so
// every rearrangement is tested without a window.
//
//   leaf   {"tabs": [pane ids], "active": index}
//   split  {"split": "h" | "v", "ratio": 0..1, "a": node, "b": node}
//   h puts a left of b, v puts a above b.
// A desktop's whole window is one of these: an id `view:<name>` is a view (Chat, Graph, History,
// Principals, an inspector), any other id a pane. Whatever floats sits beside the tree in "floats".
#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ke::model {

using json = nlohmann::json;

enum class Edge { Left, Right, Top, Bottom, Tabs };
enum class Direction { Left, Right, Up, Down };

struct LayoutRect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct LayoutLeaf {
    std::vector<std::string> tabs;
    int active = 0;
    LayoutRect rect;
    const std::string* activePane() const {
        return active >= 0 && active < static_cast<int>(tabs.size()) ? &tabs[static_cast<std::size_t>(active)] : nullptr;
    }
};

// Out of the tree, in a window of its own.
struct LayoutFloat {
    std::string pane;
    LayoutRect rect;
};

// The split a pane's leaf hangs from.
struct LayoutSide {
    std::string path; // of the split
    bool second = false; // the leaf is b
    bool horizontal = false;
    bool alone = false; // the only tab in its leaf
};

// A split's divider, with its path so a drag can address it.
struct LayoutDivider {
    std::string path; // "" the root, then a or b per step
    bool horizontal = false; // h split: a vertical bar dragged sideways
    LayoutRect rect;
    float extent = 0; // the split's size along its axis, to turn a drag into a ratio
    float ratio = 0.5f;
};

class PaneLayout {
  public:
    PaneLayout();
    ~PaneLayout();
    PaneLayout(const PaneLayout&);
    PaneLayout& operator=(const PaneLayout&);

    static PaneLayout fromJson(const json& j); // tolerant: anything malformed reads as empty
    json toJson() const;

    bool empty() const;
    bool contains(const std::string& pane) const;
    std::vector<std::string> panes() const;

    // Add to the tab group of `near`, or the focused leaf's when near is empty.
    void add(const std::string& pane, const std::string& near = {});
    // Open beside `near` on an edge: a new split, or a tab when edge is Tabs.
    void place(const std::string& pane, const std::string& near, Edge edge);
    // Beside everything, on one side of the whole tree, taking `share` of it.
    void wrap(const std::string& pane, Edge edge, float share);
    void remove(const std::string& pane);
    void move(const std::string& pane, const std::string& near, Edge edge); // remove then place
    void activate(const std::string& pane);
    void setRatio(const std::string& path, float ratio);
    // Drop ids no longer in `live`.
    void retain(const std::vector<std::string>& live);

    // Out of the tree into a floating window, and back beside `near`.
    void floatOut(const std::string& pane, LayoutRect rect);
    void dock(const std::string& pane, const std::string& near, Edge edge = Edge::Tabs);
    void setFloatRect(const std::string& pane, LayoutRect rect);
    bool floating(const std::string& pane) const;
    const std::vector<LayoutFloat>& floats() const { return floats_; }
    std::optional<LayoutSide> sideOf(const std::string& pane) const;
    float ratioAt(const std::string& path) const;

    // Leaves and dividers laid out in rect, with `gap` between siblings.
    std::vector<LayoutLeaf> leaves(LayoutRect rect, float gap = 0.0f) const;
    std::vector<LayoutDivider> dividers(LayoutRect rect, float gap = 0.0f) const;

    // The pane to focus moving from `from` in a direction: the nearest leaf with overlap on the
    // other axis. nullopt at the edge.
    std::optional<std::string> neighbour(const std::string& from, Direction d, LayoutRect rect) const;

  private:
    struct Node;
    std::unique_ptr<Node> root_;
    std::vector<LayoutFloat> floats_;
};

} // namespace ke::model
