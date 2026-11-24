/* The Messages list (messages.h). */
#define _GNU_SOURCE
#include "messages.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "appicons.h"
#include "events.h"
#include "notify.h"

void desktop_say(const char *text, bool sticky);   /* (the desktop's status line: window.c) */
void desktop_messages_enable(bool on);
#include "svc.h"
#include "thebar.h"

#define MAX_ACT 16
#define BAR_H 34
#define MAX_BTNS 8

struct msg {
    int64_t id, time, nid, urgency;
    char app[128], icon[512], summary[256], body[1024], sender[96];
    char *actions[MAX_ACT];
    size_t nact;
    bool read, closed;
};

static ui_app *app;
static struct msg *msgs;          /* newest first */
static size_t nmsgs;
static size_t unread;
static bool desktop_line = true;   /* the setting: Objectspace's messages on the desktop */
static bool listing, list_again;

static struct {
    ui_surface *s;
    bool closing;
    int64_t sel;                  /* the selected message's ID, 0: none */
    double scroll;
    int pressed;                  /* a button held, -1 none */
    /* the buttons now: label, and what each does */
    char label[MAX_BTNS][128];
    char key[MAX_BTNS][128];      /* an action's key, or "" */
    int what[MAX_BTNS];           /* 0 an action, 1 mark all read, 2 delete, 3 delete all */
    ui_rect at[MAX_BTNS];
    size_t nbtn;
} w = { .pressed = -1 };

static void ignore(void *arg, const cv *reply) { (void)arg; (void)reply; }

/* --------------------------------------------------------------- model */

static void free_msgs(void)
{
    for (size_t i = 0; i < nmsgs; i++)
        for (size_t k = 0; k < msgs[i].nact; k++) free(msgs[i].actions[k]);
    free(msgs);
    msgs = NULL;
    nmsgs = 0;
}

static void copy_text(char *out, size_t n, const cv *m, const char *key)
{
    const char *t = cv_text(m, key);
    snprintf(out, n, "%s", t ? t : "");
}

static void redraw(void) { if (w.s && !w.closing) ui_surface_damage_all(w.s); }
static void fetch(void);

static void got_list(void *arg, const cv *reply)
{
    (void)arg;
    listing = false;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *list = res ? cv_get(res, "messages") : NULL;
    if (list && list->k == CV_ARRAY) {
        free_msgs();
        msgs = calloc(list->n ? list->n : 1, sizeof *msgs);
        for (size_t i = 0; i < list->n; i++) {
            const cv *o = &list->items[i];
            if (o->k != CV_MAP) continue;
            struct msg *m = &msgs[nmsgs++];
            cv_int(o, "id", &m->id);
            cv_int(o, "time", &m->time);
            cv_int(o, "nid", &m->nid);
            cv_int(o, "urgency", &m->urgency);
            copy_text(m->app, sizeof m->app, o, "app");
            copy_text(m->icon, sizeof m->icon, o, "icon");
            copy_text(m->summary, sizeof m->summary, o, "summary");
            copy_text(m->body, sizeof m->body, o, "body");
            copy_text(m->sender, sizeof m->sender, o, "sender");
            const cv *b = cv_get(o, "read");
            m->read = b && b->k == CV_BOOL && b->b;
            b = cv_get(o, "closed");
            m->closed = b && b->k == CV_BOOL && b->b;
            const cv *a = cv_get(o, "actions");
            for (size_t k = 0; a && a->k == CV_ARRAY && k < a->n && m->nact < MAX_ACT; k++)
                if (a->items[k].k == CV_TEXT) m->actions[m->nact++] = strdup(a->items[k].s);
        }
        /* osdd's own new ones (an action not done): on the desktop for a
         * moment too (not those there at the start) */
        static int64_t seen = -1;
        int64_t top = seen;
        for (size_t i = 0; i < nmsgs; i++) {
            if (msgs[i].id > top) top = msgs[i].id;
            if (seen < 0 || msgs[i].id <= seen || strcmp(msgs[i].app, "Objectspace") || msgs[i].closed || !desktop_line) continue;
            char t[1400];
            snprintf(t, sizeof t, "%s: %.*s", msgs[i].summary, (int)strcspn(msgs[i].body, "\n"), msgs[i].body);
            desktop_say(t, false);
        }
        seen = top < 0 ? 0 : top;
        int64_t u = 0;
        if (cv_int(res, "unread", &u)) unread = (size_t)u;
        thebar_messages(unread, messages_open);
        redraw();
    }
    if (list_again) { list_again = false; fetch(); }
}

static void fetch(void)
{
    if (listing) { list_again = true; return; }
    listing = true;
    svc_msg_list(got_list, NULL);
}

void messages_init(ui_app *a)
{
    app = a;
    fetch();
}

bool messages_event(const cv *ev)
{
    const char *name = cv_text(ev, "event");
    if (!name || strcmp(name, "messages_changed")) return false;
    int64_t u;
    if (cv_int(ev, "unread", &u)) { unread = (size_t)u; thebar_messages(unread, messages_open); }
    fetch();
    return true;
}

void messages_setting(const char *key, const char *val)
{
    if (!strcmp(key, "dnd")) notify_set_dnd(!strcmp(val, "1"));
    else if (!strcmp(key, "popup_seconds")) notify_set_seconds(atoi(val));
    else if (!strcmp(key, "desktop_line")) desktop_messages_enable(desktop_line = strcmp(val, "0") != 0);
}

size_t messages_unread(void) { return unread; }

void messages_post(const char *summary, const char *body, int urgency)
{
    svc_message m = { "Objectspace", urgency == 2 ? "dialog-error" : "dialog-information", summary, body, "", urgency, 0, NULL, 0 };
    svc_msg_add(&m, ignore, NULL);
    if (urgency == 2) notify_local(summary, body, urgency);
}

static struct msg *selected(void)
{
    for (size_t i = 0; i < nmsgs; i++) if (msgs[i].id == w.sel) return &msgs[i];
    return NULL;
}

/* -------------------------------------------------------------- window */

static double row_h(void) { return ceil(ui_scheme_now.font_px * 1.35) * 2 + 10; }

static ui_rect list_rect(void)
{
    ui_rect c = ui_frame_client(w.s);
    return (ui_rect){ c.x, c.y, c.w, c.h - BAR_H };
}

static void when(int64_t t, char *out, size_t n)
{
    time_t tt = (time_t)t, now = time(NULL);
    struct tm a, b;
    localtime_r(&tt, &a);
    localtime_r(&now, &b);
    strftime(out, n, a.tm_year == b.tm_year && a.tm_yday == b.tm_yday ? "%H:%M" : "%x %H:%M", &a);
}

/* The buttons for what is selected now. */
static void make_buttons(void)
{
    w.nbtn = 0;
    struct msg *m = selected();
    if (m && !m->closed && notify_can_invoke(m->nid, m->sender)) {
        for (size_t i = 0; i + 1 < m->nact && w.nbtn < 4; i += 2) {
            snprintf(w.label[w.nbtn], sizeof w.label[0], "%s", !strcmp(m->actions[i], "default") ? "Open" : m->actions[i + 1]);
            snprintf(w.key[w.nbtn], sizeof w.key[0], "%s", m->actions[i]);
            w.what[w.nbtn++] = 0;
        }
    }
    static const char *const fixed[] = { "Mark all read", "Delete", "Delete all" };
    for (int k = 0; k < 3; k++) {
        snprintf(w.label[w.nbtn], sizeof w.label[0], "%s", fixed[k]);
        w.key[w.nbtn][0] = 0;
        w.what[w.nbtn++] = k + 1;
    }
    /* the message's actions from the left, the list's own from the right */
    ui_rect c = ui_frame_client(w.s);
    double x = c.x + 6, y = c.y + c.h - BAR_H + 5, right = c.x + c.w - 6;
    for (size_t i = 0; i < w.nbtn; i++) {
        double bw = fmax(70, ui_text_width(UI_FONT_REGULAR, w.label[i]) + 20);
        if (w.what[i] == 0) { w.at[i] = (ui_rect){ x, y, bw, BAR_H - 10 }; x += bw + 6; }
    }
    for (size_t i = w.nbtn; i-- > 0;) {
        if (w.what[i] == 0) continue;
        double bw = fmax(70, ui_text_width(UI_FONT_REGULAR, w.label[i]) + 20);
        right -= bw;
        w.at[i] = (ui_rect){ right, y, bw, BAR_H - 10 };
        right -= 6;
    }
}

static bool button_enabled(size_t i)
{
    switch (w.what[i]) {
    case 1: return unread > 0;
    case 2: return selected() != NULL;
    case 3: return nmsgs > 0;
    default: return true;
    }
}

static void mw_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(s, cr, clip, "Messages");
    ui_rect L = list_rect();
    cairo_save(cr);
    cairo_rectangle(cr, L.x, L.y, L.w, L.h);
    cairo_clip(cr);
    ui_fill(cr, L, k->client_bg);
    double rh = row_h(), lh = ceil(k->font_px * 1.35);
    if (!nmsgs) {
        ui_set_color(cr, k->disabled_text);
        const char *t = "No messages";
        ui_text_draw(cr, UI_FONT_REGULAR, L.x + (L.w - ui_text_width(UI_FONT_REGULAR, t)) / 2, L.y + 30, t, L.w);
    }
    for (size_t i = 0; i < nmsgs; i++) {
        double y = L.y + i * rh - w.scroll;
        if (y + rh < L.y || y > L.y + L.h) continue;
        const struct msg *m = &msgs[i];
        bool sel = m->id == w.sel;
        ui_rect r = { L.x, y, L.w, rh };
        if (sel) ui_fill(cr, r, k->selection);
        else if (i % 2) ui_fill(cr, r, k->alt_row);
        ui_color fg = sel ? k->selection_text : k->text;
        cairo_surface_t *ic = appicon_named(m->icon);
        if (!ic && m->app[0]) ic = appicon_named(m->app);
        double isz = 24;
        if (ic) {
            double iw = cairo_image_surface_get_width(ic), sc = isz / (iw > 0 ? iw : isz);
            cairo_save(cr);
            cairo_translate(cr, r.x + 6, y + (rh - isz) / 2);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, ic, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        } else {
            ui_draw_kind_icon(cr, UI_KIND_PROGRAM, r.x + 6, y + (rh - isz) / 2, isz);
        }
        char t[96], right[256];
        when(m->time, t, sizeof t);
        snprintf(right, sizeof right, "%s  %s", m->app[0] ? m->app : "?", t);
        double rw = ui_text_width(UI_FONT_REGULAR, right), tx = r.x + 6 + isz + 8;
        double b1 = y + 5 + ui_text_ascent();
        ui_set_color(cr, sel ? fg : k->disabled_text);
        ui_text_draw(cr, UI_FONT_REGULAR, r.x + r.w - rw - 8, b1, right, 0);
        ui_set_color(cr, m->urgency == 2 && !sel ? k->urgent_text : fg);
        ui_text_draw(cr, m->read ? UI_FONT_REGULAR : UI_FONT_BOLD, tx, b1, m->summary[0] ? m->summary : m->app, r.w - (tx - r.x) - rw - 20);
        char body[1024];
        snprintf(body, sizeof body, "%s", m->body);
        for (char *q = body; *q; q++) if (*q == '\n') *q = ' ';
        ui_set_color(cr, fg);
        ui_text_draw(cr, UI_FONT_REGULAR, tx, b1 + lh, body, r.w - (tx - r.x) - 8);
    }
    cairo_restore(cr);
    /* the buttons */
    ui_rect c = ui_frame_client(s);
    ui_rect bar = { c.x, c.y + c.h - BAR_H, c.w, BAR_H };
    ui_fill(cr, bar, k->face);
    make_buttons();
    for (size_t i = 0; i < w.nbtn; i++) {
        bool down = w.pressed == (int)i, on = button_enabled(i);
        ui_fill(cr, w.at[i], k->face);
        ui_bevel(cr, w.at[i], !down);
        ui_set_color(cr, on ? k->text : k->disabled_text);
        double tw = ui_text_width(UI_FONT_REGULAR, w.label[i]);
        ui_text_draw(cr, UI_FONT_REGULAR, w.at[i].x + (w.at[i].w - tw) / 2 + down,
                     w.at[i].y + (w.at[i].h + ui_text_ascent()) / 2 - 1 + down, w.label[i], w.at[i].w - 6);
    }
}

static void clamp_scroll(void)
{
    double max = nmsgs * row_h() - list_rect().h;
    if (w.scroll > max) w.scroll = max;
    if (w.scroll < 0) w.scroll = 0;
}

static void select_msg(size_t i)
{
    if (i >= nmsgs) return;
    w.sel = msgs[i].id;
    if (!msgs[i].read) {   /* read: seen in the list */
        msgs[i].read = true;
        svc_msg_set(&msgs[i].id, 1, 1, -1, ignore, NULL);
    }
    /* in view */
    ui_rect L = list_rect();
    double y = i * row_h();
    if (y < w.scroll) w.scroll = y;
    if (y + row_h() > w.scroll + L.h) w.scroll = y + row_h() - L.h;
    clamp_scroll();
    redraw();
}

static size_t sel_index(void)
{
    for (size_t i = 0; i < nmsgs; i++) if (msgs[i].id == w.sel) return i;
    return SIZE_MAX;
}

static void do_button(size_t i)
{
    if (i >= w.nbtn || !button_enabled(i)) return;
    struct msg *m = selected();
    switch (w.what[i]) {
    case 0:
        if (m) {
            events_raise("object.open", m->app, "message", (ui_rect){ 0, 0, 0, 0 });
            notify_invoke(m->nid, m->sender, w.key[i]);
        }
        break;
    case 1: svc_msg_set(NULL, 0, 1, -1, ignore, NULL); break;
    case 2:
        if (m) {
            size_t k = sel_index();
            int64_t id = m->id;
            svc_msg_delete(&id, 1, ignore, NULL);
            w.sel = k + 1 < nmsgs ? msgs[k + 1].id : k > 0 ? msgs[k - 1].id : 0;   /* the next one */
        }
        break;
    case 3: svc_msg_delete(NULL, 0, ignore, NULL); w.sel = 0; break;
    }
    redraw();
}

static void mw_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    if (h != UI_HIT_CLIENT && e->kind != UI_RELEASE && e->kind != UI_MOTION) { ui_frame_pointer(s, e); return; }
    if (h != UI_HIT_CLIENT && ui_frame_pointer(s, e)) return;
    ui_rect L = list_rect();
    if (e->kind == UI_WHEEL) { w.scroll += e->wheel; clamp_scroll(); redraw(); return; }
    if (e->kind == UI_PRESS && e->button == UI_BTN1) {
        for (size_t i = 0; i < w.nbtn; i++)
            if (ui_rect_has(w.at[i], e->x, e->y)) { w.pressed = (int)i; redraw(); return; }
        if (ui_rect_has(L, e->x, e->y)) {
            size_t i = (size_t)((e->y - L.y + w.scroll) / row_h());
            if (i < nmsgs) {
                select_msg(i);
                if (e->clicks == 2) {   /* its default action, if it can still be taken */
                    struct msg *m = &msgs[i];
                    for (size_t k = 0; k + 1 < m->nact; k += 2)
                        if (!strcmp(m->actions[k], "default")) { notify_invoke(m->nid, m->sender, "default"); break; }
                }
            } else { w.sel = 0; redraw(); }
        }
    } else if (e->kind == UI_RELEASE && w.pressed >= 0) {
        int p = w.pressed;
        w.pressed = -1;
        redraw();
        if (ui_rect_has(w.at[p], e->x, e->y)) do_button((size_t)p);
    }
}

static void mw_close(ui_surface *s);

static void mw_key(ui_surface *s, const ui_key_event *e)
{
    (void)s;
    size_t i = sel_index();
    if (e->sym == XKB_KEY_Escape) { mw_close(w.s); return; }
    if (e->sym == XKB_KEY_Down) select_msg(i == SIZE_MAX ? 0 : i + 1);
    else if (e->sym == XKB_KEY_Up && i != SIZE_MAX && i > 0) select_msg(i - 1);
    else if (e->sym == XKB_KEY_Home) select_msg(0);
    else if (e->sym == XKB_KEY_End && nmsgs) select_msg(nmsgs - 1);
    else if (e->sym == XKB_KEY_Delete) { for (size_t b = 0; b < w.nbtn; b++) if (w.what[b] == 2) do_button(b); }
}

static void mw_free(void *arg)
{
    (void)arg;
    if (w.s) ui_surface_destroy(w.s);
    w.s = NULL;
    w.closing = false;
}

static void mw_close(ui_surface *s)
{
    (void)s;
    if (w.closing) return;
    w.closing = true;
    events_raise("window.close", "", "window", (ui_rect){ 0, 0, 0, 0 });
    ui_app_post(app, mw_free, NULL);
}

static void mw_resized(ui_surface *s, double ww, double hh)
{
    (void)ww; (void)hh;
    clamp_scroll();
    ui_surface_damage_all(s);
}

static void mw_focus(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }

static const ui_surface_ops mw_ops = { .draw = mw_draw, .pointer = mw_pointer, .key = mw_key, .resized = mw_resized,
                                       .close = mw_close, .focus = mw_focus };

void messages_open(void)
{
    if (w.s && !w.closing) { ui_window_activate(w.s); return; }
    if (w.s) return;   /* (closing) */
    w.sel = 0;
    w.scroll = 0;
    w.pressed = -1;
    w.s = ui_window_new(app, &mw_ops, NULL, "io.github.sungsookhim.Objectspace.Messages", 560, 380);
    ui_window_set_title(w.s, "Messages");
    events_raise("window.open", "", "window", (ui_rect){ 0, 0, 0, 0 });
    fetch();
}

bool messages_is_open(void) { return w.s && !w.closing; }
size_t messages_count(void) { return nmsgs; }
const char *messages_summary(size_t i) { return i < nmsgs ? msgs[i].summary : NULL; }
bool messages_is_read(size_t i) { return i < nmsgs && msgs[i].read; }
bool messages_select(size_t i) { if (!w.s || i >= nmsgs) return false; select_msg(i); return true; }

bool messages_button(const char *label)
{
    if (!w.s) return false;
    make_buttons();
    for (size_t i = 0; i < w.nbtn; i++)
        if (!strcmp(w.label[i], label)) { do_button(i); return true; }
    return false;
}

bool messages_snapshot(const char *png) { return w.s && !w.closing && ui_surface_snapshot(w.s, png); }
