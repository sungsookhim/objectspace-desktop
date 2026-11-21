/*
 * Input methods (G4): Wayland's text-input-v3, which GNOME gives to IBus.
 *
 * The owner of an entry says when it is being typed in
 * (ui_text_input_focus); while it is, and its window has the keyboard, the
 * input method is enabled for it. Text the input method commits reaches
 * the window as key events, one character each (sym 0, utf8 the
 * character), so every owner's own key handling (live search, renaming)
 * sees it as typed; text it deletes around the cursor comes as BackSpace
 * and Delete. Text still being composed (preedit) is drawn in the entry,
 * underlined, until it is committed or dropped.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ui_internal.h"

static ui_app *the_app;
static bool debug;           /* WPS_DEBUG_IM: what the input method does, on stderr */

static void entry_freed(ui_entry *e)
{
    if (the_app && the_app->ti_entry == e) ui_text_input_focus(the_app->ti_surface, NULL, (ui_rect){ 0, 0, 0, 0 });
}

void ui_textinput_key(ui_app *a, ui_surface *s, const char *utf8)
{
    (void)a;
    if (!s || !s->ops->key) return;
    for (const unsigned char *p = (const unsigned char *)utf8; *p;) {
        size_t n = *p < 0x80 ? 1 : *p < 0xe0 ? 2 : *p < 0xf0 ? 3 : 4;
        ui_key_event e = { 0 };
        for (size_t i = 0; i < n && p[i]; i++) e.utf8[i] = (char)p[i];
        s->ops->key(s, &e);
        p += strnlen((const char *)p, n);
    }
}

static void keys(ui_surface *s, uint32_t sym, size_t n)
{
    for (size_t i = 0; i < n && s->ops->key; i++) {
        ui_key_event e = { .sym = sym };
        s->ops->key(s, &e);
    }
}

/* Characters (not bytes) in the `n` bytes before or after the cursor. */
static size_t chars_in(const char *s, size_t from, size_t n)
{
    size_t c = 0;
    for (size_t i = from; i < from + n && s[i]; i++) c += ((unsigned char)s[i] & 0xc0) != 0x80;
    return c;
}

/* Tell the input method where the entry is, or stop it. It is enabled
 * again only for another entry: enabling resets what it is composing. */
static ui_entry *enabled_for;

static void update(ui_app *a)
{
    if (!a->ti) return;
    bool want = a->ti_entry && a->ti_surface && a->ti_entered == a->ti_surface;
    if (want) {
        if (!a->ti_enabled || enabled_for != a->ti_entry) {
            zwp_text_input_v3_enable(a->ti);
            if (debug) fprintf(stderr, "wps-ui: input method enabled\n");
        }
        enabled_for = a->ti_entry;
        zwp_text_input_v3_set_content_type(a->ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                           ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        const char *t = ui_entry_text(a->ti_entry);
        size_t len = strlen(t), cur = ui_entry_cursor(a->ti_entry);
        if (len < 4000) zwp_text_input_v3_set_surrounding_text(a->ti, t, (int32_t)cur, (int32_t)cur);
        ui_rect r = a->ti_rect;
        zwp_text_input_v3_set_cursor_rectangle(a->ti, (int32_t)r.x, (int32_t)r.y, (int32_t)r.w, (int32_t)r.h);
        zwp_text_input_v3_commit(a->ti);
        a->ti_enabled = true;
    } else if (a->ti_enabled) {
        zwp_text_input_v3_disable(a->ti);
        zwp_text_input_v3_commit(a->ti);
        a->ti_enabled = false;
        enabled_for = NULL;
    }
}

void ui_text_input_focus(ui_surface *s, ui_entry *e, ui_rect r)
{
    ui_app *a = s ? s->app : the_app;
    if (!a) return;
    if (a->ti_entry && a->ti_entry != e) {
        ui_entry_set_preedit(a->ti_entry, NULL, 0);   /* what was being composed is dropped */
        if (a->ti_surface) ui_surface_damage(a->ti_surface, a->ti_rect);
    }
    a->ti_entry = e;
    a->ti_surface = e ? s : NULL;
    a->ti_rect = r;
    update(a);
    ui_ibus_focus(a, e && s && s == a->key_focus);   /* (where the session uses IBus) */
}

void ui_textinput_surface_gone(ui_surface *s)
{
    ui_app *a = s->app;
    if (a->ti_entered == s) a->ti_entered = NULL;
    if (a->ti_surface == s) { a->ti_surface = NULL; a->ti_entry = NULL; update(a); }
}

/* ------------------------------------------------------------ protocol */

static void ti_enter(void *d, struct zwp_text_input_v3 *t, struct wl_surface *w)
{
    (void)t;
    ui_app *a = d;
    a->ti_entered = w ? wl_surface_get_user_data(w) : NULL;
    update(a);
}

static void ti_leave(void *d, struct zwp_text_input_v3 *t, struct wl_surface *w)
{
    (void)t; (void)w;
    ui_app *a = d;
    a->ti_entered = NULL;
    if (a->ti_entry) {
        ui_entry_set_preedit(a->ti_entry, NULL, 0);
        if (a->ti_surface) ui_surface_damage(a->ti_surface, a->ti_rect);
    }
    update(a);
}

static void ti_preedit(void *d, struct zwp_text_input_v3 *t, const char *text, int32_t begin, int32_t end)
{
    (void)t; (void)end;
    ui_app *a = d;
    free(a->ti_preedit);
    a->ti_preedit = text ? strdup(text) : NULL;
    a->ti_pre_cursor = begin;
}

static void ti_commit(void *d, struct zwp_text_input_v3 *t, const char *text)
{
    (void)t;
    ui_app *a = d;
    free(a->ti_commit);
    a->ti_commit = text ? strdup(text) : NULL;
}

static void ti_delete(void *d, struct zwp_text_input_v3 *t, uint32_t before, uint32_t after)
{
    (void)t;
    ui_app *a = d;
    a->ti_del_before = before;
    a->ti_del_after = after;
}

static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t serial)
{
    (void)t; (void)serial;
    ui_app *a = d;
    ui_surface *s = a->ti_surface;
    ui_entry *e = a->ti_entry;
    if (debug) fprintf(stderr, "wps-ui: input method done: commit '%s' preedit '%s' delete %u,%u\n", a->ti_commit ? a->ti_commit : "",
                       a->ti_preedit ? a->ti_preedit : "", a->ti_del_before, a->ti_del_after);
    /* In the protocol's order: the preedit goes, text around the cursor is
     * deleted, the committed text is inserted, the new preedit shown. */
    if (e && s) {
        ui_entry_set_preedit(e, NULL, 0);
        if (a->ti_del_before || a->ti_del_after) {
            const char *txt = ui_entry_text(e);
            size_t cur = ui_entry_cursor(e);
            size_t b = a->ti_del_before > cur ? cur : a->ti_del_before;
            keys(s, XKB_KEY_BackSpace, chars_in(txt, cur - b, b));
            if (a->ti_entry == e) keys(s, XKB_KEY_Delete, chars_in(ui_entry_text(e), ui_entry_cursor(e), a->ti_del_after));
        }
        if (a->ti_commit && a->ti_entry == e) ui_textinput_key(a, s, a->ti_commit);
        if (a->ti_preedit && *a->ti_preedit && a->ti_entry == e) ui_entry_set_preedit(e, a->ti_preedit, a->ti_pre_cursor);
        if (a->ti_entry == e) ui_surface_damage(s, a->ti_rect);
    }
    /* The text changed: tell the input method (it answers a commit with
     * another done, so only then, or the two would go back and forth). */
    bool changed = (a->ti_commit && *a->ti_commit) || a->ti_del_before || a->ti_del_after;
    free(a->ti_preedit);
    free(a->ti_commit);
    a->ti_preedit = a->ti_commit = NULL;
    a->ti_del_before = a->ti_del_after = 0;
    if (changed && a->ti_entry == e && e && a->ti) update(a);
}

static const struct zwp_text_input_v3_listener ti_listener = {
    .enter = ti_enter, .leave = ti_leave, .preedit_string = ti_preedit, .commit_string = ti_commit,
    .delete_surrounding_text = ti_delete, .done = ti_done,
};

void ui_text_input_inject(ui_app *a, const char *commit, const char *preedit)
{
    free(a->ti_commit);
    free(a->ti_preedit);
    a->ti_commit = commit ? strdup(commit) : NULL;
    a->ti_preedit = preedit ? strdup(preedit) : NULL;
    a->ti_pre_cursor = preedit ? (int32_t)strlen(preedit) : 0;
    ti_done(a, NULL, 0);
}

void ui_textinput_bind(ui_app *a, struct wl_registry *r, uint32_t name, uint32_t ver)
{
    (void)ver;
    a->ti_manager = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
}

void ui_textinput_seat(ui_app *a)
{
    the_app = a;
    debug = getenv("WPS_DEBUG_IM") != NULL;
    ui_entry_free_hook = entry_freed;
    if (!a->ti_manager || !a->seat || getenv("WPS_UI_NO_IM")) return;
    a->ti = zwp_text_input_manager_v3_get_text_input(a->ti_manager, a->seat);
    zwp_text_input_v3_add_listener(a->ti, &ti_listener, a);
}
