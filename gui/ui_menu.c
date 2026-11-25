/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Pop-up menus with cascades, as Wayland popups (gui-design.md, Widget
 * set 3). Items come from the service's menu data. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ui_internal.h"

/* (the looks and geometry: ui_menupaint.c, shared with the compositor) */
#define ITEM_H ui_menu_look_item_h(&(ui_menu_look){ .icon_small = ui_icons()->small })
struct menu_root;

struct menu {
    struct menu_root *root;
    struct menu *parent, *child;
    const ui_menu_item *items;
    size_t n;
    int hot;
    ui_surface *surf;
    double w, h;
};

struct menu_root {
    ui_menu_fn fn;
    void *arg;
    ui_menu_item *items;   /* owned copy */
    size_t n;
    struct menu *top;
    bool closing;
    int64_t id;            /* the choice, once closing */
    ui_surface *opener;    /* where it was opened, for "click again closes" */
    ui_app *app;
    bool grab;             /* the root took a grab; so may its cascades */
    double ox, oy, closed_at;
    bool (*icon_fn)(void *arg, int64_t id, void *item);
    bool left;             /* it and its cascades open out to the left (ui_menu_next_left) */
    void (*side_fn)(void *ctx, int dir);   /* a menu bar's (ui_menu_next_sideways) */
    void *side_ctx;
    ui_rect bar;           /* the bar's area on the opener (presses there go on to it) */
    int side;              /* Left or Right at its top level: the neighbour, once closed */
};

/* At most one menu tree exists: Wayland requires popups to be destroyed
 * topmost first, so a new menu may only appear once the old one is gone. */
static struct menu_root *active;

static ui_menu_item *copy_items(const ui_menu_item *src, size_t n)
{
    ui_menu_item *d = calloc(n ? n : 1, sizeof *d);
    for (size_t i = 0; i < n; i++) {
        d[i] = src[i];
        d[i].label = strdup(src[i].label ? src[i].label : "");
        d[i].items = src[i].nitems ? copy_items(src[i].items, src[i].nitems) : NULL;
    }
    return d;
}

void ui_menu_items_free(ui_menu_item *items, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(items[i].label);
        ui_menu_items_free(items[i].items, items[i].nitems);
    }
    free(items);
}

static bool is_sep(const ui_menu_item *it) { return it->label && !strcmp(it->label, "-"); }

#define ICON_W (ui_icons()->small + 4.0)

/* Whether any item has an icon: the labels move right for it. */
static bool item_icon(struct menu_root *r, const ui_menu_item *it, ui_item *out)
{
    memset(out, 0, sizeof *out);
    out->loaded = out->detailed = true;
    return r->icon_fn && !is_sep(it) && r->icon_fn(r->arg, it->id, out);
}

static double icon_room(struct menu_root *r, const ui_menu_item *items, size_t n)
{
    ui_item ii;
    for (size_t i = 0; i < n; i++)
        if (item_icon(r, &items[i], &ii)) return ICON_W;
    return 0;
}

static ui_menu_look plain_look(struct menu *m) { return (ui_menu_look){ .items = m->items, .n = m->n, .icon_small = ui_icons()->small }; }

static double item_y(struct menu *m, size_t i)
{
    ui_menu_look l = plain_look(m);
    return ui_menu_look_item_y(&l, i);
}

static int item_at(struct menu *m, double x, double y)
{
    ui_menu_look l = plain_look(m);
    return ui_menu_look_item_at(&l, m->w, x, y);
}

static void close_child(struct menu *m);
static void finish(struct menu_root *r, int64_t id);

/* ----------------------------------------------------------------- drawing */

static bool look_icon(void *arg, const ui_menu_item *it, cairo_t *cr, double x, double y, double size)
{
    ui_item ii;
    if (!item_icon(arg, it, &ii)) return false;
    ui_draw_item_icon(cr, &ii, x, y, size);
    return true;
}

static ui_menu_look look_of(struct menu *m)
{
    ui_menu_look l = plain_look(m);
    l.room = icon_room(m->root, m->items, m->n);
    l.icon = look_icon;
    l.arg = m->root;
    return l;
}

static void menu_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)clip;
    struct menu *m = ui_surface_data(s);
    ui_menu_look l = look_of(m);
    ui_menu_look_draw(cr, &l, m->w, m->h, m->hot);
}

/* --------------------------------------------------------------- behaviour */

static void open_menu(struct menu_root *r, struct menu *parent, ui_surface *on, const ui_menu_item *items,
                      size_t n, ui_rect anchor, uint32_t serial);

static void open_child(struct menu *m, int i, uint32_t serial)
{
    const ui_menu_item *it = &m->items[i];
    if (m->child && m->child->items == it->items) return;
    close_child(m);
    if (!it->nitems || !it->enabled) return;
    ui_rect anchor = { 0, item_y(m, (size_t)i), m->w - 2, ITEM_H };
    /* A cascade grabs only if its root did (the protocol forbids a grab
     * under a popup without one). */
    open_menu(m->root, m, m->surf, it->items, it->nitems, anchor, m->root->grab ? serial : 0);
}

/* For screen readers: the highlighted item, "label, n of m". */
static void a11y_hot(struct menu *m)
{
    if (m->hot < 0 || (size_t)m->hot >= m->n) return;
    const ui_menu_item *it = &m->items[m->hot];
    char label[256], *tab;
    snprintf(label, sizeof label, "%s", it->label ? it->label : "");
    const char *accel = NULL;
    if ((tab = strchr(label, '\t'))) { *tab = 0; accel = tab + 1; }
    int index = 0, count = 0;
    for (size_t k = 0; k < m->n; k++) {
        if (m->items[k].label && !strcmp(m->items[k].label, "-")) continue;
        if ((int)k == m->hot) index = count;
        count++;
    }
    ui_a11y_info info = { .role = UI_ROLE_MENU_ITEM, .name = label, .description = accel, .disabled = !it->enabled,
                          .expandable = it->nitems > 0, .checked = it->checked, .index = index, .count = count,
                          .at = { 0, item_y(m, (size_t)m->hot), m->w, ITEM_H } };
    ui_a11y_focus(m->surf, &info);
}

static void set_hot(struct menu *m, int i)
{
    if (i == m->hot) return;
    m->hot = i;
    ui_surface_damage_all(m->surf);
    a11y_hot(m);
}

static void choose(struct menu *m, int i, uint32_t serial)
{
    if (i < 0) return;
    const ui_menu_item *it = &m->items[i];
    if (!it->enabled) return;
    if (it->nitems) { open_child(m, i, serial); return; }
    ui_action("menu.select", m->surf, (ui_rect){ 0, 0, 0, 0 });
    finish(m->root, it->id);
}

static void menu_pointer(ui_surface *s, const ui_pointer_event *e)
{
    struct menu *m = ui_surface_data(s);
    if (m->root->closing) return;
    int i = item_at(m, e->x, e->y);
    switch (e->kind) {
    case UI_ENTER:
    case UI_MOTION:
        set_hot(m, i);
        if (i >= 0 && m->items[i].nitems) open_child(m, i, s->app->last_serial);
        else if (i >= 0) close_child(m);
        break;
    case UI_RELEASE:
        if (i >= 0 && (e->button == ui_mouse_button(UI_ACT_SELECT) || e->button == ui_mouse_button(UI_ACT_POPUP)))
            choose(m, i, e->serial);
        break;
    default:
        break;
    }
}

static int step(struct menu *m, int from, int dir)
{
    for (size_t k = 0; k < m->n; k++) {
        from = (int)(((size_t)(from + dir) + m->n) % m->n);
        if (!is_sep(&m->items[from])) return from;
    }
    return -1;
}

static void menu_key(ui_surface *s, const ui_key_event *e)
{
    struct menu *m = ui_surface_data(s);
    if (m->root->closing) return;
    /* Keys go to the deepest open menu. */
    while (m->child) m = m->child;
    if (e->alt_tap || e->sym == XKB_KEY_F10) { finish(m->root, -1); return; }   /* (a menu bar's: away) */
    switch (e->sym) {
    case XKB_KEY_Down: set_hot(m, step(m, m->hot < 0 ? -1 : m->hot, 1)); break;
    case XKB_KEY_Up: set_hot(m, step(m, m->hot < 0 ? 0 : m->hot, -1)); break;
    case XKB_KEY_Right:
        if (m->hot >= 0 && m->items[m->hot].nitems) {
            open_child(m, m->hot, s->app->last_serial);
            if (m->child) set_hot(m->child, step(m->child, -1, 1));
        } else if (!m->parent && m->root->side_fn) {   /* a menu bar's: the next menu */
            m->root->side = 1;
            finish(m->root, -1);
        }
        break;
    case XKB_KEY_Left:
        if (m->parent) close_child(m->parent);
        else if (m->root->side_fn) { m->root->side = -1; finish(m->root, -1); }   /* a menu bar's: the one before */
        break;
    case XKB_KEY_Escape:
        if (m->parent) close_child(m->parent);
        else finish(m->root, -1);
        break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space:
        choose(m, m->hot, s->app->last_serial);
        break;
    default: {
        /* A letter picks the next item starting with it. */
        if (!e->utf8[0] || e->utf8[1]) break;
        char c = e->utf8[0] | 0x20;
        for (size_t k = 1; k <= m->n; k++) {
            int j = (int)(((size_t)(m->hot < 0 ? 0 : m->hot) + k) % m->n);
            const char *l = m->items[j].label;
            if (!is_sep(&m->items[j]) && l && (l[0] | 0x20) == c) { set_hot(m, j); break; }
        }
    }
    }
}

static void menu_dismissed(ui_surface *s)
{
    struct menu *m = ui_surface_data(s);
    finish(m->root, -1);
}

static const ui_surface_ops menu_ops = {
    .draw = menu_draw, .pointer = menu_pointer, .key = menu_key, .dismissed = menu_dismissed,
};

static void open_menu(struct menu_root *r, struct menu *parent, ui_surface *on, const ui_menu_item *items,
                      size_t n, ui_rect anchor, uint32_t serial)
{
    struct menu *m = calloc(1, sizeof *m);
    m->root = r;
    m->parent = parent;
    m->items = items;
    m->n = n;
    m->hot = -1;
    ui_menu_look l = look_of(m);
    ui_menu_look_size(&l, &m->w, &m->h);
    m->surf = r->left ? ui_popup_new_beside(on->app, on, &menu_ops, m, anchor, m->w, m->h, serial, true)
                      : ui_popup_new(on->app, on, &menu_ops, m, anchor, m->w, m->h, serial);
    if (parent) parent->child = m;
    else r->top = m;
}

static void destroy_menu(struct menu *m)
{
    if (m->child) destroy_menu(m->child);
    if (m->parent) m->parent->child = NULL;
    ui_surface_destroy(m->surf);
    free(m);
}

static void close_child(struct menu *m)
{
    if (m->child) destroy_menu(m->child);
}

/* Destroy the popups now (deepest first); safe to call again. */
static void close_now(struct menu_root *r)
{
    if (r->top) {
        destroy_menu(r->top);
        r->top = NULL;
    }
}

/* Closing completes on the next loop turn: the event that caused it is
 * still being delivered to one of these surfaces. */
static void finish_now(void *arg)
{
    struct menu_root *r = arg;
    close_now(r);
    if (active == r) active = NULL;
    if (!active) ui_container_menu_closed();
    if (r->fn) r->fn(r->arg, r->id);
    if (r->side && r->side_fn) r->side_fn(r->side_ctx, r->side);
    ui_menu_items_free(r->items, r->n);
    free(r);
}

static void finish(struct menu_root *r, int64_t id)
{
    if (r->closing) return;
    r->closing = true;
    r->id = id;
    r->closed_at = ui_now_ms();
    ui_app_post(r->app, finish_now, r);
}

static bool (*next_icon_fn)(void *arg, int64_t id, void *item);
static bool next_left;
static void (*next_side_fn)(void *ctx, int dir);
static void *next_side_ctx;
static ui_rect next_bar;

void ui_menu_next_sideways(void (*fn)(void *ctx, int dir), void *ctx, ui_rect bar) { next_side_fn = fn; next_side_ctx = ctx; next_bar = bar; }

void ui_menu_next_left(void) { next_left = true; }

void ui_menu_popup_icons(ui_surface *parent, const ui_menu_item *items, size_t n, double x, double y, uint32_t serial,
                         ui_menu_fn fn, void *arg, bool (*icon_fn)(void *arg, int64_t id, void *item))
{
    next_icon_fn = icon_fn;
    ui_menu_popup(parent, items, n, x, y, serial, fn, arg);
}

void ui_menu_popup(ui_surface *parent, const ui_menu_item *items, size_t n, double x, double y, uint32_t serial,
                   ui_menu_fn fn, void *arg)
{
    bool (*icon_fn)(void *, int64_t, void *) = next_icon_fn;
    next_icon_fn = NULL;
    bool left = next_left;
    next_left = false;
    void (*side_fn)(void *, int) = next_side_fn;
    void *side_ctx = next_side_ctx;
    ui_rect bar = next_bar;
    next_side_fn = NULL;
    if (!n) { if (fn) fn(arg, -1); return; }
    if (active) {
        struct menu_root *old = active;
        /* The click that dismissed a menu, landing on the button that
         * opened it, closes it rather than opening it again. */
        bool same = old->closing && old->opener == parent && fabs(old->ox - x) < 1 && fabs(old->oy - y) < 1 &&
                    ui_now_ms() - old->closed_at < 300;
        finish(old, -1);
        close_now(old);
        if (same) { if (fn) fn(arg, -1); return; }
    }
    ui_action("menu.open", parent, (ui_rect){ x, y, 0, 0 });
    struct menu_root *r = calloc(1, sizeof *r);
    active = r;
    r->opener = parent;
    r->app = parent->app;
    r->grab = serial != 0;
    r->ox = x;
    r->oy = y;
    r->fn = fn;
    r->arg = arg;
    r->icon_fn = icon_fn;
    r->left = left;
    r->side_fn = side_fn;
    r->side_ctx = side_ctx;
    r->bar = side_fn ? bar : (ui_rect){ 0, 0, 0, 0 };
    r->items = copy_items(items, n);
    r->n = n;
    ui_rect anchor = { x, y, 1, 1 };
    open_menu(r, NULL, parent, r->items, n, anchor, serial);
}

static bool in_tree(struct menu_root *r, ui_surface *s)
{
    for (struct menu *m = r->top; m; m = m->child)
        if (m->surf == s) return true;
    return false;
}

bool ui_menu_press_outside_at(ui_surface *s, double x, double y)
{
    if (active && !active->closing && s == active->opener && active->bar.w > 0 &&
        x >= active->bar.x && y >= active->bar.y && x < active->bar.x + active->bar.w && y < active->bar.y + active->bar.h) {
        return false;   /* a menu bar's: the press goes on to the bar (another title, or the same: it decides) */
    }
    return ui_menu_press_outside(s);
}

bool ui_menu_press_outside(ui_surface *s)
{
    if (!active || active->closing || in_tree(active, s)) return false;
    finish(active, -1);
    close_now(active);
    return true;
}

ui_surface *ui_menu_key_surface(void)
{
    if (!active || active->closing || !active->top) return NULL;
    return active->top->surf;   /* menu_key passes them to the deepest open menu */
}

void ui_menu_close_all(void)
{
    if (!active || active->closing) return;
    finish(active, -1);
    close_now(active);
}

bool ui_menu_is_open(void) { return active && !active->closing; }

void ui_menu_forget_surface(ui_surface *s)
{
    if (!active) return;
    if (active->opener == s || in_tree(active, s)) {
        if (in_tree(active, s)) return;   /* a menu's own popup going away */
        finish(active, -1);
        close_now(active);
        active->opener = NULL;
    }
}
