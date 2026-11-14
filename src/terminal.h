#ifndef TERMINAL_H
#define TERMINAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hw/keyboard.h"
#define TERM_COLS 53
#define TERM_ROWS 24
typedef struct {
    uint32_t ch;
    uint16_t fg, bg;
} term_cell_t;
typedef struct {
    term_cell_t cells[TERM_ROWS][TERM_COLS];
    term_cell_t primary[TERM_ROWS][TERM_COLS];
    int x, y, saved_x, saved_y, primary_x, primary_y;
    int top, bottom;
    uint16_t fg, bg;
    bool bold, inverse, cursor, wrap, alternate, app_cursor, dirty;
    bool autowrap, origin;
    unsigned parser, count, params[16];
    bool private_csi;
    uint32_t unicode;
    unsigned utf_remaining;
    bool charset_pending;
} terminal_t;
typedef bool (*term_reply_fn)(const void *data, unsigned size);
void terminal_init(terminal_t *term);
void terminal_feed(terminal_t *term, const uint8_t *data, size_t size, term_reply_fn reply);
unsigned terminal_key(const terminal_t *term, key_event_t key, uint8_t out[16]);
#endif
