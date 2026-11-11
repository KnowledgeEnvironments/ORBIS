// The one interface to libghostty-vt. Its API is pre-1.0 (holder/PIN names the exact commit), so
// nothing else in the holder includes a ghostty header: an upgrade changes vt.c and nothing more.
#ifndef HOLDER_VT_H
#define HOLDER_VT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct vt vt_t;
typedef void (*vt_reply_fn)(void *user, const uint8_t *bytes, size_t len);

// A growing byte buffer; the screen and text encoders append to it.
typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf_t;
void buf_reserve(buf_t *b, size_t more);
void buf_bytes(buf_t *b, const void *p, size_t n);
void buf_u8(buf_t *b, uint8_t v);
void buf_u16(buf_t *b, uint16_t v);
void buf_u32(buf_t *b, uint32_t v);
void buf_u64(buf_t *b, uint64_t v);

vt_t *vt_new(uint16_t cols, uint16_t rows, size_t scrollback_lines, vt_reply_fn reply, void *user);
void vt_free(vt_t *t);
void vt_write(vt_t *t, const uint8_t *bytes, size_t len);
void vt_resize(vt_t *t, uint16_t cols, uint16_t rows);

// The screen as bytes (the SCREEN event in docs/panes.md): cursor, modes, title and the rows that
// changed since the last call, or all of them when full.
void vt_screen(vt_t *t, bool full, buf_t *out);
// Everything still held, scrollback then screen, as plain lines.
void vt_text(vt_t *t, buf_t *out);
#endif
