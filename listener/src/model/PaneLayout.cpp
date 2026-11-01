#include "model/PaneLayout.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

namespace ke::model {

struct PaneLayout::Node {
    // a leaf when a is null
    std::vector<std::string> tabs;
    int active = 0;
    bool horizontal = true;
    float ratio = 0.5f;
    std::unique_ptr<Node> a, b;

    bool leaf() const { return !a; }
    std::unique_ptr<Node> clone() const {
        auto n = std::make_unique<Node>();
        n->tabs = tabs;
        n->active = active;
        n->horizontal = horizontal;
        n->ratio = ratio;
        n->a = a ? a->clone() : nullptr;
        n->b = b ? b->clone() : nullptr;
        return n;
    }
};

namespace {

float clampRatio(float r) {
    return std::clamp(r, 0.1f, 0.9f);
}

bool overlap(float a0, float a1, float b0, float b1) {
    return std::min(a1, b1) - std::max(a0, b0) > 0.5f;
}

} // namespace

PaneLayout::PaneLayout() = default;
PaneLayout::~PaneLayout() = default;
PaneLayout::PaneLayout(const PaneLayout& o) : root_(o.root_ ? o.root_->clone() : nullptr), floats_(o.floats_) {}
PaneLayout& PaneLayout::operator=(const PaneLayout& o) {
    if (this != &o) {
        floats_ = o.floats_;
        root_ = o.root_ ? o.root_->clone() : nullptr;
    }
    return *this;
}

PaneLayout PaneLayout::fromJson(const json& j) {
    std::function<std::unique_ptr<PaneLayout::Node>(const json&)> read = [&](const json& v) -> std::unique_ptr<PaneLayout::Node> {
        if (!v.is_object()) {
            return nullptr;
        }
        auto n = std::make_unique<PaneLayout::Node>();
        if (v.contains("tabs") && v["tabs"].is_array()) {
            for (const json& t : v["tabs"]) {
                if (t.is_string()) {
                    n->tabs.push_back(t.get<std::string>());
                }
            }
            if (n->tabs.empty()) {
                return nullptr;
            }
            n->active = std::clamp(v.value("active", 0), 0, static_cast<int>(n->tabs.size()) - 1);
            return n;
        }
        if (v.contains("split") && v.contains("a") && v.contains("b")) {
            n->horizontal = v.value("split", std::string("h")) != "v";
            n->ratio = clampRatio(v.value("ratio", 0.5f));
            n->a = read(v["a"]);
            n->b = read(v["b"]);
            if (!n->a || !n->b) {
                return n->a ? std::move(n->a) : std::move(n->b);
            }
            return n;
        }
        return nullptr;
    };
    PaneLayout l;
    l.root_ = read(j);
    if (j.is_object() && j.contains("floats") && j["floats"].is_array()) {
        for (const json& f : j["floats"]) {
            if (f.is_object() && f.contains("pane") && f["pane"].is_string()) {
                l.floats_.push_back(LayoutFloat{f["pane"].get<std::string>(),
                                                LayoutRect{f.value("x", 80.0f), f.value("y", 80.0f), f.value("w", 640.0f), f.value("h", 420.0f)}});
            }
        }
    }
    return l;
}

json PaneLayout::toJson() const {
    std::function<json(const PaneLayout::Node&)> write = [&](const PaneLayout::Node& n) -> json {
        if (n.leaf()) {
            return json{{"tabs", n.tabs}, {"active", n.active}};
        }
        return json{{"split", n.horizontal ? "h" : "v"}, {"ratio", n.ratio}, {"a", write(*n.a)}, {"b", write(*n.b)}};
    };
    json out = root_ ? write(*root_) : json::object();
    if (!floats_.empty()) {
        json fs = json::array();
        for (const LayoutFloat& f : floats_) {
            fs.push_back({{"pane", f.pane}, {"x", f.rect.x}, {"y", f.rect.y}, {"w", f.rect.w}, {"h", f.rect.h}});
        }
        out["floats"] = std::move(fs);
    }
    return out;
}

void PaneLayout::wrap(const std::string& pane, Edge edge, float share) {
    auto fresh = std::make_unique<Node>();
    fresh->tabs = {pane};
    if (!root_) {
        root_ = std::move(fresh);
        return;
    }
    auto split = std::make_unique<Node>();
    const bool first = edge == Edge::Left || edge == Edge::Top;
    split->horizontal = edge == Edge::Left || edge == Edge::Right;
    split->ratio = clampRatio(first ? share : 1.0f - share);
    split->a = first ? std::move(fresh) : std::move(root_);
    split->b = first ? std::move(root_) : std::move(fresh);
    root_ = std::move(split);
}

void PaneLayout::floatOut(const std::string& pane, LayoutRect rect) {
    remove(pane);
    floats_.push_back(LayoutFloat{pane, rect});
}

void PaneLayout::dock(const std::string& pane, const std::string& near, Edge edge) {
    std::erase_if(floats_, [&](const LayoutFloat& f) { return f.pane == pane; });
    if (!contains(pane)) {
        place(pane, near, edge);
    }
}

void PaneLayout::setFloatRect(const std::string& pane, LayoutRect rect) {
    for (LayoutFloat& f : floats_) {
        if (f.pane == pane) {
            f.rect = rect;
        }
    }
}

bool PaneLayout::floating(const std::string& pane) const {
    return std::any_of(floats_.begin(), floats_.end(), [&](const LayoutFloat& f) { return f.pane == pane; });
}

std::optional<LayoutSide> PaneLayout::sideOf(const std::string& pane) const {
    std::optional<LayoutSide> found;
    std::function<void(const Node&, const std::string&)> walk = [&](const Node& n, const std::string& path) {
        if (n.leaf()) {
            return;
        }
        for (const Node* kid : {n.a.get(), n.b.get()}) {
            const bool second = kid == n.b.get();
            if (kid->leaf()) {
                if (std::find(kid->tabs.begin(), kid->tabs.end(), pane) != kid->tabs.end()) {
                    found = LayoutSide{path, second, n.horizontal, kid->tabs.size() == 1};
                }
            } else {
                walk(*kid, path + (second ? "b" : "a"));
            }
        }
    };
    if (root_) {
        walk(*root_, "");
    }
    return found;
}

float PaneLayout::ratioAt(const std::string& path) const {
    const Node* n = root_.get();
    for (char c : path) {
        if (!n || n->leaf()) {
            return 0.5f;
        }
        n = c == 'a' ? n->a.get() : n->b.get();
    }
    return n && !n->leaf() ? n->ratio : 0.5f;
}

bool PaneLayout::empty() const {
    return !root_;
}

std::vector<std::string> PaneLayout::panes() const {
    std::vector<std::string> out;
    std::function<void(const PaneLayout::Node&)> walk = [&](const PaneLayout::Node& n) {
        if (n.leaf()) {
            out.insert(out.end(), n.tabs.begin(), n.tabs.end());
        } else {
            walk(*n.a);
            walk(*n.b);
        }
    };
    if (root_) {
        walk(*root_);
    }
    return out;
}

bool PaneLayout::contains(const std::string& pane) const {
    const std::vector<std::string> all = panes();
    return std::find(all.begin(), all.end(), pane) != all.end();
}

void PaneLayout::add(const std::string& pane, const std::string& near) {
    place(pane, near, Edge::Tabs);
}

void PaneLayout::place(const std::string& pane, const std::string& near, Edge edge) {
    if (contains(pane)) {
        activate(pane);
        return;
    }
    if (!root_) {
        root_ = std::make_unique<Node>();
        root_->tabs = {pane};
        return;
    }
    // The leaf holding `near`, else the first leaf.
    std::function<std::unique_ptr<Node>*(std::unique_ptr<Node>&)> find = [&](std::unique_ptr<Node>& n) -> std::unique_ptr<Node>* {
        if (n->leaf()) {
            return std::find(n->tabs.begin(), n->tabs.end(), near) != n->tabs.end() ? &n : nullptr;
        }
        if (auto* f = find(n->a)) {
            return f;
        }
        return find(n->b);
    };
    std::function<std::unique_ptr<Node>*(std::unique_ptr<Node>&)> first = [&](std::unique_ptr<Node>& n) -> std::unique_ptr<Node>* {
        return n->leaf() ? &n : first(n->a);
    };
    std::unique_ptr<Node>* slot = near.empty() ? nullptr : find(root_);
    if (!slot) {
        slot = first(root_);
    }
    if (edge == Edge::Tabs) {
        (*slot)->tabs.push_back(pane);
        (*slot)->active = static_cast<int>((*slot)->tabs.size()) - 1;
        return;
    }
    auto fresh = std::make_unique<Node>();
    fresh->tabs = {pane};
    auto split = std::make_unique<Node>();
    split->horizontal = edge == Edge::Left || edge == Edge::Right;
    const bool freshFirst = edge == Edge::Left || edge == Edge::Top;
    std::unique_ptr<Node> old = std::move(*slot);
    split->a = freshFirst ? std::move(fresh) : std::move(old);
    split->b = freshFirst ? std::move(old) : std::move(fresh);
    *slot = std::move(split);
}

void PaneLayout::remove(const std::string& pane) {
    std::function<std::unique_ptr<Node>(std::unique_ptr<Node>)> strip = [&](std::unique_ptr<Node> n) -> std::unique_ptr<Node> {
        if (n->leaf()) {
            auto it = std::find(n->tabs.begin(), n->tabs.end(), pane);
            if (it != n->tabs.end()) {
                n->tabs.erase(it);
                n->active = std::clamp(n->active, 0, std::max(0, static_cast<int>(n->tabs.size()) - 1));
            }
            return n->tabs.empty() ? nullptr : std::move(n);
        }
        n->a = strip(std::move(n->a));
        n->b = strip(std::move(n->b));
        if (!n->a) {
            return std::move(n->b);
        }
        if (!n->b) {
            return std::move(n->a);
        }
        return n;
    };
    if (root_) {
        root_ = strip(std::move(root_));
    }
}

void PaneLayout::move(const std::string& pane, const std::string& near, Edge edge) {
    if (pane == near) {
        return;
    }
    remove(pane);
    place(pane, near, edge);
}

void PaneLayout::activate(const std::string& pane) {
    std::function<void(Node&)> walk = [&](Node& n) {
        if (n.leaf()) {
            auto it = std::find(n.tabs.begin(), n.tabs.end(), pane);
            if (it != n.tabs.end()) {
                n.active = static_cast<int>(it - n.tabs.begin());
            }
        } else {
            walk(*n.a);
            walk(*n.b);
        }
    };
    if (root_) {
        walk(*root_);
    }
}

void PaneLayout::setRatio(const std::string& path, float ratio) {
    Node* n = root_.get();
    for (char c : path) {
        if (!n || n->leaf()) {
            return;
        }
        n = c == 'a' ? n->a.get() : n->b.get();
    }
    if (n && !n->leaf()) {
        n->ratio = std::clamp(ratio, 0.01f, 0.99f); // a drag has its own limits; a folded strip is thin
    }
}

void PaneLayout::retain(const std::vector<std::string>& live) {
    const std::set<std::string> keep(live.begin(), live.end());
    for (const std::string& p : panes()) {
        if (keep.count(p) == 0) {
            remove(p);
        }
    }
}

std::vector<LayoutLeaf> PaneLayout::leaves(LayoutRect rect, float gap) const {
    std::vector<LayoutLeaf> out;
    std::function<void(const Node&, LayoutRect)> walk = [&](const Node& n, LayoutRect r) {
        if (n.leaf()) {
            out.push_back(LayoutLeaf{n.tabs, n.active, r});
            return;
        }
        if (n.horizontal) {
            const float wa = (r.w - gap) * n.ratio;
            walk(*n.a, {r.x, r.y, wa, r.h});
            walk(*n.b, {r.x + wa + gap, r.y, r.w - gap - wa, r.h});
        } else {
            const float ha = (r.h - gap) * n.ratio;
            walk(*n.a, {r.x, r.y, r.w, ha});
            walk(*n.b, {r.x, r.y + ha + gap, r.w, r.h - gap - ha});
        }
    };
    if (root_) {
        walk(*root_, rect);
    }
    return out;
}

std::vector<LayoutDivider> PaneLayout::dividers(LayoutRect rect, float gap) const {
    std::vector<LayoutDivider> out;
    std::function<void(const Node&, LayoutRect, std::string)> walk = [&](const Node& n, LayoutRect r, std::string path) {
        if (n.leaf()) {
            return;
        }
        if (n.horizontal) {
            const float wa = (r.w - gap) * n.ratio;
            out.push_back({path, true, {r.x + wa, r.y, gap, r.h}, r.w - gap, n.ratio});
            walk(*n.a, {r.x, r.y, wa, r.h}, path + "a");
            walk(*n.b, {r.x + wa + gap, r.y, r.w - gap - wa, r.h}, path + "b");
        } else {
            const float ha = (r.h - gap) * n.ratio;
            out.push_back({path, false, {r.x, r.y + ha, r.w, gap}, r.h - gap, n.ratio});
            walk(*n.a, {r.x, r.y, r.w, ha}, path + "a");
            walk(*n.b, {r.x, r.y + ha + gap, r.w, r.h - gap - ha}, path + "b");
        }
    };
    if (root_) {
        walk(*root_, rect, "");
    }
    return out;
}

std::optional<std::string> PaneLayout::neighbour(const std::string& from, Direction d, LayoutRect rect) const {
    const std::vector<LayoutLeaf> all = leaves(rect, 1.0f);
    const LayoutLeaf* here = nullptr;
    for (const LayoutLeaf& l : all) {
        if (std::find(l.tabs.begin(), l.tabs.end(), from) != l.tabs.end()) {
            here = &l;
        }
    }
    if (!here) {
        return std::nullopt;
    }
    const LayoutRect& h = here->rect;
    const LayoutLeaf* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    for (const LayoutLeaf& l : all) {
        if (&l == here) {
            continue;
        }
        const LayoutRect& r = l.rect;
        float distance = -1.0f;
        switch (d) {
        case Direction::Left:
            distance = overlap(h.y, h.y + h.h, r.y, r.y + r.h) && r.x + r.w <= h.x + 0.5f ? h.x - (r.x + r.w) : -1.0f;
            break;
        case Direction::Right:
            distance = overlap(h.y, h.y + h.h, r.y, r.y + r.h) && r.x >= h.x + h.w - 0.5f ? r.x - (h.x + h.w) : -1.0f;
            break;
        case Direction::Up:
            distance = overlap(h.x, h.x + h.w, r.x, r.x + r.w) && r.y + r.h <= h.y + 0.5f ? h.y - (r.y + r.h) : -1.0f;
            break;
        case Direction::Down:
            distance = overlap(h.x, h.x + h.w, r.x, r.x + r.w) && r.y >= h.y + h.h - 0.5f ? r.y - (h.y + h.h) : -1.0f;
            break;
        }
        if (distance >= 0.0f && distance < bestDistance) {
            bestDistance = distance;
            best = &l;
        }
    }
    if (!best || best->activePane() == nullptr) {
        return std::nullopt;
    }
    return *best->activePane();
}

} // namespace ke::model
