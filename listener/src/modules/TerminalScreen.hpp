// A terminal's screen, in memory: bytes in, cells out. A thin wrapper over libvterm, with no ImGui
// and no theme, so an escape sequence is testable without a window. It holds the scrollback, which
// libvterm hands off as lines leave the top of the primary screen.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ke::modules {

struct TermColor {
    enum Kind : std::uint8_t { Default, Indexed, Rgb } kind = Default;
    std::uint8_t index = 0; // 0..255 when Indexed
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const TermColor&) const = default;
};

struct TermCell {
    std::string text; // UTF-8; empty for a blank cell
    int width = 1;    // 2 for the first half of a wide character, 0 for its continuation
    TermColor fg, bg;
    bool bold = false, italic = false, underline = false, reverse = false, strike = false, blink = false;
};

using TermLine = std::vector<TermCell>;

enum class TermCursorShape { Block, Underline, Bar };

enum class TermKey {
    None, Enter, Tab, Backspace, Escape, Up, Down, Left, Right, Insert, Delete, Home, End, PageUp, PageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
};

struct TermMods {
    bool shift = false, alt = false, ctrl = false;
};

class TerminalScreen {
  public:
    TerminalScreen(int rows, int cols, std::size_t scrollback = 5000);
    ~TerminalScreen();
    TerminalScreen(const TerminalScreen&) = delete;
    TerminalScreen& operator=(const TerminalScreen&) = delete;

    // Bytes from the program.
    void feed(std::string_view bytes);
    // A clean slate before a full-screen repaint from the world: a late client is exact.
    void reset();
    void resize(int rows, int cols);

    int rows() const { return rows_; }
    int cols() const { return cols_; }
    TermLine line(int row) const; // row 0 is the top of the visible screen
    std::string text(int row) const; // that row as text, trailing blanks dropped
    std::string screenText() const;

    const std::deque<TermLine>& scrollback() const { return scrollback_; }
    int cursorRow() const { return cursorRow_; }
    int cursorCol() const { return cursorCol_; }
    bool cursorVisible() const { return cursorVisible_; }
    bool cursorBlink() const { return cursorBlink_; }
    TermCursorShape cursorShape() const { return cursorShape_; }
    bool altScreen() const { return altScreen_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    bool mouseReporting() const { return mouseMode_ != 0; }
    const std::string& title() const { return title_; }
    std::uint64_t revision() const { return revision_; } // changes whenever a redraw is due

    // What a key, text or the mouse sends to the program.
    std::string encodeText(std::string_view utf8, TermMods mods = {});
    std::string encodeKey(TermKey key, TermMods mods = {});
    std::string encodePaste(std::string_view text);
    std::string encodeMouseMove(int row, int col, TermMods mods = {});
    std::string encodeMouseButton(int row, int col, int button, bool pressed, TermMods mods = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int rows_, cols_;
    std::size_t scrollbackLimit_;
    std::deque<TermLine> scrollback_;
    int cursorRow_ = 0, cursorCol_ = 0;
    bool cursorVisible_ = true, cursorBlink_ = true;
    TermCursorShape cursorShape_ = TermCursorShape::Block;
    bool altScreen_ = false, bracketedPaste_ = false;
    int mouseMode_ = 0;
    std::string title_;
    std::uint64_t revision_ = 0;
    friend struct TermCallbacks;
};

} // namespace ke::modules
