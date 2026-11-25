/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * The Column view (0.6 step 4; v0.6-design 3; NeXTSTEP's browser): the
 * folder and the folders chosen in it side by side, each a column of
 * small icons and titles; a folder chosen in a column opens in the next
 * one, a file chosen shows its details there; a path above the columns.
 *
 * The window's own list is the first column (its container in
 * UI_VIEW_LIST); the next ones are panes (pane_open): folders with no
 * window of their own, drawn in the window, read, followed, with menus,
 * renaming and dragging as a window's folder has them. Columns are a
 * fixed width; when there are more than fit, the last ones are shown
 * (the path's parts bring the earlier ones back).
 *
 * Keys: Up and Down in a column; Right into the chosen folder's column,
 * Left back; Enter opens; typing jumps to a name in the column.
 */
#define _GNU_SOURCE
#include "shell.h"

#include <math.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define COL_W 220
#define PATH_H (ceil(ui_font_line_h(UI_FONT_REGULAR)) + 10)
#define MAX_COLS ((int)(sizeof ((struct folder *)0)->panes / sizeof (struct folder *)))

bool is_columns(const struct folder *f) { return f && !f->col_owner && f->cont && ui_container_view(f->cont) == UI_VIEW_LIST; }

static struct folder *col(struct folder *f, int k) { return k == 0 ? f : k <= f->ncols ? f->panes[k - 1] : NULL; }

struct folder *columns_focused(struct folder *f)
{
    if (!is_columns(f)) return f;
    struct folder *c = col(f, f->col_focus);
    return c ? c : f;
}

/* ---- where things are */

static ui_rect area_of(struct folder *f)
{
    return f->col_area;
}

/* Columns shown: the file's details count as one more. */
static int total(struct folder *f) { return 1 + f->ncols + (f->col_file[0] ? 1 : 0); }
static int fits(struct folder *f) { int n = (int)(area_of(f).w / COL_W); return n < 1 ? 1 : n; }
static int first_shown(struct folder *f) { int fst = total(f) - fits(f); return fst < 0 ? 0 : fst; }

static ui_rect column_rect(struct folder *f, int k)
{
    ui_rect a = area_of(f);
    int fst = first_shown(f);
    if (k < fst) return (ui_rect){ 0, 0, 0, 0 };
    double x = a.x + (double)(k - fst) * COL_W, w = COL_W - 1;
    if (k == total(f) - 1) w = a.x + a.w - x;   /* (the last: to the edge) */
    if (w <= 0) return (ui_rect){ 0, 0, 0, 0 };
    return (ui_rect){ x, a.y + PATH_H, w, a.h - PATH_H };
}

/* The path: the window's folder, then each column's, as parts to click. */
static ui_rect part_rect(struct folder *f, int k)
{
    ui_rect a = area_of(f);
    double x = a.x + 6;
    for (int j = 0; j < k; j++) {
        struct folder *c = col(f, j);
        x += ui_text_width(UI_FONT_REGULAR, c ? c->title : "") + 22;
    }
    struct folder *c = col(f, k);
    return (ui_rect){ x, a.y + 3, ui_text_width(UI_FONT_REGULAR, c ? c->title : "") + 10, PATH_H - 6 };
}

void columns_layout(struct folder *f, ui_rect area)
{
    f->col_area = area;
    for (int k = 0; k <= f->ncols; k++) {
        struct folder *c = col(f, k);
        ui_rect r = column_rect(f, k);
        ui_container_layout(c->cont, r.w > 0 ? r : (ui_rect){ area.x, area.y, 0, 0 });
    }
}

static void relayout(struct folder *f)
{
    layout(f);
    ui_surface_damage_all(f->win);
}

/* ---- the columns: opened, closed, focused */

static void close_after(struct folder *f, int k)
{
    while (f->ncols > k) {
        struct folder *p = f->panes[--f->ncols];
        f->panes[f->ncols] = NULL;
        p->closing = true;
        free_folder(p);
    }
    if (f->col_focus > f->ncols) f->col_focus = f->ncols;
}

static void set_focus(struct folder *f, int k)
{
    if (k < 0 || k > f->ncols) return;
    f->col_focus = k;
    for (int j = 0; j <= f->ncols; j++) ui_container_set_focused(col(f, j)->cont, j == k);
    ui_surface_damage_all(f->win);
    damage_status(f);
}

void columns_start(struct folder *f)
{
    f->ncols = 0;
    f->col_focus = 0;
    f->col_file[0] = 0;
    ui_container_set_view(f->cont, UI_VIEW_LIST);
    ui_container_set_layered(f->cont, false);   /* (lists side by side: drawn on the window) */
    relayout(f);
}

void columns_stop(struct folder *f)
{
    close_after(f, 0);
    f->col_file[0] = 0;
    f->col_focus = 0;
    ui_container_set_focused(f->cont, true);
    ui_container_set_layered(f->cont, true);
}

void columns_pane_changed(struct folder *p)
{
    struct folder *f = p ? p->col_owner : NULL;
    if (f && !f->closing) ui_surface_damage_all(f->win);
}

/* The one item chosen in column c: the cursor's, if selected, else the
 * first selected (SIZE_MAX: none, or several). */
static size_t chosen(struct folder *c)
{
    size_t total_n = c->m.total, at = SIZE_MAX, n = 0;
    for (size_t i = 0; i < total_n; i++)
        if (ui_container_is_selected(c->cont, i)) { if (!n++) at = i; }
    if (n != 1) return SIZE_MAX;
    return at;
}

/* A folder row's folder (a shadow's or link's: what it stands for). */
static bool folder_of(struct folder *c, size_t i, char *out, size_t n)
{
    struct row *rw = row_at(c, i);
    if (!rw || rw->kind != UI_KIND_FOLDER) return false;
    if ((rw->shadow || rw->link) && rw->target && rw->target[0] == '/') { snprintf(out, n, "%s", rw->target); return true; }
    const char *nm = model_name(&c->m, i);
    if (!nm || nm[0] == '\x01') return false;
    snprintf(out, n, "%s/%s", strcmp(c->path, "/") ? c->path : "", nm);
    return true;
}

/* Column k's choice made the next columns: its folder opened in k + 1
 * (kept if it is that one already), or its file's details; nothing
 * chosen: the columns after it gone. */
static void follow_from(struct folder *f, int k)
{
    struct folder *c = col(f, k);
    if (!c) return;
    size_t i = chosen(c);
    char path[PATH_MAX];
    bool had_file = f->col_file[0] != 0;
    f->col_file[0] = 0;
    if (i != SIZE_MAX && folder_of(c, i, path, sizeof path)) {
        struct folder *next = col(f, k + 1);
        char real[PATH_MAX];
        const char *want = realpath(path, real) ? real : path;
        if (next && !strcmp(next->path, want)) { if (had_file) relayout(f); return; }   /* (that one already) */
        close_after(f, k);
        if (f->ncols < MAX_COLS) {
            struct folder *p = pane_open(f, want);
            ui_container_set_layered(p->cont, false);
            f->panes[f->ncols++] = p;
        }
    } else {
        close_after(f, k);
        if (i != SIZE_MAX) {   /* a file: its details beside it */
            target_of(c, i, f->col_file, sizeof f->col_file);
            f->col_file_i = i;
        }
    }
    relayout(f);
}

void columns_follow(struct folder *f)
{
    if (is_columns(f)) follow_from(f, f->col_focus);
}

/* ---- drawing */

static void draw_details(struct folder *f, cairo_t *cr, ui_rect r)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_fill(cr, r, k->client_bg);
    struct folder *c = col(f, f->ncols);
    struct row *rw = c ? row_at(c, f->col_file_i) : NULL;
    if (!rw) return;
    ui_item it = { .loaded = true, .detailed = true, .kind = rw->kind, .shadow = rw->shadow, .link = rw->link };
    int big = (int)ui_container_icon_set(c->cont)->large;
    if (big < 48) big = 48;
    double ix = r.x + (r.w - big) / 2, iy = r.y + 20;
    it.image = icons_item(NULL, rw->icon, rw->kind, rw->target && rw->target[0] ? rw->target : rw->name, (int)(big * ui_surface_scale(f->win) + 0.5));
    ui_draw_item_icon(cr, &it, ix, iy, big);
    double y = iy + big + 14, lh = ui_font_line_h(UI_FONT_REGULAR) + 2;
    const char *title = rw->title && rw->title[0] ? rw->title : rw->name;
    ui_set_color(cr, k->text);
    double tw = ui_text_width(UI_FONT_BOLD, title);
    ui_text_draw(cr, UI_FONT_BOLD, r.x + fmax(8, (r.w - tw) / 2), y + ui_font_ascent(UI_FONT_BOLD), title, r.w - 16);
    y += lh + 6;
    char size[64], date[64], type[64];
    if (rw->size >= 1024 * 1024) snprintf(size, sizeof size, "%.1f MB", rw->size / 1048576.0);
    else if (rw->size >= 1024) snprintf(size, sizeof size, "%.1f KB", rw->size / 1024.0);
    else snprintf(size, sizeof size, "%lld bytes", (long long)rw->size);
    time_t t = (time_t)rw->mtime;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
    const char *dot = rw->name ? strrchr(rw->name, '.') : NULL;
    snprintf(type, sizeof type, "%s", rw->kind == UI_KIND_PROGRAM ? "Program" : dot && dot != rw->name ? dot + 1 : "File");
    const char *labels[3] = { "Size", "Type", "Last write" }, *vals[3] = { size, type, date };
    for (int j = 0; j < 3; j++) {
        ui_set_color(cr, k->shadow);
        ui_text_draw(cr, UI_FONT_REGULAR, r.x + 12, y + ui_text_ascent(), labels[j], 80);
        ui_set_color(cr, k->text);
        ui_text_draw(cr, UI_FONT_REGULAR, r.x + 96, y + ui_text_ascent(), vals[j], r.w - 104);
        y += lh;
    }
}

void columns_draw(struct folder *f, cairo_t *cr, ui_rect clip)
{
    if (!is_columns(f)) return;
    const ui_scheme *k = &ui_scheme_now;
    ui_rect a = area_of(f);
    /* the path */
    ui_rect bar = { a.x, a.y, a.w, PATH_H };
    if (ui_rect_meets(clip, bar)) {
        ui_fill(cr, bar, k->face);
        ui_fill(cr, (ui_rect){ bar.x, bar.y + bar.h - 1, bar.w, 1 }, k->shadow);
        for (int j = 0; j <= f->ncols; j++) {
            ui_rect r = part_rect(f, j);
            if (j == f->col_focus) ui_bevel(cr, r, true);
            ui_set_color(cr, k->text);
            ui_text_draw(cr, UI_FONT_REGULAR, r.x + 5, r.y + (r.h + ui_text_ascent()) / 2 - 1, col(f, j)->title, a.x + a.w - r.x - 6);
            if (j < f->ncols) {   /* a separator: > */
                double sx = r.x + r.w + 4, sy = r.y + r.h / 2;
                ui_set_color(cr, k->shadow);
                cairo_move_to(cr, sx, sy - 4);
                cairo_line_to(cr, sx + 4, sy);
                cairo_line_to(cr, sx, sy + 4);
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
            }
        }
    }
    /* the panes (the window's own list: drawn by the window) and the lines between */
    for (int j = 1; j <= f->ncols; j++) {
        ui_rect r = column_rect(f, j);
        if (r.w > 0) ui_container_draw(f->panes[j - 1]->cont, cr, clip);
    }
    for (int j = first_shown(f); j < total(f) - 1; j++) {
        ui_rect r = column_rect(f, j);
        if (r.w > 0) ui_fill(cr, (ui_rect){ r.x + r.w, r.y, 1, r.h }, k->shadow);
    }
    if (f->col_file[0]) {
        ui_rect r = column_rect(f, total(f) - 1);
        if (r.w > 0 && ui_rect_meets(clip, r)) draw_details(f, cr, r);
    }
}

/* The column at x, y: a pane, or the window itself (its own list, or
 * anywhere else). */
struct folder *columns_at(struct folder *f, double x, double y)
{
    if (!is_columns(f)) return f;
    for (int j = 1; j <= f->ncols; j++) {
        ui_rect r = column_rect(f, j);
        if (r.w > 0 && ui_rect_has(r, x, y)) return f->panes[j - 1];
    }
    return f;
}

/* ---- the pointer and the keys */

/* Events on the path and the panes; the window's own list (the first
 * column) is left to the window (its focus taken here). True: taken. */
bool columns_pointer(struct folder *f, const ui_pointer_event *e)
{
    if (!is_columns(f)) return false;
    ui_rect a = area_of(f);
    if (e->kind == UI_PRESS && e->y >= a.y && e->y < a.y + PATH_H && e->x >= a.x && e->x < a.x + a.w) {
        for (int j = 0; j <= f->ncols; j++)
            if (ui_rect_has(part_rect(f, j), e->x, e->y)) {   /* a part of the path: that column, the ones after it gone */
                close_after(f, j);
                f->col_file[0] = 0;
                ui_container_select_none(col(f, j)->cont);
                set_focus(f, j);
                relayout(f);
                return true;
            }
        return true;
    }
    for (int j = 1; j <= f->ncols; j++) {
        ui_rect r = column_rect(f, j);
        bool in = r.w > 0 && ui_rect_has(r, e->x, e->y);
        if (!in && e->kind == UI_PRESS) continue;
        if (!in && e->kind != UI_RELEASE && e->kind != UI_MOTION) continue;
        struct folder *p = f->panes[j - 1];
        if (in && e->kind == UI_PRESS && f->col_focus != j) set_focus(f, j);
        if (!in && f->col_focus != j) continue;   /* (a release or move: the pressed column's) */
        size_t before = ui_container_selected_count(p->cont);
        ui_container_pointer(p->cont, e);
        if (e->kind == UI_PRESS || e->kind == UI_RELEASE || ui_container_selected_count(p->cont) != before) {
            damage_status(f);
            if (e->kind != UI_MOTION) follow_from(f, j);
        }
        return true;
    }
    if (e->kind == UI_PRESS) {
        ui_rect r0 = column_rect(f, 0);
        if (r0.w > 0 && ui_rect_has(r0, e->x, e->y) && f->col_focus != 0) set_focus(f, 0);
        if (f->col_file[0] && ui_rect_has(column_rect(f, total(f) - 1), e->x, e->y)) return true;   /* (the details: nothing to do) */
    }
    return false;
}

/* Left and Right between columns; the rest to the column with the
 * keyboard (a pane's here; the window's own list: false, the window's). */
bool columns_key(struct folder *f, const ui_key_event *e)
{
    if (!is_columns(f)) return false;
    struct folder *c = col(f, f->col_focus);
    if (!c) return false;
    if (e->sym == XKB_KEY_Left && !(e->mods & (UI_MOD_CTRL | UI_MOD_ALT)) && !ui_container_renaming(c->cont)) {
        if (f->col_focus > 0) {
            set_focus(f, f->col_focus - 1);
            f->col_file[0] = 0;
            relayout(f);
        }
        return true;
    }
    if (e->sym == XKB_KEY_Right && !(e->mods & (UI_MOD_CTRL | UI_MOD_ALT)) && !ui_container_renaming(c->cont)) {
        if (f->col_focus < f->ncols) {
            struct folder *n = f->panes[f->col_focus];
            set_focus(f, f->col_focus + 1);
            if (n->m.total && !ui_container_selected_count(n->cont)) {   /* into it: its first object chosen */
                ui_container_set_cursor(n->cont, 0);
                ui_container_select(n->cont, 0);
                follow_from(f, f->col_focus);
            }
        }
        return true;
    }
    if (f->col_focus == 0) return false;   /* (the window's own list: the window's keys) */
    size_t before = ui_container_selected_count(c->cont), cur = ui_container_cursor(c->cont);
    win_key_on(c, e);
    if (ui_container_selected_count(c->cont) != before || ui_container_cursor(c->cont) != cur) follow_from(f, f->col_focus);
    damage_status(f);
    return true;
}
