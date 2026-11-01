#include "modules/TextBuffer.hpp"

#include <algorithm>
#include <cctype>

namespace ke::modules {

namespace {

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
bool wordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || (c & 0x80) != 0; }

std::size_t codePoints(std::string_view s) {
    return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char c) { return !continuation(c); }));
}

} // namespace

void TextBuffer::setText(std::string_view text, bool foreign) {
    lines_.clear();
    std::size_t s = 0;
    while (true) {
        const std::size_t e = text.find('\n', s);
        if (e == std::string_view::npos) {
            lines_.emplace_back(text.substr(s));
            break;
        }
        lines_.emplace_back(text.substr(s, e - s));
        s = e + 1;
    }
    cursor_ = clamp(cursor_);
    anchor_ = clamp(anchor_);
    if (foreign) {
        undo_.clear();
        redo_.clear();
    }
    ++revision_;
}

std::string TextBuffer::text() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i > 0) {
            out += '\n';
        }
        out += lines_[i];
    }
    return out;
}

TextPos TextBuffer::clamp(TextPos p) const {
    p.line = std::clamp(p.line, 0, lineCount() - 1);
    const std::string& l = line(p.line);
    p.col = std::clamp(p.col, 0, static_cast<int>(l.size()));
    while (p.col > 0 && p.col < static_cast<int>(l.size()) && continuation(l[static_cast<std::size_t>(p.col)])) {
        --p.col;
    }
    return p;
}

std::size_t TextBuffer::byteOf(TextPos p) const {
    std::size_t b = 0;
    for (int i = 0; i < p.line; ++i) {
        b += line(i).size() + 1;
    }
    return b + static_cast<std::size_t>(p.col);
}

TextPos TextBuffer::posOfByte(std::size_t b) const {
    TextPos p;
    for (int i = 0; i < lineCount(); ++i) {
        const std::size_t n = line(i).size();
        if (b <= n) {
            p.line = i;
            p.col = static_cast<int>(b);
            return p;
        }
        b -= n + 1;
    }
    p.line = lineCount() - 1;
    p.col = static_cast<int>(line(p.line).size());
    return p;
}

std::size_t TextBuffer::offsetOf(TextPos p) const {
    std::size_t n = 0;
    for (int i = 0; i < p.line; ++i) {
        n += codePoints(line(i)) + 1;
    }
    return n + codePoints(std::string_view(line(p.line)).substr(0, static_cast<std::size_t>(p.col)));
}

TextPos TextBuffer::posOf(std::size_t offset) const {
    for (int i = 0; i < lineCount(); ++i) {
        const std::string& l = line(i);
        const std::size_t n = codePoints(l);
        if (offset <= n) {
            std::size_t b = 0;
            for (std::size_t k = 0; k < offset; ++k) {
                ++b;
                while (b < l.size() && continuation(l[b])) {
                    ++b;
                }
            }
            return TextPos{i, static_cast<int>(b)};
        }
        offset -= n + 1;
    }
    return TextPos{lineCount() - 1, static_cast<int>(lines_.back().size())};
}

std::string TextBuffer::lineCol() const {
    return std::to_string(cursor_.line + 1) + ":" + std::to_string(cursor_.col + 1);
}

std::string TextBuffer::selectedText() const {
    if (!hasSelection()) {
        return "";
    }
    const TextPos a = std::min(cursor_, anchor_), b = std::max(cursor_, anchor_);
    if (a.line == b.line) {
        return line(a.line).substr(static_cast<std::size_t>(a.col), static_cast<std::size_t>(b.col - a.col));
    }
    std::string out = line(a.line).substr(static_cast<std::size_t>(a.col));
    for (int i = a.line + 1; i < b.line; ++i) {
        out += '\n';
        out += line(i);
    }
    out += '\n';
    out += line(b.line).substr(0, static_cast<std::size_t>(b.col));
    return out;
}

void TextBuffer::selectAll() {
    anchor_ = TextPos{};
    cursor_ = TextPos{lineCount() - 1, static_cast<int>(lines_.back().size())};
}

void TextBuffer::selectWord(TextPos at) {
    at = clamp(at);
    const std::string& l = line(at.line);
    int a = at.col, b = at.col;
    while (a > 0 && wordChar(l[static_cast<std::size_t>(a - 1)])) {
        --a;
    }
    while (b < static_cast<int>(l.size()) && wordChar(l[static_cast<std::size_t>(b)])) {
        ++b;
    }
    anchor_ = TextPos{at.line, a};
    cursor_ = TextPos{at.line, b};
}

void TextBuffer::moveTo(TextPos p, bool extend) {
    cursor_ = clamp(p);
    if (!extend) {
        anchor_ = cursor_;
    }
    wantCol_ = -1;
}

void TextBuffer::moveLeft(bool extend, bool word) {
    TextPos p = cursor_;
    if (!extend && hasSelection() && !word) {
        moveTo(std::min(cursor_, anchor_));
        return;
    }
    if (p.col == 0) {
        if (p.line > 0) {
            --p.line;
            p.col = static_cast<int>(line(p.line).size());
        }
    } else if (word) {
        const std::string& l = line(p.line);
        while (p.col > 0 && !wordChar(l[static_cast<std::size_t>(p.col - 1)])) {
            --p.col;
        }
        while (p.col > 0 && wordChar(l[static_cast<std::size_t>(p.col - 1)])) {
            --p.col;
        }
    } else {
        --p.col;
        while (p.col > 0 && continuation(line(p.line)[static_cast<std::size_t>(p.col)])) {
            --p.col;
        }
    }
    moveTo(p, extend);
}

void TextBuffer::moveRight(bool extend, bool word) {
    TextPos p = cursor_;
    if (!extend && hasSelection() && !word) {
        moveTo(std::max(cursor_, anchor_));
        return;
    }
    const std::string& l = line(p.line);
    if (p.col >= static_cast<int>(l.size())) {
        if (p.line + 1 < lineCount()) {
            ++p.line;
            p.col = 0;
        }
    } else if (word) {
        while (p.col < static_cast<int>(l.size()) && !wordChar(l[static_cast<std::size_t>(p.col)])) {
            ++p.col;
        }
        while (p.col < static_cast<int>(l.size()) && wordChar(l[static_cast<std::size_t>(p.col)])) {
            ++p.col;
        }
    } else {
        ++p.col;
        while (p.col < static_cast<int>(l.size()) && continuation(l[static_cast<std::size_t>(p.col)])) {
            ++p.col;
        }
    }
    moveTo(p, extend);
}

void TextBuffer::moveUp(bool extend, int lines) {
    const int want = wantCol_ >= 0 ? wantCol_ : cursor_.col;
    TextPos p{std::max(0, cursor_.line - lines), want};
    moveTo(p, extend);
    wantCol_ = want;
}

void TextBuffer::moveDown(bool extend, int lines) {
    const int want = wantCol_ >= 0 ? wantCol_ : cursor_.col;
    TextPos p{std::min(lineCount() - 1, cursor_.line + lines), want};
    moveTo(p, extend);
    wantCol_ = want;
}

void TextBuffer::moveHome(bool extend) {
    // First press to the first non-blank, the next to the margin.
    const std::string& l = line(cursor_.line);
    int first = 0;
    while (first < static_cast<int>(l.size()) && (l[static_cast<std::size_t>(first)] == ' ' || l[static_cast<std::size_t>(first)] == '\t')) {
        ++first;
    }
    moveTo(TextPos{cursor_.line, cursor_.col == first ? 0 : first}, extend);
}

void TextBuffer::moveEnd(bool extend) { moveTo(TextPos{cursor_.line, static_cast<int>(line(cursor_.line).size())}, extend); }
void TextBuffer::moveDocStart(bool extend) { moveTo(TextPos{}, extend); }
void TextBuffer::moveDocEnd(bool extend) { moveTo(TextPos{lineCount() - 1, static_cast<int>(lines_.back().size())}, extend); }

void TextBuffer::apply(std::size_t at, std::size_t remove, std::string_view add) {
    const TextPos a = posOfByte(at), b = posOfByte(at + remove);
    const std::size_t startCp = offsetOf(a), endCp = offsetOf(b);
    const std::string head = line(a.line).substr(0, static_cast<std::size_t>(a.col));
    const std::string tail = line(b.line).substr(static_cast<std::size_t>(b.col));
    lines_.erase(lines_.begin() + a.line, lines_.begin() + b.line + 1);
    std::vector<std::string> fresh;
    std::string cur = head;
    for (char c : add) {
        if (c == '\n') {
            fresh.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    cur += tail;
    fresh.push_back(std::move(cur));
    lines_.insert(lines_.begin() + a.line, fresh.begin(), fresh.end());
    edits_.push_back(TextEdit{startCp, endCp, std::string(add)});
    dirty_ = true;
    ++revision_;
}

void TextBuffer::replaceRange(TextPos a, TextPos b, std::string_view add, bool recordUndo) {
    const std::size_t at = byteOf(a), end = byteOf(b);
    Undo u;
    u.at = at;
    u.before = cursor_;
    if (recordUndo) {
        const std::string all = text();
        u.removed = all.substr(at, end - at);
        u.added = std::string(add);
    }
    apply(at, end - at, add);
    cursor_ = anchor_ = posOfByte(at + add.size());
    wantCol_ = -1;
    if (recordUndo) {
        u.after = cursor_;
        // Typing in one run undoes as one step.
        const bool oneChar = u.removed.empty() && add.size() == 1 && add[0] != '\n' && add[0] != ' ';
        if (oneChar && !undo_.empty() && undo_.back().removed.empty() &&
            undo_.back().at + undo_.back().added.size() == at && undo_.back().added.back() != '\n') {
            undo_.back().added += add;
            undo_.back().after = u.after;
        } else {
            undo_.push_back(std::move(u));
        }
        redo_.clear();
    }
}

void TextBuffer::insert(std::string_view t) {
    replaceRange(std::min(cursor_, anchor_), std::max(cursor_, anchor_), t, true);
}

void TextBuffer::backspace(bool word) {
    if (!hasSelection()) {
        moveLeft(true, word);
    }
    if (hasSelection()) {
        insert("");
    }
}

void TextBuffer::del(bool word) {
    if (!hasSelection()) {
        moveRight(true, word);
    }
    if (hasSelection()) {
        insert("");
    }
}

bool TextBuffer::undo() {
    if (undo_.empty()) {
        return false;
    }
    Undo u = std::move(undo_.back());
    undo_.pop_back();
    apply(u.at, u.added.size(), u.removed);
    cursor_ = anchor_ = clamp(u.before);
    redo_.push_back(std::move(u));
    return true;
}

bool TextBuffer::redo() {
    if (redo_.empty()) {
        return false;
    }
    Undo u = std::move(redo_.back());
    redo_.pop_back();
    apply(u.at, u.removed.size(), u.added);
    cursor_ = anchor_ = clamp(u.after);
    undo_.push_back(std::move(u));
    return true;
}

void TextBuffer::applyRemote(std::size_t at, std::size_t del, std::string_view text) {
    const std::size_t cursor = offsetOf(cursor_), anchor = offsetOf(anchor_);
    const std::size_t added = codePoints(text);
    const auto moved = [&](std::size_t p) {
        if (p <= at) {
            return p;
        }
        return p >= at + del ? p - del + added : at + added;
    };
    const std::size_t a = byteOf(posOf(at)), b = byteOf(posOf(at + del));
    std::vector<TextEdit> keep;
    keep.swap(edits_);
    const bool wasDirty = dirty_;
    apply(a, b - a, text);
    dirty_ = wasDirty;
    edits_.swap(keep);
    cursor_ = posOf(moved(cursor));
    anchor_ = posOf(moved(anchor));
    undo_.clear();
    redo_.clear();
}

bool TextBuffer::find(std::string_view needle, bool forward) {
    if (needle.empty()) {
        return false;
    }
    const std::string all = text();
    const std::size_t from = byteOf(forward ? std::max(cursor_, anchor_) : std::min(cursor_, anchor_));
    std::size_t at = std::string::npos;
    if (forward) {
        at = all.find(needle, from);
        if (at == std::string::npos) {
            at = all.find(needle);
        }
    } else {
        if (from > 0) {
            at = all.rfind(needle, from - 1);
        }
        if (at == std::string::npos) {
            at = all.rfind(needle);
        }
    }
    if (at == std::string::npos) {
        return false;
    }
    anchor_ = posOfByte(at);
    cursor_ = posOfByte(at + needle.size());
    wantCol_ = -1;
    return true;
}

int TextBuffer::matchCount(std::string_view needle) const {
    if (needle.empty()) {
        return 0;
    }
    const std::string all = text();
    int n = 0;
    for (std::size_t at = all.find(needle); at != std::string::npos; at = all.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

std::vector<TextEdit> TextBuffer::takeEdits() {
    std::vector<TextEdit> out;
    out.swap(edits_);
    return out;
}

} // namespace ke::modules
