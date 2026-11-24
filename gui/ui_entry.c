/* A one-line text entry: a plain object that draws into a rectangle and
 * handles keys and pointer events, so it works on a notebook page or
 * inside a container row (in-place rename). */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ui.h"

#define PAD 4

struct ui_entry {
    char *s;
    size_t len, cap;
    size_t cursor, anchor;    /* byte offsets; the selection is between them */
    double scroll;            /* horizontal, so the caret stays in view */
    bool dragging;
    char *preedit;            /* being composed by an input method: drawn at the cursor */
    int pre_cursor;           /* bytes into it */
    ui_font font;             /* the Dialogs font unless set */
};

void (*ui_entry_free_hook)(ui_entry *e);

static char *clipboard;      /* ours (also offered to every program: ui_clipboard_set) */

static void reserve(ui_entry *e, size_t n)
{
    if (n + 1 <= e->cap) return;
    while (e->cap < n + 1) e->cap = e->cap ? e->cap * 2 : 64;
    e->s = realloc(e->s, e->cap);
}

ui_entry *ui_entry_new(const char *text)
{
    ui_entry *e = calloc(1, sizeof *e);
    e->font = UI_FONT_DIALOG;
    ui_entry_set_text(e, text ? text : "");
    return e;
}

void ui_entry_set_font(ui_entry *e, ui_font f) { e->font = f; }

void ui_entry_free(ui_entry *e)
{
    if (!e) return;
    if (ui_entry_free_hook) ui_entry_free_hook(e);
    free(e->preedit);
    free(e->s);
    free(e);
}

const char *ui_entry_text(ui_entry *e) { return e->s; }
size_t ui_entry_cursor(ui_entry *e) { return e->cursor; }

void ui_entry_set_preedit(ui_entry *e, const char *text, int cursor)
{
    free(e->preedit);
    e->preedit = text && *text ? strdup(text) : NULL;
    size_t n = e->preedit ? strlen(e->preedit) : 0;
    e->pre_cursor = cursor < 0 ? (int)n : cursor > (int)n ? (int)n : cursor;
}

const char *ui_entry_preedit(ui_entry *e) { return e->preedit; }

void ui_entry_set_text(ui_entry *e, const char *t)
{
    size_t n = strlen(t);
    reserve(e, n);
    memcpy(e->s, t, n + 1);
    e->len = n;
    e->cursor = e->anchor = n;
    e->scroll = 0;
}

void ui_entry_select_all(ui_entry *e)
{
    e->anchor = 0;
    e->cursor = e->len;
}

/* Select up to the last dot (the name without its extension), as a rename
 * usually wants. */
void ui_entry_select_stem(ui_entry *e)
{
    char *dot = strrchr(e->s, '.');
    e->anchor = 0;
    e->cursor = dot && dot != e->s ? (size_t)(dot - e->s) : e->len;
}

static size_t sel_lo(ui_entry *e) { return e->cursor < e->anchor ? e->cursor : e->anchor; }
static size_t sel_hi(ui_entry *e) { return e->cursor < e->anchor ? e->anchor : e->cursor; }

/* UTF-8 boundaries. */
static size_t next_char(ui_entry *e, size_t i)
{
    if (i >= e->len) return e->len;
    i++;
    while (i < e->len && ((unsigned char)e->s[i] & 0xc0) == 0x80) i++;
    return i;
}

static size_t prev_char(ui_entry *e, size_t i)
{
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)e->s[i] & 0xc0) == 0x80) i--;
    return i;
}

static bool word_byte(char c) { return isalnum((unsigned char)c) || (unsigned char)c >= 0x80 || c == '_'; }

static size_t next_word(ui_entry *e, size_t i)
{
    while (i < e->len && !word_byte(e->s[i])) i++;
    while (i < e->len && word_byte(e->s[i])) i++;
    return i;
}

static size_t prev_word(ui_entry *e, size_t i)
{
    while (i > 0 && !word_byte(e->s[i - 1])) i--;
    while (i > 0 && word_byte(e->s[i - 1])) i--;
    return i;
}

static void delete_range(ui_entry *e, size_t a, size_t b)
{
    if (b <= a) return;
    memmove(e->s + a, e->s + b, e->len - b + 1);
    e->len -= b - a;
    e->cursor = e->anchor = a;
}

static void insert(ui_entry *e, const char *t)
{
    delete_range(e, sel_lo(e), sel_hi(e));
    size_t n = strlen(t);
    /* One line: control characters (a pasted newline) are dropped. */
    char *clean = malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)t[i] >= 0x20 && t[i] != 0x7f) clean[k++] = t[i];
    clean[k] = 0;
    reserve(e, e->len + k);
    memmove(e->s + e->cursor + k, e->s + e->cursor, e->len - e->cursor + 1);
    memcpy(e->s + e->cursor, clean, k);
    e->len += k;
    e->cursor = e->anchor = e->cursor + k;
    free(clean);
}

/* Where there is no data device (the compositor's lock screen, the unit
 * tests): our own clipboard only. ui_dnd.c's replace these. */
__attribute__((weak)) void ui_clipboard_set(const char *text) { (void)text; }
__attribute__((weak)) const char *ui_clipboard_text(void) { return NULL; }

static void copy_selection(ui_entry *e)
{
    size_t a = sel_lo(e), b = sel_hi(e);
    if (a == b) return;
    free(clipboard);
    clipboard = strndup(e->s + a, b - a);
    ui_clipboard_set(clipboard);   /* every program's clipboard too */
}

/* What a paste puts in: the system's clipboard (another program's copy, or
 * ours), else ours. */
static const char *paste_text(void)
{
    const char *t = ui_clipboard_text();
    return t ? t : clipboard;
}

ui_entry_result ui_entry_key(ui_entry *e, const ui_key_event *k)
{
    bool shift = k->mods & UI_MOD_SHIFT, ctrl = k->mods & UI_MOD_CTRL;
    size_t to = e->cursor;
    switch (k->sym) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return UI_ENTRY_COMMIT;
    case XKB_KEY_Escape: return UI_ENTRY_CANCEL;
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return UI_ENTRY_NONE;   /* the owner moves focus */
    case XKB_KEY_Left:
        if (!shift && e->cursor != e->anchor) to = sel_lo(e);
        else to = ctrl ? prev_word(e, e->cursor) : prev_char(e, e->cursor);
        break;
    case XKB_KEY_Right:
        if (!shift && e->cursor != e->anchor) to = sel_hi(e);
        else to = ctrl ? next_word(e, e->cursor) : next_char(e, e->cursor);
        break;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: to = 0; break;
    case XKB_KEY_End: case XKB_KEY_KP_End: to = e->len; break;
    case XKB_KEY_BackSpace:
        if (e->cursor != e->anchor) delete_range(e, sel_lo(e), sel_hi(e));
        else delete_range(e, ctrl ? prev_word(e, e->cursor) : prev_char(e, e->cursor), e->cursor);
        return UI_ENTRY_CHANGED;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
        if (e->cursor != e->anchor) delete_range(e, sel_lo(e), sel_hi(e));
        else delete_range(e, e->cursor, ctrl ? next_word(e, e->cursor) : next_char(e, e->cursor));
        return UI_ENTRY_CHANGED;
    default:
        if (ctrl) {
            switch (k->sym) {
            case XKB_KEY_a: case XKB_KEY_A: ui_entry_select_all(e); return UI_ENTRY_REDRAW;
            case XKB_KEY_c: case XKB_KEY_C: case XKB_KEY_Insert: copy_selection(e); return UI_ENTRY_NONE;
            case XKB_KEY_x: case XKB_KEY_X:
                copy_selection(e);
                delete_range(e, sel_lo(e), sel_hi(e));
                return UI_ENTRY_CHANGED;
            case XKB_KEY_v: case XKB_KEY_V:
                if (paste_text()) { insert(e, paste_text()); return UI_ENTRY_CHANGED; }
                return UI_ENTRY_NONE;
            default: return UI_ENTRY_NONE;
            }
        }
        if (shift && k->sym == XKB_KEY_Insert) {   /* Shift+Insert pastes, as in OS/2 */
            if (paste_text()) { insert(e, paste_text()); return UI_ENTRY_CHANGED; }
            return UI_ENTRY_NONE;
        }
        if ((unsigned char)k->utf8[0] >= 0x20 && k->utf8[0] != 0x7f && !(k->mods & UI_MOD_ALT)) {
            insert(e, k->utf8);
            return UI_ENTRY_CHANGED;
        }
        return UI_ENTRY_NONE;
    }
    e->cursor = to;
    if (!shift) e->anchor = to;
    return UI_ENTRY_REDRAW;
}

static size_t hit(ui_entry *e, ui_rect r, double x)
{
    return ui_text_hit(e->font, e->s, x - r.x - PAD + e->scroll);
}

ui_entry_result ui_entry_pointer(ui_entry *e, const ui_pointer_event *p, ui_rect r)
{
    if (p->kind == UI_PRESS && p->button == UI_BTN1 && ui_rect_has(r, p->x, p->y)) {
        size_t i = hit(e, r, p->x);
        if (p->clicks == 2) {   /* a word */
            size_t a = i, b = i;
            while (a > 0 && word_byte(e->s[a - 1])) a--;
            while (b < e->len && word_byte(e->s[b])) b++;
            e->anchor = a;
            e->cursor = b;
        } else {
            e->cursor = i;
            if (!(p->mods & UI_MOD_SHIFT)) e->anchor = i;
            e->dragging = true;
        }
        return UI_ENTRY_REDRAW;
    }
    if (p->kind == UI_MOTION && e->dragging) {
        size_t i = hit(e, r, p->x);
        if (i == e->cursor) return UI_ENTRY_NONE;
        e->cursor = i;
        return UI_ENTRY_REDRAW;
    }
    if (p->kind == UI_RELEASE && e->dragging) {
        e->dragging = false;
        return UI_ENTRY_NONE;
    }
    return UI_ENTRY_NONE;
}

static void draw_preedit(ui_entry *e, cairo_t *cr, ui_rect r, bool focused);

void ui_entry_draw(ui_entry *e, cairo_t *cr, ui_rect r, bool focused)
{
    const ui_scheme *k = &ui_scheme_now;
    if (e->preedit) { draw_preedit(e, cr, r, focused); return; }
    ui_fill(cr, r, k->client_bg);
    ui_bevel(cr, r, false);
    double inner = r.w - 2 * PAD;
    double cx = ui_text_caret_x(e->font, e->s, e->cursor);
    /* Keep the caret in view. */
    if (cx - e->scroll > inner) e->scroll = cx - inner;
    if (cx - e->scroll < 0) e->scroll = cx;
    double total = ui_text_caret_x(e->font, e->s, e->len);
    if (total - e->scroll < inner && e->scroll > 0) e->scroll = total > inner ? total - inner : 0;
    cairo_save(cr);
    cairo_rectangle(cr, r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    cairo_clip(cr);
    double x0 = r.x + PAD - e->scroll, base = r.y + (r.h + ui_font_ascent(e->font)) / 2 - 1;
    if (focused && e->cursor != e->anchor) {
        double a = ui_text_caret_x(e->font, e->s, sel_lo(e)), b = ui_text_caret_x(e->font, e->s, sel_hi(e));
        ui_fill(cr, (ui_rect){ x0 + a, r.y + 3, b - a, r.h - 6 }, k->selection);
        ui_set_color(cr, k->text);
        ui_text_draw(cr, e->font, x0, base, e->s, 0);
        /* The selected part again, in the selection's text colour. */
        cairo_save(cr);
        cairo_rectangle(cr, x0 + a, r.y, b - a, r.h);
        cairo_clip(cr);
        ui_set_color(cr, k->selection_text);
        ui_text_draw(cr, e->font, x0, base, e->s, 0);
        cairo_restore(cr);
    } else {
        ui_set_color(cr, k->text);
        ui_text_draw(cr, e->font, x0, base, e->s, 0);
    }
    if (focused) {
        ui_fill(cr, (ui_rect){ x0 + cx, r.y + 3, 1, r.h - 6 }, k->cursor);
    }
    cairo_restore(cr);
}

/* The text with what an input method is composing at the cursor,
 * underlined, the caret where the input method puts it. */
static void draw_preedit(ui_entry *e, cairo_t *cr, ui_rect r, bool focused)
{
    const ui_scheme *k = &ui_scheme_now;
    size_t pl = strlen(e->preedit);
    char *t = malloc(e->len + pl + 1);
    memcpy(t, e->s, e->cursor);
    memcpy(t + e->cursor, e->preedit, pl);
    memcpy(t + e->cursor + pl, e->s + e->cursor, e->len - e->cursor + 1);
    ui_fill(cr, r, k->client_bg);
    ui_bevel(cr, r, false);
    double inner = r.w - 2 * PAD;
    double a = ui_text_caret_x(e->font, t, e->cursor), b = ui_text_caret_x(e->font, t, e->cursor + pl);
    double cx = ui_text_caret_x(e->font, t, e->cursor + (size_t)e->pre_cursor);
    if (b - e->scroll > inner) e->scroll = b - inner;
    if (a - e->scroll < 0) e->scroll = a;
    cairo_save(cr);
    cairo_rectangle(cr, r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    cairo_clip(cr);
    double x0 = r.x + PAD - e->scroll, base = r.y + (r.h + ui_font_ascent(e->font)) / 2 - 1;
    ui_set_color(cr, k->text);
    ui_text_draw(cr, e->font, x0, base, t, 0);
    ui_fill(cr, (ui_rect){ x0 + a, base + 2, b - a, 1 }, k->text);
    if (focused) ui_fill(cr, (ui_rect){ x0 + cx, r.y + 3, 1, r.h - 6 }, k->cursor);
    cairo_restore(cr);
    free(t);
}
