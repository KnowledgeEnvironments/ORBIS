#include "chat/PromptSections.hpp"

namespace ke::chat {

namespace {

std::string kindOf(const std::string& line, const std::string& current) {
    if (line.rfind("# Acting", 0) == 0) {
        return "acting";
    }
    if (line.rfind("## thread", 0) == 0) {
        return "thread";
    }
    if (line.rfind("=== field instructions/", 0) == 0) {
        return "instructions";
    }
    if (line.rfind("=== field memory/", 0) == 0) {
        return "memory";
    }
    if (line.rfind("=== field ", 0) == 0) {
        return "fields";
    }
    if (line.rfind("## ", 0) == 0) {
        return "objects";
    }
    return current;
}

// A page cut off mid-array: drop the world's trailer and close what is still open, so what did
// arrive can still be read by section.
std::string closeCutJson(std::string s) {
    const std::size_t trailer = s.rfind("\n[... range truncated");
    if (trailer != std::string::npos) {
        s.resize(trailer);
    }
    std::string closers;
    bool inString = false;
    bool escaped = false;
    std::size_t lastGood = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            closers.push_back(c == '[' ? ']' : '}');
        } else if ((c == ']' || c == '}') && !closers.empty()) {
            closers.pop_back();
        }
        if (!escaped) {
            lastGood = i + 1;
        }
    }
    s.resize(lastGood);
    if (inString) {
        // A \uXXXX cut in half would fail the parse; back off to before it.
        const std::size_t u = s.rfind("\\u");
        if (u != std::string::npos && s.size() - u < 6) {
            s.resize(u);
        }
        s.push_back('"');
    }
    s.append(closers.rbegin(), closers.rend());
    return s;
}

} // namespace

std::vector<PromptSection> promptSections(const std::string& serialized) {
    std::vector<PromptSection> out;
    json parsed = json::parse(serialized, nullptr, false);
    if (parsed.is_discarded()) {
        parsed = json::parse(closeCutJson(serialized), nullptr, false);
    }
    if (parsed.is_discarded() || !parsed.is_array()) {
        out.push_back(PromptSection{"text", "", serialized});
        return out;
    }
    auto push = [&out](const std::string& name, const std::string& role, const std::string& line) {
        if (out.empty() || out.back().name != name || out.back().role != role) {
            out.push_back(PromptSection{name, role, ""});
        }
        out.back().text += line;
        out.back().text += '\n';
    };
    for (const json& m : parsed) {
        const std::string role = m.value("role", "");
        const std::string content = m.contains("content") && m["content"].is_string() ? m["content"].get<std::string>() : m.dump();
        std::string kind = role == "system" ? "identity" : "message";
        std::size_t at = 0;
        while (at <= content.size()) {
            const std::size_t nl = content.find('\n', at);
            const std::string line = content.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            kind = kindOf(line, kind);
            push(kind, role, line);
            if (nl == std::string::npos) {
                break;
            }
            at = nl + 1;
        }
    }
    return out;
}

} // namespace ke::chat
