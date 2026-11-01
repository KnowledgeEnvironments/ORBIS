// The terminal's screen against what real programs send: colours, the alternate screen vim and
// less use, cursor shapes, scrollback, bracketed paste, and what keys encode to.
#include "modules/TerminalScreen.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ke::modules;

TEST_CASE("text lands in cells and the cursor follows") {
    TerminalScreen t(5, 20);
    t.feed("hello\r\nworld");
    CHECK(t.text(0) == "hello");
    CHECK(t.text(1) == "world");
    CHECK(t.cursorRow() == 1);
    CHECK(t.cursorCol() == 5);
    t.feed("\x1b[1;1H\x1b[2K");
    CHECK(t.text(0).empty());
}

TEST_CASE("colours: ANSI, 256 and truecolour, and the attributes") {
    TerminalScreen t(3, 30);
    t.feed("\x1b[31mr\x1b[0m\x1b[38;5;208mo\x1b[0m\x1b[38;2;1;2;3mt\x1b[0m\x1b[1;4;7mb\x1b[0m");
    const TermLine l = t.line(0);
    CHECK(l[0].fg.kind == TermColor::Indexed);
    CHECK(l[0].fg.index == 1);
    CHECK(l[1].fg.kind == TermColor::Indexed);
    CHECK(l[1].fg.index == 208);
    CHECK(l[2].fg.kind == TermColor::Rgb);
    CHECK(l[2].fg.b == 3);
    CHECK(l[3].bold);
    CHECK(l[3].underline);
    CHECK(l[3].reverse);
    CHECK(t.line(1)[0].fg.kind == TermColor::Default);
}

TEST_CASE("the alternate screen hides the shell and gives it back, as vim does") {
    TerminalScreen t(4, 20);
    t.feed("prompt$ ");
    t.feed("\x1b[?1049h\x1b[2J\x1b[H~ vim");
    CHECK(t.altScreen());
    CHECK(t.text(0) == "~ vim");
    t.feed("\x1b[?1049l");
    CHECK_FALSE(t.altScreen());
    CHECK(t.text(0) == "prompt$");
}

TEST_CASE("lines leaving the top go to the scrollback, and only from the primary screen") {
    TerminalScreen t(3, 10, 2);
    t.feed("1\r\n2\r\n3\r\n4\r\n5\r\n");
    REQUIRE(t.scrollback().size() == 2);
    CHECK(t.scrollback().back()[0].text == "3");
    t.feed("\x1b[?1049h");
    t.feed("a\r\nb\r\nc\r\nd\r\n");
    CHECK(t.scrollback().size() == 2);
}

TEST_CASE("cursor shape, title and bracketed paste follow the program") {
    TerminalScreen t(3, 20);
    t.feed("\x1b[6 q\x1b]2;my title\x07\x1b[?2004h");
    CHECK(t.cursorShape() == TermCursorShape::Bar);
    CHECK(t.title() == "my title");
    CHECK(t.bracketedPaste());
    CHECK(t.encodePaste("x") == "\x1b[200~x\x1b[201~");
    t.feed("\x1b[?2004l");
    CHECK(t.encodePaste("x") == "x");
}

TEST_CASE("keys encode as a terminal does, and honour application cursor mode") {
    TerminalScreen t(3, 20);
    CHECK(t.encodeText("a") == "a");
    CHECK(t.encodeKey(TermKey::Enter) == "\r");
    CHECK(t.encodeKey(TermKey::Up) == "\x1b[A");
    t.feed("\x1b[?1h");
    CHECK(t.encodeKey(TermKey::Up) == "\x1bOA");
    CHECK(t.encodeText("c", TermMods{false, false, true}) == "\x03");
}

TEST_CASE("mouse reporting encodes only when the program asked for it") {
    TerminalScreen t(10, 40);
    CHECK_FALSE(t.mouseReporting());
    CHECK(t.encodeMouseButton(1, 2, 1, true).empty());
    CHECK(t.encodeMouseButton(1, 2, 1, false).empty());
    t.feed("\x1b[?1000h\x1b[?1006h");
    CHECK(t.mouseReporting());
    CHECK(t.encodeMouseButton(1, 2, 1, true) == "\x1b[<0;3;2M");
}

TEST_CASE("resize keeps the content, a wide character takes two cells, reset clears") {
    TerminalScreen t(4, 20);
    t.feed("keep\r\n");
    t.resize(6, 30);
    CHECK(t.rows() == 6);
    CHECK(t.text(0) == "keep");
    t.feed("\xe6\xbc\xa2x");
    const TermLine l = t.line(1);
    CHECK(l[0].width == 2);
    CHECK(l[1].width == 0);
    CHECK(l[2].text == "x");
    t.reset();
    CHECK(t.text(0).empty());
}
