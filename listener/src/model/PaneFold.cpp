#include "model/PaneFold.hpp"

#include "model/Snapshot.hpp"

#include <algorithm>

namespace ke::model {

namespace {

std::string str(const json& j) {
    return j.is_string() ? j.get<std::string>() : std::string();
}

} // namespace

void PaneFold::setField(PaneInfo& p, const std::string& name, const json& value) {
    if (name == "object/label") {
        p.label = str(value);
    } else if (name == "pane/kind") {
        p.kind = str(value);
    } else if (name == "pane/target") {
        p.target = str(value);
    } else if (name == "pane/title") {
        p.title = str(value);
    } else if (name == "pane/field") {
        p.field = str(value);
    } else if (name == "pane/state") {
        p.state = str(value);
    } else if (name == "pane/owner") {
        p.owner = str(value);
    } else if (name == "pane/size" && value.is_object()) {
        p.cols = value.value("cols", p.cols);
        p.rows = value.value("rows", p.rows);
    } else if (name == "pane/exit" && value.is_number()) {
        p.hasExit = true;
        p.exitCode = value.get<int>();
    } else if (name == "pane/attached" && value.is_array()) {
        p.attached.clear();
        for (const json& a : value) {
            p.attached.push_back(str(a));
        }
    }
}

bool PaneFold::apply(const Event& e) {
    if (e.kind == "created" && e.body.is_object() && e.body.contains("fields") && e.body["fields"].is_object()) {
        const json& fields = e.body["fields"];
        if (!fields.contains("pane/kind")) {
            return false;
        }
        PaneInfo& p = panes_[e.stream];
        p.id = e.stream;
        p.created = e.seq;
        for (const auto& [name, value] : fields.items()) {
            setField(p, name, value);
        }
        return true;
    }
    auto it = panes_.find(e.stream);
    if (e.kind == "field-set" && it != panes_.end() && e.body.is_object()) {
        const std::string name = e.body.value("name", std::string());
        if (name.rfind("pane/", 0) == 0 || name == "object/label") {
            setField(it->second, name, e.body.value("value", json()));
            return true;
        }
        return false;
    }
    if (e.kind == "pane-closed" && e.body.is_object()) {
        const std::string id = e.body.value("pane", e.stream);
        if (auto c = panes_.find(id); c != panes_.end()) {
            c->second.state = "exited";
            return true;
        }
        return false;
    }
    if (e.kind == "pane-input" && e.body.is_object()) {
        const std::string id = e.body.value("pane", e.stream);
        auto c = panes_.find(id);
        if (c == panes_.end()) {
            return false;
        }
        PaneInput in;
        in.who = e.body.value("who", e.actor);
        in.from = e.body.value("from", std::string());
        in.text = e.body.value("text", std::string());
        in.model = e.via.has_value() || e.body.value("model", false);
        in.seq = e.seq;
        c->second.lastInput = std::move(in);
        return true;
    }
    return false;
}

void PaneFold::seed(const SnapshotObject& o) {
    if (o.field("pane/kind") == nullptr) {
        return;
    }
    PaneInfo& p = panes_[o.id];
    p.id = o.id;
    p.created = o.created;
    for (const auto& [name, f] : o.fields) {
        setField(p, name, f.value);
    }
}

const PaneInfo* PaneFold::get(std::string_view id) const {
    auto it = panes_.find(id);
    return it == panes_.end() ? nullptr : &it->second;
}

std::vector<const PaneInfo*> PaneFold::ordered() const {
    std::vector<const PaneInfo*> out;
    out.reserve(panes_.size());
    for (const auto& [id, p] : panes_) {
        out.push_back(&p);
    }
    std::sort(out.begin(), out.end(), [](const PaneInfo* a, const PaneInfo* b) {
        return a->created != b->created ? a->created < b->created : a->id < b->id;
    });
    return out;
}

} // namespace ke::model
