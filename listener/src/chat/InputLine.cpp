#include "chat/InputLine.hpp"

#include <algorithm>
#include <cctype>

namespace ke::chat {

namespace {

std::string trimmed(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

// Lower-case with runs of whitespace collapsed to one space: "Who  Am I" -> "who am i".
std::string normalized(std::string_view s) {
    std::string out;
    bool space = false;
    for (char ch : s) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out += ' ';
        }
        space = false;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool isHex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

} // namespace

std::vector<std::string> refPrefixes(std::string_view line) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '@') {
            continue;
        }
        std::string hex;
        std::size_t j = i + 1;
        for (; j < line.size() && (isHex(line[j]) || line[j] == '-'); ++j) {
            if (line[j] != '-') {
                hex += static_cast<char>(std::tolower(static_cast<unsigned char>(line[j])));
            }
        }
        if (hex.size() >= 8) {
            out.push_back(hex);
        }
        i = j > i ? j - 1 : i;
    }
    return out;
}

bool idHasPrefix(std::string_view id, std::string_view prefix) {
    std::size_t p = 0;
    for (char c : id) {
        if (c == '-') {
            continue;
        }
        if (p == prefix.size()) {
            return true;
        }
        if (std::tolower(static_cast<unsigned char>(c)) != std::tolower(static_cast<unsigned char>(prefix[p]))) {
            return false;
        }
        ++p;
    }
    return p == prefix.size();
}

Command parseLine(std::string_view line, const RefResolver& resolve) {
    Command c;
    c.text = trimmed(line);
    if (c.text.empty()) {
        return c;
    }
    if (normalized(c.text) == "who am i") {
        c.kind = CommandKind::WhoAmI;
        return c;
    }
    c.kind = CommandKind::Text;
    if (resolve) {
        for (const std::string& prefix : refPrefixes(c.text)) {
            const std::string id = resolve(prefix);
            if (!id.empty() && std::find(c.tags.begin(), c.tags.end(), id) == c.tags.end()) {
                c.tags.push_back(id);
            }
        }
    }
    return c;
}

void InputLine::push(std::string line) {
    line = trimmed(line);
    if (!line.empty() && (history_.empty() || history_.back() != line)) {
        history_.push_back(std::move(line));
        while (history_.size() > kMax) {
            history_.pop_front();
        }
    }
    resetRecall();
}

std::optional<std::string> InputLine::up(std::string_view draft) {
    if (history_.empty() || pos_ == 0) {
        return std::nullopt;
    }
    if (!recalling()) {
        draft_ = std::string(draft);
    }
    --pos_;
    return history_[pos_];
}

std::optional<std::string> InputLine::down() {
    if (!recalling()) {
        return std::nullopt;
    }
    ++pos_;
    if (pos_ >= history_.size()) {
        pos_ = history_.size();
        return draft_;
    }
    return history_[pos_];
}

void InputLine::resetRecall() {
    pos_ = history_.size();
    draft_.clear();
}

} // namespace ke::chat
