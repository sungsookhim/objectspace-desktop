/* Other program... (apppick.h). */
#define _GNU_SOURCE
#include "apppick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "appicons.h"
#include "cbor.h"
#include "events.h"
#include "svc.h"

#define M 8
#define ROW 24
#define STATUS_H 20

struct app { char *id, *name, *icon; };

enum { F_LIST, F_ALWAYS, F_COUNT };

static ui_app *app;

static struct picker {
    ui_surface *s;
    ui_container *cont;
    struct app *v;
    size_t n;
    bool read;                /* the list came */
    bool always;
    int focus;
    unsigned gen;             /* a reply for an earlier opening is dropped */
    apppick_fn chosen;
    long owner;
    char target[4096];
    char title[300];
    bool closing;
} pk;

static void clear_apps(void)
{
    for (size_t i = 0; i < pk.n; i++) { free(pk.v[i].id); free(pk.v[i].name); free(pk.v[i].icon); }
    free(pk.v);
    pk.v = NULL;
    pk.n = 0;
}

static size_t s_count(void *ctx) { (void)ctx; return pk.n; }

static void s_item(void *ctx, size_t i, ui_item *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (i >= pk.n) return;
    out->loaded = out->detailed = true;
    out->kind = UI_KIND_PROGRAM;
    out->title = out->name = pk.v[i].name;
    out->image = pk.v[i].icon && *pk.v[i].icon ? appicon_named(pk.v[i].icon) : NULL;
}

static void close_picker(void);

static void choose(size_t i)
{
    if (i >= pk.n) return;
    apppick_fn fn = pk.chosen;
    long owner = pk.owner;
    bool always = pk.always;
    char target[sizeof pk.target], id[512];
    snprintf(target, sizeof target, "%s", pk.target);
    snprintf(id, sizeof id, "%s", pk.v[i].id);
    close_picker();
    if (fn) fn(owner, target, id, always);
}

static void s_activate(void *ctx, size_t i) { (void)ctx; choose(i); }

static const ui_container_src pick_src = { s_count, s_item, NULL, s_activate, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

struct rects { ui_rect list, always, status; };

static struct rects rects(void)
{
    ui_rect c = ui_frame_client(pk.s);
    struct rects r;
    r.status = (ui_rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    r.always = (ui_rect){ c.x + M, r.status.y - M - ROW, c.w - 2 * M, ROW };
    r.list = (ui_rect){ c.x, c.y, c.w, r.always.y - M - c.y };
    return r;
}

static void pk_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s;
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(pk.s, cr, clip, pk.title);
    struct rects r = rects();
    ui_container_draw(pk.cont, cr, clip);
    ui_rect c = ui_frame_client(pk.s);
    ui_rect bottom = { c.x, r.list.y + r.list.h, c.w, c.y + c.h - (r.list.y + r.list.h) };
    if (!ui_rect_meets(clip, bottom)) return;
    ui_fill(cr, bottom, k->face);
    /* Always use for this type */
    ui_rect b = { r.always.x + 4, r.always.y + 4, ROW - 8, ROW - 8 };
    ui_fill(cr, b, k->client_bg);
    ui_bevel(cr, b, false);
    if (pk.always) {
        ui_set_color(cr, k->text);
        cairo_set_line_width(cr, 2);
        cairo_move_to(cr, b.x + 3, b.y + b.h / 2);
        cairo_line_to(cr, b.x + b.w / 2 - 1, b.y + b.h - 4);
        cairo_line_to(cr, b.x + b.w - 3, b.y + 3);
        cairo_stroke(cr);
    }
    const char *lab = "Always use for this type";
    ui_set_color(cr, k->text);
    double lx = r.always.x + ROW + 4;
    ui_text_draw(cr, UI_FONT_REGULAR, lx, r.always.y + (ROW + ui_text_ascent()) / 2 - 1, lab, r.always.w - ROW - 4);
    if (pk.focus == F_ALWAYS) {
        ui_set_color(cr, k->cursor);
        double dash = 1;
        cairo_save(cr);
        cairo_set_dash(cr, &dash, 1, 0);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, lx - 2.5, r.always.y + 2.5, ui_text_width(UI_FONT_REGULAR, lab) + 5, ROW - 5);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    ui_bevel(cr, (ui_rect){ r.status.x + 1, r.status.y + 1, r.status.w - 2, r.status.h - 2 }, false);
    char msg[160];
    if (!pk.read) snprintf(msg, sizeof msg, "Reading the programs...");
    else snprintf(msg, sizeof msg, "%zu program%s; double-click one to open the file with it", pk.n, pk.n == 1 ? "" : "s");
    ui_set_color(cr, k->text);
    cairo_save(cr);
    cairo_rectangle(cr, r.status.x + 2, r.status.y + 2, r.status.w - 4, r.status.h - 4);
    cairo_clip(cr);
    ui_text_draw(cr, UI_FONT_REGULAR, r.status.x + 6, r.status.y + (STATUS_H + ui_text_ascent()) / 2 - 1, msg, r.status.w - 12);
    cairo_restore(cr);
}

static void a11y(void)
{
    if (pk.focus != F_ALWAYS) return;   /* (the list says which program) */
    ui_a11y_info i = { .index = pk.focus, .count = F_COUNT, .role = UI_ROLE_CHECK_BOX, .name = "Always use for this type",
                       .checked = pk.always, .at = rects().always };
    ui_a11y_focus(pk.s, &i);
}

static void set_focus(int f)
{
    pk.focus = f;
    ui_container_set_focused(pk.cont, f == F_LIST);
    ui_surface_damage_all(pk.s);
    a11y();
}

static void toggle_always(void)
{
    pk.always = !pk.always;
    ui_surface_damage_all(pk.s);
    a11y();
}

static void pk_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    bool used = false;
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) used = ui_frame_pointer(s, e);
    struct rects r = rects();
    if (e->kind == UI_PRESS && !used && h == UI_HIT_CLIENT) {
        if (ui_rect_has(r.list, e->x, e->y)) {
            if (pk.focus != F_LIST) set_focus(F_LIST);
            ui_container_pointer(pk.cont, e);
            return;
        }
        if (e->button == UI_BTN1 && ui_rect_has(r.always, e->x, e->y)) {
            set_focus(F_ALWAYS);
            toggle_always();
        }
        return;
    }
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) ui_container_pointer(pk.cont, e);
}

static void pk_key(ui_surface *s, const ui_key_event *e)
{
    (void)s;
    if (e->sym == XKB_KEY_Tab || e->sym == XKB_KEY_ISO_Left_Tab) {
        set_focus((pk.focus + 1) % F_COUNT);
        return;
    }
    if (e->sym == XKB_KEY_Escape) { close_picker(); return; }
    if (pk.focus == F_ALWAYS) {
        if (e->sym == XKB_KEY_space) toggle_always();
        return;
    }
    ui_container_key(pk.cont, e);
}

static void free_later(void *arg)
{
    (void)arg;
    ui_container_free(pk.cont);
    ui_surface_destroy(pk.s);
    clear_apps();
    pk.s = NULL;
    pk.cont = NULL;
    pk.closing = false;
}

static void close_picker(void)
{
    if (!pk.s || pk.closing) return;
    pk.closing = true;
    pk.gen++;
    events_raise("window.close", "Open With", "window", (ui_rect){ 0, 0, 0, 0 });
    ui_app_post(app, free_later, NULL);
}

static void pk_close(ui_surface *s) { (void)s; close_picker(); }

static void pk_resized(ui_surface *s, double w, double h)
{
    (void)w; (void)h;
    ui_container_layout(pk.cont, rects().list);
    ui_surface_damage_all(s);
}

static void pk_focus(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }

static const ui_surface_ops pk_ops = { .draw = pk_draw, .pointer = pk_pointer, .key = pk_key, .resized = pk_resized,
                                       .close = pk_close, .focus = pk_focus };

static void got_apps(void *arg, const cv *reply)
{
    unsigned gen = (unsigned)(uintptr_t)arg;
    if (!pk.s || pk.closing || gen != pk.gen) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    clear_apps();
    pk.read = true;
    if (res && res->k == CV_ARRAY) {
        pk.v = calloc(res->n ? res->n : 1, sizeof *pk.v);
        for (size_t i = 0; i < res->n; i++) {
            const char *id = cv_text(&res->items[i], "id"), *nm = cv_text(&res->items[i], "name"), *ic = cv_text(&res->items[i], "icon");
            if (!id) continue;
            pk.v[pk.n++] = (struct app){ strdup(id), strdup(nm ? nm : id), strdup(ic ? ic : "") };
        }
    }
    ui_container_reset(pk.cont);
    ui_container_layout_changed(pk.cont);
    ui_container_set_cursor(pk.cont, 0);
    ui_surface_damage_all(pk.s);
}

void apppick_init(ui_app *a) { app = a; }

void apppick_open(const char *title, const char *target, apppick_fn chosen, long owner)
{
    pk.chosen = chosen;
    pk.owner = owner;
    snprintf(pk.target, sizeof pk.target, "%s", target);
    snprintf(pk.title, sizeof pk.title, "Open With - %s", title && *title ? title : "a file");
    pk.always = false;
    if (pk.s && !pk.closing) {   /* one at a time: it now opens this */
        ui_window_set_title(pk.s, pk.title);
        ui_window_activate(pk.s);
        ui_surface_damage_all(pk.s);
        return;
    }
    pk.read = false;
    pk.s = ui_window_new(app, &pk_ops, NULL, "org.wps.OpenWith", 560, 420);
    ui_window_set_title(pk.s, pk.title);
    pk.cont = ui_container_new(pk.s, &pick_src, NULL, UI_VIEW_ICON);
    ui_container_layout(pk.cont, rects().list);
    events_raise("window.open", "Open With", "window", (ui_rect){ 0, 0, 0, 0 });
    set_focus(F_LIST);
    svc_apps(got_apps, (void *)(uintptr_t)pk.gen);
}

void apppick_icons_changed(void)
{
    if (pk.s && !pk.closing) ui_container_changed(pk.cont, 0, SIZE_MAX);
}

bool apppick_is_open(void) { return pk.s && !pk.closing && pk.read; }
size_t apppick_count(void) { return pk.s ? pk.n : 0; }
const char *apppick_name(size_t i) { return pk.s && i < pk.n ? pk.v[i].name : NULL; }
bool apppick_choose(size_t i, bool always)
{
    if (!pk.s || i >= pk.n) return false;
    pk.always = always;
    choose(i);
    return true;
}
bool apppick_snapshot(const char *png) { return pk.s && ui_surface_snapshot(pk.s, png); }
