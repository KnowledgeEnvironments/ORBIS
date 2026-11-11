#include "ui/ActionSchema.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace ke::ui {

// ---- EDN --------------------------------------------------------------------------------------

namespace {

struct Parser {
    const std::string& s;
    std::size_t i = 0;
    std::string err;

    void ws() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
                ++i;
            } else if (c == ';') {
                while (i < s.size() && s[i] != '\n') {
                    ++i;
                }
            } else {
                break;
            }
        }
    }
    static bool delim(char c) {
        return std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '[' || c == ']' || c == '{' || c == '}' ||
               c == '(' || c == ')' || c == '"' || c == ';';
    }
    bool value(Edn& out) {
        ws();
        if (i >= s.size()) {
            err = "unexpected end";
            return false;
        }
        const char c = s[i];
        if (c == '[' || c == '(') {
            out.kind = c == '[' ? Edn::Vector : Edn::List;
            const char close = c == '[' ? ']' : ')';
            ++i;
            while (true) {
                ws();
                if (i >= s.size()) {
                    err = "unclosed sequence";
                    return false;
                }
                if (s[i] == close) {
                    ++i;
                    return true;
                }
                Edn item;
                if (!value(item)) {
                    return false;
                }
                out.items.push_back(std::move(item));
            }
        }
        if (c == '#' && i + 1 < s.size() && s[i + 1] == '{') {
            out.kind = Edn::Set;
            i += 2;
            while (true) {
                ws();
                if (i >= s.size()) {
                    err = "unclosed set";
                    return false;
                }
                if (s[i] == '}') {
                    ++i;
                    return true;
                }
                Edn item;
                if (!value(item)) {
                    return false;
                }
                out.items.push_back(std::move(item));
            }
        }
        if (c == '{') {
            out.kind = Edn::Map;
            ++i;
            while (true) {
                ws();
                if (i >= s.size()) {
                    err = "unclosed map";
                    return false;
                }
                if (s[i] == '}') {
                    ++i;
                    return true;
                }
                Edn k, v;
                if (!value(k)) {
                    return false;
                }
                ws();
                if (i < s.size() && s[i] == '}') {
                    err = "map key without value";
                    return false;
                }
                if (!value(v)) {
                    return false;
                }
                out.entries.emplace_back(std::move(k), std::move(v));
            }
        }
        if (c == '"') {
            out.kind = Edn::String;
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    ++i;
                    const char e = s[i];
                    out.text += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
                } else {
                    out.text += s[i];
                }
                ++i;
            }
            if (i >= s.size()) {
                err = "unclosed string";
                return false;
            }
            ++i;
            return true;
        }
        if (c == ':') {
            out.kind = Edn::Keyword;
            ++i;
            while (i < s.size() && !delim(s[i])) {
                out.text += s[i++];
            }
            return !out.text.empty();
        }
        // number, symbol, true/false/nil
        std::string tok;
        while (i < s.size() && !delim(s[i])) {
            tok += s[i++];
        }
        if (tok.empty()) {
            err = std::string("unexpected '") + c + "'";
            ++i;
            return false;
        }
        if (tok == "true" || tok == "false") {
            out.kind = Edn::Bool;
            out.boolean = tok == "true";
            return true;
        }
        if (tok == "nil") {
            out.kind = Edn::Nil;
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(tok[0])) || ((tok[0] == '-' || tok[0] == '+') && tok.size() > 1 && std::isdigit(static_cast<unsigned char>(tok[1])))) {
            out.kind = Edn::Number;
            out.text = tok;
            return true;
        }
        out.kind = Edn::Symbol;
        out.text = tok;
        return true;
    }
};

bool isIdRegex(const Edn& t) {
    return t.kind == Edn::Vector && !t.items.empty() && t.items[0].isKeyword("re") && t.items.size() >= 2 &&
           t.items[1].kind == Edn::String && t.items[1].text.find("[0-9a-f]{8}") != std::string::npos;
}

std::vector<std::string> enumValues(const Edn& t) {
    std::vector<std::string> out;
    for (std::size_t k = 1; k < t.items.size(); ++k) {
        const Edn& v = t.items[k];
        if (v.kind == Edn::String || v.kind == Edn::Keyword || v.kind == Edn::Symbol) {
            out.push_back(v.text);
        }
    }
    return out;
}

bool multilineName(const std::string& name) {
    return name == "charter" || name == "text" || name == "notes" || name == "instructions" || name == "policy" ||
           name == "body" || name == "prompt";
}

std::string joinPath(const std::string& path, const std::string& name) {
    return path.empty() ? name : path + "." + name;
}

} // namespace

const Edn* Edn::get(const std::string& keyword) const {
    for (const auto& [k, v] : entries) {
        if (k.kind == Keyword && k.text == keyword) {
            return &v;
        }
    }
    return nullptr;
}

bool parseEdn(const std::string& src, Edn& out, std::string& err) {
    Parser p{src};
    out = Edn{};
    if (!p.value(out)) {
        err = p.err.empty() ? "parse error" : p.err + " at " + std::to_string(p.i);
        return false;
    }
    return true;
}

// ---- schema -> specs -------------------------------------------------------------------------

ArgSpec specFromType(const std::string& name, const Edn& type, bool optional) {
    ArgSpec a;
    a.name = name;
    a.optional = optional;
    if (type.kind == Edn::Keyword) {
        if (type.text == "string") {
            a.kind = multilineName(name) ? ArgSpec::Multiline : ArgSpec::Text;
        } else if (type.text == "int" || type.text == "number" || type.text == "double" || type.text == "pos-int" ||
                   type.text == "nat-int") {
            a.kind = ArgSpec::Int;
        } else if (type.text == "boolean") {
            a.kind = ArgSpec::Bool;
        } else if (type.text == "uuid") {
            a.kind = ArgSpec::Id;
        } else {
            a.kind = ArgSpec::Text;
            a.hint = type.text;
        }
        return a;
    }
    if (type.kind == Edn::Vector && !type.items.empty() && type.items[0].kind == Edn::Keyword) {
        const std::string& head = type.items[0].text;
        // skip an options map in position 1: [:vector {:min 1} :string]
        std::size_t first = 1;
        if (type.items.size() > 1 && type.items[1].kind == Edn::Map) {
            first = 2;
        }
        if (head == "enum") {
            a.kind = ArgSpec::Enum;
            a.values = enumValues(type);
            return a;
        }
        if (head == "re") {
            a.kind = isIdRegex(type) ? ArgSpec::Id : ArgSpec::Text;
            if (a.kind == ArgSpec::Text && type.items.size() > 1) {
                a.hint = "matches " + type.items[1].text;
            }
            return a;
        }
        if (head == "vector" || head == "sequential" || head == "set") {
            if (type.items.size() > first) {
                const Edn& inner = type.items[first];
                if (inner.kind == Edn::Vector && !inner.items.empty() && inner.items[0].isKeyword("enum")) {
                    a.kind = ArgSpec::EnumList;
                    a.values = enumValues(inner);
                    return a;
                }
                if (isIdRegex(inner) || inner.isKeyword("uuid")) {
                    a.kind = ArgSpec::IdList;
                    return a;
                }
            }
            a.kind = ArgSpec::StringList;
            return a;
        }
        if (head == "map") {
            a.kind = ArgSpec::Nested;
            for (std::size_t k = first; k < type.items.size(); ++k) {
                const Edn& entry = type.items[k];
                if (entry.kind != Edn::Vector || entry.items.empty() || entry.items[0].kind != Edn::Keyword) {
                    continue;
                }
                bool opt = false;
                std::size_t ti = 1;
                if (entry.items.size() > 1 && entry.items[1].kind == Edn::Map) {
                    if (const Edn* o = entry.items[1].get("optional")) {
                        opt = o->kind == Edn::Bool && o->boolean;
                    }
                    ti = 2;
                }
                if (ti < entry.items.size()) {
                    a.children.push_back(specFromType(entry.items[0].text, entry.items[ti], opt));
                }
            }
            return a;
        }
        if (head == "or") {
            a.kind = ArgSpec::Choice;
            for (std::size_t k = first; k < type.items.size(); ++k) {
                ArgSpec v = specFromType(name, type.items[k], false);
                // a readable variant name
                if (v.kind == ArgSpec::Enum && v.values.size() == 1) {
                    v.name = v.values.front();
                } else if (v.kind == ArgSpec::Nested && v.children.size() == 1) {
                    v.name = v.children.front().name;
                } else if (v.kind == ArgSpec::IdList) {
                    v.name = "ids";
                } else {
                    v.name = "variant " + std::to_string(a.children.size() + 1);
                }
                a.children.push_back(std::move(v));
            }
            return a;
        }
        if (head == "maybe" && type.items.size() > first) {
            a = specFromType(name, type.items[first], true);
            a.optional = true;
            return a;
        }
        if (head == "string") {
            a.kind = multilineName(name) ? ArgSpec::Multiline : ArgSpec::Text;
            return a;
        }
        if (head == "int" || head == "double") {
            a.kind = ArgSpec::Int;
            return a;
        }
    }
    a.kind = ArgSpec::Unknown;
    return a;
}

std::vector<ArgSpec> specsFromSchema(const std::string& edn, std::string& err) {
    Edn root;
    if (!parseEdn(edn, root, err)) {
        return {};
    }
    const ArgSpec top = specFromType("", root, false);
    if (top.kind != ArgSpec::Nested) {
        err = "schema is not a [:map ...] form";
        return {};
    }
    return top.children;
}

// ---- values ---------------------------------------------------------------------------------------

namespace {

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == ',' || c == '\n' || c == ' ') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

json valueOf(const ArgSpec& a, const FormValues& v, const std::string& path);

json nestedValue(const std::vector<ArgSpec>& specs, const FormValues& v, const std::string& path) {
    json out = json::object();
    for (const ArgSpec& a : specs) {
        const json val = valueOf(a, v, joinPath(path, a.name));
        if (val.is_null()) {
            continue;
        }
        if (a.optional) {
            if (val.is_string() && val.get<std::string>().empty()) {
                continue;
            }
            if (val.is_array() && val.empty()) {
                continue;
            }
        }
        out[a.name] = val;
    }
    return out;
}

json valueOf(const ArgSpec& a, const FormValues& v, const std::string& path) {
    const json* stored = nullptr;
    {
        // walk the dotted path in v.values
        const json* cur = &v.values;
        std::size_t from = 0;
        while (cur && from <= path.size()) {
            const std::size_t dot = path.find('.', from);
            const std::string part = path.substr(from, dot == std::string::npos ? std::string::npos : dot - from);
            if (!cur->is_object() || !cur->contains(part)) {
                cur = nullptr;
                break;
            }
            cur = &(*cur)[part];
            if (dot == std::string::npos) {
                break;
            }
            from = dot + 1;
        }
        stored = cur;
    }
    switch (a.kind) {
    case ArgSpec::Text:
    case ArgSpec::Multiline:
    case ArgSpec::Id:
    case ArgSpec::Enum:
        return stored && stored->is_string() ? *stored : json("");
    case ArgSpec::Bool:
        return stored && stored->is_boolean() ? *stored : json(nullptr);
    case ArgSpec::Int: {
        auto it = v.intText.find(path);
        const std::string t = it != v.intText.end() ? it->second : (stored && stored->is_number() ? std::to_string(stored->get<long long>()) : "");
        if (t.empty()) {
            return json(nullptr);
        }
        char* end = nullptr;
        const long long n = std::strtoll(t.c_str(), &end, 10);
        return end && *end == '\0' ? json(n) : json(nullptr);
    }
    case ArgSpec::EnumList: {
        json arr = json::array();
        if (stored && stored->is_array()) {
            arr = *stored;
        }
        return arr;
    }
    case ArgSpec::IdList:
    case ArgSpec::StringList: {
        auto it = v.listText.find(path);
        json arr = json::array();
        if (it != v.listText.end()) {
            for (const std::string& s : splitList(it->second)) {
                arr.push_back(s);
            }
        } else if (stored && stored->is_array()) {
            arr = *stored;
        }
        return arr;
    }
    case ArgSpec::Nested:
        return nestedValue(a.children, v, path);
    case ArgSpec::Choice: {
        auto it = v.choice.find(path);
        const int idx = it != v.choice.end() ? it->second : 0;
        if (idx < 0 || static_cast<std::size_t>(idx) >= a.children.size()) {
            return json(nullptr);
        }
        const ArgSpec& variant = a.children[static_cast<std::size_t>(idx)];
        if (variant.kind == ArgSpec::Enum && variant.values.size() == 1) {
            return json(variant.values.front());
        }
        return valueOf(variant, v, path + "#" + std::to_string(idx));
    }
    case ArgSpec::Unknown:
        return stored ? *stored : json(nullptr);
    }
    return json(nullptr);
}

json* slot(FormValues& v, const std::string& path) {
    json* cur = &v.values;
    std::size_t from = 0;
    while (true) {
        const std::size_t dot = path.find('.', from);
        const std::string part = path.substr(from, dot == std::string::npos ? std::string::npos : dot - from);
        if (!cur->is_object()) {
            *cur = json::object();
        }
        if (dot == std::string::npos) {
            return &(*cur)[part];
        }
        cur = &(*cur)[part];
        from = dot + 1;
    }
}

} // namespace

json argsFromValues(const std::vector<ArgSpec>& specs, const FormValues& v, const std::string& path) {
    return nestedValue(specs, v, path);
}

json* valueSlot(FormValues& v, const std::string& path) {
    return slot(v, path);
}

void applyDefaults(const std::vector<ArgSpec>& specs, FormValues& v, const json& defaults) {
    if (!defaults.is_object()) {
        return;
    }
    for (const auto& [k, val] : defaults.items()) {
        const ArgSpec* spec = nullptr;
        for (const ArgSpec& a : specs) {
            if (a.name == k) {
                spec = &a;
            }
        }
        if (spec && spec->kind == ArgSpec::Nested && val.is_object()) {
            // nested defaults, one level: {grant {scope {prefix x} ops [...]}}
            for (const auto& [ck, cv] : val.items()) {
                const std::string cpath = k + "." + ck;
                const ArgSpec* child = nullptr;
                for (const ArgSpec& c : spec->children) {
                    if (c.name == ck) {
                        child = &c;
                    }
                }
                if (child && child->kind == ArgSpec::Choice) {
                    for (std::size_t i = 0; i < child->children.size(); ++i) {
                        const ArgSpec& variant = child->children[i];
                        if (cv.is_string() && variant.kind == ArgSpec::Enum && variant.values.size() == 1 && variant.values.front() == cv.get<std::string>()) {
                            v.choice[cpath] = static_cast<int>(i);
                        } else if (cv.is_object() && variant.kind == ArgSpec::Nested && !variant.children.empty() && cv.contains(variant.children.front().name)) {
                            v.choice[cpath] = static_cast<int>(i);
                            *slot(v, cpath + "#" + std::to_string(i) + "." + variant.children.front().name) = cv[variant.children.front().name];
                        } else if (cv.is_array() && variant.kind == ArgSpec::IdList) {
                            v.choice[cpath] = static_cast<int>(i);
                            std::string t;
                            for (const auto& x : cv) {
                                t += (t.empty() ? "" : ", ") + (x.is_string() ? x.get<std::string>() : x.dump());
                            }
                            v.listText[cpath + "#" + std::to_string(i)] = t;
                        }
                    }
                } else if (child && (child->kind == ArgSpec::IdList || child->kind == ArgSpec::StringList) && cv.is_array()) {
                    std::string t;
                    for (const auto& x : cv) {
                        t += (t.empty() ? "" : ", ") + (x.is_string() ? x.get<std::string>() : x.dump());
                    }
                    v.listText[cpath] = t;
                } else if (child && child->kind == ArgSpec::Int && cv.is_number()) {
                    v.intText[cpath] = std::to_string(cv.get<long long>());
                } else {
                    *slot(v, cpath) = cv;
                }
            }
            continue;
        }
        if (spec && (spec->kind == ArgSpec::IdList || spec->kind == ArgSpec::StringList) && val.is_array()) {
            std::string t;
            for (const auto& x : val) {
                t += (t.empty() ? "" : ", ") + (x.is_string() ? x.get<std::string>() : x.dump());
            }
            v.listText[k] = t;
            continue;
        }
        if (spec && spec->kind == ArgSpec::Int && val.is_number()) {
            v.intText[k] = std::to_string(val.get<long long>());
            continue;
        }
        *slot(v, k) = val;
    }
}

} // namespace ke::ui
