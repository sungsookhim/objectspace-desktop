#define _GNU_SOURCE
#include "listing.h"
#ifndef WPS_ICONDIR
#define WPS_ICONDIR "/usr/share/wps/icons"
#endif
#include "trash.h"
#include "launch.h"
#include "objects.h"
#include "shadows.h"

#include <dirent.h>
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

static struct listing *find(const char *path)
{
    for (struct listing *l = listings; l; l = l->next)
        if (strcmp(l->path, path) == 0) return l;
    return NULL;
}

bool wps_listing_has(const char *folder, const char *name)
{
    struct listing *l = find(folder);
    for (size_t i = 0; l && i < l->n; i++)
        if (!strcmp(l->names[i], name)) return true;
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
            if (!e->title[0] && tg.path[0]) snprintf(e->title, sizeof e->title, "%s", strrchr(tg.path, '/') + 1);
            /* the Trash Can's shadow: empty or full, unless it has its own */
            char own[sizeof e->icon] = "";
            if (op) { t = NULL; if (wps_props_str(op, "icon", &t)) snprintf(own, sizeof own, "%s", t); free(t); }
            if (!own[0] && S_ISDIR(tg.st.st_mode) && wps_is_trash_files_dir(tg.path)) wps_trash_icon(tg.path, e->icon, sizeof e->icon);
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

static void default_icon(const char *name, char *out, size_t n)
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
    if (!out[0]) default_icon(full ? "trash-full.png" : "trash-empty.png", out, n);
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
    if (c->last) { l->complete = true; l->loading = false; l->err = c->err; }
    free(c->names);
    free(c->types);
    free(c);
    serve(l);
}

struct read_job { struct listing *l; unsigned gen; char path[PATH_MAX]; };

/* Central objects are listed as "\x01vobj:<id>": not a file name (no file
 * name starts with \x01), and the address to use for them. */
struct central_add { struct chunk **c; struct read_job *j; size_t size; };

static void add_central(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct central_add *ca = user;
    struct chunk *c = *ca->c;
    char name[64];
    snprintf(name, sizeof name, "\x01vobj:%s", id);
    c->names[c->n] = strdup(name);
    c->types[c->n] = DT_UNKNOWN;
    if (++c->n == CHUNK) {
        wpo_loop_post(loop, WPO_Q_RESULTS, "listing", chunk_done, c);
        c = calloc(1, sizeof *c);
        c->l = ca->j->l;
        c->gen = ca->j->gen;
        c->names = malloc(CHUNK * sizeof *c->names);
        c->types = malloc(CHUNK);
        *ca->c = c;
    }
}

static void read_work(void *arg)   /* worker: the whole directory, posted in chunks */
{
    struct read_job *j = arg;
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
        c->names[c->n] = strdup(de->d_name);
        c->types[c->n] = de->d_type;
        if (++c->n == size) {
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
        struct central_add ca = { &c, j, size };
        wps_central_in(fld.vol, fld.id, add_central, &ca);
    }
    wps_loaded_free(&fld);
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
    wpo_defer(loop, "read folder", read_work, free, j);
}

void wps_listing_request(const char *folder, size_t offset, size_t count, bool details,
                         wps_page_fn reply, void *arg)
{
    struct listing *l = find(folder);
    if (!l) {
        l = calloc(1, sizeof *l);
        snprintf(l->path, sizeof l->path, "%s", folder);
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
    struct listing *l = find(folder);
    if (l) l->stale = true;
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
