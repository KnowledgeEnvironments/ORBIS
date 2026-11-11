// The editor's document: lines, a cursor, a selection, undo, find. No ImGui and no file: the world
// holds the file, this holds what one client shows and what the person just did, as edits it
// hands to `pane.frob`. Offsets to the world are in code points, the convention of every span.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ke::modules {

struct TextPos {
    int line = 0;
    int col = 0; // bytes into the line
    bool operator==(const TextPos&) const = default;
    bool operator<(const TextPos& o) const { return line != o.line ? line < o.line : col < o.col; }
};

// One change, as the world wants it: [start, end) in code points replaced by `text`.
struct TextEdit {
    std::size_t start = 0, end = 0;
    std::string text;
};

class TextBuffer {
  public:
    TextBuffer() : lines_(1) {}

    // What the world says the document is. The cursor keeps its line and column where it can, the
    // undo history stays for a local reload and goes when `foreign` (someone else changed it).
    void setText(std::string_view text, bool foreign = false);
    std::string text() const;
    int lineCount() const { return static_cast<int>(lines_.size()); }
    const std::string& line(int i) const { return lines_[static_cast<std::size_t>(i)]; }

    TextPos cursor() const { return cursor_; }
    TextPos anchor() const { return anchor_; }
    bool hasSelection() const { return !(cursor_ == anchor_); }
    std::string selectedText() const;
    void selectAll();
    void selectWord(TextPos at);
    void clearSelection() { anchor_ = cursor_; }

    void moveTo(TextPos p, bool extend = false);
    void moveLeft(bool extend = false, bool word = false);
    void moveRight(bool extend = false, bool word = false);
    void moveUp(bool extend = false, int lines = 1);
    void moveDown(bool extend = false, int lines = 1);
    void moveHome(bool extend = false);
    void moveEnd(bool extend = false);
    void moveDocStart(bool extend = false);
    void moveDocEnd(bool extend = false);

    void insert(std::string_view text); // over the selection
    void backspace(bool word = false);
    void del(bool word = false);
    bool undo();
    bool redo();
    // Someone else's edit, at code point `at`: `del` code points replaced by `text`. It is no undo
    // history of this client's and goes nowhere; the cursor keeps its place in the text.
    void applyRemote(std::size_t at, std::size_t del, std::string_view text);

    // Selects the next match at or after the cursor (or before it, backward), wrapping. False: none.
    bool find(std::string_view needle, bool forward = true);
    int matchCount(std::string_view needle) const;

    std::size_t offsetOf(TextPos p) const;   // code points from the start
    TextPos posOf(std::size_t offset) const; // the inverse, clamped
    std::string lineCol() const;             // `12:4`, one-based, for the status line

    bool dirty() const { return dirty_; }
    void markSaved() { dirty_ = false; }
    void markDirty() { dirty_ = true; } // the world holds edits this client did not type
    unsigned long long revision() const { return revision_; }
    // What changed since the last call, for the world.
    std::vector<TextEdit> takeEdits();

  private:
    struct Undo {
        std::size_t at = 0; // bytes in the whole text
        std::string removed, added;
        TextPos before, after;
    };
    std::size_t byteOf(TextPos p) const;
    TextPos posOfByte(std::size_t b) const;
    TextPos clamp(TextPos p) const;
    void apply(std::size_t at, std::size_t remove, std::string_view add);
    void replaceRange(TextPos a, TextPos b, std::string_view add, bool recordUndo);

    std::vector<std::string> lines_;
    TextPos cursor_, anchor_;
    int wantCol_ = -1; // the column up and down aim for
    std::vector<Undo> undo_, redo_;
    std::vector<TextEdit> edits_;
    bool dirty_ = false;
    unsigned long long revision_ = 0;
};

} // namespace ke::modules
