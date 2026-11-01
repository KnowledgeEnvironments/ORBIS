#include "chat/Symbols.hpp"

#include "chat/MessageCache.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace ke::chat {

namespace {

bool isTokenChar(char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return std::isalnum(c) != 0 || c == '.' || c == '/' || c == '-' || c == '_';
}

// `:7` or `:7-15` after a path: where its end is, and the first line. `end` itself when there is none.
std::size_t lineSuffix(std::string_view text, std::size_t end, int& line) {
    if (end + 1 >= text.size() || text[end] != ':' || std::isdigit(static_cast<unsigned char>(text[end + 1])) == 0) {
        return end;
    }
    std::size_t e = end + 1;
    int first = 0;
    while (e < text.size() && std::isdigit(static_cast<unsigned char>(text[e])) != 0 && e < end + 8) {
        first = first * 10 + (text[e] - '0');
        ++e;
    }
    if (e + 1 < text.size() && text[e] == '-' && std::isdigit(static_cast<unsigned char>(text[e + 1])) != 0) {
        ++e;
        while (e < text.size() && std::isdigit(static_cast<unsigned char>(text[e])) != 0) {
            ++e;
        }
    }
    line = first;
    return e;
}

bool isHex(char ch) {
    return std::isdigit(static_cast<unsigned char>(ch)) != 0 || (ch >= 'a' && ch <= 'f');
}

// 8-4-4-4-12
bool isFullId(std::string_view t) {
    if (t.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < t.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? t[i] != '-' : !isHex(t[i])) {
            return false;
        }
    }
    return true;
}

// Eight or more hex characters, dashes allowed after the eighth: what an id looks like cut short.
bool isIdPrefix(std::string_view t) {
    if (t.size() < 8 || t.size() >= 36) {
        return false;
    }
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (!(isHex(t[i]) || (i >= 8 && t[i] == '-'))) {
            return false;
        }
    }
    return true;
}

// Does the text before `at` say that an id comes next? `(`, or a word such as `message`.
bool idBelongsHere(std::string_view text, std::size_t at) {
    std::size_t i = at;
    if (i > 0 && text[i - 1] == '(') {
        return true;
    }
    while (i > 0 && (text[i - 1] == ' ' || text[i - 1] == ':')) {
        --i;
    }
    const std::size_t end = i;
    while (i > 0 && std::isalpha(static_cast<unsigned char>(text[i - 1])) != 0) {
        --i;
    }
    std::string word(text.substr(i, end - i));
    std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return word == "message" || word == "task" || word == "evaluation" || word == "via" || word == "id" ||
           word == "object" || word == "token" || word == "reply";
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

int classRank(const std::string& cls) {
    if (cls == "principal") {
        return 0;
    }
    if (cls == "route") {
        return 1;
    }
    if (cls == "adapter" || cls == "model" || cls == "group") {
        return 2;
    }
    if (cls == "action") {
        return 3;
    }
    return 4;
}

} // namespace

bool SymbolTable::refresh(const model::Principals& fold, const model::Actions* actions, const model::Tasks* tasks,
                          const model::Evaluations* evals) {
    const Key key{fold.namesVersion(), fold.messages().size(), fold.size(), actions ? actions->size() : 0,
                  tasks ? tasks->size() : 0, evals ? evals->size() : 0};
    if (built_ && key == key_) {
        return false;
    }
    std::map<std::string, Entry> all;
    auto put = [&all](const std::string& id, std::string name, const std::string& cls) {
        Entry& e = all[id];
        e.id = id;
        if (!name.empty()) {
            e.name = std::move(name);
        }
        if (!cls.empty()) {
            e.cls = cls;
        }
    };
    fold.forEachName([&](const std::string& id, const std::string& name) { put(id, name, ""); });
    for (const model::MessageRef& m : fold.messages()) {
        put(m.message, "", "message");
    }
    for (const auto& [id, route] : fold.routes()) {
        put(id, "", "route");
    }
    for (const model::PrincipalInfo* p : fold.ordered()) {
        const std::string n = fold.nameOf(p->id);
        put(p->id, n == idPrefix(p->id) ? std::string() : n, p->cls);
    }
    if (actions) {
        for (const model::ActionInfo* a : actions->ordered()) {
            put(a->id, "", "action"); // its bound name is `action/<name>`; the short form is added below
        }
    }
    if (tasks) {
        for (const model::TaskInfo* t : tasks->ordered()) {
            put(t->id, "", "task");
        }
    }
    if (evals) {
        evals->forEachId([&](const std::string& id) { put(id, "", "evaluation"); });
    }
    entries_.clear();
    entries_.reserve(all.size());
    names_.clear();
    for (auto& [id, e] : all) {
        entries_.push_back(std::move(e));
        if (!entries_.back().name.empty()) {
            names_[entries_.back().name] = entries_.size() - 1;
        }
    }
    if (actions) {
        for (const model::ActionInfo* a : actions->ordered()) {
            if (const Entry* e = byId(a->id); e && !a->name.empty()) {
                names_[a->name] = static_cast<std::size_t>(e - entries_.data()); // `task.run`
            }
        }
    }
    key_ = key;
    built_ = true;
    ++version_;
    return true;
}

const SymbolTable::Entry* SymbolTable::byId(std::string_view id) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), id,
                               [](const Entry& e, std::string_view v) { return e.id < v; });
    return it != entries_.end() && it->id == id ? &*it : nullptr;
}

const SymbolTable::Entry* SymbolTable::byName(std::string_view name) const {
    auto it = names_.find(std::string(name));
    return it == names_.end() ? nullptr : &entries_[it->second];
}

const SymbolTable::Entry* SymbolTable::byPrefix(std::string_view prefix) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), prefix,
                               [](const Entry& e, std::string_view v) { return e.id < v; });
    if (it == entries_.end() || it->id.compare(0, prefix.size(), prefix) != 0) {
        return nullptr;
    }
    auto next = it + 1;
    if (next != entries_.end() && next->id.compare(0, prefix.size(), prefix) == 0) {
        return nullptr; // ambiguous
    }
    return &*it;
}

std::vector<Symbol> SymbolTable::scan(std::string_view text, const ScanContext& ctx) const {
    std::vector<Symbol> out;
    const std::size_t n = text.size();
    std::size_t i = 0;
    auto emit = [&](std::size_t start, std::size_t end, SymbolKind kind, const Entry* e, std::string display) {
        Symbol s;
        s.start = start;
        s.end = end;
        s.kind = kind;
        s.object = e ? e->id : std::string();
        s.display = std::move(display);
        out.push_back(std::move(s));
    };
    while (i < n) {
        // [[id|name|class]] (the serializer's reference tag, which models half-copy)
        if (text[i] == '[' && i + 1 < n && text[i + 1] == '[') {
            const std::size_t close = text.find("]]", i + 2);
            if (close != std::string_view::npos && close - i <= 200) {
                const std::string_view inner = text.substr(i + 2, close - i - 2);
                const std::size_t bar = inner.find('|');
                const std::string_view head = inner.substr(0, bar);
                std::string given;
                if (bar != std::string_view::npos) {
                    const std::string_view rest = inner.substr(bar + 1);
                    given = std::string(rest.substr(0, rest.find('|')));
                }
                const bool shaped = isFullId(head) || isIdPrefix(head);
                const Entry* e = isFullId(head) ? byId(head) : isIdPrefix(head) ? byPrefix(head) : byName(head);
                if (e) {
                    std::string shown = !given.empty() ? given : !e->name.empty() ? e->name : idPrefix(e->id);
                    emit(i, close + 2, SymbolKind::Object, e, std::move(shown));
                    i = close + 2;
                    continue;
                }
                if (shaped) {
                    emit(i, close + 2, SymbolKind::Unresolved, nullptr, !given.empty() ? given : idPrefix(head));
                    i = close + 2;
                    continue;
                }
            }
            ++i;
            continue;
        }
        if (!isTokenChar(text[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && isTokenChar(text[j])) {
            ++j;
        }
        const std::size_t next = j;
        // An absolute path links as a file: at least two components, or one with an extension, so
        // `/` and `and/or` stay text. No mount: the world resolves it, or says it cannot.
        if (text[i] == '/' && (i == 0 || (!isTokenChar(text[i - 1]) && text[i - 1] != ':'))) {
            std::size_t e = j;
            while (e > i && (text[e - 1] == '.' || text[e - 1] == '/' || text[e - 1] == '-' || text[e - 1] == '_')) {
                --e;
            }
            const std::string_view whole = text.substr(i, e - i);
            if (whole.size() > 1 && (whole.find('/', 1) != std::string_view::npos || whole.find('.') != std::string_view::npos)) {
                Symbol s;
                s.start = i;
                s.end = lineSuffix(text, e, s.line);
                s.kind = SymbolKind::Path;
                s.path = std::string(whole);
                s.display = std::string(text.substr(i, s.end - i));
                i = std::max(next, s.end);
                out.push_back(std::move(s));
                continue;
            }
        }
        // punctuation that ends a sentence is not part of a name
        std::size_t start = i;
        std::size_t end = j;
        while (end > start && (text[end - 1] == '.' || text[end - 1] == '/' || text[end - 1] == '-' || text[end - 1] == '_')) {
            --end;
        }
        while (start < end && (text[start] == '.' || text[start] == '/' || text[start] == '-')) {
            ++start;
        }
        i = next;
        if (start == end) {
            continue;
        }
        const std::string_view tok = text.substr(start, end - start);

        if (isFullId(tok)) {
            if (const Entry* e = byId(tok)) {
                emit(start, end, SymbolKind::Object, e, !e->name.empty() ? e->name : idPrefix(e->id));
            } else {
                emit(start, end, SymbolKind::Unresolved, nullptr, std::string(tok));
            }
            continue;
        }
        if (isIdPrefix(tok)) {
            if (const Entry* e = byPrefix(tok)) {
                emit(start, end, SymbolKind::Object, e, std::string(tok));
            } else if (idBelongsHere(text, start) && !byName(tok)) {
                bool several = false;
                if (auto it = std::lower_bound(entries_.begin(), entries_.end(), tok,
                                               [](const Entry& e, std::string_view v) { return e.id < v; });
                    it != entries_.end() && it->id.compare(0, tok.size(), tok) == 0) {
                    several = true; // ambiguous, not mistyped
                }
                if (!several) {
                    emit(start, end, SymbolKind::Unresolved, nullptr, std::string(tok));
                }
            }
            continue;
        }
        if (lower(tok) == "seq") {
            std::size_t k = next;
            while (k < n && (text[k] == ' ' || text[k] == '=' || text[k] == ':' || text[k] == '#')) {
                ++k;
            }
            std::size_t d = k;
            while (d < n && std::isdigit(static_cast<unsigned char>(text[d])) != 0) {
                ++d;
            }
            if (d > k && k > next && d - k <= 15 && (d == n || !isTokenChar(text[d]) || text[d] == '.' || text[d] == '-')) {
                Symbol s;
                s.start = start;
                s.end = d;
                s.kind = SymbolKind::Seq;
                s.seq = std::stoll(std::string(text.substr(k, d - k)));
                s.display = std::string(text.substr(start, d - start));
                out.push_back(std::move(s));
                i = d;
            }
            continue;
        }
        const bool slashed = tok.find('/') != std::string_view::npos;
        const bool dotted = tok.find('.') != std::string_view::npos;
        if (slashed && ctx.fold && !ctx.object.empty() && ctx.fold->fieldOf(ctx.object, tok)) {
            Symbol s;
            s.start = start;
            s.end = end;
            s.kind = SymbolKind::Field;
            s.object = ctx.object;
            s.field = std::string(tok);
            s.display = std::string(tok);
            out.push_back(std::move(s));
            continue;
        }
        const bool hyphened = tok.find('-') != std::string_view::npos;
        if (slashed || dotted || hyphened || ctx.code) {
            if (const Entry* e = byName(tok)) {
                emit(start, end, SymbolKind::Object, e, std::string(tok));
                continue;
            }
        }
        if (slashed) {
            const std::size_t slash = tok.find('/');
            const Entry* mount = byName(tok.substr(0, slash));
            if (mount && mount->cls == "adapter") {
                Symbol s;
                s.start = start;
                s.end = lineSuffix(text, end, s.line);
                s.kind = SymbolKind::Path;
                s.object = mount->id;
                s.path = std::string(tok.substr(slash + 1));
                s.display = std::string(text.substr(start, s.end - start));
                i = std::max(i, s.end);
                out.push_back(std::move(s));
                continue;
            }
        }
    }
    return out;
}

std::vector<const SymbolTable::Entry*> SymbolTable::complete(std::string_view prefix, std::size_t limit) const {
    const std::string want = lower(prefix);
    std::vector<const Entry*> hits;
    for (const Entry& e : entries_) {
        if (!e.name.empty() && lower(e.name).compare(0, want.size(), want) == 0) {
            hits.push_back(&e);
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Entry* a, const Entry* b) {
        const int ra = classRank(a->cls), rb = classRank(b->cls);
        return ra != rb ? ra < rb : a->name < b->name;
    });
    if (hits.size() > limit) {
        hits.resize(limit);
    }
    return hits;
}

} // namespace ke::chat
