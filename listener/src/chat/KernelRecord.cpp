#include "chat/KernelRecord.hpp"

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace ke::chat {

// Terminal colour codes in a stdout tail (`ESC [ 32 m`) are noise in a chat: dropped.
std::string stripAnsi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < s.size() && !std::isalpha(static_cast<unsigned char>(s[j]))) {
                ++j;
            }
            i = j; // skip the final letter too
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

namespace {

std::string_view trimRight(std::string_view s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<long long> number(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    std::size_t j = i;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) {
        ++j;
    }
    const std::size_t digits = j;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
        ++j;
    }
    if (j == digits) {
        return std::nullopt;
    }
    return std::strtoll(std::string(s.substr(i, j - i)).c_str(), nullptr, 10);
}

// `- `cmd` → exit N (M s)` (the arrow is U+2192; `->` is accepted too). Returns false when the
// line is not a run.
bool parseRun(std::string_view line, RunRecord& out) {
    if (line.rfind("- `", 0) != 0) {
        return false;
    }
    line.remove_prefix(3);
    const std::size_t close = line.rfind('`');
    if (close == std::string_view::npos) {
        return false;
    }
    out.cmd = std::string(line.substr(0, close));
    std::string_view rest = line.substr(close + 1);
    const std::size_t ex = rest.find("exit");
    if (ex != std::string_view::npos) {
        out.exit = number(rest.substr(ex + 4));
    }
    const std::size_t paren = rest.find('(');
    if (paren != std::string_view::npos) {
        out.durationS = number(rest.substr(paren + 1));
    }
    return true;
}

} // namespace

KernelRecord splitKernelRecord(const std::string& text) {
    KernelRecord rec;
    const std::string_view heading = kKernelRecordHeading;
    const std::size_t at = text.find(heading);
    if (at == std::string::npos) {
        rec.body = text;
        return rec;
    }
    rec.present = true;
    rec.body = std::string(trimRight(std::string_view(text).substr(0, at)));
    std::string_view rest = std::string_view(text).substr(at + heading.size());
    // line by line: runs start with `- ``, tail lines are indented under the run before them
    std::size_t pos = 0;
    while (pos <= rest.size()) {
        std::size_t nl = rest.find('\n', pos);
        std::string_view line = rest.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        RunRecord run;
        if (parseRun(line, run)) {
            rec.runs.push_back(std::move(run));
        } else if (!rec.runs.empty() && !line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            std::size_t k = 0;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) {
                ++k;
            }
            rec.runs.back().tail.push_back(stripAnsi(line.substr(k)));
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    return rec;
}

} // namespace ke::chat
