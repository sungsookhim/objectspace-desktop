/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* osd-shell: Folder windows: their model (the service's listing, a page at a time), backgrounds, following a folder and the service's events. */
#define _GNU_SOURCE
#include "shell.h"

/*
 * osd-shell: the Objectspace GUI (G1: folder windows). One process draws every
 * Objectspace window (gui-design.md: Processes); the object service is osdd.
 *
 * usage: osd-shell [--details] [--bench SECONDS] FOLDER...
 *   --bench: scroll the first window by itself, print frame statistics as
 *   JSON, and exit (OSD_UI_SCALE and OSD_SHELL_SIZE=WxH help measure).
 */
#define _GNU_SOURCE

/* The status line: tall enough for the interface font (GNOME's may be larger). */
double status_h(void) { double h = ceil(ui_scheme_now.font_px + 9); return h < 20 ? 20 : h; }

ui_app *app;

struct folder *folders;

long next_folder_id = 1;

double opt_bench;

ui_view opt_view = UI_VIEW_ICON;

struct folder *folder_by_id(long id)
{
    for (struct folder *f = folders; f; f = f->next)
        if (f->id == id) return f;
    return NULL;
}

void *as_arg(struct folder *f) { return (void *)(intptr_t)f->id; }




char folders_font[128];   /* the System's Folders font: all folders' (shown by the self-tests only) */
char titles_font[128];     /* ... title bars' */

/* The length of a data file's name without its extension ("a.tar.gz":
 * "a"), or 0 if it has none to hide. */
static size_t name_sans_ext(const char *n)
{
    const char *d = strrchr(n, '.');
    if (!d || d == n || !d[1] || strlen(d) > 12) return 0;
    size_t l = (size_t)(d - n);
    if (l > 4 && !strncasecmp(n + l - 4, ".tar", 4)) l -= 4;
    return l;
}

/* What an item's icon is titled: its title (a file's is its name), and
 * with Show file extensions off a data file's name without the extension. */
const char *row_shown(struct folder *f, struct row *r)
{
    if (!r->name) return "";
    const char *t = r->title ? r->title : r->name[0] == '\x01' ? "" : r->name;   /* a central object: its title comes with details */
    if (f->show_ext || r->kind != UI_KIND_FILE || r->shadow || r->link || r->name[0] == '\x01' || (r->title && strcmp(r->title, r->name)))
        return t;
    size_t nl = strlen(r->name);
    if (nl > 8 && !strcmp(r->name + nl - 8, ".desktop")) return t;
    size_t l = name_sans_ext(r->name);
    if (!l) return t;
    if (!r->noext) r->noext = strndup(r->name, l);
    return r->noext ? r->noext : t;
}

struct folder *from_arg(void *arg) { return folder_by_id((long)(intptr_t)arg); }

/* ---- backgrounds (v0.3-design 3): the desktop's (its own, or GNOME's
 * through wallpaper.c) and folder windows' (behind their items) */

/* Behind a folder window's items, in content coordinates: its colour, and
 * its picture laid out on the first screenful (tiled: everywhere). */
static void folder_bg_draw(void *ctx, cairo_t *cr, ui_rect clip)
{
    struct folder *f = ctx;
    double rgb[3];
    if (ui_parse_color(f->bg_color, rgb)) cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    else ui_set_color(cr, ui_scheme_now.client_bg);
    cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
    cairo_fill(cr);
    if (!f->bg_img) return;
    ui_rect v = ui_container_list_rect(f->cont);
    double w = v.w, h = v.h, iw = cairo_image_surface_get_width(f->bg_img), ih = cairo_image_surface_get_height(f->bg_img);
    if (iw <= 0 || ih <= 0 || w <= 0 || h <= 0) return;
    cairo_save(cr);
    cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
    cairo_clip(cr);
    if (f->bg_mode == 1) {   /* tiled */
        cairo_pattern_t *pt = cairo_pattern_create_for_surface(f->bg_img);
        cairo_pattern_set_extend(pt, CAIRO_EXTEND_REPEAT);
        cairo_set_source(cr, pt);
        cairo_pattern_destroy(pt);
        cairo_paint(cr);
    } else {
        double sx = 1, sy = 1;
        if (f->bg_mode == 2) sx = sy = fmax(w / iw, h / ih);        /* scaled to fill */
        else if (f->bg_mode == 3) sx = sy = fmin(w / iw, h / ih);   /* scaled to fit */
        else if (f->bg_mode == 4) { sx = w / iw; sy = h / ih; }      /* stretched */
        cairo_rectangle(cr, 0, 0, w, h);   /* (the first screenful) */
        cairo_clip(cr);
        cairo_translate(cr, (w - iw * sx) / 2, (h - ih * sy) / 2);
        cairo_scale(cr, sx, sy);
        cairo_set_source_surface(cr, f->bg_img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    }
    cairo_restore(cr);
}

static void bg_show(struct folder *f)
{
    bool any = f->bg_image[0] || f->bg_color[0];
    ui_container_set_background(f->cont, any ? folder_bg_draw : NULL, any ? f : NULL, f->bg_img != NULL);
}

static void bg_loaded(void *ctx, const char *png)
{
    struct bg_load *l = ctx;
    struct folder *f = folder_by_id(l->folder);
    if (f && !f->closing && !strcmp(f->bg_img_path, l->path)) {
        if (f->bg_img) cairo_surface_destroy(f->bg_img);
        f->bg_img = png ? cairo_image_surface_create_from_png(png) : NULL;
        if (f->bg_img && cairo_surface_status(f->bg_img) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(f->bg_img); f->bg_img = NULL; }
        if (!f->bg_img) fprintf(stderr, "osd-shell: %s: cannot read the background picture %s\n", f->path, l->path);
        bg_show(f);
    }
    free(l);
}

static void bg_apply(struct folder *f)
{
    if (f->desktop) {   /* GNOME's, or its own */
        wallpaper_own(!f->bg_gnome, f->bg_image, f->bg_mode, f->bg_color);
        return;
    }
    if (strcmp(f->bg_img_path, f->bg_image)) {
        snprintf(f->bg_img_path, sizeof f->bg_img_path, "%s", f->bg_image);
        if (f->bg_img) cairo_surface_destroy(f->bg_img);
        f->bg_img = NULL;
        if (f->bg_image[0]) {
            struct bg_load *l = calloc(1, sizeof *l);
            l->folder = f->id;
            snprintf(l->path, sizeof l->path, "%s", f->bg_image);
            char out[PATH_MAX];
            if (icons_preview(f->bg_image, 1920, out, sizeof out, bg_loaded, l)) bg_loaded(l, out);   /* (a PNG, or made before) */
        }
    }
    bg_show(f);
}

/* A heading clicked: sorted by it (again: the other way), for the folder. */
void head_clicked(void *ctx, int col)
{
    struct folder *f = from_arg(ctx);
    if (!f) return;
    int choice = col <= 2 ? 1 : col == 3 ? 2 : col == 4 ? 3 : 4;   /* (title and real name: by name) */
    bool desc = choice == f->sort_choice ? !f->sort_desc : false;
    cv k = { .k = CV_INT, .i = choice }, d = { .k = CV_BOOL, .b = desc };
    svc_settings_set(f->path, "sort_key", &k, -1, ignore_reply, NULL);
    svc_settings_set(f->path, "sort_desc", &d, -1, ignore_reply, NULL);
}

/* An edge dragged: the folder keeps its columns' widths. */
void cols_resized(void *ctx, const double *w)
{
    struct folder *f = from_arg(ctx);
    if (!f) return;
    char t[128];
    snprintf(t, sizeof t, "%.0f,%.0f,%.0f,%.0f,%.0f", w[1], w[2], w[3], w[4], w[5]);
    cv v = { .k = CV_TEXT, .s = t, .len = strlen(t) };
    svc_settings_set(f->path, "col_widths", &v, -1, ignore_reply, NULL);
}

/* A choice's option number in its list ("a,b,c"), or -1. */
static int option_number(const char *choices, const char *v)
{
    int i = 0;
    for (const char *p = choices; p && *p && v; i++) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (strlen(v) == l && !strncmp(p, v, l)) return i;
        p = e ? e + 1 : NULL;
    }
    return -1;
}

/* Its properties, resolved by the service (its own, else all folders',
 * else the default; object-properties-design.md 2.2): its background,
 * sort, icon view, columns, fonts and what it shows. */
static void got_bg(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!f || f->closing || !res || res->k != CV_ARRAY) return;
    f->bg_image[0] = f->bg_color[0] = 0;
    f->bg_mode = 2;
    f->bg_gnome = true;
    f->icon_font[0] = f->title_font[0] = 0;
    char title_color[16] = "", bar_color[16] = "";
    int was_choice = f->sort_choice;
    bool was_desc = f->sort_desc, was_hidden = f->show_hidden, was_ext = f->show_ext;
    unsigned cols = 0;
    double w[6] = { 0 };
    bool whole = true, lock_all = false;
    int icon_size = -1, free_l = -1, find_place = 0, menubar = 0, pictures = 0;
    unsigned details = 0;   /* (the Column view's, CD_* bits) */
    bool offered = false;
    for (size_t i = 0; i < res->n; i++) {
        const cv *pr = &res->items[i];
        const char *key = cv_text(pr, "name"), *v = cv_text(pr, "value"), *ch = cv_text(pr, "choices");
        if (!key || !v) continue;
        bool on = !strcmp(v, "1");
        if (!strcmp(key, "bg_image")) snprintf(f->bg_image, sizeof f->bg_image, "%s", v);
        else if (!strcmp(key, "bg_color")) snprintf(f->bg_color, sizeof f->bg_color, "%s", v);
        else if (!strcmp(key, "bg_mode")) f->bg_mode = option_number(ch, v);
        else if (!strcmp(key, "bg_gnome")) f->bg_gnome = on;
        else if (!strcmp(key, "sort_key")) f->sort_choice = option_number(ch, v);
        else if (!strcmp(key, "sort_desc")) f->sort_desc = on;
        else if (!strcmp(key, "icon_titles")) whole = option_number(ch, v) == 0;
        else if (!strcmp(key, "lock_all")) lock_all = on;
        else if (!strcmp(key, "icon_font")) snprintf(f->icon_font, sizeof f->icon_font, "%s", v);
        else if (!strcmp(key, "icon_theme") && strcmp(f->icon_theme, v)) {   /* every icon again */
            snprintf(f->icon_theme, sizeof f->icon_theme, "%s", v);
            if (f->desktop) icons_set_default_theme(v);   /* (menus, the Launcher, TheBar) */
            ui_container_changed(f->cont, 0, SIZE_MAX);
        }
        else if (!strcmp(key, "icon_set")) icon_size = (int)ui_icon_size_named(v);
        else if (!strcmp(key, "icon_layout")) free_l = strcmp(v, "grid") != 0;
        else if (!strcmp(key, "title_font")) snprintf(f->title_font, sizeof f->title_font, "%s", v);
        else if (!strcmp(key, "title_color")) snprintf(title_color, sizeof title_color, "%s", v);
        else if (!strcmp(key, "title_bar_color")) snprintf(bar_color, sizeof bar_color, "%s", v);
        else if (!strcmp(key, "show_hidden")) f->show_hidden = on;
        else if (!strcmp(key, "work_area")) workarea_settings(f, on);   /* (its group comes back) */
        else if (!strcmp(key, "show_ext")) f->show_ext = on;
        else if (!strcmp(key, "arrange_offered")) offered = on || !strcmp(v, "true");
        else if (!strcmp(key, "search_place")) find_place = option_number(ch, v) < 0 ? 0 : option_number(ch, v);
        else if (!strcmp(key, "menu_bar")) menubar = on;
        else if (!strcmp(key, "show_pictures")) pictures = on;
        else if (!strncmp(key, "cd_", 3)) { if (on) details |= columns_detail_bit(key); }
        else if (!strncmp(key, "col_", 4) && on) {
            cols |= !strcmp(key, "col_name") ? 1u : !strcmp(key, "col_type") ? 2u : !strcmp(key, "col_size") ? 4u
                  : !strcmp(key, "col_date") ? 8u : 0u;
        } else if (!strcmp(key, "col_widths")) {
            sscanf(v, "%lf,%lf,%lf,%lf,%lf", &w[1], &w[2], &w[3], &w[4], &w[5]);
        }
    }
    if (f->bg_mode < 0) f->bg_mode = 2;
    if (icon_size >= 0 && icon_size != f->icon_size) {   /* its icon set (Small, Default, Large) */
        f->icon_size = icon_size;
        if (f->desktop) icons_set_default_size((ui_icon_size)icon_size);   /* (menus, the Launcher, title bars) */
        ui_icon_set set = ui_icon_set_of((ui_icon_size)icon_size), was = *folder_icons(f);
        ui_container_set_icon_set(f->cont, &set);
        if (memcmp(&set, &was, sizeof set)) folder_icons_changed(f);   /* (the one it had: nothing to do) */
    }
    if (free_l >= 0 && free_l != f->free_layout) {   /* its Layout: free (OS/2's) or the grid */
        int was = f->free_layout;
        f->free_layout = free_l;
        layout_set_free(f->lay, free_l == 1);
        for (size_t p = 0; p < f->m.npages; p++)
            for (size_t k = 0; f->m.pages[p] && k < PAGE; k++) f->m.pages[p]->rows[k].cellh = 0;
        if (was >= 0 && !is_tree(f) && f->m.complete) arrange(f);   /* changed: arranged anew in it */
        else {
            layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
            if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
        }
    }
    if (f->arrange_offered < 0) { f->arrange_offered = offered; arrange_offer_check(f); }
    if (details != f->col_details) {   /* the Column view's details: more or fewer */
        f->col_details = details;
        if (is_columns(f)) ui_surface_damage_all(f->win);
    }
    if (pictures != f->show_pictures) {   /* Show pictures: thumbnails in place of icons */
        f->show_pictures = pictures;
        ui_container_changed(f->cont, 0, SIZE_MAX);
    }
    if (menubar >= 0 && menubar != f->menubar_on) {   /* its menu bar, on or off */
        f->menubar_on = menubar;
        layout(f);
        ui_surface_damage_all(f->win);
    }
    if (find_place != f->find_place) {   /* its search field elsewhere (shown now: moved at once) */
        f->find_place = find_place;
        if (f->find_on) { layout(f); ui_surface_damage_all(f->win); }
    }
    bg_apply(f);
    folder_font(f);
    double tc[3];   /* its icons' titles (the Colour Palette with Ctrl) */
    bool has_tc = ui_parse_color(title_color, tc);
    snprintf(f->title_color, sizeof f->title_color, "%s", has_tc ? title_color : "");
    ui_container_set_title_color(f->cont, has_tc ? &(ui_color){ tc[0], tc[1], tc[2], 1 } : NULL);
    ui_window_set_title_font(f->win, f->title_font[0] != 0, ui_font_named(f->title_font));   /* its own, as on OS/2 */
    double bc[3];   /* its own active title bar colour (the Colour Palette dropped on it) */
    bool has_bc = ui_parse_color(bar_color, bc);
    snprintf(f->title_bar_color, sizeof f->title_bar_color, "%s", has_bc ? bar_color : "");
    ui_window_set_title_color(f->win, has_bc ? &(ui_color){ bc[0], bc[1], bc[2], 1 } : NULL);
    layout_lock_all(f->lay, lock_all);
    f->lock_all = lock_all;
    if (whole != f->whole_titles) {   /* the Icon view page's Titles */
        f->whole_titles = whole;
        ui_container_set_whole_titles(f->cont, whole);
        layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
        if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
    }
    f->cols = cols;
    memcpy(f->col_w, w, sizeof w);
    ui_container_set_columns(f->cont, cols, w);
    static const int col_of[5] = { -1, 1, 3, 4, 5 };   /* (by name: the Title column) */
    ui_container_set_sort_mark(f->cont, f->sort_choice >= 0 && f->sort_choice <= 4 ? col_of[f->sort_choice] : -1, f->sort_desc);
    /* the sort changed (here, or in another window or the notebook): the
     * service lists it in the new order */
    if (was_choice >= 0 && (was_choice != f->sort_choice || was_desc != f->sort_desc)) reload(f);
    else if (was_choice < 0 && f->sort_choice >= 0 && f->m.complete && !is_tree(f)) layout_follow(f, true);   /* (the first arrangement waited for it) */
    else if (was_hidden != f->show_hidden) { if (is_tree(f)) tree_start(f); else reload(f); }   /* listed again */
    if (was_ext != f->show_ext && !is_tree(f)) {   /* every title again, and the rows' heights */
        for (size_t p = 0; p < f->m.npages; p++)
            for (size_t k = 0; f->m.pages[p] && k < PAGE; k++) f->m.pages[p]->rows[k].cellh = 0;
        layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
        ui_container_changed(f->cont, 0, SIZE_MAX);
        if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
    }
}

void bg_read(struct folder *f) { svc_props(f->path, got_bg, as_arg(f)); }

/* Its titles' font: its own or all folders' (the service's), else the
 * interface font; rows as tall as they now need. */
void folder_font(struct folder *f)
{
    ui_font ft = ui_font_named(f->icon_font);
    if (ft == f->font) return;
    f->font = ft;
    ui_container_set_font(f->cont, ft);
    if (is_tree(f)) return;
    for (size_t p = 0; p < f->m.npages; p++)
        for (size_t k = 0; f->m.pages[p] && k < PAGE; k++) f->m.pages[p]->rows[k].cellh = 0;
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
    ui_container_changed(f->cont, 0, SIZE_MAX);
    ui_container_layout_changed(f->cont);
}

static void got_bg_sub(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res || res->k != CV_INT) return;
    if (!f || f->closing) { svc_unsubscribe(res->i); return; }
    f->bg_sub = res->i;
}

/* Its Background read, and followed (changes made in its notebook). */
void bg_follow(struct folder *f)
{
    if (f->bg_sub) svc_unsubscribe(f->bg_sub);
    f->bg_sub = 0;
    const char *t[1] = { f->path };
    svc_subscribe_objects(t, 1, got_bg_sub, as_arg(f));
    bg_read(f);
}

/* ------------------------------------------------------------------ model */

struct page *page_at(struct model *m, size_t p)
{
    if (p >= m->npages) {
        size_t n = m->npages ? m->npages : 64;
        while (n <= p) n *= 2;
        m->pages = realloc(m->pages, n * sizeof *m->pages);
        memset(m->pages + m->npages, 0, (n - m->npages) * sizeof *m->pages);
        m->npages = n;
    }
    if (!m->pages[p]) m->pages[p] = calloc(1, sizeof **m->pages);
    return m->pages[p];
}

void model_free(struct model *m)
{
    for (size_t p = 0; p < m->npages; p++) {
        if (!m->pages[p]) continue;
        for (size_t r = 0; r < PAGE; r++) {
            free(m->pages[p]->rows[r].name);
            free(m->pages[p]->rows[r].title);
            free(m->pages[p]->rows[r].target);
            free(m->pages[p]->rows[r].icon);
            free(m->pages[p]->rows[r].icon_open);
            free(m->pages[p]->rows[r].noext);
        }
        free(m->pages[p]);
    }
    free(m->pages);
    memset(m, 0, sizeof *m);
}

const char *model_name(struct model *m, size_t i)
{
    struct page *pg = i / PAGE < m->npages ? m->pages[i / PAGE] : NULL;
    return pg && pg->rows[i % PAGE].name ? pg->rows[i % PAGE].name : NULL;
}

static ui_kind kind_of(const char *type, const char *name)
{
    if (type && !strcmp(type, "object")) return UI_KIND_OTHER;   /* a central object */
    if (type && !strcmp(type, "folder")) return UI_KIND_FOLDER;
    if (type && !strcmp(type, "link")) return UI_KIND_LINK;
    size_t l = name ? strlen(name) : 0;
    if (l > 8 && !strcmp(name + l - 8, ".desktop")) return UI_KIND_PROGRAM;
    return UI_KIND_FILE;
}

const char *view_name(ui_view v)
{
    return v == UI_VIEW_ICON ? "Icon View" : v == UI_VIEW_TREE ? "Tree View" : v == UI_VIEW_LIST ? "Column View" : "Details View";
}

void update_title(struct folder *f)
{
    if (f->col_owner) { columns_pane_changed(f); return; }   /* (a pane: its title on the window's path) */
    char t[400];
    snprintf(t, sizeof t, "%s - %s", f->title, folder_view_name(f));
    ui_window_set_title(f->win, f->desktop ? "Desktop" : t);
}

void damage_status(struct folder *f)
{
    if (f->col_owner) f = f->col_owner;   /* (a pane's: its window's status line) */
    if (f->desktop) { desktop_status(f); return; }
    ui_rect c = ui_frame_client(f->win);
    ui_surface_damage(f->win, (ui_rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H });
}

void *page_arg(struct folder *f, bool shadow)
{
    struct pctx *pc = malloc(sizeof *pc);
    pc->id = f->id;
    pc->gen = shadow ? f->sgen : f->gen;
    pc->shadow = shadow;
    return pc;
}

char setup_ids[NSETUP][64];   /* the settings objects (see below) */

char setup_cls[NSETUP][32];   /* their classes */


/* OSD_DEBUG_TIMING: when the desktop's start-up steps happen. */
double since_start(void)
{
    static double t0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
    if (!t0) t0 = now;
    return now - t0;
}

/* Store a reply's rows; returns whether they had details. */
static bool store(struct model *m, const cv *res, size_t *off_out, size_t *n_out)
{
    int64_t off = 0, tot = 0, ver = 0;
    cv_int(res, "offset", &off);
    cv_int(res, "total", &tot);
    cv_int(res, "version", &ver);
    m->version = (uint64_t)ver;
    const cv *cmp = cv_get(res, "complete"), *items = cv_get(res, "items");
    size_t n = items && items->k == CV_ARRAY ? items->n : 0;
    bool details = false;
    for (size_t i = 0; i < n; i++) {
        const cv *it = &items->items[i];
        size_t r = (size_t)off + i;
        struct row *dst = &page_at(m, r / PAGE)->rows[r % PAGE];
        const char *nm = cv_text(it, "name"), *ti = cv_text(it, "title"), *ty = cv_text(it, "type");
        if (dst->name && (dst->title ? !ti || strcmp(dst->title, ti) : ti && *ti)) m->heights_dirty = true;
        dst->cellh = 0;
        free(dst->name);
        free(dst->title);
        free(dst->target);
        free(dst->icon);
        free(dst->icon_open);
        free(dst->noext);
        dst->noext = NULL;
        dst->target = dst->icon = dst->icon_open = NULL;
        dst->name = strdup(nm ? nm : "?");
        dst->title = ti && *ti ? strdup(ti) : NULL;
        dst->kind = kind_of(ty, nm);
        dst->link = ty && !strcmp(ty, "link");
        if (cv_get(it, "mtime")) {
            details = true;
            cv_int(it, "size", &dst->size);
            cv_int(it, "mtime", &dst->mtime);
            const cv *sh = cv_get(it, "shadow"), *iu = cv_get(it, "in_use");
            dst->shadow = sh && sh->k == CV_BOOL && sh->b;
            dst->in_use = iu && iu->k == CV_BOOL && iu->b;   /* a program has it open */
            const cv *tv = cv_get(it, "template");
            dst->tpl = tv && tv->k == CV_BOOL && tv->b;
            const char *ic = cv_text(it, "icon");     /* its own icon */
            dst->icon = ic && *ic ? strdup(ic) : NULL;
            const char *io = cv_text(it, "icon_open");   /* its own open icon */
            dst->icon_open = io && *io ? strdup(io) : NULL;
            const cv *oh = cv_get(it, "open_hatch");
            dst->open_hatch = oh && oh->k == CV_BOOL && oh->b;
            const char *ex = cv_text(it, "exec");   /* a program file (+x, ELF or #!): a program, the theme's (text mode: program-text) */
            if (ex) {
                dst->kind = UI_KIND_PROGRAM;
                if (!dst->icon) dst->icon = strdup(!strcmp(ex, "text") ? "theme:program-text" : "theme:program");
            }
            const char *mk = cv_text(it, "makes");   /* a built-in template: looks like what it makes */
            if (mk) dst->kind = !strcmp(mk, "OsdFolder") ? UI_KIND_FOLDER : !strcmp(mk, "OsdProgram") ? UI_KIND_PROGRAM : UI_KIND_FILE;
            const char *tt = cv_text(it, "target"), *tp = cv_text(it, "target_path");
            if (dst->shadow || (dst->link && tt && strcmp(tt, "missing"))) {
                /* drawn as what it points to */
                dst->kind = tt && !strcmp(tt, "folder") ? UI_KIND_FOLDER : kind_of("file", tp && *tp ? tp : nm);
                dst->target = tp && *tp ? strdup(tp) : NULL;
            }
        }
    }
    for (size_t p = (size_t)off / PAGE; n && p <= ((size_t)off + n - 1) / PAGE; p++) {
        struct page *pg = page_at(m, p);
        if (details) { pg->state = P_DETAILED; pg->requested = false; }
        else if (pg->state == P_NONE) pg->state = P_NAMES;
    }
    if ((size_t)tot > m->total) m->total = (size_t)tot;
    m->complete = cmp && cmp->k == CV_BOOL && cmp->b;
    if (!details) m->growing = false;
    *off_out = (size_t)off;
    *n_out = n;
    return details;
}

static void keep_take(struct folder *f, struct keep *kp)
{
    struct model *m = &f->m;
    memset(kp, 0, sizeof *kp);
    size_t nsel = ui_container_selected_count(f->cont), cur = ui_container_cursor(f->cont);
    if (nsel && nsel <= 256) {
        kp->sel = calloc(nsel, sizeof *kp->sel);
        for (size_t i = 0; i < m->total && kp->k < nsel; i++)
            if (ui_container_is_selected(f->cont, i) && model_name(m, i)) kp->sel[kp->k++] = strdup(model_name(m, i));
    }
    if (cur != SIZE_MAX && model_name(m, cur)) kp->cur = strdup(model_name(m, cur));
    kp->ren_i = ui_container_rename_item(f->cont);
    kp->ren = kp->ren_i != SIZE_MAX && model_name(m, kp->ren_i) ? strdup(model_name(m, kp->ren_i)) : NULL;
}

static void keep_put(struct folder *f, struct keep *kp)
{
    size_t ren_new = SIZE_MAX;
    ui_container_select_none(f->cont);
    ui_container_reset(f->cont);
    if (!f->arranged_by_name && f->m.complete) {
        layout_follow(f, true);   /* arranged by name, then stored places (a filter set or cleared) */
    } else {
        layout_update(f->lay, f->m.total, lay_name, f, true, NULL);   /* spots stay with names */
        svc_layout(f->path, got_layout, as_arg(f));                     /* positions may have changed too */
    }
    if (kp->k || kp->cur || kp->ren)
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (!n) continue;
            for (size_t j = 0; j < kp->k; j++)
                if (kp->sel[j] && !strcmp(kp->sel[j], n)) { ui_container_select(f->cont, i); free(kp->sel[j]); kp->sel[j] = NULL; }
            if (kp->cur && !strcmp(kp->cur, n)) { ui_container_set_cursor(f->cont, i); free(kp->cur); kp->cur = NULL; }
            if (kp->ren && !strcmp(kp->ren, n)) ren_new = i;
        }
    if (kp->ren_i != SIZE_MAX) ui_container_rename_moved(f->cont, ren_new);   /* gone: cancelled */
    else rename_new_now(f);
    free(kp->ren);
    for (size_t j = 0; j < kp->k; j++) free(kp->sel[j]);
    free(kp->sel);
    free(kp->cur);
    damage_status(f);
}

static void swap(struct folder *f)
{
    struct keep kp;
    keep_take(f, &kp);
    model_free(&f->m);
    f->m = *f->sh;
    free(f->sh);
    f->sh = NULL;
    f->gen++;
    keep_put(f, &kp);
    if (f->reload_again) {
        f->reload_again = false;
        reload(f);
    }
}

/* ------------------------------------------------- following a big folder */

/* One change, applied to the rows as the service applied it to its
 * listing (new names at the end, gone ones taken out, renamed ones in
 * place), so a folder of any size follows without being read again. */
ssize_t model_find(struct model *m, const char *name)
{
    for (size_t i = 0; i < m->total; i++) {
        const char *n = model_name(m, i);
        if (n && !strcmp(n, name)) return (ssize_t)i;
    }
    return -1;
}

struct row *row_ptr(struct model *m, size_t i) { return &page_at(m, i / PAGE)->rows[i % PAGE]; }

/* Its details must be read again (when it is on screen). */
void row_stale(struct model *m, size_t i)
{
    struct page *pg = page_at(m, i / PAGE);
    /* Its name is known (so it shows, and can be renamed); details are
     * read when it is on screen. */
    if (pg->state == P_DETAILED || pg->state == P_NONE) pg->state = P_NAMES;
    pg->requested = false;
}

static void model_append(struct model *m, const char *name, const char *type)
{
    size_t i = m->total++;
    struct row *r = row_ptr(m, i);
    free(r->name); free(r->title); free(r->target); free(r->icon); free(r->icon_open); free(r->noext);
    memset(r, 0, sizeof *r);
    r->name = strdup(name);
    r->kind = kind_of(type, name);
    r->link = type && !strcmp(type, "link");
    row_stale(m, i);
}

static void model_remove(struct model *m, size_t i)
{
    struct row *r = row_ptr(m, i);
    free(r->name); free(r->title); free(r->target); free(r->icon); free(r->icon_open); free(r->noext);
    for (size_t j = i; j + 1 < m->total; j++) *row_ptr(m, j) = *row_ptr(m, j + 1);   /* details move with their rows */
    memset(row_ptr(m, m->total - 1), 0, sizeof(struct row));
    m->total--;
    /* Each later page took its first row from the next one, which may not
     * have been read: read them again when shown. */
    for (size_t p = i / PAGE; p < m->npages; p++) {
        struct page *pg = m->pages[p];
        if (!pg) continue;
        if (pg->state == P_DETAILED) pg->state = P_NAMES;
        pg->requested = false;
    }
}

void changes_clear(struct folder *f)
{
    for (size_t i = 0; i < f->nchanges; i++) { free(f->changes[i].name); free(f->changes[i].to); free(f->changes[i].type); }
    f->nchanges = 0;
}

/* One change to the rows; false when it does not follow on from them.
 * Gone and renamed entries are found by position (the service's index):
 * a big folder's window has read only the names it has shown. */
static bool apply_change(struct model *m, const struct change *c)
{
    if (c->version == m->version) return true;   /* already in */
    if (c->version != m->version + 1) return false;
    if (c->kind == 'c') {
        model_append(m, c->name, c->type);
    } else {
        if (c->index < 0 || (size_t)c->index >= m->total) return false;
        size_t i = (size_t)c->index;
        const char *was = model_name(m, i);
        if (was && strcmp(was, c->name)) return false;   /* not what the service had there */
        if (c->kind == 'd') {
            model_remove(m, i);
        } else {
            struct row *r = row_ptr(m, i);
            free(r->name);
            r->name = strdup(c->to);
            free(r->title);
            r->title = NULL;   /* the title may follow the name: read again */
            if (!was) r->kind = kind_of(NULL, c->to);
            row_stale(m, i);
        }
    }
    m->version = c->version;
    return true;
}

/* The batch of changes that came together: applied in one go (the
 * window's selection and icon spots fixed up once), or, when there are too
 * many or they do not follow on from the rows, one read of the folder. */
static void apply_changes(void *arg)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    f->changes_posted = false;
    if (!f->nchanges || f->closing) { changes_clear(f); return; }
    struct model *m = &f->m;
    bool ok = f->nchanges <= MAX_FOLLOW && m->version && m->complete && !f->sh && !is_tree(f);
    for (size_t i = 0; ok && i < f->nchanges; i++) ok = f->changes[i].version != 0;
    if (!ok) { changes_clear(f); reload(f); return; }
    struct keep kp;
    keep_take(f, &kp);
    for (size_t i = 0; ok && i < f->nchanges; i++) ok = apply_change(m, &f->changes[i]);
    changes_clear(f);
    /* Replies in flight were for the old rows: drop them, ask again. */
    f->gen++;
    for (size_t p = 0; p < m->npages; p++) if (m->pages[p]) m->pages[p]->requested = false;
    m->pending = 0;
    ui_container_count_changed(f->cont);
    keep_put(f, &kp);
    ui_container_layout_changed(f->cont);
    if (!ok) { reload(f); return; }   /* partly applied: the read puts it right */
    fetch_visible(f, f->vis_first, f->vis_last);
}

/* A content event: kept for the batch applied on the next turn. */
static void queue_change(struct folder *f, const char *event, const cv *ev)
{
    int64_t v = 0, index = -1;
    cv_int(ev, "version", &v);
    cv_int(ev, "index", &index);
    const char *name = cv_text(ev, "name"), *from = cv_text(ev, "from"), *to = cv_text(ev, "to"), *type = cv_text(ev, "type");
    char kind = !strcmp(event, "content_added") ? 'c' : !strcmp(event, "content_removed") ? 'd' : 'r';
    if (kind == 'r') name = from;
    if (!name || (kind == 'r' && !to)) { reload(f); return; }
    if (f->nchanges == f->capchanges) {
        f->capchanges = f->capchanges ? f->capchanges * 2 : 16;
        f->changes = realloc(f->changes, f->capchanges * sizeof *f->changes);
    }
    f->changes[f->nchanges++] = (struct change){ kind, strdup(name), to ? strdup(to) : NULL, type && *type ? strdup(type) : NULL,
                                                 (uint64_t)v, index };
    if (!f->changes_posted) {
        f->changes_posted = true;
        ui_app_post(app, apply_changes, as_arg(f));
    }
}

void got_page(void *arg, const cv *reply)
{
    struct pctx *pc = arg;
    struct folder *f = folder_by_id(pc->id);
    bool shadow = pc->shadow;
    unsigned gen = pc->gen;
    free(pc);
    if (!f) return;
    if (f->desktop && getenv("OSD_DEBUG_TIMING")) {
        const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
        bool det = items && items->k == CV_ARRAY && items->n && cv_get(&items->items[0], "mtime");
        TIMING(f, "page: %zu items%s%s", items && items->k == CV_ARRAY ? items->n : 0, det ? " with details" : "", shadow ? " (re-read)" : "");
    }
    struct model *m = shadow ? f->sh : &f->m;
    if (!m || gen != (shadow ? f->sgen : f->gen)) return;   /* for a model replaced since */
    {
        /* A page from before a change this window has already followed. */
        const cv *r0 = reply ? cv_get(reply, "result") : NULL;
        int64_t pv = 0;
        if (!shadow && r0 && m->complete && m->version && cv_int(r0, "version", &pv) && pv && (uint64_t)pv < m->version) {
            int64_t off = 0;
            cv_int(r0, "offset", &off);
            if ((size_t)off / PAGE < m->npages && m->pages[(size_t)off / PAGE]) m->pages[(size_t)off / PAGE]->requested = false;
            fetch_visible(f, f->vis_first, f->vis_last);
            return;
        }
    }
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res) {
        const char *msg = reply ? cv_text(reply, "message") : NULL;   /* {ok:false, error, message} */
        snprintf(f->error, sizeof f->error, "%s", msg ? msg : reply ? "the folder cannot be read" : "lost the connection to osdd");
        m->growing = false;
        if (shadow) { model_free(f->sh); free(f->sh); f->sh = NULL; }
        damage_status(f);
        return;
    }
    size_t off, n;
    bool was_complete = m->complete;
    bool details = store(m, res, &off, &n);
    bool clear_error = !details;   /* reading names again clears an old read error, not a message */
    if (shadow) {
        if (details) { if (--m->pending == 0) swap(f); return; }
        if (!m->complete) { request_more(f, true); return; }
        /* All names are in: fetch details for what is on screen, then swap. */
        size_t p0 = f->vis_first / PAGE, p1 = f->vis_last / PAGE + 1;
        for (size_t p = p0; p <= p1 && p * PAGE < m->total; p++) {
            m->pending++;
            page_at(m, p)->requested = true;
            svc_list_v(f->path, f->filter, f->show_hidden, p * PAGE, PAGE, true, got_page, page_arg(f, true));
        }
        if (!m->pending) swap(f);
        return;
    }
    if (clear_error) f->error[0] = 0;
    ui_container_count_changed(f->cont);
    if (n) ui_container_changed(f->cont, off, off + n - 1);
    if (!details || m->complete != was_complete) layout_follow(f, false);
    if (m->heights_dirty) {   /* (central objects' titles come with their details) */
        m->heights_dirty = false;
        /* (the free layout: a title's width is its cell's too) */
        if ((f->whole_titles || f->free_layout == 1) && !is_tree(f)) {
            layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
            if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
        }
    }
    if (!m->complete) request_more(f, false);
    if (m->complete != was_complete || !details) damage_status(f);
    if (f->sort_pending && m->complete) sort_icons(f, f->sort_pending - 1);   /* (again: all in now?) */
}

/* Names stream in; the folder's size grows as the service reads it. */
void request_more(struct folder *f, bool shadow)
{
    struct model *m = shadow ? f->sh : &f->m;
    if (!m || m->growing || m->complete) return;
    m->growing = true;
    svc_list_v(f->path, f->filter, f->show_hidden, m->total, m->total ? 4096 : 128, false, got_page, page_arg(f, shadow));
}

/* Details (size, date, title) for the pages on screen. */
void fetch_visible(struct folder *f, size_t first, size_t last)
{
    f->vis_first = first;
    f->vis_last = last;
    size_t p0 = first / PAGE, p1 = last / PAGE + 1;
    for (size_t p = p0; p <= p1 && p * PAGE < f->m.total; p++) {
        struct page *pg = page_at(&f->m, p);
        if (pg->state == P_DETAILED || pg->requested) continue;
        pg->requested = true;
        svc_list_v(f->path, f->filter, f->show_hidden, p * PAGE, PAGE, true, got_page, page_arg(f, false));
    }
}

/* The folder changed: read it again in the background. Changes during the
 * re-read start one more when it is done, so bursts cost two reads. */
int reloads;   /* full re-reads (self-tests) */

void reload(struct folder *f)
{
    reloads++;
    if (f->sh) { f->reload_again = true; return; }
    f->sh = calloc(1, sizeof *f->sh);
    f->sgen++;
    request_more(f, true);
}

/* A program Objectspace started opened or closed a file: the file's row, and
 * shadows of it, in every window. (Sent once per subscription; rows
 * already right are left alone.) */
static void on_in_use_event(const cv *ev)
{
    const char *path = cv_text(ev, "path");
    const cv *v = cv_get(ev, "in_use");
    if (!path || !v || v->k != CV_BOOL) return;
    const char *slash = strrchr(path, '/');
    if (!slash) return;
    size_t dl = (size_t)(slash - path);
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing || is_tree(f)) continue;
        bool here = strlen(f->path) == (dl ? dl : 1) && !strncmp(f->path, dl ? path : "/", dl ? dl : 1), changed = false;
        for (size_t p = 0; p < f->m.npages; p++) {
            struct page *pg = f->m.pages[p];
            if (!pg || pg->state != P_DETAILED) continue;
            for (size_t k = 0; k < PAGE; k++) {
                struct row *r = &pg->rows[k];
                if (!r->name || r->in_use == v->b) continue;
                if ((here && !strcmp(r->name, slash + 1)) || ((r->shadow || r->link) && r->target && !strcmp(r->target, path))) {
                    r->in_use = v->b;
                    changed = true;
                }
            }
        }
        if (changed) ui_container_layout_changed(f->cont);
    }
}

/* A folder was renamed or moved: windows on it, or inside it, follow:
 * their path, their title, and what they watch. */
static void folder_moved(const char *from, const char *to)
{
    if (!from || !to) return;
    size_t fl = strlen(from);
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing || strncmp(f->path, from, fl) || (f->path[fl] && f->path[fl] != '/')) continue;
        char np[PATH_MAX];
        snprintf(np, sizeof np, "%s%s", to, f->path + fl);
        snprintf(f->path, sizeof f->path, "%s", np);
        if (!f->desktop) {
            const char *base = strrchr(f->path, '/');
            snprintf(f->title, sizeof f->title, "%s", base && base[1] ? base + 1 : f->path);
            update_title(f);
            svc_resolve(f->path, got_title, as_arg(f));   /* its own title, if it has one */
        }
        if (f->sub) svc_unsubscribe(f->sub);
        f->sub = 0;
        svc_subscribe_folder(f->path, got_sub, as_arg(f));
        bg_follow(f);
        if (is_tree(f)) tree_start(f);   /* the tree's paths changed: read it again */
        ui_surface_damage_all(f->win);
    }
    notify_in_use(to);   /* icons of it, now open under the new name */
}

/* The Trash Can went empty or full: it and its shadows are read again
 * (their icon). */
static void trash_changed(void)
{
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing || is_tree(f) || !f->m.complete) continue;
        bool any = false;
        for (size_t i = 0; i < f->m.total; i++) {
            struct row *rw = row_at(f, i);
            if (!rw || !rw->name) continue;
            char p[PATH_MAX];
            if (rw->shadow && rw->target) snprintf(p, sizeof p, "%s", rw->target);
            else if (rw->kind == UI_KIND_FOLDER) snprintf(p, sizeof p, "%s/%s", f->path, rw->name);
            else continue;
            if (!is_trash_dir(p)) continue;
            row_stale(&f->m, i);
            any = true;
        }
        if (any) fetch_visible(f, f->vis_first, f->vis_last);
    }
}

void on_event(void *arg, const cv *ev)
{
    (void)arg;
    int64_t sub;
    const char *name = ev ? cv_text(ev, "event") : NULL;
    if (!name || !cv_int(ev, "sub", &sub)) return;
    if (findwin_event(ev)) return;   /* a search's results (its own IDs) */
    if (osd_command_event(ev)) return;   /* the osd command: ask, selection, show */
    if (!strcmp(name, "say")) {   /* osdctl say: on the desktop (a script's: shown with the setting off too) */
        const cv *st = cv_get(ev, "stay");
        if (cv_text(ev, "text")) desktop_say(cv_text(ev, "text"), st && st->k == CV_BOOL && st->b);
        return;
    }
    if (messages_event(ev)) return;   /* (once per client: on whichever subscription) */
    if (!strcmp(name, "job")) { on_job_event(cv_get(ev, "status")); return; }
    if (setup_event(ev)) return;
    if (!strcmp(name, "folder_moved")) {
        folder_moved(cv_text(ev, "from"), cv_text(ev, "to"));
        launcher_event(name, sub);
        return;
    }
    if (!strcmp(name, "in_use")) { on_in_use_event(ev); return; }
    if (!strcmp(name, "trash_changed")) { trash_changed(); return; }
    if (!strcmp(name, "drives_changed")) { drives_changed_event(); return; }
    if (!strcmp(name, "defaults_changed")) {   /* all folders', all of a type's: every window reads its own again */
        const char *scope = cv_text(ev, "scope");
        if (scope && !strcmp(scope, "schemes")) { svc_setup(got_setup, NULL); return; }   /* a scheme changed */
        for (struct folder *f = folders; f; f = f->next) if (!f->closing) bg_read(f);
        return;
    }
    /* (after the events sent once per client, on whichever subscription:
     * in_use, trash_changed, ... came on the Launcher's or the trays'
     * subscription too, and were taken for their folders' changes) */
    if (launcher_event(name, sub)) return;
    if (trays_event(name, sub)) return;
    for (struct folder *f = folders; f; f = f->next) {
        if (f->bg_sub && f->bg_sub == sub) {   /* the folder object itself: its Background */
            if (!f->closing && !strcmp(name, "object_changed")) bg_read(f);
            continue;
        }
        if (!f->closing && f->troot && tree_event(f, sub)) continue;
        if (f->sub != sub || f->closing) continue;
        if (!strcmp(name, "content_added") || !strcmp(name, "content_removed") || !strcmp(name, "object_moved")) {
            queue_change(f, name, ev);
        } else if (!strcmp(name, "layout_changed")) {
            svc_layout(f->path, got_layout, as_arg(f));   /* positions stored for it (a job's drop) */
        } else if (!strcmp(name, "object_changed")) {
            /* One object's details: its row is read again, not the folder. */
            const cv *o = cv_get(ev, "object");
            const char *p = o ? cv_text(o, "path") : NULL, *sl = p ? strrchr(p, '/') : NULL;
            ssize_t i = sl && !is_tree(f) && f->m.complete ? model_find(&f->m, sl + 1) : -1;
            /* a central object (System Setup's): its row by its address */
            const char *addr = o ? cv_text(o, "address") : NULL;
            if (!p && addr && !strncmp(addr, "vobj:", 5) && !is_tree(f) && f->m.complete) {
                char nm[128];
                snprintf(nm, sizeof nm, "\x01%s", addr);
                i = model_find(&f->m, nm);
                if (i < 0) continue;   /* not in this window */
            }
            if (i >= 0) {
                row_stale(&f->m, (size_t)i);
                fetch_visible(f, f->vis_first, f->vis_last);
            } else if (!o || !p) {
                reload(f);
            }
        }
    }
}

void got_sub(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    int64_t sub;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!f || !res || res->k != CV_INT) return;
    sub = res->i;
    if (f->closing) { svc_unsubscribe(sub); return; }
    f->sub = sub;
}
