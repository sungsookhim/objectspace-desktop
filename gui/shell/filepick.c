/* The file picker (filepick.h). */
#define _GNU_SOURCE
#include "filepick.h"

#include <dirent.h>
#include <limits.h>
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
#define UP_W 60
#define STATUS_H 20
#define PREVIEW 64
#define MAX_LOADING 4         /* previews being made at once */
#define MAX_ENTRIES 20000     /* a huge folder: the first ones */

struct ent {
    char *name;
    bool dir;
    int state;                /* preview: 0 not asked, 1 being made, 2 done (img, or none) */
    cairo_surface_t *img;
};

enum { F_UP, F_PATH, F_LIST, F_COUNT };

static ui_app *app;

static struct picker {
    ui_surface *s;
    ui_container *cont;
    ui_entry *path;
    char folder[PATH_MAX];
    char kind[16];
    struct ent *v;
    size_t n;
    bool truncated;
    int focus;
    bool up_pressed;
    int dragging;
    unsigned gen;             /* changes with the folder and on close: late previews are dropped */
    int loading;
    filepick_fn chosen;
    long owner;
    char key[64];
    char title[300];
    bool closing;
} pk;

/* ----------------------------------------------------------- the folder */

static bool has_ext(const char *n, const char *const *exts)
{
    const char *d = strrchr(n, '.');
    if (!d) return false;
    for (; *exts; exts++) if (!strcasecmp(d, *exts)) return true;
    return false;
}

static const char *const image_exts[] = { ".png", ".svg", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".jxl", ".avif",
                                          ".tif", ".tiff", ".ico", ".xpm", NULL };
static const char *const audio_exts[] = { ".ogg", ".oga", ".opus", ".wav", ".flac", ".mp3", NULL };

/* A file of the kind asked for. */
static bool wanted(const char *name)
{
    if (!strcmp(pk.kind, "image")) return has_ext(name, image_exts);
    if (!strcmp(pk.kind, "audio")) return has_ext(name, audio_exts);
    return true;
}

static bool is_image(const char *name) { return has_ext(name, image_exts); }

static void clear_entries(void)
{
    for (size_t i = 0; i < pk.n; i++) {
        free(pk.v[i].name);
        if (pk.v[i].img) cairo_surface_destroy(pk.v[i].img);
    }
    free(pk.v);
    pk.v = NULL;
    pk.n = 0;
}

static int by_kind_name(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;   /* folders first */
    return strcasecmp(x->name, y->name);
}

/* Read `dir` (hidden names left out), and show it. */
static bool read_folder(const char *dir)
{
    char real[PATH_MAX];
    if (!realpath(dir, real)) return false;
    DIR *d = opendir(real);
    if (!d) return false;
    clear_entries();
    pk.gen++;
    pk.loading = 0;
    pk.truncated = false;
    snprintf(pk.folder, sizeof pk.folder, "%s", real);
    size_t cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX + NAME_MAX + 2];
        snprintf(p, sizeof p, "%s/%s", strcmp(real, "/") ? real : "", de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        bool dir = S_ISDIR(st.st_mode);
        if (!dir && (!S_ISREG(st.st_mode) || !wanted(de->d_name))) continue;
        if (pk.n == MAX_ENTRIES) { pk.truncated = true; break; }
        if (pk.n == cap) {
            cap = cap ? cap * 2 : 256;
            pk.v = realloc(pk.v, cap * sizeof *pk.v);
        }
        pk.v[pk.n++] = (struct ent){ strdup(de->d_name), dir, 0, NULL };
    }
    closedir(d);
    qsort(pk.v, pk.n, sizeof *pk.v, by_kind_name);
    if (pk.path) ui_entry_set_text(pk.path, pk.folder);
    if (pk.cont) {
        ui_container_reset(pk.cont);
        ui_container_layout_changed(pk.cont);
        ui_container_set_cursor(pk.cont, 0);
    }
    if (pk.s) ui_surface_damage_all(pk.s);
    return true;
}

static void full_path(size_t i, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", strcmp(pk.folder, "/") ? pk.folder : "", pk.v[i].name);
}

/* ------------------------------------------------------------- previews */

struct pv { unsigned gen; size_t i; };

static void preview_made(void *ctx, const char *png)
{
    struct pv *v = ctx;
    if (pk.s && !pk.closing && v->gen == pk.gen && v->i < pk.n) {
        pk.loading--;
        struct ent *e = &pk.v[v->i];
        e->state = 2;
        if (png) {
            e->img = cairo_image_surface_create_from_png(png);
            if (cairo_surface_status(e->img) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(e->img); e->img = NULL; }
        }
        ui_container_reset(pk.cont);
    }
    free(v);
}

static void want_preview(size_t i)
{
    if (i >= pk.n) return;
    struct ent *e = &pk.v[i];
    if (e->state || e->dir || !is_image(e->name) || pk.loading >= MAX_LOADING) return;
    char p[PATH_MAX + NAME_MAX + 2], out[PATH_MAX];
    full_path(i, p, sizeof p);
    struct pv *v = calloc(1, sizeof *v);
    *v = (struct pv){ pk.gen, i };
    if (icons_preview(p, PREVIEW, out, sizeof out, preview_made, v)) {
        e->state = 2;
        e->img = cairo_image_surface_create_from_png(out);
        if (cairo_surface_status(e->img) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(e->img); e->img = NULL; }
        free(v);
    } else {
        e->state = 1;
        pk.loading++;
    }
}

/* ------------------------------------------------------------ the list */

static size_t s_count(void *ctx) { (void)ctx; return pk.n; }

static void s_item(void *ctx, size_t i, ui_item *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (i >= pk.n) return;
    struct ent *e = &pk.v[i];
    if (!e->state) want_preview(i);
    out->loaded = out->detailed = true;
    out->kind = e->dir ? UI_KIND_FOLDER : UI_KIND_FILE;
    out->title = out->name = e->name;
    out->image = e->img;
    out->last_child = i + 1 == pk.n;
}

static void s_visible(void *ctx, size_t first, size_t last)
{
    (void)ctx;
    for (size_t i = first; i <= last && i < pk.n; i++) want_preview(i);
}

static void close_picker(void);

static void pick_path(const char *p)
{
    events_raise("file.choose", p, "filepick", (ui_rect){ 0, 0, 0, 0 });
    filepick_fn fn = pk.chosen;
    long owner = pk.owner;
    char key[sizeof pk.key];
    snprintf(key, sizeof key, "%s", pk.key);
    close_picker();
    if (fn) fn(owner, key, p);
}

static void activate(size_t i)
{
    if (i >= pk.n) return;
    char p[PATH_MAX + NAME_MAX + 2];
    full_path(i, p, sizeof p);
    if (pk.v[i].dir) read_folder(p);
    else pick_path(p);
}

static void s_activate(void *ctx, size_t i) { (void)ctx; activate(i); }

static const ui_container_src pick_src = { s_count, s_item, s_visible, s_activate, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static void go_up(void)
{
    if (!strcmp(pk.folder, "/")) return;
    char up[PATH_MAX];
    snprintf(up, sizeof up, "%s", pk.folder);
    char *sl = strrchr(up, '/');
    if (sl == up) sl[1] = 0;
    else if (sl) *sl = 0;
    read_folder(up);
}

/* The path field's Enter: a folder is gone into, a file of the kind picked. */
static void path_entered(void)
{
    const char *t = ui_entry_text(pk.path);
    struct stat st;
    if (stat(t, &st) == 0 && S_ISDIR(st.st_mode)) { read_folder(t); return; }
    if (stat(t, &st) == 0 && S_ISREG(st.st_mode)) {
        const char *base = strrchr(t, '/');
        if (wanted(base ? base + 1 : t)) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s", t); pick_path(p); return; }
    }
    ui_entry_set_text(pk.path, pk.folder);   /* nothing there: back to the folder */
    ui_surface_damage_all(pk.s);
}

/* --------------------------------------------------------------- window */

struct rects { ui_rect up, path, list, status; };

static struct rects rects(void)
{
    ui_rect c = ui_frame_client(pk.s);
    struct rects r;
    double y = c.y + M, x = c.x + M;
    r.up = (ui_rect){ x, y, UP_W, ROW };
    r.path = (ui_rect){ x + UP_W + 8, y, c.x + c.w - M - (x + UP_W + 8), ROW };
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
        ui_fill(cr, r.up, k->face);
        ui_bevel(cr, r.up, !pk.up_pressed);
        ui_set_color(cr, strcmp(pk.folder, "/") ? k->text : k->disabled_text);
        double tw = ui_text_width(UI_FONT_REGULAR, "Up");
        ui_text_draw(cr, UI_FONT_REGULAR, r.up.x + (r.up.w - tw) / 2 + pk.up_pressed,
                     r.up.y + (ROW + ui_text_ascent()) / 2 - 1 + pk.up_pressed, "Up", r.up.w - 6);
        if (pk.focus == F_UP) focus_ring(cr, r.up);
        ui_entry_draw(pk.path, cr, r.path, pk.focus == F_PATH);
    }
    ui_container_draw(pk.cont, cr, clip);
    if (!ui_rect_meets(clip, r.status)) return;
    ui_fill(cr, r.status, k->face);
    ui_bevel(cr, (ui_rect){ r.status.x + 1, r.status.y + 1, r.status.w - 2, r.status.h - 2 }, false);
    char msg[160];
    size_t files = 0;
    for (size_t i = 0; i < pk.n; i++) files += !pk.v[i].dir;
    const char *what = !strcmp(pk.kind, "image") ? "picture" : !strcmp(pk.kind, "audio") ? "sound" : "file";
    snprintf(msg, sizeof msg, "%zu %s%s%s; double-click one to use it, a folder to open it", files, what, files == 1 ? "" : "s",
             pk.truncated ? " (the first ones)" : "");
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
    ui_a11y_info i = { .index = pk.focus, .count = F_COUNT };
    switch (pk.focus) {
    case F_UP: i.role = UI_ROLE_BUTTON; i.name = "Up"; i.at = r.up; break;
    case F_PATH: i.role = UI_ROLE_ENTRY; i.name = "Folder or file"; i.text = ui_entry_text(pk.path); i.at = r.path; break;
    default: return;   /* the list says which file */
    }
    ui_a11y_focus(pk.s, &i);
}

static void set_focus(int f)
{
    pk.focus = f;
    ui_container_set_focused(pk.cont, f == F_LIST);
    ui_text_input_focus(pk.s, f == F_PATH ? pk.path : NULL, rects().path);
    if (f == F_PATH) ui_entry_select_all(pk.path);
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
        if (e->button != UI_BTN1) return;
        if (ui_rect_has(r.path, e->x, e->y)) {
            if (pk.focus != F_PATH) set_focus(F_PATH);
            pk.dragging = 1;
            ui_entry_pointer(pk.path, e, r.path);
            ui_surface_damage_all(s);
            return;
        }
        if (ui_rect_has(r.up, e->x, e->y)) { set_focus(F_UP); pk.up_pressed = true; ui_surface_damage_all(s); }
        return;
    }
    if ((e->kind == UI_MOTION || e->kind == UI_RELEASE) && pk.dragging) {
        if (ui_entry_pointer(pk.path, e, r.path) != UI_ENTRY_NONE) ui_surface_damage_all(s);
        if (e->kind == UI_RELEASE) pk.dragging = 0;
    }
    if (e->kind == UI_RELEASE && pk.up_pressed) {
        pk.up_pressed = false;
        ui_surface_damage_all(s);
        if (ui_rect_has(r.up, e->x, e->y)) go_up();
    }
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) ui_container_pointer(pk.cont, e);
}

static void pk_key(ui_surface *s, const ui_key_event *e)
{
    if (e->sym == XKB_KEY_Tab || e->sym == XKB_KEY_ISO_Left_Tab) {
        bool back = e->sym == XKB_KEY_ISO_Left_Tab || (e->mods & UI_MOD_SHIFT);
        set_focus((pk.focus + (back ? F_COUNT - 1 : 1)) % F_COUNT);
        return;
    }
    if (e->sym == XKB_KEY_Escape) { close_picker(); return; }
    bool enter = e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter;
    if (e->sym == XKB_KEY_BackSpace && pk.focus != F_PATH) { go_up(); return; }
    if (e->sym == XKB_KEY_Up && (e->mods & UI_MOD_ALT)) { go_up(); return; }
    switch (pk.focus) {
    case F_PATH: {
        if (enter) { path_entered(); return; }
        if (ui_entry_key(pk.path, e) != UI_ENTRY_NONE) { ui_surface_damage_all(s); a11y(); }
        return;
    }
    case F_LIST:
        ui_container_key(pk.cont, e);
        return;
    default:
        if (enter || e->sym == XKB_KEY_space) go_up();
    }
}

static void free_later(void *arg)
{
    (void)arg;
    ui_container_free(pk.cont);
    ui_entry_free(pk.path);
    ui_surface_destroy(pk.s);
    clear_entries();
    pk.s = NULL;
    pk.cont = NULL;
    pk.path = NULL;
    pk.closing = false;
}

static void close_picker(void)
{
    if (!pk.s || pk.closing) return;
    pk.closing = true;
    pk.gen++;
    pk.loading = 0;
    events_raise("window.close", "Choose a File", "window", (ui_rect){ 0, 0, 0, 0 });
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

void filepick_init(ui_app *a) { app = a; }

void filepick_open(const char *for_title, const char *kind, const char *start, filepick_fn chosen, long owner,
                   const char *key)
{
    pk.chosen = chosen;
    pk.owner = owner;
    snprintf(pk.key, sizeof pk.key, "%s", key);
    snprintf(pk.kind, sizeof pk.kind, "%s", kind ? kind : "");
    const char *what = !strcmp(pk.kind, "image") ? "a Picture" : !strcmp(pk.kind, "audio") ? "a Sound" : "a File";
    snprintf(pk.title, sizeof pk.title, "Choose %s%s%s", what, for_title && *for_title ? " - " : "", for_title ? for_title : "");
    if (pk.s && !pk.closing) {   /* one at a time: it now picks for this */
        ui_window_set_title(pk.s, pk.title);
        read_folder(pk.folder);   /* (the kind may differ) */
        ui_window_activate(pk.s);
        return;
    }
    pk.up_pressed = false;
    pk.path = ui_entry_new("");
    pk.s = ui_window_new(app, &pk_ops, NULL, "org.wps.FilePicker", 600, 460);
    ui_window_set_title(pk.s, pk.title);
    pk.cont = ui_container_new(pk.s, &pick_src, NULL, UI_VIEW_ICON);
    ui_container_layout(pk.cont, rects().list);
    events_raise("window.open", "Choose a File", "window", (ui_rect){ 0, 0, 0, 0 });
    if (!read_folder(start && *start ? start : "/")) read_folder("/");
    set_focus(F_LIST);
}

/* ------------------------------------------------------------ self-tests */

bool filepick_is_open(void) { return pk.s && !pk.closing; }
const char *filepick_folder(void) { return pk.s ? pk.folder : NULL; }
size_t filepick_count(void) { return pk.s ? pk.n : 0; }
const char *filepick_name(size_t i) { return pk.s && i < pk.n ? pk.v[i].name : NULL; }
bool filepick_go(const char *folder) { return pk.s && read_folder(folder); }
bool filepick_activate(size_t i) { if (!pk.s || i >= pk.n) return false; activate(i); return true; }
bool filepick_snapshot(const char *png) { return pk.s && ui_surface_snapshot(pk.s, png); }
