/*
 * wps-shell: the WPS GUI (G1: folder windows). One process draws every
 * WPS window (gui-design.md: Processes); the object service is wpsd.
 *
 * usage: wps-shell [--details] [--bench SECONDS] FOLDER...
 *   --bench: scroll the first window by itself, print frame statistics as
 *   JSON, and exit (WPS_UI_SCALE and WPS_SHELL_SIZE=WxH help measure).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "layout.h"
#include "wallpaper.h"
#include "events.h"
#include "winlist.h"
#include "appicons.h"
#include "warpcenter.h"
#include "notify.h"
#include "messages.h"
#include "findwin.h"
#include "mime.h"
#include "icons.h"
#include "filepick.h"
#include "session.h"
#include "iconpick.h"
#include "launchpad.h"
#include "gnome.h"
#include "sound.h"
#include "svc.h"
#include "ui.h"
#include "wps_menu_ids.h"

#define PAGE 256
/* The status line: tall enough for the interface font (GNOME's may be larger). */
static double status_h(void) { double h = ceil(ui_scheme_now.font_px + 9); return h < 20 ? 20 : h; }
#define STATUS_H status_h()

enum { P_NONE, P_NAMES, P_DETAILED };

struct row { char *name, *title; ui_kind kind; int64_t size, mtime; bool shadow, in_use, link, tpl; char *target, *icon;
             float cellh;   /* its icon's cell with the whole title (0: not measured yet) */ };
struct page { int state; bool requested; struct row rows[PAGE]; };

/* A folder's contents as read from the service, a page at a time. */
/* A content event, kept until the batch it came in is applied. */
struct change { char kind; char *name, *to, *type; uint64_t version; int64_t index; };

struct model {
    struct page **pages;
    size_t npages, total;
    uint64_t version;        /* the service's listing version these rows match (0: unknown) */
    bool complete, growing;
    bool heights_dirty;      /* a title changed: whole-title rows may be taller or shorter */
    int pending;             /* detail pages still awaited before a swap */
};

struct folder {
    struct folder *next;
    long id;                 /* replies find their window by ID: it may have closed */
    ui_surface *win;
    ui_container *cont;
    char path[PATH_MAX];
    char title[256];
    struct model m;          /* on screen */
    struct model *sh;        /* being re-read after a change, swapped in when ready */
    unsigned gen, sgen;      /* replies for an older model are dropped */
    bool reload_again;       /* a change came during the re-read */
    int64_t sub;             /* event subscription */
    bool closing;
    bool desktop;            /* the desktop: frameless, full screen, on the wallpaper; never closes */
    size_t vis_first, vis_last;
    struct tnode *troot;     /* tree view */
    struct tnode **tvis;     /* its visible rows */
    size_t ntvis, captvis;
    unsigned tgen;           /* replies for a freed tree are dropped */
    char pending_open[PATH_MAX];   /* the program asked about, if any */
    char rename_new[NAME_MAX + 64];  /* a new object made here: edit its name once it is listed */
    struct change *changes;          /* content events not yet applied (applied together, next turn) */
    size_t nchanges, capchanges;
    bool changes_posted;
    ui_view pending_view;          /* how to open a folder the service names */
    icon_layout *lay;        /* icon view: where each icon is */
    bool lay_started;        /* its grid width is set */
    bool arranged_by_name;   /* the first, unstored arrangement was done */
    int sort_pending;        /* a Sort waiting for every row's details: its key + 1 */
    char **spot_new;         /* new objects whose spots are to be stored (store_spots) */
    size_t nspot_new;
    char error[256];
    /* its Background page (v0.3-design 3), followed by a subscription to
     * the folder object itself */
    int64_t bg_sub;
    char bg_image[PATH_MAX], bg_color[16], bg_img_path[PATH_MAX];
    int bg_mode;
    bool bg_gnome;
    bool whole_titles;       /* the Icon view page: titles whole (OS/2's) or on two lines */
    bool lock_all;           /* ... Lock positions of all icons */
    cairo_surface_t *bg_img;     /* the picture, decoded */
    /* its details view (v0.3-design 4): the sort (choice 0 none, 1 name,
     * 2 type, 3 size, 4 last write; -1 not read yet) and columns */
    int sort_choice;
    bool sort_desc;
    unsigned cols;
    double col_w[6];
    /* its search field (v0.3-design 6): shown with Ctrl+F, filters its own
     * listing as typed */
    bool find_on, find_focus;
    ui_entry *find_entry;
    char filter[128];
    bool find_x_down;
    /* benchmark */
    double bench_secs, bench_start, bench_next_jump;
    unsigned bench_seed;
    size_t bench_from;
    size_t frames_seen, verified, verify_failed, verify_diff;
};

static ui_app *app;
static struct folder *folders;
static long next_folder_id = 1;
static double opt_bench;
static ui_view opt_view = UI_VIEW_ICON;

static struct folder *folder_by_id(long id)
{
    for (struct folder *f = folders; f; f = f->next)
        if (f->id == id) return f;
    return NULL;
}

static void *as_arg(struct folder *f) { return (void *)(intptr_t)f->id; }
static bool show_hidden = true;   /* the System object's Show hidden files (v0.3-design 6) */
static struct folder *from_arg(void *arg) { return folder_by_id((long)(intptr_t)arg); }

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

struct bg_load { long folder; char path[PATH_MAX]; };

static void bg_loaded(void *ctx, const char *png)
{
    struct bg_load *l = ctx;
    struct folder *f = folder_by_id(l->folder);
    if (f && !f->closing && !strcmp(f->bg_img_path, l->path)) {
        if (f->bg_img) cairo_surface_destroy(f->bg_img);
        f->bg_img = png ? cairo_image_surface_create_from_png(png) : NULL;
        if (f->bg_img && cairo_surface_status(f->bg_img) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(f->bg_img); f->bg_img = NULL; }
        if (!f->bg_img) fprintf(stderr, "wps-shell: %s: cannot read the background picture %s\n", f->path, l->path);
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

static void reload(struct folder *f);
static void ignore_reply(void *arg, const cv *reply);
static const char *lay_name(void *ctx, size_t i);

/* A heading clicked: sorted by it (again: the other way), for the folder. */
static void head_clicked(void *ctx, int col)
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
static void cols_resized(void *ctx, const double *w)
{
    struct folder *f = from_arg(ctx);
    if (!f) return;
    char t[128];
    snprintf(t, sizeof t, "%.0f,%.0f,%.0f,%.0f,%.0f", w[1], w[2], w[3], w[4], w[5]);
    cv v = { .k = CV_TEXT, .s = t, .len = strlen(t) };
    svc_settings_set(f->path, "col_widths", &v, -1, ignore_reply, NULL);
}

static void got_bg(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!f || f->closing || !res || res->k != CV_ARRAY) return;
    f->bg_image[0] = f->bg_color[0] = 0;
    f->bg_mode = 2;
    f->bg_gnome = true;
    int was_choice = f->sort_choice;
    bool was_desc = f->sort_desc;
    unsigned cols = 0;
    double w[6] = { 0 };
    bool whole = true, lock_all = false;
    for (size_t i = 0; i < res->n; i++) {
        const cv *fs = cv_get(&res->items[i], "fields");
        for (size_t k = 0; fs && fs->k == CV_ARRAY && k < fs->n; k++) {
            const cv *fd = &fs->items[k], *v = cv_get(fd, "value");
            const char *key = cv_text(fd, "key");
            if (!key || !v) continue;
            if (!strcmp(key, "bg_image") && v->k == CV_TEXT) snprintf(f->bg_image, sizeof f->bg_image, "%s", v->s);
            else if (!strcmp(key, "bg_color") && v->k == CV_TEXT) snprintf(f->bg_color, sizeof f->bg_color, "%s", v->s);
            else if (!strcmp(key, "bg_mode") && v->k == CV_INT) f->bg_mode = (int)v->i;
            else if (!strcmp(key, "bg_gnome") && v->k == CV_BOOL) f->bg_gnome = v->b;
            else if (!strcmp(key, "sort_key") && v->k == CV_INT) f->sort_choice = (int)v->i;
            else if (!strcmp(key, "sort_desc") && v->k == CV_BOOL) f->sort_desc = v->b;
            else if (!strcmp(key, "icon_titles") && v->k == CV_INT) whole = v->i == 0;
            else if (!strcmp(key, "lock_all") && v->k == CV_BOOL) lock_all = v->b;
            else if (!strncmp(key, "col_", 4) && v->k == CV_BOOL && v->b) {
                cols |= !strcmp(key, "col_name") ? 1u : !strcmp(key, "col_type") ? 2u : !strcmp(key, "col_size") ? 4u
                      : !strcmp(key, "col_date") ? 8u : 0u;
            } else if (!strcmp(key, "col_widths") && v->k == CV_TEXT) {
                sscanf(v->s, "%lf,%lf,%lf,%lf,%lf", &w[1], &w[2], &w[3], &w[4], &w[5]);
            }
        }
    }
    bg_apply(f);
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
}

static void bg_read(struct folder *f) { svc_settings(f->path, got_bg, as_arg(f)); }

static void got_bg_sub(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res || res->k != CV_INT) return;
    if (!f || f->closing) { svc_unsubscribe(res->i); return; }
    f->bg_sub = res->i;
}

/* Its Background read, and followed (changes made in its notebook). */
static void bg_follow(struct folder *f)
{
    if (f->bg_sub) svc_unsubscribe(f->bg_sub);
    f->bg_sub = 0;
    const char *t[1] = { f->path };
    svc_subscribe_objects(t, 1, got_bg_sub, as_arg(f));
    bg_read(f);
}

static struct folder *open_folder(const char *path, ui_view view);
static bool tree_event(struct folder *f, int64_t sub);
static void on_job_event(const cv *st);
static const char *lay_name(void *ctx, size_t i);
static void got_layout(void *arg, const cv *reply);
static void layout_follow(struct folder *f, bool full);
static void set_view(struct folder *f, ui_view v);

/* ------------------------------------------------------------------ model */

static struct page *page_at(struct model *m, size_t p)
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

static void model_free(struct model *m)
{
    for (size_t p = 0; p < m->npages; p++) {
        if (!m->pages[p]) continue;
        for (size_t r = 0; r < PAGE; r++) {
            free(m->pages[p]->rows[r].name);
            free(m->pages[p]->rows[r].title);
            free(m->pages[p]->rows[r].target);
            free(m->pages[p]->rows[r].icon);
        }
        free(m->pages[p]);
    }
    free(m->pages);
    memset(m, 0, sizeof *m);
}

static const char *model_name(struct model *m, size_t i)
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

static const char *view_name(ui_view v) { return v == UI_VIEW_ICON ? "Icon View" : v == UI_VIEW_TREE ? "Tree View" : "Details View"; }

static void update_title(struct folder *f)
{
    char t[400];
    snprintf(t, sizeof t, "%s - %s", f->title, view_name(ui_container_view(f->cont)));
    ui_window_set_title(f->win, f->desktop ? "Desktop" : t);
}

static void damage_status(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    ui_surface_damage(f->win, (ui_rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H });
}

/* Which model and generation a reply belongs to. */
struct pctx { long id; unsigned gen; bool shadow; };

static void *page_arg(struct folder *f, bool shadow)
{
    struct pctx *pc = malloc(sizeof *pc);
    pc->id = f->id;
    pc->gen = shadow ? f->sgen : f->gen;
    pc->shadow = shadow;
    return pc;
}

static void request_more(struct folder *f, bool shadow);
static bool setup_event(const cv *ev);
#define NSETUP 8
static char setup_ids[NSETUP][64];   /* the settings objects (see below) */
static char setup_cls[NSETUP][32];   /* their classes */
static void rename_new_now(struct folder *f);
static bool is_tree(struct folder *f);
static void fetch_visible(struct folder *f, size_t first, size_t last);
static struct row *row_at(struct folder *f, size_t i);
static void lock_items(struct folder *f, size_t i);
static void reload(struct folder *f);
static void got_page(void *arg, const cv *reply);
static void placed_reply(void *arg, const cv *reply);
static void spots_later(void);
static void new_spot(void *ctx, const char *name);
static void sort_icons(struct folder *f, int key);
static void set_status(long folder, const char *msg);
static void empty_trash_ask(struct folder *f);
static void drive_action(struct folder *f, const char *target, const char *action, bool reopen);
static void unlock_ask(struct folder *f, const char *target, bool reopen);
static void drives_changed_event(void);
static ui_dialog *last_dialog;   /* (declared again below, with the jobs) */
static void restore_items(struct folder *f, size_t i);
static bool is_trash_dir(const char *p);
static bool is_startup_dir(const char *p);
static void startup_dropped(struct folder *f, const char *mime, const char *data);
struct del;
static void trash_dropped(struct folder *f, const char *mime, const char *data);
static bool dragged_onto_itself(struct folder *f, size_t item);
static void win_close(ui_surface *s);

/* WPS_DEBUG_TIMING: when the desktop's start-up steps happen. */
static double since_start(void)
{
    static double t0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
    if (!t0) t0 = now;
    return now - t0;
}
#define TIMING(f, fmt, ...) do { if ((f)->desktop && getenv("WPS_DEBUG_TIMING")) \
    fprintf(stderr, "timing: %7.0f ms: " fmt "\n", since_start(), ##__VA_ARGS__); } while (0)

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
        dst->target = dst->icon = NULL;
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
            const char *mk = cv_text(it, "makes");   /* a built-in template: looks like what it makes */
            if (mk) dst->kind = !strcmp(mk, "WPFolder") ? UI_KIND_FOLDER : !strcmp(mk, "WPProgram") ? UI_KIND_PROGRAM : UI_KIND_FILE;
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

/* The re-read model replaces the one on screen, keeping the scroll
 * position, the selection and the cursor (by name). */
/* What the user has in a window (selection, cursor, a name being edited),
 * kept by name across a change of its rows. */
struct keep { char **sel; size_t k; char *cur, *ren; size_t ren_i; };

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
static ssize_t model_find(struct model *m, const char *name)
{
    for (size_t i = 0; i < m->total; i++) {
        const char *n = model_name(m, i);
        if (n && !strcmp(n, name)) return (ssize_t)i;
    }
    return -1;
}

static struct row *row_ptr(struct model *m, size_t i) { return &page_at(m, i / PAGE)->rows[i % PAGE]; }

/* Its details must be read again (when it is on screen). */
static void row_stale(struct model *m, size_t i)
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
    free(r->name); free(r->title); free(r->target); free(r->icon);
    memset(r, 0, sizeof *r);
    r->name = strdup(name);
    r->kind = kind_of(type, name);
    r->link = type && !strcmp(type, "link");
    row_stale(m, i);
}

static void model_remove(struct model *m, size_t i)
{
    struct row *r = row_ptr(m, i);
    free(r->name); free(r->title); free(r->target); free(r->icon);
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

/* Changes in a batch beyond this are cheaper as one read of the folder. */
#define MAX_FOLLOW 64

static void changes_clear(struct folder *f)
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

static void got_page(void *arg, const cv *reply)
{
    struct pctx *pc = arg;
    struct folder *f = folder_by_id(pc->id);
    bool shadow = pc->shadow;
    unsigned gen = pc->gen;
    free(pc);
    if (!f) return;
    if (f->desktop && getenv("WPS_DEBUG_TIMING")) {
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
        snprintf(f->error, sizeof f->error, "%s", msg ? msg : reply ? "the folder cannot be read" : "lost the connection to wpsd");
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
            svc_list_v(f->path, f->filter, show_hidden, p * PAGE, PAGE, true, got_page, page_arg(f, true));
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
        if (f->whole_titles && !is_tree(f)) {
            layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
            if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
        }
    }
    if (!m->complete) request_more(f, false);
    if (m->complete != was_complete || !details) damage_status(f);
    if (f->sort_pending && m->complete) sort_icons(f, f->sort_pending - 1);   /* (again: all in now?) */
}

/* Names stream in; the folder's size grows as the service reads it. */
static void request_more(struct folder *f, bool shadow)
{
    struct model *m = shadow ? f->sh : &f->m;
    if (!m || m->growing || m->complete) return;
    m->growing = true;
    svc_list_v(f->path, f->filter, show_hidden, m->total, m->total ? 4096 : 128, false, got_page, page_arg(f, shadow));
}

/* Details (size, date, title) for the pages on screen. */
static void fetch_visible(struct folder *f, size_t first, size_t last)
{
    f->vis_first = first;
    f->vis_last = last;
    size_t p0 = first / PAGE, p1 = last / PAGE + 1;
    for (size_t p = p0; p <= p1 && p * PAGE < f->m.total; p++) {
        struct page *pg = page_at(&f->m, p);
        if (pg->state == P_DETAILED || pg->requested) continue;
        pg->requested = true;
        svc_list_v(f->path, f->filter, show_hidden, p * PAGE, PAGE, true, got_page, page_arg(f, false));
    }
}

/* The folder changed: read it again in the background. Changes during the
 * re-read start one more when it is done, so bursts cost two reads. */
static int reloads;   /* full re-reads (self-tests) */

static void reload(struct folder *f)
{
    reloads++;
    if (f->sh) { f->reload_again = true; return; }
    f->sh = calloc(1, sizeof *f->sh);
    f->sgen++;
    request_more(f, true);
}

static bool is_tree(struct folder *f);

/* A program WPS started opened or closed a file: the file's row, and
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

static void got_sub(void *arg, const cv *reply);
static void update_title(struct folder *f);
static void got_title(void *arg, const cv *reply);
static void tree_start(struct folder *f);
static void notify_in_use(const char *path);

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

static void on_event(void *arg, const cv *ev)
{
    (void)arg;
    int64_t sub;
    const char *name = ev ? cv_text(ev, "event") : NULL;
    if (!name || !cv_int(ev, "sub", &sub)) return;
    if (findwin_event(ev)) return;   /* a search's results (its own IDs) */
    if (messages_event(ev)) return;   /* (once per client: on whichever subscription) */
    if (!strcmp(name, "job")) { on_job_event(cv_get(ev, "status")); return; }
    if (setup_event(ev)) return;
    if (launchpad_event(name, sub)) return;
    if (!strcmp(name, "folder_moved")) { folder_moved(cv_text(ev, "from"), cv_text(ev, "to")); return; }
    if (!strcmp(name, "in_use")) { on_in_use_event(ev); return; }
    if (!strcmp(name, "trash_changed")) { trash_changed(); return; }
    if (!strcmp(name, "drives_changed")) { drives_changed_event(); return; }
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

static void got_sub(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    int64_t sub;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!f || !res || res->k != CV_INT) return;
    sub = res->i;
    if (f->closing) { svc_unsubscribe(sub); return; }
    f->sub = sub;
}


/* -------------------------------------------------------------- tree view */

/* The tree view shows folders only, as in OS/2. Each node's subfolders are
 * read when it is first expanded, sorted by name, and kept when it is
 * collapsed. An expanded node is watched for changes. */
struct tnode {
    struct tnode *parent;
    char *name, *path;
    int depth;
    bool expanded, loaded, loading;
    struct tnode **kids;
    size_t nkids;
    char **pending;          /* subfolder names while a read streams in */
    size_t npending, cappending;
    int64_t sub;
};

static void tnode_free(struct tnode *n)
{
    if (!n) return;
    if (n->sub) svc_unsubscribe(n->sub);
    for (size_t i = 0; i < n->nkids; i++) tnode_free(n->kids[i]);
    for (size_t i = 0; i < n->npending; i++) free(n->pending[i]);
    free(n->pending);
    free(n->kids);
    free(n->name);
    free(n->path);
    free(n);
}

static struct tnode *tnode_new(struct tnode *parent, const char *name, const char *path)
{
    struct tnode *n = calloc(1, sizeof *n);
    n->parent = parent;
    n->name = strdup(name);
    n->path = strdup(path);
    n->depth = parent ? parent->depth + 1 : 0;
    return n;
}

static void tree_flatten(struct folder *f, struct tnode *n)
{
    if (f->ntvis == f->captvis) {
        f->captvis = f->captvis ? f->captvis * 2 : 64;
        f->tvis = realloc(f->tvis, f->captvis * sizeof *f->tvis);
    }
    f->tvis[f->ntvis++] = n;
    if (n->expanded)
        for (size_t i = 0; i < n->nkids; i++) tree_flatten(f, n->kids[i]);
}

/* Rebuild the visible rows, keeping the cursor on the same node. */
static void tree_rebuild(struct folder *f, struct tnode *keep)
{
    f->ntvis = 0;
    if (f->troot) tree_flatten(f, f->troot);
    if (ui_container_view(f->cont) != UI_VIEW_TREE) return;
    ui_container_select_none(f->cont);
    ui_container_reset(f->cont);
    for (size_t i = 0; keep && i < f->ntvis; i++)
        if (f->tvis[i] == keep) { ui_container_set_cursor(f->cont, i); ui_container_select(f->cont, i); }
    damage_status(f);
}

static int cmp_names(const void *a, const void *b) { return strcoll(*(char *const *)a, *(char *const *)b); }

struct tctx { long folder; unsigned tgen; struct tnode *n; size_t offset; };

static void tree_load(struct folder *f, struct tnode *n, size_t offset);

static void got_kids(void *arg, const cv *reply)
{
    struct tctx *t = arg;
    struct folder *f = folder_by_id(t->folder);
    struct tnode *n = t->n;
    size_t offset = t->offset;
    bool valid = f && t->tgen == f->tgen;
    free(t);
    if (!valid) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const cv *items = res ? cv_get(res, "items") : NULL, *cmp = res ? cv_get(res, "complete") : NULL;
    size_t cnt = items && items->k == CV_ARRAY ? items->n : 0;
    for (size_t i = 0; i < cnt; i++) {
        const char *ty = cv_text(&items->items[i], "type"), *nm = cv_text(&items->items[i], "name");
        if (!ty || strcmp(ty, "folder") || !nm) continue;
        if (n->npending == n->cappending) {
            n->cappending = n->cappending ? n->cappending * 2 : 16;
            n->pending = realloc(n->pending, n->cappending * sizeof *n->pending);
        }
        n->pending[n->npending++] = strdup(nm);
    }
    if (res && !(cmp && cmp->k == CV_BOOL && cmp->b)) { tree_load(f, n, offset + cnt); return; }
    /* All read: the new subfolders, keeping nodes (and their expansion)
     * that are still there. */
    if (n->npending) qsort(n->pending, n->npending, sizeof *n->pending, cmp_names);
    struct tnode **kids = calloc(n->npending ? n->npending : 1, sizeof *kids);
    for (size_t i = 0; i < n->npending; i++) {
        struct tnode *old = NULL;
        for (size_t k = 0; k < n->nkids && !old; k++)
            if (n->kids[k] && !strcmp(n->kids[k]->name, n->pending[i])) { old = n->kids[k]; n->kids[k] = NULL; }
        if (!old) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", strcmp(n->path, "/") ? n->path : "", n->pending[i]);
            old = tnode_new(n, n->pending[i], p);
        }
        kids[i] = old;
        free(n->pending[i]);
    }
    for (size_t k = 0; k < n->nkids; k++) tnode_free(n->kids[k]);   /* gone */
    free(n->kids);
    n->kids = kids;
    n->nkids = n->npending;
    n->npending = 0;
    n->loaded = true;
    n->loading = false;
    if (!reply) snprintf(f->error, sizeof f->error, "lost the connection to wpsd");
    struct tnode *cur = NULL;
    size_t ci = ui_container_cursor(f->cont);
    if (ci != SIZE_MAX && ci < f->ntvis) cur = f->tvis[ci];
    tree_rebuild(f, cur);
}

static void tree_load(struct folder *f, struct tnode *n, size_t offset)
{
    struct tctx *t = malloc(sizeof *t);
    t->folder = f->id;
    t->tgen = f->tgen;
    t->n = n;
    t->offset = offset;
    n->loading = true;
    svc_list_v(n->path, "", show_hidden, offset, 4096, false, got_kids, t);
}

struct tsub { long folder; unsigned tgen; struct tnode *n; };

static void got_tree_sub(void *arg, const cv *reply)
{
    struct tsub *t = arg;
    struct folder *f = folder_by_id(t->folder);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (f && t->tgen == f->tgen && res && res->k == CV_INT) {
        if (t->n->expanded && !t->n->sub) t->n->sub = res->i;
        else svc_unsubscribe(res->i);   /* collapsed meanwhile */
    }
    free(t);
}

static void tree_expand(struct folder *f, struct tnode *n, bool expand)
{
    if (expand == n->expanded) return;
    n->expanded = expand;
    if (expand) {
        if (!n->loaded && !n->loading) tree_load(f, n, 0);
        if (!n->sub && n != f->troot) {   /* the root is the window's own subscription */
            struct tsub *t = malloc(sizeof *t);
            t->folder = f->id;
            t->tgen = f->tgen;
            t->n = n;
            svc_subscribe_folder(n->path, got_tree_sub, t);
        }
    } else if (n->sub) {
        svc_unsubscribe(n->sub);
        n->sub = 0;
    }
    tree_rebuild(f, n);
}

/* A watched folder changed: read its subfolders again. */
static bool tree_event(struct folder *f, int64_t sub)
{
    struct tnode *stack[256];
    int sp = 0;
    if (f->troot) stack[sp++] = f->troot;
    while (sp) {
        struct tnode *n = stack[--sp];
        if (n->sub == sub || (n == f->troot && sub == f->sub)) {
            if (n->loaded && !n->loading) { n->npending = 0; tree_load(f, n, 0); }
            if (n != f->troot) return true;
        }
        for (size_t i = 0; i < n->nkids && sp < 256; i++)
            if (n->kids[i]->expanded) stack[sp++] = n->kids[i];
    }
    return false;
}

static void tree_start(struct folder *f)
{
    if (f->troot) return;
    f->tgen++;
    const char *base = strrchr(f->path, '/');
    f->troot = tnode_new(NULL, base && base[1] ? base + 1 : f->path, f->path);
    tree_expand(f, f->troot, true);
}

static void tree_free(struct folder *f)
{
    tnode_free(f->troot);
    f->troot = NULL;
    free(f->tvis);
    f->tvis = NULL;
    f->ntvis = f->captvis = 0;
    f->tgen++;
}

static bool is_tree(struct folder *f) { return ui_container_view(f->cont) == UI_VIEW_TREE; }

/* ----------------------------------------------------------- container src */
static void src_item(void *ctx, size_t i, ui_item *out);

/* A folder's window opened or closed: its icon, and any shadow of it, may
 * be in any window; they redraw (this happens only on opening or closing). */
static void notify_in_use(const char *path)
{
    (void)path;
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing) ui_container_layout_changed(o->cont);
}
static void target_of(struct folder *f, size_t i, char *out, size_t n);

static size_t src_count(void *ctx)
{
    struct folder *f = ctx;
    return is_tree(f) ? f->ntvis : f->m.total;
}

/* A program's icon was made (an SVG, by glycin): every window draws again. */
static void appicons_ready(void)
{
    warpcenter_damage();   /* (the tray's icons) */
    for (struct folder *f = folders; f; f = f->next)
        if (!f->closing && f->cont) ui_container_changed(f->cont, 0, SIZE_MAX);
}

static void src_item(void *ctx, size_t i, ui_item *out)
{
    struct folder *f = ctx;
    memset(out, 0, sizeof *out);
    if (is_tree(f)) {
        if (i >= f->ntvis) return;
        struct tnode *n = f->tvis[i];
        out->loaded = true;
        out->kind = UI_KIND_FOLDER;
        out->name = out->title = n->name;
        out->depth = n->depth;
        out->expanded = n->expanded;
        out->expandable = !n->loaded || n->nkids > 0;   /* unknown until read */
        for (struct folder *o = folders; o && !out->in_use; o = o->next)
            out->in_use = o != f && !o->closing && !strcmp(o->path, n->path);
        out->last_child = !n->parent || n->parent->kids[n->parent->nkids - 1] == n;
        for (struct tnode *a = n->parent; a && a->parent; a = a->parent)
            if (a->parent->kids[a->parent->nkids - 1] != a) out->open_levels |= 1u << a->depth;
        return;
    }
    struct page *pg = i / PAGE < f->m.npages ? f->m.pages[i / PAGE] : NULL;
    if (!pg || pg->state == P_NONE || !pg->rows[i % PAGE].name) return;
    struct row *r = &pg->rows[i % PAGE];
    out->loaded = true;
    out->detailed = pg->state == P_DETAILED;
    out->kind = r->kind;
    out->name = r->name;
    out->title = r->title ? r->title : r->name[0] == '\x01' ? "" : r->name;   /* a central object: its title comes with details */
    out->size = r->size;
    out->mtime = r->mtime;
    out->shadow = r->shadow;
    out->in_use = r->in_use;
    out->link = r->link;
    out->is_template = r->tpl;
    out->image = icons_get(r->icon);   /* its own icon, if it has one */
    if (!out->image && r->kind == UI_KIND_PROGRAM) {
        /* a program (or its shadow): its desktop entry's icon */
        char p[PATH_MAX];
        if (r->shadow || r->link) snprintf(p, sizeof p, "%s", r->target ? r->target : "");
        else snprintf(p, sizeof p, "%s/%s", strcmp(f->path, "/") ? f->path : "", r->name);
        size_t l = strlen(p);
        if (l > 8 && !strcmp(p + l - 8, ".desktop")) out->image = appicon_for(p);
    }
    if (r->kind == UI_KIND_FOLDER) {
        /* The folder, or a shadow of it, looks open while its window is (OS/2). */
        char p[PATH_MAX];
        if (r->shadow || r->link) snprintf(p, sizeof p, "%s", r->target ? r->target : "");
        else snprintf(p, sizeof p, "%s/%s", strcmp(f->path, "/") ? f->path : "", r->name);
        for (struct folder *o = folders; o && !out->in_use; o = o->next)
            out->in_use = !o->closing && !strcmp(o->path, p);
    }
}

static void src_visible(void *ctx, size_t first, size_t last)
{
    struct folder *f = ctx;
    if (!is_tree(f)) fetch_visible(f, first, last);
}

static void src_expand(void *ctx, size_t i, bool expand)
{
    struct folder *f = ctx;
    if (is_tree(f) && i < f->ntvis) tree_expand(f, f->tvis[i], expand);
}

static const char *row_name(struct folder *f, size_t i) { return model_name(&f->m, i); }

static void target_of(struct folder *f, size_t i, char *out, size_t n)
{
    const char *cn = is_tree(f) || i == SIZE_MAX ? NULL : model_name(&f->m, i);
    if (cn && cn[0] == '\x01') { snprintf(out, n, "%s", cn + 1); return; }   /* a central object: vobj:<id> */
    if (is_tree(f)) {
        snprintf(out, n, "%s", i < f->ntvis ? f->tvis[i]->path : f->path);
        return;
    }
    const char *nm = i == SIZE_MAX ? NULL : row_name(f, i);
    if (nm) snprintf(out, n, "%s/%s", strcmp(f->path, "/") ? f->path : "", nm);
    else snprintf(out, n, "%s", f->path);
}

static void ignore_reply(void *arg, const cv *reply) { (void)arg; (void)reply; }
static void opened(void *arg, const cv *reply);
static bool open_properties(const char *target);
static void raise_in(struct folder *f, const char *name, const char *object, const char *cls);
static ui_dialog *ask(const char *title, const char *msg, const char *const *buttons, size_t n, int def, ui_dialog_fn fn,
                      void *ctx);

/* A program another user owns: show its command and ask (the service
 * never runs it unconfirmed). */
struct confirm_ctx { long folder; char target[PATH_MAX]; };

static void confirmed(void *arg, int choice)
{
    struct confirm_ctx *c = arg;
    struct folder *f = folder_by_id(c->folder);
    if (choice == 0 && f) svc_open(c->target, true, opened, as_arg(f));
    free(c);
}

static void confirm_program(struct folder *f, const cv *res)
{
    const char *cmd = cv_text(res, "command");
    int64_t uid = -1;
    cv_int(res, "owner_uid", &uid);
    struct confirm_ctx *c = calloc(1, sizeof *c);
    c->folder = f->id;
    snprintf(c->target, sizeof c->target, "%s", f->pending_open);
    char msg[1400];
    snprintf(msg, sizeof msg, "This program belongs to another user (user ID %lld). It would run, as you:\n%s\nRun it?",
             (long long)uid, cmd ? cmd : "?");
    const char *buttons[] = { "Run", "Cancel" };
    ask("Run a program", msg, buttons, 2, 1, confirmed, c);
}

static void opened(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *msg = reply ? cv_text(reply, "message") : "lost the connection to wpsd";
    if (res && cv_get(res, "needs_confirmation")) {
        confirm_program(f, res);
        return;
    }
    if (svc_reply_view(reply)) return;
    if (res && cv_text(res, "folder")) {   /* a shadow of a folder, or a link to one */
        open_folder(cv_text(res, "folder"), f->pending_view);
        f->error[0] = 0;
        damage_status(f);
        return;
    }
    else if (!res && msg && strstr(msg, "opens in the GUI") && f->pending_open[0]) {
        /* An abstract object with no view of its own (a built-in
         * template): its settings notebook. */
        open_properties(f->pending_open);
        f->error[0] = 0;
    } else if (!res && reply && cv_text(reply, "error") && !strcmp(cv_text(reply, "error"), "locked") && f->pending_open[0]) {
        unlock_ask(f, f->pending_open, true);   /* a locked drive: its passphrase, then open it */
        f->error[0] = 0;
    } else if (!res && msg) {
        snprintf(f->error, sizeof f->error, "%s", msg);
        raise_in(f, "alert.error", f->pending_open, "");
    } else {
        f->error[0] = 0;
        if (res && cv_get(res, "launched")) raise_in(f, "object.open", f->pending_open, "file");
    }
    damage_status(f);
}

static struct row *row_at(struct folder *f, size_t i)
{
    if (is_tree(f) || i == SIZE_MAX || i / PAGE >= f->m.npages || !f->m.pages[i / PAGE]) return NULL;
    return &f->m.pages[i / PAGE]->rows[i % PAGE];
}

static void open_item(struct folder *f, size_t i, ui_view view)
{
    struct row *rw = row_at(f, i);
    if (rw && (rw->shadow || rw->link)) {
        /* A shadow opens its original, a link what it points to; the
         * service finds them (a shadow by ID) and says if it is a folder. */
        char t[PATH_MAX];
        target_of(f, i, t, sizeof t);
        snprintf(f->pending_open, sizeof f->pending_open, "%s", t);
        f->pending_view = view;
        svc_open(t, false, opened, as_arg(f));
        return;
    }
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    struct page *pg = is_tree(f) || i == SIZE_MAX || i / PAGE >= f->m.npages ? NULL : f->m.pages[i / PAGE];
    bool is_folder = i == SIZE_MAX || is_tree(f) || (pg && pg->rows[i % PAGE].kind == UI_KIND_FOLDER);
    if (is_folder) open_folder(t, view);
    else {
        snprintf(f->pending_open, sizeof f->pending_open, "%s", t);
        svc_open(t, false, opened, as_arg(f));
    }
}

static void src_activate(void *ctx, size_t i) { open_item(ctx, i, UI_VIEW_ICON); }



/* ---------------------------------------------------------- icon layout */

static const char *lay_name(void *ctx, size_t i) { return model_name(&((struct folder *)ctx)->m, i); }

/* Item i's cell height: with its whole title (OS/2's layout), measured
 * once per title. */
static double lay_h(void *ctx, size_t i)
{
    struct folder *f = ctx;
    if (!f->whole_titles) return UI_CELL_H;
    struct row *r = row_at(f, i);
    if (!r || !r->name) return UI_CELL_H;
    if (r->cellh <= 0) r->cellh = (float)ui_icon_cell_height(r->title ? r->title : r->name[0] == '\x01' ? "" : r->name);
    return r->cellh;
}

static int cols_now(struct folder *f)
{
    int k = (int)(ui_container_list_width(f->cont) / UI_CELL_W);
    return k < 1 ? 1 : k;
}

/* Names in name order (for Arrange). */
static struct folder *sort_folder;
static int cmp_index_name(const void *a, const void *b)
{
    const char *x = model_name(&sort_folder->m, *(const size_t *)a), *y = model_name(&sort_folder->m, *(const size_t *)b);
    return strcoll(x ? x : "", y ? y : "");
}

/* Sort: by type (the extension; folders first), size, last write - each
 * then by name. */
static int sort_key;
static struct row *row_at(struct folder *f, size_t i);
static int cmp_index_key(const void *a, const void *b)
{
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    struct row *x = row_at(sort_folder, i), *y = row_at(sort_folder, j);
    const char *nx = model_name(&sort_folder->m, i), *ny = model_name(&sort_folder->m, j);
    nx = nx ? nx : "";
    ny = ny ? ny : "";
    int r = 0;
    if (x && y) {
        if (sort_key == 1) {
            bool fx = x->kind == UI_KIND_FOLDER, fy = y->kind == UI_KIND_FOLDER;
            if (fx != fy) r = fx ? -1 : 1;
            else {
                const char *ex = strrchr(nx, '.'), *ey = strrchr(ny, '.');
                r = strcasecmp(ex && ex != nx ? ex : "", ey && ey != ny ? ey : "");
            }
        } else if (sort_key == 2) r = x->size < y->size ? -1 : x->size > y->size;
        else if (sort_key == 3) r = x->mtime > y->mtime ? -1 : x->mtime < y->mtime;   /* newest first */
    }
    return r ? r : strcoll(nx, ny);
}

static size_t *name_order(struct folder *f)
{
    size_t n = f->m.total, *o = malloc((n ? n : 1) * sizeof *o);
    for (size_t i = 0; i < n; i++) o[i] = i;
    sort_folder = f;
    qsort(o, n, sizeof *o, cmp_index_name);
    return o;
}

/* New rows from the service (or a re-read): extend or redo the layout. */
static void layout_follow(struct folder *f, bool full)
{
    if (!f->lay_started) { layout_set_cols(f->lay, cols_now(f)); f->lay_started = true; }
    /* A small folder with no stored positions opens arranged by name (not
     * in the order the directory happens to list), once it is all read. */
    if (f->m.complete && !f->arranged_by_name && f->m.total <= 100000) {
        f->arranged_by_name = true;
        layout_reset(f->lay, cols_now(f));
        size_t *o = name_order(f);
        layout_update(f->lay, f->m.total, lay_name, f, true, o);
        free(o);
        svc_layout(f->path, got_layout, as_arg(f));   /* stored positions win over it */
        layout_on_new_spot(f->lay, new_spot, f);   /* from now on, new ones' spots are kept */
    } else {
        layout_update(f->lay, f->m.total, lay_name, f, full, NULL);
    }
    if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
}

/* New objects' spots, stored a moment after they came (see layout_follow). */
static int spot_timer = -1;

static void store_spots(struct folder *f)
{
    /* every icon not placed yet, with its spot now (the new one with the
     * others: stored and unstored mixed overlapped at the next login, the
     * unstored ones arranged by name over the stored) */
    for (size_t j = 0; j < f->nspot_new; j++) free(f->spot_new[j]);
    free(f->spot_new);
    f->spot_new = NULL;
    f->nspot_new = 0;
    size_t n = f->m.total;
    if (!f->m.complete || !n || n > 2000) return;
    char **t = calloc(n, sizeof *t);
    int64_t *x = calloc(n, sizeof *x), *y = calloc(n, sizeof *y);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *nm = model_name(&f->m, i);
        if (!nm || layout_is_placed(f->lay, nm)) continue;
        double px, py;
        layout_pos(f->lay, i, &px, &py);
        if (nm[0] == '\x01') t[k] = strdup(nm + 1);   /* a central object: its address */
        else if (asprintf(&t[k], "%s/%s", strcmp(f->path, "/") ? f->path : "", nm) < 0) continue;
        x[k] = (int64_t)px;
        y[k] = (int64_t)py;
        layout_place(f->lay, nm, (int)px, (int)py);
        k++;
    }
    if (k) svc_place(f->path, (const char *const *)t, x, y, k, placed_reply, as_arg(f));
    for (size_t i = 0; i < k; i++) free(t[i]);
    free(t);
    free(x);
    free(y);
}

static void on_spot_timer(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t n;
    if (read(fd, &n, sizeof n) < 0) { /* nothing */ }
    for (struct folder *f = folders; f; f = f->next)
        if (!f->closing && f->nspot_new) store_spots(f);
}

/* An object that comes after the first arrangement (made, copied, moved
 * in) takes the next free spot; that spot is stored a moment later (a
 * drop's own position, stored as its job ends, comes first), so it is
 * there at the next login - unstored, the folder arranged by name then
 * put it elsewhere, and the order changed. */
static void new_spot(void *ctx, const char *name)
{
    struct folder *f = ctx;
    if (!f->arranged_by_name || f->nspot_new >= 64) return;
    if (f->filter[0]) return;   /* (flowed while filtered: not spots to keep) */
    f->spot_new = realloc(f->spot_new, (f->nspot_new + 1) * sizeof *f->spot_new);
    f->spot_new[f->nspot_new++] = strdup(name);
    spots_later();
}

static void spots_later(void)
{
    if (spot_timer < 0) {
        spot_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (spot_timer < 0) return;
        ui_app_watch_fd(app, spot_timer, POLLIN, on_spot_timer, NULL);
    }
    struct itimerspec its = { .it_value = { 2, 0 } };   /* (each new one starts the 2 s again) */
    timerfd_settime(spot_timer, 0, &its, NULL);
}

static void got_layout(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    if (!f || !items || items->k != CV_ARRAY) return;
    if (f->filter[0]) return;   /* filtered: the matches flow, stored positions unused */
    /* where the unplaced locked ones are now (locked in the notebook) */
    size_t nl = 0;
    char **lname = NULL;
    for (size_t i = 0; i < items->n; i++) {
        const cv *lk = cv_get(&items->items[i], "locked");
        const char *nm = cv_text(&items->items[i], "name");
        if (nm && lk && lk->k == CV_BOOL && lk->b && !cv_get(&items->items[i], "x")) {
            lname = realloc(lname, (nl + 1) * sizeof *lname);
            lname[nl++] = strdup(nm);
        }
    }
    double *lx = calloc(nl ? nl : 1, sizeof *lx), *ly = calloc(nl ? nl : 1, sizeof *ly);
    for (size_t k = 0; k < nl; k++)
        for (size_t i = 0; i < f->m.total; i++) {
            const char *nm = model_name(&f->m, i);
            if (nm && !strcmp(nm, lname[k])) { layout_pos(f->lay, i, &lx[k], &ly[k]); break; }
        }
    layout_clear_placed(f->lay);
    for (size_t i = 0; i < items->n; i++) {
        const char *nm = cv_text(&items->items[i], "name");
        const cv *lk = cv_get(&items->items[i], "locked");
        int64_t x = 0, y = 0;
        bool has = cv_int(&items->items[i], "x", &x);
        cv_int(&items->items[i], "y", &y);
        if (nm && has) layout_place(f->lay, nm, (int)x, (int)y);
        if (nm && lk && lk->k == CV_BOOL && lk->b) layout_lock(f->lay, nm, true);
    }
    for (size_t k = 0; k < nl; k++) {   /* their places now, stored (still locked) */
        char addr[PATH_MAX];
        if (lname[k][0] == '\x01') snprintf(addr, sizeof addr, "%s", lname[k] + 1);
        else snprintf(addr, sizeof addr, "%s/%s", strcmp(f->path, "/") ? f->path : "", lname[k]);
        layout_place(f->lay, lname[k], (int)lx[k], (int)ly[k]);
        svc_place_lock(f->path, addr, (int64_t)lx[k], (int64_t)ly[k], true, ignore_reply, NULL);
        free(lname[k]);
    }
    free(lname);
    free(lx);
    free(ly);
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
    if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
}

static void placed_reply(void *arg, const cv *reply)
{
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b)) {
        char m[300];
        snprintf(m, sizeof m, "Positions not kept: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        struct folder *f = from_arg(arg);
        if (f) { snprintf(f->error, sizeof f->error, "%s", m); damage_status(f); }
        return;
    }
    /* Stored: read the positions again, so an earlier answer (asked when the
     * new item appeared, before this one was kept) does not have the last word. */
    struct folder *f = from_arg(arg);
    if (f) svc_layout(f->path, got_layout, as_arg(f));
}

/* Arrange (folder menu): a grid that fits the window, in name order;
 * stored for folders up to 2000 objects (larger ones are arranged for
 * this session). */
static void arrange_by(struct folder *f, int key);
static void arrange(struct folder *f) { arrange_by(f, 0); }

/* Sort (icon view, as OS/2): the icons arranged in an order, once. Size
 * and date need every row's details: they are read first. */
static void sort_icons(struct folder *f, int key)
{
    if (key >= 2) {
        bool all = true;
        for (size_t p = 0; p * PAGE < f->m.total; p++) {
            struct page *pg = page_at(&f->m, p);
            if (pg->state == P_DETAILED) continue;
            all = false;
            if (pg->requested) continue;
            pg->requested = true;
            svc_list_v(f->path, f->filter, show_hidden, p * PAGE, PAGE, true, got_page, page_arg(f, false));
        }
        if (!all || !f->m.complete) {
            f->sort_pending = key + 1;
            snprintf(f->error, sizeof f->error, "Sorting...");
            damage_status(f);
            return;
        }
    }
    f->sort_pending = 0;
    arrange_by(f, key);
}

static void arrange_by(struct folder *f, int key)
{
    if (f->lock_all) {   /* (nothing would move) */
        snprintf(f->error, sizeof f->error, "Positions are locked (the folder's Icon view page)");
        damage_status(f);
        return;
    }
    svc_arrange(f->path, placed_reply, as_arg(f));
    layout_reset(f->lay, cols_now(f));
    size_t *o = name_order(f);
    if (key) {
        sort_key = key;
        sort_folder = f;
        qsort(o, f->m.total, sizeof *o, cmp_index_key);
    }
    layout_update(f->lay, f->m.total, lay_name, f, true, o);
    free(o);
    f->arranged_by_name = true;
    size_t n = f->m.total;
    if (n && n <= 2000) {
        char **t = calloc(n, sizeof *t);
        int64_t *x = calloc(n, sizeof *x), *y = calloc(n, sizeof *y);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            const char *nm = model_name(&f->m, i);
            if (!nm) continue;
            double px, py;
            layout_pos(f->lay, i, &px, &py);
            t[k] = malloc(strlen(f->path) + strlen(nm) + 2);
            sprintf(t[k], "%s/%s", strcmp(f->path, "/") ? f->path : "", nm);
            x[k] = (int64_t)px;
            y[k] = (int64_t)py;
            layout_place(f->lay, nm, (int)px, (int)py);
            k++;
        }
        svc_place(f->path, (const char *const *)t, x, y, k, placed_reply, as_arg(f));
        for (size_t i = 0; i < k; i++) free(t[i]);
        free(t);
        free(x);
        free(y);
    }
    ui_container_layout_changed(f->cont);
    static const char *const by[] = { "Arranged", "Sorted by type", "Sorted by size", "Sorted by last write" };
    snprintf(f->error, sizeof f->error, "%s", by[key >= 0 && key < 4 ? key : 0]);
    damage_status(f);
}

/* ------------------------------------------------------------ properties */

/* An object's settings notebook. Replies find it by ID: it may have
 * closed. The title field renames (the service renames files, titles the
 * rest), so the target path can change while it is open. */
struct props {
    struct props *next;
    long id;
    char target[PATH_MAX];
    char title[256];
    int64_t changes;
    ui_notebook *nb;
    size_t want_page;        /* reopened at login: the page it showed */
};

static struct props *props_open;
static long next_props_id = 1;

static struct props *props_by_id(long id)
{
    for (struct props *p = props_open; p; p = p->next)
        if (p->id == id) return p;
    return NULL;
}

/* The service's pages, as notebook pages (strings point into `res`). */
static ui_page_desc *pages_from(const cv *res, size_t *n)
{
    *n = res && res->k == CV_ARRAY ? res->n : 0;
    ui_page_desc *pages = calloc(*n ? *n : 1, sizeof *pages);
    for (size_t i = 0; i < *n; i++) {
        const cv *pg = &res->items[i], *fs = cv_get(pg, "fields");
        pages[i].title = cv_text(pg, "title");
        size_t nf = fs && fs->k == CV_ARRAY ? fs->n : 0;
        ui_field_desc *fields = calloc(nf ? nf : 1, sizeof *fields);
        size_t kept = 0;
        for (size_t k = 0; k < nf; k++) {
            const cv *fd = &fs->items[k], *v = cv_get(fd, "value"), *ch = cv_get(fd, "choices");
            const char *type = cv_text(fd, "type");
            if (type && !strcmp(type, "hidden")) continue;   /* (kept by the GUI itself: column widths) */
            ui_field_desc *d = &fields[kept++];
            d->key = cv_text(fd, "key");
            d->label = cv_text(fd, "label");
            d->kind = !type || !strcmp(type, "text") ? UI_FIELD_TEXT : !strcmp(type, "checkbox") ? UI_FIELD_CHECKBOX
                      : !strcmp(type, "number") ? UI_FIELD_NUMBER : !strcmp(type, "icon") ? UI_FIELD_ICON
                      : !strcmp(type, "info") ? UI_FIELD_INFO : !strcmp(type, "file") ? UI_FIELD_FILE
                      : !strcmp(type, "color") ? UI_FIELD_COLOR : UI_FIELD_CHOICE;
            if (v && v->k == CV_TEXT) d->value.text = (char *)v->s;
            else if (v && v->k == CV_INT) d->value.number = v->i;
            else if (v && v->k == CV_BOOL) d->value.checked = v->b;
            if (ch && ch->k == CV_ARRAY) {
                const char **c = calloc(ch->n ? ch->n : 1, sizeof *c);
                for (size_t j = 0; j < ch->n; j++) c[j] = ch->items[j].k == CV_TEXT ? ch->items[j].s : "";
                d->choices = c;
                d->nchoices = ch->n;
            }
        }
        pages[i].fields = fields;
        pages[i].nfields = kept;
    }
    return pages;
}

static void pages_free(ui_page_desc *pages, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < pages[i].nfields; k++) free((void *)pages[i].fields[k].choices);
        free((void *)pages[i].fields);
    }
    free(pages);
}

static void note_object(struct props *p, const cv *reply)
{
    const cv *o = reply ? cv_get(reply, "object") : NULL;
    int64_t ch;
    if (o && cv_int(o, "changes", &ch)) p->changes = ch;
    const char *path = o ? cv_text(o, "path") : NULL;
    if (path && *path) snprintf(p->target, sizeof p->target, "%s", path);   /* renamed */
}

static void props_set(void *ctx, const char *key, ui_field_kind kind, const ui_value *v);

static void props_refreshed(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!p || !res || !p->nb) return;
    note_object(p, reply);
    size_t n;
    ui_page_desc *pages = pages_from(res, &n);
    ui_notebook_update(p->nb, pages, n);
    pages_free(pages, n);
}

static void props_refresh(struct props *p) { svc_settings(p->target, props_refreshed, (void *)(intptr_t)p->id); }

static void props_applied(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    if (!p || !p->nb) return;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        note_object(p, reply);
        const cv *res = cv_get(reply, "result");
        if (res && res->k == CV_BOOL && !res->b)   /* a rename without permission */
            ui_notebook_set_status(p->nb, "No permission to rename the file: the new name is kept as its title");
        else ui_notebook_set_status(p->nb, "");
        props_refresh(p);
        return;
    }
    const char *code = reply ? cv_text(reply, "error") : NULL, *msg = reply ? cv_text(reply, "message") : NULL;
    char m[300];
    if (code && !strcmp(code, "conflict")) snprintf(m, sizeof m, "Changed elsewhere meanwhile: these are the current values");
    else snprintf(m, sizeof m, "Not changed: %s", msg ? msg : reply ? "refused" : "lost the connection to wpsd");
    ui_notebook_set_status(p->nb, m);
    props_refresh(p);
}

/* An icon made from an image (dropped, or chosen in the picker): the
 * object's icon now. */
struct icon_req { long props; char key[64]; };

static void icon_made(void *ctx, const char *png)
{
    struct icon_req *r = ctx;
    struct props *p = props_by_id(r->props);
    if (p && p->nb) {
        if (!png) {
            char m[300];
            snprintf(m, sizeof m, "No icon made of it: %s. The icon is as it was.", icons_import_error());
            ui_notebook_set_status(p->nb, m);
        }
        else {
            cv val = { .k = CV_TEXT, .s = (char *)png, .len = strlen(png) };
            svc_settings_set(p->target, r->key, &val, p->changes, props_applied, (void *)(intptr_t)p->id);
            events_raise("object.icon", p->target, "properties", (ui_rect){ 0, 0, 0, 0 });
        }
    }
    free(r);
}

void props_icon_from(long props, const char *key, const char *image)
{
    struct props *p = props_by_id(props);
    if (!p) return;
    if (p->nb) ui_notebook_set_status(p->nb, "Making the icon...");
    struct icon_req *r = calloc(1, sizeof *r);
    r->props = props;
    snprintf(r->key, sizeof r->key, "%s", key);
    icons_import(image, icon_made, r);
}

/* Browse...'s choice: into the field, which applies it. */
static void props_file_chosen(long owner, const char *key, const char *path)
{
    struct props *p = props_by_id(owner);
    if (p && p->nb) ui_notebook_set_value(p->nb, key, path);
}

static void props_set(void *ctx, const char *key, ui_field_kind kind, const ui_value *v)
{
    struct props *p = ctx;
    void *arg = (void *)(intptr_t)p->id;
    if (kind == UI_FIELD_ICON) {
        if (!v->text) { iconpick_open(p->title, props_icon_from, p->id, key); return; }   /* Choose... */
        if (*v->text) { props_icon_from(p->id, key, v->text); return; }                     /* an image dropped */
        /* Default: "" below */
    }
    if (kind == UI_FIELD_FILE && !v->text) {   /* Browse... */
        const char *home = getenv("HOME");
        filepick_open(p->title, ui_notebook_field_hint(p->nb, key), home && *home ? home : "/", props_file_chosen, p->id, key);
        return;
    }
    if (!strcmp(key, "title") && kind == UI_FIELD_TEXT) {
        /* Decided: a file's title is its name; the service renames it (or,
         * without permission, keeps it as a title). */
        svc_rename(p->target, v->text ? v->text : "", props_applied, arg);
        return;
    }
    cv val = { 0 };
    if (kind == UI_FIELD_TEXT || kind == UI_FIELD_ICON || kind == UI_FIELD_FILE || kind == UI_FIELD_COLOR) { val.k = CV_TEXT; val.s = v->text ? v->text : ""; val.len = strlen(val.s); }
    else if (kind == UI_FIELD_CHECKBOX) { val.k = CV_BOOL; val.b = v->checked; }
    else { val.k = CV_INT; val.i = v->number; }
    svc_settings_set(p->target, key, &val, p->changes, props_applied, arg);
}

static void props_closed(void *ctx)
{
    struct props *p = ctx;
    for (struct props **pp = &props_open; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    free(p);
}

static void got_settings(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    if (!p) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res) {
        fprintf(stderr, "wps-shell: no settings for %s: %s\n", p->target, reply ? cv_text(reply, "message") : "no service");
        props_closed(p);
        return;
    }
    note_object(p, reply);
    const cv *o = cv_get(reply, "object");
    const char *t = o ? cv_text(o, "title") : NULL;
    snprintf(p->title, sizeof p->title, "%s - Properties", t && *t ? t : p->target);
    size_t n;
    ui_page_desc *pages = pages_from(res, &n);
    p->nb = ui_notebook_open(app, p->title, pages, n, props_set, props_closed, p);
    if (p->want_page && p->want_page < n) ui_notebook_show_page(p->nb, p->want_page);
    pages_free(pages, n);
}

/* Open an object's notebook, or leave the open one (Wayland lets no program
 * raise its own window on its own). */
/* Its settings notebook; one already open comes to the front (false). */
static bool open_properties(const char *target)
{
    for (struct props *p = props_open; p; p = p->next)
        if (!strcmp(p->target, target)) {
            if (p->nb) ui_window_activate(ui_notebook_surface(p->nb));
            return false;
        }
    struct props *p = calloc(1, sizeof *p);
    p->id = next_props_id++;
    snprintf(p->target, sizeof p->target, "%s", target);
    p->next = props_open;
    props_open = p;
    svc_settings(target, got_settings, (void *)(intptr_t)p->id);
    return true;
}

/* Menus: the service gives the items; the GUI carries out the well-known
 * ones and sends the rest back. */
struct menu_ctx { long folder; size_t item; double x, y; uint32_t serial; };

static char setup_folder[PATH_MAX];   /* System Setup's (the LaunchPad's first buttons) */


static void convert(const cv *arr, ui_menu_item **out, size_t *n)
{
    *n = arr && arr->k == CV_ARRAY ? arr->n : 0;
    *out = calloc(*n ? *n : 1, sizeof **out);
    for (size_t i = 0; i < *n; i++) {
        const cv *it = &arr->items[i];
        int64_t id = 0;
        cv_int(it, "id", &id);
        const cv *en = cv_get(it, "enabled"), *ck = cv_get(it, "checked");
        (*out)[i].id = id;
        (*out)[i].label = (char *)(cv_text(it, "label") ? cv_text(it, "label") : "");
        (*out)[i].enabled = !en || (en->k == CV_BOOL && en->b);
        (*out)[i].checked = ck && ck->k == CV_BOOL && ck->b;
        convert(cv_get(it, "items"), &(*out)[i].items, &(*out)[i].nitems);
    }
}

static void free_converted(ui_menu_item *m, size_t n)
{
    for (size_t i = 0; i < n; i++) free_converted(m[i].items, m[i].nitems);
    free(m);
}

static void selected_done(void *arg, const cv *reply) { opened(arg, reply); }
static void delete_request(struct folder *f, size_t i, bool permanent);
static void create_another(struct folder *f, size_t i);

static void menu_chosen(void *arg, int64_t id)
{
    struct menu_ctx *m = arg;
    struct folder *f = folder_by_id(m->folder);
    if (f && id >= 0) {
        char t[PATH_MAX];
        target_of(f, m->item, t, sizeof t);
        switch (id) {
        case WPS_MI_OPEN: open_item(f, m->item, UI_VIEW_ICON); break;
        case WPS_MI_OPEN_ICON: open_item(f, m->item, UI_VIEW_ICON); break;
        case WPS_MI_OPEN_DETAILS: open_item(f, m->item, UI_VIEW_DETAILS); break;
        case WPS_MI_PROPERTIES: {
            /* a shadow's are its original's (OS/2): renaming there renames it */
            struct row *rw = is_tree(f) ? NULL : row_at(f, m->item);
            if (rw && rw->shadow && rw->target && *rw->target) snprintf(t, sizeof t, "%s", rw->target);
            open_properties(t);   /* (or the open one to the front) */
            break;
        }
        case WPS_MI_OPEN_WITH:
            snprintf(f->error, sizeof f->error, "Open With comes in G2");
            damage_status(f);
            break;
        case WPS_MI_VIEW_ICON: set_view(f, UI_VIEW_ICON); break;
        case WPS_MI_VIEW_DETAILS: set_view(f, UI_VIEW_DETAILS); break;
        case WPS_MI_VIEW_TREE: set_view(f, UI_VIEW_TREE); break;
        case WPS_MI_OPEN_TREE: open_item(f, m->item, UI_VIEW_TREE); break;
        case WPS_MI_ARRANGE: arrange(f); break;
        case WPS_MI_SORT_NAME: case WPS_MI_SORT_TYPE: case WPS_MI_SORT_SIZE: case WPS_MI_SORT_DATE:
            sort_icons(f, (int)(id - WPS_MI_SORT_NAME));
            break;
        case WPS_MI_FIND: findwin_open(f->path, NULL); break;
        case WPS_MI_CLOSE: win_close(f->win); break;
        case WPS_MI_OPEN_PARENT: {
            char parent[PATH_MAX];
            snprintf(parent, sizeof parent, "%s", f->path);
            char *sl = strrchr(parent, '/');
            if (sl == parent) sl[1] = 0;
            else if (sl) *sl = 0;
            if (strcmp(parent, f->path)) open_folder(parent, UI_VIEW_ICON);
            break;
        }
        case WPS_MI_DELETE: delete_request(f, m->item, false); break;
        case WPS_MI_EMPTY_TRASH: empty_trash_ask(f); break;
        case WPS_MI_MOUNT: case WPS_MI_UNMOUNT: case WPS_MI_EJECT: case WPS_MI_POWER_OFF: case WPS_MI_LOCK: case WPS_MI_UNLOCK: {
            char t[PATH_MAX];
            target_of(f, m->item, t, sizeof t);
            if (id == WPS_MI_UNLOCK) unlock_ask(f, t, false);
            else drive_action(f, t, id == WPS_MI_MOUNT ? "mount" : id == WPS_MI_UNMOUNT ? "unmount" : id == WPS_MI_EJECT ? "eject"
                                    : id == WPS_MI_POWER_OFF ? "poweroff" : "lock", false);
            break;
        }
        case WPS_MI_RESTORE: if (m->item != SIZE_MAX) restore_items(f, m->item); break;
        case WPS_MI_LOCK_POS: if (m->item != SIZE_MAX) lock_items(f, m->item); break;
        case WPS_MI_DELETE_FOREVER: delete_request(f, m->item, true); break;
        case WPS_MI_CREATE_ANOTHER: create_another(f, m->item); break;
        case WPS_MI_WINDOW_LIST: winlist_open(false); break;
        case WPS_MI_LOCKUP: session_lock("desktop"); break;
        case WPS_MI_SYSTEM_SETUP: if (setup_folder[0]) open_folder(setup_folder, UI_VIEW_ICON); break;
        case WPS_MI_SHUTDOWN: session_ask_end(app, "desktop"); break;
        case WPS_MI_MINIMIZED: winlist_open(true); break;
        default:
            svc_menu_select(t, id, selected_done, as_arg(f));
            break;
        }
    }
    free(m);
}

/* ---- Drives (v0.2-design 5): their menu's actions, the passphrase */

struct drive_ctx { long folder; char target[PATH_MAX], action[16]; bool reopen; ui_dialog *dlg; };

static void drive_acted(void *arg, const cv *reply)
{
    struct drive_ctx *x = arg;
    struct folder *f = folder_by_id(x->folder);
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        const char *mp = cv_text(reply, "result");
        char m[PATH_MAX + 64];
        if (!strcmp(x->action, "mount")) snprintf(m, sizeof m, "Mounted at %s", mp && *mp ? mp : "?");
        else if (!strcmp(x->action, "unmount")) snprintf(m, sizeof m, "Unmounted");
        else if (!strcmp(x->action, "eject") || !strcmp(x->action, "poweroff")) snprintf(m, sizeof m, "It can be removed now");
        else if (!strcmp(x->action, "unlock")) snprintf(m, sizeof m, "Unlocked");
        else snprintf(m, sizeof m, "Locked");
        if (f) set_status(f->id, m);
        if (x->reopen && f) {   /* unlocked to open it: open it now */
            snprintf(f->pending_open, sizeof f->pending_open, "%s", x->target);
            svc_open(x->target, false, opened, as_arg(f));
        }
    } else {
        const char *e = reply ? cv_text(reply, "error") : NULL, *msg = reply ? cv_text(reply, "message") : NULL;
        char m[600];
        if (e && !strcmp(e, "busy"))
            snprintf(m, sizeof m, "The drive is in use. Close the windows and programs using it, then try again.");
        else if (e && !strcmp(e, "not authorized"))
            snprintf(m, sizeof m, "This needs an administrator's permission, which WPS cannot ask for yet. "
                                  "Removable drives work; for this one, use GNOME's Disks or Files.");
        else snprintf(m, sizeof m, "%s", msg && *msg ? msg : reply ? "it failed" : "lost the connection to wpsd");
        if (!strcmp(x->action, "unlock") && e && !strcmp(e, "failed") && f) {
            unlock_ask(f, x->target, x->reopen);   /* (a wrong passphrase: again) */
            set_status(f->id, "Wrong passphrase?");
        } else {
            const char *b[] = { "OK" };
            ask("Drive", m, b, 1, 0, NULL, NULL);
        }
    }
    free(x);
}

static void drive_action(struct folder *f, const char *target, const char *action, bool reopen)
{
    struct drive_ctx *x = calloc(1, sizeof *x);
    x->folder = f->id;
    x->reopen = reopen;
    snprintf(x->target, sizeof x->target, "%s", target);
    snprintf(x->action, sizeof x->action, "%s", action);
    svc_drive(target, action, NULL, drive_acted, x);
}

static void unlock_chosen(void *arg, int choice)
{
    struct drive_ctx *x = arg;
    if (choice != 0) { free(x); return; }
    snprintf(x->action, sizeof x->action, "unlock");
    svc_drive(x->target, "unlock", ui_dialog_text(x->dlg), drive_acted, x);   /* (wiped when the dialog goes) */
}

static void unlock_ask(struct folder *f, const char *target, bool reopen)
{
    struct drive_ctx *x = calloc(1, sizeof *x);
    x->folder = f->id;
    x->reopen = reopen;
    snprintf(x->target, sizeof x->target, "%s", target);
    const char *b[] = { "Unlock", "Cancel" };
    events_raise("alert.warning", "", "", (ui_rect){ 0, 0, 0, 0 });
    x->dlg = ui_dialog_open_entry(app, "Unlock", "This volume is encrypted. Its passphrase:", true, b, 2, 0, unlock_chosen, x);
    last_dialog = x->dlg;
}

/* Drives came, went, were mounted or unmounted: drive objects and
 * shadows are read again (their icons and titles). */
static void drives_changed_event(void)
{
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing || is_tree(f) || !f->m.complete) continue;
        bool any = false;
        for (size_t i = 0; i < f->m.total; i++) {
            struct row *rw = row_at(f, i);
            if (!rw || !rw->name || !(rw->shadow || rw->name[0] == '\x01')) continue;
            row_stale(&f->m, i);
            any = true;
        }
        if (any) fetch_visible(f, f->vis_first, f->vis_last);
    }
}

/* A trash's files folder (the Trash Can): .../files beside .../info. */
static bool is_trash_dir(const char *p)
{
    size_t l = p ? strlen(p) : 0;
    if (l < 7 || strcmp(p + l - 6, "/files")) return false;
    char info[PATH_MAX];
    snprintf(info, sizeof info, "%.*s/info", (int)(l - 6), p);
    struct stat st;
    return stat(info, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Startup (v0.2-design 7.1): the XDG autostart folder. */
static void startup_dir(char *out, size_t n)
{
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xc && *xc) snprintf(out, n, "%s/autostart", xc);
    else snprintf(out, n, "%s/.config/autostart", home ? home : "");
}

static bool is_startup_dir(const char *p)
{
    char d[PATH_MAX], a[PATH_MAX], b[PATH_MAX];
    startup_dir(d, sizeof d);
    return p && realpath(d, a) && realpath(p, b) && !strcmp(a, b);
}

/* At login (once a session: XDG_SESSION_ID), the desktop opens the
 * shadows in Startup, as OS/2's Startup folder did; its program entries
 * the session itself runs (systemd's XDG autostart). */
static void startup_opened(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res) return;
    if (cv_text(res, "folder")) open_folder(cv_text(res, "folder"), UI_VIEW_ICON);
    else svc_reply_view(reply);
}

static void got_startup(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    for (size_t i = 0; items && items->k == CV_ARRAY && i < items->n; i++) {
        const cv *it = &items->items[i], *sh = cv_get(it, "shadow");
        const char *nm = cv_text(it, "name");
        if (!nm || nm[0] != '\x01' || !sh || sh->k != CV_BOOL || !sh->b) continue;
        svc_open(nm + 1, false, startup_opened, NULL);
    }
}

static void startup_at_login(void)
{
    const char *rt = getenv("WPS_RUNTIME_DIR");
    if (!rt || !*rt) rt = getenv("XDG_RUNTIME_DIR");
    const char *sid = getenv("XDG_SESSION_ID");
    char marker[PATH_MAX], was[64] = "";
    snprintf(marker, sizeof marker, "%s/wps-startup-opened", rt && *rt ? rt : "/tmp");
    FILE *m = fopen(marker, "r");
    if (m) { if (!fgets(was, sizeof was, m)) was[0] = 0; fclose(m); }
    was[strcspn(was, "\n")] = 0;
    if (m && !strcmp(was, sid ? sid : "")) return;   /* (the desktop started again in this session) */
    m = fopen(marker, "w");
    if (m) { fprintf(m, "%s\n", sid ? sid : ""); fclose(m); }
    char d[PATH_MAX];
    startup_dir(d, sizeof d);
    svc_list(d, 0, 500, true, got_startup, NULL);
}

static void emptied(void *arg, const cv *reply)
{
    long folder = (long)(intptr_t)arg;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b)) {
        char m[300];
        snprintf(m, sizeof m, "Cannot empty the trash: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(folder, m);
    } else set_status(folder, "Emptying the trash...");
}

static void empty_confirmed(void *arg, int choice)
{
    last_dialog = NULL;
    if (choice == 0) svc_empty_trash(emptied, arg);
}

static void empty_trash_ask(struct folder *f)
{
    const char *buttons[] = { "Empty", "Cancel" };
    last_dialog = ask("Empty trash", "Delete everything in the trash for good?\nThis cannot be undone.", buttons, 2, 1,
                      empty_confirmed, (void *)(intptr_t)f->id);
}

static void restored(void *arg, const cv *reply)
{
    long folder = (long)(intptr_t)arg;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    char m[PATH_MAX + 64];
    if (ok && ok->k == CV_BOOL && ok->b) snprintf(m, sizeof m, "Restored to %s", cv_text(reply, "result") ? cv_text(reply, "result") : "its folder");
    else snprintf(m, sizeof m, "Cannot restore: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
    set_status(folder, m);
}

/* Restore item i (the selection if i is in it): each back where it was. */
static void restore_items(struct folder *f, size_t i)
{
    bool whole = ui_container_is_selected(f->cont, i);
    for (size_t k = whole ? 0 : i; whole ? k < f->m.total : k == i; k++) {
        if (whole && !ui_container_is_selected(f->cont, k)) continue;
        const char *nm = model_name(&f->m, k);
        if (!nm) continue;
        char p[PATH_MAX];
        if (nm[0] == '\x01') snprintf(p, sizeof p, "%s", nm + 1);   /* a central object: its address */
        else snprintf(p, sizeof p, "%s/%s", f->path, nm);
        svc_restore(p, restored, (void *)(intptr_t)f->id);
    }
}

/* Lock position: the item (and the others selected with it) locked where
 * it is, or unlocked. */
static void lock_items(struct folder *f, size_t i)
{
    const char *n0 = model_name(&f->m, i);
    if (!n0 || is_tree(f)) return;
    bool on = !layout_is_locked(f->lay, n0);
    bool whole = ui_container_is_selected(f->cont, i);
    for (size_t k = whole ? 0 : i; whole ? k < f->m.total : k == i; k++) {
        if (whole && !ui_container_is_selected(f->cont, k)) continue;
        const char *nm = model_name(&f->m, k);
        if (!nm) continue;
        char t[PATH_MAX];
        target_of(f, k, t, sizeof t);
        double x, y;
        layout_pos(f->lay, k, &x, &y);
        layout_place(f->lay, nm, (int)x, (int)y);
        layout_lock(f->lay, nm, on);
        svc_place_lock(f->path, t, (int64_t)x, (int64_t)y, on, placed_reply, as_arg(f));
    }
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
}

static void got_menu(void *arg, const cv *reply)
{
    struct menu_ctx *m = arg;
    struct folder *f = folder_by_id(m->folder);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!f || !res) { free(m); return; }
    ui_menu_item *items;
    size_t n;
    convert(res, &items, &n);
    if (m->item != SIZE_MAX && is_trash_dir(f->path) && !is_tree(f)) {
        /* in the Trash Can: put back, or delete for good (not into the
         * trash again, nor Create another) */
        size_t w = 0;
        for (size_t k = 0; k < n; k++) {
            if (items[k].id == WPS_MI_DELETE || items[k].id == WPS_MI_CREATE_ANOTHER) continue;
            items[w++] = items[k];
        }
        n = w;
        items = realloc(items, (n + 3) * sizeof *items);
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_RESTORE, "Restore", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_DELETE_FOREVER, "Delete permanently...", true, false, NULL, 0 };
    }
    if (m->item != SIZE_MAX && !is_tree(f)) {
        /* a shadow of the Trash Can (the desktop's): Empty trash too */
        struct row *rw = row_at(f, m->item);
        if (rw && rw->shadow && rw->target && is_trash_dir(rw->target)) {
            items = realloc(items, (n + 1) * sizeof *items);
            items[n++] = (ui_menu_item){ WPS_MI_EMPTY_TRASH, "Empty trash", true, false, NULL, 0 };
        }
    }
    if (m->item != SIZE_MAX && !is_tree(f) && ui_container_view(f->cont) == UI_VIEW_ICON && !f->filter[0]) {
        /* Lock position (v0.3-design 5): its spot in this folder */
        const char *nm = model_name(&f->m, m->item);
        items = realloc(items, (n + 2) * sizeof *items);
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_LOCK_POS, "Lock position", nm && !f->lock_all, layout_is_locked(f->lay, nm), NULL, 0 };
    }
    if (m->item == SIZE_MAX) {
        /* Every folder's (the desktop's too), as OS/2's: Open parent after
         * Open, then Find, Sort; Close for a window (the desktop stays). */
        bool icons = ui_container_view(f->cont) == UI_VIEW_ICON;
        ui_menu_item *sorts = calloc(4, sizeof *sorts);
        sorts[0] = (ui_menu_item){ WPS_MI_SORT_NAME, "By name", icons, false, NULL, 0 };
        sorts[1] = (ui_menu_item){ WPS_MI_SORT_TYPE, "By type", icons, false, NULL, 0 };
        sorts[2] = (ui_menu_item){ WPS_MI_SORT_SIZE, "By size", icons, false, NULL, 0 };
        sorts[3] = (ui_menu_item){ WPS_MI_SORT_DATE, "By last write", icons, false, NULL, 0 };
        items = realloc(items, (n + 6) * sizeof *items);
        size_t at = n ? 1 : 0;
        memmove(items + at + 1, items + at, (n - at) * sizeof *items);
        items[at] = (ui_menu_item){ WPS_MI_OPEN_PARENT, "Open parent", strcmp(f->path, "/") != 0, false, NULL, 0 };
        n++;
        items[n++] = (ui_menu_item){ WPS_MI_FIND, "Find...", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_SORT, "Sort", icons, false, sorts, 4 };
        if (!f->desktop) {
            items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
            items[n++] = (ui_menu_item){ WPS_MI_CLOSE, "Close", true, false, NULL, 0 };
        }
    }
    if (m->item == SIZE_MAX && f->desktop) {
        /* The desktop's own menu, after the folder's items, as OS/2's:
         * System setup (the folder: it will hold many objects), Lockup now,
         * Shut down (v0.3-design 2); then the window list and the minimized
         * windows (G4). */
        items = realloc(items, (n + 7) * sizeof *items);
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_SYSTEM_SETUP, "System setup", setup_folder[0] != 0, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_LOCKUP, "Lockup now", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_SHUTDOWN, "Shut down...", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_WINDOW_LIST, "Window list", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ WPS_MI_MINIMIZED, "Minimized windows", true, false, NULL, 0 };
    }
    if (m->item == SIZE_MAX && !f->desktop) {
        /* The folder window's own menu: a View cascade after Open, to switch
         * this window's view (the Open cascade opens new windows). */
        ui_view v = ui_container_view(f->cont);
        ui_menu_item *views = calloc(3, sizeof *views);
        views[0] = (ui_menu_item){ WPS_MI_VIEW_ICON, "Icon view\tCtrl+1", true, v == UI_VIEW_ICON, NULL, 0 };
        views[1] = (ui_menu_item){ WPS_MI_VIEW_TREE, "Tree view\tCtrl+2", true, v == UI_VIEW_TREE, NULL, 0 };
        views[2] = (ui_menu_item){ WPS_MI_VIEW_DETAILS, "Details view\tCtrl+3", true, v == UI_VIEW_DETAILS, NULL, 0 };
        items = realloc(items, (n + 1) * sizeof *items);
        size_t at = n ? 1 : 0;
        memmove(items + at + 1, items + at, (n - at) * sizeof *items);
        items[at] = (ui_menu_item){ WPS_MI_VIEW, "View", true, false, views, 3 };
        n++;
    }
    /* No grab (serial 0): the button was released before the service
     * answered, and GNOME refuses a grab then, dismissing the menu at once.
     * The shell closes its own menus on clicks elsewhere. */
    ui_menu_popup(f->win, items, n, m->x, m->y, 0, menu_chosen, m);
    free_converted(items, n);
}

static void src_menu(void *ctx, size_t i, double x, double y, uint32_t serial)
{
    struct folder *f = ctx;
    struct menu_ctx *m = calloc(1, sizeof *m);
    m->folder = f->id;
    m->item = i;
    m->x = x;
    m->y = y;
    m->serial = serial;
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    svc_menu(t, got_menu, m);
}

static void renamed(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        const cv *res = cv_get(reply, "result");
        if (res && res->k == CV_BOOL && !res->b)
            snprintf(f->error, sizeof f->error, "No permission to rename the file: the new name is kept as its title");
        else f->error[0] = 0;
    } else {
        const char *msg = reply ? cv_text(reply, "message") : "lost the connection to wpsd";
        snprintf(f->error, sizeof f->error, "Cannot rename: %s", msg ? msg : "refused");
    }
    damage_status(f);
    reload(f);   /* events follow too; this covers a title-only change */
}

static void src_rename(void *ctx, size_t i, const char *name)
{
    struct folder *f = ctx;
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    const char *old = row_name(f, i);
    /* A shadow has no name of its own (OS/2): renaming it renames the
     * original, and the shadow shows the new name with it. */
    struct row *rw = is_tree(f) ? NULL : row_at(f, i);
    if (rw && rw->shadow && rw->target && *rw->target) {
        if (rw->title && !strcmp(rw->title, name)) return;
        svc_rename(rw->target, name, renamed, as_arg(f));
        return;
    }
    if (old && !strcmp(old, name)) return;
    /* Shown at once (the service's answer and the change's event follow
     * in a moment; a refused rename reads the folder again, which puts
     * the old title back). */
    if (rw) {
        free(rw->title);
        rw->title = strdup(name);
        ui_container_changed(f->cont, i, i);
    }
    /* its place goes with it (the file's new name; a line break is a space there) */
    if (old) {
        char fsname[NAME_MAX + 1];
        snprintf(fsname, sizeof fsname, "%s", name);
        for (char *q = fsname; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
        layout_rename(f->lay, old, fsname);
    }
    svc_rename(t, name, renamed, as_arg(f));
}


/* ---------------------------------------------------------- drag and drop */

#define MIME_WPS "application/x-wps-objects"
#define MIME_URIS "text/uri-list"

/* What this program is dragging now (for refusing a folder dropped into
 * itself before the data is read). */
struct dragging {
    char **paths;
    bool *tpl;               /* templates: dropping one makes a new object */
    size_t n;
    char src_folder[PATH_MAX];
    char *wps, *uris;        /* the data in both formats */
    double *dx, *dy;         /* each icon's offset from the grab point (icon view) */
};
static struct dragging *cur_drag;

static void dragging_free(struct dragging *d)
{
    if (!d) return;
    for (size_t i = 0; i < d->n; i++) free(d->paths[i]);
    free(d->paths);
    free(d->tpl);
    free(d->dx);
    free(d->dy);
    free(d->wps);
    free(d->uris);
    free(d);
}

/* Append text with %-escapes for a file: URI. */
static void uri_append(char **buf, size_t *len, size_t *cap, const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t need = *len + strlen(path) * 3 + 16;
    if (need > *cap) { while (*cap < need) *cap = *cap ? *cap * 2 : 1024; *buf = realloc(*buf, *cap); }
    *len += (size_t)sprintf(*buf + *len, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("/-._~", *p))
            (*buf)[(*len)++] = (char)*p;
        else { (*buf)[(*len)++] = '%'; (*buf)[(*len)++] = hex[*p >> 4]; (*buf)[(*len)++] = hex[*p & 15]; }
    }
    (*buf)[(*len)++] = '\r';
    (*buf)[(*len)++] = '\n';
    (*buf)[*len] = 0;
}

static void drag_send(void *ctx, const char *mime, int fd)
{
    struct dragging *d = ctx;
    const char *data = !strcmp(mime, MIME_WPS) ? d->wps : d->uris;
    ui_app_write_async(app, fd, data, strlen(data));
}

static void drag_end(void *ctx, bool dropped, uint32_t action)
{
    (void)dropped; (void)action;
    if (cur_drag == ctx) cur_drag = NULL;
    dragging_free(ctx);
}

/* Start dragging what d holds (its paths and both formats ready). */
static void start_dragging(struct dragging *d, ui_surface *win, uint32_t serial, ui_kind kind)
{
    if (!d->wps || !d->uris) { dragging_free(d); return; }
    /* The drag image: the item's icon, and how many go. */
    int sc = ui_surface_scale(win);
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48 * sc, 40 * sc);
    cairo_surface_set_device_scale(img, sc, sc);
    cairo_t *cr = cairo_create(img);
    ui_draw_kind_icon(cr, kind, 0, 4, 32);
    if (d->n > 1) {
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%zu", d->n);
        double w = ui_text_width(UI_FONT_BOLD, cnt) + 8;
        ui_fill(cr, (ui_rect){ 46 - w, 22, w, 16 }, ui_scheme_now.selection);
        ui_set_color(cr, ui_scheme_now.selection_text);
        ui_text_draw(cr, UI_FONT_BOLD, 50 - w, 35, cnt, 0);
    }
    cairo_destroy(cr);
    const char *mimes[] = { MIME_WPS, MIME_URIS };
    cur_drag = d;
    if (!ui_drag_start(win, serial, mimes, 2, UI_DND_COPY | UI_DND_MOVE | UI_DND_ASK, img, sc, drag_send, drag_end, d)) {
        cur_drag = NULL;
        dragging_free(d);
    }
    cairo_surface_destroy(img);
}

static void src_drag(void *ctx, size_t i, uint32_t serial)
{
    struct folder *f = ctx;
    struct dragging *d = calloc(1, sizeof *d);
    raise_in(f, "object.drag", NULL, "");
    /* The selection if the item is in it, else the item alone (OS/2). */
    size_t total = is_tree(f) ? f->ntvis : f->m.total;
    bool whole = ui_container_is_selected(f->cont, i);
    size_t cap = 0;
    for (size_t k = whole ? 0 : i; k < (whole ? total : i + 1); k++) {
        if (whole && !ui_container_is_selected(f->cont, k)) continue;
        char t[PATH_MAX];
        target_of(f, k, t, sizeof t);
        if (d->n == cap) {
            cap = cap ? cap * 2 : 16;
            d->paths = realloc(d->paths, cap * sizeof *d->paths);
            d->tpl = realloc(d->tpl, cap * sizeof *d->tpl);
            d->dx = realloc(d->dx, cap * sizeof *d->dx);
            d->dy = realloc(d->dy, cap * sizeof *d->dy);
        }
        double gx, gy, sx, sy, ix = 0, iy = 0;
        ui_container_drag_origin(f->cont, &sx, &sy);
        ui_container_to_list(f->cont, sx, sy, &gx, &gy);
        if (!is_tree(f)) layout_pos(f->lay, k, &ix, &iy);
        d->dx[d->n] = ix - gx;
        d->dy[d->n] = iy - gy;
        struct row *rw = row_at(f, k);
        d->tpl[d->n] = rw && rw->tpl;
        d->paths[d->n++] = strdup(t);
    }
    size_t wl = 0, wc = 0, ul = 0, uc = 0;
    for (size_t k = 0; k < d->n; k++) {
        size_t need = wl + strlen(d->paths[k]) + 12;
        if (need > wc) { while (wc < need) wc = wc ? wc * 2 : 1024; d->wps = realloc(d->wps, wc); }
        /* "<path>\ttemplate" for a template (WPS's own type only) */
        wl += (size_t)sprintf(d->wps + wl, "%s%s\n", d->paths[k], d->tpl[k] ? "\ttemplate" : "");
        if (strncmp(d->paths[k], "vobj:", 5)) uri_append(&d->uris, &ul, &uc, d->paths[k]);   /* central: no file to offer */
    }
    if (!d->uris) d->uris = strdup("");
    snprintf(d->src_folder, sizeof d->src_folder, "%s", f->path);
    ui_item it = { 0 };
    src_item(f, i, &it);
    start_dragging(d, f->win, serial, it.loaded ? it.kind : UI_KIND_FILE);
}

/* Find's results dragged (paths from anywhere): moved, copied or made
 * shadows of wherever they are dropped, as from a folder. */
void shell_drag_paths(ui_surface *win, uint32_t serial, const char *const *paths, size_t n, ui_kind kind)
{
    if (!n) return;
    struct dragging *d = calloc(1, sizeof *d);
    d->paths = calloc(n, sizeof *d->paths);
    d->tpl = calloc(n, sizeof *d->tpl);
    d->dx = calloc(n, sizeof *d->dx);
    d->dy = calloc(n, sizeof *d->dy);
    size_t wl = 0, wc = 0, ul = 0, uc = 0;
    for (size_t k = 0; k < n; k++) {
        d->paths[d->n] = strdup(paths[k]);
        d->dx[d->n] = d->dy[d->n] = 16.0 * (double)k;   /* a small step each, where they land */
        d->n++;
        size_t need = wl + strlen(paths[k]) + 12;
        if (need > wc) { while (wc < need) wc = wc ? wc * 2 : 1024; d->wps = realloc(d->wps, wc); }
        wl += (size_t)sprintf(d->wps + wl, "%s\n", paths[k]);
        if (strncmp(paths[k], "vobj:", 5)) uri_append(&d->uris, &ul, &uc, paths[k]);
    }
    if (!d->uris) d->uris = strdup("");
    if (!d->wps) { dragging_free(d); return; }
    start_dragging(d, win, serial, kind);   /* (no source folder: they come from many) */
}

/* Where a drop at (x, y) goes: a folder under the pointer, else the
 * window's folder. */
static void drop_dest(struct folder *f, double x, double y, char *out, size_t n, size_t *item)
{
    size_t i = ui_container_item_at(f->cont, x, y);
    ui_item it = { 0 };
    if (i != SIZE_MAX) src_item(f, i, &it);
    /* onto its own icon (a short drag): the folder it is in, not what it
     * is (a folder's shadow would otherwise go into its own original) */
    if (i != SIZE_MAX && dragged_onto_itself(f, i)) i = SIZE_MAX;
    if (i != SIZE_MAX && it.loaded && it.kind == UI_KIND_FOLDER) {
        struct row *rw = row_at(f, i);
        if (rw && rw->shadow && rw->target) snprintf(out, n, "%s", rw->target);   /* into the original */
        else target_of(f, i, out, n);
        *item = i;
    } else {
        snprintf(out, n, "%s", f->path);
        *item = SIZE_MAX;
    }
}

static bool shadow_mods(unsigned mods) { return ui_mouse_drop_how(mods) == 2; }

/* Dragged out of a folder this user cannot change (/usr/share/applications,
 * another user's): a move there could only copy and then fail to remove
 * the originals, so a plain drag elsewhere copies, as GNOME's Files does. */
static bool drag_copies_by_default(const char *dest)
{
    if (!cur_drag || !strcmp(cur_drag->src_folder, dest)) return false;
    for (size_t i = 0; i < cur_drag->n; i++) {
        if (!strncmp(cur_drag->paths[i], "vobj:", 5)) continue;
        /* each item's own folder (Find's results come from many) */
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", cur_drag->paths[i]);
        char *sl = strrchr(parent, '/');
        if (!sl) continue;
        if (sl == parent) sl[1] = 0; else *sl = 0;
        if (strcmp(parent, dest) && access(parent, W_OK) != 0) return true;
    }
    return false;
}

static bool under(const char *path, const char *dir)
{
    size_t l = strlen(dir);
    return !strncmp(path, dir, l) && (path[l] == '/' || path[l] == 0);
}

/* A program's icon under (x, y) (a program object, a .desktop file, an
 * executable, or a shadow of one): objects dropped on it start it with
 * them, as in OS/2. */
/* (1: it takes them; -1: a program that does not open what is dragged -
 * the drop is refused, nothing moves; 0: no program there) */
static int program_at(struct folder *f, double x, double y, char *out, size_t n, size_t *item)
{
    size_t i = ui_container_item_at(f->cont, x, y);
    if (i == SIZE_MAX || is_tree(f)) return 0;
    ui_item it = { 0 };
    src_item(f, i, &it);
    if (!it.loaded || it.kind != UI_KIND_PROGRAM) return 0;
    target_of(f, i, out, n);
    *item = i;
    /* not onto itself, dragged */
    for (size_t k = 0; cur_drag && k < cur_drag->n; k++) if (!strcmp(cur_drag->paths[k], out)) return 0;
    /* what it opens (its desktop entry's types; a shadow's original's) */
    if (cur_drag) {
        struct row *rw = row_at(f, i);
        const char *prog = rw && rw->shadow && rw->target && rw->target[0] == '/' ? rw->target : out;
        const char **files = calloc(cur_drag->n ? cur_drag->n : 1, sizeof *files);
        size_t nf = 0;
        for (size_t k = 0; k < cur_drag->n; k++) if (cur_drag->paths[k][0] == '/') files[nf++] = cur_drag->paths[k];
        bool ok = nf && program_opens(prog, files, nf);
        free(files);
        if (!ok) return -1;
    }
    return 1;
}

static uint32_t win_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n,
                                uint32_t src_actions, unsigned mods, const char **mime)
{
    struct folder *f = ui_surface_data(s);
    *mime = NULL;
    for (size_t i = 0; i < n && !*mime; i++) if (!strcmp(mimes[i], MIME_WPS)) *mime = MIME_WPS;
    for (size_t i = 0; i < n && !*mime; i++) if (!strcmp(mimes[i], MIME_URIS)) *mime = MIME_URIS;
    if (!*mime) return 0;
    char dest[PATH_MAX];
    size_t item;
    int pa = program_at(f, x, y, dest, sizeof dest, &item);
    if (pa < 0) { ui_container_set_drop_target(f->cont, SIZE_MAX); return 0; }   /* it does not open these */
    if (pa > 0) {
        /* (a copy, never a move: the objects stay where they are) */
        ui_container_set_drop_target(f->cont, src_actions & UI_DND_COPY ? item : SIZE_MAX);
        return src_actions & UI_DND_COPY ? UI_DND_COPY : 0;
    }
    drop_dest(f, x, y, dest, sizeof dest, &item);
    if (is_trash_dir(dest) && !(cur_drag && !strcmp(cur_drag->src_folder, dest)) && !dragged_onto_itself(f, item)) {
        /* onto the Trash Can: into the trash (a copy for the source: it
         * removes nothing itself) */
        ui_container_set_drop_target(f->cont, item);
        return src_actions & UI_DND_COPY ? UI_DND_COPY : src_actions & UI_DND_MOVE ? UI_DND_MOVE : 0;
    }
    if (is_startup_dir(dest) && !(cur_drag && !strcmp(cur_drag->src_folder, dest)) && !dragged_onto_itself(f, item)) {
        /* onto Startup: an entry or a shadow is made there (a copy for the source) */
        ui_container_set_drop_target(f->cont, item);
        return src_actions & UI_DND_COPY ? UI_DND_COPY : src_actions & UI_DND_MOVE ? UI_DND_MOVE : 0;
    }
    /* As in OS/2, from the keys held (the Mouse object's mappings; by
     * default none moves, Ctrl copies, Ctrl+Shift makes a shadow, Alt
     * asks). GNOME's own choice is not used: for a button 2 drag it says
     * "ask", and now and then "copy", with no key held. A shadow is
     * answered as a copy (nothing is removed from the source). */
    int how = ui_mouse_drop_how(mods);
    if (how == 0 && drag_copies_by_default(dest)) how = 1;
    uint32_t act = how < 0 ? UI_DND_ASK : how ? UI_DND_COPY : UI_DND_MOVE;
    if (!(act & src_actions)) act = src_actions & UI_DND_COPY ? UI_DND_COPY : src_actions & UI_DND_MOVE ? UI_DND_MOVE : 0;
    if (cur_drag) {
        for (size_t i = 0; i < cur_drag->n; i++) {
            if (under(dest, cur_drag->paths[i])) act = 0;   /* a folder into itself */
            char parent[PATH_MAX];
            snprintf(parent, sizeof parent, "%s", cur_drag->paths[i]);
            char *sl = strrchr(parent, '/');
            if (sl) *sl = 0;
            if (act == UI_DND_MOVE && !shadow_mods(mods) && !strcmp(parent, dest) && cur_drag->n == 1 &&
                ui_container_view(f->cont) != UI_VIEW_ICON)
                act = 0;   /* already there (in icon view: moving the icon) */
        }
    }
    ui_container_set_drop_target(f->cont, act ? item : SIZE_MAX);
    return act;
}

static void win_drag_leave(ui_surface *s)
{
    struct folder *f = ui_surface_data(s);
    ui_container_set_drop_target(f->cont, SIZE_MAX);
}

/* Jobs started by drops: progress in the target window, decisions asked. */
struct jobtrack {
    struct jobtrack *next;
    int64_t job;
    long folder;
    char kind[8];
    bool asking;
    char dest[PATH_MAX];   /* where the objects go */
};
static struct jobtrack *jobs;
static ui_dialog *last_dialog;   /* for self-tests */

struct xfer {
    long folder;
    char kind[8], dest[PATH_MAX], from[PATH_MAX];
    char **names;
    int64_t *x, *y;          /* where each lands (if place) */
    bool place;
    size_t n;
};

static void xfer_free(struct xfer *x)
{
    for (size_t i = 0; i < x->n; i++) free(x->names[i]);
    free(x->names);
    free(x->x);
    free(x->y);
    free(x);
}

static void set_status(long folder, const char *msg)
{
    struct folder *f = folder_by_id(folder);
    if (!f) return;
    snprintf(f->error, sizeof f->error, "%s", msg);
    damage_status(f);
}

/* An action event about a folder window (or an object in it). */
static void raise_in(struct folder *f, const char *name, const char *object, const char *cls)
{
    ui_rect at = f ? ui_frame_client(f->win) : (ui_rect){ 0, 0, 0, 0 };
    events_raise(name, object ? object : f ? f->path : "", cls, at);
}

/* Something failed: the status line says what, and alert.error sounds. */
static void report_error(long folder, const char *msg)
{
    set_status(folder, msg);
    struct folder *f = folder_by_id(folder);
    raise_in(f, "alert.error", NULL, "");
    if (f && f->desktop) messages_post(msg, "", 2);   /* (the desktop has no status line: Messages) */
}

/* A question to the user: alert.warning sounds, as OS/2's warning did. */
static ui_dialog *ask(const char *title, const char *msg, const char *const *buttons, size_t n, int def, ui_dialog_fn fn,
                      void *ctx)
{
    events_raise("alert.warning", "", "", (ui_rect){ 0, 0, 0, 0 });
    return ui_dialog_open(app, title, msg, buttons, n, def, fn, ctx);
}

static void got_job(void *arg, const cv *reply)
{
    struct xfer *x = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && res->k == CV_INT) {
        struct jobtrack *t = calloc(1, sizeof *t);
        t->job = res->i;
        t->folder = x->folder;
        snprintf(t->kind, sizeof t->kind, "%s", x->kind);
        snprintf(t->dest, sizeof t->dest, "%s", x->dest);
        t->next = jobs;
        jobs = t;
    } else {
        char m[300];
        snprintf(m, sizeof m, "Cannot %s: %s", x->kind, reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        report_error(x->folder, m);
    }
    xfer_free(x);
}

static void got_sel(void *arg, const cv *reply)
{
    struct xfer *x = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res || res->k != CV_INT) { set_status(x->folder, "Cannot select the dragged objects"); xfer_free(x); return; }
    svc_job_start(x->kind, res->i, x->dest, (const char *const *)x->names, x->x, x->y, x->place ? x->n : 0, got_job, x);
}


/* "file:///a%20b" -> "/a b"; NULL if not a local file. */
static char *uri_path(const char *u)
{
    if (strncmp(u, "file://", 7)) return NULL;
    u += 7;
    if (!strncmp(u, "localhost", 9)) u += 9;
    if (*u != '/') return NULL;
    char *out = malloc(strlen(u) + 1), *o = out;
    for (; *u; u++) {
        if (*u == '%' && u[1] && u[2]) {
            char h[3] = { u[1], u[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            u += 2;
        } else *o++ = *u;
    }
    *o = 0;
    return out;
}

enum { DROP_MOVE, DROP_COPY, DROP_SHADOW };

/* Icons dragged within their own folder, in icon view: they go where they
 * were dropped, each keeping its offset from the grab point, and the
 * places are stored. Returns false if the drag came from elsewhere. */
static bool reposition(struct folder *f, double x, double y)
{
    struct dragging *d = cur_drag;
    for (size_t i = 0; i < d->n; i++) {
        if (!strncmp(d->paths[i], "vobj:", 5)) {
            if (strcmp(d->src_folder, f->path)) return false;
            continue;
        }
        const char *sl = strrchr(d->paths[i], '/');
        size_t pl = sl ? (size_t)(sl - d->paths[i]) : 0;
        if (pl != strlen(f->path) || strncmp(d->paths[i], f->path, pl)) return false;
    }
    double lx, ly;
    ui_container_to_list(f->cont, x, y, &lx, &ly);
    int64_t *px = calloc(d->n, sizeof *px), *py = calloc(d->n, sizeof *py);
    const char **moved = calloc(d->n ? d->n : 1, sizeof *moved);
    size_t k = 0;
    for (size_t i = 0; i < d->n; i++) {
        char nm[NAME_MAX + 8];
        if (!strncmp(d->paths[i], "vobj:", 5)) snprintf(nm, sizeof nm, "\x01%s", d->paths[i]);
        else snprintf(nm, sizeof nm, "%s", strrchr(d->paths[i], '/') + 1);
        if (layout_is_locked(f->lay, nm)) continue;   /* locked: stays (it can still go to other folders) */
        double nx = lx + d->dx[i], ny = ly + d->dy[i];
        px[k] = nx < 0 ? 0 : (int64_t)nx;
        py[k] = ny < 0 ? 0 : (int64_t)ny;
        moved[k] = d->paths[i];
        layout_place(f->lay, nm, (int)px[k], (int)py[k]);
        k++;
    }
    if (k < d->n) {
        snprintf(f->error, sizeof f->error, "%s", k ? "Locked icons stay where they are" : "The position is locked");
        damage_status(f);
    }
    if (k) svc_place(f->path, moved, px, py, k, placed_reply, as_arg(f));
    free(moved);
    free(px);
    free(py);
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
    ui_container_layout_changed(f->cont);
    return true;
}

/* Where dropped items land: on the window's own folder's background in icon
 * view, at the drop point (in list coordinates), each keeping its offset in
 * the drag; elsewhere (onto a folder icon, other views) wherever is free. */
struct drop_at { bool place; double lx, ly; };

static void do_drop(struct folder *f, const char *dest, const char *mime, const char *data, int how, struct drop_at at);

/* The compositor said "ask" (Alt in GNOME): a menu at the drop point. */
struct ask_ctx { long folder; char dest[PATH_MAX]; char *mime, *data; struct drop_at at; };

static void ask_chosen(void *arg, int64_t id)
{
    struct ask_ctx *a = arg;
    struct folder *f = folder_by_id(a->folder);
    if (f && id >= DROP_MOVE && id <= DROP_SHADOW) do_drop(f, a->dest, a->mime, a->data, (int)id, a->at);
    free(a->mime);
    free(a->data);
    free(a);
}

/* How a drop is meant, as the Mouse object maps the keys (OS/2's: none moves, Ctrl copies, Ctrl+Shift
  * makes a shadow, Alt asks). Keys held when the drag began reach us; keys
 * pressed during it reach only GNOME, which then changes its choice: to
 * copy for Ctrl, to move for Shift or Ctrl+Shift (it cannot tell them
 * apart, so a change to move means a shadow). */
static int drop_how(uint32_t action, uint32_t start_action, unsigned mods)
{
    int how = ui_mouse_drop_how(mods);   /* the keys held, as mapped */
    if (how) return how;
    if (start_action && action != start_action) {
        if (action == UI_DND_COPY) return 1;
        if (action == UI_DND_MOVE) return 2;
        if (action == UI_DND_ASK) return -1;
    }
    return 0;
}

static uint32_t win_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len, uint32_t action,
                     uint32_t start_action, unsigned mods)
{
    (void)len;
    struct folder *f = ui_surface_data(s);
    ui_container_set_drop_target(f->cont, SIZE_MAX);
    char dest[PATH_MAX];
    size_t item;
    char prog[PATH_MAX];
    int pa = program_at(f, x, y, prog, sizeof prog, &item);
    if (pa < 0) return 0;   /* refused: nothing moves, the drag ends where it began */
    if (pa > 0) {
        /* dropped on a program: it starts with them */
        const char *files[32];
        size_t nf = 0;
        char *copy = strdup(data), *save = NULL;
        for (char *line = strtok_r(copy, "\r\n", &save); line && nf < 32; line = strtok_r(NULL, "\r\n", &save)) {
            if (!*line || *line == '#') continue;
            char *tab = !strcmp(mime, MIME_URIS) ? NULL : strchr(line, '\t');
            if (tab) *tab = 0;
            char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
            if (p && p[0] == '/') files[nf++] = p;
            else free(p);
        }
        free(copy);
        /* (from another program: what it opens is checked only now) */
        struct row *prw = row_at(f, item);
        const char *pp = prw && prw->shadow && prw->target && prw->target[0] == '/' ? prw->target : prog;
        if (nf && !program_opens(pp, files, nf)) {
            for (size_t k = 0; k < nf; k++) free((char *)files[k]);
            snprintf(f->error, sizeof f->error, "That program does not open this kind of file");
            damage_status(f);
            return 0;
        }
        if (nf) {
            snprintf(f->pending_open, sizeof f->pending_open, "%s", prog);
            raise_in(f, "object.drop", prog, "program");
            svc_open_files(prog, false, files, nf, opened, as_arg(f));
        } else {
            snprintf(f->error, sizeof f->error, "Only files can be given to a program");
            damage_status(f);
        }
        for (size_t k = 0; k < nf; k++) free((char *)files[k]);
        return UI_DND_COPY;
    }
    drop_dest(f, x, y, dest, sizeof dest, &item);
    if (is_trash_dir(dest) && !(cur_drag && !strcmp(cur_drag->src_folder, dest)) && !dragged_onto_itself(f, item)) {
        trash_dropped(f, mime, data);
        return UI_DND_COPY;
    }
    if (is_startup_dir(dest) && !(cur_drag && !strcmp(cur_drag->src_folder, dest)) && !dragged_onto_itself(f, item)) {
        startup_dropped(f, mime, data);
        return UI_DND_COPY;
    }
    int w = drop_how(action, start_action, mods);
    if (w == 0 && drag_copies_by_default(dest)) w = 1;
    struct drop_at at = { item == SIZE_MAX && !strcmp(dest, f->path) && ui_container_view(f->cont) == UI_VIEW_ICON, 0, 0 };
    if (at.place) ui_container_to_list(f->cont, x, y, &at.lx, &at.ly);
    int how = w == 2 ? DROP_SHADOW : w == 1 ? DROP_COPY : DROP_MOVE;
    if (w == 0 && how == DROP_MOVE && cur_drag && !strcmp(dest, f->path) && ui_container_view(f->cont) == UI_VIEW_ICON &&
        reposition(f, x, y))
        return UI_DND_MOVE;
    if (w < 0) {
        struct ask_ctx *a = calloc(1, sizeof *a);
        a->folder = f->id;
        snprintf(a->dest, sizeof a->dest, "%s", dest);
        a->mime = strdup(mime);
        a->data = strdup(data);
        a->at = at;
        ui_menu_item items[] = {
            { DROP_MOVE, "Move here", true, false, NULL, 0 },
            { DROP_COPY, "Copy here", true, false, NULL, 0 },
            { DROP_SHADOW, "Create shadow here", true, false, NULL, 0 },
            { -1, "-", false, false, NULL, 0 },
            { 99, "Cancel", true, false, NULL, 0 },
        };
        ui_menu_popup(s, items, 5, x, y, 0, ask_chosen, a);
        return UI_DND_COPY;   /* decided later in the menu: the source must not remove anything */
    }
    if (getenv("WPS_DEBUG_DND"))
        fprintf(stderr, "dnd: dropping as %s\n", how == DROP_SHADOW ? "shadow" : how == DROP_COPY ? "copy" : "move");
    raise_in(f, "object.drop", dest, how == DROP_SHADOW ? "shadow" : how == DROP_COPY ? "copy" : "move");
    do_drop(f, dest, mime, data, how, at);
    return how == DROP_MOVE ? UI_DND_MOVE : UI_DND_COPY;
}

/* A shadow or central object made or moved by a drop: place it where it
 * was dropped once the service gives its address. */
struct land { long folder; char dest[PATH_MAX]; int64_t x, y; bool place; };

static void landed(void *arg, const cv *reply);

/* The new object made here, once listed: selected, its name being edited. */
static void rename_new_now(struct folder *f)
{
    if (!f->rename_new[0]) return;
    for (size_t i = 0; i < f->m.total; i++)
        if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), f->rename_new)) {
            f->rename_new[0] = 0;
            /* Typing goes to this window (the drop came from another). */
            ui_window_activate(f->win);
            /* The object just asked for takes over from a name still open. */
            if (ui_container_rename_item(f->cont) != SIZE_MAX) ui_container_rename_moved(f->cont, SIZE_MAX);
            ui_container_select_none(f->cont);
            ui_container_select(f->cont, i);
            ui_container_set_cursor(f->cont, i);
            ui_container_begin_rename(f->cont, i);
            return;
        }
}

/* Create another (OS/2): a new, empty object of the same kind beside
 * item i (the folder itself for SIZE_MAX), its name ready to edit. */
static void made_new(void *arg, const cv *reply);
static void create_another(struct folder *f, size_t i)
{
    struct row *rw = row_at(f, i);
    char dir[PATH_MAX], title[NAME_MAX + 32];
    const char *cls;
    if (i == SIZE_MAX || is_tree(f)) {
        char t[PATH_MAX];
        target_of(f, i, t, sizeof t);
        char *sl = strrchr(t, '/');
        if (!sl || sl == t) return;
        *sl = 0;
        snprintf(dir, sizeof dir, "%s", t);
        cls = "WPFolder";
        snprintf(title, sizeof title, "New Folder");
    } else {
        if (!rw || rw->shadow || rw->link || rw->name[0] == '\x01' || rw->tpl) {
            set_status(f->id, "Create another works on files, folders and programs (drag a template to make one from it)");
            return;
        }
        snprintf(dir, sizeof dir, "%s", f->path);
        size_t nl = strlen(rw->name);
        if (rw->kind == UI_KIND_FOLDER) { cls = "WPFolder"; snprintf(title, sizeof title, "New Folder"); }
        else if (nl > 8 && !strcmp(rw->name + nl - 8, ".desktop")) { cls = "WPProgram"; snprintf(title, sizeof title, "New Program"); }
        else {
            /* a data file of the same type: the same extension */
            const char *dot = strrchr(rw->name, '.');
            cls = "WPDataFile";
            snprintf(title, sizeof title, "New Data File%s", dot && dot != rw->name ? dot : "");
        }
    }
    struct land *l = calloc(1, sizeof *l);
    l->folder = f->id;
    snprintf(l->dest, sizeof l->dest, "%s", dir);
    svc_create(dir, cls, title, made_new, l);
}

/* A new object was made (from a template, or Create another): place it
 * where it was dropped, and edit its name once the window lists it. */
static void made_new(void *arg, const cv *reply)
{
    struct land *l = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *addr = res ? cv_text(res, "address") : NULL, *path = res ? cv_text(res, "path") : NULL;
    if (!addr || !*addr) addr = path;
    if (!res || !addr || !*addr) {
        char m[300];
        snprintf(m, sizeof m, "Not made: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(l->folder, m);
        free(l);
        return;
    }
    char nm[NAME_MAX + 64];
    if (!strncmp(addr, "vobj:", 5)) snprintf(nm, sizeof nm, "\x01%s", addr);
    else snprintf(nm, sizeof nm, "%s", strrchr(addr, '/') ? strrchr(addr, '/') + 1 : addr);
    raise_in(folder_by_id(l->folder), "object.create", addr, "");
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing && !strcmp(o->path, l->dest) && !is_tree(o)) {
            snprintf(o->rename_new, sizeof o->rename_new, "%s", nm);
            rename_new_now(o);   /* already listed (the folder's change came first) */
        }
    struct folder *f = folder_by_id(l->folder);
    if (l->place && f) {
        const char *t[1] = { addr };
        svc_place(l->dest, t, &l->x, &l->y, 1, placed_reply, as_arg(f));
        layout_place(f->lay, nm, (int)l->x, (int)l->y);   /* before the listing catches up */
    }
    free(l);
}

static void landed(void *arg, const cv *reply)
{
    struct land *l = arg;
    struct folder *f = folder_by_id(l->folder);
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    const char *addr = reply ? cv_text(reply, "result") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b) || !addr) {
        char m[300];
        snprintf(m, sizeof m, "Not done: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(l->folder, m);
    } else if (l->place && f) {
        const char *t[1] = { addr };
        svc_place(l->dest, t, &l->x, &l->y, 1, placed_reply, as_arg(f));
        char nm[NAME_MAX + 8];
        if (!strncmp(addr, "vobj:", 5)) snprintf(nm, sizeof nm, "\x01%s", addr);
        else snprintf(nm, sizeof nm, "%s", strrchr(addr, '/') ? strrchr(addr, '/') + 1 : addr);
        layout_place(f->lay, nm, (int)l->x, (int)l->y);   /* before the listing catches up */
    }
    free(l);
}

static void do_drop(struct folder *f, const char *dest, const char *mime, const char *data, int how, struct drop_at at)
{
    size_t k = 0;   /* the item's place in the drag */
    bool shadow = how == DROP_SHADOW;
    const char *kind = how == DROP_COPY ? "copy" : "move";
    /* Paths, grouped by the folder they come from (a job per folder). */
    char *copy = strdup(data), *save = NULL;
    struct xfer *groups[64];
    size_t ng = 0, total = 0;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        bool tpl = false;
        char *tab = !strcmp(mime, MIME_URIS) ? NULL : strchr(line, '\t');
        if (tab) { tpl = !strcmp(tab + 1, "template"); *tab = 0; }
        char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
        if (!p) continue;
        total++;
        /* Where this item lands: its offset in our own drag, or a small
         * step per item for drags from other programs. */
        double ox = 16.0 * (double)k, oy = 16.0 * (double)k;
        if (cur_drag && k < cur_drag->n && !strcmp(cur_drag->paths[k], p)) { ox = cur_drag->dx[k]; oy = cur_drag->dy[k]; }
        int64_t px = at.lx + ox < 0 ? 0 : (int64_t)(at.lx + ox), py = at.ly + oy < 0 ? 0 : (int64_t)(at.ly + oy);
        k++;
        if (tpl && !shadow) {
            /* A template dropped (moved or copied): a new object from it,
             * where it was dropped, with its name ready to edit (OS/2). */
            struct land *l = calloc(1, sizeof *l);
            l->folder = f->id;
            snprintf(l->dest, sizeof l->dest, "%s", dest);
            l->x = px;
            l->y = py;
            l->place = at.place;
            svc_from_template(p, dest, NULL, made_new, l);
            free(p);
            continue;
        }
        if (shadow || !strncmp(p, "vobj:", 5)) {
            struct land *l = calloc(1, sizeof *l);
            l->folder = f->id;
            snprintf(l->dest, sizeof l->dest, "%s", dest);
            l->x = px;
            l->y = py;
            l->place = at.place;
            if (shadow) svc_shadow(p, dest, landed, l);
            else if (!strcmp(kind, "move") && cur_drag && !strcmp(cur_drag->src_folder, dest)) free(l);   /* already there */
            else svc_vmove(p, dest, !strcmp(kind, "copy"), landed, l);
            free(p);
            continue;
        }
        char *sl = strrchr(p, '/');
        if (!sl || under(dest, p)) { free(p); continue; }   /* never into itself */
        *sl = 0;
        const char *from = p[0] ? p : "/", *name = sl + 1;
        if (!strcmp(kind, "move") && !strcmp(from, dest)) { free(p); continue; }   /* already there */
        struct xfer *g = NULL;
        for (size_t k = 0; k < ng && !g; k++) if (!strcmp(groups[k]->from, from)) g = groups[k];
        if (!g && ng < 64) {
            g = calloc(1, sizeof *g);
            g->folder = f->id;
            snprintf(g->kind, sizeof g->kind, "%s", kind);
            snprintf(g->dest, sizeof g->dest, "%s", dest);
            snprintf(g->from, sizeof g->from, "%s", from);
            groups[ng++] = g;
        }
        if (g) {
            g->names = realloc(g->names, (g->n + 1) * sizeof *g->names);
            g->x = realloc(g->x, (g->n + 1) * sizeof *g->x);
            g->y = realloc(g->y, (g->n + 1) * sizeof *g->y);
            g->x[g->n] = px;
            g->y[g->n] = py;
            g->place = at.place;
            g->names[g->n++] = strdup(name);
        }
        free(p);
    }
    free(copy);
    for (size_t k = 0; k < ng; k++) svc_select(groups[k]->from, (const char *const *)groups[k]->names, groups[k]->n, got_sel, groups[k]);
    char m[300];
    snprintf(m, sizeof m, "%s %zu object%s...", shadow ? "Making shadows of" : !strcmp(kind, "copy") ? "Copying" : "Moving",
             total, total == 1 ? "" : "s");
    set_status(f->id, m);
}

struct decide_ctx { int64_t job; };

static void decided(void *arg, int choice)
{
    struct decide_ctx *d = arg;
    for (struct jobtrack *t = jobs; t; t = t->next)
        if (t->job == d->job) t->asking = false;
    switch (choice) {
    case 0: svc_job_decide(d->job, "rename", false); break;
    case 1: svc_job_decide(d->job, "skip", false); break;
    case 2: svc_job_decide(d->job, "rename", true); break;
    case 3: svc_job_decide(d->job, "skip", true); break;
    default: svc_job_cancel(d->job); break;
    }
    free(d);
}

/* ---------------------------------------------------------------- delete */

/* Confirmations, as on OS/2's System object (its Confirmations page, read
 * with the other settings objects); <config>/system.conf, read first, is
 * 0.1's older place for them. Deleting for good always asks. */
static bool confirm_delete = true, confirm_folder_delete = true;

static void load_system_conf(void)
{
    char path[4096];
    const char *cfg = getenv("WPS_CONFIG_DIR"), *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cfg && *cfg) snprintf(path, sizeof path, "%s/system.conf", cfg);
    else if (xdg && *xdg) snprintf(path, sizeof path, "%s/wps/system.conf", xdg);
    else snprintf(path, sizeof path, "%s/.config/wps/system.conf", home ? home : "");
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        char k[64], v[64];
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %63s", k, v) != 2) continue;
        bool on = !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
        if (!strcmp(k, "confirm_delete")) confirm_delete = on;
        else if (!strcmp(k, "confirm_folder_delete")) confirm_folder_delete = on;
        else fprintf(stderr, "wps-shell: %s: unknown setting \"%s\"\n", path, k);
    }
    fclose(fp);
}

struct del {
    long folder;
    bool permanent;
    char parent[PATH_MAX];   /* the folder the files are in */
    char **names;            /* files and folders there */
    size_t n;
    char **vobjs;            /* central objects ("vobj:ID"): deleted outright */
    size_t nv;
    struct del *next;        /* the same request's files in another folder (a tree's selection) */
};

static void del_free(struct del *d)
{
    while (d) {
        struct del *nx = d->next;
        for (size_t i = 0; i < d->n; i++) free(d->names[i]);
        for (size_t i = 0; i < d->nv; i++) free(d->vobjs[i]);
        free(d->names);
        free(d->vobjs);
        free(d);
        d = nx;
    }
}

/* The request's part for files in `parent` (one job per folder). */
static struct del *del_for(struct del *d, const char *parent)
{
    for (struct del *e = d;; e = e->next) {
        if (!e->n || !strcmp(e->parent, parent)) {
            if (!e->n) snprintf(e->parent, sizeof e->parent, "%s", parent);
            return e;
        }
        if (!e->next) {
            struct del *nd = calloc(1, sizeof *nd);
            nd->folder = d->folder;
            nd->permanent = d->permanent;
            e->next = nd;
        }
    }
}

static void push(char ***v, size_t *n, const char *s)
{
    *v = realloc(*v, (*n + 1) * sizeof **v);
    (*v)[(*n)++] = strdup(s);
}

static void vdeleted(void *arg, const cv *reply)
{
    long folder = (long)(intptr_t)arg;
    if (!reply || !cv_get(reply, "result")) {
        char m[300];
        snprintf(m, sizeof m, "Cannot delete: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(folder, m);
    }
}

static void start_deletion(struct del *d)
{
    for (struct del *e = d; e; e = e->next) {
        /* central objects (shadows ...): into the Trash Can, or for good */
        for (size_t i = 0; i < e->nv; i++)
            (e->permanent ? svc_vdelete : svc_vtrash)(e->vobjs[i], vdeleted, (void *)(intptr_t)e->folder);
        if (e->n) {
            struct xfer *x = calloc(1, sizeof *x);
            x->folder = e->folder;
            snprintf(x->kind, sizeof x->kind, "%s", e->permanent ? "delete" : "trash");
            snprintf(x->from, sizeof x->from, "%s", e->parent);
            x->names = e->names;
            x->n = e->n;
            e->names = NULL;
            e->n = 0;
            svc_select(x->from, (const char *const *)x->names, x->n, got_sel, x);
        }
    }
    del_free(d);
}

static void delete_confirmed(void *arg, int choice)
{
    struct del *d = arg;
    last_dialog = NULL;
    if (choice == 0) start_deletion(d);
    else del_free(d);
}

/* Delete item i (the folder itself for SIZE_MAX): the selection if i is in
 * it, else i alone (OS/2). Into the trash, or for good. */
static void delete_request(struct folder *f, size_t i, bool permanent)
{
    struct del *d = calloc(1, sizeof *d);
    d->folder = f->id;
    d->permanent = permanent;
    bool any_folder = false;
    char first[PATH_MAX] = "";
    size_t total = is_tree(f) ? f->ntvis : f->m.total;
    bool whole = i != SIZE_MAX && ui_container_is_selected(f->cont, i);
    /* a tree's selection: folders in other folders; an item whose folder is
     * selected too goes with it */
    char **sel = NULL;
    size_t nsel = 0;
    if (whole && is_tree(f))
        for (size_t k = 0; k < total; k++)
            if (ui_container_is_selected(f->cont, k)) {
                char t[PATH_MAX];
                target_of(f, k, t, sizeof t);
                push(&sel, &nsel, t);
            }
    for (size_t k = whole ? 0 : i; whole ? k < total : k == i; k++) {
        if (whole && !ui_container_is_selected(f->cont, k)) continue;
        char t[PATH_MAX];
        target_of(f, k, t, sizeof t);
        bool inside = false;
        for (size_t q = 0; q < nsel && !inside; q++) {
            size_t l = strlen(sel[q]);
            inside = l > 1 && !strncmp(t, sel[q], l) && t[l] == '/';
        }
        if (inside) continue;
        struct row *rw = row_at(f, k);
        const char *title = rw && rw->title ? rw->title : NULL;
        char p[PATH_MAX];   /* outlives the branches: title may point into it */
        if (!strncmp(t, "vobj:", 5)) {
            push(&d->vobjs, &d->nv, t);
        } else {
            /* the item's own path, not a shadow's or link's target */
            if (is_tree(f) || k == SIZE_MAX || !rw) snprintf(p, sizeof p, "%s", k == SIZE_MAX ? f->path : t);
            else snprintf(p, sizeof p, "%s/%s", strcmp(f->path, "/") ? f->path : "", rw->name);
            char *sl = strrchr(p, '/');
            if (!sl || !sl[1] || !strcmp(p, "/")) continue;
            char par[PATH_MAX];
            snprintf(par, sizeof par, "%.*s", sl == p ? 1 : (int)(sl - p), p);
            struct del *e = del_for(d, par);
            push(&e->names, &e->n, sl + 1);
            if (!title) title = sl + 1;
            any_folder |= k == SIZE_MAX || is_tree(f) || (rw && rw->kind == UI_KIND_FOLDER && !rw->shadow && !rw->link);
        }
        if (!first[0]) snprintf(first, sizeof first, "%s", title ? title : t);
        if (k == SIZE_MAX) break;
    }
    for (size_t q = 0; q < nsel; q++) free(sel[q]);
    free(sel);
    size_t n = 0;
    for (struct del *e = d; e; e = e->next) n += e->n + e->nv;
    if (!n) { del_free(d); return; }
    if (!permanent && !(any_folder ? confirm_folder_delete : confirm_delete)) { start_deletion(d); return; }
    char msg[700], what[400];
    if (n == 1) snprintf(what, sizeof what, "\"%s\"", first);
    else snprintf(what, sizeof what, "these %zu objects", n);
    if (permanent) snprintf(msg, sizeof msg, "Delete %s for good?\nThis cannot be undone.", what);
    else if (!d->n) snprintf(msg, sizeof msg, "Delete %s?", what);   /* shadows only: their originals stay */
    else snprintf(msg, sizeof msg, "Move %s to the trash?%s", what,
                  any_folder ? "\nFolders go with everything in them." : "");
    const char *buttons[] = { "Delete", "Cancel" };
    last_dialog = ask(permanent ? "Delete for good" : "Delete", msg, buttons, 2, 0, delete_confirmed, d);
}

/* Objects dropped on the Trash Can go to the trash (shadows are deleted,
 * as OS/2 did): one job per folder they came from. */
/* Over one of the icons being dragged (the Trash Can dragged a little
 * way): not a drop onto it. */
static bool dragged_onto_itself(struct folder *f, size_t item)
{
    if (!cur_drag || item == SIZE_MAX || strcmp(cur_drag->src_folder, f->path)) return false;
    const char *nm = model_name(&f->m, item);
    if (!nm) return false;
    char p[PATH_MAX];
    if (nm[0] == '\x01') snprintf(p, sizeof p, "%s", nm + 1);   /* a central object: vobj:ID */
    else snprintf(p, sizeof p, "%s/%s", f->path, nm);
    for (size_t k = 0; k < cur_drag->n; k++)
        if (!strcmp(cur_drag->paths[k], p)) return true;
    return false;
}

static void startup_added(void *arg, const cv *reply)
{
    long folder = (long)(intptr_t)arg;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        set_status(folder, "Added to Startup: it starts (or opens) at login");
        raise_in(folder_by_id(folder), "object.create", NULL, "");
    } else {
        char m[600];
        snprintf(m, sizeof m, "Cannot add to Startup: %s", reply && cv_text(reply, "message") ? cv_text(reply, "message") : "lost the connection to wpsd");
        report_error(folder, m);
    }
}

/* Dropped on Startup: programs and scripts become autostart entries,
 * other objects shadows (wpsd decides); nothing moves. */
static void startup_dropped(struct folder *f, const char *mime, const char *data)
{
    char *copy = strdup(data), *save = NULL;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char *tab = !strcmp(mime, MIME_URIS) ? NULL : strchr(line, '\t');
        if (tab) *tab = 0;
        char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
        if (!p) continue;
        svc_startup_add(p, startup_added, (void *)(intptr_t)f->id);
        free(p);
    }
    free(copy);
}

static void trash_dropped(struct folder *f, const char *mime, const char *data)
{
    struct del *by[16];
    size_t nb = 0;
    char *copy = strdup(data), *save = NULL;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char *tab = !strcmp(mime, MIME_URIS) ? NULL : strchr(line, '\t');
        if (tab) *tab = 0;
        char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
        if (!p) continue;
        if (!strncmp(p, "vobj:", 5)) {
            if (!nb) { by[0] = calloc(1, sizeof **by); by[0]->folder = f->id; nb = 1; }
            push(&by[0]->vobjs, &by[0]->nv, p);
            free(p);
            continue;
        }
        char *sl = strrchr(p, '/');
        if (!sl || sl == p || !sl[1]) { free(p); continue; }
        *sl = 0;
        struct del *d = NULL;
        for (size_t k = 0; k < nb && !d; k++) if (!strcmp(by[k]->parent, p) || (!by[k]->parent[0] && !by[k]->n)) d = by[k];
        if (!d && nb < 16) { d = by[nb++] = calloc(1, sizeof *d); d->folder = f->id; }
        if (d) {
            if (!d->parent[0]) snprintf(d->parent, sizeof d->parent, "%s", p);
            push(&d->names, &d->n, sl + 1);
        }
        free(p);
    }
    free(copy);
    for (size_t k = 0; k < nb; k++) start_deletion(by[k]);
    raise_in(f, "object.drop", "trash", "");
}

static void win_close(ui_surface *s);

/* Folder windows on folders that are gone (deleted, or in the trash) close. */
static void close_gone_windows(void)
{
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing && (access(o->path, F_OK) != 0 || strstr(o->path, "/Trash/files/") || strstr(o->path, "/.Trash-")))
            win_close(o->win);
}

static void forgood_confirmed(void *arg, int choice)
{
    struct del *d = arg;
    last_dialog = NULL;
    if (choice == 0) { d->permanent = true; start_deletion(d); }
    else del_free(d);
}

/* A trash or delete job's progress; true when it has ended. */
static bool deletion_event(struct jobtrack *t, const cv *st, const char *state, int64_t done, int64_t total)
{
    bool trash = !strcmp(t->kind, "trash");
    char m[400];
    const char *err = cv_text(st, "error"), *cur = cv_text(st, "current");
    if (strcmp(state, "done") && strcmp(state, "failed") && strcmp(state, "cancelled")) {
        snprintf(m, sizeof m, "%s %lld of %lld%s%s", trash ? "Moving to the trash" : "Deleting", (long long)done,
                 (long long)total, cur && *cur ? ": " : "", cur ? cur : "");
        set_status(t->folder, m);
        return false;
    }
    const cv *nt = cv_get(st, "not_trashed"), *miss = cv_get(st, "missing");
    size_t nn = nt && nt->k == CV_ARRAY ? nt->n : 0, nm = miss && miss->k == CV_ARRAY ? miss->n : 0;
    long long ok = (long long)done - (long long)nn - (long long)nm;
    if (!strcmp(state, "done"))
        snprintf(m, sizeof m, "%s %lld object%s%s%s", trash ? "Moved to the trash:" : "Deleted", ok, ok == 1 ? "" : "s",
                 nm ? "; some were gone already" : "", nn ? "; some cannot go to the trash" : "");
    else if (!strcmp(state, "failed")) snprintf(m, sizeof m, "Delete failed: %s", err ? err : "unknown error");
    else snprintf(m, sizeof m, "Delete cancelled after %lld", (long long)done);
    set_status(t->folder, m);
    if (!strcmp(state, "failed")) {
        raise_in(folder_by_id(t->folder), "job.failed", NULL, "");
        messages_post(m, err ? err : "", 2);   /* kept in Messages (a failure only: not every delete) */
    }
    else if (!strcmp(state, "done") && ok > 0) raise_in(folder_by_id(t->folder), trash ? "object.trash" : "object.delete", NULL, "");
    close_gone_windows();
    if (nn) {
        /* No trash takes them (a drive without one, or the trash itself):
         * ask before deleting them for good. */
        struct del *d = calloc(1, sizeof *d);
        d->folder = t->folder;
        const char *from = cv_text(st, "from");
        snprintf(d->parent, sizeof d->parent, "%s", from ? from : "");
        for (size_t i = 0; i < nn; i++)
            if (nt->items[i].k == CV_TEXT) push(&d->names, &d->n, nt->items[i].s);
        char msg[700];
        if (d->n == 1) snprintf(msg, sizeof msg, "\"%s\" cannot go to the trash (it is in the trash already, or its drive has none).\nDelete it for good?", d->names[0]);
        else snprintf(msg, sizeof msg, "%zu objects cannot go to the trash (they are in the trash already, or their drive has none).\nDelete them for good?", d->n);
        const char *buttons[] = { "Delete", "Cancel" };
        last_dialog = ask("Delete for good", msg, buttons, 2, 1, forgood_confirmed, d);
    }
    return true;
}

static void on_job_event(const cv *st)
{
    int64_t id, done = 0, total = 0;
    if (!st || !cv_int(st, "job", &id)) return;
    struct jobtrack *t = jobs, **pp = &jobs;
    for (; t && t->job != id; pp = &t->next, t = t->next) {}
    if (!t) return;   /* not one of ours */
    cv_int(st, "items_done", &done);
    cv_int(st, "items_total", &total);
    const char *state = cv_text(st, "state"), *cur = cv_text(st, "current"), *err = cv_text(st, "error");
    bool move = !strcmp(t->kind, "move");
    char m[400];
    if (!state) return;
    if (!strcmp(t->kind, "trash") || !strcmp(t->kind, "delete")) {
        if (deletion_event(t, st, state, done, total)) { *pp = t->next; free(t); }
        return;
    }
    if (!strcmp(state, "done") || !strcmp(state, "failed") || !strcmp(state, "cancelled")) {
        /* The service keeps where the objects were dropped as the job
         * ends: the windows on the destination read the positions again
         * (the new icons may have appeared, in the next free spot, before
         * their positions were kept). */
        for (struct folder *o = folders; o; o = o->next)
            if (!o->closing && t->dest[0] && !strcmp(o->path, t->dest) && !is_tree(o)) svc_layout(o->path, got_layout, as_arg(o));
        const cv *miss = cv_get(st, "missing"), *skip = cv_get(st, "skipped");
        size_t nm = miss && miss->k == CV_ARRAY ? miss->n : 0, ns = skip && skip->k == CV_ARRAY ? skip->n : 0;
        if (!strcmp(state, "done"))
            snprintf(m, sizeof m, "%s %lld object%s%s%s", move ? "Moved" : "Copied", (long long)(done - (int64_t)ns),
                     done - (int64_t)ns == 1 ? "" : "s", ns ? "; some skipped" : "", nm ? "; some were gone already" : "");
        else if (!strcmp(state, "failed")) snprintf(m, sizeof m, "%s failed: %s", move ? "Move" : "Copy", err ? err : "unknown error");
        else snprintf(m, sizeof m, "%s cancelled after %lld", move ? "Move" : "Copy", (long long)done);
        set_status(t->folder, m);
        raise_in(folder_by_id(t->folder), !strcmp(state, "failed") ? "job.failed" : "job.done", NULL, "");
        if (!strcmp(state, "failed")) messages_post(m, t->dest, 2);
        *pp = t->next;
        free(t);
        return;
    }
    if (!strcmp(state, "needs-decision") && !t->asking) {
        const char *clash = cv_text(st, "conflict"), *to = cv_text(st, "to");
        char msg[1200];
        snprintf(msg, sizeof msg, "An object named \"%s\" is already in %s.\nRename the one being %s, or skip it?",
                 clash ? clash : "?", to ? to : "the folder", move ? "moved" : "copied");
        const char *buttons[] = { "Rename", "Skip", "Rename all", "Skip all", "Cancel" };
        struct decide_ctx *d = malloc(sizeof *d);
        d->job = id;
        t->asking = true;
        last_dialog = ask(move ? "Move" : "Copy", msg, buttons, 5, 0, decided, d);
        return;
    }
    snprintf(m, sizeof m, "%s %lld of %lld%s%s", move ? "Moving" : "Copying", (long long)done, (long long)total,
             cur && *cur ? ": " : "", cur ? cur : "");
    set_status(t->folder, m);
}

static void src_item_pos(void *ctx, size_t i, double *x, double *y) { layout_pos(((struct folder *)ctx)->lay, i, x, y); }

static size_t src_items_in(void *ctx, ui_rect r, size_t *out, size_t max)
{
    return layout_items_in(((struct folder *)ctx)->lay, r.x, r.y, r.w, r.h, out, max);
}

static void src_content_size(void *ctx, double *w, double *h) { layout_size(((struct folder *)ctx)->lay, w, h); }
static double src_item_h(void *ctx, size_t i) { return layout_item_h(((struct folder *)ctx)->lay, i); }

static const ui_container_src folder_src = { src_count, src_item, src_visible, src_activate, src_menu, src_rename, src_expand,
                                             src_drag, src_item_pos, src_items_in, src_content_size, src_item_h };

/* ------------------------------------------------------------------ window */

/* ---- the search field (v0.3-design 6): a row under the title bar, shown
 * with Ctrl+F; what is typed filters the window's own listing (wpsd's:
 * names containing it, any case); Esc or x: all again, the row gone. */

#define FIND_H 30

static void layout(struct folder *f);

static ui_rect find_row(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    return (ui_rect){ c.x, c.y, c.w, FIND_H };
}
static ui_rect find_entry_rect(struct folder *f) { ui_rect r = find_row(f); return (ui_rect){ r.x + 68, r.y + 4, r.w - 68 - 44, FIND_H - 9 }; }
static ui_rect find_x_rect(struct folder *f) { ui_rect r = find_row(f); return (ui_rect){ r.x + r.w - 36, r.y + 4, 28, FIND_H - 9 }; }

static void find_draw(struct folder *f, cairo_t *cr)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect r = find_row(f);
    ui_fill(cr, r, k->face);
    ui_fill(cr, (ui_rect){ r.x, r.y + r.h - 1, r.w, 1 }, k->shadow);
    /* a magnifier, and "Find:" */
    ui_set_color(cr, k->text);
    cairo_set_line_width(cr, 2);
    cairo_arc(cr, r.x + 14, r.y + 13, 5, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 3);
    cairo_move_to(cr, r.x + 18, r.y + 17); cairo_line_to(cr, r.x + 23, r.y + 22);
    cairo_stroke(cr);
    ui_text_draw(cr, UI_FONT_REGULAR, r.x + 28, r.y + (FIND_H + ui_text_ascent()) / 2 - 1, "Find:", 40);
    ui_rect er = find_entry_rect(f);
    ui_entry_draw(f->find_entry, cr, er, f->find_focus);
    if (!*ui_entry_text(f->find_entry)) {
        ui_set_color(cr, k->disabled_text);
        ui_text_draw(cr, UI_FONT_REGULAR, er.x + 5, er.y + (er.h + ui_text_ascent()) / 2 - 1,
                     "Type to show only the objects whose names contain it", er.w - 10);
    }
    ui_rect x = find_x_rect(f);
    ui_fill(cr, x, k->face);
    ui_bevel(cr, x, !f->find_x_down);
    ui_set_color(cr, k->text);
    cairo_set_line_width(cr, 2);
    double cx = x.x + x.w / 2 + f->find_x_down, cy = x.y + x.h / 2 + f->find_x_down;
    cairo_move_to(cr, cx - 4, cy - 4); cairo_line_to(cr, cx + 4, cy + 4);
    cairo_move_to(cr, cx + 4, cy - 4); cairo_line_to(cr, cx - 4, cy + 4);
    cairo_stroke(cr);
}

static void find_focus(struct folder *f, bool on)
{
    f->find_focus = on;
    ui_container_set_focused(f->cont, !on);
    ui_text_input_focus(f->win, on ? f->find_entry : NULL, find_entry_rect(f));
    ui_surface_damage(f->win, find_row(f));
}

/* The filter as typed: the window's own listing, read again. */
static void find_apply(struct folder *f)
{
    const char *t = ui_entry_text(f->find_entry);
    if (!strcmp(t, f->filter)) return;
    snprintf(f->filter, sizeof f->filter, "%s", t);
    /* icons: the matches flow from the top left; all again, their places */
    layout_reset(f->lay, cols_now(f));
    f->arranged_by_name = false;   /* (arranged by name again once read; then stored places, if not filtered) */
    reload(f);
    damage_status(f);
}

static void find_show(struct folder *f)
{
    if (!f->find_entry) f->find_entry = ui_entry_new("");
    if (!f->find_on) {
        f->find_on = true;
        layout(f);
        ui_surface_damage_all(f->win);
    }
    ui_entry_select_all(f->find_entry);
    find_focus(f, true);
}

static void find_hide(struct folder *f)
{
    if (!f->find_on) return;
    f->find_on = false;
    ui_entry_set_text(f->find_entry, "");
    find_focus(f, false);
    find_apply(f);
    layout(f);
    ui_surface_damage_all(f->win);
}

static bool find_key(struct folder *f, const ui_key_event *e)
{
    if (e->sym == XKB_KEY_Escape) { find_hide(f); return true; }
    if (e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter || e->sym == XKB_KEY_Down || e->sym == XKB_KEY_Tab) {
        find_focus(f, false);   /* to the objects (the filter stays) */
        return true;
    }
    ui_entry_result r = ui_entry_key(f->find_entry, e);
    if (r == UI_ENTRY_CHANGED) find_apply(f);   /* as typed */
    if (r != UI_ENTRY_NONE) ui_surface_damage(f->win, find_row(f));
    return true;
}

/* A press in the row: the entry (focused), x (all again); elsewhere the
 * entry lets go of the keyboard. */
static bool find_pointer(struct folder *f, const ui_pointer_event *e)
{
    ui_rect er = find_entry_rect(f), x = find_x_rect(f), row = find_row(f);
    if (e->kind == UI_RELEASE && f->find_x_down) {
        f->find_x_down = false;
        ui_surface_damage(f->win, row);
        if (ui_rect_has(x, e->x, e->y)) find_hide(f);
        return true;
    }
    if (f->find_focus && (e->kind == UI_MOTION || e->kind == UI_RELEASE)) {
        if (ui_entry_pointer(f->find_entry, e, er) != UI_ENTRY_NONE) ui_surface_damage(f->win, row);
    }
    if (e->kind != UI_PRESS) return false;
    if (ui_rect_has(x, e->x, e->y)) { f->find_x_down = true; ui_surface_damage(f->win, row); return true; }
    if (ui_rect_has(er, e->x, e->y)) {
        if (!f->find_focus) find_focus(f, true);
        ui_entry_pointer(f->find_entry, e, er);
        ui_surface_damage(f->win, row);
        return true;
    }
    if (ui_rect_has(row, e->x, e->y)) return true;
    if (f->find_focus) find_focus(f, false);
    return false;
}

static void win_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    struct folder *f = ui_surface_data(s);
    {
        static bool first_empty, first_items;
        if (!first_empty) { first_empty = true; TIMING(f, "first frame (%zu objects)", f->m.total); }
        if (!first_items && f->m.total) { first_items = true; TIMING(f, "first frame with objects"); }
    }
    const ui_scheme *k = &ui_scheme_now;
    char t[400];
    snprintf(t, sizeof t, "%s - %s", f->title, view_name(ui_container_view(f->cont)));
    ui_frame_draw(s, cr, clip, t);
    ui_container_draw(f->cont, cr, clip);
    if (f->desktop) return;   /* no status line */
    if (f->find_on) find_draw(f, cr);
    ui_rect c = ui_frame_client(s);
    ui_rect st = { c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    if (!ui_rect_meets(clip, st)) return;
    ui_fill(cr, st, k->face);
    ui_bevel(cr, (ui_rect){ st.x + 1, st.y + 1, st.w - 2, st.h - 2 }, false);
    char msg[512];
    size_t nsel = ui_container_selected_count(f->cont);
    if (f->error[0]) snprintf(msg, sizeof msg, "%s", f->error);
    else if (is_tree(f)) snprintf(msg, sizeof msg, "%zu folders shown", f->ntvis);
    else if (f->filter[0]) snprintf(msg, sizeof msg, "%zu objects match \"%s\"%s   (Esc: all again)", f->m.total, f->filter,
                                    f->m.complete ? "" : " (reading...)");
    else if (nsel) snprintf(msg, sizeof msg, "%zu objects, %zu selected%s", f->m.total, nsel, f->m.complete ? "" : " (reading...)");
    else snprintf(msg, sizeof msg, "%zu objects%s", f->m.total, f->m.complete ? "" : " (reading...)");
    ui_set_color(cr, k->text);
    cairo_save(cr);   /* inside its bevel, whatever the font */
    cairo_rectangle(cr, st.x + 2, st.y + 2, st.w - 4, st.h - 4);
    cairo_clip(cr);
    ui_text_draw(cr, UI_FONT_REGULAR, st.x + 6, st.y + (STATUS_H + ui_text_ascent()) / 2 - 1, msg, st.w - 12);
    cairo_restore(cr);
}

static void layout(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    if (!f->desktop) c.h -= STATUS_H;
    if (f->find_on) { c.y += FIND_H; c.h -= FIND_H; }
    ui_container_layout(f->cont, c);
}

static void win_resized(ui_surface *s, double w, double h)
{
    (void)w; (void)h;
    struct folder *f = ui_surface_data(s);
    if (f->desktop) wallpaper_set_size(w, h, ui_surface_scale(s));
    layout(f);
    ui_surface_damage_all(s);
}

static void win_pointer(ui_surface *s, const ui_pointer_event *e)
{
    struct folder *f = ui_surface_data(s);
    /* The window-list button (button 3 by default, as on OS/2) on the desktop. */
    if (f->desktop && e->kind == UI_PRESS && ui_mouse_is(UI_ACT_WINDOW_LIST, e)) { winlist_open(false); return; }
    size_t before = ui_container_selected_count(f->cont);
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    bool used = false;
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) used = ui_frame_pointer(s, e);
    if (!used && f->find_on && find_pointer(f, e)) return;
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) ui_container_pointer(f->cont, e);
    if (ui_container_selected_count(f->cont) != before) damage_status(f);
}

static void win_close(ui_surface *s);

static void set_view(struct folder *f, ui_view v)
{
    if (v == UI_VIEW_TREE) tree_start(f);
    ui_container_set_view(f->cont, v);
    if (v == UI_VIEW_TREE) tree_rebuild(f, f->troot);
    update_title(f);
    damage_status(f);
}

static void win_key(ui_surface *s, const ui_key_event *e)
{
    struct folder *f = ui_surface_data(s);
    size_t before = ui_container_selected_count(f->cont);
    if ((e->mods & UI_MOD_CTRL) && (e->sym == XKB_KEY_w || e->sym == XKB_KEY_W)) { win_close(s); return; }
    /* Find in this folder and its subfolders (G4): Ctrl+Shift+F; Ctrl+F:
     * the search field, this folder only (v0.3-design 6) */
    if ((e->mods & UI_MOD_CTRL) && (e->sym == XKB_KEY_f || e->sym == XKB_KEY_F)) {
        if ((e->mods & UI_MOD_SHIFT) || f->desktop || is_tree(f)) findwin_open(f->path, NULL);
        else if (f->find_on) find_hide(f);   /* (again: the row goes, as Escape) */
        else find_show(f);
        return;
    }
    if (f->find_on && f->find_focus && find_key(f, e)) return;
    /* Escape with the row up but the keyboard on the objects (a click came
     * back into the window): the row goes too, unless a title is edited */
    if (f->find_on && e->sym == XKB_KEY_Escape && !ui_container_renaming(f->cont)) { find_hide(f); return; }
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_1) { set_view(f, UI_VIEW_ICON); return; }
    /* In the order the View menu lists them (OS/2's): icon, tree, details. */
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_2) { set_view(f, UI_VIEW_TREE); return; }
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_3) { set_view(f, UI_VIEW_DETAILS); return; }
    if (e->sym == XKB_KEY_Delete && !ui_container_renaming(f->cont)) {
        /* the selection, or the item with the cursor (Shift: for good) */
        size_t total = is_tree(f) ? f->ntvis : f->m.total, at = SIZE_MAX;
        for (size_t k = 0; k < total && at == SIZE_MAX; k++)
            if (ui_container_is_selected(f->cont, k)) at = k;
        if (at == SIZE_MAX) at = ui_container_cursor(f->cont);
        if (at != SIZE_MAX && at < total) delete_request(f, at, (e->mods & UI_MOD_SHIFT) != 0 || is_trash_dir(f->path));
        return;
    }
    ui_container_key(f->cont, e);
    if (ui_container_selected_count(f->cont) != before) damage_status(f);
}

static void win_focus(ui_surface *s, bool on)
{
    (void)on;
    ui_frame_damage(s);
}

static void free_folder(struct folder *f)
{
    for (struct folder **pp = &folders; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    if (f->sub) svc_unsubscribe(f->sub);
    if (f->bg_sub) svc_unsubscribe(f->bg_sub);
    if (f->bg_img) cairo_surface_destroy(f->bg_img);
    if (f->find_entry) ui_entry_free(f->find_entry);
    changes_clear(f);
    free(f->changes);
    tree_free(f);
    layout_free(f->lay);
    for (size_t j = 0; j < f->nspot_new; j++) free(f->spot_new[j]);
    free(f->spot_new);
    model_free(&f->m);
    if (f->sh) { model_free(f->sh); free(f->sh); }
    ui_container_free(f->cont);
    ui_surface_destroy(f->win);
    free(f);
}

static void report(struct folder *f);

/* Closing happens on the next loop turn: the request comes from inside
 * this window's own event handling (its close button), which must not
 * find the window freed under it. */
static void close_later(void *arg)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    raise_in(f, "window.close", NULL, "folder");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", f->path);
    free_folder(f);
    notify_in_use(path);   /* its icon elsewhere is no longer open */
}

static void win_close(ui_surface *s)
{
    struct folder *f = ui_surface_data(s);
    if (f->closing || f->desktop) return;   /* the desktop stays */
    f->closing = true;
    if (f->bench_secs > 0) report(f);
    ui_app_post(app, close_later, as_arg(f));
}

/* Benchmark: a fling, then jumps as if the scroll bar thumb were dragged. */
/* WPS_SHELL_SELFTEST=menus: open, re-open and toggle menus in quick
 * succession (the orders that broke Wayland's popup rules), then quit. */
static void selftest_chosen(void *arg, int64_t id) { (void)arg; (void)id; }
static void selftest_record(const wps_event *e, void *ctx)
{
    char *seen = ctx;
    if (strlen(seen) + strlen(e->name) + 2 < 1024) { strcat(seen, " "); strcat(seen, e->name); }
}

static void selftest(ui_surface *s, double now)
{
    static double t0;
    static int step;
    if (!t0) t0 = now;
    double t = now - t0;
    ui_menu_item items[] = { { 1, "One", true, false, NULL, 0 }, { 2, "Two", true, false, NULL, 0 } };
    if (step == 0 && t > 200) { ui_frame_system_menu(s, 0); ui_frame_system_menu(s, 0); step++; }
    else if (step == 1 && t > 500) { ui_frame_system_menu(s, 0); step++; }           /* after a dismissal */
    else if (step == 2 && t > 520) { ui_frame_system_menu(s, 0); step++; }           /* click again: closes */
    else if (step == 3 && t > 800) { ui_menu_popup(s, items, 2, 100, 120, 0, selftest_chosen, NULL); step++; }
    else if (step == 4 && t > 820) { ui_menu_popup(s, items, 2, 300, 200, 0, selftest_chosen, NULL); ui_frame_system_menu(s, 0); step++; }
    else if (step == 5 && t > 1000) {
        /* The menu opened last is open; a press on the window must close it. */
        ui_menu_item one[] = { { 1, "One", true, false, NULL, 0 } };
        ui_menu_popup(s, one, 1, 50, 50, 0, selftest_chosen, NULL);
        bool open_before = ui_menu_is_open();
        extern bool ui_menu_press_outside(ui_surface *);   /* what a press on the window does */
        bool used = ui_menu_press_outside(s);
        fprintf(stderr, "selftest: press outside: open before %d, used %d, open after %d\n", open_before, used, ui_menu_is_open());
        step++;
    }
    else if (step == 6 && t > 1500) { fprintf(stderr, "selftest: done\n"); ui_app_quit(app); step++; }
}

/* WPS_SHELL_SELFTEST=live: files created and deleted in the folder appear
 * and disappear. =close: closing one of two windows (as its close button
 * does) leaves the other and the process running. */
static void selftest_live(struct folder *f, double now)
{
    static double t0;
    static int step;
    static size_t base;
    char p[PATH_MAX + 32];
    if (!t0) { if (!f->m.complete) return; t0 = now; base = f->m.total; }
    double t = now - t0;
    if (step == 0) {
        for (int i = 0; i < 3; i++) {
            snprintf(p, sizeof p, "%s/selftest-%d.txt", f->path, i);
            FILE *x = fopen(p, "w");
            if (x) fclose(x);
        }
        step++;
    } else if (step == 1 && t > 1500) {
        fprintf(stderr, "selftest: after creating 3 files: %zu objects (was %zu)\n", f->m.total, base);
        for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/selftest-%d.txt", f->path, i); unlink(p); }
        step++;
    } else if (step == 2 && t > 3000) {
        fprintf(stderr, "selftest: after deleting them: %zu objects\n", f->m.total);
        ui_app_quit(app);
        step++;
    }
}

static void selftest_close(ui_surface *s, double now)
{
    static double t0;
    static int step;
    if (!t0) t0 = now;
    if (step == 0 && now - t0 > 300) {
        int n = 0;
        for (struct folder *x = folders; x; x = x->next) n++;
        fprintf(stderr, "selftest: %d windows; closing one\n", n);
        win_close(s);
        step++;
    }
}

static void selftest_after_close(void *arg)
{
    (void)arg;
    int n = 0;
    for (struct folder *x = folders; x; x = x->next) n++;
    fprintf(stderr, "selftest: %d window(s) left, process running\n", n);
    ui_app_quit(app);
}

/* WPS_SHELL_SELFTEST=fields: what a notebook's fields set, printed. */
static ui_notebook *selftest_nb;

static void selftest_file_chosen(long owner, const char *key, const char *path)
{
    (void)owner;
    const char *base = strrchr(path, '/');
    fprintf(stderr, "selftest: fields: picked %s\n", base ? base + 1 : path);
    if (selftest_nb) ui_notebook_set_value(selftest_nb, key, path);
}

static void selftest_field_set(void *ctx, const char *key, ui_field_kind kind, const ui_value *v)
{
    (void)ctx;
    if (kind == UI_FIELD_FILE && !v->text) {
        fprintf(stderr, "selftest: fields: Browse...\n");
        filepick_open("Fields", ui_notebook_field_hint(selftest_nb, key), "/", selftest_file_chosen, 0, key);
        return;
    }
    const char *base = v->text ? strrchr(v->text, '/') : NULL;
    fprintf(stderr, "selftest: fields: set %s = %s\n", key, base ? base + 1 : v->text ? v->text : "");
}

/* WPS_SHELL_SELFTEST=layout: the icons' cells, checked. */
static size_t selftest_index(struct folder *f, const char *name)
{
    for (size_t i = 0; i < f->m.total; i++)
        if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), name)) return i;
    return SIZE_MAX;
}

static void selftest_cells(struct folder *f, const char *when)
{
    size_t n = f->m.total, over = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            double xi, yi, xj, yj, hi = layout_item_h(f->lay, i), hj = layout_item_h(f->lay, j);
            layout_pos(f->lay, i, &xi, &yi);
            layout_pos(f->lay, j, &xj, &yj);
            if (xi < xj + UI_CELL_W && xj < xi + UI_CELL_W && yi < yj + hj && yj < yi + hi) over++;
        }
    /* the second row: below the first row's tallest title */
    double top1 = 1e9, tall = 0, row2 = 1e9;
    for (size_t i = 0; i < n; i++) { double x, y; layout_pos(f->lay, i, &x, &y); if (y < top1) top1 = y; }
    for (size_t i = 0; i < n; i++) {
        double x, y;
        layout_pos(f->lay, i, &x, &y);
        if (y == top1 && layout_item_h(f->lay, i) > tall) tall = layout_item_h(f->lay, i);
        if (y > top1 && y < row2) row2 = y;
    }
    fprintf(stderr, "selftest: layout: %s: %zu overlaps; tallest of row 1 %.0f, row 2 at %.0f\n", when, over, tall, row2 - top1);
}

static void selftest_lock_page(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *v = "missing";
    for (size_t i = 0; res && res->k == CV_ARRAY && i < res->n; i++) {
        const cv *fs = cv_get(&res->items[i], "fields");
        for (size_t k = 0; fs && fs->k == CV_ARRAY && k < fs->n; k++) {
            const char *key = cv_text(&fs->items[k], "key");
            const cv *val = cv_get(&fs->items[k], "value");
            if (key && !strcmp(key, "lock_pos") && val && val->k == CV_BOOL)
                v = !strcmp(cv_text(&res->items[i], "title") ? cv_text(&res->items[i], "title") : "", "General") ? (val->b ? "on" : "off") : "elsewhere";
        }
    }
    fprintf(stderr, "selftest: layout: b.txt's General page: Lock position %s\n", v);
}

static void win_frame(ui_surface *s, double now)
{
    struct folder *f = ui_surface_data(s);
    const char *st = getenv("WPS_SHELL_SELFTEST");
    if (st && !strcmp(st, "layout")) {
        /* OS/2's layout and Lock position (0.3 step 6). <D> has a.txt
         * (a long title), b.txt .. h.txt. */
        static double t0, bx, by;
        static int step;
        if (!t0) { if (!f->m.complete || !f->arranged_by_name) return; t0 = now; }
        double t = now - t0;
        size_t b = selftest_index(f, "b.txt"), c = selftest_index(f, "c.txt");
        if (step == 0 && t > 1000) {
            size_t a = selftest_index(f, "a-rather-long-title-for-a-file-that-takes-four-lines.txt");
            fprintf(stderr, "selftest: layout: the long title's cell %.0f (a cell %d)\n", layout_item_h(f->lay, a), UI_CELL_H);
            selftest_cells(f, "arranged");
            layout_pos(f->lay, b, &bx, &by);
            lock_items(f, b);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            char bp[PATH_MAX];
            snprintf(bp, sizeof bp, "%s/b.txt", f->path);
            svc_settings(bp, selftest_lock_page, NULL);
            /* b and c dragged within the folder */
            struct dragging d = { 0 };
            char p0[PATH_MAX], p1[PATH_MAX], *ps[2] = { p0, p1 };
            snprintf(p0, sizeof p0, "%s/b.txt", f->path);
            snprintf(p1, sizeof p1, "%s/c.txt", f->path);
            double lx, ly, dx[2], dy[2];
            ui_container_to_list(f->cont, 300, 300, &lx, &ly);
            dx[0] = 400 - lx; dy[0] = 300 - ly;
            dx[1] = 500 - lx; dy[1] = 300 - ly;
            d.paths = ps; d.n = 2; d.dx = dx; d.dy = dy;
            snprintf(d.src_folder, sizeof d.src_folder, "%s", f->path);
            struct dragging *was = cur_drag;
            cur_drag = &d;
            reposition(f, 300, 300);
            cur_drag = was;
            double x, y, cx, cy;
            layout_pos(f->lay, b, &x, &y);
            layout_pos(f->lay, c, &cx, &cy);
            fprintf(stderr, "selftest: layout: dragged: the locked one stayed %d, the other moved %d (%s)\n",
                    x == bx && y == by, cx == 500 && cy == 300, f->error);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            arrange(f);
            double x, y;
            layout_pos(f->lay, b, &x, &y);
            fprintf(stderr, "selftest: layout: Arrange: the locked one stayed %d\n", x == bx && y == by);
            selftest_cells(f, "arranged around it");
            step++;
            t0 = now;
        } else if (step == 3 && t > 2000) {
            double x, y;
            layout_pos(f->lay, b, &x, &y);
            fprintf(stderr, "selftest: layout: read back: locked %d, in place %d\n", layout_is_locked(f->lay, "b.txt"), x == bx && y == by);
            lock_items(f, b);
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500) {
            fprintf(stderr, "selftest: layout: unlocked: %d\n", !layout_is_locked(f->lay, "b.txt"));
            cv v = { .k = CV_INT, .i = 1 };
            svc_settings_set(f->path, "icon_titles", &v, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 5 && t > 2000) {
            size_t a = selftest_index(f, "a-rather-long-title-for-a-file-that-takes-four-lines.txt");
            fprintf(stderr, "selftest: layout: two lines: whole %d, the long title's cell %.0f\n", f->whole_titles, layout_item_h(f->lay, a));
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "search")) {
        /* The search field (0.3 step 5): Ctrl+F, "rep" typed, Escape; then
         * Show hidden files off and on. <D> has report-2025.ods,
         * report-2026.ods, reports/, notes.txt, .hidden. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char names[300] = "";
        for (size_t i = 0; i < f->m.total && strlen(names) < 250; i++)
            snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? " " : "", model_name(&f->m, i) ? model_name(&f->m, i) : "?");
        if (step == 0) {
            fprintf(stderr, "selftest: search: at first %zu: %s\n", f->m.total, names);
            ui_key_event k = { .sym = XKB_KEY_f, .mods = UI_MOD_CTRL };
            win_key(s, &k);
            fprintf(stderr, "selftest: search: Ctrl+F: field shown %d, focused %d\n", f->find_on, f->find_focus);
            const char *typed = "rep";
            for (const char *c = typed; *c; c++) {   /* as typed (the input method's commit path) */
                ui_key_event kc = { .sym = (uint32_t)*c, .utf8 = { *c, 0 } };
                win_key(s, &kc);
            }
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500 && f->m.complete) {
            fprintf(stderr, "selftest: search: \"%s\": %zu: %s\n", f->filter, f->m.total, names);
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            win_key(s, &esc);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500 && f->m.complete) {
            fprintf(stderr, "selftest: search: Escape: field shown %d, %zu objects\n", f->find_on, f->m.total);
            ui_key_event cf = { .sym = XKB_KEY_f, .mods = UI_MOD_CTRL };
            win_key(s, &cf);
            int shown = f->find_on;
            win_key(s, &cf);
            fprintf(stderr, "selftest: search: Ctrl+F twice: shown %d, then %d\n", shown, f->find_on);
            win_key(s, &cf);
            find_focus(f, false);   /* the keyboard on the objects (a click back into the window) */
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            win_key(s, &esc);
            fprintf(stderr, "selftest: search: Escape, the field not focused: shown %d\n", f->find_on);
            show_hidden = false;
            reload(f);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500 && f->m.complete) {
            fprintf(stderr, "selftest: search: hidden files off: %zu: %s\n", f->m.total, names);
            show_hidden = true;
            reload(f);
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500 && f->m.complete) {
            fprintf(stderr, "selftest: search: hidden files on: %zu\n", f->m.total);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "trashlive") && getenv("WPS_SELFTEST_TRASH")) {
        /* The Trash Can's window follows deletes and restores of several
         * objects at once (a.txt b.txt c.txt in <D>) */
        static double t0;
        static int step;
        static long dw = -1;
        char tp[PATH_MAX];
        snprintf(tp, sizeof tp, "%s/files", getenv("WPS_SELFTEST_TRASH"));
        struct folder *tw = NULL, *d = dw >= 0 ? folder_by_id(dw) : NULL;
        for (struct folder *x = folders; x; x = x->next)
            if (!x->closing && !strcmp(x->path, tp)) tw = x;
        if (f == tw) return;   /* (driven from <D>'s window) */
        if (!t0) { if (!f->m.complete) return; t0 = now; dw = f->id; d = f; }
        if (f != d) return;
        double t = now - t0;
        char rows[300] = "";
        for (size_t i = 0; tw && i < tw->m.total && strlen(rows) < 250; i++)
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s%s", i ? " " : "",
                     model_name(&tw->m, i) ? model_name(&tw->m, i) : "?");
        if (step == 0) {
            mkdir(tp, 0700);
            open_folder(tp, UI_VIEW_ICON);
            step++;
        } else if (step == 1 && t > 1500) {
            ui_container_select_none(d->cont);
            size_t at = SIZE_MAX;
            for (size_t i = 0; i < d->m.total; i++)
                if (model_name(&d->m, i) && strstr("a.txt b.txt c.txt", model_name(&d->m, i))) { ui_container_select(d->cont, i); at = i; }
            last_dialog = NULL;
            if (at != SIZE_MAX) delete_request(d, at, false);
            ui_key_event ret = { .sym = XKB_KEY_Return };
            if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            last_dialog = NULL;
            step++;
        } else if (step == 2 && t > 4000) {
            fprintf(stderr, "selftest: trashlive: after deleting three: the Trash Can shows %zu: %s\n", tw ? tw->m.total : 0, rows);
            if (tw && tw->m.total) {
                ui_container_select_none(tw->cont);
                for (size_t i = 0; i < tw->m.total; i++) ui_container_select(tw->cont, i);
                restore_items(tw, 0);
            }
            step++;
        } else if (step == 3 && t > 7000) {
            fprintf(stderr, "selftest: trashlive: after restoring them: the Trash Can shows %zu: %s\n", tw ? tw->m.total : 0, rows);
            DIR *dd = opendir(tp);
            int left = 0;
            for (struct dirent *e; dd && (e = readdir(dd));) left += e->d_name[0] != '.';
            if (dd) closedir(dd);
            fprintf(stderr, "selftest: trashlive: in the trash folder itself: %d\n", left);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "fields")) {
        /* A notebook's file and colour fields (0.3): a colour typed (a bad
         * one put back), Browse... into <D>/pics (pictures only), one
         * picked into the field; a dropped path. */
        static double t0;
        static int step;
        ui_notebook *nb = selftest_nb;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        if (step == 0) {
            static const char *const kind[] = { "image" };
            static ui_field_desc fd[2];
            fd[0] = (ui_field_desc){ "picture", "Image", UI_FIELD_FILE, { .text = "" }, kind, 1 };
            fd[1] = (ui_field_desc){ "colour", "Colour", UI_FIELD_COLOR, { .text = "#336699" }, NULL, 0 };
            static ui_page_desc pd[1];
            pd[0] = (ui_page_desc){ "Background", fd, 2 };
            selftest_nb = ui_notebook_open(app, "Fields", pd, 1, selftest_field_set, NULL, NULL);
            step++;
        } else if (step == 1 && t > 800) {
            ui_notebook_set_value(nb, "colour", "nonsense");
            ui_notebook_set_value(nb, "colour", "#C00");
            fprintf(stderr, "selftest: fields: hint \"%s\"\n", ui_notebook_field_hint(nb, "picture"));
            const char *shot = getenv("WPS_FIELDS_SHOT");   /* prefix: -notebook.png, -picker.png */
            char png[PATH_MAX];
            if (shot) { snprintf(png, sizeof png, "%s-notebook.png", shot); ui_surface_snapshot(ui_notebook_surface(nb), png); }
            /* Browse... (as its button does): the owner opens the picker */
            ui_value v = { .text = NULL };
            selftest_field_set(nb, "picture", UI_FIELD_FILE, &v);
            step++;
        } else if (step == 2 && t > 1500) {
            char pics[PATH_MAX + 8];
            snprintf(pics, sizeof pics, "%s/pics", f->path);
            filepick_go(pics);
            char names[200] = "";
            for (size_t i = 0; i < filepick_count(); i++)
                snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? " " : "", filepick_name(i));
            fprintf(stderr, "selftest: fields: the picker in pics shows: %s\n", names);
            step++;
        } else if (step == 3 && t > 2300) {
            const char *shot = getenv("WPS_FIELDS_SHOT");
            char png[PATH_MAX];
            if (shot) { snprintf(png, sizeof png, "%s-picker.png", shot); filepick_snapshot(png); }
            for (size_t i = 0; i < filepick_count(); i++)
                if (!strcmp(filepick_name(i), "sub")) filepick_activate(i);   /* into a folder */
            fprintf(stderr, "selftest: fields: into sub: %s\n", strrchr(filepick_folder(), '/') + 1);
            for (size_t i = 0; i < filepick_count(); i++)
                if (!strcmp(filepick_name(i), "b.png")) filepick_activate(i);   /* picked */
            fprintf(stderr, "selftest: fields: picker open after picking: %d\n", filepick_is_open());
            step++;
        } else if (step == 4 && t > 3000) {
            ui_notebook_close(nb);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "first") && f->desktop) {
        /* A new desktop's first start: its icons as they come, never more
         * than it ends with (an object listed twice for a moment) */
        static double t0;
        static long last = -1, most = 0;
        if (!t0) t0 = now;
        long n = (long)f->m.total;
        if (n != last) {
            char names[400] = "";
            for (size_t i = 0; i < f->m.total && strlen(names) < 300; i++) {
                struct row *rw = row_at(f, i);
                snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? ", " : "",
                         rw && rw->title ? rw->title : model_name(&f->m, i) ? model_name(&f->m, i) : "?");
            }
            fprintf(stderr, "selftest: first start: %.1f s: %ld icons: %s\n", (now - t0) / 1000, n, names);
            last = n;
            if (n > most) most = n;
        }
        if (now - t0 > 5000) {
            fprintf(stderr, "selftest: first start: at most %ld icons, %ld at the end\n", most, n);
            ui_app_quit(app);
            t0 = 1e18;
        }
        return;
    }
    if (st && !strcmp(st, "ctxmenu")) {
        /* A right click on the background, as GNOME delivers it (with a
         * real-looking serial); the menu must still be open afterwards. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        ui_rect c = ui_frame_client(s);
        ui_pointer_event e = { .x = c.x + c.w - 60, .y = c.y + c.h - 60, .button = ui_mouse_button(UI_ACT_POPUP), .clicks = 1, .serial = 4242 };
        if (step == 0) { e.kind = UI_PRESS; win_pointer(s, &e); e.kind = UI_RELEASE; win_pointer(s, &e); step++; }
        else if (step == 1 && now - t0 > 700) { fprintf(stderr, "selftest: right click on background: menu open %d\n", ui_menu_is_open()); step++; }
        else if (step == 2) {
            /* and on an item */
            e.x = c.x + 40; e.y = c.y + 30;
            e.kind = UI_PRESS; win_pointer(s, &e); e.kind = UI_RELEASE; win_pointer(s, &e); step++; t0 = now;
        }
        else if (step == 3 && now - t0 > 700) {
            fprintf(stderr, "selftest: right click on an item: menu open %d\n", ui_menu_is_open());
            /* Choose View > Details view as the background menu would. */
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = SIZE_MAX;
            ui_view before = ui_container_view(f->cont);
            menu_chosen(mc, WPS_MI_VIEW_DETAILS);
            fprintf(stderr, "selftest: View > Details view: %s -> %s\n", before == UI_VIEW_ICON ? "icon" : "details",
                    ui_container_view(f->cont) == UI_VIEW_ICON ? "icon" : "details");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "rename")) {
        /* F2 on old-name.txt, type "renamed", Enter: the file is renamed and
         * the window shows the new name. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        if (step == 0) {
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "old-name.txt")) i = k;
            if (i == SIZE_MAX) { fprintf(stderr, "selftest: old-name.txt not found\n"); ui_app_quit(app); return; }
            ui_container_set_cursor(f->cont, i);
            ui_key_event k = { .sym = XKB_KEY_F2 };
            win_key(s, &k);
            fprintf(stderr, "selftest: F2 starts a rename: %d\n", ui_container_renaming(f->cont));
            for (const char *p = "renamed"; *p; p++) {
                ui_key_event c = { .sym = (uint32_t)*p };
                c.utf8[0] = *p;
                win_key(s, &c);
            }
            ui_key_event ret = { .sym = XKB_KEY_Return };
            win_key(s, &ret);
            step++;
        } else if (step == 1 && now - t0 > 1500) {
            char p[PATH_MAX + 32];
            snprintf(p, sizeof p, "%s/renamed.txt", f->path);
            bool on_disk = access(p, F_OK) == 0;
            bool shown = false;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "renamed.txt")) shown = true;
            fprintf(stderr, "selftest: renamed.txt on disk %d, in the window %d, status '%s'\n", on_disk, shown, f->error);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "props")) {
        /* Properties on old-name.txt, all by keyboard: Tab to Title, type,
         * Enter (renames the file); Tab to Template, Space; Escape closes. */
        static double t0;
        static int step;
        static ui_surface *nbs;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        if (step == 0) {
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "old-name.txt")) i = k;
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = i;
            menu_chosen(mc, WPS_MI_PROPERTIES);
            step++;
        } else if (step == 1 && props_open && props_open->nb) {
            nbs = ui_notebook_surface(props_open->nb);
            fprintf(stderr, "selftest: notebook open: %s\n", props_open->title);
            ui_key_event tab = { .sym = XKB_KEY_Tab }, ret = { .sym = XKB_KEY_Return };
            ui_surface_send_key(nbs, &tab);
            for (const char *p = "titled.txt"; *p; p++) {
                ui_key_event c = { .sym = (uint32_t)*p };
                c.utf8[0] = *p;
                ui_surface_send_key(nbs, &c);
            }
            ui_surface_send_key(nbs, &ret);
            step++;
            t0 = now;
        } else if (step == 2 && now - t0 > 800) {
            ui_key_event tab = { .sym = XKB_KEY_Tab }, sp = { .sym = XKB_KEY_space, .utf8 = " " };
            ui_surface_send_key(nbs, &tab);
            ui_surface_send_key(nbs, &sp);
            step++;
            t0 = now;
        } else if (step == 3 && now - t0 > 800) {
            fprintf(stderr, "selftest: notebook now for %s\n", props_open ? props_open->target : "(closed)");
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            ui_surface_send_key(nbs, &esc);
            step++;
            t0 = now;
        } else if (step == 4 && now - t0 > 500) {
            fprintf(stderr, "selftest: after Escape the notebook is %s\n", props_open ? "open" : "closed");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "tree")) {
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        ui_key_event right = { .sym = XKB_KEY_Right }, left = { .sym = XKB_KEY_Left }, down = { .sym = XKB_KEY_Down };
        char rows[512] = "";
        for (size_t i = 0; i < f->ntvis && strlen(rows) < 400; i++) {
            ui_item it;
            src_item(f, i, &it);
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s%*s%s%s", i ? " | " : "", it.depth, "", it.name,
                     it.expandable ? (it.expanded ? "[-]" : "[+]") : "");
        }
        if (step == 0) { ui_key_event k2 = { .sym = XKB_KEY_2, .mods = UI_MOD_CTRL }; win_key(s, &k2); step++; }
        else if (step == 1 && t > 600) {
            fprintf(stderr, "selftest: tree: %s\n", rows);
            ui_container_set_cursor(f->cont, 0);
            win_key(s, &down);    /* to a */
            win_key(s, &right);   /* expand a */
            step++;
        } else if (step == 2 && t > 1200) {
            fprintf(stderr, "selftest: a expanded: %s\n", rows);
            for (size_t i = 0; i < f->ntvis; i++)
                if (!strcmp(f->tvis[i]->name, "b")) ui_container_set_cursor(f->cont, i);
            win_key(s, &right);   /* expand b: it has no subfolders */
            char p[PATH_MAX + 16];
            snprintf(p, sizeof p, "%s/a/a3", f->path);
            mkdir(p, 0755);        /* a new subfolder appears in the expanded a */
            step++;
        } else if (step == 3 && t > 2400) {
            fprintf(stderr, "selftest: b read, a3 made: %s\n", rows);
            for (size_t i = 0; i < f->ntvis; i++)
                if (!strcmp(f->tvis[i]->name, "a")) ui_container_set_cursor(f->cont, i);
            win_key(s, &left);     /* collapse a */
            step++;
        } else if (step == 4 && t > 3000) {
            fprintf(stderr, "selftest: a collapsed: %s\n", rows);
            step++;
        } else if (step == 5 && t > 3300) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;   /* a change still being drawn: next frame */
            fprintf(stderr, "selftest: tree pixels against a fresh redraw: %zu differ\n", d);
            for (size_t i = 0; i < f->ntvis; i++)
                if (!strcmp(f->tvis[i]->name, "a")) ui_container_set_cursor(f->cont, i);
            win_key(s, &right);    /* expand a again */
            step++;
        } else if (step == 6 && t > 4000) {
            /* Delete with a selection in two folders: a/a1 and a/a2 in a, b
             * in the folder itself */
            ui_container_select_none(f->cont);
            size_t at = SIZE_MAX;
            for (size_t i = 0; i < f->ntvis; i++) {
                const char *nm = f->tvis[i]->name;
                if (!strcmp(nm, "a1") || !strcmp(nm, "b") || !strcmp(nm, "a2")) { ui_container_select(f->cont, i); at = i; }
            }
            last_dialog = NULL;
            if (at != SIZE_MAX) delete_request(f, at, false);
            fprintf(stderr, "selftest: tree Delete asked: %s\n", last_dialog ? "yes" : "no");
            ui_key_event ret = { .sym = XKB_KEY_Return };
            if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            last_dialog = NULL;
            step++;
        } else if (step == 7 && t > 6000) {
            char p1[PATH_MAX + 8], p2[PATH_MAX + 8], p3[PATH_MAX + 8], p4[PATH_MAX + 8];
            snprintf(p1, sizeof p1, "%s/a/a1", f->path);
            snprintf(p2, sizeof p2, "%s/b", f->path);
            snprintf(p3, sizeof p3, "%s/a/a2", f->path);
            snprintf(p4, sizeof p4, "%s/a", f->path);
            fprintf(stderr, "selftest: after the tree's Delete: a/a1 %s, b %s, a/a2 %s, a %s\n", access(p1, F_OK) ? "gone" : "there",
                    access(p2, F_OK) ? "gone" : "there", access(p3, F_OK) ? "gone" : "there", access(p4, F_OK) ? "gone" : "there");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "dnd") && f == folders) {   /* the last window opened: <D>/sub */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char D[PATH_MAX];
        snprintf(D, sizeof D, "%s", f->path);
        char *sl = strrchr(D, '/');
        *sl = 0;
        char data[PATH_MAX * 2];
        if (step == 0) {
            /* What a drag from the first window offers. */
            struct folder *src = f->next;
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < src->m.total; k++)
                if (model_name(&src->m, k) && !strcmp(model_name(&src->m, k), "with space.txt")) i = k;
            src_drag(src, i, 0);
            if (cur_drag)   /* first line of each (the test prints, it does not change them) */
                fprintf(stderr, "selftest: drag data: wps \"%.*s\" uris \"%.*s\"\n", (int)strcspn(cur_drag->wps, "\n"),
                        cur_drag->wps, (int)strcspn(cur_drag->uris, "\r"), cur_drag->uris);
            /* A folder dropped into itself is refused. */
            struct dragging fake = { 0 };
            char *paths[1] = { f->path };
            fake.paths = paths;
            fake.n = 1;
            struct dragging *was = cur_drag;
            cur_drag = &fake;
            const char *mime, *mimes[] = { MIME_WPS };
            uint32_t act = win_drag_motion(s, 20, 60, mimes, 1, UI_DND_COPY | UI_DND_MOVE, 0, &mime);
            cur_drag = was;
            fprintf(stderr, "selftest: sub dropped into itself: action %u (0 = refused)\n", act);
            snprintf(data, sizeof data, "%s/a.txt\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_MOVE, 0, 0);                      /* move */
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);            /* copy */
            snprintf(data, sizeof data, "%s/sub2\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL | UI_MOD_SHIFT);   /* shadow */
            data[0] = 0;
            char *u = NULL;
            size_t ul = 0, uc = 0;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/with space.txt", D);
            uri_append(&u, &ul, &uc, p);
            win_drop(s, 20, 60, MIME_URIS, u, ul, UI_DND_MOVE, 0, 0);                                 /* from another app */
            free(u);
            step++;
        } else if (step == 1 && t > 1500) {
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);            /* b.txt is there now */
            step++;
        } else if (step == 2 && last_dialog) {
            fprintf(stderr, "selftest: a name clash asks: dialog open\n");
            ui_key_event r = { .sym = XKB_KEY_r, .utf8 = "r" };
            ui_surface_send_key(ui_dialog_surface(last_dialog), &r);   /* Rename */
            last_dialog = NULL;
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500) {
            fprintf(stderr, "selftest: status: %s\n", f->error);
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);            /* and again */
            step++;
            t0 = now;
        } else if (step == 4 && last_dialog) {
            ui_key_event r = { .sym = XKB_KEY_r, .utf8 = "r" };
            ui_surface_send_key(ui_dialog_surface(last_dialog), &r);
            last_dialog = NULL;
            step++;
            t0 = now;
        } else if (step == 5 && (last_dialog || t > 2000)) {
            fprintf(stderr, "selftest: a second Rename asked again: %s\n", last_dialog ? "yes" : "no");
            /* What the window shows, with no other change to prompt a refresh. */
            char shown[1000] = "";
            for (size_t i = 0; i < f->m.total; i++) {
                struct page *pg = f->m.pages[i / PAGE];
                struct row *r = &pg->rows[i % PAGE];
                snprintf(shown + strlen(shown), sizeof shown - strlen(shown), " %s%s", r->name,
                         r->shadow ? (r->kind == UI_KIND_FOLDER ? "(shadow of a folder)" : "(shadow)") : "");
            }
            fprintf(stderr, "selftest: the window shows:%s\n", shown);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "dnd")) return;
    if (st && !strcmp(st, "place")) {
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char line[600] = "";
        for (size_t i = 0; i < f->m.total; i++) {
            double x, y;
            layout_pos(f->lay, i, &x, &y);
            snprintf(line + strlen(line), sizeof line - strlen(line), " %s@%.0f,%.0f", model_name(&f->m, i), x, y);
        }
        if (step == 0 && t > 500) {
            fprintf(stderr, "selftest: opened:%s\n", line);
            size_t bi = SIZE_MAX;
            for (size_t i = 0; i < f->m.total; i++) if (!strcmp(model_name(&f->m, i), "b.txt")) bi = i;
            /* Press the drag button on b's icon, as a real drag begins. */
            double bx, by;
            layout_pos(f->lay, bi, &bx, &by);
            ui_rect c = ui_frame_client(s);
            double sx = c.x + 4 + bx + 50, sy = c.y + 4 + by + 20;   /* on the icon */
            ui_pointer_event pr = { .kind = UI_PRESS, .x = sx, .y = sy, .button = ui_mouse_button(UI_ACT_DRAG), .clicks = 1 };
            win_pointer(s, &pr);
            src_drag(f, bi, 0);
            char data[PATH_MAX];
            snprintf(data, sizeof data, "%s/b.txt\n", f->path);
            win_drop(s, sx + 150, sy + 200, MIME_WPS, data, strlen(data), UI_DND_MOVE, 0, 0);
            ui_pointer_event rl = { .kind = UI_RELEASE, .x = sx + 150, .y = sy + 200, .button = ui_mouse_button(UI_ACT_DRAG) };
            win_pointer(s, &rl);
            step++;
        } else if (step == 1 && t > 1500) {
            fprintf(stderr, "selftest: after moving b:%s\n", line);
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = SIZE_MAX;
            menu_chosen(mc, WPS_MI_ARRANGE);
            step++;
        } else if (step == 2 && t > 2500) {
            fprintf(stderr, "selftest: after Arrange:%s\n", line);
            step++;
        } else if (step == 3 && t > 2800) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "shadows")) {
        static double t0;
        static int step;
        static struct folder *home;
        if (!home) { if (!f->m.complete) return; home = f; t0 = now; }
        if (f != home) return;
        double t = now - t0;
        char data[PATH_MAX];
        size_t si = SIZE_MAX, ti = SIZE_MAX, li = SIZE_MAX;
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (!n) continue;
            struct page *pg = f->m.pages[i / PAGE];
            if (pg && pg->rows[i % PAGE].shadow) si = i;
            if (!strcmp(n, "T")) ti = i;
            if (!strcmp(n, "link-to-T")) li = i;
        }
        int nT = 0;
        char tpath[PATH_MAX];
        snprintf(tpath, sizeof tpath, "%s/T", f->path);
        for (struct folder *o = folders; o; o = o->next) nT += !o->closing && !strcmp(o->path, tpath);
        if (step == 0) {
            snprintf(data, sizeof data, "%s/T\n", f->path);
            ui_rect c = ui_frame_client(s);   /* on the background, away from the icons */
            win_drop(s, c.x + c.w - 60, c.y + c.h - 60, MIME_WPS, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL | UI_MOD_SHIFT);
            step++;
        } else if (step == 1 && t > 1500 && si != SIZE_MAX) {
            ui_item it;
            src_item(f, si, &it);
            fprintf(stderr, "selftest: shadow row: shadow %d, a folder %d, title '%s'\n", it.shadow, it.kind == UI_KIND_FOLDER, it.title);
            src_activate(f, si);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1200) {
            fprintf(stderr, "selftest: windows on T after opening the shadow: %d\n", nT);
            src_activate(f, si);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1200) {
            ui_item it;
            src_item(f, ti, &it);
            ui_item sh;
            src_item(f, si, &sh);
            fprintf(stderr, "selftest: after opening it again: %d window(s); T shown in use: %d, its shadow too: %d\n", nT,
                    it.in_use, sh.in_use);
            for (struct folder *o = folders; o; o = o->next)
                if (!strcmp(o->path, tpath)) win_close(o->win);
            step++;
            t0 = now;
        } else if (step == 4 && t > 800) {
            ui_item it;
            src_item(f, ti, &it);
            fprintf(stderr, "selftest: T window closed: in use %d\n", it.in_use);
            if (li != SIZE_MAX) {
                ui_item lk;
                src_item(f, li, &lk);
                fprintf(stderr, "selftest: link-to-T drawn as: a folder %d, link mark %d\n", lk.kind == UI_KIND_FOLDER, lk.link);
            }
            if (li != SIZE_MAX) src_activate(f, li);
            step++;
            t0 = now;
        } else if (step == 5 && t > 1200) {
            fprintf(stderr, "selftest: a plain link to T opened T: %d window(s)\n", nT);
            step++;
        } else if (step == 6) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "dropkeys")) {
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        static const char *names[] = { "move", "copy", "shadow" };
        if (step == 0) {
            int a = drop_how(UI_DND_MOVE, UI_DND_ASK, 0), b = drop_how(UI_DND_COPY, UI_DND_ASK, 0), c = drop_how(UI_DND_ASK, UI_DND_ASK, 0),
                d = drop_how(UI_DND_MOVE, UI_DND_ASK, UI_MOD_CTRL | UI_MOD_SHIFT);
            fprintf(stderr, "selftest: ask->move: %s; ask->copy: %s; ask unchanged: %s; Ctrl+Shift held: %s\n",
                    a < 0 ? "ask" : names[a], b < 0 ? "ask" : names[b], c < 0 ? "ask" : names[c], d < 0 ? "ask" : names[d]);
            /* The logged drag, end to end: began as ask, changed to move, no keys seen. */
            char data[PATH_MAX];
            snprintf(data, sizeof data, "%s/T\n", f->path);
            ui_rect cl = ui_frame_client(s);
            win_drop(s, cl.x + cl.w - 60, cl.y + cl.h - 60, MIME_WPS, data, strlen(data), UI_DND_MOVE, UI_DND_ASK, 0);
            step++;
        } else if (step == 1 && now - t0 > 1500) {
            bool made = false;
            for (size_t i = 0; i < f->m.total; i++) {
                struct page *pg = f->m.pages[i / PAGE];
                made |= pg && pg->rows[i % PAGE].shadow;
            }
            fprintf(stderr, "selftest: the logged drag made a shadow: %d\n", made);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "droppos")) {
        /* Drops onto the background land where they were dropped. */
        static double t0;
        static int step;
        static double sx, sy, cx, cy;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char data[PATH_MAX];
        ui_rect cl = ui_frame_client(s);
        if (step == 0) {
            sx = cl.x + 260; sy = cl.y + 200;
            snprintf(data, sizeof data, "%s/T\n", f->path);
            win_drop(s, sx, sy, MIME_WPS, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL | UI_MOD_SHIFT);   /* a shadow */
            cx = cl.x + 420; cy = cl.y + 120;
            snprintf(data, sizeof data, "%s/f.txt\n", f->path);
            win_drop(s, cx, cy, MIME_WPS, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);                 /* a copy (renamed) */
            step++;
        } else if (step == 1 && last_dialog) {
            ui_key_event r = { .sym = XKB_KEY_r, .utf8 = "r" };
            ui_surface_send_key(ui_dialog_surface(last_dialog), &r);
            last_dialog = NULL;
            step++;
            t0 = now;
        } else if (step == 2 && t > 2000) {
            double lsx, lsy, lcx, lcy;
            ui_container_to_list(f->cont, sx, sy, &lsx, &lsy);
            ui_container_to_list(f->cont, cx, cy, &lcx, &lcy);
            for (size_t i = 0; i < f->m.total; i++) {
                const char *n = model_name(&f->m, i);
                struct page *pg = f->m.pages[i / PAGE];
                double x, y;
                layout_pos(f->lay, i, &x, &y);
                if (pg && pg->rows[i % PAGE].shadow)
                    fprintf(stderr, "selftest: shadow at %.0f,%.0f (dropped at %.0f,%.0f)\n", x, y, lsx, lsy);
                if (n && !strcmp(n, "f (2).txt"))
                    fprintf(stderr, "selftest: copy 'f (2).txt' at %.0f,%.0f (dropped at %.0f,%.0f)\n", x, y, lcx, lcy);
            }
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "inuse")) {
        /* a.txt opened with a program that holds it for 2 s (the runner sets
         * up the default application); then a lock file marks b.txt. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        size_t ai = SIZE_MAX, bi = SIZE_MAX;
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (n && !strcmp(n, "a.txt")) ai = i;
            if (n && !strcmp(n, "b.txt")) bi = i;
        }
        ui_item ia, ib;
        if (ai != SIZE_MAX) src_item(f, ai, &ia);
        if (bi != SIZE_MAX) src_item(f, bi, &ib);
        if (step == 0 && ai != SIZE_MAX) { src_activate(f, ai); step++; t0 = now; }
        else if (step == 1 && t > 800) { fprintf(stderr, "selftest: a.txt open: in use %d\n", ia.in_use); step++; }
        else if (step == 2 && t > 3200) {
            fprintf(stderr, "selftest: its program ended: in use %d\n", ia.in_use);
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/.~lock.b.txt#", f->path);
            FILE *lf = fopen(p, "w");
            if (lf) fclose(lf);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500) {
            fprintf(stderr, "selftest: b.txt with a lock file: in use %d\n", bi != SIZE_MAX && ib.in_use);
            step++;
        } else if (step == 4) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "delete")) {
        /* a.txt and sub selected, Delete, Enter: into the trash; then b.txt,
         * Shift+Delete, Enter: gone for good. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        size_t ai = SIZE_MAX, si = SIZE_MAX, bi = SIZE_MAX;
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (n && !strcmp(n, "a.txt")) ai = i;
            if (n && !strcmp(n, "sub")) si = i;
            if (n && !strcmp(n, "b.txt")) bi = i;
        }
        ui_key_event del = { .sym = XKB_KEY_Delete }, sdel = { .sym = XKB_KEY_Delete, .mods = UI_MOD_SHIFT },
                     ret = { .sym = XKB_KEY_Return };
        char p[PATH_MAX + 64];
        const char *dh = getenv("WPS_SELFTEST_TRASH");
        if (step == 0 && ai != SIZE_MAX && si != SIZE_MAX) {
            ui_container_select_none(f->cont);
            ui_container_select(f->cont, ai);
            ui_container_select(f->cont, si);
            win_key(s, &del);
            fprintf(stderr, "selftest: Delete asks: %s\n", last_dialog ? "yes" : "no");
            if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            snprintf(p, sizeof p, "%s/files/a.txt", dh ? dh : "");
            bool a_in = access(p, F_OK) == 0;
            snprintf(p, sizeof p, "%s/files/sub", dh ? dh : "");
            fprintf(stderr, "selftest: after Delete: a.txt shown %d, sub shown %d; in the trash: a.txt %d, sub %d; status '%s'\n",
                    ai != SIZE_MAX, si != SIZE_MAX, a_in, access(p, F_OK) == 0, f->error);
            if (bi != SIZE_MAX) {
                ui_container_select_none(f->cont);
                ui_container_select(f->cont, bi);
                win_key(s, &sdel);
                if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            }
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            snprintf(p, sizeof p, "%s/files/b.txt", dh ? dh : "");
            char q[PATH_MAX];
            snprintf(q, sizeof q, "%s/b.txt", f->path);
            fprintf(stderr, "selftest: after Shift+Delete: b.txt shown %d, on disk %d, in the trash %d; status '%s'\n",
                    bi != SIZE_MAX, access(q, F_OK) == 0, access(p, F_OK) == 0, f->error);
            step++;
        } else if (step == 3) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "desktop")) {
        /* The desktop: frameless, its folder's icons on the wallpaper. */
        static double t0;
        static int step;
        if (!f->desktop) return;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        if (step == 0 && (strcmp(wallpaper_showing(), "color") || t > 20000)) {
            ui_rect c = ui_frame_client(s);
            fprintf(stderr, "selftest: desktop %s: %zu objects, frameless %d (%.0fx%.0f of %.0fx%.0f), background %s after %.1f s\n",
                    f->path, f->m.total, c.x == 0 && c.y == 0 && c.w == ui_surface_width(s) && c.h == ui_surface_height(s),
                    c.w, c.h, ui_surface_width(s), ui_surface_height(s), wallpaper_showing(), t / 1000);
            win_close(s);   /* the desktop stays */
            step++;
            t0 = now;
        } else if (step == 1 && t > 300) {
            fprintf(stderr, "selftest: after Close: desktop still open %d\n", !f->closing);
            if (getenv("WPS_SHELL_SHOT")) {   /* a picture of it (with WPS_UI_NO_LAYER=1: all on one surface) */
                cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ui_surface_width(s), (int)ui_surface_height(s));
                cairo_t *icr = cairo_create(img);
                win_draw(s, icr, (ui_rect){ 0, 0, ui_surface_width(s), ui_surface_height(s) });
                cairo_destroy(icr);
                cairo_surface_write_to_png(img, getenv("WPS_SHELL_SHOT"));
                cairo_surface_destroy(img);
            }
            step++;
        } else if (step == 2) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            static double last;
            (void)last;
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "templates")) {
        /* Two windows: <D>/Templates (the templates folder) and <D>. Drop
         * the built-in Folder template on <D>, type a name; drop Data File
         * and the Letter.odt template; Create another on the new folder. */
        static double t0;
        static int step;
        struct folder *tw = NULL;
        for (struct folder *o = folders; o; o = o->next)
            if (strlen(o->path) > 10 && !strcmp(o->path + strlen(o->path) - 10, "/Templates")) tw = o;
        if (f == tw || !tw) return;
        if (!t0) { if (!f->m.complete || !tw->m.complete) return; t0 = now; }
        double t = now - t0;
        ui_rect cl = ui_frame_client(s);
        char data[PATH_MAX + 32], tpl_folder[PATH_MAX] = "", tpl_data[PATH_MAX] = "", tpl_letter[PATH_MAX] = "";
        char rows[600] = "";
        for (size_t i = 0; i < tw->m.total; i++) {
            struct row *r = row_at(tw, i);
            if (!r || !r->name) continue;
            ui_item it;
            src_item(tw, i, &it);
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s%s(%s%s)", rows[0] ? ", " : "", it.title ? it.title : r->name,
                     r->kind == UI_KIND_FOLDER ? "folder" : r->kind == UI_KIND_PROGRAM ? "program" : "file", r->tpl ? ", template" : "");
            char tg[PATH_MAX];
            target_of(tw, i, tg, sizeof tg);
            if (r->tpl && r->name[0] == '\x01' && r->kind == UI_KIND_FOLDER) snprintf(tpl_folder, sizeof tpl_folder, "%s", tg);
            if (r->tpl && r->name[0] == '\x01' && r->kind == UI_KIND_FILE) snprintf(tpl_data, sizeof tpl_data, "%s", tg);
            if (r->tpl && !strcmp(r->name, "Letter.odt")) snprintf(tpl_letter, sizeof tpl_letter, "%s", tg);
        }
        char p[PATH_MAX + 32];
        if (step == 0 && t > 300 && tpl_folder[0] && tpl_data[0] && tpl_letter[0]) {
            fprintf(stderr, "selftest: the templates folder: %s\n", rows);
            snprintf(data, sizeof data, "%s\ttemplate\n", tpl_folder);
            win_drop(s, cl.x + 220, cl.y + 150, MIME_WPS, data, strlen(data), UI_DND_MOVE, 0, 0);
            step++;
            t0 = now;
        } else if (step == 0 && t > 5000) {
            fprintf(stderr, "selftest: the templates folder lacks the templates: %s\n", rows);
            ui_app_quit(app);
            step = 99;
        } else if (step == 1 && t > 1500) {
            size_t ri = ui_container_rename_item(f->cont);
            fprintf(stderr, "selftest: dropped the Folder template: Folder made %d, its name being edited %d\n",
                    (snprintf(p, sizeof p, "%s/Folder", f->path), access(p, F_OK) == 0), ri != SIZE_MAX && !strcmp(model_name(&f->m, ri), "Folder"));
            if (ri != SIZE_MAX) {
                ui_key_event sa = { .sym = XKB_KEY_a, .mods = UI_MOD_CTRL };
                ui_surface_send_key(s, &sa);
                for (const char *c = "Letters"; *c; c++) {
                    ui_key_event k = { .sym = (uint32_t)*c };
                    k.utf8[0] = *c;
                    ui_surface_send_key(s, &k);
                }
                ui_key_event en = { .sym = XKB_KEY_Return };
                ui_surface_send_key(s, &en);
            }
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            snprintf(p, sizeof p, "%s/Letters", f->path);
            fprintf(stderr, "selftest: typed a name: Letters on disk %d\n", access(p, F_OK) == 0);
            snprintf(data, sizeof data, "%s\ttemplate\n%s\ttemplate\n", tpl_data, tpl_letter);
            win_drop(s, cl.x + 420, cl.y + 150, MIME_WPS, data, strlen(data), UI_DND_COPY, 0, UI_MOD_CTRL);
            step++;
            t0 = now;
        } else if (step == 3 && t > 2000) {
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ui_container_rename_item(f->cont) != SIZE_MAX) ui_surface_send_key(s, &esc);
            char a1[PATH_MAX + 32], a2[PATH_MAX + 32];
            snprintf(a1, sizeof a1, "%s/Data File", f->path);
            snprintf(a2, sizeof a2, "%s/Letter.odt", f->path);
            snprintf(p, sizeof p, "%s/Templates/Letter.odt", f->path);
            fprintf(stderr, "selftest: dropped Data File and Letter.odt: made %d %d; the templates stay %d\n",
                    access(a1, F_OK) == 0, access(a2, F_OK) == 0, access(p, F_OK) == 0);
            size_t li = SIZE_MAX;
            for (size_t i = 0; i < f->m.total; i++)
                if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), "Letters")) li = i;
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = li;
            menu_chosen(mc, WPS_MI_CREATE_ANOTHER);
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500) {
            size_t ri = ui_container_rename_item(f->cont);
            snprintf(p, sizeof p, "%s/New Folder", f->path);
            fprintf(stderr, "selftest: Create another on Letters: New Folder %d, its name being edited %d\n", access(p, F_OK) == 0,
                    ri != SIZE_MAX && !strcmp(model_name(&f->m, ri), "New Folder"));
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ri != SIZE_MAX) ui_surface_send_key(s, &esc);
            step++;
        } else if (step == 5) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            const char *shot = getenv("WPS_SHELL_SHOT");   /* the templates window (with WPS_UI_NO_LAYER=1) */
            if (shot) {
                cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ui_surface_width(tw->win), (int)ui_surface_height(tw->win));
                cairo_t *icr = cairo_create(img);
                win_draw(tw->win, icr, (ui_rect){ 0, 0, ui_surface_width(tw->win), ui_surface_height(tw->win) });
                cairo_destroy(icr);
                cairo_surface_write_to_png(img, shot);
                cairo_surface_destroy(img);
            }
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "follow")) {
        /* A 3000-entry folder follows changes without being read again:
         * 40 new files, 20 renamed, 20 deleted; the selection stays. */
        static double t0;
        static int step;
        char p[PATH_MAX + 64], q[PATH_MAX + 64];
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        if (step == 0 && t > 500) {
            /* a row on screen (a big folder's window has read only those) */
            ssize_t keep = f->vis_first + 5 < f->m.total && model_name(&f->m, f->vis_first + 5) ? (ssize_t)(f->vis_first + 5) : -1;
            static char kept[NAME_MAX + 1];
            if (keep >= 0) {
                snprintf(kept, sizeof kept, "%s", model_name(&f->m, (size_t)keep));
                ui_container_select(f->cont, (size_t)keep);
                ui_container_set_cursor(f->cont, (size_t)keep);
            }
            setenv("WPS_SELFTEST_KEPT", kept, 1);
            reloads = 0;
            int adds = getenv("FOLLOW_ADD") ? atoi(getenv("FOLLOW_ADD")) : 40;   /* a burst, for timing */
            for (int i = 0; i < adds; i++) { snprintf(p, sizeof p, "%s/new-%04d", f->path, i); FILE *x = fopen(p, "w"); if (x) fclose(x); }
            for (int i = 0; i < 20; i++) {
                snprintf(p, sizeof p, "%s/file-%04d", f->path, i * 7);
                snprintf(q, sizeof q, "%s/moved-%02d", f->path, i);
                rename(p, q);
            }
            for (int i = 0; i < 20; i++) { snprintf(p, sizeof p, "%s/file-%04d", f->path, 2000 + i); unlink(p); }
            step++;
            t0 = now;
        } else if (step == 1 && t > (getenv("FOLLOW_WAIT") ? atof(getenv("FOLLOW_WAIT")) : 2500)) {
            /* the rows against the folder itself */
            /* every row read so far exists, and the count is the folder's */
            size_t on_disk = 0, wrong = 0, loaded = 0;
            DIR *d = opendir(f->path);
            struct dirent *de;
            while (d && (de = readdir(d))) on_disk += de->d_name[0] != '.';
            if (d) closedir(d);
            for (size_t i = 0; i < f->m.total; i++) {
                const char *n = model_name(&f->m, i);
                if (!n) continue;
                loaded++;
                snprintf(p, sizeof p, "%s/%s", f->path, n);
                if (access(p, F_OK) != 0) wrong++;
            }
            const char *kept = getenv("WPS_SELFTEST_KEPT");
            ssize_t k = kept && *kept ? model_find(&f->m, kept) : -1;
            fprintf(stderr, "selftest: on disk %zu, in the window %zu (%zu names read, %zu not on disk); full re-reads %d; "
                    "%s still selected %d\n", on_disk, f->m.total, loaded, wrong, reloads, kept ? kept : "?",
                    k >= 0 && ui_container_is_selected(f->cont, (size_t)k));
            step++;
        } else if (step == 2) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "events")) {
        /* Action events and their sounds: a menu, then deleting a.txt
         * (asked first: alert.warning; then object.trash). */
        static double t0;
        static int step;
        static char seen[1024];
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        if (step == 0) {
            events_subscribe(selftest_record, seen);
            ui_menu_item one[] = { { 1, "One", true, false, NULL, 0 } };
            ui_menu_popup(s, one, 1, 50, 50, 0, selftest_chosen, NULL);
            step++;
            t0 = now;
        } else if (step == 1 && t > 500) {
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            ui_surface_send_key(s, &esc);
            ssize_t ai = model_find(&f->m, "a.txt");
            if (ai >= 0) delete_request(f, (size_t)ai, false);
            ui_key_event ret = { .sym = XKB_KEY_Return };
            if (last_dialog) ui_surface_send_key(ui_dialog_surface(last_dialog), &ret);
            step++;
            t0 = now;
        } else if (step == 2 && t > 2000) {
            fprintf(stderr, "selftest: events:%s\n", seen);
            fprintf(stderr, "selftest: sound: PipeWire %s, formats %s, %u sounds started\n", sound_ready() ? "connected" : "not there",
                    sound_formats(), sound_played());
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "setup")) {
        /* The settings objects, live: the Scheme Palette to dark, System's
         * corners and delete confirmation, a Sound event; then a delete
         * that is not asked about. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete || !setup_ids[0][0]) return; t0 = now; }
        double t = now - t0;
        const char *sys = NULL, *snd = NULL, *pal = NULL;
        for (int i = 0; i < 4; i++) {
            /* the order is the service's: System, Sound, Mouse, Scheme Palette */
            if (i == 0) sys = setup_ids[i];
            if (i == 1) snd = setup_ids[i];
            if (i == 3) pal = setup_ids[i];
        }
        if (step == 0) {
            fprintf(stderr, "selftest: at start: scheme %s, corners %.0f, confirm delete %d, window.open sound %s\n",
                    ui_scheme_now.name, ui_scheme_now.corner_radius, confirm_delete, events_sound_for("window.open") ? "yes" : "none");
            cv one = { .k = CV_INT, .i = 1 }, three = { .k = CV_INT, .i = 3 }, no = { .k = CV_BOOL, .b = false },
               bell = { .k = CV_TEXT, .s = (char *)"bell", .len = 4 };
            svc_settings_set(pal, "scheme", &one, -1, ignore_reply, NULL);
            svc_settings_set(sys, "corner_radius", &three, -1, ignore_reply, NULL);
            svc_settings_set(sys, "confirm_delete", &no, -1, ignore_reply, NULL);
            svc_settings_set(snd, "window.open", &bell, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            fprintf(stderr, "selftest: after the notebooks' changes: scheme %s, corners %.0f, confirm delete %d, window.open sound %s\n",
                    ui_scheme_now.name, ui_scheme_now.corner_radius, confirm_delete,
                    events_sound_for("window.open") ? strrchr(events_sound_for("window.open"), '/') + 1 : "none");
            ssize_t ai = model_find(&f->m, "a.txt");
            last_dialog = NULL;
            if (ai >= 0) delete_request(f, (size_t)ai, false);
            fprintf(stderr, "selftest: Delete asked: %s\n", last_dialog ? "yes" : "no");
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            fprintf(stderr, "selftest: a.txt shown %d\n", model_find(&f->m, "a.txt") >= 0);
            const char *shot = getenv("WPS_SHELL_SHOT");   /* the window in the dark scheme (WPS_UI_NO_LAYER=1) */
            if (shot) {
                cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ui_surface_width(s), (int)ui_surface_height(s));
                cairo_t *icr = cairo_create(img);
                win_draw(s, icr, (ui_rect){ 0, 0, ui_surface_width(s), ui_surface_height(s) });
                cairo_destroy(icr);
                cairo_surface_write_to_png(img, shot);
                cairo_surface_destroy(img);
            }
            step++;
        } else if (step == 3) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "newfolder")) {
        /* As reported: a folder from the Folder template, opened; a file
         * from the Data File template dropped into its window; then the
         * open folder renamed. */
        static double t0;
        static int step;
        static struct folder *nw;
        struct folder *tw = NULL;
        for (struct folder *o = folders; o; o = o->next)
            if (strlen(o->path) > 10 && !strcmp(o->path + strlen(o->path) - 10, "/Templates")) tw = o;
        if (f == tw || !tw || (nw && f == nw)) return;
        if (!t0) { if (!f->m.complete || !tw->m.complete) return; t0 = now; }
        double t = now - t0;
        char tf[PATH_MAX] = "", td[PATH_MAX] = "", data[PATH_MAX + 32], p[PATH_MAX + 32];
        for (size_t i = 0; i < tw->m.total; i++) {
            struct row *r = row_at(tw, i);
            if (!r || !r->name || !r->tpl || r->name[0] != '\x01') continue;
            if (r->kind == UI_KIND_FOLDER) target_of(tw, i, tf, sizeof tf);
            if (r->kind == UI_KIND_FILE) target_of(tw, i, td, sizeof td);
        }
        if (step == 0 && tf[0] && td[0]) {
            snprintf(data, sizeof data, "%s\ttemplate\n", tf);
            win_drop(s, 200, 150, MIME_WPS, data, strlen(data), UI_DND_MOVE, 0, 0);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ui_container_rename_item(f->cont) != SIZE_MAX) ui_surface_send_key(s, &esc);
            snprintf(p, sizeof p, "%s/Folder", f->path);
            nw = open_folder(p, UI_VIEW_ICON);
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500 && nw && nw->m.complete) {
            snprintf(data, sizeof data, "%s\ttemplate\n", td);
            win_drop(nw->win, 150, 120, MIME_WPS, data, strlen(data), UI_DND_MOVE, 0, 0);
            step++;
            t0 = now;
        } else if (step == 3 && t > 2000) {
            snprintf(p, sizeof p, "%s/Folder/Data File", f->path);
            fprintf(stderr, "selftest: in the new folder: Data File on disk %d, in its window %d (window %zu objects, rename open %d)\n",
                    access(p, F_OK) == 0, model_find(&nw->m, "Data File") >= 0, nw->m.total, ui_container_rename_item(nw->cont) != SIZE_MAX);
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ui_container_rename_item(nw->cont) != SIZE_MAX) ui_surface_send_key(nw->win, &esc);
            /* three more Data Files, each dropped at its own spot */
            snprintf(data, sizeof data, "%s\ttemplate\n", td);
            static const double spots[3][2] = { { 350, 120 }, { 150, 300 }, { 400, 330 } };
            for (int k = 0; k < 3; k++) win_drop(nw->win, spots[k][0], spots[k][1], MIME_WPS, data, strlen(data), UI_DND_MOVE, 0, 0);
            step = 10;
            t0 = now;
        } else if (step == 10 && t > 3000) {
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            if (ui_container_rename_item(nw->cont) != SIZE_MAX) ui_surface_send_key(nw->win, &esc);
            static const double spots[4][2] = { { 150, 120 }, { 350, 120 }, { 150, 300 }, { 400, 330 } };
            char out[1024] = "";
            for (size_t i = 0; i < nw->m.total; i++) {
                ui_item it = { 0 };
                src_item(nw, i, &it);
                double x, y, lx = 0, ly = 0;
                layout_pos(nw->lay, i, &x, &y);
                /* where it was dropped (each in drop order: Data File, (2), (3), (4)) */
                const char *n = model_name(&nw->m, i);
                int k = !n ? -1 : !strcmp(n, "Data File") ? 0 : !strcmp(n, "Data File (2)") ? 1 : !strcmp(n, "Data File (3)") ? 2 :
                        !strcmp(n, "Data File (4)") ? 3 : -1;
                if (k >= 0) ui_container_to_list(nw->cont, spots[k][0], spots[k][1], &lx, &ly);
                snprintf(out + strlen(out), sizeof out - strlen(out), " [%s titled '%s' at %.0f,%.0f, dropped at %.0f,%.0f]",
                         n ? n : "?", it.title ? it.title : "", x, y, lx, ly);
            }
            fprintf(stderr, "selftest: four Data Files:%s\n", out);
            /* rename the open folder (as its icon's rename does) */
            snprintf(p, sizeof p, "%s/Folder", f->path);
            svc_rename(p, "Renamed", ignore_reply, NULL);
            step = 4;
            t0 = now;
        } else if (step == 4 && t > 2000) {
            snprintf(p, sizeof p, "%s/Renamed", f->path);
            ssize_t ri = model_find(&f->m, "Renamed");
            ui_item it = { 0 };
            if (ri >= 0) src_item(f, (size_t)ri, &it);
            fprintf(stderr, "selftest: after renaming the open folder: its window shows %s; its icon in use %d\n", nw->path, it.in_use);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "appearance")) {
        /* The System object's window options: strong shadows, translucent
         * title bars, the mirrored layout; then the frame and the buffer. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete || !setup_ids[0][0]) return; t0 = now; }
        double t = now - t0;
        if (step == 0) {
            cv two = { .k = CV_INT, .i = 2 }, one = { .k = CV_INT, .i = 1 };
            svc_settings_set(setup_ids[0], "shadows", &two, -1, ignore_reply, NULL);
            svc_settings_set(setup_ids[0], "translucency", &one, -1, ignore_reply, NULL);
            svc_settings_set(setup_ids[0], "title_layout", &one, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1500) {
            ui_rect c = ui_frame_client(s);
            double tw = ui_surface_width(s), ty = c.y - ui_scheme_now.title_h / 2;
            fprintf(stderr, "selftest: options: shadows %d, translucency %d, mirrored %d\n", ui_scheme_now.shadows,
                    ui_scheme_now.translucency, ui_scheme_now.title_mirrored);
            fprintf(stderr, "selftest: mirrored frame: left end is %s, right end is %s\n",
                    ui_frame_hit(s, 12, ty) == UI_HIT_CLOSE ? "close" : "something else",
                    ui_frame_hit(s, tw - 12, ty) == UI_HIT_SYSMENU ? "the system menu" : "something else");
            step++;
        } else if (step == 2) {
            size_t d = ui_surface_verify(s, getenv("WPS_VERIFY_PNG"));   /* WPS_VERIFY_PNG=prefix saves a mismatch */
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw (inside the shadow): %zu differ\n", d);
            if (getenv("WPS_SHELL_SHOT")) fprintf(stderr, "selftest: buffer saved %d\n", ui_surface_snapshot(s, getenv("WPS_SHELL_SHOT")));
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "gnome")) {
        /* Settings shared with GNOME, both ways (the desktop process). */
        static double t0;
        static int step;
        if (!f->desktop) return;
        if (!t0) { if (!f->m.complete || !setup_cls[3][0]) return; t0 = now; }
        double t = now - t0;
        const char *mouse = NULL, *pal = NULL;
        for (int i = 0; i < 4; i++) {
            if (!strcmp(setup_cls[i], "WPMouse")) mouse = setup_ids[i];
            if (!strcmp(setup_cls[i], "WPSchemePalette")) pal = setup_ids[i];
        }
        char v[256];
        #define GGET(schema, key) (snprintf(v, sizeof v, "gsettings get %s %s", schema, key), \
            (void)0, ({ FILE *p_ = popen(v, "r"); v[0] = 0; if (p_) { if (!fgets(v, sizeof v, p_)) v[0] = 0; pclose(p_); } \
            v[strcspn(v, "\n")] = 0; v; }))
        if (step == 0 && t > 1000) {
            fprintf(stderr, "selftest: at start: scheme %s, GNOME color-scheme %s\n", ui_scheme_now.name,
                    GGET("org.gnome.desktop.interface", "color-scheme"));
            if (system("gsettings set org.gnome.desktop.interface color-scheme prefer-dark")) {}
            step++;
            t0 = now;
        } else if (step == 1 && t > 2500) {
            fprintf(stderr, "selftest: GNOME to dark: WPS scheme %s\n", ui_scheme_now.name);
            cv d = { .k = CV_TEXT, .s = (char *)"650", .len = 3 };
            svc_settings_set(mouse, "double_click_ms", &d, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 2 && t > 2500) {
            fprintf(stderr, "selftest: WPS double-click 650: GNOME %s, the toolkit %d\n",
                    GGET("org.gnome.desktop.peripherals.mouse", "double-click"), ui_mouse.double_click_ms);
            if (system("gsettings set org.gnome.desktop.interface font-name 'DejaVu Sans 12'")) {}
            step++;
            t0 = now;
        } else if (step == 3 && t > 2500) {
            fprintf(stderr, "selftest: GNOME font DejaVu Sans 12: WPS font %.0f px\n", ui_scheme_now.font_px);
            cv hc = { .k = CV_TEXT, .s = (char *)"high contrast", .len = 13 };
            svc_settings_set(pal, "scheme", &hc, -1, ignore_reply, NULL);
            step++;
            t0 = now;
        } else if (step == 4 && t > 2500) {
            fprintf(stderr, "selftest: WPS high contrast: GNOME high-contrast %s; WPS scheme %s\n",
                    GGET("org.gnome.desktop.a11y.interface", "high-contrast"), ui_scheme_now.name);
            ui_app_quit(app);
            step++;
        }
        #undef GGET
        return;
    }
    if (st && !strcmp(st, "lpfill")) {
        /* Three objects in the first LaunchPad drawer, then nothing (for
         * PROBE_MODE=drawer to click). */
        static bool done;
        if (!f->desktop || done || launchpad_count() < 2) return;
        done = true;
        const char *names[] = { "notes.txt", "Projects", "added-later.txt" };
        for (int k = 0; k < 3; k++) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", f->path, names[k]);
            if (access(p, F_OK) == 0) launchpad_drawer(0, p);
        }
        fprintf(stderr, "selftest: first drawer filled\n");
        return;
    }
    if (st && !strcmp(st, "launchpad")) {
        /* The LaunchPad on the desktop: its first buttons, a button
         * pressed (a folder opens), a drop on it and on a drawer arrow. */
        static double t0;
        static int step;
        if (!f->desktop) return;
        if (!t0) { if (launchpad_count() < 3) return; t0 = now; }
        double t = now - t0;
        char rows[600] = "";
        #define LROWS() do { rows[0] = 0; for (size_t i_ = 0; launchpad_title(i_); i_++) \
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s'%s'", i_ ? ", " : "", launchpad_title(i_)); } while (0)
        if (step == 0 && t > 500) {
            LROWS();
            fprintf(stderr, "selftest: LaunchPad (%zu): %s\n", launchpad_count(), rows);
            char shot[PATH_MAX];
            if (getenv("WPS_SHELL_SHOT")) {
                snprintf(shot, sizeof shot, "%s-launchpad.png", getenv("WPS_SHELL_SHOT"));
                launchpad_snapshot(shot);
            }
            size_t tb = SIZE_MAX;
            for (size_t i = 0; launchpad_title(i); i++) if (!strcmp(launchpad_title(i), "templates")) tb = i;
            fprintf(stderr, "selftest: press Templates: %d\n", tb != SIZE_MAX && launchpad_press(4 + tb));
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/notes.txt", f->path);
            launchpad_add(p);
            launchpad_drawer(0, p);
            step++;
            t0 = now;
        } else if (step == 1 && t > 2000) {
            LROWS();
            fprintf(stderr, "selftest: after the drop (%zu): %s\n", launchpad_count(), rows);
            int open = 0;
            for (struct folder *o = folders; o; o = o->next) open += !o->closing && !o->desktop && strstr(o->path, "templates");
            fprintf(stderr, "selftest: templates window open: %d\n", open);
            char d[PATH_MAX];
            snprintf(d, sizeof d, "%s Drawers", launchpad_dir());
            DIR *dd = opendir(d);
            int drawers = 0;
            struct dirent *de;
            while (dd && (de = readdir(dd))) drawers += de->d_name[0] != '.';
            if (dd) closedir(dd);
            fprintf(stderr, "selftest: drawers made: %d\n", drawers);
            if (getenv("WPS_SHELL_SHOT")) {
                char shot[PATH_MAX];
                snprintf(shot, sizeof shot, "%s-launchpad2.png", getenv("WPS_SHELL_SHOT"));
                launchpad_snapshot(shot);
            }
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            ui_app_quit(app);
            step++;
        }
        #undef LROWS
        return;
    }
    if (st && !strcmp(st, "iconlib")) {
        /* The picker's icon library (WPS_ICON_LIBRARY), chosen as a user
         * does: by the button, then back to the themes and again. */
        static double t0;
        static int step;
        if (f != folders || f->closing || !f->m.complete) return;
        ui_surface_damage_all(f->win);
        if (!t0) { t0 = now; return; }
        double t = now - t0;
        if (step == 0 && t > 300) { iconpick_open("test", NULL, 0, "icon"); step++; t0 = now; }
        else if (step == 1 && t > 500) { iconpick_source(true); step++; t0 = now; }
        else if (step == 2 && t > 1500) {
            fprintf(stderr, "selftest: library: %zu icons\n", iconpick_count());
            if (getenv("WPS_SHELL_SHOT")) iconpick_snapshot(getenv("WPS_SHELL_SHOT"));
            iconpick_source(false); step++; t0 = now;
        } else if (step == 3 && t > 1500) { iconpick_source(true); step++; t0 = now; }
        else if (step == 4 && t > 1500) {
            fprintf(stderr, "selftest: library again: %zu icons\n", iconpick_count());
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "icons")) {
        /* An object's own icon (G4): Properties' Icon page with an image
         * dropped on it, then one chosen in the picker, then Default. */
        static double t0;
        static int step;
        static char target[PATH_MAX];
        if (f != folders || f->closing || !f->m.complete) return;
        ui_surface_damage_all(f->win);   /* frames keep coming */
        if (!t0) { t0 = now; snprintf(target, sizeof target, "%s/a.txt", f->path); return; }
        double t = now - t0;
        struct props *p = NULL;
        for (struct props *q = props_open; q; q = q->next) if (!strcmp(q->target, target)) p = q;
        ssize_t row = model_find(&f->m, "a.txt");
        const char *icon = row >= 0 && row_ptr(&f->m, (size_t)row)->icon ? row_ptr(&f->m, (size_t)row)->icon : "";
        char shot[PATH_MAX];
        const char *sh = getenv("WPS_SHELL_SHOT");
        if (step == 0 && t > 500) { open_properties(target); step++; t0 = now; }
        else if (step == 1 && p && p->nb && t > 800) {
            ui_value v = { .text = "/usr/share/icons/Adwaita/scalable/places/folder-music.svg" };   /* as dropped */
            props_set(p, "icon", UI_FIELD_ICON, &v);
            step++; t0 = now;
        } else if (step == 2 && (*icon || t > 6000)) {
            fprintf(stderr, "selftest: dropped image: icon %s, drawn %d\n", *icon ? strrchr(icon, '/') + 1 : "(none)", icons_get(icon) != NULL);
            ui_notebook_show_page(p->nb, 1);   /* the Icon page, for its picture */
            ui_value v = { .text = NULL };   /* Choose... */
            props_set(p, "icon", UI_FIELD_ICON, &v);
            step++; t0 = now;
        } else if (step == 3 && (iconpick_ready() || t > 10000)) {
            fprintf(stderr, "selftest: picker: %zu icons in the themes\n", iconpick_count());
            iconpick_search("folder");
            fprintf(stderr, "selftest: picker, \"folder\": %zu, first '%s'\n", iconpick_count(), iconpick_name(0) ? iconpick_name(0) : "");
            if (sh && p->nb) { snprintf(shot, sizeof shot, "%s-notebook.png", sh); ui_surface_snapshot(ui_notebook_surface(p->nb), shot); }
            if (sh) { snprintf(shot, sizeof shot, "%s-folder.png", sh); ui_surface_snapshot(f->win, shot); }
            step++; t0 = now;
        } else if (step == 4 && t > 1500) {
            if (sh) { snprintf(shot, sizeof shot, "%s-picker.png", sh); iconpick_snapshot(shot); }
            iconpick_choose(0);
            step++; t0 = now;
        } else if (step == 5 && (t > 6000 || (*icon && !strstr(icon, "folder-music")))) {
            fprintf(stderr, "selftest: chosen in the picker: icon %s\n", *icon ? strrchr(icon, '/') + 1 : "(none)");
            ui_value v = { .text = "" };   /* Default */
            props_set(p, "icon", UI_FIELD_ICON, &v);
            step++; t0 = now;
        } else if (step == 6 && (!*icon || t > 4000)) {
            fprintf(stderr, "selftest: after Default: icon %s\n", *icon ? icon : "(its kind's)");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "a11y")) {
        /* For a screen reader (tests/gui/a11y.py listens): the cursor
         * through the folder by keys, then a dialog's buttons. */
        static double t0;
        static int step;
        static ui_dialog *d;
        if (f != folders || f->closing || !f->m.complete) return;
        if (!t0) { t0 = now; return; }
        double t = now - t0;
        ui_surface_damage_all(f->win);   /* frames keep coming: this runs on each */
        ui_key_event tab = { .sym = XKB_KEY_Tab };
        if (step < 3 && t > 2500 + step * 500.0) {
            ui_key_event k = { .sym = step == 0 ? XKB_KEY_End : step == 1 ? XKB_KEY_Home : XKB_KEY_Right };
            ui_container_key(f->cont, &k);
            step++;
        }
        else if (step == 3 && t > 4500) {
            static const char *const b[] = { "Delete", "Cancel" };
            d = ui_dialog_open(app, "Delete", "Delete gamma?", b, 2, 1, NULL, NULL);
            step++;
        } else if (step == 4 && t > 6000) {
            ui_dialog_key(d, &tab);
            step++;
        } else if (step == 5 && t > 6800) {
            findwin_open(f->path, NULL);   /* its fields: Name, then Tab: Folder, Include subfolders */
            step++;
        } else if (step >= 6 && step <= 7 && t > 7800 + (step - 6) * 500.0) {
            findwin_key(XKB_KEY_Tab);
            step++;
        } else if (step == 8 && t > 8800) {
            findwin_key(XKB_KEY_space);   /* the check box, unticked */
            step++;
        } else if (step == 9 && t > 11000) {
            ui_app_quit(app);
        }
        return;
    }
    if (st && !strcmp(st, "typeahead")) {
        /* Keys typed in a folder: the item they spell is the cursor's. */
        static double t0;
        if (f != folders || f->closing || !f->m.complete) return;
        if (!t0) { t0 = now; return; }
        if (now - t0 < 500) return;
        static const char *const typed[] = { "b", "b", "b", "ca", "CAT", "x" };
        for (size_t k = 0; k < sizeof typed / sizeof *typed; k++) {
            for (const char *p = typed[k]; *p; p++) {
                ui_key_event e = { .sym = (uint32_t)(unsigned char)*p };
                e.utf8[0] = *p;
                ui_container_key(f->cont, &e);
            }
            size_t c = ui_container_cursor(f->cont);
            ui_item it = { 0 };
            if (c != SIZE_MAX) src_item(f, c, &it);
            fprintf(stderr, "selftest: typed '%s': %s\n", typed[k], it.title ? it.title : "(none)");
            /* the next word: as after a pause */
            if (k != 0 && k != 1) ui_container_typeahead_reset(f->cont);   /* b, b, b: one after another */
        }
        ui_app_quit(app);
        return;
    }
    if (st && !strcmp(st, "find")) {
        /* Find (G4 search) on the folder: typed as a user types (each key
         * a new search), then a pattern, then folders only. */
        static double t0;
        static int step;
        if (f != folders || f->closing) return;
        if (!t0) t0 = now;
        double t = now - t0;
        static const char *const typed[] = { "note", "*.TXT", "a" };
        if (step == 0 && t > 500) {
            findwin_open(f->path, NULL);
            findwin_type(typed[0]);
            step++;
            t0 = now;
        } else if (step == 11 && t > 400) {
            if (getenv("WPS_SHELL_SHOT")) {
                char shot[PATH_MAX];
                snprintf(shot, sizeof shot, "%s-preedit.png", getenv("WPS_SHELL_SHOT"));
                findwin_snapshot(shot);
            }
            ui_text_input_inject(app, "日本", NULL);
            step = 10;
            t0 = now;
        } else if (step == 10 && (t > 5000 || (!findwin_searching() && t > 300))) {
            fprintf(stderr, "selftest: find by input method: %zu: %s\n", findwin_count(), findwin_result(0) ? strrchr(findwin_result(0), '/') + 1 : "");
            ui_app_quit(app);
            return;
        } else if (step >= 1 && step <= 3 && (t > 5000 || (!findwin_searching() && t > 300))) {
            char rows[1200] = "";
            size_t pl = strlen(f->path);
            for (size_t i = 0; findwin_result(i); i++)
                snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s'%s'", i ? ", " : "", findwin_result(i) + pl);
            fprintf(stderr, "selftest: find '%s'%s: %zu: %s\n", typed[step - 1], step == 3 ? " (folders)" : "", findwin_count(), rows);
            if (step == 2 && getenv("WPS_SHELL_SHOT")) findwin_snapshot(getenv("WPS_SHELL_SHOT"));
            if (step == 1) findwin_type(typed[1]);
            else if (step == 3) {
                /* an input method: 日本 composed (preedit), then committed */
                findwin_kind(0);
                findwin_type("");
                ui_text_input_inject(app, NULL, "にほん");
                step = 11;   /* drawn, then committed */
                t0 = now;
                return;
            }
            else if (step == 2) {
                /* Type: Folders (the type field, as Space there does), then the name */
                findwin_kind(1);
                findwin_type(typed[2]);
            }
            step++;
            t0 = now;
        }
        return;
    }
    if (st && !strcmp(st, "winlist")) {
        /* The Window List and the Minimized Window Viewer, with the
         * extension: the folder window run.sh opens, minimized and restored. */
        static double t0;
        static int step;
        if (!f->desktop) return;
        if (!t0) { if (!winlist_count(false)) return; t0 = now; }
        double t = now - t0;
        char rows[600] = "";
        #define ROWS(minimized) do { rows[0] = 0; for (size_t i_ = 0; winlist_title(i_, minimized); i_++) \
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s'%s'", i_ ? ", " : "", winlist_title(i_, minimized)); } while (0)
        if (step == 0 && t > 1000) {
            winlist_open(false);
            step++;
            t0 = now;
        } else if (step == 1 && t > 1000) {
            ROWS(false);
            fprintf(stderr, "selftest: Window List (%zu): %s\n", winlist_count(false), rows);
            char shot[PATH_MAX];
            if (getenv("WPS_SHELL_SHOT")) {
                snprintf(shot, sizeof shot, "%s-list.png", getenv("WPS_SHELL_SHOT"));
                winlist_snapshot(false, shot);
            }
            fprintf(stderr, "selftest: minimize Projects: sent %d\n", winlist_command("minimize", "Projects"));
            step++;
            t0 = now;
        } else if (step == 2 && t > 1500) {
            winlist_open(true);
            step++;
            t0 = now;
        } else if (step == 3 && t > 1000) {
            ROWS(true);
            fprintf(stderr, "selftest: Minimized Window Viewer (%zu): %s\n", winlist_count(true), rows);
            char shot[PATH_MAX];
            if (getenv("WPS_SHELL_SHOT")) {
                snprintf(shot, sizeof shot, "%s-minimized.png", getenv("WPS_SHELL_SHOT"));
                winlist_snapshot(true, shot);
            }
            fprintf(stderr, "selftest: restore it: sent %d\n", winlist_command("activate", "Projects"));
            step++;
            t0 = now;
        } else if (step == 4 && t > 1500) {
            fprintf(stderr, "selftest: after restoring: %zu minimized\n", winlist_count(true));
            fprintf(stderr, "selftest: maximize Projects: sent %d\n", winlist_command("maximize", "Projects"));
            step++;
            t0 = now;
        } else if (step == 5 && t > 1500) {
            bool mn = false, mx = false;
            winlist_state("Projects", &mn, &mx);
            fprintf(stderr, "selftest: after maximizing: minimized %d maximized %d\n", mn, mx);
            fprintf(stderr, "selftest: restore Projects: sent %d\n", winlist_command("restore", "Projects"));
            step++;
            t0 = now;
        } else if (step == 6 && t > 1500) {
            bool mn = false, mx = false;
            winlist_state("Projects", &mn, &mx);
            fprintf(stderr, "selftest: after restoring: minimized %d maximized %d\n", mn, mx);
            /* another program (run.sh APP=), ended unasked */
            const char *other = getenv("WPS_SELFTEST_TERMINATE");
            if (other && !*other) other = NULL;
            if (other) fprintf(stderr, "selftest: terminate %s: sent %d\n", other, winlist_command("terminate", other));
            step++;
            t0 = now;
        } else if (step == 7 && t > 2000) {
            const char *other = getenv("WPS_SELFTEST_TERMINATE");
            if (other && !*other) other = NULL;
            bool mn, mx;
            if (other) fprintf(stderr, "selftest: after terminating: %s %s\n", other, winlist_state(other, &mn, &mx) ? "still there" : "gone");
            ui_app_quit(app);
            step++;
        }
        #undef ROWS
        return;
    }
    if (st && !strcmp(st, "live")) { selftest_live(f, now); return; }
    if (st && !strcmp(st, "close")) {
        if (f == folders) {   /* the first window closes itself */
            selftest_close(s, now);
            if (f->closing) ui_app_post(app, selftest_after_close, NULL);
        }
        return;
    }
    if (st) { selftest(s, now); return; }
    if (f->bench_secs <= 0) return;
    if (f->bench_start == 0) {
        if (!f->m.total) return;
        size_t n;
        const ui_frame_stat *st;
        ui_surface_stats(s, &st, &n);
        f->bench_start = now;
        f->bench_next_jump = now + 3000;
        f->bench_from = n;
    }
    /* Every 30 frames, check the picture against a full redraw. */
    if (getenv("WPS_SHELL_VERIFY") && ++f->frames_seen % 30 == 0) {
        size_t d = ui_surface_verify(s, f->verify_failed ? NULL : getenv("WPS_SHELL_VERIFY"));
        if (d != SIZE_MAX) {
            f->verified++;
            if (d) { f->verify_failed++; f->verify_diff += d; }
        }
    }
    if (now - f->bench_start > f->bench_secs * 1000) {
        report(f);
        f->bench_secs = 0;
        ui_surface_set_continuous(s, false);
        ui_app_quit(app);
        return;
    }
    if (now < f->bench_next_jump) ui_container_scroll_by(f->cont, 60);
    else {
        ui_container_scroll_to_fraction(f->cont, (double)(rand_r(&f->bench_seed) % 1000000) / 1e6);
        f->bench_next_jump = now + 500;
    }
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void report(struct folder *f)
{
    const ui_frame_stat *st;
    size_t n;
    ui_surface_stats(f->win, &st, &n);
    size_t from = f->bench_from + 1, m = n > from ? n - from : 0;
    double *d = malloc((m ? m : 1) * sizeof *d), *iv = malloc((m ? m : 1) * sizeof *iv), frac = 0;
    double *mv = malloc((m ? m : 1) * sizeof *mv);
    size_t fulls = 0;
    for (size_t i = 0; i < m; i++) {
        d[i] = st[from + i].draw_ms;
        mv[i] = st[from + i].move_ms;
        iv[i] = st[from + i].interval_ms;
        frac += st[from + i].drawn_fraction;
        fulls += st[from + i].full;
    }
    qsort(d, m, sizeof *d, cmp_d);
    qsort(iv, m, sizeof *iv, cmp_d);
    qsort(mv, m, sizeof *mv, cmp_d);
    double secs = (ui_now_ms() - f->bench_start) / 1000.0;
    size_t late = 0;
    double med = m ? iv[m / 2] : 0;
    for (size_t i = 0; i < m; i++) late += iv[i] > med * 1.5;
#define P(v, p) (m ? (v)[(size_t)((p) / 100.0 * (double)(m - 1) + 0.5)] : 0)
    printf("{\"folder\":\"%s\",\"total\":%zu,\"view\":\"%s\",\"scale\":%d,\"size\":[%.0f,%.0f],\"frames\":%zu,"
           "\"fps\":%.1f,\"draw_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},"
           "\"interval_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},\"late_frames\":%zu,"
           "\"full_redraws\":%zu,\"drawn_fraction_mean\":%.3f,\"move_ms\":{\"p50\":%.2f,\"p99\":%.2f},"
           "\"verified\":%zu,\"verify_failed\":%zu,\"verify_diff_px\":%zu}\n",
           f->path, f->m.total, ui_container_view(f->cont) == UI_VIEW_ICON ? "icon" : "details", ui_surface_scale(f->win),
           ui_surface_width(f->win), ui_surface_height(f->win), m, secs > 0 ? (double)m / secs : 0, P(d, 50), P(d, 99),
           m ? d[m - 1] : 0, P(iv, 50), P(iv, 99), m ? iv[m - 1] : 0, late, fulls, m ? frac / (double)m : 0, P(mv, 50), P(mv, 99), f->verified, f->verify_failed, f->verify_diff);
    fflush(stdout);
    free(d);
    free(iv);
    free(mv);
}

static const ui_surface_ops win_ops = {
    .draw = win_draw, .pointer = win_pointer, .key = win_key, .resized = win_resized,
    .drag_motion = win_drag_motion, .drag_leave = win_drag_leave, .drop = win_drop,
    .focus = win_focus, .close = win_close, .frame = win_frame,
};

static void got_title(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *t = res ? cv_text(res, "title") : NULL;
    if (!f || !t || !*t) return;
    snprintf(f->title, sizeof f->title, "%s", t);
    update_title(f);
}

static void notify_in_use(const char *path);

/* ------------------------------------------------------- settings objects */

/* The System, Sound, Mouse and Scheme Palette objects (G3): read at start
 * and whenever one changes (their notebooks), applied at once. */
static int64_t setup_sub;
static double next_w, next_h;   /* the next folder window's size (0: the default) */
static bool restore_windows = true;   /* System: "Restore open windows at login" */
static void windows_at_login(void);
/* The LaunchPad's and the WarpCenter's "Show at start" (automatic, yes, no). */
static char lp_show[16] = "automatic", wc_show[16] = "automatic";
static char scheme_name[32] = "classic";
static void apply_scheme(void);

static void got_setup(void *arg, const cv *reply);
static void user_dir(const char *key, const char *fallback, char *out, size_t n);
static void launchpad_open_folder(const char *path);
static void got_setup_sub(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && res->k == CV_INT) setup_sub = res->i;
}

static void apply_setup_object(const char *cls, const cv *settings)
{
    if (!settings || settings->k != CV_MAP) return;
    if (!strcmp(cls, "WPSound")) events_setup_begin();
    for (size_t i = 0; i < settings->n; i++) {
        const cv *k = &settings->items[2 * i], *v = &settings->items[2 * i + 1];
        if (k->k != CV_TEXT || v->k != CV_TEXT) continue;
        const char *key = k->s, *val = v->s;
        gnome_wps_value(cls, key, val);
        if (!strcmp(cls, "WPSound")) events_setup_set(key, val);
        else if (!strcmp(cls, "WPMouse")) {
            ui_mouse_set(key, val);
            char kv[160];   /* the compositor's too (WPS session): its drag pointer follows them */
            snprintf(kv, sizeof kv, "%s=%s", key, val);
            winlist_ext("mouse", kv);
        }
        else if (!strcmp(cls, "WPSchemePalette") && !strcmp(key, "scheme")) snprintf(scheme_name, sizeof scheme_name, "%s", val);
        else if (!strcmp(cls, "WPLaunchPad") && !strcmp(key, "show_at_start")) snprintf(lp_show, sizeof lp_show, "%s", val);
        else if (!strcmp(cls, "WPWarpCenter")) {
            if (!strcmp(key, "show_at_start")) snprintf(wc_show, sizeof wc_show, "%s", val);
            else warpcenter_setting(key, val);
        }
        else if (!strcmp(cls, "WPMessages")) messages_setting(key, val);
        else if (!strcmp(cls, "WPSystem")) {
            if (ui_scheme_set_option(key, val)) {   /* corners, buttons, border, shadows, translucency, title layout */
                char kv[160];   /* the compositor's frames too (WPS session) */
                snprintf(kv, sizeof kv, "%s=%s", key, val);
                winlist_ext("frame", kv);
            }
            else if (!strcmp(key, "confirm_delete")) confirm_delete = !strcmp(val, "1");
            else if (!strcmp(key, "confirm_folder_delete")) confirm_folder_delete = !strcmp(val, "1");
            else if (!strcmp(key, "restore_windows")) restore_windows = !strcmp(val, "1");
            else if (!strcmp(key, "show_hidden") && show_hidden != !strcmp(val, "1")) {
                show_hidden = !strcmp(val, "1");   /* every window lists again */
                for (struct folder *x = folders; x; x = x->next)
                    if (!x->closing) { if (is_tree(x)) tree_start(x); else reload(x); }
            }
        }
    }
}


static void show_warpcenter(void)
{
    if (warpcenter_is_open()) return;   /* (the bar: always in view) */
    for (struct folder *f = folders; f; f = f->next)
        if (f->desktop && !f->closing) {
            warpcenter_open(app, f->path, getenv("WPS_WARPCENTER_TOP") != NULL || warpcenter_top(), launchpad_open_folder);
            warpcenter_messages(messages_unread(), messages_open);
            return;
        }
}

/* Opened, or brought forward: never two (only the desktop's process
 * has one). */
static void show_launchpad(void)
{
    if (launchpad_is_open()) { launchpad_raise(); return; }
    bool desktop = false;
    for (struct folder *f = folders; f && !desktop; f = f->next) desktop = f->desktop && !f->closing;
    if (!desktop) return;
    char tdir[PATH_MAX];
    const char *t = getenv("WPS_TEMPLATES_DIR");
    if (t && *t) snprintf(tdir, sizeof tdir, "%s", t);
    else user_dir("TEMPLATES", "Templates", tdir, sizeof tdir);
    launchpad_open(app, launchpad_open_folder, tdir, setup_folder[0] ? setup_folder : NULL);
}

/* Restore Desktop: asked first; what was put back, said after. */
static void defaults_restored(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *got = res ? cv_get(res, "restored") : NULL;
    static const char *const ok[] = { "OK" };
    if (!res) {
        char m[512];
        snprintf(m, sizeof m, "The default objects could not be restored: %s",
                 reply && cv_text(reply, "message") ? cv_text(reply, "message") : "no answer from wpsd");
        ui_dialog_open(app, "Restore Desktop", m, ok, 1, 0, NULL, NULL);
        return;
    }
    char m[2048];
    size_t n = got && got->k == CV_ARRAY ? got->n : 0;
    if (!n) snprintf(m, sizeof m, "All the default objects are in place: nothing was changed.");
    else {
        snprintf(m, sizeof m, "Put back: ");
        for (size_t i = 0; i < n; i++) {
            const cv *t = &got->items[i];
            if (t->k != CV_TEXT) continue;
            size_t l = strlen(m);
            snprintf(m + l, sizeof m - l, "%s%s", i ? ", " : "", t->s);
        }
        strncat(m, ".", sizeof m - strlen(m) - 1);
    }
    ui_dialog_open(app, "Restore Desktop", m, ok, 1, 0, NULL, NULL);
}

static void restore_chosen(void *ctx, int choice)
{
    (void)ctx;
    if (choice != 0) return;
    const char *desk = NULL;
    for (struct folder *f = folders; f && !desk; f = f->next) if (f->desktop && !f->closing) desk = f->path;
    svc_restore_defaults(desk, defaults_restored, NULL);
}

static void restore_defaults_ask(void)
{
    static const char *const b[] = { "Restore", "Cancel" };
    ui_dialog_open(app, "Restore Desktop",
                   "Put the default objects back where they belong? System, Programs and the Trash Can on the desktop; "
                   "Templates, Startup, the Minimized Window Viewer and the settings objects in System. Ones moved "
                   "elsewhere or into the Trash Can are moved back; ones gone are made again. Nothing else changes.",
                   b, 2, 1, restore_chosen, NULL);
}

/* An open's reply that names a view (svc_reply_view). */
static bool open_view(const char *v)
{
    if (!strcmp(v, "messages")) messages_open();
    else if (!strcmp(v, "minimized-windows")) winlist_open(true);   /* the Minimized Window Viewer */
    else if (!strcmp(v, "launchpad")) show_launchpad();
    else if (!strcmp(v, "warpcenter")) show_warpcenter();
    else if (!strcmp(v, "restore-defaults")) restore_defaults_ask();
    else return false;
    return true;
}

static void got_setup(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *objs = res ? cv_get(res, "objects") : NULL;
    if (!objs || objs->k != CV_MAP) return;
    const char *ids[NSETUP];
    size_t n = 0;
    for (size_t i = 0; i < objs->n && i < NSETUP; i++) {
        const cv *cls = &objs->items[2 * i], *o = &objs->items[2 * i + 1];
        if (cls->k != CV_TEXT) continue;
        apply_setup_object(cls->s, cv_get(o, "settings"));
        const char *a = cv_text(o, "address");
        if (a) {
            snprintf(setup_ids[n], sizeof setup_ids[n], "%s", a);
            snprintf(setup_cls[n], sizeof setup_cls[n], "%s", cls->s);
            ids[n] = setup_ids[n];
            n++;
        }
    }
    apply_scheme();
    gnome_wps_read();   /* GNOME follows (or, the first time, leads) */
    for (struct folder *f = folders; f; f = f->next) if (!f->closing) ui_surface_damage_all(f->win);
    if (!setup_sub && n) svc_subscribe_objects(ids, n, got_setup_sub, NULL);   /* changes made in their notebooks */
    const char *folder = cv_text(res, "folder");
    /* The desktop's LaunchPad (G4), once System Setup is known (its first
     * buttons include it). WPS_NO_LAUNCHPAD: none (tests). */
    bool desktop = false;
    for (struct folder *f = folders; f && !desktop; f = f->next) desktop = f->desktop && !f->closing;
    /* In the WPS session the WarpCenter, and not the LaunchPad (the
     * owner's decision for 0.2; the LaunchPad stays in System). Inside
     * GNOME, which has its own bar, the LaunchPad as before. */
    /* The LaunchPad and WarpCenter objects' "Show at start" can say
     * otherwise (only at start: closing them is not offered). */
    const char *wcs = getenv("WPS_WARPCENTER");
    bool wpsession = getenv("WPS_SESSION") != NULL;
    bool want_wc = wcs ? !strcmp(wcs, "1") : wpsession && strcmp(wc_show, "no");
    bool want_lp = getenv("WPS_LAUNCHPAD") ? true : !strcmp(lp_show, "automatic") ? !wpsession : !strcmp(lp_show, "yes");
    if (wcs && !strcmp(wcs, "1")) want_lp = want_lp && (getenv("WPS_LAUNCHPAD") || !strcmp(lp_show, "yes"));
    static bool started;   /* (settings read again later change neither) */
    bool first = !started && desktop;
    if (desktop) started = true;
    if (folder) snprintf(setup_folder, sizeof setup_folder, "%s", folder);
    if (first && want_wc) show_warpcenter();
    if (first) windows_at_login();
    if (first && !getenv("WPS_NO_LAUNCHPAD") && want_lp) show_launchpad();
}

/* The Scheme Palette's scheme: colours for every window; the desktop's
 * picture is GNOME's either way. */
static void apply_scheme(void)
{
    if (!ui_scheme_use(scheme_name)) ui_scheme_use("classic");
    /* in the WPS session the compositor draws other programs' frames: the
     * same scheme and font (under GNOME the extension ignores these) */
    winlist_ext("scheme", ui_scheme_now.name);
    char font[300];
    snprintf(font, sizeof font, "%g %s", ui_scheme_now.font_px, ui_text_family());
    winlist_ext("font", font);
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing) continue;
        layout(f);   /* border and title sizes may have changed */
        ui_container_layout_changed(f->cont);
        ui_surface_damage_all(f->win);
    }
}

/* GNOME changed a shared setting: the settings object takes it. */
static void gnome_set_wps(const char *cls, const char *key, const char *value)
{
    for (int i = 0; i < NSETUP; i++)
        if (!strcmp(setup_cls[i], cls)) {
            cv v = { .k = CV_TEXT, .s = (char *)value, .len = strlen(value) };
            svc_settings_set(setup_ids[i], key, &v, -1, ignore_reply, NULL);
        }
}

/* GNOME's interface font changed: every window lays out again. */
static void gnome_font(void)
{
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing) continue;
        layout(f);
        ui_container_layout_changed(f->cont);
        ui_surface_damage_all(f->win);
    }
}

/* Is this event about one of the settings objects? Then read them again. */
static bool setup_event(const cv *ev)
{
    int64_t sub;
    if (!setup_sub || !cv_int(ev, "sub", &sub) || sub != setup_sub) return false;
    svc_setup(got_setup, NULL);
    return true;
}

/* What the toolkit reports (minimize, maximize, restore, menus). */
static void toolkit_action(const char *name, ui_surface *s, ui_rect at)
{
    struct folder *f = NULL;
    for (struct folder *o = folders; o && !f; o = o->next) if (o->win == s) f = o;
    events_raise(name, f ? f->path : "", !strncmp(name, "window.", 7) ? "folder" : "", at);
}

static struct folder *open_window(const char *path, ui_view view, bool desktop);
static struct folder *open_folder(const char *path, ui_view view) { return open_window(path, view, false); }
static void launchpad_open_folder(const char *path) { open_folder(path, UI_VIEW_ICON); }

static void desk_backdrop(void *ctx, cairo_t *cr, ui_rect clip) { (void)ctx; wallpaper_draw(cr, clip); }

static void desk_wallpaper_changed(void *ctx)
{
    struct folder *f = from_arg(ctx);
    if (f) TIMING(f, "background: %s", wallpaper_showing());
    if (f) ui_container_layout_changed(f->cont);   /* redraws the whole list area */
}

/* An XDG user folder (XDG_<key>_DIR in user-dirs.dirs), else ~/<fallback>. */
static void user_dir(const char *key, const char *fallback, char *out, size_t n)
{
    const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    char cfg[PATH_MAX], line[PATH_MAX + 64], want[64];
    if (xdg && *xdg) snprintf(cfg, sizeof cfg, "%s/user-dirs.dirs", xdg);
    else snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home ? home : "");
    snprintf(out, n, "%s/%s", home ? home : "", fallback);
    snprintf(want, sizeof want, "XDG_%s_DIR=\"", key);
    size_t wl = strlen(want);
    FILE *fp = fopen(cfg, "r");
    while (fp && fgets(line, sizeof line, fp)) {
        if (strncmp(line, want, wl)) continue;
        char *v = line + wl, *e = strrchr(v, '"');
        if (!e) break;
        *e = 0;
        if (!strncmp(v, "$HOME", 5)) snprintf(out, n, "%s%s", home ? home : "", v + 5);
        else if (v[0] == '/') snprintf(out, n, "%s", v);
        break;
    }
    if (fp) fclose(fp);
    mkdir(out, 0755);   /* a fresh account may not have one yet */
}

/* The desktop folder: XDG_DESKTOP_DIR, else ~/Desktop. */
static void desktop_dir(char *out, size_t n) { user_dir("DESKTOP", "Desktop", out, n); }

/* The desktop's first start (the Templates and System folders' shadows,
 * and 0.2's objects): wpsd does it, once each step, versioned. */
/* First-start step 7 (0.2): the Trash Can in the desktop's lower right
 * corner, as on OS/2. Done by the desktop, which knows its size: a moment
 * after the first start (the WarpCenter has taken its edge by then, and
 * the shadow is made), tried again a few times while it is not there. */
static int corner_timer = -1, corner_tries;

static bool trash_to_corner(void)
{
    struct folder *f = NULL;
    for (struct folder *o = folders; o && !f; o = o->next) if (o->desktop && !o->closing) f = o;
    if (!f || !f->m.complete) return false;
    for (size_t i = 0; i < f->m.total; i++) {
        struct row *rw = row_at(f, i);
        const char *nm = model_name(&f->m, i);
        if (!rw || !nm || !rw->shadow || !rw->target || !is_trash_dir(rw->target)) continue;
        ui_rect c = ui_frame_client(f->win);
        int64_t x = (int64_t)(c.w - UI_CELL_W - 8), y = (int64_t)(c.h - UI_CELL_H - 8);
        if (x < 0 || y < 0) return false;
        char addr[PATH_MAX];
        if (nm[0] == '\x01') snprintf(addr, sizeof addr, "%s", nm + 1);
        else snprintf(addr, sizeof addr, "%s/%s", f->path, nm);
        const char *t[1] = { addr };
        layout_place(f->lay, nm, (int)x, (int)y);
        svc_place(f->path, t, &x, &y, 1, placed_reply, as_arg(f));
        layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
        ui_container_layout_changed(f->cont);
        ui_surface_damage_all(f->win);
        return true;
    }
    return false;
}

static void on_corner_timer(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t x;
    if (read(fd, &x, sizeof x) < 0) {}
    if (trash_to_corner() || ++corner_tries >= 8) {
        ui_app_unwatch_fd(app, fd);
        close(fd);
        corner_timer = -1;
    }
}

static void first_started(void *arg, const cv *reply)
{
    (void)arg;
    for (struct folder *f = folders; f; f = f->next)
        if (f->desktop && !f->closing) bg_follow(f);   /* its Background (its own, or GNOME's) */
    if (!getenv("WPS_NO_STARTUP")) startup_at_login();
    int64_t from = -1;
    if (!reply || !cv_int(reply, "result", &from) || from >= 7 || corner_timer >= 0) return;
    corner_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (corner_timer < 0) return;
    struct itimerspec its = { { 1, 500000000 }, { 2, 0 } };
    timerfd_settime(corner_timer, 0, &its, NULL);
    ui_app_watch_fd(app, corner_timer, POLLIN, on_corner_timer, NULL);
}

static void desktop_first_start(const char *desk) { svc_first_start(desk, first_started, NULL); }

/* ---- open windows restored at login (v0.2-design 7.2)
 *
 * The desktop's own windows - folder windows (folder, view, size, place),
 * Find windows (folder, name), notebooks (object, page) - are written to
 * <state>/open-windows when they change (looked at every 2 s, so a crash
 * loses little; the end of a session closes none of them, so the list
 * stands as it was). At the next login (once a session) they open again;
 * in the WPS session the compositor puts each where it was ("place"),
 * inside GNOME GNOME places them. Lines: F path view w h x y maximized,
 * S folder name x y, N target page x y (x -1: not known). */
static int rec_timer = -1;
static char *rec_last;

static void open_windows_file(char *out, size_t n)
{
    const char *sd = getenv("WPS_STATE_DIR"), *xs = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char dir[PATH_MAX];
    if (sd && *sd) snprintf(dir, sizeof dir, "%s", sd);
    else if (xs && *xs) snprintf(dir, sizeof dir, "%s/wps", xs);
    else snprintf(dir, sizeof dir, "%s/.local/state/wps", home ? home : "");
    char *q = dir;
    while ((q = strchr(q + 1, '/'))) { *q = 0; mkdir(dir, 0700); *q = '/'; }
    mkdir(dir, 0700);
    snprintf(out, n, "%s/open-windows", dir);
}

static bool plain(const char *s) { return s && !strpbrk(s, "\t\n"); }

struct rec { char *buf; size_t len, cap; };

static void rec_add(struct rec *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char line[PATH_MAX * 2 + 128];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof line) return;
    if (r->len + (size_t)n + 1 > r->cap) {
        r->cap = (r->len + (size_t)n + 1) * 2;
        r->buf = realloc(r->buf, r->cap);
    }
    memcpy(r->buf + r->len, line, (size_t)n + 1);
    r->len += (size_t)n;
}

static void where_is(ui_surface *s, const char *app_id, int *x, int *y, bool *max)
{
    *x = *y = -1;
    *max = false;
    if (!winlist_where(app_id, ui_window_title(s), x, y, max)) *x = *y = -1;
}

static void rec_find(void *ctx, const char *folder, const char *name, ui_surface *s)
{
    int x, y;
    bool mx;
    where_is(s, "org.wps.Find", &x, &y, &mx);
    if (plain(folder) && plain(name)) rec_add(ctx, "S\t%s\t%s\t%d\t%d\n", folder, name, x, y);
}

static char *describe_windows(void)
{
    struct rec r = { 0 };
    rec_add(&r, "# WPS's open windows (the desktop writes this; they reopen at login)\n");
    /* oldest first: they reopen in the same order (the list is newest first) */
    size_t n = 0;
    for (struct folder *f = folders; f; f = f->next) n++;
    struct folder **all = calloc(n ? n : 1, sizeof *all);
    n = 0;
    for (struct folder *f = folders; f; f = f->next) all[n++] = f;
    for (size_t i = n; i-- > 0;) {
        struct folder *f = all[i];
        if (f->desktop || f->closing || !plain(f->path)) continue;
        int x, y;
        bool mx;
        where_is(f->win, "org.wps.shell", &x, &y, &mx);
        rec_add(&r, "F\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n", f->path, (int)ui_container_view(f->cont), (int)ui_surface_width(f->win),
                (int)ui_surface_height(f->win), x, y, mx || ui_window_maximized(f->win));
    }
    free(all);
    findwin_each(rec_find, &r);
    for (struct props *p = props_open; p; p = p->next) {
        if (!p->nb || !plain(p->target)) continue;
        int x, y;
        bool mx;
        where_is(ui_notebook_surface(p->nb), "org.wps.notebook", &x, &y, &mx);
        rec_add(&r, "N\t%s\t%zu\t%d\t%d\n", p->target, ui_notebook_page(p->nb), x, y);
    }
    return r.buf;
}

static void on_rec_timer(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t x;
    if (read(fd, &x, sizeof x) < 0) {}
    char *now = describe_windows();
    if (!now || (rec_last && !strcmp(now, rec_last))) { free(now); return; }
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    open_windows_file(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (f && fputs(now, f) >= 0 && fclose(f) == 0 && rename(tmp, path) == 0) { free(rec_last); rec_last = now; return; }
    if (f) unlink(tmp);
    free(now);
}

static void start_recording(void)
{
    if (rec_timer >= 0) return;
    rec_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (rec_timer < 0) return;
    struct itimerspec its = { { 2, 0 }, { 2, 0 } };
    timerfd_settime(rec_timer, 0, &its, NULL);
    ui_app_watch_fd(app, rec_timer, POLLIN, on_rec_timer, NULL);
}

static void place_next(const char *app_id, const char *title, int x, int y)
{
    if (x < 0 && y < 0) return;
    char arg[600];
    snprintf(arg, sizeof arg, "%s\t%s\t%d\t%d", app_id, plain(title) ? title : "", x, y);
    winlist_ext("place", arg);   /* (the WPS session's compositor; GNOME's extension ignores it) */
}

/* At login: what was open opens again (once a session), then the
 * recording goes on. */
static void windows_at_login(void)
{
    const char *rt = getenv("WPS_RUNTIME_DIR");
    if (!rt || !*rt) rt = getenv("XDG_RUNTIME_DIR");
    const char *sid = getenv("XDG_SESSION_ID");
    char marker[PATH_MAX], was[64] = "";
    snprintf(marker, sizeof marker, "%s/wps-windows-restored", rt && *rt ? rt : "/tmp");
    FILE *m = fopen(marker, "r");
    if (m) { if (!fgets(was, sizeof was, m)) was[0] = 0; fclose(m); }
    was[strcspn(was, "\n")] = 0;
    bool again = m && !strcmp(was, sid ? sid : "");   /* (the desktop started again in this session) */
    if ((m = fopen(marker, "w"))) { fprintf(m, "%s\n", sid ? sid : ""); fclose(m); }
    char path[PATH_MAX];
    open_windows_file(path, sizeof path);
    FILE *f = again || !restore_windows || getenv("WPS_NO_RESTORE") ? NULL : fopen(path, "r");
    char line[PATH_MAX * 2 + 128];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *fl[9] = { 0 };
        int k = 0;
        for (char *p = line, *t; k < 9 && (t = strsep(&p, "\t"));) fl[k++] = t;
        struct stat st;
        if (k >= 8 && !strcmp(fl[0], "F") && stat(fl[1], &st) == 0 && S_ISDIR(st.st_mode)) {
            int v = atoi(fl[2]);
            next_w = atof(fl[3]);
            next_h = atof(fl[4]);
            struct folder *nf = open_folder(fl[1], v == 1 ? UI_VIEW_DETAILS : v == 2 ? UI_VIEW_TREE : UI_VIEW_ICON);
            /* (asked once it has its title; it shows only after a round trip) */
            if (nf) place_next("org.wps.shell", ui_window_title(nf->win), atoi(fl[5]), atoi(fl[6]));
            if (nf && atoi(fl[7])) ui_window_set_maximized(nf->win);
        } else if (k >= 5 && !strcmp(fl[0], "S")) {
            place_next("org.wps.Find", "Find", atoi(fl[3]), atoi(fl[4]));
            findwin_open(fl[1], fl[2]);
        } else if (k >= 5 && !strcmp(fl[0], "N")) {
            place_next("org.wps.notebook", "", atoi(fl[3]), atoi(fl[4]));   /* (its title comes with its pages) */
            if (open_properties(fl[1])) props_open->want_page = (size_t)atoll(fl[2]);
        }
    }
    if (f) fclose(f);
    start_recording();   /* (also with restoring off: turned on again, it has the list) */
}

static struct folder *open_desktop(const char *path)
{
    char d[PATH_MAX];
    if (!path) { desktop_dir(d, sizeof d); path = d; }
    struct folder *f = open_window(path, UI_VIEW_ICON, true);
    desktop_first_start(f->path);
    return f;
}

static struct folder *open_window(const char *path, ui_view view, bool desktop)
{
    char real0[PATH_MAX];
    const char *want = realpath(path, real0) ? real0 : path;
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing && !strcmp(o->path, want) && ui_container_view(o->cont) == view && !desktop) {
            ui_window_activate(o->win);   /* already open in this view: switch to it, as OS/2 did */
            return o;
        }
    struct folder *f = calloc(1, sizeof *f);
    f->id = next_folder_id++;
    char real[PATH_MAX];
    snprintf(f->path, sizeof f->path, "%s", realpath(path, real) ? real : path);
    const char *base = strrchr(f->path, '/');
    snprintf(f->title, sizeof f->title, "%s", base && base[1] ? base + 1 : f->path);
    double w = view == UI_VIEW_ICON ? 640 : 760, h = 480;
    const char *sz = getenv("WPS_SHELL_SIZE");
    if (sz) sscanf(sz, "%lfx%lf", &w, &h);
    if (next_w > 0 && next_h > 0 && !desktop) { w = next_w; h = next_h; }   /* (reopened at login: its size) */
    next_w = next_h = 0;
    f->desktop = desktop;
    if (desktop) findwin_set_desktop(path);   /* Find's saved results go there */
    if (desktop) {
        /* Its app ID tells the GNOME Shell extension which window to make
         * GNOME's desktop (below everything, on every workspace). */
        snprintf(f->title, sizeof f->title, "Desktop");
        f->win = ui_window_new(app, &win_ops, f, "org.wps.Desktop", w, h);
        ui_window_set_frameless(f->win, true);
        ui_a11y_window(f->win, UI_ROLE_DESKTOP, NULL);   /* for screen readers: the desktop */
        ui_window_set_maximized(f->win);
    } else {
        f->win = ui_window_new(app, &win_ops, f, "org.wps.shell", w, h);
    }
    f->lay = layout_new(UI_CELL_W, UI_CELL_H);
    f->whole_titles = true;   /* (until its Icon view page says otherwise) */
    layout_set_height_fn(f->lay, lay_h, f);
    f->cont = ui_container_new(f->win, &folder_src, f, view);
    ui_container_set_whole_titles(f->cont, true);
    f->sort_choice = -1;
    ui_container_set_head_fns(f->cont, head_clicked, cols_resized, as_arg(f));
    if (desktop) {
        ui_container_set_backdrop(f->cont, desk_backdrop, f);
        wallpaper_start(app, desk_wallpaper_changed, as_arg(f));
    }
    if (view == UI_VIEW_TREE) tree_start(f);
    f->next = folders;
    folders = f;
    update_title(f);
    layout(f);
    if (!desktop) svc_resolve(f->path, got_title, as_arg(f));
    if (!desktop) raise_in(f, "window.open", NULL, "folder");
    notify_in_use(f->path);
    svc_subscribe_folder(f->path, got_sub, as_arg(f));
    if (!desktop) bg_follow(f);   /* (the desktop's after its first start: then wpsd knows it is the desktop) */
    svc_layout(f->path, got_layout, as_arg(f));
    request_more(f, false);
    return f;
}

/* Window events from the GNOME Shell extension, one line each on
 * standard input: event TAB app TAB title TAB x TAB y TAB w TAB h. */
static char ev_buf[8192];
static size_t ev_len;

static void on_window_events(void *arg, int fd, short rev)
{
    (void)arg;
    ssize_t n = read(fd, ev_buf + ev_len, sizeof ev_buf - 1 - ev_len);
    if (n <= 0) {
        if (n < 0 && errno == EAGAIN) return;
        ui_app_unwatch_fd(app, fd);   /* the extension went away */
        return;
    }
    (void)rev;
    ev_len += (size_t)n;
    ev_buf[ev_len] = 0;
    char *line = ev_buf, *nl;
    while ((nl = strchr(line, '\n'))) {
        *nl = 0;
        char *f[13] = { 0 };
        int k = 0;
        for (char *p = line, *t; k < 13 && (t = strsep(&p, "\t"));) f[k++] = t;
        if (k >= 7 && !strcmp(f[0], "W")) winlist_line(f, k);   /* the window list (G4) */
        else if (k >= 2 && !strcmp(f[0], "K") && !strcmp(f[1], "window-list")) winlist_open(false);   /* Ctrl+Esc */
        else if (k >= 5 && !strcmp(f[0], "I")) warpcenter_input_source(atoi(f[1]), atoi(f[2]), !strcmp(f[3], "ibus"), f[4]);
        else if (k == 7 && !strncmp(f[0], "window.", 7)) {
            ui_rect at = { atof(f[3]), atof(f[4]), atof(f[5]), atof(f[6]) };
            events_raise(f[0], f[1], "window", at);
        }
        line = nl + 1;
    }
    ev_len = strlen(line);
    memmove(ev_buf, line, ev_len + 1);
    if (ev_len == sizeof ev_buf - 1) ev_len = 0;   /* a line too long: dropped */
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("wps-shell %s\n", WPS_VERSION); return 0; }
    /* --events-stdin (anywhere: the extension adds it last) */
    bool events_stdin = false;
    for (int k = 1; k < argc; k++)
        if (!strcmp(argv[k], "--events-stdin")) {
            events_stdin = true;
            memmove(argv + k, argv + k + 1, (size_t)(argc - k) * sizeof *argv);
            argc--;
            k--;
        }
    int i = 1;
    bool desk = false;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--desktop")) desk = true;
        else if (!strcmp(argv[i], "--details")) opt_view = UI_VIEW_DETAILS;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) opt_bench = atof(argv[++i]);
        else { fprintf(stderr, "usage: wps-shell [--desktop [FOLDER]] [--details] [--bench SECONDS] [FOLDER...]\n"); return 2; }
    }
    if (i >= argc) desk = true;   /* no folder: the desktop */
    since_start();
    app = ui_app_new();
    load_system_conf();
    events_init();
    ui_set_action_hook(toolkit_action);
    if (events_stdin) {
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
        ui_app_watch_fd(app, 0, POLLIN, on_window_events, NULL);
        /* Standard output is the extension's command pipe now: anything
         * else printed goes to standard error. */
        int cmd = dup(1);
        dup2(2, 1);
        fcntl(cmd, F_SETFD, FD_CLOEXEC);
        winlist_init(app, cmd);
    } else {
        winlist_init(app, -1);
    }
    findwin_init(app, launchpad_open_folder, shell_drag_paths);
    icons_init(app);
    appicons_init(appicons_ready);
    iconpick_init(app, launchpad_open_folder);
    filepick_init(app);
    if (!app) return 1;
    char err[512];
    /* The service may be starting at the same moment (a session starts
     * both): wait for it a little before giving up. */
    bool up = false;
    for (int tries = 0; tries < 50 && !(up = svc_start(app, err, sizeof err)); tries++) {
        if (tries == 0) fprintf(stderr, "wps-shell: waiting for wpsd...\n");
        usleep(100000);
    }
    if (!up) {
        fprintf(stderr, "wps-shell: %s\n", err);
        fprintf(stderr, "wps-shell: start wpsd first (build/wpsd &), with the same WPS_RUNTIME_DIR%s%s\n",
                getenv("WPS_RUNTIME_DIR") ? ": " : " (unset here)", getenv("WPS_RUNTIME_DIR") ? getenv("WPS_RUNTIME_DIR") : "");
        return 1;
    }
    svc_on_event(on_event, NULL);
    svc_view_fn = open_view;
    svc_subscribe_jobs(ignore_reply, NULL);
    svc_setup(got_setup, NULL);
    /* Settings shared with GNOME: every shell follows its font; the
     * desktop keeps the rest in step both ways. */
    gnome_start(app, desk, gnome_set_wps, gnome_font);
    if (desk) {
        /* the desktop process: Messages, and in the WPS session the
         * notification service */
        messages_init(app);
        notify_init(app);
        /* --desktop FOLDER: that folder as the desktop (tests) */
        struct folder *f = open_desktop(i < argc ? argv[i++] : NULL);
        if (getenv("WPS_SHELL_SELFTEST")) ui_surface_set_continuous(f->win, true);
    }
    for (int k = i; k < argc; k++) {
        struct folder *f = open_folder(argv[k], opt_view);
        if (getenv("WPS_SHELL_SELFTEST")) ui_surface_set_continuous(f->win, true);   /* every window ticks */
        if (k == i && opt_bench > 0) {
            f->bench_secs = opt_bench;
            f->bench_seed = 12345;
            ui_surface_set_continuous(f->win, true);
        }
    }
    if (desk) events_raise("shell.startup", "", "", (ui_rect){ 0, 0, 0, 0 });
    ui_app_run(app);
    events_raise("shell.shutdown", "", "", (ui_rect){ 0, 0, 0, 0 });
    events_shutdown();
    gnome_stop();
    wallpaper_stop();
    while (folders) free_folder(folders);
    svc_stop();
    return 0;
}
