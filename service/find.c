/* Find: objects by name under a folder (find.h). */
#define _GNU_SOURCE
#include "find.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BATCH 64
#define BATCH_MS 150
#define MAX_DEPTH 64

struct find {
    struct find *next;            /* the running searches (loop thread) */
    int64_t id;
    char folder[PATH_MAX];
    char pattern[256];
    bool subfolders;
    wps_find_kind kind;
    size_t limit;
    wps_find_fn fn;
    void *arg;
    atomic_bool cancel;
};

struct batch {
    struct find *f;
    char **paths;
    const char **types;
    size_t n, cap;
    bool done, truncated;
    uint64_t scanned;
    int err;
};

static wpo_loop *loop;
static struct find *running;
static int64_t next_id = 1;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;
static int threads;               /* search threads still running */

bool wps_find_match(const char *name, const char *pattern)
{
    if (!*pattern) return true;
    if (strpbrk(pattern, "*?[")) return fnmatch(pattern, name, FNM_CASEFOLD | FNM_PERIOD) == 0;
    return strcasestr(name, pattern) != NULL;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ------------------------------------------------------------ loop side */

static void batch_free(struct batch *b)
{
    for (size_t i = 0; i < b->n; i++) free(b->paths[i]);
    free(b->paths);
    free(b->types);
    free(b);
}

static void deliver(void *arg)
{
    struct batch *b = arg;
    struct find *f = b->f;
    wps_found *items = calloc(b->n ? b->n : 1, sizeof *items);
    for (size_t i = 0; i < b->n; i++) items[i] = (wps_found){ b->paths[i], b->types[i] };
    f->fn(f->arg, f->id, items, b->n, b->done, b->scanned, b->truncated, b->err);
    free(items);
    if (b->done) {
        for (struct find **p = &running; *p; p = &(*p)->next)
            if (*p == f) { *p = f->next; break; }
        free(f);
    }
    batch_free(b);
}

bool wps_find_cancel(int64_t id)
{
    for (struct find *f = running; f; f = f->next)
        if (f->id == id) { atomic_store(&f->cancel, true); return true; }
    return false;
}

/* ---------------------------------------------------------- the thread */

struct walk {
    struct find *f;
    struct batch *b;
    double last_post;
    uint64_t scanned;
    size_t found;
};

static struct batch *batch_new(struct find *f)
{
    struct batch *b = calloc(1, sizeof *b);
    b->f = f;
    return b;
}

static void post(struct walk *w, bool done)
{
    w->b->scanned = w->scanned;
    w->b->done = done;
    wpo_loop_post(loop, WPO_Q_RESULTS, "find results", deliver, w->b);
    w->b = done ? NULL : batch_new(w->f);
    w->last_post = now_ms();
}

static void add(struct walk *w, const char *path, const char *type)
{
    struct batch *b = w->b;
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : BATCH;
        b->paths = realloc(b->paths, b->cap * sizeof *b->paths);
        b->types = realloc(b->types, b->cap * sizeof *b->types);
    }
    b->paths[b->n] = strdup(path);
    b->types[b->n++] = type;
    w->found++;
    if (b->n >= BATCH) post(w, false);
}

static bool stopped(struct walk *w)
{
    if (atomic_load(&w->f->cancel)) return true;
    if (w->found >= w->f->limit) { w->b->truncated = true; return true; }
    return false;
}

static void walk_dir(struct walk *w, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) {
        if (!depth) w->b->err = errno;
        return;
    }
    struct find *f = w->f;
    bool hidden_too = f->pattern[0] == '.';
    /* Subfolders after the folder's own entries: nearer matches first. */
    char **subs = NULL;
    size_t nsubs = 0, capsubs = 0;
    struct dirent *e;
    while ((e = readdir(d)) && !stopped(w)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && !hidden_too) continue;
        w->scanned++;
        unsigned char t = e->d_type;
        struct stat st;
        bool have_st = false;
        if (t == DT_UNKNOWN && fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            have_st = true;
            t = S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : S_ISLNK(st.st_mode) ? DT_LNK : DT_UNKNOWN;
        }
        char path[PATH_MAX];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", strcmp(dir, "/") ? dir : "", e->d_name) >= sizeof path) continue;
        if (wps_find_match(e->d_name, f->pattern)) {
            bool want = true;
            if (f->kind == WPS_FIND_FOLDER) want = t == DT_DIR;
            else if (f->kind == WPS_FIND_FILE) want = t == DT_REG;
            else if (f->kind == WPS_FIND_PROGRAM) {
                size_t nl = strlen(e->d_name);
                if (t != DT_REG) want = false;
                else if (nl > 8 && !strcmp(e->d_name + nl - 8, ".desktop")) want = true;
                else want = (have_st || fstatat(dirfd(d), e->d_name, &st, 0) == 0) && (st.st_mode & 0111);
            }
            if (want) add(w, path, t == DT_DIR ? "folder" : t == DT_REG ? "file" : t == DT_LNK ? "link" : "other");
        }
        if (f->subfolders && t == DT_DIR && depth < MAX_DEPTH) {
            if (nsubs == capsubs) {
                capsubs = capsubs ? capsubs * 2 : 16;
                subs = realloc(subs, capsubs * sizeof *subs);
            }
            subs[nsubs++] = strdup(path);
        }
        if (w->b->n && now_ms() - w->last_post > BATCH_MS) post(w, false);
    }
    closedir(d);
    for (size_t i = 0; i < nsubs; i++) {
        if (!stopped(w)) walk_dir(w, subs[i], depth + 1);
        free(subs[i]);
    }
    free(subs);
}

static void *thread_main(void *arg)
{
    struct find *f = arg;
    struct walk w = { .f = f, .b = batch_new(f), .last_post = now_ms() };
    walk_dir(&w, f->folder, 0);
    post(&w, true);
    pthread_mutex_lock(&mu);
    if (--threads == 0) pthread_cond_broadcast(&idle);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int64_t wps_find_start(const char *folder, const char *pattern, bool subfolders, wps_find_kind kind, size_t limit,
                       wps_find_fn fn, void *arg)
{
    struct find *f = calloc(1, sizeof *f);
    f->id = next_id++;
    snprintf(f->folder, sizeof f->folder, "%s", folder);
    snprintf(f->pattern, sizeof f->pattern, "%s", pattern ? pattern : "");
    f->subfolders = subfolders;
    f->kind = kind;
    f->limit = limit ? limit : 1000;
    f->fn = fn;
    f->arg = arg;
    atomic_init(&f->cancel, false);
    f->next = running;
    running = f;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_mutex_lock(&mu);
    threads++;
    pthread_mutex_unlock(&mu);
    if (pthread_create(&th, &at, thread_main, f)) {
        pthread_mutex_lock(&mu);
        threads--;
        pthread_mutex_unlock(&mu);
        /* no thread: the search ends at once, empty */
        struct batch *b = batch_new(f);
        b->done = true;
        b->err = EAGAIN;
        wpo_loop_post(loop, WPO_Q_RESULTS, "find results", deliver, b);
    }
    pthread_attr_destroy(&at);
    return f->id;
}

void wps_find_init(wpo_loop *l) { loop = l; }

void wps_find_shutdown(void)
{
    for (struct find *f = running; f; f = f->next) atomic_store(&f->cancel, true);
    pthread_mutex_lock(&mu);
    while (threads) pthread_cond_wait(&idle, &mu);
    pthread_mutex_unlock(&mu);
}
