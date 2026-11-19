#define _GNU_SOURCE
#include "jobs.h"
#include "trash.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "attrs.h"
#include "shadows.h"

#define MAX_RUNNING 2
#define CHUNK_BYTES (64u << 20)

static wpo_loop *loop;
static wps_db *db;
static char owner[16];   /* start of the WPS identity: temporary names are per user */

/* ------------------------------------------------------------ selections */

struct selection {
    struct selection *next;
    int64_t id;
    char folder[PATH_MAX];
    bool all;               /* everything in the folder except `names`; else exactly `names` */
    char **names;
    size_t n;
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;   /* selections and the job list */
static struct selection *selections;
static int64_t next_sel = 1;

int64_t wps_select(const char *folder, bool all, const char *const *names, size_t n)
{
    struct selection *s = calloc(1, sizeof *s);
    if (!s) return -1;
    snprintf(s->folder, sizeof s->folder, "%s", folder);
    s->all = all;
    s->names = calloc(n + 1, sizeof *s->names);
    for (size_t i = 0; i < n; i++) {
        const char *nm = strrchr(names[i], '/');   /* full paths or plain names */
        s->names[s->n++] = strdup(nm ? nm + 1 : names[i]);
    }
    pthread_mutex_lock(&mu);
    s->id = next_sel++;
    s->next = selections;
    selections = s;
    pthread_mutex_unlock(&mu);
    return s->id;
}

/* "all" or "items", then one name per line: the journal's form. */
static char *encode_selection(const struct selection *s)
{
    size_t len = 8;
    for (size_t i = 0; i < s->n; i++) len += strlen(s->names[i]) + 1;
    char *out = malloc(len), *w = out;
    w += sprintf(w, "%s\n", s->all ? "all" : "items");
    for (size_t i = 0; i < s->n; i++) w += sprintf(w, "%s\n", s->names[i]);
    return out;
}

/* ------------------------------------------------------------------ jobs */

typedef enum { J_QUEUED, J_RUNNING, J_PAUSED, J_DECISION, J_DONE, J_FAILED, J_CANCELLED, J_INTERRUPTED, J_LOGOUT } jstate;
static const char *state_names[] = { "queued", "running", "paused", "needs-decision", "done", "failed", "cancelled",
                                     "interrupted", "paused-for-logout" };

struct job {
    struct job *next;
    int64_t id;
    char kind[8];
    struct selection sel;   /* a copy, owned by the job */
    char dest[PATH_MAX];
    pthread_t th;
    bool has_thread;
    pthread_cond_t cv;
    jstate state;
    bool want_cancel, want_pause, want_logout;
    uint64_t items_total, items_done, bytes_done;
    char current[NAME_MAX + 1];
    char conflict[NAME_MAX + 1];
    int decision;           /* 0 none yet, 1 skip, 2 rename */
    int decision_all;       /* a standing decision for the rest of the job */
    char **missing, **skipped;
    size_t nmissing, nskipped;
    char **not_trashed;     /* a trash job: items no trash can take (the GUI may delete them for good) */
    size_t nnot_trashed;
    char error[256];
    time_t journaled;
    uint64_t journaled_items;
    bool resuming;          /* skip items already copied completely */
    char **pos_names;       /* drop positions, by item name */
    int64_t *pos_x, *pos_y;
    size_t npos;
    char **placed;          /* final paths of positioned items, and where */
    int64_t *placed_x, *placed_y;
    size_t nplaced;
};

static struct job *jobs;
static int64_t next_job = 1;
static int running;
static pthread_cond_t idle_cv = PTHREAD_COND_INITIALIZER;   /* a job thread ended */

static struct job *job_by_id(int64_t id)
{
    for (struct job *j = jobs; j; j = j->next)
        if (j->id == id) return j;
    return NULL;
}

void (*wps_on_job)(int64_t id);

static void job_event(void *arg)   /* loop thread */
{
    int64_t *id = arg;
    if (wps_on_job) wps_on_job(*id);
    free(id);
}

static void journal(struct job *j, bool force)   /* caller holds mu */
{
    /* At most four times a second, so progress after a crash is close. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    time_t now = (time_t)(ts.tv_sec * 4 + ts.tv_nsec / 250000000);
    /* ... and every 256 items, so even a short-lived job's progress is kept. */
    if (!force && now == j->journaled && j->items_done - j->journaled_items < 256) return;
    j->journaled = now;
    j->journaled_items = j->items_done;
    /* Subscribers hear about it on the loop thread. */
    int64_t *eid = malloc(sizeof *eid);
    if (eid && loop) { *eid = j->id; wpo_loop_post(loop, WPO_Q_RESULTS, "job event", job_event, eid); }
    else free(eid);
    char *sel = encode_selection(&j->sel);
    char src[PATH_MAX], dest[PATH_MAX], kind[8], state[32];
    snprintf(src, sizeof src, "%s", j->sel.folder);
    snprintf(dest, sizeof dest, "%s", j->dest);
    snprintf(kind, sizeof kind, "%s", j->kind);
    snprintf(state, sizeof state, "%s", state_names[j->state]);
    int64_t items = (int64_t)j->items_done, bytes = (int64_t)j->bytes_done, id = j->id;
    pthread_mutex_unlock(&mu);   /* the database has its own lock */
    wps_db_job_put(db, id, kind, src, sel, dest, state, items, bytes);
    pthread_mutex_lock(&mu);
    free(sel);
}

/* Append to a name list: 16 slots, then doubling each time it fills (a
 * selection may hold a million names). */
static void push_name(char ***arr, size_t *n, const char *name)
{
    size_t k = *n;
    if (k == 0 || (k >= 16 && (k & (k - 1)) == 0)) {
        size_t cap = k < 16 ? 16 : k * 2;
        char **na = realloc(*arr, cap * sizeof *na);
        if (!na) return;
        *arr = na;
    }
    (*arr)[(*n)++] = strdup(name);
}

/* Between steps: honour pause and cancel. Returns false to stop. */
static bool checkpoint(struct job *j)
{
    pthread_mutex_lock(&mu);
    while (j->want_pause && !j->want_cancel && !j->want_logout) {
        j->state = J_PAUSED;
        journal(j, true);
        pthread_cond_wait(&j->cv, &mu);
    }
    if (j->state == J_PAUSED) j->state = J_RUNNING;
    bool go = !j->want_cancel && !j->want_logout;
    pthread_mutex_unlock(&mu);
    return go;
}

/* ------------------------------------------------------------- copying */

static void copy_xattrs(int sfd, int dfd, bool keep_id)
{
    char names[16384];
    ssize_t n = flistxattr(sfd, names, sizeof names);
    for (ssize_t o = 0; n > 0 && o < n; o += (ssize_t)strlen(names + o) + 1) {
        const char *nm = names + o;
        if (strncmp(nm, "user.", 5) != 0) continue;            /* SELinux labels etc. are the system's */
        if (!keep_id && strcmp(nm, WPS_ATTR_ID) == 0) continue;  /* a copy is a new object */
        char val[65536];
        ssize_t vl = fgetxattr(sfd, nm, val, sizeof val);
        if (vl >= 0) fsetxattr(dfd, nm, val, (size_t)vl, 0);
    }
}

static int copy_file(struct job *j, const char *src, const char *dst, const struct stat *st, bool keep_id)
{
    int s = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (s < 0) return errno;
    int d = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st->st_mode & 07777);
    if (d < 0) { int e = errno; close(s); return e; }
    int rc = 0;
    if (ioctl(d, FICLONE, s) == 0) {
        /* Shared blocks: instant on btrfs and XFS. */
        pthread_mutex_lock(&mu);
        j->bytes_done += (uint64_t)st->st_size;
        pthread_mutex_unlock(&mu);
    } else {
        off_t left = st->st_size;
        while (left > 0) {
            if (!checkpoint(j)) { rc = ECANCELED; break; }
            size_t want = left > (off_t)CHUNK_BYTES ? CHUNK_BYTES : (size_t)left;
            ssize_t c = copy_file_range(s, NULL, d, NULL, want, 0);
            if (c < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {
                static char buf[1 << 20];   /* per-thread use is serialised below */
                static pthread_mutex_t bm = PTHREAD_MUTEX_INITIALIZER;
                pthread_mutex_lock(&bm);
                ssize_t r = read(s, buf, sizeof buf < want ? sizeof buf : want);
                c = r > 0 ? write(d, buf, (size_t)r) : r;
                pthread_mutex_unlock(&bm);
            }
            if (c <= 0) { rc = c == 0 ? EIO : errno; break; }
            left -= c;
            pthread_mutex_lock(&mu);
            j->bytes_done += (uint64_t)c;
            journal(j, false);
            pthread_mutex_unlock(&mu);
        }
    }
    if (!rc) {
        copy_xattrs(s, d, keep_id);
        struct timespec ts[2] = { st->st_atim, st->st_mtim };
        futimens(d, ts);   /* last: a copy's mtime equal to the original's marks it complete */
    }
    close(s);
    close(d);
    if (rc) unlink(dst);
    return rc;
}

static int rm_tree_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st; (void)flag; (void)f;
    return remove(p) < 0 && errno != ENOENT ? -1 : 0;
}

static int rm_tree(const char *path)
{
    return nftw(path, rm_tree_cb, 32, FTW_DEPTH | FTW_PHYS) == 0 ? 0 : errno;
}

/* Copy a file, folder or symlink tree; never follows symlinks. */
static int copy_tree(struct job *j, const char *src, const char *dst, bool keep_id)
{
    struct stat st;
    if (lstat(src, &st) < 0) return errno;
    if (S_ISREG(st.st_mode)) return copy_file(j, src, dst, &st, keep_id);
    if (S_ISLNK(st.st_mode)) {
        char t[PATH_MAX];
        ssize_t n = readlink(src, t, sizeof t - 1);
        if (n < 0) return errno;
        t[n] = 0;
        return symlink(t, dst) == 0 ? 0 : errno;
    }
    if (!S_ISDIR(st.st_mode)) return 0;   /* devices and sockets are not copied */
    if (mkdir(dst, 0700) < 0) return errno;
    DIR *d = opendir(src);
    if (!d) return errno;
    struct dirent *de;
    int rc = 0;
    while (!rc && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char s2[PATH_MAX], d2[PATH_MAX];
        snprintf(s2, sizeof s2, "%s/%s", src, de->d_name);
        snprintf(d2, sizeof d2, "%s/%s", dst, de->d_name);
        rc = copy_tree(j, s2, d2, keep_id);
        if (rc == ENOENT) rc = 0;   /* vanished meanwhile: skip it */
    }
    closedir(d);
    if (!rc) {
        int sfd = open(src, O_RDONLY | O_DIRECTORY | O_CLOEXEC), dfd = open(dst, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (sfd >= 0 && dfd >= 0) copy_xattrs(sfd, dfd, keep_id);
        if (dfd >= 0) {
            fchmod(dfd, st.st_mode & 07777);
            struct timespec ts[2] = { st.st_atim, st.st_mtim };
            futimens(dfd, ts);
        }
        if (sfd >= 0) close(sfd);
        if (dfd >= 0) close(dfd);
    }
    return rc;
}

/* A name clash: wait for (or apply a standing) decision. Returns 1 skip,
 * 2 rename, 0 stop. */
static int decide(struct job *j, const char *name)
{
    pthread_mutex_lock(&mu);
    if (j->decision_all) { int d = j->decision_all; pthread_mutex_unlock(&mu); return d; }
    snprintf(j->conflict, sizeof j->conflict, "%s", name);
    j->decision = 0;
    j->state = J_DECISION;
    journal(j, true);
    while (!j->decision && !j->want_cancel && !j->want_logout) pthread_cond_wait(&j->cv, &mu);
    int d = j->want_cancel || j->want_logout ? 0 : j->decision;
    j->conflict[0] = 0;
    j->state = J_RUNNING;
    pthread_mutex_unlock(&mu);
    return d;
}

static void alt_name(const char *dir, const char *name, int k, char *out, size_t n)
{
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) snprintf(out, n, "%s/%.*s (%d)%s", dir, (int)(dot - name), name, k, dot);
    else snprintf(out, n, "%s/%s (%d)", dir, name, k);
}

/* Put `tmp` (or, for a same-filesystem move, the source itself) at
 * dest/name without replacing anything. Returns 0, or 1 if skipped. */
static void note_placed(struct job *j, const char *name, const char *final)
{
    pthread_mutex_lock(&mu);
    for (size_t i = 0; i < j->npos; i++)
        if (!strcmp(j->pos_names[i], name)) {
            j->placed = realloc(j->placed, (j->nplaced + 1) * sizeof *j->placed);
            j->placed_x = realloc(j->placed_x, (j->nplaced + 1) * sizeof *j->placed_x);
            j->placed_y = realloc(j->placed_y, (j->nplaced + 1) * sizeof *j->placed_y);
            j->placed[j->nplaced] = strdup(final);
            j->placed_x[j->nplaced] = j->pos_x[i];
            j->placed_y[j->nplaced] = j->pos_y[i];
            j->nplaced++;
            break;
        }
    pthread_mutex_unlock(&mu);
}

static int place(struct job *j, const char *from, const char *name, bool is_tmp)
{
    char target[PATH_MAX];
    snprintf(target, sizeof target, "%s/%s", j->dest, name);
    bool renaming = false;   /* once Rename is chosen, count up to a free name without asking again */
    for (int k = 2;; k++) {
        if (renameat2(AT_FDCWD, from, AT_FDCWD, target, RENAME_NOREPLACE) == 0) { note_placed(j, name, target); return 0; }
        if (errno != EEXIST) return -errno;
        if (renaming && k < 100000) { alt_name(j->dest, name, k, target, sizeof target); continue; }
        int d = decide(j, name);
        if (d == 1 || d == 0) {
            if (is_tmp) rm_tree(from);
            if (d == 1) {
                pthread_mutex_lock(&mu);
                push_name(&j->skipped, &j->nskipped, name);
                pthread_mutex_unlock(&mu);
            }
            return d == 1 ? 1 : -ECANCELED;
        }
        renaming = true;
        alt_name(j->dest, name, k, target, sizeof target);
    }
}

void (*wps_on_job_placed)(const char *dest, char **paths, int64_t *x, int64_t *y, size_t n);

struct placed_post { char dest[PATH_MAX]; char **paths; int64_t *x, *y; size_t n; };

static void placed_now(void *arg)   /* loop thread */
{
    struct placed_post *p = arg;
    if (wps_on_job_placed && p->n) wps_on_job_placed(p->dest, p->paths, p->x, p->y, p->n);
    for (size_t i = 0; i < p->n; i++) free(p->paths[i]);
    free(p->paths);
    free(p->x);
    free(p->y);
    free(p);
}

static void finish(struct job *j, jstate st)
{
    pthread_mutex_lock(&mu);
    if (j->nplaced && loop) {
        struct placed_post *p = calloc(1, sizeof *p);
        snprintf(p->dest, sizeof p->dest, "%s", j->dest);
        p->paths = j->placed;
        p->x = j->placed_x;
        p->y = j->placed_y;
        p->n = j->nplaced;
        j->placed = NULL;
        j->placed_x = j->placed_y = NULL;
        j->nplaced = 0;
        wpo_loop_post(loop, WPO_Q_RESULTS, "place dropped items", placed_now, p);
    }
    pthread_mutex_unlock(&mu);
    pthread_mutex_lock(&mu);
    j->state = st;
    journal(j, true);
    running--;
    j->has_thread = false;
    pthread_cond_broadcast(&idle_cv);
    pthread_mutex_unlock(&mu);
}

static void start_queued(void);

/* Trash: one rename per item into the trash of its filesystem (its ID,
 * settings and shadows go with it); items no trash can take are listed.
 * Delete: removed for good (with their info files, in a trash). */
static jstate trash_or_delete(struct job *j, char **items, size_t nitems)
{
    bool trash = !strcmp(j->kind, "trash");
    for (size_t i = 0; i < nitems; i++) {
        if (!checkpoint(j)) break;
        const char *name = items[i];
        char src[PATH_MAX], to[PATH_MAX];
        snprintf(src, sizeof src, "%s/%s", j->sel.folder, name);
        pthread_mutex_lock(&mu);
        snprintf(j->current, sizeof j->current, "%s", name);
        pthread_mutex_unlock(&mu);
        int rc;
        if (trash) {
            rc = wps_trash(src, to, sizeof to);
        } else {
            struct stat st;
            rc = lstat(src, &st) < 0 ? -errno : rm_tree(src) ? -errno : 0;
            if (rc == 0) wps_trash_forget_info(src);
        }
        pthread_mutex_lock(&mu);
        if (rc == -ENOENT) push_name(&j->missing, &j->nmissing, name);
        else if (rc == -EXDEV && trash) push_name(&j->not_trashed, &j->nnot_trashed, name);
        else if (rc < 0) {
            snprintf(j->error, sizeof j->error, "%s: %s", name, strerror(-rc));
            pthread_mutex_unlock(&mu);
            return J_FAILED;
        }
        j->items_done++;
        journal(j, false);
        pthread_mutex_unlock(&mu);
    }
    return J_DONE;
}

static void *job_main(void *arg)
{
    struct job *j = arg;
    /* Expand the selection now (constant-time to create, linear to run). */
    char **items = NULL;
    size_t nitems = 0;
    if (j->sel.all) {
        DIR *d = opendir(j->sel.folder);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            bool excepted = false;
            for (size_t i = 0; i < j->sel.n && !excepted; i++) excepted = !strcmp(j->sel.names[i], de->d_name);
            if (!excepted) push_name(&items, &nitems, de->d_name);
        }
        if (d) closedir(d);
    } else {
        for (size_t i = 0; i < j->sel.n; i++) push_name(&items, &nitems, j->sel.names[i]);
    }
    pthread_mutex_lock(&mu);
    j->items_total = nitems;
    pthread_mutex_unlock(&mu);

    if (!strcmp(j->kind, "trash") || !strcmp(j->kind, "delete")) {
        jstate r = trash_or_delete(j, items, nitems);
        for (size_t i = 0; i < nitems; i++) free(items[i]);
        free(items);
        pthread_mutex_lock(&mu);
        if (r == J_DONE && j->want_cancel) r = J_CANCELLED;
        if (r == J_DONE && j->want_logout) r = J_LOGOUT;
        pthread_mutex_unlock(&mu);
        finish(j, r);
        pthread_mutex_lock(&mu);
        start_queued();
        pthread_mutex_unlock(&mu);
        return NULL;
    }
    struct stat dst_st;
    bool move = strcmp(j->kind, "move") == 0;
    jstate result = J_DONE;
    if (stat(j->dest, &dst_st) < 0 || !S_ISDIR(dst_st.st_mode)) {
        snprintf(j->error, sizeof j->error, "destination is not a folder");
        result = J_FAILED;
    }
    /* Same filesystem: a move is just renames; a missing item shows as
     * ENOENT from the rename itself, so no per-item stat is needed. */
    struct stat src_st;
    bool same_fs = stat(j->sel.folder, &src_st) == 0 && src_st.st_dev == dst_st.st_dev;
    for (size_t i = 0; result == J_DONE && i < nitems; i++) {
        if (!checkpoint(j)) break;
        const char *name = items[i];
        char src[PATH_MAX], tmp[PATH_MAX];
        snprintf(src, sizeof src, "%s/%s", j->sel.folder, name);
        pthread_mutex_lock(&mu);
        snprintf(j->current, sizeof j->current, "%s", name);
        pthread_mutex_unlock(&mu);
        if (move && same_fs) {
            int rc = place(j, src, name, false);
            if (rc == -ECANCELED) break;
            pthread_mutex_lock(&mu);
            if (rc == -ENOENT) push_name(&j->missing, &j->nmissing, name);
            else if (rc < 0 && !j->error[0]) snprintf(j->error, sizeof j->error, "%s: %s", name, strerror(-rc));
            j->items_done++;
            journal(j, false);
            pthread_mutex_unlock(&mu);
            if (rc < 0 && rc != -ENOENT) { result = J_FAILED; break; }
            continue;
        }
        struct stat st;
        if (lstat(src, &st) < 0) {
            /* Moved away or deleted by someone else meanwhile: report, go on. */
            pthread_mutex_lock(&mu);
            push_name(&j->missing, &j->nmissing, name);
            j->items_done++;
            pthread_mutex_unlock(&mu);
            continue;
        }
        int rc;
        if (move && st.st_dev == dst_st.st_dev) {
            rc = place(j, src, name, false);   /* one rename; contents and IDs move with it */
        } else {
            char existing[PATH_MAX];
            snprintf(existing, sizeof existing, "%s/%s", j->dest, name);
            struct stat es;
            /* Resuming: an item already copied completely is skipped. */
            if (lstat(existing, &es) == 0 && S_ISREG(st.st_mode) && S_ISREG(es.st_mode) && es.st_size == st.st_size &&
                es.st_mtim.tv_sec == st.st_mtim.tv_sec && es.st_mtim.tv_nsec == st.st_mtim.tv_nsec &&
                j->resuming) {
                rc = 0;
            } else {
                /* Unique per user and item: two users' jobs may write into one folder. */
                snprintf(tmp, sizeof tmp, "%s/" WPS_SHADOW_TMP "%s-job%lld-%zu", j->dest, owner, (long long)j->id, i);
                rm_tree(tmp);   /* left by a crash mid-item */
                rc = copy_tree(j, src, tmp, move);
                if (rc == 0) rc = place(j, tmp, name, true);
                else { rm_tree(tmp); rc = -rc; }
                if (rc == 0 && move) rc = rm_tree(src) ? -errno : 0;
            }
        }
        if (rc == -ECANCELED) break;
        if (rc == -ENOENT) {
            pthread_mutex_lock(&mu);
            push_name(&j->missing, &j->nmissing, name);
            pthread_mutex_unlock(&mu);
        } else if (rc < 0) {
            snprintf(j->error, sizeof j->error, "%s: %s", name, strerror(-rc));
            result = J_FAILED;
            break;
        }
        pthread_mutex_lock(&mu);
        j->items_done++;
        journal(j, false);
        pthread_mutex_unlock(&mu);
    }
    for (size_t i = 0; i < nitems; i++) free(items[i]);
    free(items);
    pthread_mutex_lock(&mu);
    if (result == J_DONE && j->want_cancel) result = J_CANCELLED;
    if (result == J_DONE && j->want_logout) result = J_LOGOUT;
    pthread_mutex_unlock(&mu);
    finish(j, result);
    pthread_mutex_lock(&mu);
    start_queued();
    pthread_mutex_unlock(&mu);
    return NULL;   /* detached */
}

static void start_queued(void)   /* caller holds mu */
{
    for (struct job *j = jobs; j && running < MAX_RUNNING; j = j->next)
        if (j->state == J_QUEUED && !j->has_thread) {
            j->state = J_RUNNING;
            j->want_cancel = j->want_pause = j->want_logout = false;
            running++;
            if (pthread_create(&j->th, NULL, job_main, j) == 0) {
                pthread_detach(j->th);   /* never joined under the lock */
                j->has_thread = true;
            } else {
                j->state = J_FAILED;
                running--;
            }
        }
}

static struct job *new_job(const char *kind, const struct selection *s, const char *dest)
{
    struct job *j = calloc(1, sizeof *j);
    snprintf(j->kind, sizeof j->kind, "%s", kind);
    j->sel = *s;
    j->sel.next = NULL;
    j->sel.names = calloc(s->n + 1, sizeof *j->sel.names);
    for (size_t i = 0; i < s->n; i++) j->sel.names[i] = strdup(s->names[i]);
    snprintf(j->dest, sizeof j->dest, "%s", dest);
    pthread_cond_init(&j->cv, NULL);
    return j;
}

int64_t wps_job_start(const char *kind, int64_t selection, const char *dest, char *err, size_t errlen)
{
    if (strcmp(kind, "move") && strcmp(kind, "copy") && strcmp(kind, "trash") && strcmp(kind, "delete")) {
        snprintf(err, errlen, "kind is move, copy, trash or delete");
        return -1;
    }
    pthread_mutex_lock(&mu);
    struct selection *s = selections;
    while (s && s->id != selection) s = s->next;
    if (!s) { pthread_mutex_unlock(&mu); snprintf(err, errlen, "no such selection"); return -1; }
    struct job *j = new_job(kind, s, dest);
    j->id = next_job++;
    j->state = J_QUEUED;
    j->next = jobs;
    jobs = j;
    journal(j, true);
    start_queued();
    int64_t id = j->id;
    pthread_mutex_unlock(&mu);
    return id;
}

static bool control(int64_t id, int what, const char *decision, bool all)
{
    pthread_mutex_lock(&mu);
    struct job *j = job_by_id(id);
    bool ok = j != NULL;
    if (j) {
        switch (what) {
        case 0: j->want_cancel = true; if (j->state == J_QUEUED || j->state == J_INTERRUPTED) { j->state = J_CANCELLED; journal(j, true); } break;
        case 1: ok = j->state == J_RUNNING; j->want_pause = true; break;
        case 2:
            if (j->state == J_PAUSED || j->state == J_RUNNING) j->want_pause = false;
            else if (j->state == J_INTERRUPTED || j->state == J_LOGOUT) { j->state = J_QUEUED; j->resuming = true; start_queued(); }
            else ok = false;
            break;
        case 3: {
            int d = !strcmp(decision, "skip") ? 1 : !strcmp(decision, "rename") ? 2 : 0;
            ok = d && j->state == J_DECISION;
            if (ok) { j->decision = d; if (all) j->decision_all = d; }
            break;
        }
        }
        pthread_cond_broadcast(&j->cv);
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

bool wps_job_cancel(int64_t id) { return control(id, 0, NULL, false); }
bool wps_job_pause(int64_t id) { return control(id, 1, NULL, false); }
bool wps_job_resume(int64_t id) { return control(id, 2, NULL, false); }
bool wps_job_decide(int64_t id, const char *decision, bool all) { return control(id, 3, decision, all); }

static void put_names(cbuf *b, char **v, size_t n)
{
    cb_array(b, n);
    for (size_t i = 0; i < n; i++) cb_text(b, v[i]);
}

static void put_job(cbuf *b, struct job *j)
{
    cb_map(b, 14);
    cb_text(b, "job"); cb_int(b, j->id);
    cb_text(b, "kind"); cb_text(b, j->kind);
    cb_text(b, "state"); cb_text(b, state_names[j->state]);
    cb_text(b, "from"); cb_text(b, j->sel.folder);
    cb_text(b, "to"); cb_text(b, j->dest);
    cb_text(b, "items_total"); cb_uint(b, j->items_total);
    cb_text(b, "items_done"); cb_uint(b, j->items_done);
    cb_text(b, "bytes_done"); cb_uint(b, j->bytes_done);
    cb_text(b, "current"); cb_text(b, j->current);
    cb_text(b, "conflict"); j->conflict[0] ? cb_text(b, j->conflict) : cb_null(b);
    cb_text(b, "missing"); put_names(b, j->missing, j->nmissing);
    cb_text(b, "skipped"); put_names(b, j->skipped, j->nskipped);
    cb_text(b, "not_trashed"); put_names(b, j->not_trashed, j->nnot_trashed);
    cb_text(b, "error"); j->error[0] ? cb_text(b, j->error) : cb_null(b);
}

bool wps_job_status(int64_t id, cbuf *out)
{
    pthread_mutex_lock(&mu);
    bool ok = true;
    if (id < 0) {
        size_t n = 0;
        for (struct job *j = jobs; j; j = j->next) n++;
        cb_array(out, n);
        for (struct job *j = jobs; j; j = j->next) put_job(out, j);
    } else {
        struct job *j = job_by_id(id);
        if (j) put_job(out, j);
        else ok = false;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

/* ------------------------------------------------------------ journal */

static void restore_row(int64_t id, const char *kind, const char *src, const char *sel, const char *dest,
                        const char *state, int64_t items, int64_t bytes, void *user)
{
    (void)user;
    bool unfinished = !strcmp(state, "running") || !strcmp(state, "queued") || !strcmp(state, "paused") ||
                      !strcmp(state, "needs-decision") || !strcmp(state, "paused-for-logout") ||
                      !strcmp(state, "interrupted");
    if (id >= next_job) next_job = id + 1;
    if (!unfinished) return;
    struct selection s = { 0 };
    snprintf(s.folder, sizeof s.folder, "%s", src);
    char *copy = strdup(sel), *save = NULL;
    char *line = strtok_r(copy, "\n", &save);
    s.all = line && !strcmp(line, "all");
    char **names = NULL;
    size_t n = 0;
    while ((line = strtok_r(NULL, "\n", &save))) push_name(&names, &n, line);
    s.names = names;
    s.n = n;
    struct job *j = new_job(kind, &s, dest);
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    free(copy);
    j->id = id;
    j->items_done = (uint64_t)items;
    j->bytes_done = (uint64_t)bytes;
    j->resuming = true;   /* skip items already copied */
    /* Paused for logout: resume now. Anything else unfinished was cut off by
     * a crash: report it, and resume when asked. */
    j->state = !strcmp(state, "paused-for-logout") ? J_QUEUED : J_INTERRUPTED;
    j->next = jobs;
    jobs = j;
}

void wps_jobs_init(wpo_loop *l, wps_db *d, const char *who)
{
    loop = l;
    db = d;
    snprintf(owner, sizeof owner, "%.8s", who);
    wps_db_jobs_list(db, restore_row, NULL);
    pthread_mutex_lock(&mu);
    for (struct job *j = jobs; j; j = j->next)
        if (j->state == J_INTERRUPTED) journal(j, true);
    start_queued();
    pthread_mutex_unlock(&mu);
}

void wps_jobs_shutdown(bool logout)
{
    pthread_mutex_lock(&mu);
    for (struct job *j = jobs; j; j = j->next) {
        if (logout) j->want_logout = true;
        else j->want_cancel = true;
        pthread_cond_broadcast(&j->cv);
    }
    /* Wait (bounded) for job threads to reach a checkpoint and stop. */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 10;
    while (running > 0 && pthread_cond_timedwait(&idle_cv, &mu, &until) == 0) {}
    if (running > 0) { pthread_mutex_unlock(&mu); return; }   /* leave them; the process is exiting */
    while (jobs) {
        struct job *j = jobs;
        jobs = j->next;
        for (size_t i = 0; i < j->sel.n; i++) free(j->sel.names[i]);
        free(j->sel.names);
        for (size_t i = 0; i < j->nmissing; i++) free(j->missing[i]);
        for (size_t i = 0; i < j->nskipped; i++) free(j->skipped[i]);
        for (size_t i = 0; i < j->nnot_trashed; i++) free(j->not_trashed[i]);
        free(j->missing);
        free(j->skipped);
        free(j->not_trashed);
        for (size_t i = 0; i < j->npos; i++) free(j->pos_names[i]);
        free(j->pos_names);
        free(j->pos_x);
        free(j->pos_y);
        for (size_t i = 0; i < j->nplaced; i++) free(j->placed[i]);
        free(j->placed);
        free(j->placed_x);
        free(j->placed_y);
        pthread_cond_destroy(&j->cv);
        free(j);
    }
    while (selections) {
        struct selection *s = selections;
        selections = s->next;
        for (size_t i = 0; i < s->n; i++) free(s->names[i]);
        free(s->names);
        free(s);
    }
    pthread_mutex_unlock(&mu);
}

void wps_job_set_positions(int64_t id, const char *const *names, const int64_t *x, const int64_t *y, size_t n)
{
    pthread_mutex_lock(&mu);
    struct job *j = job_by_id(id);
    if (j && !j->npos) {
        j->pos_names = calloc(n ? n : 1, sizeof *j->pos_names);
        j->pos_x = calloc(n ? n : 1, sizeof *j->pos_x);
        j->pos_y = calloc(n ? n : 1, sizeof *j->pos_y);
        for (size_t i = 0; i < n; i++) {
            j->pos_names[i] = strdup(names[i]);
            j->pos_x[i] = x[i];
            j->pos_y[i] = y[i];
        }
        j->npos = n;
    }
    pthread_mutex_unlock(&mu);
}
