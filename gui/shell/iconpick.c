/* The icon picker (iconpick.h). */
#define _GNU_SOURCE
#include "iconpick.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "events.h"
#include "icons.h"

#define M 8
#define ROW 24
#define BTN_W 110
#define STATUS_H 20
#define PREVIEW 64
#define MAX_LOADING 4        /* previews being made at once */

struct icon {
    char *name, *path;
    int score;               /* themes: how well this file suits (the best one is kept) */
    int state;               /* preview: 0 not asked, 1 being made, 2 done (img, or none) */
    cairo_surface_t *img;
};

struct list { struct icon *v; size_t n, cap; };

enum { F_THEMES, F_LIBRARY, F_FIND, F_LIST, F_OPEN, F_COUNT };

static ui_app *app;
static void (*open_folder)(const char *path);

static struct picker {
    ui_surface *s;
    ui_container *cont;
    ui_entry *find;
    bool library;
    struct list themes, lib;
    bool themes_ready, scanning;
    size_t *rows, nrows;     /* the shown icons (indexes into the source), after the search */
    int focus;
    int pressed;             /* a button held: F_THEMES, F_LIBRARY, F_OPEN, or -1 */
    int dragging;
    unsigned gen;            /* changes on close: late previews and scans are dropped */
    int loading;
    iconpick_fn chosen;
    long owner;
    char key[64];
    char title[300];
    bool closing;
} pk;

/* --------------------------------------------------------------- lists */

static void list_add(struct list *l, const char *name, const char *path, int score)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = (struct icon){ strdup(name), strdup(path), score, 0, NULL };
}

static void list_free(struct list *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].name);
        free(l->v[i].path);
        if (l->v[i].img) cairo_surface_destroy(l->v[i].img);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const struct icon *)a)->name, ((const struct icon *)b)->name);
}

static bool image_ext(const char *n, bool any)
{
    const char *d = strrchr(n, '.');
    if (!d) return false;
    if (!strcasecmp(d, ".png") || !strcasecmp(d, ".svg")) return true;
    if (!any) return false;
    static const char *const more[] = { ".jpg", ".jpeg", ".ico", ".icns", ".webp", ".gif", ".bmp", ".jxl", ".avif", ".tif", ".tiff", ".xpm" };
    for (size_t i = 0; i < sizeof more / sizeof *more; i++) if (!strcasecmp(d, more[i])) return true;
    return false;
}

/* ------------------------------------------------------- the themes */

/* Every icon name once, with the file that suits best: a PNG drawn at 48
 * to 64 pixels, else a scalable SVG, else the nearest size. Symbolic
 * (single-colour) icons and cursors are left out. */
struct scan { struct list out; size_t cap; size_t *slot; unsigned gen; };

static uint64_t hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

static void consider(struct scan *sc, const char *name, const char *path, int score)
{
    if (sc->out.n * 2 >= sc->cap) {   /* grow the table */
        size_t nc = sc->cap ? sc->cap * 2 : 8192;
        size_t *ns = malloc(nc * sizeof *ns);
        for (size_t i = 0; i < nc; i++) ns[i] = SIZE_MAX;
        for (size_t i = 0; i < sc->out.n; i++) {
            size_t h = hash(sc->out.v[i].name) & (nc - 1);
            while (ns[h] != SIZE_MAX) h = (h + 1) & (nc - 1);
            ns[h] = i;
        }
        free(sc->slot);
        sc->slot = ns;
        sc->cap = nc;
    }
    size_t h = hash(name) & (sc->cap - 1);
    while (sc->slot[h] != SIZE_MAX) {
        struct icon *ic = &sc->out.v[sc->slot[h]];
        if (!strcmp(ic->name, name)) {
            if (score > ic->score) { free(ic->path); ic->path = strdup(path); ic->score = score; }
            return;
        }
        h = (h + 1) & (sc->cap - 1);
    }
    sc->slot[h] = sc->out.n;
    list_add(&sc->out, name, path, score);
}

static int size_score(const char *dir_part)
{
    int w = 0, hh = 0;
    if (!strcmp(dir_part, "scalable")) return 50;
    if (sscanf(dir_part, "%dx%d", &w, &hh) == 2 && w > 0) {
        if (w >= 48 && w <= 64) return 100;
        if (w > 64) return 70 - (w > 256 ? 20 : 0);
        return 20 + w / 2;
    }
    return -1;   /* not a size folder */
}

static void walk(struct scan *sc, const char *dir, int depth, int score)
{
    if (depth > 5) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX];
        if ((size_t)snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= sizeof p) continue;
        bool isdir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) { struct stat st; isdir = stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
        if (isdir) {
            if (!strcmp(e->d_name, "cursors") || strstr(e->d_name, "symbolic")) continue;
            int s = size_score(e->d_name);
            walk(sc, p, depth + 1, s >= 0 ? s : score);
            continue;
        }
        if (!image_ext(e->d_name, false) || strstr(e->d_name, "-symbolic")) continue;
        char name[256];
        snprintf(name, sizeof name, "%s", e->d_name);
        *strrchr(name, '.') = 0;
        bool svg = !strcasecmp(strrchr(e->d_name, '.'), ".svg");
        consider(sc, name, p, (score > 0 ? score : 30) + (svg ? 0 : 1));
    }
    closedir(d);
}

static void refilter(void);

/* The themes, read: kept for every picker after (they are the same). */
static void scan_done(void *arg)
{
    struct scan *sc = arg;
    list_free(&pk.themes);
    pk.themes = sc->out;
    pk.themes_ready = true;
    pk.scanning = false;
    if (pk.s && !pk.closing) refilter();
    free(sc->slot);
    free(sc);
}

static void *scan_thread(void *arg)
{
    struct scan *sc = arg;
    const char *x = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char user[PATH_MAX];
    if (x && *x) snprintf(user, sizeof user, "%s/icons", x);
    else snprintf(user, sizeof user, "%s/.local/share/icons", home ? home : "/nonexistent");
    const char *roots[] = { user, "/usr/share/icons", "/usr/local/share/icons" };
    for (size_t r = 0; r < sizeof roots / sizeof *roots; r++) walk(sc, roots[r], 0, 0);
    walk(sc, "/usr/share/pixmaps", 4, 40);
    qsort(sc->out.v, sc->out.n, sizeof *sc->out.v, by_name);
    ui_app_post(app, scan_done, sc);
    return NULL;
}

static void read_library(void)
{
    list_free(&pk.lib);
    const char *dir = icons_library_dir();
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !image_ext(e->d_name, true)) continue;
        char p[PATH_MAX], name[256];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        snprintf(name, sizeof name, "%s", e->d_name);
        list_add(&pk.lib, name, p, 0);
    }
    if (d) closedir(d);
    qsort(pk.lib.v, pk.lib.n, sizeof *pk.lib.v, by_name);
}

/* ---------------------------------------------------------- the model */

static struct list *src(void) { return pk.library ? &pk.lib : &pk.themes; }

static void refilter(void)
{
    struct list *l = src();
    const char *q = pk.find ? ui_entry_text(pk.find) : "";
    free(pk.rows);
    pk.rows = calloc(l->n ? l->n : 1, sizeof *pk.rows);
    pk.nrows = 0;
    for (size_t i = 0; i < l->n; i++)
        if (!*q || strcasestr(l->v[i].name, q)) pk.rows[pk.nrows++] = i;
    if (pk.cont) {
        ui_container_reset(pk.cont);
        ui_container_layout_changed(pk.cont);
    }
    if (pk.s) ui_surface_damage_all(pk.s);
}

struct pv { unsigned gen; bool library; size_t i; };

static void preview_made(void *ctx, const char *png)
{
    struct pv *v = ctx;
    if (pk.s && !pk.closing && v->gen == pk.gen) {
        pk.loading--;
        struct list *l = v->library ? &pk.lib : &pk.themes;
        if (v->i < l->n) {
            struct icon *ic = &l->v[v->i];
            ic->state = 2;
            if (png) {
                ic->img = cairo_image_surface_create_from_png(png);
                if (cairo_surface_status(ic->img) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(ic->img); ic->img = NULL; }
            }
            ui_container_reset(pk.cont);
        }
    }
    free(v);
}

static void want_preview(size_t row)
{
    struct list *l = src();
    if (row >= pk.nrows || pk.rows[row] >= l->n) return;
    struct icon *ic = &l->v[pk.rows[row]];
    if (ic->state || pk.loading >= MAX_LOADING) return;
    struct pv *v = calloc(1, sizeof *v);
    *v = (struct pv){ pk.gen, pk.library, pk.rows[row] };
    char out[PATH_MAX];
    if (icons_preview(ic->path, PREVIEW, out, sizeof out, preview_made, v)) {
        ic->state = 2;
        ic->img = cairo_image_surface_create_from_png(out);
        if (cairo_surface_status(ic->img) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(ic->img); ic->img = NULL; }
        free(v);
    } else {
        ic->state = 1;
        pk.loading++;
    }
}

static size_t s_count(void *ctx) { (void)ctx; return pk.nrows; }

static void s_item(void *ctx, size_t i, ui_item *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (i >= pk.nrows || pk.rows[i] >= src()->n) return;
    struct icon *ic = &src()->v[pk.rows[i]];
    if (!ic->state) want_preview(i);
    out->loaded = out->detailed = true;
    out->kind = UI_KIND_FILE;
    out->title = out->name = ic->name;
    out->image = ic->img;
    out->last_child = i + 1 == pk.nrows;
}

static void s_visible(void *ctx, size_t first, size_t last)
{
    (void)ctx;
    for (size_t i = first; i <= last && i < pk.nrows; i++) want_preview(i);
}

static void close_picker(void);

static void pick(size_t i)
{
    if (i >= pk.nrows || pk.rows[i] >= src()->n) return;
    struct icon *ic = &src()->v[pk.rows[i]];
    events_raise("icon.choose", ic->name, "iconpick", (ui_rect){ 0, 0, 0, 0 });
    if (pk.chosen) pk.chosen(pk.owner, pk.key, ic->path);
    close_picker();
}

static void s_activate(void *ctx, size_t i) { (void)ctx; pick(i); }

static const ui_container_src pick_src = { s_count, s_item, s_visible, s_activate, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* --------------------------------------------------------------- window */

struct rects { ui_rect themes, library, find_l, find, open, list, status; };

static struct rects rects(void)
{
    ui_rect c = ui_frame_client(pk.s);
    struct rects r;
    double y = c.y + M, x = c.x + M;
    r.themes = (ui_rect){ x, y, BTN_W, ROW };
    r.library = (ui_rect){ x + BTN_W + 4, y, BTN_W, ROW };
    r.find_l = (ui_rect){ x + 2 * BTN_W + 16, y, 40, ROW };
    double fx = r.find_l.x + 40, open_w = pk.library ? 150 : 0;
    r.find = (ui_rect){ fx, y, c.x + c.w - M - fx - (open_w ? open_w + 8 : 0), ROW };
    r.open = (ui_rect){ c.x + c.w - M - open_w, y, open_w, ROW };
    r.status = (ui_rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    double ly = y + ROW + M;
    r.list = (ui_rect){ c.x, ly, c.w, r.status.y - ly };
    return r;
}

static void focus_ring(cairo_t *cr, ui_rect r)
{
    ui_set_color(cr, ui_scheme_now.cursor);
    double dash = 1;
    cairo_save(cr);
    cairo_set_dash(cr, &dash, 1, 0);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, r.x + 3.5, r.y + 3.5, r.w - 7, r.h - 7);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void button(cairo_t *cr, ui_rect b, const char *t, bool down, bool on, bool focused)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_fill(cr, b, on ? k->selection : k->face);
    ui_bevel(cr, b, !(down || on));
    ui_set_color(cr, on ? k->selection_text : k->text);
    double tw = ui_text_width(UI_FONT_REGULAR, t);
    ui_text_draw(cr, UI_FONT_REGULAR, b.x + (b.w - tw) / 2, b.y + (b.h + ui_text_ascent()) / 2 - 1, t, b.w - 6);
    if (focused) focus_ring(cr, b);
}

static void pk_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s;
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(pk.s, cr, clip, pk.title);
    struct rects r = rects();
    ui_rect c = ui_frame_client(pk.s);
    ui_rect top = { c.x, c.y, c.w, r.list.y - c.y };
    if (ui_rect_meets(clip, top)) {
        ui_fill(cr, top, k->face);
        button(cr, r.themes, "Icon themes", pk.pressed == F_THEMES, !pk.library, pk.focus == F_THEMES);
        button(cr, r.library, "Icon library", pk.pressed == F_LIBRARY, pk.library, pk.focus == F_LIBRARY);
        ui_set_color(cr, k->text);
        ui_text_draw(cr, UI_FONT_REGULAR, r.find_l.x, r.find_l.y + (ROW + ui_text_ascent()) / 2 - 1, "Find:", 40);
        ui_entry_draw(pk.find, cr, r.find, pk.focus == F_FIND);
        if (pk.library) button(cr, r.open, "Open library folder", pk.pressed == F_OPEN, false, pk.focus == F_OPEN);
    }
    ui_container_draw(pk.cont, cr, clip);
    if (!ui_rect_meets(clip, r.status)) return;
    ui_fill(cr, r.status, k->face);
    ui_bevel(cr, (ui_rect){ r.status.x + 1, r.status.y + 1, r.status.w - 2, r.status.h - 2 }, false);
    char msg[PATH_MAX + 80];
    if (!pk.library && !pk.themes_ready) snprintf(msg, sizeof msg, "Reading the icon themes...");
    else if (pk.library && !pk.lib.n) snprintf(msg, sizeof msg, "No images in %s yet: put some there", icons_library_dir());
    else snprintf(msg, sizeof msg, "%zu icons; double-click one to use it", pk.nrows);
    ui_set_color(cr, k->text);
    cairo_save(cr);
    cairo_rectangle(cr, r.status.x + 2, r.status.y + 2, r.status.w - 4, r.status.h - 4);
    cairo_clip(cr);
    ui_text_draw(cr, UI_FONT_REGULAR, r.status.x + 6, r.status.y + (STATUS_H + ui_text_ascent()) / 2 - 1, msg, r.status.w - 12);
    cairo_restore(cr);
}

static void a11y(void)
{
    struct rects r = rects();
    ui_a11y_info i = { .index = pk.focus, .count = pk.library ? F_COUNT : F_COUNT - 1 };
    switch (pk.focus) {
    case F_THEMES: i.role = UI_ROLE_BUTTON; i.name = "Icon themes"; i.selected = !pk.library; i.at = r.themes; break;
    case F_LIBRARY: i.role = UI_ROLE_BUTTON; i.name = "Icon library"; i.selected = pk.library; i.at = r.library; break;
    case F_FIND: i.role = UI_ROLE_ENTRY; i.name = "Find"; i.text = ui_entry_text(pk.find); i.at = r.find; break;
    case F_OPEN: i.role = UI_ROLE_BUTTON; i.name = "Open library folder"; i.at = r.open; break;
    default: return;   /* the list says which icon */
    }
    ui_a11y_focus(pk.s, &i);
}

static void set_focus(int f)
{
    if (f == F_OPEN && !pk.library) f = F_THEMES;
    pk.focus = f;
    ui_container_set_focused(pk.cont, f == F_LIST);
    struct rects r = rects();
    ui_text_input_focus(pk.s, f == F_FIND ? pk.find : NULL, r.find);
    ui_surface_damage_all(pk.s);
    a11y();
}

static void set_source(bool library)
{
    if (library == pk.library) return;
    pk.library = library;
    if (library) read_library();
    refilter();   /* the shown rows are the new source's before anything is drawn */
    ui_container_layout(pk.cont, rects().list);
}

static void press(int which)
{
    if (which == F_THEMES) set_source(false);
    else if (which == F_LIBRARY) set_source(true);
    else if (which == F_OPEN && open_folder) open_folder(icons_library_dir());
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
        if (e->button != UI_BTN1) return;
        if (ui_rect_has(r.find, e->x, e->y)) {
            set_focus(F_FIND);
            pk.dragging = 1;
            ui_entry_pointer(pk.find, e, r.find);
            ui_surface_damage_all(s);
            return;
        }
        int b = ui_rect_has(r.themes, e->x, e->y) ? F_THEMES : ui_rect_has(r.library, e->x, e->y) ? F_LIBRARY
              : pk.library && ui_rect_has(r.open, e->x, e->y) ? F_OPEN : -1;
        if (b >= 0) { set_focus(b); pk.pressed = b; ui_surface_damage_all(s); }
        return;
    }
    if ((e->kind == UI_MOTION || e->kind == UI_RELEASE) && pk.dragging) {
        if (ui_entry_pointer(pk.find, e, r.find) != UI_ENTRY_NONE) ui_surface_damage_all(s);
        if (e->kind == UI_RELEASE) pk.dragging = 0;
    }
    if (e->kind == UI_RELEASE && pk.pressed >= 0) {
        int b = pk.pressed;
        pk.pressed = -1;
        ui_surface_damage_all(s);
        ui_rect br = b == F_THEMES ? r.themes : b == F_LIBRARY ? r.library : r.open;
        if (ui_rect_has(br, e->x, e->y)) press(b);
    }
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) ui_container_pointer(pk.cont, e);
}

static void pk_key(ui_surface *s, const ui_key_event *e)
{
    if (e->sym == XKB_KEY_Tab || e->sym == XKB_KEY_ISO_Left_Tab) {
        bool back = e->sym == XKB_KEY_ISO_Left_Tab || (e->mods & UI_MOD_SHIFT);
        int n = pk.library ? F_COUNT : F_COUNT - 1;
        set_focus((pk.focus + (back ? n - 1 : 1)) % n);
        return;
    }
    if (e->sym == XKB_KEY_Escape) { close_picker(); return; }
    bool enter = e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter;
    switch (pk.focus) {
    case F_FIND: {
        if (enter) { set_focus(F_LIST); return; }
        ui_entry_result r = ui_entry_key(pk.find, e);
        if (r == UI_ENTRY_CHANGED) refilter();   /* as you type */
        if (r != UI_ENTRY_NONE) { ui_surface_damage_all(s); a11y(); }
        return;
    }
    case F_LIST:
        ui_container_key(pk.cont, e);
        return;
    default:
        if (enter || e->sym == XKB_KEY_space) press(pk.focus);
    }
}

static void free_later(void *arg)
{
    (void)arg;
    ui_container_free(pk.cont);
    ui_entry_free(pk.find);
    ui_surface_destroy(pk.s);
    list_free(&pk.lib);
    free(pk.rows);
    /* the themes are kept: read again they would be the same */
    pk.s = NULL;
    pk.cont = NULL;
    pk.find = NULL;
    pk.rows = NULL;
    pk.nrows = 0;
    pk.closing = false;
}

static void close_picker(void)
{
    if (!pk.s || pk.closing) return;
    pk.closing = true;
    pk.gen++;
    pk.loading = 0;
    /* previews being made will be dropped: forget their state */
    for (size_t i = 0; i < pk.themes.n; i++) if (pk.themes.v[i].state == 1) pk.themes.v[i].state = 0;
    events_raise("window.close", "Choose an Icon", "window", (ui_rect){ 0, 0, 0, 0 });
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

void iconpick_init(ui_app *a, void (*of)(const char *path))
{
    app = a;
    open_folder = of;
    pk.pressed = -1;
}

void iconpick_open(const char *for_title, iconpick_fn chosen, long owner, const char *key)
{
    if (pk.s && !pk.closing) {   /* one at a time: it now picks for this */
        pk.chosen = chosen;
        pk.owner = owner;
        snprintf(pk.key, sizeof pk.key, "%s", key);
        ui_window_activate(pk.s);
        return;
    }
    pk.chosen = chosen;
    pk.owner = owner;
    pk.pressed = -1;
    pk.library = false;
    snprintf(pk.key, sizeof pk.key, "%s", key);
    snprintf(pk.title, sizeof pk.title, "Choose an Icon%s%s", for_title && *for_title ? " - " : "", for_title ? for_title : "");
    pk.find = ui_entry_new("");
    pk.s = ui_window_new(app, &pk_ops, NULL, "org.wps.IconPicker", 600, 460);
    ui_window_set_title(pk.s, pk.title);
    pk.cont = ui_container_new(pk.s, &pick_src, NULL, UI_VIEW_ICON);
    ui_container_layout(pk.cont, rects().list);
    events_raise("window.open", "Choose an Icon", "window", (ui_rect){ 0, 0, 0, 0 });
    if (!pk.themes_ready && !pk.scanning) {
        pk.scanning = true;
        struct scan *sc = calloc(1, sizeof *sc);
        sc->gen = pk.gen;
        pthread_t th;
        if (pthread_create(&th, NULL, scan_thread, sc) == 0) pthread_detach(th);
        else { free(sc); pk.scanning = false; }
    }
    refilter();
    set_focus(F_FIND);
}

/* ------------------------------------------------------------ self-tests */

bool iconpick_ready(void) { return pk.s && (pk.library || pk.themes_ready); }
size_t iconpick_count(void) { return pk.s ? pk.nrows : 0; }
const char *iconpick_name(size_t i) { return pk.s && i < pk.nrows && pk.rows[i] < src()->n ? src()->v[pk.rows[i]].name : NULL; }
bool iconpick_search(const char *t)
{
    if (!pk.s) return false;
    ui_entry_set_text(pk.find, t);
    refilter();
    return true;
}
bool iconpick_source(bool library) { if (!pk.s) return false; set_source(library); return true; }
bool iconpick_choose(size_t i) { if (!pk.s || i >= pk.nrows) return false; pick(i); return true; }
bool iconpick_snapshot(const char *png) { return pk.s && ui_surface_snapshot(pk.s, png); }
