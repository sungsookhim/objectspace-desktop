#define _GNU_SOURCE
#include "listing.h"

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
};

struct detail_slice { struct detail_job *j; size_t from, to; };

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
            wps_props_free(&ps);
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
    wps_page pg = { .err = l->err, .total = l->n, .complete = l->complete, .offset = w->offset };
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
    memcpy(l->names + l->n, c->names, c->n * sizeof *c->names);
    memcpy(l->types + l->n, c->types, c->n);
    l->n += c->n;
    if (c->last) { l->complete = true; l->loading = false; l->err = c->err; }
    free(c->names);
    free(c->types);
    free(c);
    serve(l);
}

struct read_job { struct listing *l; unsigned gen; char path[PATH_MAX]; };

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
    c->last = true;
    wpo_loop_post(loop, WPO_Q_RESULTS, "listing", chunk_done, c);
}

static void start_read(struct listing *l)
{
    clear(l);
    l->gen++;
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
