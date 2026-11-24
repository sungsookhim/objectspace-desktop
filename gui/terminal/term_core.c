/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Objectspace Terminal's core (term_core.h). */
#define _GNU_SOURCE
#include "term_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct sbline {
    int cols;
    VTermScreenCell *cells;
};

struct term_core {
    VTerm *vt;
    VTermScreen *scr;
    VTermState *state;
    int rows, cols;
    term_out_fn out;
    void *user;
    /* the scrollback: a ring, newest at (head + n - 1) % cap */
    struct sbline *sb;
    size_t cap, n, head;
    VTermPos cursor;
    bool cursor_visible, altscreen, bell, changed;
    int mouse;
    char title[256];
    char title_build[256];   /* (the title arrives in pieces) */
};

/* ------------------------------------------------------------ callbacks */

static int cb_damage(VTermRect r, void *user) { (void)r; ((term_core *)user)->changed = true; return 1; }
static int cb_moverect(VTermRect d, VTermRect s, void *user) { (void)d; (void)s; ((term_core *)user)->changed = true; return 1; }

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    (void)old; (void)visible;
    term_core *t = user;
    t->cursor = pos;
    t->changed = true;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    term_core *t = user;
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: t->cursor_visible = val->boolean; break;
    case VTERM_PROP_ALTSCREEN: t->altscreen = val->boolean; break;
    case VTERM_PROP_MOUSE: t->mouse = val->number; break;
    case VTERM_PROP_TITLE: {
        const VTermStringFragment *f = &val->string;
        if (f->initial) t->title_build[0] = 0;
        size_t have = strlen(t->title_build);
        if (have + f->len < sizeof t->title_build) {
            memcpy(t->title_build + have, f->str, f->len);
            t->title_build[have + f->len] = 0;
        }
        if (f->final) snprintf(t->title, sizeof t->title, "%s", t->title_build);
        break;
    }
    default: return 0;
    }
    t->changed = true;
    return 1;
}

static int cb_bell(void *user) { ((term_core *)user)->bell = true; return 1; }

static int cb_sb_push(int cols, const VTermScreenCell *cells, void *user)
{
    term_core *t = user;
    if (!t->cap) return 0;
    struct sbline *l;
    if (t->n < t->cap) {
        l = &t->sb[(t->head + t->n) % t->cap];
        t->n++;
    } else {   /* full: the oldest goes */
        l = &t->sb[t->head];
        t->head = (t->head + 1) % t->cap;
        free(l->cells);
    }
    l->cells = malloc((size_t)cols * sizeof *cells);
    l->cols = l->cells ? cols : 0;
    if (l->cells) memcpy(l->cells, cells, (size_t)cols * sizeof *cells);
    return 1;
}

static int cb_sb_pop(int cols, VTermScreenCell *cells, void *user)
{
    term_core *t = user;
    if (!t->n) return 0;
    struct sbline *l = &t->sb[(t->head + t->n - 1) % t->cap];
    for (int c = 0; c < cols; c++) {
        if (c < l->cols) cells[c] = l->cells[c];
        else { memset(&cells[c], 0, sizeof cells[c]); cells[c].width = 1; cells[c].fg = l->cols ? l->cells[0].fg : cells[c].fg; }
    }
    free(l->cells);
    l->cells = NULL;
    t->n--;
    return 1;
}

static const VTermScreenCallbacks callbacks = {
    .damage = cb_damage, .moverect = cb_moverect, .movecursor = cb_movecursor, .settermprop = cb_settermprop,
    .bell = cb_bell, .sb_pushline = cb_sb_push, .sb_popline = cb_sb_pop,
};

static void output(const char *s, size_t len, void *user)
{
    term_core *t = user;
    if (t->out) t->out(s, len, t->user);
}

/* ------------------------------------------------------------- lifetime */

term_core *term_core_new(int rows, int cols, size_t scrollback, term_out_fn out, void *user)
{
    term_core *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->rows = rows;
    t->cols = cols;
    t->out = out;
    t->user = user;
    t->cap = scrollback;
    t->sb = scrollback ? calloc(scrollback, sizeof *t->sb) : NULL;
    t->cursor_visible = true;
    t->vt = vterm_new(rows, cols);
    vterm_set_utf8(t->vt, 1);
    vterm_output_set_callback(t->vt, output, t);
    t->scr = vterm_obtain_screen(t->vt);
    t->state = vterm_obtain_state(t->vt);
    vterm_screen_set_callbacks(t->scr, &callbacks, t);
    vterm_screen_enable_altscreen(t->scr, 1);
    vterm_screen_enable_reflow(t->scr, true);
    vterm_screen_set_damage_merge(t->scr, VTERM_DAMAGE_SCROLL);
    vterm_screen_reset(t->scr, 1);
    return t;
}

void term_core_free(term_core *t)
{
    if (!t) return;
    for (size_t i = 0; i < t->cap; i++) free(t->sb[i].cells);
    free(t->sb);
    vterm_free(t->vt);
    free(t);
}

bool term_core_input(term_core *t, const char *bytes, size_t n)
{
    t->changed = false;
    vterm_input_write(t->vt, bytes, n);
    vterm_screen_flush_damage(t->scr);
    return t->changed;
}

void term_core_resize(term_core *t, int rows, int cols)
{
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows == t->rows && cols == t->cols) return;
    t->rows = rows;
    t->cols = cols;
    vterm_set_size(t->vt, rows, cols);
    vterm_screen_flush_damage(t->scr);
}

int term_core_rows(const term_core *t) { return t->rows; }
int term_core_cols(const term_core *t) { return t->cols; }
size_t term_core_scrollback(const term_core *t) { return t->n; }

/* ----------------------------------------------------------------- keys */

static VTermModifier mods_of(unsigned m)
{
    return (VTermModifier)((m & UI_MOD_SHIFT ? VTERM_MOD_SHIFT : 0) | (m & UI_MOD_CTRL ? VTERM_MOD_CTRL : 0) |
                           (m & UI_MOD_ALT ? VTERM_MOD_ALT : 0));
}

static uint32_t utf8_first(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (!u[0]) return 0;
    if (u[0] < 0x80) return u[0];
    if (u[0] < 0xe0) return (uint32_t)(u[0] & 0x1f) << 6 | (u[1] & 0x3f);
    if (u[0] < 0xf0) return (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f);
    return (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3f) << 12 | (uint32_t)(u[2] & 0x3f) << 6 | (u[3] & 0x3f);
}

bool term_core_key(term_core *t, uint32_t sym, const char *utf8, unsigned mods)
{
    VTermModifier m = mods_of(mods);
    VTermKey k = VTERM_KEY_NONE;
    switch (sym) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: k = VTERM_KEY_ENTER; break;
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: k = VTERM_KEY_TAB; break;
    case XKB_KEY_BackSpace: k = VTERM_KEY_BACKSPACE; break;
    case XKB_KEY_Escape: k = VTERM_KEY_ESCAPE; break;
    case XKB_KEY_Up: case XKB_KEY_KP_Up: k = VTERM_KEY_UP; break;
    case XKB_KEY_Down: case XKB_KEY_KP_Down: k = VTERM_KEY_DOWN; break;
    case XKB_KEY_Left: case XKB_KEY_KP_Left: k = VTERM_KEY_LEFT; break;
    case XKB_KEY_Right: case XKB_KEY_KP_Right: k = VTERM_KEY_RIGHT; break;
    case XKB_KEY_Insert: case XKB_KEY_KP_Insert: k = VTERM_KEY_INS; break;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: k = VTERM_KEY_DEL; break;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: k = VTERM_KEY_HOME; break;
    case XKB_KEY_End: case XKB_KEY_KP_End: k = VTERM_KEY_END; break;
    case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: k = VTERM_KEY_PAGEUP; break;
    case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: k = VTERM_KEY_PAGEDOWN; break;
    default:
        if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F35) k = VTERM_KEY_FUNCTION((int)(sym - XKB_KEY_F1 + 1));
        break;
    }
    if (sym == XKB_KEY_ISO_Left_Tab) m |= VTERM_MOD_SHIFT;
    if (k != VTERM_KEY_NONE) { vterm_keyboard_key(t->vt, k, m); return true; }
    /* a character: the key's own (Ctrl+C is "c" with Ctrl, which libvterm
     * makes ^C), else the text (an input method's, sym 0) */
    uint32_t c = sym ? xkb_keysym_to_utf32(sym) : 0;
    if (c && (m & (VTERM_MOD_CTRL | VTERM_MOD_ALT))) {
        if (c >= 'A' && c <= 'Z' && (m & VTERM_MOD_CTRL)) c += 'a' - 'A';
        vterm_keyboard_unichar(t->vt, c, (VTermModifier)(m & ~VTERM_MOD_SHIFT));
        return true;
    }
    if (utf8 && utf8[0] && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
        for (const char *p = utf8; *p;) {   /* (an input method's text: several characters) */
            uint32_t ch = utf8_first(p);
            vterm_keyboard_unichar(t->vt, ch, VTERM_MOD_NONE);
            unsigned char b = (unsigned char)*p;
            p += b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
        }
        return true;
    }
    if (c >= 0x20) { vterm_keyboard_unichar(t->vt, c, VTERM_MOD_NONE); return true; }
    return false;
}

void term_core_paste(term_core *t, const char *text)
{
    vterm_keyboard_start_paste(t->vt);
    for (const char *p = text; *p;) {
        uint32_t c = utf8_first(p);
        unsigned char b = (unsigned char)*p;
        p += b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
        if (c == '\n') vterm_keyboard_key(t->vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
        else if (c >= 0x20 || c == '\t') vterm_keyboard_unichar(t->vt, c, VTERM_MOD_NONE);
    }
    vterm_keyboard_end_paste(t->vt);
}

/* ---------------------------------------------------------------- cells */

static bool get_cell(term_core *t, int row, int col, VTermScreenCell *c)
{
    memset(c, 0, sizeof *c);
    if (row >= 0) {
        if (row >= t->rows || col >= t->cols) return false;
        return vterm_screen_get_cell(t->scr, (VTermPos){ row, col }, c) != 0;
    }
    if ((size_t)-row > t->n) return false;
    struct sbline *l = &t->sb[(t->head + t->n + (size_t)row) % t->cap];   /* -1: the newest */
    if (col >= l->cols) { c->width = 1; vterm_state_get_default_colors(t->state, &c->fg, &c->bg); return true; }
    *c = l->cells[col];
    return true;
}

static ui_color rgb(const term_core *t, VTermColor col)
{
    vterm_screen_convert_color_to_rgb(t->scr, &col);
    return (ui_color){ col.rgb.red / 255.0, col.rgb.green / 255.0, col.rgb.blue / 255.0, 1 };
}

static size_t put_utf8(char *o, uint32_t c)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xc0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xe0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 0x3f)); o[2] = (char)(0x80 | (c & 0x3f)); return 3; }
    o[0] = (char)(0xf0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3f)); o[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

void term_core_cell(term_core *t, int row, int col, ui_color fg_default, ui_color bg_default, term_cell *out)
{
    memset(out, 0, sizeof *out);
    VTermScreenCell c;
    out->width = 1;
    out->fg = fg_default;
    out->bg = bg_default;
    out->bg_default = true;
    if (!get_cell(t, row, col, &c)) return;
    out->width = c.width < 0 ? 1 : c.width;
    if (c.chars[0] == (uint32_t)-1) out->width = 0;   /* (the second half of a wide one) */
    size_t o = 0;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i] && c.chars[i] != (uint32_t)-1 && o + 5 < sizeof out->text; i++)
        o += put_utf8(out->text + o, c.chars[i]);
    out->text[o] = 0;
    bool fgd = VTERM_COLOR_IS_DEFAULT_FG(&c.fg), bgd = VTERM_COLOR_IS_DEFAULT_BG(&c.bg);
    ui_color fg = fgd ? fg_default : rgb(t, c.fg), bg = bgd ? bg_default : rgb(t, c.bg);
    out->bg_default = bgd;
    if (c.attrs.reverse) {
        ui_color x = fg; fg = bg; bg = x;
        out->bg_default = false;
    }
    out->fg = fg;
    out->bg = bg;
    out->bold = c.attrs.bold;
    out->italic = c.attrs.italic;
    out->underline = c.attrs.underline != 0;
    out->strike = c.attrs.strike;
}

void term_core_cursor(const term_core *t, int *row, int *col, bool *visible)
{
    *row = t->cursor.row;
    *col = t->cursor.col;
    *visible = t->cursor_visible;
}

const char *term_core_title(const term_core *t) { return t->title; }
bool term_core_mouse(const term_core *t) { return t->mouse != VTERM_PROP_MOUSE_NONE; }
bool term_core_altscreen(const term_core *t) { return t->altscreen; }

bool term_core_bell(term_core *t)
{
    bool b = t->bell;
    t->bell = false;
    return b;
}

void term_core_mouse_event(term_core *t, int row, int col, int button, bool pressed, bool move, unsigned mods)
{
    VTermModifier m = mods_of(mods);
    vterm_mouse_move(t->vt, row, col, m);
    if (!move) vterm_mouse_button(t->vt, button, pressed, m);
}

char *term_core_text(term_core *t, int r0, int c0, int r1, int c1)
{
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { int r = r0, c = c0; r0 = r1; c0 = c1; r1 = r; c1 = c; }
    size_t cap = 256, len = 0;
    char *s = malloc(cap);
    if (!s) return NULL;
    for (int r = r0; r <= r1; r++) {
        int from = r == r0 ? c0 : 0, to = r == r1 ? c1 : t->cols - 1;
        size_t line_start = len;
        for (int c = from; c <= to && c < t->cols; c++) {
            term_cell cell;
            term_core_cell(t, r, c, (ui_color){ 0 }, (ui_color){ 0 }, &cell);
            if (!cell.width) continue;
            const char *txt = cell.text[0] ? cell.text : " ";
            size_t l = strlen(txt);
            if (len + l + 2 > cap) { cap = (len + l + 2) * 2; char *n = realloc(s, cap); if (!n) { free(s); return NULL; } s = n; }
            memcpy(s + len, txt, l);
            len += l;
        }
        while (len > line_start && s[len - 1] == ' ') len--;   /* (a row's trailing blanks) */
        if (r < r1) { if (len + 2 > cap) { cap *= 2; char *n = realloc(s, cap); if (!n) { free(s); return NULL; } s = n; } s[len++] = '\n'; }
    }
    s[len] = 0;
    return s;
}

void term_shell_quote(const char *path, char *out, size_t n)
{
    bool plain = *path != 0;
    for (const char *p = path; *p && plain; p++)
        plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/._-+,:@%", *p);
    if (plain) { snprintf(out, n, "%s", path); return; }
    size_t o = 0;
    if (o + 1 < n) out[o++] = '\'';
    for (const char *p = path; *p && o + 5 < n; p++) {
        if (*p == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }   /* ' ends the quote, \' is one, ' starts again */
        else out[o++] = *p;
    }
    if (o + 1 < n) out[o++] = '\'';
    out[o < n ? o : n - 1] = 0;
}
