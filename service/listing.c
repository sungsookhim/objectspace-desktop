#define _GNU_SOURCE
#include "listing.h"
#ifndef WPS_ICONDIR
#define WPS_ICONDIR "/usr/share/wps/icons"
#endif
#include "trash.h"
#include "udisks.h"
#include "launch.h"
#include "objects.h"
#include "shadows.h"
#include "wps_state.h"

#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK 4096

struct waiter {
    struct waiter *next;
    size_t offset, count;
    bool details;
    wps_page_fn reply;
    void *arg;
};

struct listing {
    struct listing *next;
    char path[PATH_MAX];
    char **names;
    unsigned char *types;
    size_t n, cap;
    bool complete, loading, stale;
    int err;
    uint64_t version;        /* bumped by each edit; a new, unused one for each read */
    bool sorted;             /* in the folder's sort order (v0.3-design 4): additions read it again */
    /* a window's own listing of the folder (v0.3-design 6): only names
     * containing `filter`, or without the hidden (dot) ones; read again on
     * any change. The folder's plain listing has neither. */
    char filter[128];
    bool hide;
    long last_index;         /* where the last edit was (see wps_listing_last_index) */
    unsigned gen;            /* bumped when re-read; chunks of an old read are dropped */
    struct waiter *waiters;
};

static wpo_loop *loop;
static struct listing *listings;

size_t wps_listing_count(void)
{
    size_t n = 0;
    for (struct listing *l = listings; l; l = l->next) n++;
    return n;
}

static struct listing *find_v(const char *path, const char *filter, bool hide)
{
    for (struct listing *l = listings; l; l = l->next)
        if (!strcmp(l->path, path) && !strcmp(l->filter, filter) && l->hide == hide) return l;
    return NULL;
}

/* The folder's plain listing (the one changes follow in place). */
static struct listing *find(const char *path) { return find_v(path, "", false); }

bool wps_listing_has(const char *folder, const char *name)
{
    for (struct listing *l = listings; l; l = l->next) {
        if (strcmp(l->path, folder)) continue;
        for (size_t i = 0; i < l->n; i++)
            if (!strcmp(l->names[i], name)) return true;
    }
    return false;
}

static void clear(struct listing *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->names[i]);
    free(l->names);
    free(l->types);
    l->names = NULL;
    l->types = NULL;
    l->n = l->cap = 0;
}

/* ------------------------------------------------------------- details */

/* Details for a page are read in parallel slices (one per worker), so a
 * cold disk serves several reads at once; the reply goes when all are in. */
#define SLICES 8

struct detail_job {
    char folder[PATH_MAX];
    wps_page page;
    wps_page_fn reply;
    void *arg;
    int pending;            /* slices not yet done (loop thread only) */
    bool in_trash;          /* a trash's files folder: titles from its info files */
};

struct detail_slice { struct detail_job *j; size_t from, to; };

static const wps_props *props_of(const wps_loaded *ld, const char *cls)
{
    for (int i = 0; i < ld->nc; i++)
        if (!strcmp(ld->c[i].cls, cls)) return &ld->c[i].props;
    return NULL;
}

static void drive_icon(const wps_loaded *ld, char *out, size_t n);
static bool special_icon(const char *path, char *out, size_t n);

static void central_details(wps_entry *e)
{
    wps_loaded ld;
    wps_load(e->name + 1, false, &ld);
    e->detailed = true;
    e->size = e->mtime = 0;
    if (ld.err) { wps_loaded_free(&ld); return; }
    memcpy(e->id, ld.id, sizeof e->id);
    const wps_props *op = props_of(&ld, "WPObject"), *wp = props_of(&ld, "WPS");
    char *t = NULL;
    if (op && wps_props_str(op, "title", &t)) snprintf(e->title, sizeof e->title, "%s", t);
    free(t);
    t = NULL;
    if (op && wps_props_str(op, "icon", &t)) snprintf(e->icon, sizeof e->icon, "%s", t);
    free(t);
    int64_t tv;
    if (op && wps_props_int(op, "template", &tv) && tv) e->is_template = true;
    t = NULL;
    if (wp && wps_props_str(wp, "makes", &t)) { snprintf(e->makes, sizeof e->makes, "%s", t); e->is_template = true; }
    free(t);
    if (!e->icon[0] && !strcmp(ld.obj_class, "WPDrive")) drive_icon(&ld, e->icon, sizeof e->icon);
    if (ld.shadow_of[0]) {
        e->shadow = true;
        e->target_type = DT_UNKNOWN;
        char tl[WPS_ID_LEN + 8];
        snprintf(tl, sizeof tl, "id:%s", ld.shadow_of);
        wps_loaded tg;
        wps_load(tl, false, &tg);
        if (!tg.err) {
            snprintf(e->target_path, sizeof e->target_path, "%s", tg.path[0] ? tg.path : tl);
            e->target_type = S_ISDIR(tg.st.st_mode) ? DT_DIR : DT_REG;
            const wps_props *tp = props_of(&tg, "WPObject");
            t = NULL;
            if (!e->title[0] && tp && wps_props_str(tp, "title", &t)) snprintf(e->title, sizeof e->title, "%s", t);
            free(t);
            t = NULL;
            if (!e->icon[0] && tp && wps_props_str(tp, "icon", &t)) snprintf(e->icon, sizeof e->icon, "%s", t);
            free(t);
            if (!e->title[0] && tg.desk_name[0]) snprintf(e->title, sizeof e->title, "%s", tg.desk_name);   /* a program: its Name */
            if (!e->title[0] && tg.path[0]) snprintf(e->title, sizeof e->title, "%s", strrchr(tg.path, '/') + 1);
            /* the Trash Can's shadow: empty or full, unless it has its own */
            char own[sizeof e->icon] = "";
            if (op) { t = NULL; if (wps_props_str(op, "icon", &t)) snprintf(own, sizeof own, "%s", t); free(t); }
            /* (or its original's own: chosen in the original's notebook) */
            if (!own[0] && tp) { t = NULL; if (wps_props_str(tp, "icon", &t)) snprintf(own, sizeof own, "%s", t); free(t); }
            if (!own[0] && S_ISDIR(tg.st.st_mode) && wps_is_trash_files_dir(tg.path)) wps_trash_icon(tg.path, e->icon, sizeof e->icon);
            else if (!own[0] && S_ISDIR(tg.st.st_mode) && special_icon(tg.path, e->icon, sizeof e->icon)) {}
            else if (!own[0] && !strcmp(tg.obj_class, "WPDrive")) drive_icon(&tg, e->icon, sizeof e->icon);
        }
        wps_loaded_free(&tg);
    }
    wps_loaded_free(&ld);
}

/* ---- the Trash Can's icon: empty or full (v0.2-design 4) */

static void any_central(const char *vol, const char *id, void *user) { (void)vol; (void)id; *(bool *)user = true; }

bool wps_trash_full(const char *files)
{
    bool full = false;
    DIR *d = opendir(files);
    for (struct dirent *e; d && (e = readdir(d)) && !full;) full = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    if (d) closedir(d);
    if (!full) {   /* trashed shadows and other central objects */
        wps_loaded ld;
        wps_load(files, false, &ld);
        if (!ld.err && ld.id[0]) wps_central_in(ld.vol, ld.id, any_central, &full);
        wps_loaded_free(&ld);
    }
    return full;
}

void wps_default_icon(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", WPS_ICONDIR, name);
    if (access(out, R_OK) == 0) return;
    /* not installed: the tree's (build/wpsd -> data/icons) */
    char exe[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (l <= 0) return;
    exe[l] = 0;
    for (int up = 0; up < 2; up++) { char *s = strrchr(exe, '/'); if (s) *s = 0; }
    char t[PATH_MAX + 64];
    snprintf(t, sizeof t, "%s/data/icons/%s", exe, name);
    if (access(t, R_OK) == 0) snprintf(out, n, "%s", t);
}

void wps_trash_icon(const char *files, char *out, size_t n)
{
    bool full = wps_trash_full(files);
    wps_props ps;
    char *t = NULL;
    out[0] = 0;
    /* its own: Icon (WPObject's) when empty, Full icon (WPTrashCan's) when full */
    if (wps_attr_read(files, full ? "WPTrashCan" : "WPObject", &ps) == WPS_A_OK) {
        if (wps_props_str(&ps, full ? "fullicon" : "icon", &t) && t[0]) snprintf(out, n, "%s", t);
        free(t);
        wps_props_free(&ps);
    }
    if (!out[0]) wps_default_icon(full ? "trash-full.png" : "trash-empty.png", out, n);
}

/* A drive's icon: its kind's, lit when mounted (v0.2-design 5). */
static void drive_icon(const wps_loaded *ld, char *out, size_t n)
{
    const wps_props *dp = props_of(ld, "WPDrive");
    char *u = NULL;
    wps_drive_info d;
    char name[64];
    if (dp && wps_props_str(dp, "uuid", &u) && wps_udisks_find(u, &d))
        snprintf(name, sizeof name, "drive-%s%s.png", d.kind, d.mount[0] ? "" : "-off");
    else snprintf(name, sizeof name, "drive-disk-off.png");   /* not present */
    free(u);
    wps_default_icon(name, out, n);
}

/* WPS's special folders' own icons (Computer, ...), by path. */
static struct { char path[PATH_MAX]; char png[64]; } special[8];
static int nspecial;
static pthread_mutex_t special_mu = PTHREAD_MUTEX_INITIALIZER;

void wps_listing_folder_icon(const char *path, const char *png)
{
    pthread_mutex_lock(&special_mu);
    int i = 0;
    while (i < nspecial && strcmp(special[i].path, path)) i++;
    if (i == nspecial && nspecial < 8) nspecial++;
    if (i < 8) {
        snprintf(special[i].path, sizeof special[i].path, "%s", path);
        snprintf(special[i].png, sizeof special[i].png, "%s", png);
    }
    pthread_mutex_unlock(&special_mu);
}

static bool special_icon(const char *path, char *out, size_t n)
{
    char png[64] = "";
    pthread_mutex_lock(&special_mu);
    for (int i = 0; i < nspecial; i++) if (!strcmp(special[i].path, path)) snprintf(png, sizeof png, "%s", special[i].png);
    pthread_mutex_unlock(&special_mu);
    if (png[0]) wps_default_icon(png, out, n);
    return png[0] != 0;
}

void wps_listing_shown_icon(const char *path, bool dir, bool program, char *out, size_t n)
{
    if (dir && path && *path && wps_is_trash_files_dir(path)) { wps_trash_icon(path, out, n); return; }
    if (dir && path && *path && special_icon(path, out, n)) return;
    snprintf(out, n, "%s", dir ? "kind:folder" : program ? "kind:program" : "kind:file");
}

static void detail_work(void *arg)
{
    struct detail_slice *sl = arg;
    struct detail_job *j = sl->j;
    for (size_t i = sl->from; i < sl->to; i++) {
        wps_entry *e = &j->page.items[i];
        char p[PATH_MAX + NAME_MAX + 2];
        snprintf(p, sizeof p, "%s/%s", j->folder, e->name);
        struct stat st;
        if (lstat(p, &st) == 0) {
            e->size = st.st_size;
            e->mtime = st.st_mtim.tv_sec;
            e->detailed = true;
        }
        if (!wps_attr_get_id(p, e->id)) e->id[0] = 0;
        wps_props ps;
        char *t = NULL;
        if (e->id[0] && wps_attr_read(p, "WPObject", &ps) == WPS_A_OK) {
            if (wps_props_str(&ps, "title", &t)) snprintf(e->title, sizeof e->title, "%s", t);
            free(t);
            t = NULL;
            if (wps_props_str(&ps, "icon", &t)) snprintf(e->icon, sizeof e->icon, "%s", t);
            free(t);
            int64_t tv;
            if (wps_props_int(&ps, "template", &tv) && tv) e->is_template = true;
            wps_props_free(&ps);
        }
        if (e->name[0] != '\x01' && !wps_is_lock_file(e->name) && wps_in_templates(p)) e->is_template = true;
        if (e->name[0] == '\x01') {   /* a central object */
            central_details(e);
            if (e->shadow && e->target_type == DT_REG) e->locked = wps_lock_file_present(e->target_path);
            continue;
        }
        if (e->type == DT_LNK) {
            /* A plain link shows what it points to: its kind, and where. */
            struct stat ts;
            e->target_type = DT_UNKNOWN;
            if (stat(p, &ts) == 0 && realpath(p, e->target_path)) {
                e->target_type = S_ISDIR(ts.st_mode) ? DT_DIR : DT_REG;
                if (S_ISREG(ts.st_mode)) e->locked = wps_lock_file_present(e->target_path);
            } else {
                e->target_path[0] = 0;   /* dangling */
            }
        }
        if (e->type == DT_REG) e->locked = wps_lock_file_present(p);
        if (e->type == DT_DIR && wps_is_trash_files_dir(p)) wps_trash_icon(p, e->icon, sizeof e->icon);
        else if (e->type == DT_DIR && !e->icon[0]) special_icon(p, e->icon, sizeof e->icon);
        /* in the Trash Can: its name before it was deleted (trash names
         * may have a number added) */
        if (!e->title[0] && j->in_trash) {
            char orig[PATH_MAX];
            if (wps_trash_info(p, orig, sizeof orig, NULL, 0) == 0) {
                const char *b = strrchr(orig, '/');
                snprintf(e->title, sizeof e->title, "%s", b ? b + 1 : orig);
            }
        }
        size_t nl = strlen(e->name);
        char tid[WPS_ID_LEN + 1];
        if (e->type == DT_REG && nl > 6 && !strcmp(e->name + nl - 6, ".wpobj") &&
            wps_shadow_read(p, tid, e->target_path, sizeof e->target_path)) {
            /* A shadow shows its target: its title (or name), kind and path. */
            e->shadow = true;
            wps_shadows_file_seen(p);
            char hid[WPS_ID_LEN + 1];
            struct stat ts;
            if (e->target_path[0] && wps_attr_get_id(e->target_path, hid) && !strcmp(hid, tid) && stat(e->target_path, &ts) == 0) {
                e->target_type = S_ISDIR(ts.st_mode) ? DT_DIR : DT_REG;
                t = NULL;
                if (wps_attr_read(e->target_path, "WPObject", &ps) == WPS_A_OK) {
                    if (wps_props_str(&ps, "title", &t)) snprintf(e->title, sizeof e->title, "%s", t);
                    free(t);
                    t = NULL;   /* the target's icon, unless the shadow has its own */
                    if (!e->icon[0] && wps_props_str(&ps, "icon", &t)) snprintf(e->icon, sizeof e->icon, "%s", t);
                    free(t);
                    wps_props_free(&ps);
                }
            } else {
                e->target_type = DT_UNKNOWN;   /* not where it was: the service finds it by ID when opened */
            }
            e->locked = e->target_type == DT_REG && wps_lock_file_present(e->target_path);
            if (!e->title[0] && e->target_path[0]) {
                const char *b = strrchr(e->target_path, '/');
                snprintf(e->title, sizeof e->title, "%s", b ? b + 1 : e->target_path);
            }
        }
    }
}


static void detail_done(void *arg)
{
    struct detail_slice *sl = arg;
    struct detail_job *j = sl->j;
    free(sl);
    if (--j->pending) return;
    j->reply(j->arg, &j->page);
    free(j->page.items);
    free(j);
}

/* Answer a waiter whose range is available (or the folder is complete). */
static void answer(struct listing *l, struct waiter *w)
{
    wps_page pg = { .err = l->err, .total = l->n, .complete = l->complete, .offset = w->offset,
                    .version = l->complete && !l->stale ? l->version : 0 };
    size_t end = w->offset + w->count;
    if (end > l->n) end = l->n;
    pg.n = w->offset < end ? end - w->offset : 0;
    pg.items = calloc(pg.n ? pg.n : 1, sizeof *pg.items);
    for (size_t i = 0; i < pg.n; i++) {
        snprintf(pg.items[i].name, sizeof pg.items[i].name, "%s", l->names[w->offset + i]);
        pg.items[i].type = l->types[w->offset + i];
    }
    if (w->details && pg.n) {
        struct detail_job *j = calloc(1, sizeof *j);
        snprintf(j->folder, sizeof j->folder, "%s", l->path);
        j->in_trash = wps_is_trash_files_dir(l->path);
        j->page = pg;
        j->reply = w->reply;
        j->arg = w->arg;
        size_t slices = pg.n < SLICES ? pg.n : SLICES, per = (pg.n + slices - 1) / slices;
        j->pending = 0;
        for (size_t from = 0; from < pg.n; from += per) j->pending++;
        for (size_t from = 0; from < pg.n; from += per) {
            struct detail_slice *sl = calloc(1, sizeof *sl);
            sl->j = j;
            sl->from = from;
            sl->to = from + per < pg.n ? from + per : pg.n;
            wpo_defer(loop, "listing details", detail_work, detail_done, sl);
        }
    } else {
        w->reply(w->arg, &pg);
        free(pg.items);
    }
    free(w);
}

static void serve(struct listing *l)
{
    for (struct waiter **pp = &l->waiters; *pp;) {
        struct waiter *w = *pp;
        if (l->complete || l->err || w->offset + w->count <= l->n) {
            *pp = w->next;
            answer(l, w);
        } else {
            pp = &w->next;
        }
    }
}

/* ------------------------------------------------------------- reading */

struct chunk {
    struct listing *l;
    unsigned gen;
    char **names;
    unsigned char *types;
    size_t n;
    bool last;
    bool sorted;             /* the whole folder, sorted (one chunk) */
    int err;
};

static void chunk_done(void *arg)   /* loop thread */
{
    struct chunk *c = arg;
    struct listing *l = c->l;
    bool alive = false;
    for (struct listing *x = listings; x; x = x->next) alive |= x == l;
    if (!alive || c->gen != l->gen) {
        for (size_t i = 0; i < c->n; i++) free(c->names[i]);
        free(c->names);
        free(c->types);
        free(c);
        return;
    }
    if (l->n + c->n > l->cap) {
        size_t ncap = l->cap ? l->cap : CHUNK;
        while (ncap < l->n + c->n) ncap *= 2;
        l->names = realloc(l->names, ncap * sizeof *l->names);
        l->types = realloc(l->types, ncap);
        l->cap = ncap;
    }
    if (c->n) {   /* an empty folder's only chunk: nothing to copy, and no array yet */
        memcpy(l->names + l->n, c->names, c->n * sizeof *c->names);
        memcpy(l->types + l->n, c->types, c->n);
    }
    l->n += c->n;
    if (c->last) { l->complete = true; l->loading = false; l->err = c->err; l->sorted = c->sorted; }
    free(c->names);
    free(c->types);
    free(c);
    serve(l);
}

struct read_job {
    struct listing *l;
    unsigned gen;
    char path[PATH_MAX];
    char filter[128];
    bool hide;
    int key;       /* the folder's sort, from its object in memory (-2: not loaded, read it) */
    bool desc;
};

/* Central objects are listed as "\x01vobj:<id>": not a file name (no file
 * name starts with \x01), and the address to use for them. */
struct central_add { struct chunk **c; struct read_job *j; size_t size; size_t *whole_cap; /* sorted: grow, do not post */ };

static void add_central(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct central_add *ca = user;
    struct chunk *c = *ca->c;
    char name[64];
    snprintf(name, sizeof name, "\x01vobj:%s", id);
    if (ca->whole_cap && c->n == *ca->whole_cap) {
        *ca->whole_cap *= 2;
        c->names = realloc(c->names, *ca->whole_cap * sizeof *c->names);
        c->types = realloc(c->types, *ca->whole_cap);
    }
    c->names[c->n] = strdup(name);
    c->types[c->n] = DT_UNKNOWN;
    if (++c->n == CHUNK && !ca->whole_cap) {
        wpo_loop_post(loop, WPO_Q_RESULTS, "listing", chunk_done, c);
        c = calloc(1, sizeof *c);
        c->l = ca->j->l;
        c->gen = ca->j->gen;
        c->names = malloc(CHUNK * sizeof *c->names);
        c->types = malloc(CHUNK);
        *ca->c = c;
    }
}

/* ---- a folder's sort order (its WPFolder sort_key: -1 none, 0 name,
 * 1 type, 2 size, 3 last write; sort_desc): folders first, then the key,
 * then the name. Central objects sort by their titles. */
struct sort_ent { char *name; unsigned char type; bool dir; char *key; int64_t size, mtime; };
struct sort_ctx { int key; bool desc; };

static const char *ext_of(const char *name)
{
    const char *d = strrchr(name, '.');
    return d && d != name ? d + 1 : "";
}

static int sort_cmp(const void *a, const void *b, void *arg)
{
    const struct sort_ent *x = a, *y = b;
    const struct sort_ctx *cx = arg;
    if (x->dir != y->dir) return x->dir ? -1 : 1;   /* folders first, either way */
    int r = 0;
    switch (cx->key) {
    case 1: r = strcasecmp(ext_of(x->key), ext_of(y->key)); break;
    case 2: r = x->size < y->size ? -1 : x->size > y->size; break;
    case 3: r = x->mtime < y->mtime ? -1 : x->mtime > y->mtime; break;
    default: break;
    }
    if (!r) r = strcasecmp(x->key, y->key);
    if (!r) r = strcmp(x->key, y->key);
    return cx->desc ? -r : r;
}

/* The chunk (every entry of the folder) in the folder's order. */
static void sort_chunk(struct chunk *c, const char *folder, int key, bool desc)
{
    struct sort_ent *v = calloc(c->n ? c->n : 1, sizeof *v);
    for (size_t i = 0; i < c->n; i++) {
        struct sort_ent *e = &v[i];
        e->name = c->names[i];
        e->type = c->types[i];
        char p[PATH_MAX + NAME_MAX + 2];
        snprintf(p, sizeof p, "%s/%s", folder, e->name);
        if (e->name[0] == '\x01') {   /* a central object: its title */
            wps_loaded ld;
            wps_load(e->name + 1, false, &ld);
            char *t = NULL;
            const wps_props *op = ld.err ? NULL : props_of(&ld, "WPObject");
            if (op && wps_props_str(op, "title", &t) && t) e->key = t;
            else free(t);
            wps_loaded_free(&ld);
        } else {
            struct stat st;
            bool need = key == 2 || key == 3 || e->type == DT_UNKNOWN || e->type == DT_LNK;
            if (need && stat(p, &st) == 0) {
                e->size = st.st_size;
                e->mtime = st.st_mtim.tv_sec;
                if (e->type != DT_LNK) e->type = S_ISDIR(st.st_mode) ? DT_DIR : DT_REG;
                e->dir = S_ISDIR(st.st_mode);
            } else {
                e->dir = e->type == DT_DIR;
            }
        }
        if (!e->key) e->key = strdup(e->name);
    }
    struct sort_ctx cx = { key, desc };
    qsort_r(v, c->n, sizeof *v, sort_cmp, &cx);
    for (size_t i = 0; i < c->n; i++) {
        c->names[i] = v[i].name;
        c->types[i] = v[i].type;
        free(v[i].key);
    }
    free(v);
    c->sorted = true;
}

/* Only the entries whose names (central objects: titles) contain `filter`,
 * any case. */
static void filter_chunk(struct chunk *c, const char *filter)
{
    size_t w = 0;
    for (size_t i = 0; i < c->n; i++) {
        bool keep;
        if (c->names[i][0] == '\x01') {
            wps_loaded ld;
            wps_load(c->names[i] + 1, false, &ld);
            char *t = NULL;
            const wps_props *op = ld.err ? NULL : props_of(&ld, "WPObject");
            keep = op && wps_props_str(op, "title", &t) && t && strcasestr(t, filter);
            free(t);
            wps_loaded_free(&ld);
        } else {
            keep = strcasestr(c->names[i], filter) != NULL;
        }
        if (keep) { c->names[w] = c->names[i]; c->types[w++] = c->types[i]; }
        else free(c->names[i]);
    }
    c->n = w;
}

static void read_work(void *arg)   /* worker: the whole directory, posted in chunks */
{
    struct read_job *j = arg;
    /* the folder's sort order: then it is read whole, sorted, and posted at once */
    int key = j->key;
    bool desc = j->desc;
    if (key == -2) {   /* (not in memory: as stored) */
        key = -1;
        wps_loaded fl;
        wps_load(j->path, false, &fl);
        const wps_props *fp = fl.err ? NULL : props_of(&fl, "WPFolder");
        int64_t v;
        if (fp && wps_props_int(fp, "sort_key", &v) && v >= 0 && v <= 3) key = (int)v;
        if (fp && wps_props_int(fp, "sort_desc", &v)) desc = v != 0;
        wps_loaded_free(&fl);
    }
    size_t whole_cap = CHUNK;   /* (sorted or filtered: one chunk that grows) */
    bool whole = key >= 0 || j->filter[0];
    DIR *d = opendir(j->path);
    /* Small chunks first, so the first page is ready after a few directory
     * blocks even when the disk cache is cold; then larger ones. */
    size_t size = 64;
    struct chunk *c = calloc(1, sizeof *c);
    c->l = j->l;
    c->gen = j->gen;
    c->names = malloc(CHUNK * sizeof *c->names);
    c->types = malloc(CHUNK);
    if (!d) {
        c->err = errno;
        c->last = true;
        wpo_loop_post(loop, WPO_Q_RESULTS, "listing", chunk_done, c);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strncmp(de->d_name, WPS_SHADOW_TMP, strlen(WPS_SHADOW_TMP))) continue;   /* a job's half-made item */
        if (wps_is_lock_file(de->d_name)) continue;   /* (shown as the in-use emphasis of what it locks) */
        if (j->hide && de->d_name[0] == '.') continue;
        if (whole && c->n == whole_cap) {
            whole_cap *= 2;
            c->names = realloc(c->names, whole_cap * sizeof *c->names);
            c->types = realloc(c->types, whole_cap);
        }
        c->names[c->n] = strdup(de->d_name);
        c->types[c->n] = de->d_type;
        if (++c->n == size && !whole) {
            if (size < CHUNK) size *= 4;
            wpo_loop_post(loop, WPO_Q_RESULTS, "listing", chunk_done, c);
            c = calloc(1, sizeof *c);
            c->l = j->l;
            c->gen = j->gen;
            c->names = malloc(CHUNK * sizeof *c->names);
            c->types = malloc(CHUNK);
        }
    }
    closedir(d);
    /* Then the central objects kept for this folder (by its object ID). */
    wps_loaded fld;
    wps_load(j->path, false, &fld);
    if (!fld.err && fld.id[0]) {
        struct central_add ca = { &c, j, size, whole ? &whole_cap : NULL };
        wps_central_in(fld.vol, fld.id, add_central, &ca);
    }
    wps_loaded_free(&fld);
    if (j->filter[0]) filter_chunk(c, j->filter);
    if (key >= 0) sort_chunk(c, j->path, key, desc);
    else if (whole) c->sorted = true;   /* (filtered: read again on changes, as a sorted one) */
    c->last = true;
    wpo_loop_post(loop, WPO_Q_RESULTS, "listing", chunk_done, c);
}

static uint64_t versions;   /* never reused: a client cannot mistake one read for another */

static void start_read(struct listing *l)
{
    clear(l);
    l->gen++;
    l->version = ++versions << 20;   /* room for a million edits before the next read's */
    l->complete = l->stale = false;
    l->err = 0;
    l->loading = true;
    struct read_job *j = calloc(1, sizeof *j);
    j->l = l;
    j->gen = l->gen;
    snprintf(j->path, sizeof j->path, "%s", l->path);
    snprintf(j->filter, sizeof j->filter, "%s", l->filter);
    j->hide = l->hide;
    /* the folder's sort, as it is now (its object's state may not be
     * written yet: a sort just chosen) */
    j->key = -2;
    sobj *so = wps_sobj_by_path(l->path);
    if (so && so->o) {
        int64_t v = -1;
        j->key = wps_state_get_int(so->o, "WPFolder", "sort_key", &v) && v >= 0 && v <= 3 ? (int)v : -1;
        v = 0;
        j->desc = wps_state_get_int(so->o, "WPFolder", "sort_desc", &v) && v;
    }
    wpo_defer(loop, "read folder", read_work, free, j);
}

void wps_listing_request(const char *folder, size_t offset, size_t count, bool details,
                         wps_page_fn reply, void *arg)
{
    wps_listing_request_v(folder, "", false, offset, count, details, reply, arg);
}

void wps_listing_request_v(const char *folder, const char *filter, bool hide, size_t offset, size_t count, bool details,
                           wps_page_fn reply, void *arg)
{
    if (!filter) filter = "";
    struct listing *l = find_v(folder, filter, hide);
    if (!l) {
        l = calloc(1, sizeof *l);
        snprintf(l->path, sizeof l->path, "%s", folder);
        snprintf(l->filter, sizeof l->filter, "%s", filter);
        l->hide = hide;
        l->next = listings;
        listings = l;
        start_read(l);
    } else if (l->stale && offset == 0 && !l->loading) {
        start_read(l);
    }
    struct waiter *w = calloc(1, sizeof *w);
    w->offset = offset;
    w->count = count ? count : 100;
    w->details = details;
    w->reply = reply;
    w->arg = arg;
    w->next = l->waiters;
    l->waiters = w;
    serve(l);
}

void wps_listing_invalidate(const char *folder)
{
    for (struct listing *l = listings; l; l = l->next)
        if (!strcmp(l->path, folder)) l->stale = true;
}

uint64_t wps_listing_version(const char *folder)
{
    struct listing *l = find(folder);
    return l && l->complete && !l->loading && !l->stale && !l->err ? l->version : 0;
}

static ssize_t index_of(struct listing *l, const char *name)
{
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->names[i], name)) return (ssize_t)i;
    return -1;
}

static void remove_at(struct listing *l, size_t i)
{
    free(l->names[i]);
    memmove(l->names + i, l->names + i + 1, (l->n - i - 1) * sizeof *l->names);
    memmove(l->types + i, l->types + i + 1, l->n - i - 1);
    l->n--;
    l->version++;
}

void wps_listing_apply(const char *folder, char kind, const char *name, const char *newname, unsigned char type)
{
    /* windows' own listings (filtered, without hidden ones): read again */
    for (struct listing *v = listings; v; v = v->next)
        if (!strcmp(v->path, folder) && (v->filter[0] || v->hide)) v->stale = true;
    struct listing *l = find(folder);
    if (!l) return;
    /* Mid-read, the reader may or may not have seen it: read again. */
    l->last_index = -1;
    if (!l->complete || l->loading || l->err) { l->stale = true; return; }
    if (l->stale) return;
    bool tmp = !strncmp(name, WPS_SHADOW_TMP, strlen(WPS_SHADOW_TMP));
    if (kind == 'r' && tmp) { kind = 'c'; name = newname; tmp = false; }   /* a job's item finished */
    if (kind == 'r' && !strncmp(newname, WPS_SHADOW_TMP, strlen(WPS_SHADOW_TMP))) kind = 'd';
    if (tmp) return;
    /* lock files are never listed (as when read) */
    if (kind == 'c' && wps_is_lock_file(name)) return;
    if (kind == 'r' && wps_is_lock_file(newname)) kind = 'd';
    if (kind == 'd' && wps_is_lock_file(name)) return;
    /* sorted: an addition or rename belongs somewhere in the order, which
     * windows cannot place: read again (removals follow by position) */
    if (l->sorted && kind != 'd') { l->stale = true; return; }
    ssize_t i = index_of(l, name);
    if (kind == 'c') {
        if (i >= 0) return;
        if (l->n == l->cap) {
            size_t ncap = l->cap ? l->cap * 2 : 64;
            char **nn = realloc(l->names, ncap * sizeof *nn);
            unsigned char *nt = nn ? realloc(l->types, ncap) : NULL;
            if (nn) l->names = nn;
            if (!nn || !nt) { l->stale = true; return; }
            l->types = nt;
            l->cap = ncap;
        }
        l->last_index = (long)l->n;
        l->names[l->n] = strdup(name);
        l->types[l->n++] = type;
        l->version++;
    } else if (kind == 'd') {
        if (i >= 0) { l->last_index = (long)i; remove_at(l, (size_t)i); }
    } else if (kind == 'r') {
        ssize_t over = index_of(l, newname);   /* renamed over another: that one goes */
        if (over >= 0 && over != i) {
            remove_at(l, (size_t)over);
            if (over < i) i--;
        }
        if (i < 0) {
            wps_listing_apply(folder, 'c', newname, NULL, type);
            return;
        }
        free(l->names[i]);
        l->names[i] = strdup(newname);
        l->last_index = (long)i;
        l->version++;
    }
}

long wps_listing_last_index(const char *folder)
{
    struct listing *l = find(folder);
    return l ? l->last_index : -1;
}

void wps_listing_init(wpo_loop *l) { loop = l; }

void wps_listing_shutdown(void)
{
    while (listings) {
        struct listing *l = listings;
        listings = l->next;
        clear(l);
        while (l->waiters) { struct waiter *w = l->waiters; l->waiters = w->next; free(w); }
        free(l);
    }
}
