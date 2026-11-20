/* The LaunchPad (launchpad.h). */
#define _GNU_SOURCE
#include "launchpad.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "events.h"
#include "svc.h"
#include "winlist.h"
#include "findwin.h"

#define GRIP_W 12.0
#define SYS_W 40.0           /* OS/2's own buttons */
#define SEP_W 8.0
#define BTN_W 56.0           /* an object's button */
#define ARROW_H 12.0         /* the drawer arrow above each object button */
#define BTN_H 56.0
#define PAD 3.0
#define NSYS 4
#define MAX_ITEMS 64
#define DRAWER_ROW 30.0
#define DRAWER_W 200.0

static const char *const sys_names[NSYS] = { "Lockup", "Find", "Shut down", "Window list" };
static const char *const sys_cmds[NSYS] = { "lock", "find", "shutdown", NULL };

struct entry {
    char name[NAME_MAX + 16];  /* as listed ("\x01vobj:<id>" for a central object) */
    char title[256];
    ui_kind kind;
    bool shadow, link;
    bool folder_target;        /* a shadow or link of a folder */
    char target[PATH_MAX];     /* what a shadow or link stands for */
};

struct drawer {
    ui_surface *s;
    size_t button;
    char dir[PATH_MAX];
    struct entry items[MAX_ITEMS];
    size_t n;
    bool loaded;
    ssize_t hot;
    ssize_t down;              /* the row pressed in it (the release of the
                                  click that opened it is not a choice) */
};

static struct {
    ui_app *app;
    void (*open_folder)(const char *path);
    ui_surface *s;
    char dir[PATH_MAX], drawers[PATH_MAX];
    struct entry items[MAX_ITEMS];
    size_t n;
    int64_t sub;
    ssize_t pressed;           /* a button held down (0..3 OS/2's, 4.. objects), -1 none */
    ssize_t drop_at;           /* where a drag is (object button, -2 the bar), -1 none */
    bool drop_arrow;
    struct drawer drawer;
    size_t kfocus;             /* the button the keyboard is on (0..3 OS/2's, 4.. objects) */
    bool focused;              /* the bar has the keyboard */
} lp = { .pressed = -1, .drop_at = -1 };

static void reload(void);
static void drawer_close(void);

/* -------------------------------------------------------------- geometry */

static double obj_x(size_t i) { return GRIP_W + NSYS * SYS_W + SEP_W + (double)i * BTN_W; }
static double bar_w(void) { return obj_x(lp.n ? lp.n : 1) + PAD; }
static double bar_h(void) { return ARROW_H + BTN_H + 2 * PAD; }

static ui_rect sys_rect(size_t i) { return (ui_rect){ GRIP_W + (double)i * SYS_W, ARROW_H + PAD, SYS_W - 2, BTN_H }; }
static ui_rect obj_rect(size_t i) { return (ui_rect){ obj_x(i), ARROW_H + PAD, BTN_W - 2, BTN_H }; }
static ui_rect arrow_rect(size_t i) { return (ui_rect){ obj_x(i), PAD, BTN_W - 2, ARROW_H - 1 }; }

static bool in(ui_rect r, double x, double y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* What is at (x, y): 0..3 OS/2's buttons, 4.. object buttons; *arrow for
 * a drawer arrow; -2 the grip; -1 nothing. */
static ssize_t hit(double x, double y, bool *arrow)
{
    *arrow = false;
    if (x < GRIP_W) return -2;
    for (size_t i = 0; i < NSYS; i++) if (in(sys_rect(i), x, y)) return (ssize_t)i;
    for (size_t i = 0; i < lp.n; i++) {
        if (in(obj_rect(i), x, y)) return (ssize_t)(NSYS + i);
        if (in(arrow_rect(i), x, y)) { *arrow = true; return (ssize_t)(NSYS + i); }
    }
    return -1;
}

static void address(const char *dir, const struct entry *e, char *out, size_t n)
{
    if (e->name[0] == '\x01') snprintf(out, n, "%s", e->name + 1);
    else snprintf(out, n, "%s/%s", dir, e->name);
}

/* A button's drawer: a folder named after it, beside the LaunchPad's. */
static void drawer_dir(size_t i, char *out, size_t n)
{
    const struct entry *e = &lp.items[i];
    const char *nm = e->name[0] == '\x01' ? e->name + 6 : e->name;   /* "vobj:<id>": its ID */
    snprintf(out, n, "%s/%s", lp.drawers, nm);
}

static void mkdirs(const char *path)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
}

/* --------------------------------------------------------------- listing */

static ui_kind kind_for(const char *type, const char *name)
{
    if (type && !strcmp(type, "folder")) return UI_KIND_FOLDER;
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (type && !strcmp(type, "program")) return UI_KIND_PROGRAM;
    if (dot && (!strcmp(dot, ".desktop") || !strcmp(dot, ".sh"))) return UI_KIND_PROGRAM;
    return UI_KIND_FILE;
}

static size_t parse(const cv *reply, struct entry *out, size_t max)
{
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    size_t n = 0;
    for (size_t i = 0; items && items->k == CV_ARRAY && i < items->n && n < max; i++) {
        const cv *it = &items->items[i];
        const char *nm = cv_text(it, "name"), *ti = cv_text(it, "title"), *ty = cv_text(it, "type");
        if (!nm || nm[0] == '.') continue;
        struct entry *e = &out[n++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", nm);
        snprintf(e->title, sizeof e->title, "%s", ti && *ti ? ti : nm[0] == '\x01' ? "" : nm);
        e->kind = kind_for(ty, nm);
        const cv *sh = cv_get(it, "shadow");
        e->shadow = sh && sh->k == CV_BOOL && sh->b;
        e->link = ty && !strcmp(ty, "link");
        const char *mk = cv_text(it, "makes");
        if (mk) e->kind = !strcmp(mk, "WPFolder") ? UI_KIND_FOLDER : !strcmp(mk, "WPProgram") ? UI_KIND_PROGRAM : UI_KIND_FILE;
        const char *tt = cv_text(it, "target"), *tp = cv_text(it, "target_path");
        if (e->shadow || e->link) {
            e->folder_target = tt && !strcmp(tt, "folder");
            e->kind = e->folder_target ? UI_KIND_FOLDER : kind_for("file", tp && *tp ? tp : nm);
            if (tp) snprintf(e->target, sizeof e->target, "%s", tp);
            if (!e->title[0] && tp && *tp) {
                const char *sl = strrchr(tp, '/');
                snprintf(e->title, sizeof e->title, "%s", sl && sl[1] ? sl + 1 : tp);
            }
        }
    }
    return n;
}

static void layout(void)
{
    if (!lp.s) return;
    ui_window_set_size(lp.s, bar_w(), bar_h(), true);
    ui_surface_damage_all(lp.s);
}

/* The buttons' order is the LaunchPad folder's icon positions (wpsd keeps
 * them, as for any folder): left to right by x. Objects with no position
 * (just added) go after the others, and every button gets x = its place
 * times a cell, so the order outlives the session and the folder, opened,
 * shows them in a row the same way. */
#define ORDER_CELL 100

static struct entry listed_items[MAX_ITEMS];
static size_t nlisted;
static int64_t order_x[MAX_ITEMS];

static int by_order(const void *a, const void *b)
{
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    int64_t x = order_x[i], y = order_x[j];
    if (x != y) return x < y ? -1 : 1;
    return i < j ? -1 : i > j;   /* listing order among equals */
}

static void placed(void *arg, const cv *reply) { (void)arg; (void)reply; }

static void got_order(void *arg, const cv *reply)
{
    (void)arg;
    if (!lp.s) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    for (size_t i = 0; i < nlisted; i++) {
        order_x[i] = INT64_MAX;   /* no position: at the end */
        for (size_t k = 0; items && items->k == CV_ARRAY && k < items->n; k++) {
            const char *nm = cv_text(&items->items[k], "name");
            int64_t x;
            if (nm && !strcmp(nm, listed_items[i].name) && cv_int(&items->items[k], "x", &x)) { order_x[i] = x; break; }
        }
    }
    size_t idx[MAX_ITEMS];
    for (size_t i = 0; i < nlisted; i++) idx[i] = i;
    qsort(idx, nlisted, sizeof *idx, by_order);
    /* positions that differ from place * cell are written back */
    char *t[MAX_ITEMS];
    int64_t px[MAX_ITEMS], py[MAX_ITEMS];
    size_t nplace = 0;
    for (size_t k = 0; k < nlisted; k++) {
        lp.items[k] = listed_items[idx[k]];
        /* (central objects keep their listing order: their layout names differ) */
        if (order_x[idx[k]] != (int64_t)k * ORDER_CELL && lp.items[k].name[0] != '\x01') {
            char a[PATH_MAX + 32];
            address(lp.dir, &lp.items[k], a, sizeof a);
            t[nplace] = strdup(a);
            px[nplace] = (int64_t)k * ORDER_CELL;
            py[nplace++] = 0;
        }
    }
    lp.n = nlisted;
    if (nplace) svc_place(lp.dir, (const char *const *)t, px, py, nplace, placed, NULL);
    for (size_t k = 0; k < nplace; k++) free(t[k]);
    layout();
}

static void got_list(void *arg, const cv *reply)
{
    (void)arg;
    if (!lp.s) return;
    nlisted = parse(reply, listed_items, MAX_ITEMS);
    svc_layout(lp.dir, got_order, NULL);
}

static void reload(void) { svc_list(lp.dir, 0, MAX_ITEMS, true, got_list, NULL); }

static void got_sub(void *arg, const cv *reply)
{
    (void)arg;
    const cv *r = reply ? cv_get(reply, "result") : NULL;
    if (r && r->k == CV_INT) lp.sub = r->i;
}

bool launchpad_event(const char *name, int64_t sub)
{
    if (!lp.s || !lp.sub || sub != lp.sub) return false;
    if (strcmp(name, "folder_moved")) reload();
    return true;
}

/* ---------------------------------------------------------------- acting */

static void opened(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && cv_text(res, "folder")) { lp.open_folder(cv_text(res, "folder")); return; }
    const char *msg = reply ? cv_text(reply, "message") : "lost the connection to wpsd";
    if (!res && msg) fprintf(stderr, "wps-shell: LaunchPad: %s\n", msg);
}

static void open_entry(const char *dir, const struct entry *e)
{
    char a[PATH_MAX + 32];
    address(dir, e, a, sizeof a);
    events_raise("object.open", e->title, "launchpad", (ui_rect){ 0, 0, 0, 0 });
    if (e->kind == UI_KIND_FOLDER && !e->shadow && !e->link && e->name[0] != '\x01') lp.open_folder(a);
    else svc_open(a, false, opened, NULL);
}

/* Shut down: OS/2 asked before shutting down; a session of today can also
 * end with the machine left on (log off) or restart it. */
static const char *const end_how[] = { "poweroff", "reboot", "logout" };

static void end_chosen(void *ctx, int choice)
{
    (void)ctx;
    if (choice < 0 || choice > 2) return;   /* Cancel */
    events_raise("session.end", end_how[choice], "launchpad", (ui_rect){ 0, 0, 0, 0 });
    if (!winlist_ext("shutdown", end_how[choice]))
        fprintf(stderr, "wps-shell: LaunchPad: Shut down needs the WPS GNOME Shell extension\n");
}

static void ask_end(void)
{
    static const char *const buttons[] = { "Shut down", "Restart", "Log off", "Cancel" };
    ui_dialog_open(lp.app, "Shut Down", "End this session: shut the computer down, restart it, or log off?", buttons, 4,
                   0, end_chosen, NULL);
}

static void press_sys(size_t i)
{
    events_raise("launchpad.button", sys_names[i], "launchpad", (ui_rect){ 0, 0, 0, 0 });
    if (i == 3) { winlist_open(false); return; }
    if (i == 2) { ask_end(); return; }
    if (i == 1) {   /* WPS's own Find, from the home folder */
        const char *home = getenv("HOME");
        findwin_open(home && *home ? home : "/", NULL);
        return;
    }
    if (!winlist_ext(sys_cmds[i], NULL))
        fprintf(stderr, "wps-shell: LaunchPad: %s needs the WPS GNOME Shell extension\n", sys_names[i]);
}

static void removed(void *arg, const cv *reply)
{
    (void)arg;
    if (!reply || !cv_get(reply, "result"))
        fprintf(stderr, "wps-shell: LaunchPad: cannot remove: %s\n", reply ? cv_text(reply, "message") : "no wpsd");
    reload();
}

struct sel_ctx { char dir[PATH_MAX]; char name[NAME_MAX + 16]; };

static void got_sel(void *arg, const cv *reply)
{
    struct sel_ctx *c = arg;
    const cv *r = reply ? cv_get(reply, "result") : NULL;
    if (r && r->k == CV_INT) svc_job_start("trash", r->i, NULL, NULL, NULL, NULL, 0, removed, NULL);
    free(c);
}

/* Off the LaunchPad: a shadow goes (its original stays); anything else
 * put in its folder goes to the trash. */
static void remove_entry(const char *dir, const struct entry *e)
{
    if (e->name[0] == '\x01') { svc_vdelete(e->name + 1, removed, NULL); return; }
    struct sel_ctx *c = calloc(1, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    snprintf(c->name, sizeof c->name, "%s", e->name);
    const char *names[] = { c->name };
    svc_select(c->dir, names, 1, got_sel, c);
}

/* Already a button there (the LaunchPad's own row only: drawers are read
 * when opened). */
static bool has(const char *dir, const char *path)
{
    if (strcmp(dir, lp.dir)) return false;
    for (size_t i = 0; i < lp.n; i++) if (!strcmp(lp.items[i].target, path)) return true;
    return false;
}

/* Shadows of what was dropped, in `dir` (the LaunchPad or a drawer). */
static void add_paths(const char *dir, const char *data, size_t len)
{
    mkdirs(dir);
    const char *p = data, *end = data + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[PATH_MAX + 64];
        snprintf(line, sizeof line, "%.*s", (int)l, p);
        line[strcspn(line, "\t\r")] = 0;
        char path[PATH_MAX + 64];
        if (!strncmp(line, "file://", 7)) {
            /* a URI: file:///a%20b */
            const char *u = line + 7;
            if (!strncmp(u, "localhost", 9)) u += 9;
            char *o = path;
            for (; *u && o < path + sizeof path - 1; u++) {
                if (*u == '%' && u[1] && u[2]) { char h[3] = { u[1], u[2], 0 }; *o++ = (char)strtol(h, NULL, 16); u += 2; }
                else *o++ = *u;
            }
            *o = 0;
        } else {
            snprintf(path, sizeof path, "%s", line);
        }
        if ((path[0] == '/' || !strncmp(path, "vobj:", 5)) && !has(dir, path)) svc_shadow(path, dir, removed, NULL);
        p += l + 1;
    }
}

/* ---------------------------------------------------------------- drawer */

static void drawer_layout(void)
{
    struct drawer *d = &lp.drawer;
    double h = (double)(d->n ? d->n : 1) * DRAWER_ROW + 2 * PAD;
    ui_popup_set_size(d->s, DRAWER_W, h);
}

static void drawer_got(void *arg, const cv *reply)
{
    (void)arg;
    struct drawer *d = &lp.drawer;
    if (!d->s) return;
    d->n = parse(reply, d->items, MAX_ITEMS);
    d->loaded = true;
    drawer_layout();
    ui_surface_damage_all(d->s);
}

static void drawer_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)clip;
    struct drawer *d = ui_surface_data(s);
    const ui_scheme *k = &ui_scheme_now;
    ui_rect all = { 0, 0, DRAWER_W, (double)(d->n ? d->n : 1) * DRAWER_ROW + 2 * PAD };
    ui_fill(cr, all, k->face);
    ui_bevel(cr, all, true);
    if (!d->n) {
        ui_set_color(cr, k->disabled_text);
        ui_text_draw(cr, UI_FONT_REGULAR, 8, PAD + (DRAWER_ROW + ui_text_ascent()) / 2 - 1,
                     d->loaded ? "Empty: drop objects on the arrow" : "...", DRAWER_W - 16);
        return;
    }
    for (size_t i = 0; i < d->n; i++) {
        ui_rect r = { PAD, PAD + (double)i * DRAWER_ROW, DRAWER_W - 2 * PAD, DRAWER_ROW };
        if ((ssize_t)i == d->hot) ui_fill(cr, r, k->menu_hilite);
        ui_item it = { .loaded = true, .detailed = true, .kind = d->items[i].kind, .shadow = d->items[i].shadow,
                       .link = d->items[i].link };
        ui_draw_item_icon(cr, &it, r.x + 3, r.y + 3, 24);
        ui_set_color(cr, (ssize_t)i == d->hot ? k->menu_hilite_text : k->text);
        ui_text_draw(cr, UI_FONT_REGULAR, r.x + 32, r.y + (DRAWER_ROW + ui_text_ascent()) / 2 - 1, d->items[i].title,
                     r.w - 36);
    }
}

static void drawer_pointer(ui_surface *s, const ui_pointer_event *e)
{
    struct drawer *d = ui_surface_data(s);
    double h = (double)(d->n ? d->n : 1) * DRAWER_ROW + 2 * PAD;
    ssize_t row = e->y >= PAD && e->y < h - PAD ? (ssize_t)((e->y - PAD) / DRAWER_ROW) : -1;
    if (row >= (ssize_t)d->n || e->x < 0 || e->x >= DRAWER_W) row = -1;
    if (e->kind == UI_MOTION || e->kind == UI_ENTER) {
        if (row != d->hot) { d->hot = row; ui_surface_damage_all(s); }
    } else if (e->kind == UI_LEAVE) {
        d->hot = -1;
        ui_surface_damage_all(s);
    } else if (e->kind == UI_PRESS && e->button == UI_BTN1) {
        d->down = row;
    } else if (e->kind == UI_RELEASE && e->button == UI_BTN1 && row >= 0 && row == d->down) {
        struct entry it = d->items[row];
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", d->dir);
        drawer_close();
        open_entry(dir, &it);
    }
}

/* A closed drawer's popup, until it is destroyed: not inside its own
 * event, but before any new popup (Wayland: a new grabbing popup must be
 * the child of the topmost one; another arrow clicked with a drawer open
 * was a protocol error, and GNOME disconnected the desktop). */
static ui_surface *dying;

static void drawer_free(void *arg)
{
    (void)arg;
    if (dying) ui_surface_destroy(dying);
    dying = NULL;
}

static void drawer_close(void)
{
    struct drawer *d = &lp.drawer;
    if (!d->s) return;
    drawer_free(NULL);   /* an older one first */
    dying = d->s;
    d->s = NULL;
    ui_app_post(lp.app, drawer_free, NULL);
    if (lp.s) ui_surface_damage_all(lp.s);
}

static void drawer_dismissed(ui_surface *s) { (void)s; drawer_close(); }

static const ui_surface_ops drawer_ops = { .draw = drawer_draw, .pointer = drawer_pointer, .dismissed = drawer_dismissed };

static void drawer_open(size_t i, uint32_t serial)
{
    bool same = lp.drawer.s && lp.drawer.button == i;
    drawer_close();
    drawer_free(NULL);   /* gone now, not later: the new popup's parent must be the topmost */
    if (same) return;   /* its arrow again: closed */
    struct drawer *d = &lp.drawer;
    memset(d, 0, sizeof *d);
    d->button = i;
    d->hot = d->down = -1;
    drawer_dir(i, d->dir, sizeof d->dir);
    mkdirs(d->dir);
    /* above its button, as OS/2's drawers opened (below if there is no room) */
    d->s = ui_popup_new_above(lp.app, lp.s, &drawer_ops, d, arrow_rect(i), DRAWER_W, DRAWER_ROW + 2 * PAD, serial);
    svc_list(d->dir, 0, MAX_ITEMS, true, drawer_got, NULL);
    events_raise("menu.open", lp.items[i].title, "launchpad", (ui_rect){ 0, 0, 0, 0 });
    ui_surface_damage_all(lp.s);
}

/* ------------------------------------------------------------------ bar */

static void draw_glyph(cairo_t *cr, size_t i, ui_rect r)
{
    double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    cairo_save(cr);
    cairo_set_line_width(cr, 2);
    switch (i) {
    case 0:   /* Lockup: a padlock */
        cairo_set_source_rgb(cr, 0.75, 0.6, 0.1);
        cairo_rectangle(cr, cx - 9, cy - 2, 18, 13);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
        cairo_arc(cr, cx, cy - 4, 6, M_PI, 2 * M_PI);
        cairo_move_to(cr, cx - 6, cy - 4); cairo_line_to(cr, cx - 6, cy - 2);
        cairo_move_to(cr, cx + 6, cy - 4); cairo_line_to(cr, cx + 6, cy - 2);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_arc(cr, cx, cy + 4, 2, 0, 2 * M_PI);
        cairo_fill(cr);
        break;
    case 1:   /* Find: a magnifying glass */
        cairo_set_source_rgb(cr, 0.7, 0.85, 1);
        cairo_arc(cr, cx - 3, cy - 3, 7, 0, 2 * M_PI);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.3);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 4);
        cairo_move_to(cr, cx + 2, cy + 2); cairo_line_to(cr, cx + 10, cy + 10);
        cairo_stroke(cr);
        break;
    case 2:   /* Shut down: the power mark */
        cairo_set_source_rgb(cr, 0.75, 0.1, 0.1);
        cairo_set_line_width(cr, 3);
        cairo_arc(cr, cx, cy + 1, 9, -M_PI / 2 + 0.6, 3 * M_PI / 2 - 0.6);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, cy - 11); cairo_line_to(cr, cx, cy);
        cairo_stroke(cr);
        break;
    case 3:   /* Window list: windows stacked */
        for (int k = 0; k < 2; k++) {
            ui_rect w = { cx - 11 + k * 6, cy - 9 + k * 6, 16, 12 };
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_rectangle(cr, w.x, w.y, w.w, w.h);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 0, 0, 0.5);
            cairo_rectangle(cr, w.x, w.y, w.w, 3);
            cairo_fill(cr);
            cairo_set_line_width(cr, 1);
            cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
            cairo_rectangle(cr, w.x + 0.5, w.y + 0.5, w.w - 1, w.h - 1);
            cairo_stroke(cr);
        }
        break;
    }
    cairo_restore(cr);
}

static void focus_ring(cairo_t *cr, ui_rect r)
{
    ui_set_color(cr, ui_scheme_now.cursor);
    double dash = 1;
    cairo_save(cr);
    cairo_set_dash(cr, &dash, 1, 0);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void lp_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s; (void)clip;
    const ui_scheme *k = &ui_scheme_now;
    ui_rect all = { 0, 0, bar_w(), bar_h() };
    ui_fill(cr, all, k->face);
    ui_bevel(cr, all, true);
    /* the grip: it moves the LaunchPad */
    for (double y = 6; y < all.h - 6; y += 4) {
        ui_fill(cr, (ui_rect){ 4, y, 2, 2 }, k->light);
        ui_fill(cr, (ui_rect){ 5, y + 1, 2, 2 }, k->shadow);
    }
    for (size_t i = 0; i < NSYS; i++) {
        ui_rect r = sys_rect(i);
        bool down = lp.pressed == (ssize_t)i;
        ui_bevel(cr, r, !down);
        if (down) cairo_translate(cr, 1, 1);
        draw_glyph(cr, i, (ui_rect){ r.x, r.y, r.w, r.h - 2 });
        if (down) cairo_translate(cr, -1, -1);
        if (lp.focused && lp.kfocus == i) focus_ring(cr, (ui_rect){ r.x + 3, r.y + 3, r.w - 6, r.h - 6 });
    }
    double sx = GRIP_W + NSYS * SYS_W + SEP_W / 2 - 1;
    ui_fill(cr, (ui_rect){ sx, ARROW_H + PAD + 2, 1, BTN_H - 4 }, k->shadow);
    ui_fill(cr, (ui_rect){ sx + 1, ARROW_H + PAD + 2, 1, BTN_H - 4 }, k->light);
    if (!lp.n) {
        ui_rect r = obj_rect(0);
        ui_set_color(cr, k->disabled_text);
        ui_text_draw(cr, UI_FONT_REGULAR, r.x + 2, r.y + r.h / 2 + 4, "Drop here", r.w - 4);
    }
    for (size_t i = 0; i < lp.n; i++) {
        ui_rect r = obj_rect(i), a = arrow_rect(i);
        bool down = lp.pressed == (ssize_t)(NSYS + i), target = lp.drop_at == (ssize_t)(NSYS + i);
        bool open = lp.drawer.s && lp.drawer.button == i;
        /* the drawer arrow */
        if ((target && lp.drop_arrow) || open) ui_fill(cr, a, k->selection);
        ui_set_color(cr, (target && lp.drop_arrow) || open ? k->selection_text : k->text);
        double ax = a.x + a.w / 2, ay = a.y + a.h / 2;
        cairo_move_to(cr, ax - 5, ay + 2);
        cairo_line_to(cr, ax + 5, ay + 2);
        cairo_line_to(cr, ax, ay - 3);
        cairo_close_path(cr);
        cairo_fill(cr);
        if (target && !lp.drop_arrow) ui_fill(cr, r, k->selection);
        ui_bevel(cr, r, !down);
        ui_item it = { .loaded = true, .detailed = true, .kind = lp.items[i].kind, .shadow = lp.items[i].shadow,
                       .link = lp.items[i].link };
        double d = down ? 1 : 0;
        ui_draw_item_icon(cr, &it, r.x + (r.w - 32) / 2 + d, r.y + 4 + d, 32);
        if (lp.focused && lp.kfocus == NSYS + i) focus_ring(cr, (ui_rect){ r.x + 3, r.y + 3, r.w - 6, r.h - 6 });
        ui_set_color(cr, k->text);
        double tw = ui_text_width(UI_FONT_REGULAR, lp.items[i].title);
        double tx = tw < r.w - 4 ? r.x + (r.w - tw) / 2 : r.x + 2;
        cairo_save(cr);
        cairo_rectangle(cr, r.x + 1, r.y, r.w - 2, r.h);
        cairo_clip(cr);
        ui_text_draw(cr, UI_FONT_REGULAR, tx + d, r.y + r.h - 6 + d, lp.items[i].title, r.w - 4);
        cairo_restore(cr);
    }
}

struct menu_ctx { size_t i; };

static void menu_chosen(void *arg, int64_t id)
{
    size_t i = (size_t)(intptr_t)arg;
    if (id < 0 || i >= lp.n) return;
    struct entry e = lp.items[i];
    if (id == 1) open_entry(lp.dir, &e);
    else if (id == 2) remove_entry(lp.dir, &e);
    else if (id == 3) lp.open_folder(lp.dir);
}

static void bar_menu_chosen(void *arg, int64_t id)
{
    (void)arg;
    if (id == 3) lp.open_folder(lp.dir);
    else if (id == 4) winlist_open(false);
}

static void button_menu(ui_surface *s, size_t h, double x, double y, uint32_t serial);

static void lp_pointer(ui_surface *s, const ui_pointer_event *e)
{
    bool arrow;
    ssize_t h = hit(e->x, e->y, &arrow);
    if (e->kind == UI_PRESS && e->button == UI_BTN1) {
        if (h == -2) { ui_window_move(s, e->serial); return; }
        if (h >= NSYS && arrow) { drawer_open((size_t)h - NSYS, e->serial); return; }
        lp.pressed = h >= 0 ? h : -1;
        ui_surface_damage_all(s);
    } else if (e->kind == UI_RELEASE && e->button == UI_BTN1) {
        ssize_t was = lp.pressed;
        lp.pressed = -1;
        ui_surface_damage_all(s);
        if (was < 0 || was != h || arrow) return;
        if (was < NSYS) press_sys((size_t)was);
        else open_entry(lp.dir, &lp.items[was - NSYS]);
    } else if (e->kind == UI_PRESS && e->button == UI_BTN2) {
        button_menu(s, h >= NSYS && !arrow ? (size_t)h : SIZE_MAX, e->x, e->y, e->serial);
    } else if (e->kind == UI_LEAVE && lp.pressed >= 0) {
        lp.pressed = -1;
        ui_surface_damage_all(s);
    }
}

/* An object button's menu (h: 4..), or the bar's (anything else). */
static void button_menu(ui_surface *s, size_t h, double x, double y, uint32_t serial)
{
    if (h >= NSYS && h < NSYS + lp.n) {
        ui_menu_item items[] = {
            { 1, "Open", true, false, NULL, 0 },
            { 2, "Remove from LaunchPad", true, false, NULL, 0 },
            { 0, "-", false, false, NULL, 0 },
            { 3, "Open LaunchPad folder", true, false, NULL, 0 },
        };
        ui_menu_popup(s, items, 4, x, y, serial, menu_chosen, (void *)(intptr_t)(h - NSYS));
    } else {
        ui_menu_item items[] = {
            { 3, "Open LaunchPad folder", true, false, NULL, 0 },
            { 4, "Window list", true, false, NULL, 0 },
        };
        ui_menu_popup(s, items, 2, x, y, serial, bar_menu_chosen, NULL);
    }
}

#define MIME_WPS "application/x-wps-objects"
#define MIME_URIS "text/uri-list"

static uint32_t lp_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n,
                               uint32_t src_actions, unsigned mods, const char **mime)
{
    (void)mods;
    *mime = NULL;
    for (size_t i = 0; i < n && !*mime; i++) if (!strcmp(mimes[i], MIME_WPS)) *mime = MIME_WPS;
    for (size_t i = 0; i < n && !*mime; i++) if (!strcmp(mimes[i], MIME_URIS)) *mime = MIME_URIS;
    if (!*mime) return 0;
    bool arrow;
    ssize_t h = hit(x, y, &arrow);
    ssize_t at = h >= NSYS && arrow ? h : -2;
    if (at != lp.drop_at || arrow != lp.drop_arrow) {
        lp.drop_at = at;
        lp.drop_arrow = arrow && h >= NSYS;
        ui_surface_damage_all(s);
    }
    /* a shadow is made, whatever the source offers */
    return src_actions & UI_DND_COPY ? UI_DND_COPY : src_actions & UI_DND_MOVE ? UI_DND_MOVE : UI_DND_COPY;
}

static void lp_drag_leave(ui_surface *s)
{
    lp.drop_at = -1;
    ui_surface_damage_all(s);
}

static uint32_t lp_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len,
                        uint32_t action, uint32_t start_action, unsigned mods)
{
    (void)mime; (void)action; (void)start_action; (void)mods;
    bool arrow;
    ssize_t h = hit(x, y, &arrow);
    lp.drop_at = -1;
    ui_surface_damage_all(s);
    if (h >= NSYS && arrow) {
        char d[PATH_MAX];
        drawer_dir((size_t)h - NSYS, d, sizeof d);
        add_paths(d, data, len);
    } else {
        add_paths(lp.dir, data, len);
    }
    events_raise("object.drop", "", "launchpad", (ui_rect){ 0, 0, 0, 0 });
    return UI_DND_COPY;   /* the source keeps its object: this was a shadow */
}

/* For screen readers: the button the keyboard is on. */
static void a11y_button(void)
{
    if (!lp.s) return;
    size_t total = NSYS + lp.n, k = lp.kfocus < total ? lp.kfocus : total - 1;
    ui_a11y_info i = { .role = UI_ROLE_BUTTON, .index = (int)k, .count = (int)total,
                       .at = k < NSYS ? sys_rect(k) : obj_rect(k - NSYS) };
    i.name = k < NSYS ? sys_names[k] : lp.items[k - NSYS].title;
    i.description = k < NSYS ? NULL : "a drawer above it (Up)";
    ui_a11y_focus(lp.s, &i);
}

/* The keyboard (accessibility, G4): Left and Right (Home, End) go from
 * button to button, Enter or Space presses one, Up opens its drawer,
 * Shift+F10 (or the Menu key) its menu. */
static void lp_key(ui_surface *s, const ui_key_event *e)
{
    size_t total = NSYS + lp.n;
    if (lp.kfocus >= total) lp.kfocus = total - 1;
    size_t k = lp.kfocus;
    switch (e->sym) {
    case XKB_KEY_Left: case XKB_KEY_ISO_Left_Tab: lp.kfocus = k ? k - 1 : total - 1; break;
    case XKB_KEY_Right: case XKB_KEY_Tab: lp.kfocus = (k + 1) % total; break;
    case XKB_KEY_Home: lp.kfocus = 0; break;
    case XKB_KEY_End: lp.kfocus = total - 1; break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space:
        if (k < NSYS) press_sys(k);
        else open_entry(lp.dir, &lp.items[k - NSYS]);
        return;
    case XKB_KEY_Up:
        if (k >= NSYS) drawer_open(k - NSYS, ui_app_input_serial(lp.app));
        return;
    case XKB_KEY_Menu: case XKB_KEY_F10: {
        if (e->sym == XKB_KEY_F10 && !(e->mods & UI_MOD_SHIFT)) return;
        ui_rect r = k < NSYS ? sys_rect(k) : obj_rect(k - NSYS);
        button_menu(s, k, r.x + r.w / 2, r.y + r.h / 2, ui_app_input_serial(lp.app));
        return;
    }
    default: return;
    }
    ui_surface_damage_all(s);
    a11y_button();
}

static void lp_focus(ui_surface *s, bool on)
{
    lp.focused = on;
    ui_surface_damage_all(s);
    if (on) a11y_button();
}

static void lp_close(ui_surface *s)
{
    (void)s;   /* the LaunchPad stays while the desktop runs (OS/2: it closed with the desktop) */
}

static const ui_surface_ops lp_ops = { .draw = lp_draw, .pointer = lp_pointer, .key = lp_key, .focus = lp_focus, .close = lp_close,
                                       .drag_motion = lp_drag_motion, .drag_leave = lp_drag_leave, .drop = lp_drop };

void launchpad_open(ui_app *a, void (*open_folder)(const char *path), const char *templates, const char *setup)
{
    if (lp.s) { ui_window_activate(lp.s); return; }
    lp.app = a;
    lp.open_folder = open_folder;
    const char *d = getenv("WPS_LAUNCHPAD_DIR"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (d && *d) snprintf(lp.dir, sizeof lp.dir, "%s", d);
    else if (xdg && *xdg) snprintf(lp.dir, sizeof lp.dir, "%s/wps/LaunchPad", xdg);
    else snprintf(lp.dir, sizeof lp.dir, "%s/.local/share/wps/LaunchPad", home ? home : "");
    snprintf(lp.drawers, sizeof lp.drawers, "%s Drawers", lp.dir);
    struct stat st;
    if (stat(lp.dir, &st) != 0) {
        /* The first time: shadows of the home folder, Templates and System Setup. */
        mkdirs(lp.dir);
        if (home && *home) svc_shadow(home, lp.dir, removed, NULL);
        if (templates && *templates) svc_shadow(templates, lp.dir, removed, NULL);
        if (setup && *setup) svc_shadow(setup, lp.dir, removed, NULL);
    }
    lp.s = ui_window_new(a, &lp_ops, NULL, "org.wps.LaunchPad", bar_w(), bar_h());
    ui_a11y_window(lp.s, UI_ROLE_TOOL_BAR, "OS/2's buttons, then objects");
    ui_window_set_title(lp.s, "LaunchPad");
    ui_window_set_frameless(lp.s, true);
    ui_window_set_size(lp.s, bar_w(), bar_h(), true);
    svc_subscribe_folder(lp.dir, got_sub, NULL);
    reload();
}

/* ---------------------------------------------------------------- tests */

size_t launchpad_count(void) { return lp.n; }
const char *launchpad_title(size_t i) { return i < lp.n ? lp.items[i].title : NULL; }
const char *launchpad_dir(void) { return lp.dir; }

bool launchpad_press(size_t b)
{
    if (b < NSYS) { press_sys(b); return true; }
    if (b - NSYS >= lp.n) return false;
    open_entry(lp.dir, &lp.items[b - NSYS]);
    return true;
}

bool launchpad_add(const char *path)
{
    if (!lp.s) return false;
    add_paths(lp.dir, path, strlen(path));
    return true;
}

bool launchpad_drawer(size_t i, const char *path)
{
    if (i >= lp.n) return false;
    char d[PATH_MAX];
    drawer_dir(i, d, sizeof d);
    add_paths(d, path, strlen(path));
    return true;
}

bool launchpad_snapshot(const char *png) { return lp.s && ui_surface_snapshot(lp.s, png); }
