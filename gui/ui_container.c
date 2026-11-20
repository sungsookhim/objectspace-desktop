/* Containers: icon and details views over a model of any size
 * (gui-design.md, Widget set 2). Rows are fetched through the source as
 * they come into view; unloaded rows are placeholders. */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ui_internal.h"

#define ROW_H 20
#define HEAD_H 20
#define SB_W 16
#define SBW(c) ((c)->no_scrollbars ? 0 : SB_W)
#define CELL_W UI_CELL_W
#define CELL_H UI_CELL_H
#define ICON 32
#define INDENT 20                /* tree view: per level */

struct ui_container {
    bool no_scrollbars;      /* the desktop: none drawn, none taken */
    ui_layer_draw_fn backdrop;   /* behind the items, if set (the desktop) */
    void *backdrop_ctx;
    ui_surface *s;
    const ui_container_src *src;
    void *ctx;
    ui_view view;
    ui_rect r;
    double scroll;            /* whole logical pixels */
    double hscroll;           /* free icon view only */
    bool hthumb_drag;
    size_t count;
    uint8_t *sel;             /* selection bitmap */
    size_t selcap, nsel;
    size_t cursor, anchor;
    bool has_cursor, focused;
    bool thumb_drag;
    double thumb_off;
    int arrow_down;           /* -1 up, +1 down, 0 none */
    bool band;
    double bx0, by0, bx1, by1;   /* rubber band, content coordinates */
    bool b2_down;
    double b2x, b2y;
    uint32_t b2_serial;
    const char *name_head;
    char ta[64];             /* type-ahead: what was typed, the last key's time */
    double ta_ms;   /* details view: the second column's title (NULL: "Name") */   /* the press's: GNOME grants a menu's grab for a press, not a release */
    size_t vis_first, vis_last;
    ui_layer *layer;          /* rows are drawn here; NULL: on the surface */
    ui_entry *rename;         /* in-place rename, if active */
    size_t rename_i;
    size_t drag_i;            /* the drag button went down on this item */
    double drag_x, drag_y;
    uint32_t drag_serial;
    size_t drop_i;            /* target emphasis, as in OS/2 */
};

/* -------------------------------------------------------------- geometry */

static ui_rect head_rect(ui_container *c)
{
    return (ui_rect){ c->r.x, c->r.y, c->r.w - SBW(c), c->view == UI_VIEW_DETAILS ? HEAD_H : 0 };
}

static bool free_icons(ui_container *c) { return c->view == UI_VIEW_ICON && c->src->item_pos && c->src->items_in; }

static double content_w(ui_container *c)
{
    double w = 0, h = 0;
    if (free_icons(c) && c->src->content_size) c->src->content_size(c->ctx, &w, &h);
    return w + 8;
}

/* A horizontal scroll bar when placed icons reach past the window. */
static bool has_hsb(ui_container *c)
{
    return !c->no_scrollbars && free_icons(c) && content_w(c) > c->r.w - SBW(c);
}

static ui_rect list_rect(ui_container *c)
{
    ui_rect h = head_rect(c);
    double hb = has_hsb(c) ? SB_W : 0;
    return (ui_rect){ c->r.x, c->r.y + h.h, c->r.w - SBW(c), c->r.h - h.h - hb };
}

static ui_rect hsb_rect(ui_container *c)
{
    return (ui_rect){ c->r.x, c->r.y + c->r.h - SB_W, c->r.w - SB_W, SB_W };
}

static double max_hscroll(ui_container *c)
{
    double m = content_w(c) - list_rect(c).w;
    return m > 0 ? floor(m) : 0;
}

static ui_rect sb_rect(ui_container *c) { return (ui_rect){ c->r.x + c->r.w - SBW(c), c->r.y, SBW(c), c->r.h }; }

static size_t cols(ui_container *c)
{
    if (c->view != UI_VIEW_ICON) return 1;
    size_t n = (size_t)(list_rect(c).w / CELL_W);
    return n ? n : 1;
}

static double line_h(ui_container *c) { return c->view == UI_VIEW_ICON ? CELL_H : ROW_H; }

static size_t nlines(ui_container *c) { return (c->count + cols(c) - 1) / cols(c); }

static double content_h(ui_container *c)
{
    if (free_icons(c)) {
        double w = 0, h = 0;
        if (c->src->content_size) c->src->content_size(c->ctx, &w, &h);
        return h + 8;
    }
    return (double)nlines(c) * line_h(c) + (c->view == UI_VIEW_ICON ? 8 : 0);
}

static double max_scroll(ui_container *c)
{
    double m = content_h(c) - list_rect(c).h;
    return m > 0 ? floor(m) : 0;
}

/* Item i's rectangle in content coordinates: x as on the surface, y from
 * the top of the content (independent of scrolling). */
static ui_rect item_crect(ui_container *c, size_t i)
{
    ui_rect l = list_rect(c);
    if (c->view != UI_VIEW_ICON) return (ui_rect){ l.x, (double)i * ROW_H, l.w, ROW_H };
    if (free_icons(c)) {
        double x = 0, y = 0;
        c->src->item_pos(c->ctx, i, &x, &y);
        return (ui_rect){ l.x + 4 + x - c->hscroll, 4 + y, CELL_W, CELL_H };
    }
    size_t k = cols(c), row = i / k, col = i % k;
    return (ui_rect){ l.x + 4 + (double)col * CELL_W, 4 + (double)row * CELL_H, CELL_W, CELL_H };
}

/* ... and in surface coordinates (may be off screen). */
static ui_rect item_rect(ui_container *c, size_t i)
{
    ui_rect r = item_crect(c, i);
    r.y += list_rect(c).y - c->scroll;
    return r;
}

/* The visible part of the content, in content coordinates. */
static ui_rect window_crect(ui_container *c)
{
    ui_rect l = list_rect(c);
    return (ui_rect){ l.x, c->scroll, l.w, l.h };
}

static void columns(ui_container *c, double *x);

/* Where the rename entry sits: over the title (details: the Title column;
 * icons: the label), content coordinates. */
static ui_rect rename_crect(ui_container *c, size_t i)
{
    ui_rect r = item_crect(c, i);
    if (c->view == UI_VIEW_DETAILS) {
        double x[6];
        columns(c, x);
        return (ui_rect){ x[1] - 3, r.y, x[2] - x[1] - 3, ROW_H };
    }
    if (c->view == UI_VIEW_TREE) {
        ui_item it = { 0 };
        c->src->item(c->ctx, i, &it);
        double x = r.x + 4 + it.depth * INDENT + INDENT + 20;
        return (ui_rect){ x - 3, r.y, fmax(120, r.x + r.w - x - 8), ROW_H };
    }
    return (ui_rect){ r.x + 2, r.y + ICON + 6, r.w - 4, ROW_H + 2 };
}

/* Damage content (content coordinates). */
static void damage_content(ui_container *c, ui_rect r)
{
    if (c->layer) { ui_layer_damage(c->layer, r); return; }
    ui_rect l = list_rect(c);
    r.y += l.y - c->scroll;
    /* Only the list's part: a cell half scrolled out must not damage the
     * title bar above it. */
    double x0 = fmax(r.x, l.x), y0 = fmax(r.y, l.y), x1 = fmin(r.x + r.w, l.x + l.w), y1 = fmin(r.y + r.h, l.y + l.h);
    if (x1 > x0 && y1 > y0) ui_surface_damage(c->s, (ui_rect){ x0, y0, x1 - x0, y1 - y0 });
}

static size_t item_at(ui_container *c, double x, double y)
{
    ui_rect l = list_rect(c);
    if (!ui_rect_has(l, x, y)) return SIZE_MAX;
    double cy = y - l.y + c->scroll;
    if (c->view != UI_VIEW_ICON) {
        size_t i = (size_t)(cy / ROW_H);
        return i < c->count ? i : SIZE_MAX;
    }
    if (free_icons(c)) {
        /* The topmost (last drawn) item whose icon or label is under the point. */
        double lx = x - l.x - 4 + c->hscroll, ly = cy - 4;
        size_t found[64], n = c->src->items_in(c->ctx, (ui_rect){ lx - CELL_W, ly - CELL_H, 2 * CELL_W, 2 * CELL_H }, found, 64);
        size_t hit = SIZE_MAX;
        for (size_t k = 0; k < n; k++) {
            double ix, iy;
            c->src->item_pos(c->ctx, found[k], &ix, &iy);
            double ox = lx - ix, oy = ly - iy;
            if (ox >= 8 && ox <= CELL_W - 8 && oy >= 0 && oy <= CELL_H - 8 && (hit == SIZE_MAX || found[k] > hit)) hit = found[k];
        }
        return hit < c->count ? hit : SIZE_MAX;
    }
    double cx = x - l.x - 4;
    cy -= 4;
    if (cx < 0 || cy < 0) return SIZE_MAX;
    size_t col = (size_t)(cx / CELL_W), row = (size_t)(cy / CELL_H);
    if (col >= cols(c)) return SIZE_MAX;
    /* Only the icon and its label count, not the empty part of the cell. */
    double ox = cx - (double)col * CELL_W, oy = cy - (double)row * CELL_H;
    if (ox < 8 || ox > CELL_W - 8 || oy > CELL_H - 8) return SIZE_MAX;
    size_t i = row * cols(c) + col;
    return i < c->count ? i : SIZE_MAX;
}

static void visible_range(ui_container *c, size_t *first, size_t *last)
{
    ui_rect l = list_rect(c);
    size_t k = cols(c);
    size_t r0 = (size_t)(c->scroll / line_h(c)), r1 = (size_t)((c->scroll + l.h) / line_h(c));
    *first = r0 * k;
    *last = (r1 + 1) * k;
    if (*last > c->count) *last = c->count ? c->count - 1 : 0;
}

static void report_visible(ui_container *c)
{
    if (!c->count || !c->src->visible) return;
    if (free_icons(c)) {
        ui_rect l = list_rect(c);
        size_t found[4096], n = c->src->items_in(c->ctx, (ui_rect){ c->hscroll - 4, c->scroll - 4, l.w, l.h }, found, 4096);
        for (size_t k = 0; k < n; k++) c->src->visible(c->ctx, found[k], found[k]);   /* the source groups them by page */
        c->vis_first = c->vis_last = SIZE_MAX;
        return;
    }
    size_t f, l;
    visible_range(c, &f, &l);
    if (f == c->vis_first && l == c->vis_last) return;
    c->vis_first = f;
    c->vis_last = l;
    c->src->visible(c->ctx, f, l);
}

/* ------------------------------------------------------------- selection */

static bool sel_get(ui_container *c, size_t i) { return i / 8 < c->selcap && (c->sel[i / 8] >> (i % 8)) & 1; }

static void sel_set(ui_container *c, size_t i, bool on)
{
    if (i / 8 >= c->selcap) {
        if (!on) return;
        size_t n = c->selcap ? c->selcap : 1024;
        while (n <= i / 8) n *= 2;
        c->sel = realloc(c->sel, n);
        memset(c->sel + c->selcap, 0, n - c->selcap);
        c->selcap = n;
    }
    bool was = sel_get(c, i);
    if (was == on) return;
    if (on) { c->sel[i / 8] |= (uint8_t)(1u << (i % 8)); c->nsel++; }
    else { c->sel[i / 8] &= (uint8_t)~(1u << (i % 8)); c->nsel--; }
}

static void damage_item(ui_container *c, size_t i)
{
    ui_rect r = item_crect(c, i);
    if (ui_rect_meets(r, window_crect(c))) damage_content(c, r);
}

static void damage_list(ui_container *c) { damage_content(c, window_crect(c)); }

static void sel_clear(ui_container *c)
{
    if (!c->nsel) return;
    memset(c->sel, 0, c->selcap);
    c->nsel = 0;
    damage_list(c);
}

static void sel_range(ui_container *c, size_t a, size_t b, bool on)
{
    if (a > b) { size_t t = a; a = b; b = t; }
    for (size_t i = a; i <= b && i < c->count; i++) sel_set(c, i, on);
    damage_list(c);
}

size_t ui_container_selected_count(ui_container *c) { return c->nsel; }
bool ui_container_is_selected(ui_container *c, size_t i) { return sel_get(c, i); }

/* -------------------------------------------------------------- scrolling */

static void damage_sb(ui_container *c) { ui_surface_damage(c->s, sb_rect(c)); }

static void scroll_to(ui_container *c, double y)
{
    double m = max_scroll(c);
    y = floor(y < 0 ? 0 : y > m ? m : y);
    double d = y - c->scroll;
    if (d == 0) return;
    c->scroll = y;
    if (c->layer) ui_layer_set_offset(c->layer, y);
    else ui_surface_scroll(c->s, list_rect(c), d);
    damage_sb(c);
    report_visible(c);
}

static void damage_hsb(ui_container *c) { if (has_hsb(c)) ui_surface_damage(c->s, hsb_rect(c)); }

static void hscroll_to(ui_container *c, double x)
{
    double m = max_hscroll(c);
    x = floor(x < 0 ? 0 : x > m ? m : x);
    if (x == c->hscroll) return;
    c->hscroll = x;
    /* The layer scrolls vertically only; a sideways move redraws the list. */
    if (c->layer) ui_layer_damage_all(c->layer);
    else ui_surface_damage(c->s, list_rect(c));
    damage_hsb(c);
    report_visible(c);
}

void ui_container_scroll_by(ui_container *c, double dy) { scroll_to(c, c->scroll + dy); }
void ui_container_scroll_to_fraction(ui_container *c, double f) { scroll_to(c, f * max_scroll(c)); }

static void ensure_visible(ui_container *c, size_t i)
{
    ui_rect r = item_rect(c, i), l = list_rect(c);
    if (free_icons(c)) {
        if (r.x < l.x) hscroll_to(c, c->hscroll - (l.x - r.x) - 4);
        else if (r.x + r.w > l.x + l.w) hscroll_to(c, c->hscroll + (r.x + r.w - (l.x + l.w)) + 4);
        r = item_rect(c, i);
    }
    if (r.y < l.y) scroll_to(c, c->scroll - (l.y - r.y) - (c->view == UI_VIEW_ICON ? 4 : 0));
    else if (r.y + r.h > l.y + l.h) scroll_to(c, c->scroll + (r.y + r.h - (l.y + l.h)) + (c->view == UI_VIEW_ICON ? 4 : 0));
}

static void thumb(ui_container *c, double *y, double *h)
{
    ui_rect t = sb_rect(c);
    t.y += SB_W;
    t.h -= 2 * SB_W;
    double ch = content_h(c), vh = list_rect(c).h;
    *h = ch > vh ? fmax(20, t.h * vh / ch) : t.h;
    if (*h > t.h) *h = t.h;
    double m = max_scroll(c);
    *y = t.y + (m > 0 ? (t.h - *h) * c->scroll / m : 0);
}

/* ------------------------------------------------------------------ icons */

/* Simple vector icons of our own; real icons come with the icon objects. */
static void draw_icon_paths(cairo_t *cr, ui_kind k, double x, double y, double sz);
static void shadow_mark(cairo_t *cr, double x, double y, double size);

/* In-use emphasis (OS/2): diagonal hatching behind the icon of an object
 * whose window is open, or a file a program has open. */
static void in_use(cairo_t *cr, double x, double y, double size)
{
    cairo_save(cr);
    cairo_rectangle(cr, x, y, size, size);
    cairo_clip(cr);
    ui_set_color(cr, ui_scheme_now.shadow);
    cairo_set_line_width(cr, 1);
    for (double d = -size; d < size; d += 4) {
        cairo_move_to(cr, x + d, y + size);
        cairo_line_to(cr, x + d + size, y);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void open_folder_icon(cairo_t *cr, double x, double y, double sz)
{
    double u = sz / 32.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, u, u);
    cairo_set_line_width(cr, 1.2);
    /* the back, then the front flap opened downwards */
    cairo_move_to(cr, 2, 8); cairo_line_to(cr, 12, 8); cairo_line_to(cr, 15, 11); cairo_line_to(cr, 28, 11);
    cairo_line_to(cr, 28, 27); cairo_line_to(cr, 2, 27); cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.80, 0.64, 0.20); cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.45, 0.33, 0.05); cairo_stroke(cr);
    cairo_move_to(cr, 6, 15); cairo_line_to(cr, 31, 15); cairo_line_to(cr, 27, 27); cairo_line_to(cr, 2, 27); cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.97, 0.86, 0.45); cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.45, 0.33, 0.05); cairo_stroke(cr);
    cairo_restore(cr);
}

/* Rendered once per kind, size and scale, then painted as images. */
static struct { ui_kind kind; int size, scale; cairo_surface_t *img; } icon_cache[32];

static void draw_icon(cairo_t *cr, ui_kind k, double x, double y, double sz)
{
    double sx, sy;
    cairo_surface_get_device_scale(cairo_get_target(cr), &sx, &sy);
    int scale = (int)sx, size = (int)sz;
    cairo_surface_t *img = NULL;
    for (size_t i = 0; i < sizeof icon_cache / sizeof icon_cache[0] && !img; i++) {
        if (icon_cache[i].img && icon_cache[i].kind == k && icon_cache[i].size == size && icon_cache[i].scale == scale)
            img = icon_cache[i].img;
        else if (!icon_cache[i].img) {
            img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size * scale, size * scale);
            cairo_surface_set_device_scale(img, scale, scale);
            cairo_t *ic = cairo_create(img);
            draw_icon_paths(ic, k, 0, 0, sz);
            cairo_destroy(ic);
            icon_cache[i].kind = k;
            icon_cache[i].size = size;
            icon_cache[i].scale = scale;
            icon_cache[i].img = img;
        }
    }
    if (!img) { draw_icon_paths(cr, k, x, y, sz); return; }
    cairo_set_source_surface(cr, img, x, y);
    cairo_paint(cr);
}

static void draw_icon_paths(cairo_t *cr, ui_kind k, double x, double y, double sz)
{
    double u = sz / 32.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, u, u);
    cairo_set_line_width(cr, 1.2);
    switch (k) {
    case UI_KIND_FOLDER:
        cairo_move_to(cr, 2, 8); cairo_line_to(cr, 12, 8); cairo_line_to(cr, 15, 11); cairo_line_to(cr, 30, 11);
        cairo_line_to(cr, 30, 27); cairo_line_to(cr, 2, 27); cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.93, 0.78, 0.30); cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.45, 0.33, 0.05); cairo_stroke(cr);
        cairo_move_to(cr, 2, 14); cairo_line_to(cr, 30, 14);
        cairo_stroke(cr);
        break;
    case UI_KIND_PROGRAM:
        cairo_rectangle(cr, 3, 6, 26, 21);
        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95); cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2); cairo_stroke(cr);
        cairo_rectangle(cr, 3, 6, 26, 5);
        cairo_set_source_rgb(cr, 0.1, 0.2, 0.55); cairo_fill(cr);
        break;
    default:
        cairo_move_to(cr, 7, 3); cairo_line_to(cr, 20, 3); cairo_line_to(cr, 26, 9);
        cairo_line_to(cr, 26, 29); cairo_line_to(cr, 7, 29); cairo_close_path(cr);
        cairo_set_source_rgb(cr, 1, 1, 1); cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.3); cairo_stroke(cr);
        cairo_move_to(cr, 20, 3); cairo_line_to(cr, 20, 9); cairo_line_to(cr, 26, 9);
        cairo_stroke(cr);
        for (int l = 0; l < 4; l++) { cairo_move_to(cr, 10, 14 + l * 4); cairo_line_to(cr, 23, 14 + l * 4); }
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6); cairo_stroke(cr);
        break;
    }
    cairo_restore(cr);
}

/* ----------------------------------------------------------------- rename */

static void damage_rename(ui_container *c)
{
    if (c->rename) damage_content(c, rename_crect(c, c->rename_i));
}

static void end_rename(ui_container *c, bool apply)
{
    if (!c->rename) return;
    damage_rename(c);
    damage_item(c, c->rename_i);
    char *name = apply ? strdup(ui_entry_text(c->rename)) : NULL;
    size_t i = c->rename_i;
    ui_entry_free(c->rename);
    c->rename = NULL;
    if (name && *name && c->src->rename) c->src->rename(c->ctx, i, name);
    free(name);
}

void ui_container_begin_rename(ui_container *c, size_t i)
{
    if (i >= c->count || !c->src->rename) return;
    end_rename(c, true);
    ui_item it = { 0 };
    c->src->item(c->ctx, i, &it);
    if (!it.loaded) return;
    c->rename = ui_entry_new(it.title ? it.title : it.name);
    if (it.kind == UI_KIND_FOLDER) ui_entry_select_all(c->rename);
    else ui_entry_select_stem(c->rename);
    c->rename_i = i;
    ensure_visible(c, i);
    damage_rename(c);
}

bool ui_container_renaming(ui_container *c) { return c->rename != NULL; }
size_t ui_container_rename_item(ui_container *c) { return c->rename ? c->rename_i : SIZE_MAX; }

void ui_container_rename_moved(ui_container *c, size_t i)
{
    if (!c->rename) return;
    if (i >= c->count) { end_rename(c, false); return; }
    c->rename_i = i;
    damage_rename(c);
}

/* The entry's rectangle in surface coordinates, for pointer events. */
static ui_rect rename_rect(ui_container *c)
{
    ui_rect r = rename_crect(c, c->rename_i);
    r.y += list_rect(c).y - c->scroll;
    return r;
}

/* ---------------------------------------------------------------- drawing */

static const char *heads[] = { "", "Title", "Name", "Size", "Last write" };

void ui_container_set_name_head(ui_container *c, const char *title) { c->name_head = title; }

static void columns(ui_container *c, double *x)
{
    ui_rect l = list_rect(c);
    x[0] = l.x + 4;
    x[1] = l.x + 26;
    x[4] = l.x + l.w - 130;
    x[3] = x[4] - 90;
    x[2] = x[1] + (x[3] - x[1]) * 0.55;
    x[5] = l.x + l.w;
}


/* An item's icon: in use, it has the hatching behind it, and a folder is
 * drawn open (OS/2). */
static void item_icon(cairo_t *cr, const ui_item *it, double x, double y, double sz);

/* An object's icon anywhere (the LaunchPad, drawers): as in a folder. */
void ui_draw_item_icon(cairo_t *cr, const ui_item *it, double x, double y, double size)
{
    item_icon(cr, it, x, y, size);
    if (it->shadow) shadow_mark(cr, x, y, size);
}

/* A file-system link's mark: a green corner, bottom left. */
static void link_mark(cairo_t *cr, double x, double y, double sz)
{
    double u = sz / 32.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, u, u);
    cairo_move_to(cr, 1, 31); cairo_line_to(cr, 1, 21); cairo_line_to(cr, 11, 31); cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.1, 0.5, 0.1); cairo_fill(cr);
    cairo_restore(cr);
}

/* A template (OS/2): the object's icon on a pad of paper. */
static void template_pad(cairo_t *cr, double x, double y, double sz)
{
    double u = sz / 32.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, u, u);
    cairo_set_line_width(cr, 1);
    for (int k = 2; k >= 0; k--) {   /* the sheets below, then the top one */
        cairo_rectangle(cr, 3.5 + k, 4.5 + k, 24, 24);
        cairo_set_source_rgb(cr, k ? 0.85 : 0.98, k ? 0.85 : 0.98, k ? 0.82 : 0.94);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
        cairo_stroke(cr);
    }
    /* the binding at the top */
    cairo_rectangle(cr, 3, 2, 25, 4);
    cairo_set_source_rgb(cr, 0.45, 0.25, 0.1);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void item_icon(cairo_t *cr, const ui_item *it, double x, double y, double sz)
{
    if (it->is_template) {
        template_pad(cr, x, y, sz);
        /* the object it makes, smaller, on the top sheet */
        ui_item in = *it;
        in.is_template = false;
        item_icon(cr, &in, x + sz * 0.2, y + sz * 0.25, sz * 0.6);
        return;
    }
    if (it->in_use) in_use(cr, x, y, sz);
    if (it->image && cairo_surface_status(it->image) == CAIRO_STATUS_SUCCESS) {
        double iw = cairo_image_surface_get_width(it->image), ih = cairo_image_surface_get_height(it->image);
        double k = sz / (iw > ih ? iw : ih);
        cairo_save(cr);
        cairo_translate(cr, x + (sz - iw * k) / 2, y + (sz - ih * k) / 2);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, it->image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else if (it->in_use && it->kind == UI_KIND_FOLDER) open_folder_icon(cr, x, y, sz);
    else draw_icon(cr, it->kind == UI_KIND_LINK ? UI_KIND_FILE : it->kind, x, y, sz);
    if (it->link || it->kind == UI_KIND_LINK) link_mark(cr, x, y, sz);
}

static void draw_details_row(ui_container *c, cairo_t *cr, size_t i, ui_rect r, double *x)
{
    const ui_scheme *k = &ui_scheme_now;
    bool sel = sel_get(c, i);
    ui_fill(cr, r, sel ? k->selection : (i % 2 ? k->alt_row : k->client_bg));
    ui_item it = { 0 };
    c->src->item(c->ctx, i, &it);
    double base = r.y + (ROW_H + ui_text_ascent()) / 2 - 1;
    if (!it.loaded) {
        ui_fill(cr, (ui_rect){ x[1], r.y + 6, 140, 8 }, k->placeholder);
    } else {
        item_icon(cr, &it, x[0], r.y + 2, 16);
        if (it.shadow) shadow_mark(cr, x[0], r.y + 2, 16);
        ui_set_color(cr, sel ? k->selection_text : it.shadow ? k->shadow_text : k->text);
        ui_text_draw(cr, UI_FONT_REGULAR, x[1], base, it.title ? it.title : "", x[2] - x[1] - 6);
        ui_set_color(cr, sel ? k->selection_text : k->text);
        ui_text_draw(cr, UI_FONT_REGULAR, x[2], base, it.name ? it.name : "", x[3] - x[2] - 6);
        if (it.detailed) {
            char sz[32], dt[32];
            if (it.kind == UI_KIND_FOLDER) sz[0] = 0;
            else snprintf(sz, sizeof sz, "%lld", (long long)it.size);
            time_t t = (time_t)it.mtime;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(dt, sizeof dt, "%Y-%m-%d %H:%M", &tm);
            double w = ui_text_width(UI_FONT_REGULAR, sz);
            ui_text_draw(cr, UI_FONT_REGULAR, x[4] - 10 - w, base, sz, 0);   /* sizes align right */
            ui_text_draw(cr, UI_FONT_REGULAR, x[4], base, dt, x[5] - x[4] - 4);
        } else {
            ui_fill(cr, (ui_rect){ x[4] - 60, r.y + 6, 50, 8 }, k->placeholder);
            ui_fill(cr, (ui_rect){ x[4], r.y + 6, 100, 8 }, k->placeholder);
        }
    }
    if (c->focused && c->has_cursor && c->cursor == i) {
        ui_set_color(cr, sel ? k->selection_text : k->cursor);
        cairo_set_line_width(cr, 1);
        double dash = 1;
        cairo_set_dash(cr, &dash, 1, 0);
        cairo_rectangle(cr, r.x + 1.5, r.y + 0.5, r.w - 3, r.h - 1);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
}

/* A tree row: joining lines, the expander (+/-), a small icon, the title. */
static void draw_tree_row(ui_container *c, cairo_t *cr, size_t i, ui_rect r)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_item it = { 0 };
    c->src->item(c->ctx, i, &it);
    bool sel = sel_get(c, i);
    double x0 = r.x + 4, cy = floor(r.y + ROW_H / 2) + 0.5;
    if (!it.loaded) {
        ui_fill(cr, (ui_rect){ x0 + 30, r.y + 6, 120, 8 }, k->placeholder);
        return;
    }
    /* Lines: a vertical for each open ancestor level, then this item's
     * branch (dotted, as in OS/2). */
    ui_set_color(cr, k->shadow);
    double dash = 1;
    cairo_set_dash(cr, &dash, 1, 0);
    cairo_set_line_width(cr, 1);
    for (int d = 0; d < it.depth; d++) {
        double lx = floor(x0 + d * INDENT + INDENT / 2) + 0.5;
        if (d == it.depth - 1) {
            cairo_move_to(cr, lx, r.y);
            cairo_line_to(cr, lx, it.last_child ? cy : r.y + r.h);
            cairo_move_to(cr, lx, cy);
            cairo_line_to(cr, lx + INDENT / 2 + 2, cy);
        } else if (it.open_levels & (1u << (d + 1))) {
            cairo_move_to(cr, lx, r.y);
            cairo_line_to(cr, lx, r.y + r.h);
        }
    }
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
    double ex = x0 + it.depth * INDENT;
    if (it.expandable) {
        ui_rect b = { floor(ex + INDENT / 2 - 5), floor(cy - 5), 10, 10 };
        ui_fill(cr, b, k->client_bg);
        ui_set_color(cr, k->shadow);
        cairo_rectangle(cr, b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1);
        cairo_stroke(cr);
        ui_set_color(cr, k->text);
        cairo_move_to(cr, b.x + 2, cy);
        cairo_line_to(cr, b.x + b.w - 2, cy);
        if (!it.expanded) {
            cairo_move_to(cr, b.x + b.w / 2, b.y + 2);
            cairo_line_to(cr, b.x + b.w / 2, b.y + b.h - 2);
        }
        cairo_stroke(cr);
    }
    double ix = ex + INDENT;
    item_icon(cr, &it, ix, r.y + 2, 16);
    const char *t = it.title ? it.title : "";
    double tx = ix + 20, tw = ui_text_width(UI_FONT_REGULAR, t), maxw = r.x + r.w - tx - 4;
    if (tw > maxw) tw = maxw;
    if (sel) ui_fill(cr, (ui_rect){ tx - 2, r.y + 1, tw + 4, ROW_H - 2 }, k->selection);
    ui_set_color(cr, sel ? k->selection_text : k->text);
    ui_text_draw(cr, UI_FONT_REGULAR, tx, r.y + (ROW_H + ui_text_ascent()) / 2 - 1, t, maxw);
    if (c->focused && c->has_cursor && c->cursor == i) {
        ui_set_color(cr, sel ? k->selection_text : k->cursor);
        cairo_set_dash(cr, &dash, 1, 0);
        cairo_rectangle(cr, tx - 2.5, r.y + 1.5, tw + 5, ROW_H - 3);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
}

/* Where item i's expander is (surface coordinates), for clicks. */
static bool on_expander(ui_container *c, size_t i, double x)
{
    if (c->view != UI_VIEW_TREE) return false;
    ui_item it = { 0 };
    c->src->item(c->ctx, i, &it);
    if (!it.expandable) return false;
    double ex = list_rect(c).x + 4 + it.depth * INDENT;
    return x >= ex && x < ex + INDENT;
}

/* The mark on a shadow's icon: a small arrow in the corner. */
static void shadow_mark(cairo_t *cr, double x, double y, double size)
{
    double u = size / 32.0;
    cairo_save(cr);
    cairo_translate(cr, x, y + size - 12 * u);
    cairo_scale(cr, u, u);
    cairo_rectangle(cr, 0, 0, 12, 12);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_move_to(cr, 3, 9);
    cairo_line_to(cr, 9, 3);
    cairo_move_to(cr, 5, 3);
    cairo_line_to(cr, 9, 3);
    cairo_line_to(cr, 9, 7);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_icon_cell(ui_container *c, cairo_t *cr, size_t i, ui_rect r)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_item it = { 0 };
    c->src->item(c->ctx, i, &it);
    double ix = r.x + (r.w - ICON) / 2, iy = r.y + 4;
    if (!it.loaded) {
        ui_fill(cr, (ui_rect){ ix + 4, iy + 4, ICON - 8, ICON - 8 }, k->placeholder);
        ui_fill(cr, (ui_rect){ r.x + 20, r.y + ICON + 12, r.w - 40, 8 }, k->placeholder);
        return;
    }
    item_icon(cr, &it, ix, iy, ICON);
    if (it.shadow) shadow_mark(cr, ix, iy, ICON);
    const char *t = it.title ? it.title : "";
    double tw = ui_text_width(UI_FONT_REGULAR, t), maxw = r.w - 8;
    if (tw > maxw) tw = maxw;
    double tx = r.x + (r.w - tw) / 2, ty = r.y + ICON + 8;
    bool sel = sel_get(c, i);
    if (sel) ui_fill(cr, (ui_rect){ tx - 2, ty, tw + 4, ROW_H - 2 }, k->selection);
    double by = ty + (ROW_H + ui_text_ascent()) / 2 - 2;
    if (c->backdrop && !sel) {
        /* on a picture: light text with a dark shadow reads on anything */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
        ui_text_draw(cr, UI_FONT_REGULAR, tx + 1, by + 1, t, maxw);
        if (it.shadow) cairo_set_source_rgb(cr, 0.75, 0.85, 1.0);
        else cairo_set_source_rgb(cr, 1, 1, 1);
    } else {
        ui_set_color(cr, sel ? k->selection_text : it.shadow ? k->shadow_text : k->text);
    }
    ui_text_draw(cr, UI_FONT_REGULAR, tx, by, t, maxw);
    if (c->focused && c->has_cursor && c->cursor == i) {
        ui_set_color(cr, sel ? k->selection_text : k->cursor);
        double dash = 1;
        cairo_set_dash(cr, &dash, 1, 0);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, tx - 2.5, ty + 0.5, tw + 5, ROW_H - 3);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
}

static void draw_scrollbar(ui_container *c, cairo_t *cr)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect sb = sb_rect(c);
    ui_fill(cr, sb, k->shadow);
    ui_fill(cr, (ui_rect){ sb.x + 1, sb.y + 1, sb.w - 2, sb.h - 2 }, (ui_color){ k->face.r * 0.85, k->face.g * 0.85, k->face.b * 0.85, 1 });
    for (int a = 0; a < 2; a++) {
        ui_rect b = { sb.x, a ? sb.y + sb.h - SB_W : sb.y, SB_W, SB_W };
        bool down = c->arrow_down == (a ? 1 : -1);
        ui_fill(cr, b, k->face);
        ui_bevel(cr, b, !down);
        ui_set_color(cr, k->text);
        double cx = b.x + SB_W / 2 + (down ? 1 : 0), cy = b.y + SB_W / 2 + (down ? 1 : 0);
        if (a) { cairo_move_to(cr, cx - 4, cy - 2); cairo_line_to(cr, cx + 4, cy - 2); cairo_line_to(cr, cx, cy + 2); }
        else { cairo_move_to(cr, cx - 4, cy + 2); cairo_line_to(cr, cx + 4, cy + 2); cairo_line_to(cr, cx, cy - 2); }
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    double ty, th;
    thumb(c, &ty, &th);
    ui_rect t = { sb.x, ty, SB_W, th };
    ui_fill(cr, t, k->face);
    ui_bevel(cr, t, true);
}

/* Rows, band and cursor that meet `clip`, in content coordinates. */
static void draw_content(ui_container *c, cairo_t *cr, ui_rect clip)
{
    const ui_scheme *k = &ui_scheme_now;
    double x[6];
    columns(c, x);
    if (c->backdrop) c->backdrop(c->backdrop_ctx, cr, clip);
    else ui_fill(cr, clip, k->client_bg);
    double lh = line_h(c), off = c->view == UI_VIEW_ICON ? 4 : 0;
    double r0 = floor((clip.y - off) / lh), r1 = floor((clip.y + clip.h - off) / lh);
    size_t kcols = cols(c);
    if (free_icons(c)) {
        ui_rect l = list_rect(c);
        ui_rect q = { clip.x - l.x - 4 + c->hscroll, clip.y - 4, clip.w, clip.h };
        size_t found[8192], n = c->src->items_in(c->ctx, q, found, 8192);
        /* In index order, so overlapping icons stack the same way each time. */
        for (size_t a = 1; a < n; a++)
            for (size_t b = a; b > 0 && found[b - 1] > found[b]; b--) { size_t t = found[b]; found[b] = found[b - 1]; found[b - 1] = t; }
        for (size_t k = 0; k < n; k++)
            if (found[k] < c->count) draw_icon_cell(c, cr, found[k], item_crect(c, found[k]));
        r0 = 1;
        r1 = 0;   /* nothing for the grid loop below */
    }
    for (double row = r0 < 0 ? 0 : r0; row <= r1; row++) {
        for (size_t col = 0; col < kcols; col++) {
            size_t i = (size_t)row * kcols + col;
            if (i >= c->count) break;
            ui_rect r = item_crect(c, i);
            if (!ui_rect_meets(r, clip)) continue;
            if (c->view == UI_VIEW_DETAILS) draw_details_row(c, cr, i, r, x);
            else if (c->view == UI_VIEW_TREE) draw_tree_row(c, cr, i, r);
            else draw_icon_cell(c, cr, i, r);
        }
    }
    if (c->rename && c->rename_i < c->count) {
        ui_rect er = rename_crect(c, c->rename_i);
        if (ui_rect_meets(er, clip)) ui_entry_draw(c->rename, cr, er, true);
    }
    if (c->drop_i < c->count) {
        ui_rect r = item_crect(c, c->drop_i);
        if (ui_rect_meets(r, clip)) {
            ui_set_color(cr, k->text);
            cairo_set_line_width(cr, 2);
            cairo_rectangle(cr, r.x + 2, r.y + 1, r.w - 4, r.h - 2);
            cairo_stroke(cr);
        }
    }
    if (c->band) {
        double bx = fmin(c->bx0, c->bx1), by = fmin(c->by0, c->by1);
        double bw = fabs(c->bx1 - c->bx0), bh = fabs(c->by1 - c->by0);
        ui_set_color(cr, k->text);
        double dash = 2;
        cairo_set_dash(cr, &dash, 1, 0);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, floor(bx) + 0.5, floor(by) + 0.5, floor(bw), floor(bh));
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
}

static void layer_draw(void *ctx, cairo_t *cr, ui_rect clip) { draw_content(ctx, cr, clip); }

void ui_container_draw(ui_container *c, cairo_t *cr, ui_rect clip)
{
    const ui_scheme *k = &ui_scheme_now;
    double x[6];
    columns(c, x);
    ui_rect h = head_rect(c);
    if (h.h && ui_rect_meets(clip, h)) {
        ui_fill(cr, h, k->face);
        for (int i = 1; i < 5; i++) {
            ui_rect cell = { x[i] - 4, h.y, x[i + 1] - x[i], h.h };
            ui_bevel(cr, cell, true);
            ui_set_color(cr, k->text);
            ui_text_draw(cr, UI_FONT_REGULAR, x[i], h.y + (h.h + ui_text_ascent()) / 2 - 1, i == 2 && c->name_head ? c->name_head : heads[i], cell.w - 8);
        }
    }
    ui_rect l = list_rect(c);
    if (!c->layer && ui_rect_meets(clip, l)) {
        /* No layer (the compositor lacks it): rows drawn on the surface. */
        cairo_save(cr);
        cairo_rectangle(cr, l.x, l.y, l.w, l.h);
        cairo_clip(cr);
        cairo_translate(cr, 0, l.y - c->scroll);
        double top = fmax(clip.y, l.y), bot = fmin(clip.y + clip.h, l.y + l.h);
        draw_content(c, cr, (ui_rect){ l.x, top - l.y + c->scroll, l.w, bot - top });
        cairo_restore(cr);
    }
    if (ui_rect_meets(clip, sb_rect(c))) draw_scrollbar(c, cr);
    if (has_hsb(c) && ui_rect_meets(clip, hsb_rect(c))) {
        ui_rect h = hsb_rect(c);
        ui_fill(cr, h, (ui_color){ k->face.r * 0.85, k->face.g * 0.85, k->face.b * 0.85, 1 });
        double cw = content_w(c), vw = list_rect(c).w, m = max_hscroll(c);
        double tw = fmax(20, h.w * vw / cw), tx = h.x + (m > 0 ? (h.w - tw) * c->hscroll / m : 0);
        ui_fill(cr, (ui_rect){ tx, h.y, tw, h.h }, k->face);
        ui_bevel(cr, (ui_rect){ tx, h.y, tw, h.h }, true);
    }
}

/* ------------------------------------------------------------------ input */

static void move_cursor(ui_container *c, size_t to, unsigned mods)
{
    if (!c->count) return;
    if (to >= c->count) to = c->count - 1;
    size_t old = c->cursor;
    bool had = c->has_cursor;
    c->cursor = to;
    c->has_cursor = true;
    if (mods & UI_MOD_SHIFT) {
        sel_clear(c);
        sel_range(c, c->anchor, to, true);
    } else if (!(mods & UI_MOD_CTRL)) {
        sel_clear(c);
        sel_set(c, to, true);
        c->anchor = to;
    }
    if (had) damage_item(c, old);
    damage_item(c, to);
    ensure_visible(c, to);
}

static void band_update(ui_container *c)
{
    ui_rect l = list_rect(c);
    double x0 = fmin(c->bx0, c->bx1) - l.x - 4, x1 = fmax(c->bx0, c->bx1) - l.x - 4;
    double y0 = fmin(c->by0, c->by1) - 4, y1 = fmax(c->by0, c->by1) - 4;
    sel_clear(c);
    if (free_icons(c)) {
        static size_t found[65536];
        size_t n = c->src->items_in(c->ctx, (ui_rect){ x0 + c->hscroll, y0, x1 - x0, y1 - y0 }, found, 65536);
        for (size_t i = 0; i < n; i++) sel_set(c, found[i], true);
        damage_list(c);
        return;
    }
    size_t k = cols(c);
    if (x1 < 0 || y1 < 0) { damage_list(c); return; }
    size_t c0 = (size_t)fmax(0, x0 / CELL_W), c1 = (size_t)(x1 / CELL_W);
    size_t r0 = (size_t)fmax(0, y0 / CELL_H), r1 = (size_t)(y1 / CELL_H);
    if (c1 >= k) c1 = k - 1;
    for (size_t r = r0; r <= r1; r++)
        for (size_t col = c0; col <= c1; col++) {
            size_t i = r * k + col;
            if (i < c->count) sel_set(c, i, true);
        }
    damage_list(c);
}

bool ui_container_pointer(ui_container *c, const ui_pointer_event *e)
{
    ui_rect sb = sb_rect(c), l = list_rect(c);
    if (c->rename) {
        ui_rect er = rename_rect(c);
        if (e->kind == UI_PRESS && ui_rect_has(er, e->x, e->y)) {
            if (ui_entry_pointer(c->rename, e, er) != UI_ENTRY_NONE) damage_rename(c);
            return true;
        }
        if (e->kind == UI_MOTION || e->kind == UI_RELEASE) {
            if (ui_entry_pointer(c->rename, e, er) != UI_ENTRY_NONE) damage_rename(c);
        } else if (e->kind == UI_PRESS) {
            end_rename(c, true);   /* a click elsewhere applies it, as in OS/2 */
        }
    }
    if (e->kind == UI_WHEEL) {
        if ((e->mods & UI_MOD_SHIFT) && has_hsb(c)) hscroll_to(c, c->hscroll + e->wheel);
        else ui_container_scroll_by(c, e->wheel);
        return true;
    }
    if (e->kind == UI_MOTION && c->hthumb_drag) {
        ui_rect h = hsb_rect(c);
        double cw = content_w(c), vw = list_rect(c).w, tw = fmax(20, h.w * vw / cw);
        if (h.w - tw > 0) hscroll_to(c, (e->x - h.x - tw / 2) / (h.w - tw) * max_hscroll(c));
        return true;
    }
    if (e->kind == UI_RELEASE && c->hthumb_drag) { c->hthumb_drag = false; return true; }
    if (e->kind == UI_PRESS && has_hsb(c) && ui_rect_has(hsb_rect(c), e->x, e->y)) {
        c->hthumb_drag = e->button == ui_mouse_button(UI_ACT_SELECT);
        if (c->hthumb_drag) {
            ui_rect h = hsb_rect(c);
            double cw = content_w(c), vw = list_rect(c).w, tw = fmax(20, h.w * vw / cw);
            if (h.w - tw > 0) hscroll_to(c, (e->x - h.x - tw / 2) / (h.w - tw) * max_hscroll(c));
        }
        return true;
    }
    if (e->kind == UI_MOTION) {
        if (c->drag_i != SIZE_MAX && (fabs(e->x - c->drag_x) > 5 || fabs(e->y - c->drag_y) > 5)) {
            size_t i = c->drag_i;
            c->drag_i = SIZE_MAX;
            c->b2_down = false;   /* a drag, not a click: no menu */
            if (c->src->drag) c->src->drag(c->ctx, i, c->drag_serial);
            return true;
        }
        if (c->thumb_drag) {
            double ty, th;
            thumb(c, &ty, &th);
            double range = sb.h - 2 * SB_W - th;
            if (range > 0) scroll_to(c, (e->y - c->thumb_off - (sb.y + SB_W)) / range * max_scroll(c));
            return true;
        }
        if (c->band) {
            c->bx1 = e->x;
            c->by1 = e->y - l.y + c->scroll;
            band_update(c);
            return true;
        }
        return ui_rect_has(c->r, e->x, e->y);
    }
    if (e->kind == UI_RELEASE) {
        if (e->button == ui_mouse_button(UI_ACT_DRAG)) c->drag_i = SIZE_MAX;
        bool was = c->thumb_drag || c->band || c->arrow_down;
        c->thumb_drag = false;
        if (c->arrow_down) { c->arrow_down = 0; damage_sb(c); }
        if (c->band) { c->band = false; damage_list(c); }
        if (e->button == ui_mouse_button(UI_ACT_POPUP) && c->b2_down) {
            c->b2_down = false;
            if (fabs(e->x - c->b2x) < 5 && fabs(e->y - c->b2y) < 5 && c->src->menu) {
                size_t i = item_at(c, e->x, e->y);
                if (i != SIZE_MAX && !sel_get(c, i)) move_cursor(c, i, 0);
                c->src->menu(c->ctx, i, e->x, e->y, c->b2_serial);
            }
            return true;
        }
        return was;
    }
    if (e->kind != UI_PRESS || !ui_rect_has(c->r, e->x, e->y)) return false;
    if (ui_rect_has(sb, e->x, e->y)) {
        if (e->button != ui_mouse_button(UI_ACT_SELECT)) return true;
        double ty, th;
        thumb(c, &ty, &th);
        if (e->y < sb.y + SB_W) { c->arrow_down = -1; ui_container_scroll_by(c, -line_h(c)); damage_sb(c); }
        else if (e->y >= sb.y + sb.h - SB_W) { c->arrow_down = 1; ui_container_scroll_by(c, line_h(c)); damage_sb(c); }
        else if (e->y < ty) ui_container_scroll_by(c, -l.h);
        else if (e->y >= ty + th) ui_container_scroll_by(c, l.h);
        else { c->thumb_drag = true; c->thumb_off = e->y - ty; }
        return true;
    }
    if (!ui_rect_has(l, e->x, e->y)) return true;
    size_t i = item_at(c, e->x, e->y);
    if (i != SIZE_MAX && ui_mouse_is(UI_ACT_DRAG, e) && !(e->mods & UI_MOD_ALT)) {
        c->drag_i = i;
        c->drag_x = e->x;
        c->drag_y = e->y;
        c->drag_serial = e->serial;
    }
    if (i != SIZE_MAX && on_expander(c, i, e->x) && ui_mouse_is(UI_ACT_SELECT, e)) {
        ui_item it = { 0 };
        c->src->item(c->ctx, i, &it);
        move_cursor(c, i, 0);
        if (c->src->expand) c->src->expand(c->ctx, i, !it.expanded);
        return true;
    }
    /* The mapped actions, most specific first (edit title before select,
     * open before select). */
    if (i != SIZE_MAX && ui_mouse_is(UI_ACT_EDIT_TITLE, e)) {
        move_cursor(c, i, 0);
        ui_container_begin_rename(c, i);
        return true;
    }
    if (i != SIZE_MAX && e->clicks == 2 && ui_mouse_is(UI_ACT_OPEN, e)) {
        move_cursor(c, i, 0);
        if (c->src->activate) c->src->activate(c->ctx, i);
        return true;
    }
    if (e->button == ui_mouse_button(UI_ACT_POPUP) && !ui_mouse_is(UI_ACT_SELECT, e)) {
        c->b2_down = true;   /* a click (press and release in place) opens the menu */
        c->b2x = e->x;
        c->b2y = e->y;
        c->b2_serial = e->serial;
        return true;
    }
    if (!ui_mouse_is(UI_ACT_SELECT, e)) return true;
    if (i == SIZE_MAX) {
        if (!(e->mods & UI_MOD_CTRL)) sel_clear(c);
        if (c->view == UI_VIEW_ICON) {
            c->band = true;
            c->bx0 = c->bx1 = e->x;
            c->by0 = c->by1 = e->y - l.y + c->scroll;
        }
        return true;
    }
    if (e->mods & UI_MOD_CTRL) {
        sel_set(c, i, !sel_get(c, i));
        size_t old = c->cursor;
        c->cursor = i;
        c->anchor = i;
        c->has_cursor = true;
        damage_item(c, old);
        damage_item(c, i);
    } else {
        move_cursor(c, i, e->mods & UI_MOD_SHIFT);
    }
    return true;
}

static bool starts_with(const char *s, const char *p)
{
    if (!s) return false;
    for (; *p; s++, p++) {
        unsigned char a = (unsigned char)*s, b = (unsigned char)*p;
        if (!a) return false;
        if (a < 128 && b < 128 ? tolower(a) != tolower(b) : a != b) return false;
    }
    return true;
}

/* Type-ahead: a printable key goes to the next item whose title starts
 * with it (OS/2); keys typed within a second of each other spell the
 * start of a title. The same letter again goes on to the next one. */
static bool type_ahead(ui_container *c, const ui_key_event *e)
{
    unsigned char ch = (unsigned char)e->utf8[0];
    if (ch < 0x20 || ch == 0x7f || (e->mods & (UI_MOD_CTRL | UI_MOD_ALT))) return false;
    if (ch == ' ' && !c->ta[0]) return false;   /* Space selects, unless in the middle of a name */
    double now = ui_now_ms();
    if (now - c->ta_ms > 1000) c->ta[0] = 0;
    c->ta_ms = now;
    size_t l = strlen(c->ta), add = strlen(e->utf8);
    if (l + add < sizeof c->ta) memcpy(c->ta + l, e->utf8, add + 1);
    /* "aaa": the next title starting with "a", each time */
    bool same = true;
    for (size_t i = add; c->ta[i] && same; i++) same = c->ta[i] == c->ta[i % add];
    const char *want = c->ta;
    char one[8];
    if (same && strlen(c->ta) > add) { snprintf(one, sizeof one, "%s", e->utf8); want = one; }
    size_t start = c->has_cursor ? c->cursor + (want == one || !l ? 1 : 0) : 0;
    for (size_t k = 0; k < c->count; k++) {
        size_t i = (start + k) % c->count;
        ui_item it = { 0 };
        c->src->item(c->ctx, i, &it);
        if (!it.loaded) continue;
        if (starts_with(it.title ? it.title : it.name, want)) { move_cursor(c, i, 0); return true; }
    }
    return true;   /* no match: the key is still the type-ahead's */
}

void ui_container_typeahead_reset(ui_container *c) { c->ta[0] = 0; }

bool ui_container_key(ui_container *c, const ui_key_event *e)
{
    if (c->rename) {
        switch (ui_entry_key(c->rename, e)) {
        case UI_ENTRY_COMMIT: end_rename(c, true); break;
        case UI_ENTRY_CANCEL: end_rename(c, false); break;
        case UI_ENTRY_NONE: break;
        default: damage_rename(c); break;
        }
        return true;
    }
    if (!c->count) return false;
    if (e->sym == XKB_KEY_F2) {
        if (c->has_cursor) ui_container_begin_rename(c, c->cursor);
        return true;
    }
    if (c->view == UI_VIEW_TREE && c->has_cursor && c->src->expand) {
        ui_item it = { 0 };
        c->src->item(c->ctx, c->cursor, &it);
        bool right = e->sym == XKB_KEY_Right || e->sym == XKB_KEY_plus || e->sym == XKB_KEY_KP_Add;
        bool left = e->sym == XKB_KEY_Left || e->sym == XKB_KEY_minus || e->sym == XKB_KEY_KP_Subtract;
        if (right) {
            if (it.expandable && !it.expanded) c->src->expand(c->ctx, c->cursor, true);
            else if (it.expanded && e->sym == XKB_KEY_Right) move_cursor(c, c->cursor + 1, 0);   /* first child */
            return true;
        }
        if (left) {
            if (it.expanded) { c->src->expand(c->ctx, c->cursor, false); return true; }
            if (e->sym == XKB_KEY_Left && it.depth > 0)   /* to the parent */
                for (size_t k = c->cursor; k-- > 0;) {
                    ui_item p = { 0 };
                    c->src->item(c->ctx, k, &p);
                    if (p.depth < it.depth) { move_cursor(c, k, 0); break; }
                }
            return true;
        }
    }
    size_t cur = c->has_cursor ? c->cursor : 0, k = cols(c);
    size_t page = (size_t)(list_rect(c).h / line_h(c)) * k;
    if (page < k) page = k;
    switch (e->sym) {
    case XKB_KEY_Down: case XKB_KEY_Up: case XKB_KEY_Left: case XKB_KEY_Right:
        if (free_icons(c) && c->has_cursor) {
            int dx = e->sym == XKB_KEY_Right ? 1 : e->sym == XKB_KEY_Left ? -1 : 0;
            int dy = e->sym == XKB_KEY_Down ? 1 : e->sym == XKB_KEY_Up ? -1 : 0;
            double x0, y0;
            c->src->item_pos(c->ctx, cur, &x0, &y0);
            size_t best = SIZE_MAX;
            double bestd = 1e18;
            for (double reach = 2; reach <= 64 && best == SIZE_MAX; reach *= 2) {
                ui_rect q = { x0 - reach * CELL_W, y0 - reach * CELL_H, 2 * reach * CELL_W + CELL_W, 2 * reach * CELL_H + CELL_H };
                size_t found[4096], n = c->src->items_in(c->ctx, q, found, 4096);
                for (size_t a = 0; a < n; a++) {
                    if (found[a] == cur) continue;
                    double x, y;
                    c->src->item_pos(c->ctx, found[a], &x, &y);
                    double ax = (x - x0) * dx + (y - y0) * dy;            /* along the arrow */
                    double off = fabs((x - x0) * dy) + fabs((y - y0) * dx);   /* to the side */
                    if (ax <= 0) continue;
                    double d = ax + 2 * off;
                    if (d < bestd) { bestd = d; best = found[a]; }
                }
            }
            if (best != SIZE_MAX) move_cursor(c, best, e->mods);
            return true;
        }
        if (e->sym == XKB_KEY_Down) { move_cursor(c, c->has_cursor ? cur + k : 0, e->mods); return true; }
        if (e->sym == XKB_KEY_Up) { move_cursor(c, cur >= k ? cur - k : 0, e->mods); return true; }
        if (c->view != UI_VIEW_ICON) return false;
        move_cursor(c, e->sym == XKB_KEY_Right ? cur + 1 : cur ? cur - 1 : 0, e->mods);
        return true;
    case XKB_KEY_Page_Down: move_cursor(c, cur + page, e->mods); return true;
    case XKB_KEY_Page_Up: move_cursor(c, cur >= page ? cur - page : 0, e->mods); return true;
    case XKB_KEY_Home: move_cursor(c, 0, e->mods); return true;
    case XKB_KEY_End: move_cursor(c, c->count - 1, e->mods); return true;
    case XKB_KEY_space:
        if (c->ta[0] && ui_now_ms() - c->ta_ms <= 1000) return type_ahead(c, e);   /* a name with a space */
        if (c->has_cursor) { sel_set(c, cur, !sel_get(c, cur)); damage_item(c, cur); }
        return true;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter:
        if (c->has_cursor && c->src->activate) c->src->activate(c->ctx, cur);
        return true;
    case XKB_KEY_slash: case XKB_KEY_a:   /* Ctrl+/ (OS/2) and Ctrl+A select all */
        if (!(e->mods & UI_MOD_CTRL)) return type_ahead(c, e);
        sel_range(c, 0, c->count - 1, true);
        return true;
    case XKB_KEY_backslash:               /* Ctrl+\ deselects all (OS/2) */
        if (!(e->mods & UI_MOD_CTRL)) return type_ahead(c, e);
        sel_clear(c);
        return true;
    case XKB_KEY_Menu: case XKB_KEY_F10:
        if (e->sym == XKB_KEY_F10 && !(e->mods & UI_MOD_SHIFT)) return false;
        if (c->src->menu) {
            ui_rect r = c->has_cursor ? item_rect(c, cur) : list_rect(c);
            c->src->menu(c->ctx, c->has_cursor ? cur : SIZE_MAX, r.x + 20, r.y + r.h / 2, c->s->app->last_serial);
        }
        return true;
    default:
        return type_ahead(c, e);
    }
}

/* ------------------------------------------------------------------ model */

void ui_container_count_changed(ui_container *c)
{
    size_t old = c->count, now = c->src->count(c->ctx);
    if (now == old) return;
    c->count = now;
    if (c->has_cursor && c->cursor >= now) c->has_cursor = false;
    if (c->scroll > max_scroll(c)) scroll_to(c, max_scroll(c));
    /* Newly existing items on screen, and the scroll bar. */
    size_t from = old < now ? old : now;
    ui_rect r = item_crect(c, from - from % cols(c)), w = window_crect(c);
    if (r.y < w.y + w.h) {
        double y0 = fmax(r.y, w.y);
        damage_content(c, (ui_rect){ w.x, y0, w.w, w.y + w.h - y0 });
    }
    damage_sb(c);
    c->vis_first = c->vis_last = SIZE_MAX;
    report_visible(c);
}

void ui_container_changed(ui_container *c, size_t first, size_t last)
{
    if (free_icons(c)) {
        /* An item's index says nothing about where it is: look at each. */
        if (last >= c->count) last = c->count ? c->count - 1 : 0;
        if (last - first > 512) { damage_list(c); return; }
        for (size_t i = first; i <= last && i < c->count; i++) damage_item(c, i);
        return;
    }
    size_t f, l;
    visible_range(c, &f, &l);
    if (last < f || first > l) return;
    if (first < f) first = f;
    if (last > l) last = l;
    if (c->view != UI_VIEW_ICON) {
        ui_rect a = item_crect(c, first), b = item_crect(c, last), w = window_crect(c);
        double y0 = fmax(a.y, w.y), y1 = fmin(b.y + b.h, w.y + w.h);
        if (y1 > y0) damage_content(c, (ui_rect){ w.x, y0, w.w, y1 - y0 });
    } else {
        for (size_t i = first; i <= last; i++) damage_item(c, i);
    }
}

void ui_container_layout(ui_container *c, ui_rect r)
{
    r.x = floor(r.x);
    r.y = floor(r.y);
    r.w = floor(r.w);
    r.h = floor(r.h);
    c->r = r;
    c->count = c->src->count(c->ctx);
    if (c->scroll > max_scroll(c)) c->scroll = max_scroll(c);
    c->vis_first = c->vis_last = SIZE_MAX;
    ui_surface_damage(c->s, r);
    if (c->layer) {
        ui_layer_set_area(c->layer, list_rect(c));
        ui_layer_set_offset(c->layer, c->scroll);
        ui_layer_damage_all(c->layer);
    }
    report_visible(c);
}

void ui_container_set_view(ui_container *c, ui_view v)
{
    if (v == c->view) return;
    /* Keep the same items at the top. */
    size_t top = (size_t)(c->scroll / line_h(c)) * cols(c);
    c->view = v;
    c->scroll = 0;
    c->scroll = fmin(max_scroll(c), floor((double)(top / cols(c)) * line_h(c)));
    ui_container_layout(c, c->r);
}

ui_view ui_container_view(ui_container *c) { return c->view; }

void ui_container_set_focused(ui_container *c, bool on)
{
    if (c->focused == on) return;
    c->focused = on;
    damage_list(c);
}

ui_container *ui_container_new(ui_surface *s, const ui_container_src *src, void *ctx, ui_view view)
{
    ui_container *c = calloc(1, sizeof *c);
    c->s = s;
    c->src = src;
    c->ctx = ctx;
    c->view = view;
    c->focused = true;
    c->vis_first = c->vis_last = SIZE_MAX;
    c->drag_i = c->drop_i = SIZE_MAX;
    if (!getenv("WPS_UI_NO_LAYER")) c->layer = ui_layer_new(s, layer_draw, c);
    return c;
}

void ui_container_free(ui_container *c)
{
    if (!c) return;
    ui_entry_free(c->rename);
    ui_layer_free(c->layer);
    free(c->sel);
    free(c);
}

void ui_container_select_none(ui_container *c) { sel_clear(c); }

void ui_container_set_backdrop(ui_container *c, ui_layer_draw_fn fn, void *ctx)
{
    c->no_scrollbars = fn != NULL;   /* a picture behind: a desktop, which does not scroll */
    if (fn && c->layer) {
        /* No scroll layer either: it would cover the whole window, and the
         * compositor (Mutter) holds the frame callbacks of a surface its
         * subsurface hides entirely, which stops all drawing. */
        ui_layer_free(c->layer);
        c->layer = NULL;
    }
    c->backdrop = fn;
    c->backdrop_ctx = ctx;
    if (c->layer) ui_layer_damage_all(c->layer);
    ui_surface_damage_all(c->s);
}

void ui_container_select(ui_container *c, size_t i)
{
    if (i >= c->count) return;
    sel_set(c, i, true);
    damage_item(c, i);
}

size_t ui_container_cursor(ui_container *c) { return c->has_cursor ? c->cursor : SIZE_MAX; }

void ui_container_set_cursor(ui_container *c, size_t i)
{
    if (i >= c->count) { c->has_cursor = false; return; }
    c->cursor = c->anchor = i;
    c->has_cursor = true;
    damage_item(c, i);
}

void ui_container_reset(ui_container *c)
{
    c->count = c->src->count(c->ctx);
    if (c->scroll > max_scroll(c)) scroll_to(c, max_scroll(c));
    if (c->has_cursor && c->cursor >= c->count) c->has_cursor = false;
    damage_list(c);
    damage_sb(c);
    c->vis_first = c->vis_last = SIZE_MAX;
    report_visible(c);
}

size_t ui_container_item_at(ui_container *c, double x, double y) { return item_at(c, x, y); }

void ui_container_set_drop_target(ui_container *c, size_t i)
{
    if (i == c->drop_i) return;
    if (c->drop_i < c->count) damage_item(c, c->drop_i);
    c->drop_i = i;
    if (i < c->count) damage_item(c, i);
}

void ui_draw_kind_icon(cairo_t *cr, ui_kind k, double x, double y, double size) { draw_icon(cr, k, x, y, size); }

void ui_container_to_list(ui_container *c, double x, double y, double *lx, double *ly)
{
    ui_rect l = list_rect(c);
    *lx = x - l.x - 4 + c->hscroll;
    *ly = y - l.y - 4 + c->scroll;
}

void ui_container_drag_origin(ui_container *c, double *x, double *y)
{
    *x = c->drag_x;
    *y = c->drag_y;
}

void ui_container_layout_changed(ui_container *c)
{
    if (c->hscroll > max_hscroll(c)) c->hscroll = max_hscroll(c);
    if (c->scroll > max_scroll(c)) scroll_to(c, max_scroll(c));
    ui_surface_damage(c->s, c->r);
    if (c->layer) {
        ui_layer_set_area(c->layer, list_rect(c));   /* a horizontal bar may have come or gone */
        ui_layer_damage_all(c->layer);
    }
    report_visible(c);
}

double ui_container_list_width(ui_container *c) { return list_rect(c).w - 8; }
