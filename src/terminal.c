#include "terminal.h"
#include <stdio.h>
#include <string.h>

static const uint16_t colors[16] = {
    0x0000, 0xa800, 0x0540, 0xad40, 0x0015, 0xa815, 0x0555, 0xad55,
    0x52aa, 0xfaaa, 0x57ea, 0xffea, 0x52bf, 0xfabf, 0x57ff, 0xffff
};
static int clamp(int n, int lo, int hi) { return n < lo ? lo : n > hi ? hi : n; }
static void blank(terminal_t *t, term_cell_t *p, unsigned n) {
    while (n--) *p++ = (term_cell_t){' ', t->fg, t->bg};
}
void terminal_init(terminal_t *t) {
    memset(t, 0, sizeof(*t));
    t->fg = colors[7];
    t->bg = colors[0];
    t->bottom = TERM_ROWS - 1;
    t->cursor = t->autowrap = t->dirty = true;
    blank(t, &t->cells[0][0], TERM_ROWS * TERM_COLS);
}
static void scroll(terminal_t *t, int amount) {
    int height = t->bottom - t->top + 1;
    int n = clamp(amount < 0 ? -amount : amount, 1, height);
    if (amount > 0) {
        memmove(t->cells[t->top], t->cells[t->top + n],
                (size_t)(height - n) * sizeof(t->cells[0]));
        blank(t, t->cells[t->bottom - n + 1], (unsigned)n * TERM_COLS);
    } else {
        memmove(t->cells[t->top + n], t->cells[t->top],
                (size_t)(height - n) * sizeof(t->cells[0]));
        blank(t, t->cells[t->top], (unsigned)n * TERM_COLS);
    }
}
static void linefeed(terminal_t *t) {
    t->wrap = false;
    if (t->y == t->bottom) scroll(t, 1);
    else if (t->y < TERM_ROWS - 1) t->y++;
}
static void put(terminal_t *t, uint32_t c) {
    if (t->wrap && t->autowrap) { t->x = 0; linefeed(t); }
    uint16_t fg = t->fg, bg = t->bg;
    if (t->bold)
        for (int i = 0; i < 8; i++) if (fg == colors[i]) { fg = colors[i + 8]; break; }
    t->cells[t->y][t->x] = (term_cell_t){c, t->inverse ? bg : fg, t->inverse ? fg : bg};
    t->wrap = t->x == TERM_COLS - 1;
    if (!t->wrap) t->x++;
}
static uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 248) << 8) | ((g & 252) << 3) | (b >> 3));
}
static uint16_t indexed(unsigned n) {
    if (n < 16) return colors[n];
    if (n < 232) {
        n -= 16;
        unsigned r = n / 36, g = (n / 6) % 6, b = n % 6;
        return rgb(r ? 55 + r * 40 : 0, g ? 55 + g * 40 : 0, b ? 55 + b * 40 : 0);
    }
    unsigned v = 8 + (n - 232) * 10;
    return rgb(v, v, v);
}
static unsigned param(const terminal_t *t, unsigned i, unsigned fallback) {
    return i > t->count || !t->params[i] ? fallback : t->params[i];
}
static void sgr(terminal_t *t) {
    for (unsigned i = 0; i <= t->count; i++) {
        unsigned p = t->params[i];
        if (p == 0) { t->fg = colors[7]; t->bg = colors[0]; t->bold = t->inverse = false; }
        else if (p == 1) t->bold = true;
        else if (p == 22) t->bold = false;
        else if (p == 7) t->inverse = true;
        else if (p == 27) t->inverse = false;
        else if (p >= 30 && p <= 37) t->fg = colors[p - 30];
        else if (p >= 40 && p <= 47) t->bg = colors[p - 40];
        else if (p >= 90 && p <= 97) t->fg = colors[p - 90 + 8];
        else if (p >= 100 && p <= 107) t->bg = colors[p - 100 + 8];
        else if (p == 39) t->fg = colors[7];
        else if (p == 49) t->bg = colors[0];
        else if ((p == 38 || p == 48) && i + 2 <= t->count) {
            uint16_t color;
            if (t->params[i + 1] == 5) { color = indexed(t->params[i + 2] & 255); i += 2; }
            else if (t->params[i + 1] == 2 && i + 4 <= t->count) {
                color = rgb(t->params[i + 2] & 255, t->params[i + 3] & 255, t->params[i + 4] & 255);
                i += 4;
            } else continue;
            if (p == 38) t->fg = color; else t->bg = color;
        }
    }
}
static void mode(terminal_t *t, bool on) {
    if (!t->private_csi) return;
    for (unsigned i = 0; i <= t->count; i++) {
        switch (t->params[i]) {
        case 1: t->app_cursor = on; break;
        case 6: t->origin = on; t->x = 0; t->y = on ? t->top : 0; break;
        case 7: t->autowrap = on; break;
        case 25: t->cursor = on; break;
        case 1049:
            if (on && !t->alternate) {
                memcpy(t->primary, t->cells, sizeof(t->cells));
                t->primary_x = t->x; t->primary_y = t->y;
                blank(t, &t->cells[0][0], TERM_ROWS * TERM_COLS);
                t->x = t->y = 0;
            } else if (!on && t->alternate) {
                memcpy(t->cells, t->primary, sizeof(t->cells));
                t->x = t->primary_x; t->y = t->primary_y;
            }
            t->alternate = on;
            t->wrap = false;
            break;
        default: break;
        }
    }
}
static void csi(terminal_t *t, uint8_t command, term_reply_fn reply) {
    int n = (int)param(t, 0, 1);
    int lo = t->origin ? t->top : 0;
    int hi = t->origin ? t->bottom : TERM_ROWS - 1;
    switch (command) {
    case 'A': t->y = clamp(t->y - n, lo, hi); break;
    case 'B': case 'e': t->y = clamp(t->y + n, lo, hi); break;
    case 'C': case 'a': t->x = clamp(t->x + n, 0, TERM_COLS - 1); break;
    case 'D': t->x = clamp(t->x - n, 0, TERM_COLS - 1); break;
    case 'E': t->y = clamp(t->y + n, lo, hi); t->x = 0; break;
    case 'F': t->y = clamp(t->y - n, lo, hi); t->x = 0; break;
    case 'G': case '`': t->x = clamp(n - 1, 0, TERM_COLS - 1); break;
    case 'd': t->y = clamp(lo + n - 1, lo, hi); break;
    case 'H': case 'f':
        t->y = clamp(lo + n - 1, lo, hi);
        t->x = clamp((int)param(t, 1, 1) - 1, 0, TERM_COLS - 1);
        break;
    case 'J': {
        unsigned pos = (unsigned)(t->y * TERM_COLS + t->x);
        unsigned p = t->params[0];
        if (p == 0) blank(t, &t->cells[0][0] + pos, TERM_ROWS * TERM_COLS - pos);
        else if (p == 1) blank(t, &t->cells[0][0], pos + 1);
        else if (p == 2 || p == 3) blank(t, &t->cells[0][0], TERM_ROWS * TERM_COLS);
        break;
    }
    case 'K':
        if (t->params[0] == 0) blank(t, &t->cells[t->y][t->x], TERM_COLS - (unsigned)t->x);
        else if (t->params[0] == 1) blank(t, t->cells[t->y], (unsigned)t->x + 1);
        else if (t->params[0] == 2) blank(t, t->cells[t->y], TERM_COLS);
        break;
    case 'm': sgr(t); break;
    case 'h': mode(t, true); break;
    case 'l': mode(t, false); break;
    case 's': t->saved_x = t->x; t->saved_y = t->y; break;
    case 'u': t->x = t->saved_x; t->y = t->saved_y; break;
    case 'r':
        lo = clamp(n - 1, 0, TERM_ROWS - 1);
        hi = clamp((int)param(t, 1, TERM_ROWS) - 1, 0, TERM_ROWS - 1);
        if (lo < hi) { t->top = lo; t->bottom = hi; t->x = 0; t->y = t->origin ? lo : 0; }
        break;
    case 'S': scroll(t, n); break;
    case 'T': scroll(t, -n); break;
    case 'L': case 'M':
        if (t->y >= t->top && t->y <= t->bottom) {
            int old = t->top;
            t->top = t->y;
            scroll(t, command == 'L' ? -n : n);
            t->top = old;
        }
        break;
    case 'P': case '@': case 'X': {
        n = clamp(n, 1, TERM_COLS - t->x);
        term_cell_t *p = &t->cells[t->y][t->x];
        size_t bytes = (size_t)(TERM_COLS - t->x - n) * sizeof(*p);
        if (command == 'P') { memmove(p, p + n, bytes); blank(t, &t->cells[t->y][TERM_COLS - n], n); }
        else if (command == '@') { memmove(p + n, p, bytes); blank(t, p, n); }
        else blank(t, p, n);
        break;
    }
    case 'n':
        if (reply && !t->private_csi) {
            char response[32];
            if (t->params[0] == 6) {
                int size = snprintf(response, sizeof(response), "\033[%d;%dR", t->y + 1, t->x + 1);
                reply(response, (unsigned)size);
            } else if (t->params[0] == 5) reply("\033[0n", 4);
        }
        break;
    case 'c': if (reply) reply("\033[?1;2c", 7); break;
    default: break;
    }
    if (command != 'm') t->wrap = false;
}

void terminal_feed(terminal_t *t, const uint8_t *data, size_t size, term_reply_fn reply) {
    t->dirty = true;
    while (size--) {
        uint8_t c = *data++;
        if (t->parser == 3) { /* OSC: discard titles, including split ST terminators. */
            if (c == 7) t->parser = 0;
            else if (c == 27) t->parser = 4;
            continue;
        }
        if (t->parser == 4) { t->parser = c == '\\' ? 0 : 3; continue; }
        if (t->parser == 1) {
            t->parser = 0;
            if (c == '[') {
                t->parser = 2; t->count = 0; t->private_csi = false;
                memset(t->params, 0, sizeof(t->params));
            } else if (c == ']') t->parser = 3;
            else if (c == '(' || c == ')') t->charset_pending = true;
            else if (c == '7') { t->saved_x = t->x; t->saved_y = t->y; }
            else if (c == '8') { t->x = t->saved_x; t->y = t->saved_y; t->wrap = false; }
            else if (c == 'D') linefeed(t);
            else if (c == 'E') { t->x = 0; linefeed(t); }
            else if (c == 'M') {
                if (t->y == t->top) scroll(t, -1);
                else if (t->y > 0) t->y--;
            } else if (c == 'c') terminal_init(t);
            continue;
        }
        if (t->parser == 2) {
            if (c == '?') t->private_csi = true;
            else if (c >= '0' && c <= '9')
                t->params[t->count] = (unsigned)clamp((int)t->params[t->count] * 10 + c - '0', 0, 9999);
            else if (c == ';') { if (t->count < 15) t->count++; }
            else if (c >= 0x40 && c <= 0x7e) { csi(t, c, reply); t->parser = 0; }
            else if (c == 27) t->parser = 1;
            continue;
        }
        if (t->charset_pending) { t->charset_pending = false; continue; }
        if (c == 27) { t->parser = 1; t->utf_remaining = 0; continue; }
        if (c == '\r') { t->x = 0; t->wrap = false; continue; }
        if (c == '\n' || c == 11 || c == 12) { linefeed(t); continue; }
        if (c == '\b') { if (t->x) t->x--; t->wrap = false; continue; }
        if (c == '\t') { t->x = clamp((t->x / 8 + 1) * 8, 0, TERM_COLS - 1); t->wrap = false; continue; }
        if (c < 32 || c == 127) continue;
        if (t->utf_remaining) {
            if ((c & 0xc0) == 0x80) {
                t->unicode = (t->unicode << 6) | (c & 63);
                if (!--t->utf_remaining) put(t, t->unicode);
                continue;
            }
            t->utf_remaining = 0;
            put(t, '?');
        }
        if (c < 128) put(t, c);
        else if ((c & 0xe0) == 0xc0) { t->unicode = c & 31; t->utf_remaining = 1; }
        else if ((c & 0xf0) == 0xe0) { t->unicode = c & 15; t->utf_remaining = 2; }
        else if ((c & 0xf8) == 0xf0) { t->unicode = c & 7; t->utf_remaining = 3; }
        else put(t, '?');
    }
}

unsigned terminal_key(const terminal_t *t, key_event_t ev, uint8_t out[16]) {
    const char *seq = NULL;
    unsigned n = 0;
    switch (ev.key) {
    case KEY_ENTER: seq = "\r"; break;
    case KEY_BACKSPACE: seq = "\177"; break;
    case KEY_TAB: seq = ev.mods & MOD_SHIFT ? "\033[Z" : "\t"; break;
    case KEY_ESC: seq = "\033"; break;
    case KEY_UP: seq = t->app_cursor ? "\033OA" : "\033[A"; break;
    case KEY_DOWN: seq = t->app_cursor ? "\033OB" : "\033[B"; break;
    case KEY_RIGHT: seq = t->app_cursor ? "\033OC" : "\033[C"; break;
    case KEY_LEFT: seq = t->app_cursor ? "\033OD" : "\033[D"; break;
    case KEY_HOME: seq = "\033[H"; break;
    case KEY_END: seq = "\033[F"; break;
    case KEY_INSERT: seq = "\033[2~"; break;
    case KEY_DEL: seq = "\033[3~"; break;
    case KEY_PAGE_UP: seq = "\033[5~"; break;
    case KEY_PAGE_DOWN: seq = "\033[6~"; break;
    case KEY_F1: seq = "\033OP"; break;
    case KEY_F2: seq = "\033OQ"; break;
    case KEY_F3: seq = "\033OR"; break;
    case KEY_F4: seq = "\033OS"; break;
    case KEY_F5: seq = "\033[15~"; break;
    case KEY_F6: seq = "\033[17~"; break;
    case KEY_F7: seq = "\033[18~"; break;
    case KEY_F8: seq = "\033[19~"; break;
    case KEY_F9: seq = "\033[20~"; break;
    case KEY_F10: seq = "\033[21~"; break;
    default:
        if (ev.key >= 32 && ev.key < 127) {
            uint8_t c = ev.key;
            if (ev.mods & MOD_CTRL) c &= 31;
            if (ev.mods & MOD_ALT) out[n++] = 27;
            out[n++] = c;
        }
        return n;
    }
    n = (unsigned)strlen(seq);
    memcpy(out, seq, n);
    return n;
}
