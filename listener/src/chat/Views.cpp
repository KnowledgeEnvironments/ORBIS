#include "chat/Views.hpp"

#include "chat/MessageCache.hpp"
#include "util/Base64.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numeric>

namespace ke::chat {

namespace {

bool isHex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

json parseJsonMaybe(const std::string& s) {
    if (s.empty() || (s.front() != '{' && s.front() != '[' && s.front() != '"')) {
        return nullptr;
    }
    json v = json::parse(s, nullptr, false);
    return v.is_discarded() ? json(nullptr) : v;
}

std::string numberText(double d) {
    if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 1e15) {
        return std::to_string(static_cast<long long>(d));
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", d);
    return buf;
}

std::string scalarText(const json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number()) {
        return numberText(v.get<double>());
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_null()) {
        return "";
    }
    return v.dump();
}

} // namespace

bool looksLikeRef(std::string_view s) {
    if (s.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        } else if (!isHex(c)) {
            return false;
        }
    }
    return true;
}

bool isViewState(const json& state) {
    return stateValue(state, "view/kind").is_string();
}

ViewSpec parseView(const std::string& id, const json& state) {
    ViewSpec v;
    v.id = id;
    const json kind = stateValue(state, "view/kind");
    v.kind = kind.is_string() ? kind.get<std::string>() : std::string();
    const json title = stateValue(state, "view/title");
    v.title = title.is_string() ? title.get<std::string>() : std::string();
    const json about = stateValue(state, "view/about");
    if (about.is_array()) {
        for (const json& a : about) {
            if (a.is_string()) {
                v.about.push_back(a.get<std::string>());
            }
        }
    } else if (about.is_string()) {
        v.about.push_back(about.get<std::string>());
    }
    const json tr = stateValue(state, "view/truncated?");
    v.truncated = tr.is_boolean() && tr.get<bool>();
    const json data = stateValue(state, "view/data");
    if (data.is_string() && looksLikeRef(data.get<std::string>())) {
        v.blob = data.get<std::string>();
    } else if (data.is_string()) {
        const json parsed = parseJsonMaybe(data.get<std::string>());
        v.data = parsed.is_null() ? data : parsed;
        v.dataReady = true;
    } else if (!data.is_null()) {
        v.data = data;
        v.dataReady = true;
    } else {
        v.data = json::object();
        v.dataReady = true;
    }
    return v;
}

std::vector<unsigned char> blobBytes(const json& state) {
    std::vector<unsigned char> out;
    for (const char* key : {"file/bytes", "bytes", "blob/bytes", "serialization/bytes", "view/bytes"}) {
        const json v = stateValue(state, key);
        if (v.is_string() && util::base64Decode(v.get<std::string>(), out) && !out.empty()) {
            return out;
        }
    }
    for (const char* key : {"file/text", "text", "serialization/text"}) {
        const json v = stateValue(state, key);
        if (v.is_string()) {
            const std::string& s = v.get_ref<const std::string&>();
            out.assign(s.begin(), s.end());
            return out;
        }
    }
    out.clear();
    return out;
}

// ---- table ----------------------------------------------------------------------------------

TableModel TableModel::from(const json& data) {
    TableModel t;
    json rows = json::array();
    if (data.is_object()) {
        if (data.contains("columns") && data["columns"].is_array()) {
            // A column is a string, or the world's {key label align?} (view.table-of, cost.report).
            for (const json& c : data["columns"]) {
                if (c.is_object()) {
                    const std::string key = c.contains("key") ? scalarText(c["key"]) : std::string();
                    const std::string label = c.contains("label") ? scalarText(c["label"]) : key;
                    t.columns.push_back(label.empty() ? key : label);
                    t.keys.push_back(key.empty() ? label : key);
                    t.aligns.push_back(c.contains("align") ? scalarText(c["align"]) : std::string());
                } else {
                    const std::string s = c.is_string() ? c.get<std::string>() : scalarText(c);
                    t.columns.push_back(s);
                    t.keys.push_back(s);
                    t.aligns.push_back("");
                }
            }
        }
        if (data.contains("rows") && data["rows"].is_array()) {
            rows = data["rows"];
        }
    } else if (data.is_array()) {
        rows = data;
    }
    // Columns from the rows when the payload did not name them: the union of object keys in
    // first-seen order, or col N for arrays.
    if (t.columns.empty()) {
        std::size_t widest = 0;
        for (const json& r : rows) {
            if (r.is_object()) {
                for (const auto& [k, v] : r.items()) {
                    if (std::find(t.columns.begin(), t.columns.end(), k) == t.columns.end()) {
                        t.columns.push_back(k);
                    }
                }
            } else if (r.is_array()) {
                widest = std::max(widest, r.size());
            }
        }
        for (std::size_t i = t.columns.size(); i < widest; ++i) {
            t.columns.push_back("col " + std::to_string(i + 1));
        }
    }
    while (t.keys.size() < t.columns.size()) {
        t.keys.push_back(t.columns[t.keys.size()]);
    }
    t.aligns.resize(t.columns.size());
    for (const json& r : rows) {
        std::vector<json> row(t.columns.size(), json(nullptr));
        if (r.is_object()) {
            for (std::size_t i = 0; i < t.columns.size(); ++i) {
                if (r.contains(t.keys[i])) {
                    row[i] = r[t.keys[i]];
                } else if (r.contains(t.columns[i])) {
                    row[i] = r[t.columns[i]];
                }
            }
        } else if (r.is_array()) {
            for (std::size_t i = 0; i < std::min(r.size(), row.size()); ++i) {
                row[i] = r[i];
            }
        } else {
            if (!row.empty()) {
                row[0] = r;
            }
        }
        t.rows.push_back(std::move(row));
    }
    t.order.resize(t.rows.size());
    std::iota(t.order.begin(), t.order.end(), 0);
    return t;
}

void TableModel::sortBy(int col, bool asc) {
    sortCol = col;
    sortAsc = asc;
    order.resize(rows.size());
    std::iota(order.begin(), order.end(), 0);
    if (col < 0 || static_cast<std::size_t>(col) >= columns.size()) {
        sortCol = -1;
        return;
    }
    const std::size_t c = static_cast<std::size_t>(col);
    auto numeric = [&](const json& v, double& d) {
        if (v.is_number()) {
            d = v.get<double>();
            return true;
        }
        if (v.is_string()) {
            const std::string& s = v.get_ref<const std::string&>();
            char* end = nullptr;
            d = std::strtod(s.c_str(), &end);
            return end != s.c_str() && *end == '\0';
        }
        return false;
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const json& va = rows[a][c];
        const json& vb = rows[b][c];
        double da = 0, db = 0;
        const bool na = numeric(va, da), nb = numeric(vb, db);
        bool less;
        if (na && nb) {
            less = da < db;
        } else if (na != nb) {
            less = na; // numbers before text
        } else {
            less = cellText(va) < cellText(vb);
        }
        return asc ? less : (na && nb ? db < da : (na != nb ? nb : cellText(vb) < cellText(va)));
    });
}

std::string TableModel::cellText(const json& cell) {
    if (cell.is_string() && looksLikeRef(cell.get<std::string>())) {
        return idPrefix(cell.get<std::string>());
    }
    if (cell.is_object() && cell.contains("ref") && cell["ref"].is_string()) {
        if (cell.contains("name") && cell["name"].is_string()) {
            return cell["name"].get<std::string>();
        }
        return idPrefix(cell["ref"].get<std::string>());
    }
    return scalarText(cell);
}

std::string TableModel::cellRef(const json& cell) {
    if (cell.is_string() && looksLikeRef(cell.get<std::string>())) {
        return cell.get<std::string>();
    }
    if (cell.is_object() && cell.contains("ref") && cell["ref"].is_string()) {
        return cell["ref"].get<std::string>();
    }
    return "";
}

std::string TableModel::tsv() const {
    std::string out;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i) {
            out += '\t';
        }
        out += columns[i];
    }
    out += '\n';
    for (const std::size_t ri : order) {
        const auto& row = rows[ri];
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i) {
                out += '\t';
            }
            std::string s = row[i].is_string() ? row[i].get<std::string>() : scalarText(row[i]);
            std::replace(s.begin(), s.end(), '\t', ' ');
            std::replace(s.begin(), s.end(), '\n', ' ');
            out += s;
        }
        out += '\n';
    }
    return out;
}

// ---- tree -----------------------------------------------------------------------------------

std::size_t TreeNode::count() const {
    std::size_t n = 1;
    for (const TreeNode& c : children) {
        n += c.count();
    }
    return n;
}

namespace {

TreeNode nodeFrom(const std::string& label, const json& v, int depth) {
    TreeNode n;
    n.label = label;
    if (v.is_object()) {
        // An explicit node: {label, ref?, value?, children?}; otherwise every key is a child.
        const bool explicitNode = v.contains("label") || v.contains("children");
        if (explicitNode) {
            if (v.contains("label") && v["label"].is_string()) {
                n.label = v["label"].get<std::string>();
            }
            if (v.contains("ref") && v["ref"].is_string()) {
                n.ref = v["ref"].get<std::string>();
            }
            if (v.contains("value") && !v["value"].is_null()) {
                n.value = scalarText(v["value"]);
            }
            if (v.contains("children") && v["children"].is_array() && depth < 32) {
                for (const json& c : v["children"]) {
                    n.children.push_back(nodeFrom(c.is_object() && c.contains("label") ? std::string() : std::string("·"), c,
                                                  depth + 1));
                }
            }
            return n;
        }
        if (depth < 32) {
            for (const auto& [k, cv] : v.items()) {
                n.children.push_back(nodeFrom(k, cv, depth + 1));
            }
        }
        return n;
    }
    if (v.is_array()) {
        if (depth < 32) {
            std::size_t i = 0;
            for (const json& c : v) {
                n.children.push_back(nodeFrom(std::to_string(i++), c, depth + 1));
            }
        }
        return n;
    }
    if (v.is_string() && looksLikeRef(v.get<std::string>())) {
        n.ref = v.get<std::string>();
        n.value = idPrefix(n.ref);
        return n;
    }
    n.value = scalarText(v);
    return n;
}

} // namespace

TreeNode treeFrom(const json& data) {
    if (data.is_object() && (data.contains("label") || data.contains("children"))) {
        return nodeFrom("", data, 0);
    }
    if (data.is_object() && data.contains("root")) {
        return nodeFrom("", data["root"], 0);
    }
    TreeNode root = nodeFrom("", data, 0);
    if (root.label.empty() && root.children.size() == 1 && root.value.empty()) {
        return root.children.front();
    }
    return root;
}

// ---- graph ----------------------------------------------------------------------------------

model::GraphModel graphFrom(const json& data) {
    model::GraphModel g;
    if (!data.is_object()) {
        return g;
    }
    Seq order = 0;
    auto str = [](const json& o, const char* key) {
        return o.is_object() && o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
    };
    if (data.contains("nodes") && data["nodes"].is_array()) {
        for (const json& nj : data["nodes"]) {
            model::GraphNode n;
            if (nj.is_string()) {
                n.id = nj.get<std::string>();
                n.name = looksLikeRef(n.id) ? idPrefix(n.id) : n.id;
            } else {
                n.id = str(nj, "id");
                if (n.id.empty()) {
                    n.id = str(nj, "name");
                }
                n.name = str(nj, "name");
                if (n.name.empty()) {
                    n.name = looksLikeRef(n.id) ? idPrefix(n.id) : n.id;
                }
                n.cls = str(nj, "cls");
                if (n.cls.empty()) {
                    n.cls = str(nj, "class");
                }
                n.drivenBy = str(nj, "driven-by");
                if (nj.contains("x") && nj.contains("y") && nj["x"].is_number() && nj["y"].is_number()) {
                    n.placed = true;
                    n.x = nj["x"].get<float>();
                    n.y = nj["y"].get<float>();
                }
            }
            if (n.id.empty()) {
                continue;
            }
            n.created = ++order;
            g.nodes.push_back(std::move(n));
        }
    }
    if (data.contains("edges") && data["edges"].is_array()) {
        for (const json& ej : data["edges"]) {
            model::GraphEdge e;
            if (ej.is_array() && ej.size() >= 2 && ej[0].is_string() && ej[1].is_string()) {
                e.from = ej[0].get<std::string>();
                e.to = ej[1].get<std::string>();
                if (ej.size() > 2 && ej[2].is_string()) {
                    e.label = ej[2].get<std::string>();
                }
            } else {
                e.from = str(ej, "from");
                e.to = str(ej, "to");
                e.label = str(ej, "label");
                const std::string kind = str(ej, "kind");
                if (kind == "model") {
                    e.kind = model::EdgeKind::Model;
                } else if (kind == "adapter") {
                    e.kind = model::EdgeKind::Adapter;
                } else if (kind == "workspace") {
                    e.kind = model::EdgeKind::Workspace;
                }
            }
            if (e.from.empty() || e.to.empty()) {
                continue;
            }
            // Edges may name nodes the payload forgot to list: add them so the picture is whole.
            for (const std::string* id : {&e.from, &e.to}) {
                if (!g.node(*id)) {
                    model::GraphNode n;
                    n.id = *id;
                    n.name = looksLikeRef(*id) ? idPrefix(*id) : *id;
                    n.created = ++order;
                    g.nodes.push_back(std::move(n));
                }
            }
            e.traffic = true;
            g.edges.push_back(std::move(e));
        }
    }
    model::layoutLayers(g);
    return g;
}

// ---- image ----------------------------------------------------------------------------------

ImageBytes imageFrom(const json& data) {
    ImageBytes img;
    if (data.is_object()) {
        if (data.contains("mime") && data["mime"].is_string()) {
            img.mime = data["mime"].get<std::string>();
        }
        if (data.contains("bytes") && data["bytes"].is_string()) {
            const std::string& b = data["bytes"].get_ref<const std::string&>();
            if (looksLikeRef(b)) {
                img.blob = b;
            } else {
                util::base64Decode(b, img.bytes);
            }
        } else if (data.contains("blob") && data["blob"].is_string()) {
            img.blob = data["blob"].get<std::string>();
        }
    } else if (data.is_string()) {
        const std::string& b = data.get_ref<const std::string&>();
        if (looksLikeRef(b)) {
            img.blob = b;
        } else {
            util::base64Decode(b, img.bytes);
        }
    }
    return img;
}

// ---- text -----------------------------------------------------------------------------------

std::string textFrom(const json& data) {
    if (data.is_string()) {
        return data.get<std::string>();
    }
    if (data.is_object()) {
        for (const char* key : {"text", "markdown", "body"}) {
            if (data.contains(key) && data[key].is_string()) {
                return data[key].get<std::string>();
            }
        }
    }
    return data.is_null() ? std::string() : data.dump(2);
}

// ---- cache ----------------------------------------------------------------------------------

bool ViewCache::request(const std::string& id) {
    ++version_;
    Entry& e = entries_[id];
    if (e.state != ViewState::Unknown || e.notView) {
        return false;
    }
    e.state = ViewState::Pending;
    return true;
}

std::string ViewCache::onRead(const std::string& id, const json& state) {
    ++version_;
    Entry& e = entries_[id];
    if (!isViewState(state)) {
        e.state = ViewState::Unknown;
        e.notView = true;
        return "";
    }
    e.spec = parseView(id, state);
    e.error.clear();
    e.notView = false;
    if (!e.spec.blob.empty() && !e.spec.dataReady) {
        e.state = ViewState::Pending;
        auto& waiters = blobWaiters_[e.spec.blob];
        const bool first = waiters.empty();
        waiters.push_back(id);
        return first ? e.spec.blob : std::string();
    }
    e.state = ViewState::Ready;
    return "";
}

void ViewCache::onReadFailed(const std::string& id, const std::string& code) {
    ++version_;
    Entry& e = entries_[id];
    e.state = ViewState::Failed;
    e.error = code;
}

void ViewCache::onBlob(const std::string& blob, const json& state) {
    ++version_;
    auto it = blobWaiters_.find(blob);
    if (it == blobWaiters_.end()) {
        return;
    }
    const std::vector<unsigned char> bytes = blobBytes(state);
    for (const std::string& id : it->second) {
        Entry& e = entries_[id];
        if (e.spec.kind == "image") {
            e.spec.data = json{{"mime", e.spec.data.is_object() && e.spec.data.contains("mime") ? e.spec.data["mime"] : json("")},
                               {"bytes", util::base64Encode(bytes.data(), bytes.size())}};
        } else {
            const std::string text(bytes.begin(), bytes.end());
            const json parsed = parseJsonMaybe(text);
            e.spec.data = parsed.is_null() ? json(text) : parsed;
        }
        e.spec.dataReady = true;
        e.state = ViewState::Ready;
    }
    blobWaiters_.erase(it);
}

void ViewCache::onBlobFailed(const std::string& blob, const std::string& code) {
    ++version_;
    auto it = blobWaiters_.find(blob);
    if (it == blobWaiters_.end()) {
        return;
    }
    for (const std::string& id : it->second) {
        Entry& e = entries_[id];
        e.state = ViewState::Failed;
        e.error = "blob " + code;
    }
    blobWaiters_.erase(it);
}

std::vector<std::string> ViewCache::resetPending() {
    ++version_;
    std::vector<std::string> again;
    for (auto& [id, e] : entries_) {
        if (e.state == ViewState::Pending && e.spec.blob.empty()) {
            e.state = ViewState::Unknown;
            again.push_back(id);
        }
    }
    for (auto& [blob, waiters] : blobWaiters_) {
        again.push_back(blob);
    }
    return again;
}

ViewState ViewCache::state(const std::string& id) const {
    auto it = entries_.find(id);
    return it == entries_.end() ? ViewState::Unknown : it->second.state;
}

const ViewSpec* ViewCache::get(const std::string& id) const {
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.state == ViewState::Ready ? &it->second.spec : nullptr;
}

const std::string* ViewCache::failure(const std::string& id) const {
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.state == ViewState::Failed ? &it->second.error : nullptr;
}

bool ViewCache::knownNotView(const std::string& id) const {
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.notView;
}

} // namespace ke::chat
