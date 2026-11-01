#include "vt.h"

#include <ghostty/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vt {
    GhosttyTerminal term;
    GhosttyRenderState state;
    GhosttyRenderStateRowIterator rows;
    GhosttyRenderStateRowCells cells;
    vt_reply_fn reply;
    void *user;
};

void buf_reserve(buf_t *b, size_t more) {
    if (b->len + more <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + more) cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) abort();
    b->p = p;
    b->cap = cap;
}
void buf_bytes(buf_t *b, const void *p, size_t n) {
    buf_reserve(b, n);
    if (n) memcpy(b->p + b->len, p, n);
    b->len += n;
}
void buf_u8(buf_t *b, uint8_t v) { buf_bytes(b, &v, 1); }
void buf_u16(buf_t *b, uint16_t v) {
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    buf_bytes(b, x, 2);
}
void buf_u32(buf_t *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    buf_bytes(b, x, 4);
}
void buf_u64(buf_t *b, uint64_t v) {
    buf_u32(b, (uint32_t)(v >> 32));
    buf_u32(b, (uint32_t)v);
}

static void on_write(GhosttyTerminal term, void *user, const uint8_t *data, size_t len) {
    (void)term;
    vt_t *t = user;
    t->reply(t->user, data, len);
}

// A program asking what it is talking to gets an honest xterm-ish answer.
static bool on_attrs(GhosttyTerminal term, void *user, GhosttyDeviceAttributes *out) {
    (void)term;
    (void)user;
    memset(out, 0, sizeof *out);
    out->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
    out->primary.features[0] = 22; // ANSI colour
    out->primary.num_features = 1;
    out->secondary.device_type = 1;
    out->secondary.firmware_version = 10;
    return true;
}

vt_t *vt_new(uint16_t cols, uint16_t rows, size_t scrollback_lines, vt_reply_fn reply, void *user) {
    vt_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->reply = reply;
    t->user = user;
    if (ghostty_terminal_new(NULL, &t->term, cols, rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_new(NULL, &t->state) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_iterator_new(NULL, &t->rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(NULL, &t->cells) != GHOSTTY_SUCCESS) {
        vt_free(t);
        return NULL;
    }
    ghostty_terminal_set(t->term, GHOSTTY_TERMINAL_OPT_USERDATA, t);
    ghostty_terminal_set(t->term, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *)on_write);
    ghostty_terminal_set(t->term, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES, (const void *)on_attrs);
    // by lines alone: the byte limit's default is far below what the line limit means
    ghostty_terminal_set(t->term, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, NULL);
    ghostty_terminal_set(t->term, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &scrollback_lines);
    return t;
}

void vt_free(vt_t *t) {
    if (!t) return;
    if (t->cells) ghostty_render_state_row_cells_free(t->cells);
    if (t->rows) ghostty_render_state_row_iterator_free(t->rows);
    if (t->state) ghostty_render_state_free(t->state);
    if (t->term) ghostty_terminal_free(t->term);
    free(t);
}

void vt_write(vt_t *t, const uint8_t *bytes, size_t len) { ghostty_terminal_vt_write(t->term, bytes, len); }

void vt_resize(vt_t *t, uint16_t cols, uint16_t rows) { ghostty_terminal_resize(t->term, cols, rows, 8, 16); }

static size_t put_color(char *s, size_t n, size_t at, GhosttyStyleColor c, int base) {
    if (c.tag == GHOSTTY_STYLE_COLOR_PALETTE) {
        at += (size_t)snprintf(s + at, n - at, "%s%d;5;%d", at ? ";" : "", base, c.value.palette);
    } else if (c.tag == GHOSTTY_STYLE_COLOR_RGB) {
        at += (size_t)snprintf(s + at, n - at, "%s%d;2;%d;%d;%d", at ? ";" : "", base, c.value.rgb.r,
                               c.value.rgb.g, c.value.rgb.b);
    }
    return at;
}

// A style as SGR parameters, the form the world's cells carry; empty for the default style.
static size_t sgr(const GhosttyStyle *st, char *s, size_t n) {
    size_t at = 0;
    const struct {
        bool on;
        int code;
    } flags[] = {{st->bold, 1},      {st->faint, 2},     {st->italic, 3},        {st->underline != 0, 4},
                 {st->blink, 5},     {st->inverse, 7},   {st->invisible, 8},     {st->strikethrough, 9},
                 {st->overline, 53}};
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        if (flags[i].on) at += (size_t)snprintf(s + at, n - at, "%s%d", at ? ";" : "", flags[i].code);
    }
    at = put_color(s, n, at, st->fg_color, 38);
    at = put_color(s, n, at, st->bg_color, 48);
    return at;
}

static void put_row(vt_t *t, uint16_t y, buf_t *out) {
    ghostty_render_state_row_get(t->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &t->cells);
    buf_u16(out, y);
    const size_t count_at = out->len;
    buf_u16(out, 0);
    uint16_t cells = 0;
    while (ghostty_render_state_row_cells_next(t->cells)) {
        GhosttyCell raw = 0;
        ghostty_render_state_row_cells_get(t->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
        uint32_t cps = 0;
        ghostty_render_state_row_cells_get(t->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &cps);
        GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
        ghostty_render_state_row_cells_get(t->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
        char attr[96];
        const size_t attr_len = sgr(&style, attr, sizeof attr);

        uint8_t text[64];
        size_t text_len = 0;
        if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
            text_len = 0; // the right half of a wide character is an empty cell
        } else if (cps == 0) {
            text[0] = ' ';
            text_len = 1;
        } else {
            size_t written = 0;
            uint32_t cp[16];
            const uint32_t n = cps < 16 ? cps : 16;
            ghostty_render_state_row_cells_get(t->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cp);
            for (uint32_t i = 0; i < n && written + 4 <= sizeof text; i++) {
                const uint32_t c = cp[i];
                if (c < 0x80) {
                    text[written++] = (uint8_t)c;
                } else if (c < 0x800) {
                    text[written++] = (uint8_t)(0xC0 | (c >> 6));
                    text[written++] = (uint8_t)(0x80 | (c & 0x3F));
                } else if (c < 0x10000) {
                    text[written++] = (uint8_t)(0xE0 | (c >> 12));
                    text[written++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
                    text[written++] = (uint8_t)(0x80 | (c & 0x3F));
                } else {
                    text[written++] = (uint8_t)(0xF0 | (c >> 18));
                    text[written++] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
                    text[written++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
                    text[written++] = (uint8_t)(0x80 | (c & 0x3F));
                }
            }
            text_len = written;
        }
        buf_u8(out, (uint8_t)text_len);
        buf_bytes(out, text, text_len);
        buf_u8(out, (uint8_t)attr_len);
        buf_bytes(out, attr, attr_len);
        cells++;
    }
    out->p[count_at] = (uint8_t)(cells >> 8);
    out->p[count_at + 1] = (uint8_t)cells;
}

static bool mode_on(vt_t *t, GhosttyMode mode) {
    GhosttyTerminalModeConfig m = {.mode = mode, .value = false};
    ghostty_terminal_get(t->term, GHOSTTY_TERMINAL_DATA_MODE, &m);
    return m.value;
}

void vt_screen(vt_t *t, bool full, buf_t *out) {
    ghostty_render_state_update(t->state, t->term);
    GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_get(t->state, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
    if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL) full = true;

    uint16_t cols = 0, rows = 0;
    ghostty_terminal_get(t->term, GHOSTTY_TERMINAL_DATA_COLS, &cols);
    ghostty_terminal_get(t->term, GHOSTTY_TERMINAL_DATA_ROWS, &rows);
    GhosttyRenderStateCursor cur = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(t->state, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cur);
    uint16_t cx = 0, cy = 0;
    ghostty_terminal_get(t->term, GHOSTTY_TERMINAL_DATA_CURSOR_X, &cx);
    ghostty_terminal_get(t->term, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &cy);
    GhosttyTerminalScreen active = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    ghostty_terminal_get(t->term, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &active);
    size_t scrollback = 0;
    ghostty_terminal_get(t->term, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &scrollback);
    GhosttyString title = {0};
    ghostty_terminal_get(t->term, GHOSTTY_TERMINAL_DATA_TITLE, &title);

    int shape = 0; // DECSCUSR: 1 blinking block, 2 block, 3/4 underline, 5/6 bar
    switch (cur.visual_style) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR: shape = cur.blinking ? 5 : 6; break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE: shape = cur.blinking ? 3 : 4; break;
    default: shape = cur.blinking ? 1 : 2; break;
    }

    buf_u16(out, cols);
    buf_u16(out, rows);
    buf_u16(out, cx);
    buf_u16(out, cy);
    uint8_t flags = 0;
    if (cur.visible) flags |= 1;
    if (active == GHOSTTY_TERMINAL_SCREEN_ALTERNATE) flags |= 2;
    if (mode_on(t, GHOSTTY_MODE_KEYPAD_KEYS)) flags |= 4;
    if (full) flags |= 8;
    buf_u8(out, flags);
    buf_u8(out, (uint8_t)shape);
    buf_u32(out, (uint32_t)scrollback);
    buf_u16(out, title.ptr ? (uint16_t)(title.len > 4096 ? 4096 : title.len) : 0);
    if (title.ptr) buf_bytes(out, title.ptr, title.len > 4096 ? 4096 : title.len);

    // The private modes a client must know: docs/panes.md, and the world's passed-modes.
    const struct {
        GhosttyMode mode;
        uint16_t number;
    } passed[] = {{GHOSTTY_MODE_DECCKM, 1},        {GHOSTTY_MODE_NORMAL_MOUSE, 1000}, {GHOSTTY_MODE_BUTTON_MOUSE, 1002},
                  {GHOSTTY_MODE_ANY_MOUSE, 1003},  {GHOSTTY_MODE_FOCUS_EVENT, 1004},  {GHOSTTY_MODE_UTF8_MOUSE, 1005},
                  {GHOSTTY_MODE_SGR_MOUSE, 1006},  {GHOSTTY_MODE_URXVT_MOUSE, 1015},  {GHOSTTY_MODE_BRACKETED_PASTE, 2004}};
    uint8_t on = 0;
    const size_t count_at = out->len;
    buf_u8(out, 0);
    for (size_t i = 0; i < sizeof passed / sizeof passed[0]; i++) {
        if (mode_on(t, passed[i].mode)) {
            buf_u16(out, passed[i].number);
            on++;
        }
    }
    out->p[count_at] = on;

    ghostty_render_state_get(t->state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &t->rows);
    const size_t rows_at = out->len;
    buf_u16(out, 0);
    uint16_t sent = 0;
    if (full) {
        uint16_t y = 0;
        while (ghostty_render_state_row_iterator_next(t->rows)) {
            put_row(t, y++, out);
            sent++;
        }
    } else {
        uint16_t y = 0;
        while (ghostty_render_state_row_iterator_next_dirty(t->rows, &y)) {
            put_row(t, y, out);
            sent++;
        }
    }
    out->p[rows_at] = (uint8_t)(sent >> 8);
    out->p[rows_at + 1] = (uint8_t)sent;
    ghostty_render_state_clean(t->state);
}

void vt_text(vt_t *t, buf_t *out) {
    GhosttyFormatterTerminalOptions opt = GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
    opt.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    opt.trim = true;
    opt.extra.size = sizeof opt.extra;
    opt.extra.screen.size = sizeof opt.extra.screen;
    GhosttyFormatter f = NULL;
    if (ghostty_formatter_terminal_new(NULL, &f, t->term, opt) != GHOSTTY_SUCCESS) return;
    uint8_t *p = NULL;
    size_t len = 0;
    if (ghostty_formatter_format_alloc(f, NULL, &p, &len) == GHOSTTY_SUCCESS) {
        buf_bytes(out, p, len);
        ghostty_free(NULL, p, len);
    }
    ghostty_formatter_free(f);
}
