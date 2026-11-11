#include "modules/TerminalScreen.hpp"

extern "C" {
#include <vterm.h>
}

#include <algorithm>
#include <cstring>
#include <utility>

namespace ke::modules {

namespace {

void appendUtf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

TermColor colorOf(const VTermColor& c) {
    TermColor out;
    if (VTERM_COLOR_IS_DEFAULT_FG(&c) || VTERM_COLOR_IS_DEFAULT_BG(&c)) {
        return out;
    }
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        out.kind = TermColor::Indexed;
        out.index = c.indexed.idx;
    } else {
        out.kind = TermColor::Rgb;
        out.r = c.rgb.red;
        out.g = c.rgb.green;
        out.b = c.rgb.blue;
    }
    return out;
}

TermCell cellOf(const VTermScreenCell& c) {
    TermCell out;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i] != 0; ++i) {
        appendUtf8(out.text, c.chars[i]);
    }
    if (c.chars[0] == static_cast<std::uint32_t>(-1)) {
        out.text.clear();
        out.width = 0;
    } else {
        out.width = c.width;
    }
    out.fg = colorOf(c.fg);
    out.bg = colorOf(c.bg);
    out.bold = c.attrs.bold != 0;
    out.italic = c.attrs.italic != 0;
    out.underline = c.attrs.underline != 0;
    out.reverse = c.attrs.reverse != 0;
    out.strike = c.attrs.strike != 0;
    out.blink = c.attrs.blink != 0;
    return out;
}

VTermModifier modsOf(TermMods m) {
    int v = VTERM_MOD_NONE;
    if (m.shift) {
        v |= VTERM_MOD_SHIFT;
    }
    if (m.alt) {
        v |= VTERM_MOD_ALT;
    }
    if (m.ctrl) {
        v |= VTERM_MOD_CTRL;
    }
    return static_cast<VTermModifier>(v);
}

VTermKey keyOf(TermKey k) {
    switch (k) {
    case TermKey::Enter: return VTERM_KEY_ENTER;
    case TermKey::Tab: return VTERM_KEY_TAB;
    case TermKey::Backspace: return VTERM_KEY_BACKSPACE;
    case TermKey::Escape: return VTERM_KEY_ESCAPE;
    case TermKey::Up: return VTERM_KEY_UP;
    case TermKey::Down: return VTERM_KEY_DOWN;
    case TermKey::Left: return VTERM_KEY_LEFT;
    case TermKey::Right: return VTERM_KEY_RIGHT;
    case TermKey::Insert: return VTERM_KEY_INS;
    case TermKey::Delete: return VTERM_KEY_DEL;
    case TermKey::Home: return VTERM_KEY_HOME;
    case TermKey::End: return VTERM_KEY_END;
    case TermKey::PageUp: return VTERM_KEY_PAGEUP;
    case TermKey::PageDown: return VTERM_KEY_PAGEDOWN;
    case TermKey::None: return VTERM_KEY_NONE;
    default:
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(static_cast<int>(k) - static_cast<int>(TermKey::F1) + 1));
    }
}

} // namespace

struct TerminalScreen::Impl {
    VTerm* vt = nullptr;
    VTermScreen* screen = nullptr;
    std::string output; // what vterm wrote back: key encodings and terminal replies
};

// libvterm's callbacks, with access to the screen's private state.
struct TermCallbacks {
    static int damage(VTermRect, void* user) {
        ++static_cast<TerminalScreen*>(user)->revision_;
        return 1;
    }
    static int moveCursor(VTermPos pos, VTermPos, int visible, void* user) {
        auto* t = static_cast<TerminalScreen*>(user);
        t->cursorRow_ = pos.row;
        t->cursorCol_ = pos.col;
        t->cursorVisible_ = visible != 0;
        ++t->revision_;
        return 1;
    }
    static int setTermProp(VTermProp prop, VTermValue* val, void* user) {
        auto* t = static_cast<TerminalScreen*>(user);
        switch (prop) {
        case VTERM_PROP_CURSORVISIBLE:
            t->cursorVisible_ = val->boolean != 0;
            break;
        case VTERM_PROP_CURSORBLINK:
            t->cursorBlink_ = val->boolean != 0;
            break;
        case VTERM_PROP_CURSORSHAPE:
            t->cursorShape_ = val->number == VTERM_PROP_CURSORSHAPE_UNDERLINE ? TermCursorShape::Underline
                              : val->number == VTERM_PROP_CURSORSHAPE_BAR_LEFT ? TermCursorShape::Bar
                                                                               : TermCursorShape::Block;
            break;
        case VTERM_PROP_ALTSCREEN:
            t->altScreen_ = val->boolean != 0;
            break;
        case VTERM_PROP_MOUSE:
            t->mouseMode_ = val->number;
            break;
        case VTERM_PROP_TITLE:
            if (val->string.initial) {
                t->title_.clear();
            }
            t->title_.append(val->string.str, val->string.len);
            break;
        default:
            break;
        }
        ++t->revision_;
        return 1;
    }
    static int pushline(int cols, const VTermScreenCell* cells, void* user) {
        auto* t = static_cast<TerminalScreen*>(user);
        TermLine line;
        line.reserve(static_cast<std::size_t>(cols));
        for (int i = 0; i < cols; ++i) {
            line.push_back(cellOf(cells[i]));
        }
        t->scrollback_.push_back(std::move(line));
        while (t->scrollback_.size() > t->scrollbackLimit_) {
            t->scrollback_.pop_front();
        }
        return 1;
    }
    static void output(const char* s, size_t len, void* user) {
        static_cast<TerminalScreen::Impl*>(user)->output.append(s, len);
    }
};

namespace {

const VTermScreenCallbacks kCallbacks = [] {
    VTermScreenCallbacks c{};
    c.damage = &TermCallbacks::damage;
    c.movecursor = &TermCallbacks::moveCursor;
    c.settermprop = &TermCallbacks::setTermProp;
    c.sb_pushline = &TermCallbacks::pushline;
    return c;
}();

} // namespace

TerminalScreen::TerminalScreen(int rows, int cols, std::size_t scrollback)
    : impl_(std::make_unique<Impl>()), rows_(std::max(rows, 1)), cols_(std::max(cols, 1)), scrollbackLimit_(scrollback) {
    impl_->vt = vterm_new(rows_, cols_);
    vterm_set_utf8(impl_->vt, 1);
    vterm_output_set_callback(impl_->vt, &TermCallbacks::output, impl_.get());
    impl_->screen = vterm_obtain_screen(impl_->vt);
    vterm_screen_set_callbacks(impl_->screen, &kCallbacks, this);
    vterm_screen_enable_altscreen(impl_->screen, 1);
    vterm_screen_reset(impl_->screen, 1);
}

TerminalScreen::~TerminalScreen() {
    vterm_free(impl_->vt);
}

void TerminalScreen::feed(std::string_view bytes) {
    // bracketed paste has no property of its own: watch for the mode in the stream
    for (std::string_view on : {std::string_view("\x1b[?2004h")}) {
        if (bytes.find(on) != std::string_view::npos) {
            bracketedPaste_ = true;
        }
    }
    if (bytes.find("\x1b[?2004l") != std::string_view::npos) {
        bracketedPaste_ = false;
    }
    vterm_input_write(impl_->vt, bytes.data(), bytes.size());
    vterm_screen_flush_damage(impl_->screen);
    ++revision_;
}

void TerminalScreen::reset() {
    vterm_screen_reset(impl_->screen, 1);
    bracketedPaste_ = false;
    altScreen_ = false;
    ++revision_;
}

void TerminalScreen::resize(int rows, int cols) {
    rows = std::max(rows, 1);
    cols = std::max(cols, 1);
    if (rows == rows_ && cols == cols_) {
        return;
    }
    rows_ = rows;
    cols_ = cols;
    vterm_set_size(impl_->vt, rows_, cols_);
    vterm_screen_flush_damage(impl_->screen);
    ++revision_;
}

TermLine TerminalScreen::line(int row) const {
    TermLine out;
    out.reserve(static_cast<std::size_t>(cols_));
    for (int col = 0; col < cols_; ++col) {
        VTermScreenCell cell{};
        vterm_screen_get_cell(impl_->screen, VTermPos{row, col}, &cell);
        out.push_back(cellOf(cell));
    }
    return out;
}

std::string TerminalScreen::text(int row) const {
    std::string out;
    for (const TermCell& c : line(row)) {
        out += c.text.empty() ? (c.width == 0 ? "" : " ") : c.text;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string TerminalScreen::screenText() const {
    std::string out;
    for (int r = 0; r < rows_; ++r) {
        out += text(r);
        out.push_back('\n');
    }
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') {
        out.pop_back();
    }
    return out;
}

std::string TerminalScreen::encodeText(std::string_view utf8, TermMods mods) {
    impl_->output.clear();
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char b = static_cast<unsigned char>(utf8[i]);
        std::uint32_t c = b;
        std::size_t n = 1;
        if (b >= 0xF0) {
            c = b & 0x07;
            n = 4;
        } else if (b >= 0xE0) {
            c = b & 0x0F;
            n = 3;
        } else if (b >= 0xC0) {
            c = b & 0x1F;
            n = 2;
        }
        for (std::size_t k = 1; k < n && i + k < utf8.size(); ++k) {
            c = (c << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        }
        vterm_keyboard_unichar(impl_->vt, c, modsOf(mods));
        i += n;
    }
    return std::exchange(impl_->output, {});
}

std::string TerminalScreen::encodeKey(TermKey key, TermMods mods) {
    impl_->output.clear();
    vterm_keyboard_key(impl_->vt, keyOf(key), modsOf(mods));
    return std::exchange(impl_->output, {});
}

std::string TerminalScreen::encodePaste(std::string_view text) {
    if (!bracketedPaste_) {
        return std::string(text);
    }
    return "\x1b[200~" + std::string(text) + "\x1b[201~";
}

std::string TerminalScreen::encodeMouseMove(int row, int col, TermMods mods) {
    impl_->output.clear();
    vterm_mouse_move(impl_->vt, row, col, modsOf(mods));
    return std::exchange(impl_->output, {});
}

std::string TerminalScreen::encodeMouseButton(int row, int col, int button, bool pressed, TermMods mods) {
    impl_->output.clear();
    vterm_mouse_move(impl_->vt, row, col, modsOf(mods));
    impl_->output.clear();
    vterm_mouse_button(impl_->vt, button, pressed, modsOf(mods));
    return std::exchange(impl_->output, {});
}

} // namespace ke::modules
