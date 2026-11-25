/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* TheBar's trays (see trays.h). */
#define _GNU_SOURCE
#include "trays.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "events.h"
#include "osd_menu_ids.h"
#include "svc.h"
#include "tooltip.h"

const char *setup_addr(const char *cls);   /* (shell.h: a settings object's address) */

#define MAX_TRAYS 32
#define MAX_OBJS 48
#define ORDER_CELL 10   /* stored x of place k: k * ORDER_CELL (the order is what counts) */

static struct {
    ui_app *app;
    void (*changed)(void);
    void (*open_folder)(const char *path);
    void (*open_props)(const char *path);
    char dir[PATH_MAX];
    char names[MAX_TRAYS][NAME_MAX + 1];
    size_t ntrays;
    char want[NAME_MAX + 1];       /* TheBar's setting: the tray to show */
    char cur[NAME_MAX + 1];        /* the one shown ("": none read yet) */
    char cur_dir[PATH_MAX + NAME_MAX + 2];
    struct tray_obj objs[MAX_OBJS];
    size_t n;
    int64_t root_sub, cur_sub;
    unsigned gen;                  /* the current tray's reading (older replies are dropped) */
} tr;

static struct tray_obj listed[MAX_OBJS];
static size_t nlisted;
static int64_t order_x[MAX_OBJS];

static void reload_trays(void);
static void reload_objs(void);

const char *trays_dir(void) { return tr.dir; }
size_t trays_count(void) { return tr.ntrays; }
const char *trays_name(size_t i) { return i < tr.ntrays ? tr.names[i] : ""; }
const char *trays_current(void) { return tr.cur; }
size_t trays_n(void) { return tr.n; }
const struct tray_obj *trays_obj(size_t i) { return i < tr.n ? &tr.objs[i] : NULL; }

static void changed(void) { if (tr.changed) tr.changed(); }

static void address(const struct tray_obj *o, char *out, size_t n)
{
    if (o->name[0] == '\x01') snprintf(out, n, "%s", o->name + 1);   /* a central object: vobj:<id> */
    else snprintf(out, n, "%s/%s", tr.cur_dir, o->name);
}

/* ------------------------------------------------------------ reading */

static ui_kind kind_for(const char *type, const char *name)
{
    if (type && !strcmp(type, "folder")) return UI_KIND_FOLDER;
    if (type && !strcmp(type, "program")) return UI_KIND_PROGRAM;
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot && !strcmp(dot, ".desktop")) return UI_KIND_PROGRAM;
    return UI_KIND_FILE;
}

static size_t parse(const cv *reply, struct tray_obj *out, size_t max)
{
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    size_t n = 0;
    for (size_t i = 0; items && items->k == CV_ARRAY && i < items->n && n < max; i++) {
        const cv *it = &items->items[i];
        const char *nm = cv_text(it, "name"), *ti = cv_text(it, "title"), *ty = cv_text(it, "type");
        if (!nm || nm[0] == '.') continue;
        struct tray_obj *e = &out[n++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", nm);
        snprintf(e->title, sizeof e->title, "%s", ti && *ti ? ti : nm[0] == '\x01' ? "" : nm);
        e->kind = kind_for(ty, nm);
        const cv *sh = cv_get(it, "shadow");
        e->shadow = sh && sh->k == CV_BOOL && sh->b;
        e->link = ty && !strcmp(ty, "link");
        const char *oid = cv_text(it, "id"), *ic = cv_text(it, "icon");
        if (oid) snprintf(e->id, sizeof e->id, "%s", oid);
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

static int by_order(const void *a, const void *b)
{
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    if (order_x[i] != order_x[j]) return order_x[i] < order_x[j] ? -1 : 1;
    return i < j ? -1 : i > j;
}

static void ignore(void *arg, const cv *reply) { (void)arg; (void)reply; }

/* The objects in `idx` order become places 0, 1, ... (written back where they differ). */
static void put_in_order(const size_t *idx, size_t n)
{
    char *t[MAX_OBJS];
    int64_t px[MAX_OBJS], py[MAX_OBJS];
    size_t np = 0;
    for (size_t k = 0; k < n; k++) {
        tr.objs[k] = listed[idx[k]];
        if (order_x[idx[k]] != (int64_t)k * ORDER_CELL) {
            char a[PATH_MAX + 64];
            address(&tr.objs[k], a, sizeof a);
            t[np] = strdup(a);
            px[np] = (int64_t)k * ORDER_CELL;
            py[np++] = 0;
        }
    }
    tr.n = n;
    if (np) svc_place(tr.cur_dir, (const char *const *)t, px, py, np, ignore, NULL);
    for (size_t k = 0; k < np; k++) free(t[k]);
}

static void got_order(void *arg, const cv *reply)
{
    if ((unsigned)(uintptr_t)arg != tr.gen) return;   /* (another tray since) */
    const cv *res = reply ? cv_get(reply, "result") : NULL, *pos = res ? cv_get(res, "positions") : NULL;
    size_t idx[MAX_OBJS];
    for (size_t i = 0; i < nlisted; i++) {
        order_x[i] = INT64_MAX;   /* no place yet: at the end */
        const char *v = listed[i].id[0] && pos ? cv_text(pos, listed[i].id) : NULL;
        if (v) order_x[i] = strtoll(v, NULL, 10);
        idx[i] = i;
    }
    qsort(idx, nlisted, sizeof *idx, by_order);
    put_in_order(idx, nlisted);
    changed();
}

static void got_objs(void *arg, const cv *reply)
{
    if ((unsigned)(uintptr_t)arg != tr.gen) return;
    nlisted = parse(reply, listed, MAX_OBJS);
    svc_layout(tr.cur_dir, got_order, arg);
}

static void got_cur_sub(void *arg, const cv *reply)
{
    const cv *r = reply ? cv_get(reply, "result") : NULL;
    if (!r || r->k != CV_INT) return;
    if ((unsigned)(uintptr_t)arg != tr.gen) { svc_unsubscribe(r->i); return; }   /* (another tray since) */
    tr.cur_sub = r->i;
}

static void reload_objs(void)
{
    svc_list(tr.cur_dir, 0, MAX_OBJS, true, got_objs, (void *)(uintptr_t)tr.gen);
}

/* Show tray `name` (one of tr.names). */
static void show(const char *name)
{
    if (!strcmp(name, tr.cur)) return;
    snprintf(tr.cur, sizeof tr.cur, "%s", name);
    snprintf(tr.cur_dir, sizeof tr.cur_dir, "%s/%s", tr.dir, name);
    tr.gen++;
    tr.n = 0;
    if (tr.cur_sub) { svc_unsubscribe(tr.cur_sub); tr.cur_sub = 0; }
    changed();
    if (!name[0]) return;
    svc_subscribe_folder(tr.cur_dir, got_cur_sub, (void *)(uintptr_t)tr.gen);
    reload_objs();
}

/* The tray to show: the wanted one if it is there, else the first. */
static void pick(void)
{
    for (size_t i = 0; i < tr.ntrays; i++)
        if (!strcmp(tr.names[i], tr.want)) { show(tr.names[i]); return; }
    show(tr.ntrays ? tr.names[0] : "");
}

static int by_name(const void *a, const void *b) { return strcasecmp(a, b); }

static void got_trays(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    tr.ntrays = 0;
    for (size_t i = 0; items && items->k == CV_ARRAY && i < items->n && tr.ntrays < MAX_TRAYS; i++) {
        const cv *it = &items->items[i];
        const char *nm = cv_text(it, "name"), *ty = cv_text(it, "type");
        if (!nm || nm[0] == '.' || nm[0] == '\x01' || !ty || strcmp(ty, "folder")) continue;   /* (trays: its folders) */
        snprintf(tr.names[tr.ntrays++], sizeof tr.names[0], "%s", nm);
    }
    qsort(tr.names, tr.ntrays, sizeof tr.names[0], by_name);
    bool still = false;
    for (size_t i = 0; i < tr.ntrays; i++) still = still || !strcmp(tr.names[i], tr.cur);
    if (!still) tr.cur[0] = 0;
    pick();
    changed();
}

static void got_root_sub(void *arg, const cv *reply)
{
    (void)arg;
    const cv *r = reply ? cv_get(reply, "result") : NULL;
    if (r && r->k == CV_INT) tr.root_sub = r->i;
}

static void reload_trays(void) { svc_list(tr.dir, 0, MAX_TRAYS * 2, true, got_trays, NULL); }

void trays_start(ui_app *a, void (*on_changed)(void), void (*open_folder)(const char *path), void (*open_props)(const char *path))
{
    tr.app = a;
    tr.changed = on_changed;
    tr.open_folder = open_folder;
    tr.open_props = open_props;
    const char *d = getenv("OSD_TRAYS_DIR"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (d && *d) snprintf(tr.dir, sizeof tr.dir, "%s", d);
    else if (xdg && *xdg) snprintf(tr.dir, sizeof tr.dir, "%s/objectspace/Trays", xdg);
    else snprintf(tr.dir, sizeof tr.dir, "%s/.local/share/objectspace/Trays", home ? home : "");
    svc_subscribe_folder(tr.dir, got_root_sub, NULL);
    reload_trays();
}

bool trays_event(const char *name, int64_t sub)
{
    if (tr.root_sub && sub == tr.root_sub) { reload_trays(); return true; }
    if (tr.cur_sub && sub == tr.cur_sub) {
        if (strcmp(name, "folder_moved")) reload_objs();
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ choosing */

/* Kept in TheBar object's settings, so it is the same at the next login. */
static void keep(const char *name)
{
    const char *a = setup_addr("OsdTheBar");
    if (!*a) return;
    cv v = { .k = CV_TEXT, .s = (char *)name, .len = strlen(name) };
    svc_settings_set(a, "tray_current", &v, -1, ignore, NULL);
}

void trays_want(const char *name)
{
    snprintf(tr.want, sizeof tr.want, "%s", name ? name : "");
    if (tr.ntrays) pick();
}

void trays_choose(const char *name)
{
    snprintf(tr.want, sizeof tr.want, "%s", name);
    pick();
    keep(name);
}

void trays_next(void)
{
    if (!tr.ntrays) return;
    size_t i = 0;
    while (i < tr.ntrays && strcmp(tr.names[i], tr.cur)) i++;
    trays_choose(tr.names[i < tr.ntrays ? (i + 1) % tr.ntrays : 0]);
}

/* ------------------------------------------------------------ acting */

static void opened(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && cv_text(res, "folder")) { if (tr.open_folder) tr.open_folder(cv_text(res, "folder")); return; }
    if (svc_reply_view(reply)) return;
    const char *msg = reply ? cv_text(reply, "message") : "lost the connection to osdd";
    if (!res && msg) fprintf(stderr, "osd-shell: TheBar's tray: %s\n", msg);
}

void trays_open(size_t i)
{
    if (i >= tr.n) return;
    const struct tray_obj *o = &tr.objs[i];
    char a[PATH_MAX + 64];
    address(o, a, sizeof a);
    events_raise("object.open", o->title, "thebar", (ui_rect){ 0, 0, 0, 0 });
    if (o->kind == UI_KIND_FOLDER && !o->shadow && !o->link && o->name[0] != '\x01' && tr.open_folder) tr.open_folder(a);
    else svc_open(a, false, opened, NULL);
}

static void removed(void *arg, const cv *reply)
{
    (void)arg;
    if (!reply || !cv_get(reply, "result"))
        fprintf(stderr, "osd-shell: TheBar's tray: cannot remove: %s\n", reply ? cv_text(reply, "message") : "no osdd");
    reload_objs();
}

static void got_sel_trash(void *arg, const cv *reply)
{
    (void)arg;
    const cv *r = reply ? cv_get(reply, "result") : NULL;
    if (r && r->k == CV_INT) svc_job_start("trash", r->i, NULL, NULL, NULL, NULL, 0, removed, NULL);
}

/* Off the tray: a shadow goes (its original stays); anything else put in
 * the tray's folder goes to the Trash Can. */
static void remove_obj(const struct tray_obj *o)
{
    if (o->name[0] == '\x01') { svc_vdelete(o->name + 1, removed, NULL); return; }
    const char *names[] = { o->name };
    svc_select(tr.cur_dir, names, 1, got_sel_trash, NULL);
}

enum { TR_REMOVE = 9101, TR_FOLDER = 9102 };
struct obj_menu { char name[NAME_MAX + 16]; char addr[PATH_MAX + 64]; double x, y; ui_surface *s; };

static void convert_items(const cv *arr, ui_menu_item **out, size_t *n, size_t extra)
{
    size_t m = arr && arr->k == CV_ARRAY ? arr->n : 0;
    *out = calloc(m + extra + 1, sizeof **out);
    *n = 0;
    for (size_t i = 0; i < m; i++) {
        const cv *it = &arr->items[i];
        int64_t id = 0;
        cv_int(it, "id", &id);
        /* (a folder window's own actions on its icons: not here) */
        if (id == OSD_MI_DELETE || id == OSD_MI_CREATE_ANOTHER || id == OSD_MI_ARRANGE || id == OSD_MI_LOCK_POS) continue;
        const cv *en = cv_get(it, "enabled"), *ck = cv_get(it, "checked");
        ui_menu_item *o = &(*out)[(*n)++];
        o->id = id;
        o->label = strdup(cv_text(it, "label") ? cv_text(it, "label") : "");
        o->enabled = !en || (en->k == CV_BOOL && en->b);
        o->checked = ck && ck->k == CV_BOOL && ck->b;
        convert_items(cv_get(it, "items"), &o->items, &o->nitems, 0);
    }
}

static const struct tray_obj *by_name_now(const char *name)
{
    for (size_t i = 0; i < tr.n; i++) if (!strcmp(tr.objs[i].name, name)) return &tr.objs[i];
    return NULL;
}

static void obj_menu_chosen(void *arg, int64_t id)
{
    struct obj_menu *m = arg;
    const struct tray_obj *o = by_name_now(m->name);
    if (id >= 0 && o) {
        struct tray_obj e = *o;
        if (id == OSD_MI_OPEN || id == OSD_MI_OPEN_ICON) {
            for (size_t i = 0; i < tr.n; i++) if (&tr.objs[i] == o) trays_open(i);
        } else if (id == TR_REMOVE) remove_obj(&e);
        else if (id == TR_FOLDER) { if (tr.open_folder) tr.open_folder(tr.cur_dir); }
        else if (id == OSD_MI_PROPERTIES) { if (tr.open_props) tr.open_props(e.target[0] == '/' && e.shadow ? e.target : m->addr); }
        else if (id == OSD_MI_OPEN_TREE || id == OSD_MI_OPEN_DETAILS) svc_open(m->addr, false, opened, NULL);
        else if (id >= OSD_MI_OPEN_APP && id < OSD_MI_OPEN_APP + OSD_MI_OPEN_APPS) svc_open_with(m->addr, NULL, (int)(id - OSD_MI_OPEN_APP), false, opened, NULL);
        else if (id >= OSD_MI_FIRST_CLASS) svc_menu_select(m->addr, id, opened, NULL);
    }
    free(m);
}

static void got_obj_menu(void *arg, const cv *reply)
{
    struct obj_menu *m = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!by_name_now(m->name)) { free(m); return; }   /* (gone meanwhile) */
    ui_menu_item *items;
    size_t n;
    convert_items(res, &items, &n, 3);
    items[n++] = (ui_menu_item){ -1, strdup("-"), false, false, NULL, 0 };
    items[n++] = (ui_menu_item){ TR_REMOVE, strdup("Remove from tray"), true, false, NULL, 0 };
    items[n++] = (ui_menu_item){ TR_FOLDER, strdup("Open tray folder"), true, false, NULL, 0 };
    ui_menu_popup(m->s, items, n, m->x, m->y, 0, obj_menu_chosen, m);
    ui_menu_items_free(items, n);
}

void trays_obj_menu(ui_surface *s, size_t i, double x, double y)
{
    if (i >= tr.n) return;
    struct obj_menu *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", tr.objs[i].name);
    address(&tr.objs[i], m->addr, sizeof m->addr);
    m->x = x;
    m->y = y;
    m->s = s;
    tooltip_hide();
    svc_menu(m->addr, got_obj_menu, m);
}

/* The tray button's menu: 1.. the trays, then add, rename, delete. */
enum { TM_ADD = 1001, TM_RENAME, TM_DELETE, TM_FOLDER };
struct name_ask { char from[NAME_MAX + 1]; bool rename; ui_dialog *dlg; };

static bool good_name(const char *n)
{
    if (!n[0] || n[0] == '.' || strchr(n, '/') || strlen(n) > NAME_MAX) return false;
    for (size_t i = 0; i < tr.ntrays; i++) if (!strcmp(tr.names[i], n)) return false;
    return true;
}

static void renamed(void *arg, const cv *reply)
{
    char *to = arg;
    if (reply && cv_get(reply, "result")) trays_choose(to);
    free(to);
    reload_trays();
}

static void name_chosen(void *ctx, int choice)
{
    struct name_ask *q = ctx;
    char name[NAME_MAX + 1];
    snprintf(name, sizeof name, "%s", ui_dialog_text(q->dlg));
    if (choice == 0 && good_name(name)) {
        if (q->rename) {
            char from[PATH_MAX + NAME_MAX + 2];
            snprintf(from, sizeof from, "%s/%s", tr.dir, q->from);
            svc_rename(from, name, renamed, strdup(name));
        } else {
            char p[PATH_MAX + NAME_MAX + 2];
            snprintf(p, sizeof p, "%s/%s", tr.dir, name);
            if (mkdir(p, 0700) == 0) { trays_choose(name); reload_trays(); }
        }
    }
    free(q);
}

static void ask_name(bool rename)
{
    struct name_ask *q = calloc(1, sizeof *q);
    q->rename = rename;
    snprintf(q->from, sizeof q->from, "%s", tr.cur);
    static const char *const b[] = { "OK", "Cancel" };
    q->dlg = ui_dialog_open_entry(tr.app, rename ? "Rename Tray" : "Add Tray",
                                  rename ? "The tray's new name:" : "The new tray's name:", false, b, 2, 0, name_chosen, q);
    if (rename) ui_dialog_set_text(q->dlg, tr.cur);
}

static void got_sel_delete(void *arg, const cv *reply)
{
    (void)arg;
    const cv *r = reply ? cv_get(reply, "result") : NULL;
    if (r && r->k == CV_INT) svc_job_start("delete", r->i, NULL, NULL, NULL, NULL, 0, removed, NULL);
}

static void delete_chosen(void *ctx, int choice)
{
    char *name = ctx;
    if (choice == 0) {
        const char *names[] = { name };
        svc_select(tr.dir, names, 1, got_sel_delete, NULL);   /* (its shadows with it; the objects stay) */
    }
    free(name);
}

static void tray_menu_chosen(void *arg, int64_t id)
{
    (void)arg;
    if (id >= 1 && id <= (int64_t)tr.ntrays) trays_choose(tr.names[id - 1]);
    else if (id == TM_ADD) ask_name(false);
    else if (id == TM_RENAME && tr.cur[0]) ask_name(true);
    else if (id == TM_FOLDER) { if (tr.open_folder) tr.open_folder(tr.dir); }
    else if (id == TM_DELETE && tr.cur[0]) {
        char msg[NAME_MAX + 128];
        snprintf(msg, sizeof msg, "Delete the tray \"%s\"? Its objects stay where they are; only the tray goes.", tr.cur);
        static const char *const b[] = { "Delete", "Cancel" };
        ui_dialog_open(tr.app, "Delete Tray", msg, b, 2, 1, delete_chosen, strdup(tr.cur));
    }
}

void trays_menu(ui_surface *s, double x, double y, uint32_t serial)
{
    ui_menu_item items[MAX_TRAYS + 6];
    size_t n = 0;
    for (size_t i = 0; i < tr.ntrays; i++)
        items[n++] = (ui_menu_item){ (int64_t)i + 1, tr.names[i], true, !strcmp(tr.names[i], tr.cur), NULL, 0 };
    if (n) items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
    items[n++] = (ui_menu_item){ TM_ADD, "Add tray...", true, false, NULL, 0 };
    items[n++] = (ui_menu_item){ TM_RENAME, "Rename tray...", tr.cur[0] != 0, false, NULL, 0 };
    items[n++] = (ui_menu_item){ TM_DELETE, "Delete tray...", tr.cur[0] != 0 && tr.ntrays > 1, false, NULL, 0 };
    items[n++] = (ui_menu_item){ TM_FOLDER, "Open Trays folder", true, false, NULL, 0 };
    tooltip_hide();
    ui_menu_popup(s, items, n, x, y, serial, tray_menu_chosen, NULL);
}

/* ------------------------------------------------------------ dropping */

struct drop_place { size_t at; unsigned gen; };

/* A shadow made: it takes place `at` (the others after it move along). */
static void made(void *arg, const cv *reply)
{
    struct drop_place *d = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *link = res && res->k == CV_TEXT ? res->s : NULL;
    if (d->gen == tr.gen && link) {
        char *t[MAX_OBJS + 1];
        int64_t px[MAX_OBJS + 1], py[MAX_OBJS + 1];
        size_t k = 0, at = d->at < tr.n ? d->at : tr.n;
        for (size_t i = 0; i < tr.n && k < MAX_OBJS; i++) {
            if (i == at) { t[k] = strdup(link); px[k] = (int64_t)k * ORDER_CELL; py[k++] = 0; }
            char a[PATH_MAX + 64];
            address(&tr.objs[i], a, sizeof a);
            t[k] = strdup(a); px[k] = (int64_t)k * ORDER_CELL; py[k++] = 0;
        }
        if (at >= tr.n) { t[k] = strdup(link); px[k] = (int64_t)k * ORDER_CELL; py[k++] = 0; }
        svc_place(tr.cur_dir, (const char *const *)t, px, py, k, ignore, NULL);
        for (size_t i = 0; i < k; i++) free(t[i]);
    } else if (!link) {
        fprintf(stderr, "osd-shell: TheBar's tray: no shadow made: %s\n", reply ? cv_text(reply, "message") : "no osdd");
    }
    free(d);
    reload_objs();
}

static bool in_tray(const char *path)
{
    for (size_t i = 0; i < tr.n; i++) {
        const struct tray_obj *o = &tr.objs[i];
        if (o->target[0] && !strcmp(o->target, path)) return true;
        if (!strncmp(path, "vobj:", 5) && o->name[0] == '\x01' && !strcmp(o->name + 1, path)) return true;
    }
    return false;
}

void trays_drop(const char *data, size_t len, size_t at)
{
    if (!tr.cur[0]) return;
    const char *p = data, *end = data + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[PATH_MAX + 64], path[PATH_MAX + 64];
        snprintf(line, sizeof line, "%.*s", (int)l, p);
        line[strcspn(line, "\t\r")] = 0;
        if (!strncmp(line, "file://", 7)) {   /* a URI: file:///a%20b */
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
        if ((path[0] == '/' || !strncmp(path, "vobj:", 5)) && !in_tray(path)) {
            struct drop_place *d = calloc(1, sizeof *d);
            d->at = at++;
            d->gen = tr.gen;
            svc_shadow(path, tr.cur_dir, made, d);
        }
        p += l + 1;
    }
}
