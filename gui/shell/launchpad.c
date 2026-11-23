/* The LaunchPad (launchpad.h). */
#define _GNU_SOURCE
#include "launchpad.h"

#include <ctype.h>
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
#include "session.h"
#include "winlist.h"
#include "findwin.h"
#include "mime.h"
#include "icons.h"
#include "objmenu.h"
#include "tooltip.h"
#include "wps_menu_ids.h"

#define GRIP_W 12.0
#define SEP_W 8.0
#define ARROW_H 12.0         /* the drawer arrow above each object button (beside it, vertical) */
#define PAD 3.0
/* (sizes from its settings: SYS_W, BTN_W, BTN_H below) */
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
    char icon[PATH_MAX];       /* its own icon (a PNG), or "" */
    char id[48];               /* its object ID (positions are kept by it) */
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
    int64_t sub;               /* its folder followed while it is open */
    objmenu *cascade;          /* a folder's cascade from it (objmenu.h) */
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
    ssize_t r_press;           /* an object button pressed with the drag button: a drag or its menu */
    double r_x, r_y;
    uint32_t r_serial;
    ssize_t dragging;          /* the object button being dragged (from this bar), -1 none */
    char drag_wps[PATH_MAX + 64], drag_uri[3 * PATH_MAX + 64];
    /* the LaunchPad object's settings (OS/2's, v0.3-design 11) */
    bool vertical, small, text, float_top, close_drawer;
    bool drawers_left;         /* vertical: the drawer arrows (and drawers) on the left */
    void (*open_props)(const char *addr);
    objmenu *obj_menu_m;       /* (unused: object menus come from wpsd) */
} lp = { .pressed = -1, .drop_at = -1, .r_press = -1, .dragging = -1, .text = true, .float_top = true, .close_drawer = true };

static void reload(void);
static void start_drag(ui_surface *s, size_t i, uint32_t serial);
static void drawer_close(void);
static void cascade_open(size_t row);
static void drawer_sub(void *arg, const cv *reply);
static void drawer_got(void *arg, const cv *reply);
static void opened(void *arg, const cv *reply);

/* -------------------------------------------------------------- geometry */

/* Along the bar (x; y when vertical) and across it. Buttons: SYS_W and
 * BTN_W along, BTN_H across (vertical with text: wide enough for it). */
#define SYS_W (lp.small ? 28.0 : 40.0)
#define BTN_W (lp.small ? (lp.text && !lp.vertical ? 48.0 : 30.0) : (lp.text && !lp.vertical ? 56.0 : 42.0))
#define BTN_H (lp.vertical && lp.text ? (lp.small ? 130.0 : 150.0) : lp.small ? (lp.text ? 40.0 : 28.0) : (lp.text ? 56.0 : 42.0))
#define ICON_PX (lp.small ? 16.0 : 32.0)

static ui_rect at(double a, double c, double la, double lc)
{
    return lp.vertical ? (ui_rect){ c, a, lc, la } : (ui_rect){ a, c, la, lc };
}

static double obj_x(size_t i) { return GRIP_W + NSYS * SYS_W + SEP_W + (double)i * BTN_W; }
static double bar_len(void) { return obj_x(lp.n ? lp.n : 1) + PAD; }
static double bar_cross(void) { return ARROW_H + BTN_H + 2 * PAD; }
static double bar_w(void) { return lp.vertical ? bar_cross() : bar_len(); }
static double bar_h(void) { return lp.vertical ? bar_len() : bar_cross(); }

/* (the arrows across the bar from the buttons: above them; on a vertical
 * one, on the side the drawers open to) */
static bool arrows_after(void) { return lp.vertical && !lp.drawers_left; }
static double btn_c(void) { return arrows_after() ? PAD : ARROW_H + PAD; }
static ui_rect sys_rect(size_t i) { return at(GRIP_W + (double)i * SYS_W, btn_c(), SYS_W - 2, BTN_H); }
static ui_rect obj_rect(size_t i) { return at(obj_x(i), btn_c(), BTN_W - 2, BTN_H); }
static ui_rect arrow_rect(size_t i) { return at(obj_x(i), arrows_after() ? PAD + BTN_H + 1 : PAD, BTN_W - 2, ARROW_H - 1); }

static bool in(ui_rect r, double x, double y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* What is at (x, y): 0..3 OS/2's buttons, 4.. object buttons; *arrow for
 * a drawer arrow; -2 the grip; -1 nothing. */
static ssize_t hit(double x, double y, bool *arrow)
{
    *arrow = false;
    if ((lp.vertical ? y : x) < GRIP_W) return -2;
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
        const char *oid = cv_text(it, "id");
        if (oid) snprintf(e->id, sizeof e->id, "%s", oid);
        const char *ic = cv_text(it, "icon");
        if (ic) snprintf(e->icon, sizeof e->icon, "%s", ic);
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

/* the first time's shadows are being made and placed (below): no places
 * written back meanwhile, or a stale listing's order could win */
static bool first_filling;

static void got_order(void *arg, const cv *reply)
{
    (void)arg;
    if (!lp.s) return;
    /* positions by object ID ("x,y"): files and central objects alike */
    const cv *res = reply ? cv_get(reply, "result") : NULL, *pos = res ? cv_get(res, "positions") : NULL;
    for (size_t i = 0; i < nlisted; i++) {
        order_x[i] = INT64_MAX;   /* no position: at the end */
        const char *v = listed_items[i].id[0] && pos ? cv_text(pos, listed_items[i].id) : NULL;
        if (v) order_x[i] = strtoll(v, NULL, 10);
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
        if (order_x[idx[k]] != (int64_t)k * ORDER_CELL) {
            char a[PATH_MAX + 32];
            address(lp.dir, &lp.items[k], a, sizeof a);
            t[nplace] = strdup(a);
            px[nplace] = (int64_t)k * ORDER_CELL;
            py[nplace++] = 0;
        }
    }
    lp.n = nlisted;
    if (nplace && !first_filling) svc_place(lp.dir, (const char *const *)t, px, py, nplace, placed, NULL);
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

static void drawer_sub(void *arg, const cv *reply)
{
    (void)arg;
    const cv *r = reply ? cv_get(reply, "result") : NULL;
    if (!r || r->k != CV_INT) return;
    if (!lp.drawer.s) { svc_unsubscribe(r->i); return; }   /* (closed meanwhile) */
    if (lp.drawer.sub) svc_unsubscribe(lp.drawer.sub);
    lp.drawer.sub = r->i;
}

bool launchpad_event(const char *name, int64_t sub)
{
    if (lp.drawer.s && lp.drawer.sub && sub == lp.drawer.sub) {   /* its drawer's folder changed */
        svc_list(lp.drawer.dir, 0, MAX_ITEMS, true, drawer_got, NULL);
        return true;
    }
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
    if (svc_reply_view(reply)) return;
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

static void press_sys(size_t i, unsigned mods)
{
    events_raise("launchpad.button", sys_names[i], "launchpad", (ui_rect){ 0, 0, 0, 0 });
    if (i == 3) { winlist_open(false); return; }
    if (i == 2) { session_ask_end(lp.app, "launchpad"); return; }
    if (i == 0) { session_lock("launchpad"); return; }
    if (i == 1) {   /* WPS's own Find, from the home folder */
        const char *home = getenv("HOME");
        findwin_show(home && *home ? home : "/", mods & UI_MOD_SHIFT);   /* Shift: another */
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
    if (d->hot >= 0) tooltip_hide();
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
        if (d->items[i].kind == UI_KIND_FOLDER) {   /* its cascade (the WarpCenter's menus) */
            ui_set_color(cr, (ssize_t)i == d->hot ? k->menu_hilite_text : k->text);
            double ax = r.x + r.w - 10, ay = r.y + r.h / 2;
            cairo_move_to(cr, ax - 3, ay - 5);
            cairo_line_to(cr, ax + 2, ay);
            cairo_line_to(cr, ax - 3, ay + 5);
            cairo_close_path(cr);
            cairo_fill(cr);
            r.w -= 16;
        }
        ui_item it = { .loaded = true, .detailed = true, .kind = d->items[i].kind, .shadow = d->items[i].shadow,
                       .link = d->items[i].link };
        it.image = icons_get(d->items[i].icon);
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
        /* a folder's arrow: its cascade, as the WarpCenter's menus */
        if (row >= 0 && d->items[row].kind == UI_KIND_FOLDER && e->x >= DRAWER_W - 2 * PAD - 18) {
            d->down = -1;
            cascade_open((size_t)row);
        }
    } else if (e->kind == UI_RELEASE && e->button == UI_BTN1 && row >= 0 && row == d->down) {
        struct entry it = d->items[row];
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", d->dir);
        if (lp.close_drawer) drawer_close();   /* (the setting off: it stays open) */
        open_entry(dir, &it);
    }
}

/* A folder in a drawer: its objects as a cascade with icons (objmenu.h). */
static size_t cascade_row;

static void cascade_chosen(void *arg, int64_t id)
{
    objmenu *m = arg;
    struct drawer *d = &lp.drawer;
    char addr[PATH_MAX], title[256];
    bool folder;
    if (m && objmenu_choice(m, id, addr, sizeof addr, &folder, title, sizeof title)) {
        events_raise("object.open", title, "launchpad", (ui_rect){ 0, 0, 0, 0 });
        if (folder) lp.open_folder(addr);
        else svc_open(addr, false, opened, NULL);
        if (lp.close_drawer) drawer_close();
    }
    if (d->cascade == m) d->cascade = NULL;
    objmenu_free(m);
}

static void cascade_read(objmenu *m, void *ctx)
{
    (void)ctx;
    struct drawer *d = &lp.drawer;
    if (!d->s || d->cascade != m) { if (d->cascade != m) objmenu_free(m); return; }
    size_t n = 0;
    ui_menu_item *it = objmenu_items(m, &n, true, 0);
    ui_menu_popup_icons(d->s, it, n, DRAWER_W - PAD, PAD + (double)cascade_row * DRAWER_ROW, ui_app_input_serial(lp.app),
                        cascade_chosen, m, objmenu_icon);
    ui_menu_items_free(it, n);
}

static void cascade_open(size_t row)
{
    struct drawer *d = &lp.drawer;
    if (row >= d->n) return;
    char a[PATH_MAX + 32];
    address(d->dir, &d->items[row], a, sizeof a);
    const char *folder = (d->items[row].shadow || d->items[row].link) && d->items[row].target[0] == '/' ? d->items[row].target : a;
    if (d->cascade) objmenu_free(d->cascade);
    cascade_row = row;
    d->cascade = objmenu_read(folder, 3, cascade_read, NULL);
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
    if (d->sub) svc_unsubscribe(d->sub);   /* (no longer followed) */
    d->sub = 0;
    if (d->cascade) { objmenu_free(d->cascade); d->cascade = NULL; }
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
    /* above its button, as OS/2's drawers opened (below if there is no
     * room); beside it on a vertical LaunchPad */
    d->s = lp.vertical ? ui_popup_new_beside(lp.app, lp.s, &drawer_ops, d, arrow_rect(i), DRAWER_W, DRAWER_ROW + 2 * PAD, serial,
                                             lp.drawers_left)
                       : ui_popup_new_above(lp.app, lp.s, &drawer_ops, d, arrow_rect(i), DRAWER_W, DRAWER_ROW + 2 * PAD, serial);
    svc_list(d->dir, 0, MAX_ITEMS, true, drawer_got, NULL);
    svc_subscribe_folder(d->dir, drawer_sub, NULL);   /* followed while open */
    events_raise("menu.open", lp.items[i].title, "launchpad", (ui_rect){ 0, 0, 0, 0 });
    ui_surface_damage_all(lp.s);
}

/* ------------------------------------------------------------------ bar */

static void draw_glyph(cairo_t *cr, size_t i, ui_rect r);

/* The system buttons' pictures (0 Lockup, 1 Find, 2 Shut down, 3 Window
 * list), for the WarpCenter's buttons too. */
void launchpad_glyph(cairo_t *cr, int i, ui_rect r) { draw_glyph(cr, (size_t)i, r); }

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
    for (double c = 6; c < bar_cross() - 6; c += 4) {
        ui_fill(cr, at(4, c, 2, 2), k->light);
        ui_fill(cr, at(5, c + 1, 2, 2), k->shadow);
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
    ui_fill(cr, at(sx, btn_c() + 2, 1, BTN_H - 4), k->shadow);
    ui_fill(cr, at(sx + 1, btn_c() + 2, 1, BTN_H - 4), k->light);
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
        if (lp.vertical) {   /* pointing out to the side, where the drawer opens */
            double dir = lp.drawers_left ? -1 : 1;
            cairo_move_to(cr, ax - 2 * dir, ay - 5);
            cairo_line_to(cr, ax - 2 * dir, ay + 5);
            cairo_line_to(cr, ax + 3 * dir, ay);
        } else {
            cairo_move_to(cr, ax - 5, ay + 2);
            cairo_line_to(cr, ax + 5, ay + 2);
            cairo_line_to(cr, ax, ay - 3);
        }
        cairo_close_path(cr);
        cairo_fill(cr);
        if (target && !lp.drop_arrow) ui_fill(cr, r, k->selection);
        ui_bevel(cr, r, !down);
        ui_item it = { .loaded = true, .detailed = true, .kind = lp.items[i].kind, .shadow = lp.items[i].shadow,
                       .link = lp.items[i].link };
        double d = down ? 1 : 0, ip = ICON_PX;
        it.image = icons_get(lp.items[i].icon);
        /* the icon: centred, above the title (horizontal), or left of it (vertical) */
        double ix = lp.vertical && lp.text ? r.x + 5 : r.x + (r.w - ip) / 2;
        double iy = lp.text && !lp.vertical ? r.y + 4 : r.y + (r.h - ip) / 2;
        ui_draw_item_icon(cr, &it, ix + d, iy + d, ip);
        if (lp.focused && lp.kfocus == NSYS + i) focus_ring(cr, (ui_rect){ r.x + 3, r.y + 3, r.w - 6, r.h - 6 });
        if (lp.text) {
            ui_set_color(cr, k->text);
            cairo_save(cr);
            cairo_rectangle(cr, r.x + 1, r.y, r.w - 2, r.h);
            cairo_clip(cr);
            if (lp.vertical) {
                ui_text_draw(cr, UI_FONT_REGULAR, ix + ip + 6 + d, r.y + (r.h + ui_text_ascent()) / 2 - 1 + d, lp.items[i].title,
                             r.x + r.w - (ix + ip + 6) - 4);
            } else {
                double tw = ui_text_width(UI_FONT_REGULAR, lp.items[i].title);
                double tx = tw < r.w - 4 ? r.x + (r.w - tw) / 2 : r.x + 2;
                ui_text_draw(cr, UI_FONT_REGULAR, tx + d, r.y + r.h - 6 + d, lp.items[i].title, r.w - 4);
            }
            cairo_restore(cr);
        }
    }
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
    /* tooltips: the object's title (or the button's name) after a moment */
    if (e->kind == UI_MOTION || e->kind == UI_ENTER) {
        if (h >= 0 && !arrow) {
            ui_rect r = h < NSYS ? sys_rect((size_t)h) : obj_rect((size_t)h - NSYS);
            tooltip_hover(s, r, h < NSYS ? sys_names[h] : lp.items[h - NSYS].title, !lp.vertical);
        } else if (h >= NSYS && arrow) {
            tooltip_hover(s, arrow_rect((size_t)h - NSYS), "Drawer", !lp.vertical);
        } else {
            tooltip_hide();
        }
    } else if (e->kind == UI_PRESS || e->kind == UI_LEAVE) {
        tooltip_hide();
    }
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
        if (was < NSYS) press_sys((size_t)was, e->mods);
        else open_entry(lp.dir, &lp.items[was - NSYS]);
    } else if (e->kind == UI_PRESS && e->button == UI_BTN2) {
        /* on an object: a drag (moved while held) or its menu (released) */
        if (h >= NSYS && !arrow) { lp.r_press = h - NSYS; lp.r_x = e->x; lp.r_y = e->y; lp.r_serial = e->serial; return; }
        button_menu(s, SIZE_MAX, e->x, e->y, e->serial);
    } else if (e->kind == UI_RELEASE && e->button == UI_BTN2 && lp.r_press >= 0) {
        size_t i = (size_t)lp.r_press;
        lp.r_press = -1;
        if (i < lp.n) button_menu(s, NSYS + i, lp.r_x, lp.r_y, 0);   /* (no grab: the button is up) */
    } else if (e->kind == UI_MOTION && lp.r_press >= 0 && hypot(e->x - lp.r_x, e->y - lp.r_y) > 4) {
        size_t i = (size_t)lp.r_press;
        lp.r_press = -1;
        if (i < lp.n) start_drag(s, i, lp.r_serial);
    } else if (e->kind == UI_LEAVE && lp.pressed >= 0) {
        lp.pressed = -1;
        ui_surface_damage_all(s);
    }
}

/* An object button's menu: the object's own (as a folder's icon has it:
 * wpsd's), then the LaunchPad's items. */
enum { LP_REMOVE = 9001, LP_FOLDER = 9002 };
struct obj_menu { size_t i; double x, y; char addr[PATH_MAX + 32]; };

static void convert_items(const cv *arr, ui_menu_item **out, size_t *n, size_t extra)
{
    size_t m = arr && arr->k == CV_ARRAY ? arr->n : 0;
    *out = calloc(m + extra + 1, sizeof **out);
    *n = 0;
    for (size_t i = 0; i < m; i++) {
        const cv *it = &arr->items[i];
        int64_t id = 0;
        cv_int(it, "id", &id);
        /* the GUI's own actions on a folder window's icons stay there */
        if (id == WPS_MI_DELETE || id == WPS_MI_CREATE_ANOTHER || id == WPS_MI_ARRANGE || id == WPS_MI_LOCK_POS) continue;
        const cv *en = cv_get(it, "enabled"), *ck = cv_get(it, "checked");
        ui_menu_item *o = &(*out)[(*n)++];
        o->id = id;
        o->label = strdup(cv_text(it, "label") ? cv_text(it, "label") : "");
        o->enabled = !en || (en->k == CV_BOOL && en->b);
        o->checked = ck && ck->k == CV_BOOL && ck->b;
        convert_items(cv_get(it, "items"), &o->items, &o->nitems, 0);
    }
}

static void obj_menu_chosen(void *arg, int64_t id)
{
    struct obj_menu *m = arg;
    size_t i = m->i;
    if (id >= 0 && i < lp.n) {
        struct entry e = lp.items[i];
        if (id == WPS_MI_OPEN || id == WPS_MI_OPEN_ICON) open_entry(lp.dir, &e);
        else if (id == LP_REMOVE) remove_entry(lp.dir, &e);
        else if (id == LP_FOLDER) lp.open_folder(lp.dir);
        else if (id == WPS_MI_PROPERTIES) { if (lp.open_props) lp.open_props(e.target[0] == '/' && e.shadow ? e.target : m->addr); }
        else if ((id == WPS_MI_OPEN_TREE || id == WPS_MI_OPEN_DETAILS) && e.kind == UI_KIND_FOLDER) open_entry(lp.dir, &e);
        else if (id >= WPS_MI_OPEN_APP && id < WPS_MI_OPEN_APP + WPS_MI_OPEN_APPS) svc_open_with(m->addr, NULL, (int)(id - WPS_MI_OPEN_APP), false, opened, NULL);
        else if (id >= WPS_MI_FIRST_CLASS) svc_menu_select(m->addr, id, opened, NULL);
    }
    free(m);
}

static void got_obj_menu(void *arg, const cv *reply)
{
    struct obj_menu *m = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!lp.s || m->i >= lp.n) { free(m); return; }
    ui_menu_item *items;
    size_t n;
    convert_items(res, &items, &n, 3);
    items[n++] = (ui_menu_item){ -1, strdup("-"), false, false, NULL, 0 };
    items[n++] = (ui_menu_item){ LP_REMOVE, strdup("Remove from LaunchPad"), true, false, NULL, 0 };
    items[n++] = (ui_menu_item){ LP_FOLDER, strdup("Open LaunchPad folder"), true, false, NULL, 0 };
    /* (no grab: the button is up by now) */
    ui_menu_popup(lp.s, items, n, m->x, m->y, 0, obj_menu_chosen, m);
    ui_menu_items_free(items, n);
}

/* An object button's menu (h: 4..), or the bar's (anything else). */
static void button_menu(ui_surface *s, size_t h, double x, double y, uint32_t serial)
{
    if (h >= NSYS && h < NSYS + lp.n) {
        struct obj_menu *m = calloc(1, sizeof *m);
        m->i = h - NSYS;
        m->x = x;
        m->y = y;
        address(lp.dir, &lp.items[m->i], m->addr, sizeof m->addr);
        tooltip_hide();
        svc_menu(m->addr, got_obj_menu, m);
        (void)serial;
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

/* An object button dragged: dropped on the bar it goes to that place
 * (the buttons' order); elsewhere it is the LaunchPad's shadow that goes
 * (moved out, copied, ... as the keys say), as in OS/2. */
static void drag_send(void *ctx, const char *mime, int fd)
{
    (void)ctx;
    const char *d = !strcmp(mime, MIME_WPS) ? lp.drag_wps : lp.drag_uri;
    ui_app_write_async(lp.app, fd, d, strlen(d));
}

static void drag_end(void *ctx, bool dropped, uint32_t action)
{
    (void)ctx; (void)dropped; (void)action;
    lp.dragging = -1;
}

static void start_drag(ui_surface *s, size_t i, uint32_t serial)
{
    const struct entry *e = &lp.items[i];
    char a[PATH_MAX + 32];
    address(lp.dir, e, a, sizeof a);
    snprintf(lp.drag_wps, sizeof lp.drag_wps, "%s\n", a);
    /* other programs get the file it stands for */
    const char *f = e->target[0] == '/' ? e->target : a[0] == '/' ? a : NULL;
    lp.drag_uri[0] = 0;
    if (f) {
        static const char hex[] = "0123456789ABCDEF";
        size_t o = (size_t)snprintf(lp.drag_uri, sizeof lp.drag_uri, "file://");
        for (const unsigned char *p = (const unsigned char *)f; *p && o + 4 < sizeof lp.drag_uri; p++) {
            if (isalnum(*p) || strchr("/-._~", *p)) lp.drag_uri[o++] = (char)*p;
            else { lp.drag_uri[o++] = '%'; lp.drag_uri[o++] = hex[*p >> 4]; lp.drag_uri[o++] = hex[*p & 15]; }
        }
        snprintf(lp.drag_uri + o, sizeof lp.drag_uri - o, "\r\n");
    }
    int sc = ui_surface_scale(s);
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 40 * sc, 40 * sc);
    cairo_surface_set_device_scale(img, sc, sc);
    cairo_t *cr = cairo_create(img);
    ui_draw_kind_icon(cr, e->kind, 4, 4, 32);
    cairo_destroy(cr);
    const char *mimes[] = { MIME_WPS, MIME_URIS };
    lp.dragging = (ssize_t)i;
    if (!ui_drag_start(s, serial, mimes, lp.drag_uri[0] ? 2 : 1, UI_DND_COPY | UI_DND_MOVE | UI_DND_ASK, img, sc, drag_send,
                       drag_end, NULL))
        lp.dragging = -1;
    cairo_surface_destroy(img);
}

/* The place (0..n) a drop at x goes to: before the button whose middle
 * it is left of. */
static size_t slot_at(double x, double y)
{
    for (size_t i = 0; i < lp.n; i++) {
        ui_rect r = obj_rect(i);
        if (lp.vertical ? y < r.y + r.h / 2 : x < r.x + r.w / 2) return i;
    }
    return lp.n;
}

static void reorder(size_t from, size_t to)
{
    if (from >= lp.n || to > lp.n || to == from || to == from + 1) return;
    struct entry moved = lp.items[from];
    if (to > from) {
        memmove(&lp.items[from], &lp.items[from + 1], (to - 1 - from) * sizeof moved);
        lp.items[to - 1] = moved;
    } else {
        memmove(&lp.items[to + 1], &lp.items[to], (from - to) * sizeof moved);
        lp.items[to] = moved;
    }
    /* the order is the positions: every button's x, kept by wpsd */
    char *t[MAX_ITEMS];
    int64_t px[MAX_ITEMS], py[MAX_ITEMS];
    for (size_t k = 0; k < lp.n; k++) {
        char a[PATH_MAX + 32];
        address(lp.dir, &lp.items[k], a, sizeof a);
        t[k] = strdup(a);
        px[k] = (int64_t)k * ORDER_CELL;
        py[k] = 0;
    }
    svc_place(lp.dir, (const char *const *)t, px, py, lp.n, placed, NULL);
    for (size_t k = 0; k < lp.n; k++) free(t[k]);
    layout();
    ui_surface_damage_all(lp.s);
    events_raise("object.move", "", "launchpad", (ui_rect){ 0, 0, 0, 0 });
}

static void opened_with(void *arg, const cv *reply)
{
    (void)arg;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b))
        fprintf(stderr, "wps-shell: LaunchPad: cannot start it: %s\n", reply ? cv_text(reply, "message") : "no wpsd");
}

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
    if (lp.dragging >= 0 && !(h >= NSYS && arrow)) {   /* one of its own buttons: to this place */
        reorder((size_t)lp.dragging, slot_at(x, y));
        return UI_DND_COPY;   /* (nothing to remove: it stays, elsewhere in the row) */
    }
    if (h >= NSYS && !arrow && lp.items[h - NSYS].kind == UI_KIND_PROGRAM && !lp.items[h - NSYS].folder_target) {
        /* on a program's button: it starts with the objects dropped (files) */
        const char *files[32];
        size_t nf = 0;
        char *copy = strndup(data, len), *save = NULL;
        for (char *line = strtok_r(copy, "\r\n", &save); line && nf < 32; line = strtok_r(NULL, "\r\n", &save)) {
            line[strcspn(line, "\t")] = 0;
            char path[PATH_MAX];
            if (!strncmp(line, "file://", 7)) {
                const char *u = line + 7;
                if (!strncmp(u, "localhost", 9)) u += 9;
                char *o = path;
                for (; *u && o < path + sizeof path - 1; u++) {
                    if (*u == '%' && u[1] && u[2]) { char hx[3] = { u[1], u[2], 0 }; *o++ = (char)strtol(hx, NULL, 16); u += 2; }
                    else *o++ = *u;
                }
                *o = 0;
            } else snprintf(path, sizeof path, "%s", line);
            if (path[0] == '/') files[nf++] = strdup(path);
        }
        free(copy);
        char a[PATH_MAX + 32];
        address(lp.dir, &lp.items[h - NSYS], a, sizeof a);
        const char *prog = lp.items[h - NSYS].target[0] == '/' ? lp.items[h - NSYS].target : a;
        if (nf && !program_opens(prog, files, nf)) {   /* it does not open these: refused */
            for (size_t k = 0; k < nf; k++) free((char *)files[k]);
            return 0;
        }
        if (nf) svc_open_files(a, false, files, nf, opened_with, NULL);
        for (size_t k = 0; k < nf; k++) free((char *)files[k]);
        events_raise("object.drop", lp.items[h - NSYS].title, "launchpad", (ui_rect){ 0, 0, 0, 0 });
        return UI_DND_COPY;
    }
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
        if (k < NSYS) press_sys(k, e->mods);
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

/* The first time's shadows (home, Templates, System Setup): made one after
 * another, then placed in that order together - the reloads in between
 * give each new one a place of its own as it appears, by listing order. */
static char first[3][PATH_MAX], first_link[3][PATH_MAX];
static int nfirst, ifirst;

static void first_placed(void *arg, const cv *reply)
{
    (void)arg; (void)reply;
    first_filling = false;
    reload();
}

static void first_next(void *arg, const cv *reply)
{
    (void)arg;
    if (reply) {
        const char *link = cv_text(reply, "result");
        if (link && *link) snprintf(first_link[ifirst - 1], sizeof first_link[0], "%s", link);
        else fprintf(stderr, "wps-shell: LaunchPad: a shadow was not made: %s\n", cv_text(reply, "message"));
    }
    if (ifirst < nfirst) { ifirst++; svc_shadow(first[ifirst - 1], lp.dir, first_next, NULL); return; }
    const char *t[3];
    int64_t x[3], y[3];
    size_t n = 0;
    for (int k = 0; k < nfirst; k++)
        if (first_link[k][0]) { t[n] = first_link[k]; x[n] = (int64_t)n * ORDER_CELL; y[n++] = 0; }
    if (n) svc_place(lp.dir, t, x, y, n, first_placed, NULL);
    else first_placed(NULL, NULL);
}

void launchpad_open(ui_app *a, void (*open_folder)(const char *path), const char *templates, const char *setup)
{
    /* once: the settings objects are read again whenever one changes (a
     * rename in System Setup), and bringing it forward each time took the
     * keyboard from the window being typed in */
    if (lp.s) return;
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
        if (home && *home) snprintf(first[nfirst++], sizeof first[0], "%s", home);
        if (templates && *templates) snprintf(first[nfirst++], sizeof first[0], "%s", templates);
        if (setup && *setup) snprintf(first[nfirst++], sizeof first[0], "%s", setup);
        first_filling = true;
        first_next(NULL, NULL);
    }
    /* floating on top: the compositor keeps org.wps.LaunchPad above the
     * windows (the WPS session); else an ordinary window's place */
    lp.s = ui_window_new(a, &lp_ops, NULL, lp.float_top ? "org.wps.LaunchPad" : "org.wps.LaunchPadWindow", bar_w(), bar_h());
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
    if (b < NSYS) { press_sys(b, 0); return true; }
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

bool launchpad_is_open(void) { return lp.s != NULL; }

void launchpad_set_props(void (*fn)(const char *addr)) { lp.open_props = fn; }

void launchpad_setting(const char *key, const char *val)
{
    bool on = !strcmp(val, "1");
    bool was_float = lp.float_top;
    if (!strcmp(key, "orientation")) lp.vertical = !strcmp(val, "vertical");
    else if (!strcmp(key, "small_icons")) lp.small = on;
    else if (!strcmp(key, "text")) lp.text = on;
    else if (!strcmp(key, "float_top")) lp.float_top = on;
    else if (!strcmp(key, "close_drawer")) lp.close_drawer = on;
    else if (!strcmp(key, "drawer_side")) lp.drawers_left = !strcmp(val, "left");
    else return;
    if (!lp.s) return;
    drawer_close();
    tooltip_hide();
    if (was_float != lp.float_top) {   /* another kind of window: made again */
        ui_surface_destroy(lp.s);
        lp.s = ui_window_new(lp.app, &lp_ops, NULL, lp.float_top ? "org.wps.LaunchPad" : "org.wps.LaunchPadWindow", bar_w(), bar_h());
        ui_a11y_window(lp.s, UI_ROLE_TOOL_BAR, "OS/2's buttons, then objects");
        ui_window_set_title(lp.s, "LaunchPad");
        ui_window_set_frameless(lp.s, true);
    }
    layout();
}

bool launchpad_vertical(void) { return lp.vertical; }
double launchpad_width(void) { return bar_w(); }
double launchpad_height(void) { return bar_h(); }
bool launchpad_hover(size_t button)
{
    if (!lp.s || button >= NSYS + lp.n) return false;
    ui_rect r = button < NSYS ? sys_rect(button) : obj_rect(button - NSYS);
    ui_pointer_event e = { .kind = UI_MOTION, .x = r.x + r.w / 2, .y = r.y + r.h / 2 };
    lp_pointer(lp.s, &e);
    return true;
}
bool launchpad_drawer_open(size_t i) { if (!lp.s || i >= lp.n) return false; drawer_open(i, 0); return true; }
size_t launchpad_drawer_count(void) { return lp.drawer.s ? lp.drawer.n : 0; }

void launchpad_raise(void)
{
    if (lp.s) ui_window_activate(lp.s);
}
