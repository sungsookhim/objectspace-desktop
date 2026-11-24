/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#define _GNU_SOURCE
#include "tracker.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "attrs.h"
#include "cbor.h"
#include "objects.h"
#include "listing.h"
#include "shadows.h"
#include "OsdObject.h"

static osdo_loop *loop;
static const osd_env *env;
static osd_db *db;
static bool backup_names;
static osd_tracker_stats stats;
static bool trace;

/* ------------------------------------------------------------ events */

typedef enum { E_CREATE, E_DELETE, E_MOVE, E_ATTRIB, E_WRITE, E_OVERFLOW } ekind;

typedef struct {
    ekind k;
    bool isdir;
    char from[PATH_MAX];    /* E_MOVE: old path, "" if it came from out of view */
    char path[PATH_MAX];    /* the entry; E_MOVE: new path, "" if it left our view */
} tev;

static void apply(const tev *e);

static void join(char *out, size_t n, const char *dir, const char *name)
{
    if (!dir || !dir[0]) { out[0] = 0; return; }
    snprintf(out, n, "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, name);
}

static void dir_of(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == out) out[1] = 0;
    else if (slash) *slash = 0;
}

/* ----------------------------------------------- watched folders */

struct watch { char path[PATH_MAX]; int wd; int reg; };
static struct watch *watches;
static size_t nwatches, capwatches;

static struct watch *watch_by_path(const char *path)
{
    for (size_t i = 0; i < nwatches; i++)
        if (strcmp(watches[i].path, path) == 0) return &watches[i];
    return NULL;
}

static struct watch *watch_add(const char *path)
{
    if (nwatches == capwatches) {
        size_t ncap = capwatches ? capwatches * 2 : 64;
        struct watch *n = realloc(watches, ncap * sizeof *n);
        if (!n) return NULL;
        watches = n;
        capwatches = ncap;
    }
    struct watch *w = &watches[nwatches++];
    snprintf(w->path, sizeof w->path, "%s", path);
    w->wd = w->reg = -1;
    return w;
}

static void watch_remove(struct watch *w)
{
    *w = watches[--nwatches];
}

/* A folder was renamed: everything recorded under it moves too. */
static void rename_prefix(const char *from, const char *to)
{
    osd_shadows_prefix_moved(from, to);   /* shadows of everything inside, loaded or not */
    size_t l = strlen(from);
    char np[PATH_MAX];
    for (size_t i = 0; i < nwatches; i++)
        if (strncmp(watches[i].path, from, l) == 0 && (watches[i].path[l] == '/' || watches[i].path[l] == 0)) {
            snprintf(np, sizeof np, "%s%s", to, watches[i].path + l);
            snprintf(watches[i].path, sizeof watches[i].path, "%s", np);
        }
    for (size_t i = 0; i < osd_sobj_count(); i++) {
        sobj *s = osd_sobj_at(i);
        if (strncmp(s->path, from, l) == 0 && s->path[l] == '/') {
            snprintf(np, sizeof np, "%s%s", to, s->path + l);
            osd_sobj_set_path(s, np);
        }
    }
}

/* ----------------------------------------------------------- inotify */

static int ino_fd = -1;
#define INO_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | \
                  IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_EXCL_UNLINK | IN_ONLYDIR)

static struct watch *watch_by_wd(int wd)
{
    for (size_t i = 0; i < nwatches; i++)
        if (watches[i].wd == wd) return &watches[i];
    return NULL;
}

static void on_inotify(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    _Alignas(struct inotify_event) char buf[64 * 1024];
    ssize_t len = read(fd, buf, sizeof buf);
    if (len <= 0) return;
    struct inotify_event *pending_from = NULL;
    char pending_dir[PATH_MAX] = "";
    tev e;
    for (char *p = buf; p < buf + len;) {
        struct inotify_event *ie = (struct inotify_event *)p;
        p += sizeof *ie + ie->len;
        if (ie->mask & IN_Q_OVERFLOW) { e.k = E_OVERFLOW; apply(&e); continue; }
        struct watch *w = watch_by_wd(ie->wd);
        if (!w) continue;
        if (ie->mask & (IN_IGNORED | IN_DELETE_SELF)) { if (ie->mask & IN_IGNORED) watch_remove(w); continue; }
        if (ie->mask & IN_MOVE_SELF) continue;   /* seen as a move in the parent */
        memset(&e, 0, sizeof e);
        e.isdir = (ie->mask & IN_ISDIR) != 0;
        /* A rename arrives as MOVED_FROM then MOVED_TO with one cookie. */
        if (pending_from && !(ie->mask & IN_MOVED_TO && ie->cookie == pending_from->cookie)) {
            tev out = { .k = E_MOVE, .isdir = (pending_from->mask & IN_ISDIR) != 0 };
            join(out.from, sizeof out.from, pending_dir, pending_from->name);
            apply(&out);   /* moved out of view */
            pending_from = NULL;
        }
        if (ie->mask & IN_MOVED_FROM) {
            pending_from = ie;
            snprintf(pending_dir, sizeof pending_dir, "%s", w->path);
            continue;
        }
        if (ie->mask & IN_MOVED_TO) {
            e.k = E_MOVE;
            if (pending_from) join(e.from, sizeof e.from, pending_dir, pending_from->name);
            join(e.path, sizeof e.path, w->path, ie->name);
            pending_from = NULL;
            apply(&e);
            continue;
        }
        e.k = ie->mask & IN_CREATE ? E_CREATE : ie->mask & IN_DELETE ? E_DELETE :
              ie->mask & IN_ATTRIB ? E_ATTRIB : E_WRITE;
        join(e.path, sizeof e.path, w->path, ie->len ? ie->name : "");
        if (!ie->len) continue;
        apply(&e);
    }
    if (pending_from) {
        tev out = { .k = E_MOVE, .isdir = (pending_from->mask & IN_ISDIR) != 0 };
        join(out.from, sizeof out.from, pending_dir, pending_from->name);
        apply(&out);
    }
}

/* -------------------------------------------------------- osd-watchd */

static int helper_fd = -1;
static char **reg_pending;        /* paths waiting for their registration reply, in order */
static size_t nreg_pending;

static struct watch *watch_by_reg(int reg)
{
    for (size_t i = 0; i < nwatches; i++)
        if (watches[i].reg == reg) return &watches[i];
    return NULL;
}

static void helper_register(struct watch *w)
{
    int fd = open(w->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    cbuf b;
    cb_init(&b);
    cb_map(&b, 1);
    cb_text(&b, "op"); cb_text(&b, "reg");
    union { char c[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
    struct iovec iov = { b.d, b.n };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.c, .msg_controllen = sizeof u.c };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    if (sendmsg(helper_fd, &m, MSG_NOSIGNAL) >= 0) {
        reg_pending = realloc(reg_pending, (nreg_pending + 1) * sizeof *reg_pending);
        reg_pending[nreg_pending++] = strdup(w->path);
    }
    close(fd);
    cb_free(&b);
}

static void helper_lost(void);
static void inotify_add(struct watch *w);
static void verify_folder(const char *dir);

static void loc_path(const cv *loc, char *out, size_t n)
{
    out[0] = 0;
    if (!loc || loc->k != CV_MAP) return;   /* "<hidden>": out of view */
    int64_t reg;
    const char *name = cv_text(loc, "name");
    struct watch *w = cv_int(loc, "reg", &reg) ? watch_by_reg((int)reg) : NULL;
    if (w && name) join(out, n, w->path, name);
}

static void on_helper(int fd, short rev, void *arg)
{
    (void)arg;
    if (rev & (POLLHUP | POLLERR)) { helper_lost(); return; }
    unsigned char buf[65536];
    ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n == 0) { helper_lost(); return; }
    if (n < 0) return;
    cv m;
    if (cv_decode(buf, (size_t)n, &m)) return;
    int64_t v;
    const char *mask = cv_text(&m, "mask");
    if (cv_get(&m, "reg") && cv_int(&m, "reg", &v) && !mask) {
        /* Registration reply, in request order. */
        if (nreg_pending) {
            struct watch *w = watch_by_path(reg_pending[0]);
            if (w && v >= 0) {
                w->reg = (int)v;
                verify_folder(w->path);
            } else if (w) {
                /* The helper cannot watch this folder (for example a btrfs
                 * subvolume on an older kernel): fall back to inotify here. */
                const char *why = cv_text(&m, "error");
                fprintf(stderr, "osdd: osd-watchd refused %s (%s); using inotify for it\n", w->path, why ? why : "?");
                inotify_add(w);
                stats.mode = "osd-watchd + inotify";
            }
            free(reg_pending[0]);
            memmove(reg_pending, reg_pending + 1, --nreg_pending * sizeof *reg_pending);
        }
    } else if (cv_int(&m, "revoked", &v)) {
        struct watch *w = watch_by_reg((int)v);
        if (w) w->reg = -1;
    } else if (cv_int(&m, "overflow", &v)) {
        tev e = { .k = E_OVERFLOW };
        apply(&e);
    } else if (mask) {
        tev e;
        memset(&e, 0, sizeof e);
        e.isdir = strstr(mask, "ONDIR") != NULL;
        if (strstr(mask, "RENAME")) {
            e.k = E_MOVE;
            loc_path(cv_get(&m, "old"), e.from, sizeof e.from);
            loc_path(cv_get(&m, "new"), e.path, sizeof e.path);
            apply(&e);
        } else if (strstr(mask, "MOVED_")) {
            /* covered by RENAME */
        } else if (cv_get(&m, "dir")) {
            loc_path(cv_get(&m, "dir"), e.path, sizeof e.path);
            e.k = strstr(mask, "CREATE") ? E_CREATE : strstr(mask, "DELETE") && !strstr(mask, "DELETE_SELF") ? E_DELETE :
                  strstr(mask, "CLOSE_WRITE") ? E_WRITE : E_ATTRIB;
            if (e.path[0]) apply(&e);
        }
    }
    cv_free(&m);
}

static void helper_connect(void)
{
    const char *p = getenv("OSD_WATCHD_SOCKET");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", p && *p ? p : "/run/osd-watchd.sock");
    if (strcmp(path, "none") == 0) return;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof sa.sun_path) {
        fprintf(stderr, "osdd: helper socket path too long: %s\n", path);
        if (fd >= 0) close(fd);
        return;
    }
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (fd >= 0) close(fd);
        return;
    }
    helper_fd = fd;
    osdo_loop_watch_fd(loop, fd, POLLIN, on_helper, NULL);
}

static void inotify_add(struct watch *w)
{
    if (ino_fd < 0) {
        ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ino_fd < 0) return;
        osdo_loop_watch_fd(loop, ino_fd, POLLIN, on_inotify, NULL);
    }
    w->wd = inotify_add_watch(ino_fd, w->path, INO_MASK);
}

static void helper_lost(void)
{
    /* Fall back to inotify for everything we watch. */
    osdo_loop_unwatch_fd(loop, helper_fd);
    close(helper_fd);
    helper_fd = -1;
    for (size_t i = 0; i < nreg_pending; i++) free(reg_pending[i]);
    nreg_pending = 0;
    for (size_t i = 0; i < nwatches; i++) { watches[i].reg = -1; inotify_add(&watches[i]); }
    stats.mode = "inotify";
}

void osd_tracker_watch(const char *dir)
{
    if (!dir[0] || osd_path_untracked(dir) || watch_by_path(dir)) return;
    struct watch *w = watch_add(dir);
    if (!w) return;
    if (helper_fd >= 0) helper_register(w);
    else inotify_add(w);
    stats.watched = nwatches;
}

/* --------------------------------------------------- pending pairings */

/* An object renamed within its folder (design: displacement rule). */
struct displaced {
    sobj *obj;
    char oldpath[PATH_MAX];   /* the name it left */
    bool backup;              /* renamed to a backup name and backup-names is on */
    bool paired;
    char newpath[PATH_MAX];   /* a new file at oldpath */
};
/* A file that appeared with another file's ID. */
struct dup {
    char id[OSD_ID_LEN + 1];
    sobj *orig;
    char newpath[PATH_MAX];
    struct timespec orig_mtime;   /* the original's, when the duplicate appeared */
    bool settled;                 /* the copy is complete: its mtime equals the original's */
};
static struct displaced *disp;
static size_t ndisp;
static struct dup *dups;
static size_t ndups;

/* Files created recently that carry no ID yet: copy tools (mv across
 * filesystems, cp -a) set the attributes after creating the file, so an
 * attribute change on one of these is checked for an ID again. */
#define RECENT 256
static char recent[RECENT][PATH_MAX];
static size_t recent_next;

static void recent_add(const char *path) { snprintf(recent[recent_next++ % RECENT], PATH_MAX, "%s", path); }

static bool recent_take(const char *path)
{
    for (size_t i = 0; i < RECENT; i++)
        if (recent[i][0] && strcmp(recent[i], path) == 0) { recent[i][0] = 0; return true; }
    return false;
}

static void disp_remove(size_t i) { disp[i] = disp[--ndisp]; }
static void dup_remove(size_t i) { dups[i] = dups[--ndups]; }

/* An object now exists at `path`: it is no longer an unknown new file. */
void osd_tracker_object_at(const char *path) { recent_take(path); }

static void undelete(sobj *s);

void osd_tracker_forget(sobj *s)
{
    undelete(s);
    for (size_t i = 0; i < ndisp;) { if (disp[i].obj == s) disp_remove(i); else i++; }
    for (size_t i = 0; i < ndups;) { if (dups[i].orig == s) dup_remove(i); else i++; }
}

static bool is_backup_name(const char *oldpath, const char *newpath)
{
    static const char *suffixes[] = { "~", ".bak", ".orig", NULL };
    size_t l = strlen(oldpath);
    if (strncmp(newpath, oldpath, l) != 0) return false;
    for (int i = 0; suffixes[i]; i++)
        if (strcmp(newpath + l, suffixes[i]) == 0) return true;
    return false;
}

/* --------------------------------------------------------- worker jobs */

enum jkind { J_PROBE_CREATE, J_PROBE_OVER, J_TRANSFER, J_NEWID, J_DUPCHECK, J_ADOPT, J_FORGET, J_SEEK, J_VERIFY };

struct job {
    enum jkind kind;
    char path[PATH_MAX];
    sobj *target;              /* J_PROBE_OVER, J_TRANSFER, J_ADOPT */
    osd_loaded snap;           /* J_TRANSFER: the object's properties */
    bool split;                /* J_TRANSFER: a hard-link split */
    size_t dupi;               /* J_DUPCHECK */
    char dup_id[OSD_ID_LEN + 1];
    char owner_path[PATH_MAX];  /* J_PROBE_CREATE: the path of the object that may own the ID */
    bool owner_gone;
    /* results */
    int err;
    struct stat st, st2;
    char id[OSD_ID_LEN + 1];
};

/* Find a file by its object ID under `root` (no symlinks, no other mounts). */
static char seek_id[OSD_ID_LEN + 1], seek_found[PATH_MAX];
static pthread_mutex_t seek_mu = PTHREAD_MUTEX_INITIALIZER;   /* nftw has no user pointer */

static int seek_visit(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)ftw;
    if (osd_path_untracked(path)) return flag == FTW_D ? FTW_SKIP_SUBTREE : FTW_CONTINUE;
    char id[OSD_ID_LEN + 1];
    if (flag != FTW_SL && osd_attr_get_id(path, id) && strcmp(id, seek_id) == 0) {
        snprintf(seek_found, sizeof seek_found, "%s", path);
        return FTW_STOP;
    }
    return FTW_CONTINUE;
}

static bool seek(const char *root, const char *id, char *out, size_t n)
{
    pthread_mutex_lock(&seek_mu);
    snprintf(seek_id, sizeof seek_id, "%s", id);
    seek_found[0] = 0;
    nftw(root, seek_visit, 32, FTW_PHYS | FTW_MOUNT | FTW_ACTIONRETVAL);
    bool ok = seek_found[0] != 0;
    if (ok) snprintf(out, n, "%s", seek_found);
    pthread_mutex_unlock(&seek_mu);
    return ok;
}

static void job_work(void *arg)
{
    struct job *j = arg;
    if (j->kind == J_VERIFY) {
        /* Still there? If not, search for it like an object that left view. */
        char id[OSD_ID_LEN + 1];
        if (lstat(j->path, &j->st) == 0 && osd_attr_get_id(j->path, id) && strcmp(id, j->dup_id) == 0) return;
        j->kind = J_SEEK;
    }
    if (j->kind == J_SEEK) {
        /* Search near where it was first, then the whole volume. */
        char root[PATH_MAX], found[PATH_MAX];
        dir_of(j->path, root, sizeof root);
        osd_volume v;
        bool have_vol = osd_volume_for_path(j->path, &v);
        for (int up = 0; up < 3; up++) {
            char next[PATH_MAX];
            dir_of(root, next, sizeof next);
            if (strcmp(next, root) == 0 || (have_vol && strlen(next) < strlen(v.mnt))) break;
            snprintf(root, sizeof root, "%s", next);
            if (seek(root, j->dup_id, found, sizeof found)) {
                snprintf(j->path, sizeof j->path, "%s", found);
                lstat(found, &j->st);
                return;
            }
        }
        if (have_vol && seek(v.mnt, j->dup_id, found, sizeof found)) {
            snprintf(j->path, sizeof j->path, "%s", found);
            lstat(found, &j->st);
            return;
        }
        j->err = ENOENT;
        return;
    }
    if (j->kind == J_FORGET) {
        osd_db_location_forget(db, j->id);
        return;
    }
    if (j->kind == J_DUPCHECK) {
        if (lstat(j->path, &j->st) < 0) j->err = errno;
        if (j->target && lstat(j->target->path, &j->st2) < 0) j->err = errno;
        return;
    }
    if (lstat(j->path, &j->st) < 0) { j->err = errno; return; }
    if (j->kind == J_PROBE_CREATE || j->kind == J_PROBE_OVER || j->kind == J_ADOPT) {
        if (!osd_attr_get_id(j->path, j->id)) j->id[0] = 0;
        /* Is the object that owns this ID still at its last known place? If
         * not, this file is that object, moved here unseen. */
        if (j->kind == J_PROBE_CREATE && j->id[0]) {
            char vol[OSD_VOLID_MAX], rel[PATH_MAX];
            osd_volume v;
            if (osd_db_get_location(db, j->id, vol, sizeof vol, rel, sizeof rel) == 0 && osd_volume_for_uuid(vol, &v)) {
                snprintf(j->owner_path, sizeof j->owner_path, "%s%s%s", v.mnt, strcmp(v.mnt, "/") ? "/" : "", rel);
                j->owner_gone = strcmp(j->owner_path, j->path) != 0 && lstat(j->owner_path, &j->st2) < 0;
            }
        }
        return;
    }
    if (j->kind == J_NEWID) {
        /* A copy: same settings, its own identity. */
        osd_new_id(j->id);
        if (lsetxattr(j->path, OSD_ATTR_ID, j->id, OSD_ID_LEN, 0) < 0) j->err = errno;
        return;
    }
    /* J_TRANSFER: write the object's identity and settings onto the new
     * inode (merging under its lock; another service may do the same). */
    char want[OSD_ID_LEN + 1];
    memcpy(want, j->snap.id, sizeof want);
    if (j->split || !want[0]) osd_new_id(want);
    if (osd_attr_assign_id(j->path, want, j->id) != OSD_A_OK) j->id[0] = 0;
    for (int c = 0; c < j->snap.nc; c++) {
        osd_props *p = &j->snap.c[c].props;
        if (j->split && strcmp(j->snap.c[c].cls, "OsdObject") == 0) osd_props_set_int(p, "split", 1);
        if (p->n) osd_attr_update(j->path, j->snap.c[c].cls, p, NULL, 200, NULL, NULL);
    }
    lstat(j->path, &j->st);
}

static void watch_parent(const char *path)
{
    char d[PATH_MAX];
    dir_of(path, d, sizeof d);
    osd_tracker_watch(d);
}

static void start_job(struct job *j);

static void transfer(sobj *s, const char *newpath, bool split)
{
    struct job *j = calloc(1, sizeof *j);
    j->kind = J_TRANSFER;
    j->target = s;
    j->split = split;
    snprintf(j->path, sizeof j->path, "%s", newpath);
    osd_sobj_snapshot(s, &j->snap);
    start_job(j);
}

static bool target_alive(sobj *s)
{
    for (size_t i = 0; i < osd_sobj_count(); i++)
        if (osd_sobj_at(i) == s) return true;
    return false;
}

/* The object is gone. `deleted`: really deleted (its managed shadows go
 * too); otherwise only this record is dropped (it reloads on demand). */
static void forget_object_as(sobj *s, bool deleted)
{
    osd_tracker_forget(s);
    if (deleted) osd_shadows_target_deleted(OsdObject_data(s->o)->id);
    if (OsdObject_data(s->o)->id[0]) {
        /* Keep the location row, marked as deleted (on a worker). */
        struct job *j = calloc(1, sizeof *j);
        j->kind = J_FORGET;
        memcpy(j->id, OsdObject_data(s->o)->id, sizeof j->id);
        start_job(j);
    }
    osd_sobj_forget(s);
}

static void forget_object(sobj *s) { forget_object_as(s, true); }

/* Deletions wait while new files are being checked for IDs: one of them
 * may be the deleted object, moved (across filesystems, or from a folder
 * we did not see). When the checks drain, the rest are forgotten. */
static size_t probes_inflight;
static sobj **pending_del;
static size_t npending_del;

static void delete_later(sobj *s)
{
    for (size_t i = 0; i < npending_del; i++)
        if (pending_del[i] == s) return;
    pending_del = realloc(pending_del, (npending_del + 1) * sizeof *pending_del);
    pending_del[npending_del++] = s;
}

static void undelete(sobj *s)
{
    for (size_t i = 0; i < npending_del; i++)
        if (pending_del[i] == s) { pending_del[i] = pending_del[--npending_del]; return; }
}

static void drain_deletes(void)
{
    if (probes_inflight) return;
    while (npending_del) {
        sobj *s = pending_del[--npending_del];
        if (target_alive(s)) forget_object(s);
    }
}

static void job_done(void *arg)
{
    struct job *j = arg;
    if (j->target && !target_alive(j->target)) j->target = NULL;
    if (trace) {
        static const char *k[] = { "probe-create", "probe-over", "transfer", "new-id", "dup-check", "adopt",
                                   "forget", "seek", "verify" };
        fprintf(stderr, "osdd: job %s %s err=%d id=%.8s owner_gone=%d owner=%s target=%s ndups=%zu\n", k[j->kind],
                j->path, j->err, j->id, j->owner_gone, j->owner_path, j->target ? j->target->path : "-", ndups);
    }
    switch (j->kind) {
    case J_PROBE_CREATE:
        if (!j->err && !j->id[0] && S_ISREG(j->st.st_mode)) recent_add(j->path);
        if (!j->err && j->id[0]) {
            /* A file appeared carrying an ID. Someone else's? Then it is a
             * copy or a move in progress: shared until one changes or the
             * original goes away (design decision 2026-11-18). */
            sobj *owner = osd_sobj_by_id(j->id);
            if (owner && j->owner_gone && strcmp(owner->path, j->owner_path) == 0) {
                undelete(owner);
                /* Its old place is empty: it moved here from somewhere we
                 * did not see (an unwatched folder, or the trash). */
                for (size_t i = 0; i < ndups;) { if (dups[i].orig == owner) dup_remove(i); else i++; }
                osd_sobj_set_inode(owner, &j->st);
                osd_sobj_set_path(owner, j->path);
                watch_parent(j->path);
                break;
            }
            bool saving = false;   /* the new half of a vim-style save, not a copy */
            for (size_t i = 0; i < ndisp && !saving; i++)
                saving = disp[i].obj == owner && disp[i].paired && strcmp(disp[i].newpath, j->path) == 0;
            if (owner && !saving && strcmp(owner->path, j->path) != 0) {
                dups = realloc(dups, (ndups + 1) * sizeof *dups);
                struct dup *d = &dups[ndups++];
                memcpy(d->id, j->id, sizeof d->id);
                d->orig = owner;
                snprintf(d->newpath, sizeof d->newpath, "%s", j->path);
                d->orig_mtime = owner->mtime;
                /* mv and cp -a give the copy the original's modification
                 * time when they finish; until then it is still being copied. */
                d->settled = j->st.st_mtim.tv_sec == owner->mtime.tv_sec && j->st.st_mtim.tv_nsec == owner->mtime.tv_nsec;
            }
        }
        break;
    case J_PROBE_OVER:
        if (!j->target || j->err) break;
        if (S_ISLNK(j->st.st_mode)) {
            osd_sobj_set_inode(j->target, &j->st);   /* a shadow's symlink was rewritten */
        } else if (!j->id[0]) {
            /* Atomic save: same object, new contents. A hard-linked file
             * splits instead: the new file becomes its own object. */
            transfer(j->target, j->path, j->target->nlink > 1 && j->target->xattr_ok);
        } else if (strcmp(j->id, OsdObject_data(j->target->o)->id) == 0) {
            osd_sobj_set_inode(j->target, &j->st);   /* already carried over (full vim, or another service) */
        } else {
            forget_object(j->target);                /* replaced by a different object */
        }
        break;
    case J_TRANSFER:
        if (!j->target || j->err) break;
        if (j->split) {
            forget_object_as(j->target, false);   /* reloaded on demand through its other name */
        } else {
            osd_sobj_set_inode(j->target, &j->st);
            osd_sobj_set_path(j->target, j->path);
            watch_parent(j->path);
        }
        break;
    case J_ADOPT:
        /* The original of a duplicate was deleted: it was a move. */
        if (j->target && !j->err) {
            osd_sobj_set_inode(j->target, &j->st);
            osd_sobj_set_path(j->target, j->path);
            watch_parent(j->path);
        }
        break;
    case J_FORGET:
    case J_VERIFY:   /* still there */
        break;
    case J_SEEK:
        if (j->target && !j->err) {
            osd_sobj_set_path(j->target, j->path);
            watch_parent(j->path);
        } else if (j->target && j->err == ENOENT) {
            forget_object(j->target);   /* nowhere to be found: deleted (as far as we can see) */
        }
        break;
    case J_NEWID: {
        sobj *s = osd_sobj_by_path(j->path);
        if (s && !j->err) memcpy(OsdObject_data(s->o)->id, j->id, sizeof j->id);
        break;
    }
    case J_DUPCHECK:
        for (size_t i = 0; i < ndups; i++) {
            struct dup *d = &dups[i];
            if (strcmp(d->id, j->dup_id) != 0) continue;
            if (d->orig && strcmp(d->orig->path, d->newpath) == 0) { dup_remove(i); break; }   /* same place now */
            bool orig_changed = j->target && (j->st2.st_mtim.tv_sec != d->orig_mtime.tv_sec ||
                                              j->st2.st_mtim.tv_nsec != d->orig_mtime.tv_nsec);
            bool same_as_orig = j->st.st_mtim.tv_sec == d->orig_mtime.tv_sec &&
                                j->st.st_mtim.tv_nsec == d->orig_mtime.tv_nsec;
            if (!d->settled && same_as_orig) d->settled = true;   /* the copy just finished */
            /* Either side changed after the copy finished: it is a copy. */
            bool changed = orig_changed || (d->settled && !same_as_orig);
            if (changed && !j->err) {
                struct job *n = calloc(1, sizeof *n);
                n->kind = J_NEWID;
                snprintf(n->path, sizeof n->path, "%s", d->newpath);
                start_job(n);
                dup_remove(i);
            }
            break;
        }
        break;
    }
    if (j->kind == J_PROBE_CREATE) { probes_inflight--; drain_deletes(); }
    osd_loaded_free(&j->snap);
    free(j);
}

static void start_job(struct job *j)
{
    if (j->kind == J_PROBE_CREATE) probes_inflight++;
    static const char *names[] = { "probe new file", "probe replacement", "carry identity", "new ID",
                                   "check duplicate", "adopt move", "forget location", "find moved object",
                                   "verify object" };
    osdo_defer(loop, names[j->kind], job_work, job_done, j);
}

/* Events before a registration took effect were missed: check that the
 * objects in this folder are still there, and look for those that are not. */
static void verify_folder(const char *dir)
{
    for (size_t i = 0; i < osd_sobj_count(); i++) {
        sobj *o = osd_sobj_at(i);
        char d[PATH_MAX];
        dir_of(o->path, d, sizeof d);
        if (strcmp(d, dir) != 0 || !OsdObject_data(o->o)->id[0]) continue;
        struct job *j = calloc(1, sizeof *j);
        j->kind = J_VERIFY;
        j->target = o;
        snprintf(j->path, sizeof j->path, "%s", o->path);
        memcpy(j->dup_id, OsdObject_data(o->o)->id, sizeof j->dup_id);
        start_job(j);
    }
}

static void probe(enum jkind k, const char *path, sobj *target)
{
    struct job *j = calloc(1, sizeof *j);
    j->kind = k;
    j->target = target;
    snprintf(j->path, sizeof j->path, "%s", path);
    start_job(j);
}

static void dup_check(size_t i)
{
    struct job *j = calloc(1, sizeof *j);
    j->kind = J_DUPCHECK;
    j->target = dups[i].orig;
    memcpy(j->dup_id, dups[i].id, sizeof j->dup_id);
    snprintf(j->path, sizeof j->path, "%s", dups[i].newpath);
    start_job(j);
}

/* An Objectspace settings change on either side of a duplicate makes it a copy. */
static void on_saved(sobj *s)
{
    for (size_t i = 0; i < ndups; i++)
        if (dups[i].orig == s || strcmp(dups[i].newpath, s->path) == 0) {
            if (dups[i].orig == s) {
                struct job *n = calloc(1, sizeof *n);
                n->kind = J_NEWID;
                snprintf(n->path, sizeof n->path, "%s", dups[i].newpath);
                start_job(n);
            }
            /* If the copy itself was changed, it already has its ID shared;
             * give it its own. */
            else {
                struct job *n = calloc(1, sizeof *n);
                n->kind = J_NEWID;
                snprintf(n->path, sizeof n->path, "%s", s->path);
                start_job(n);
            }
            dup_remove(i);
            return;
        }
}

/* ----------------------------------------------------------- the rules */

static void scan_soon(void);

void (*osd_on_fs_event)(char kind, const char *from, const char *path);

static void apply(const tev *e)
{
    stats.events++;
    /* Subscribers hear about real names only. A rename from one of our
     * temporary names onto a real one (a finished copy, a rewritten shadow)
     * is that name appearing. */
    bool tmp_path = e->path[0] && strstr(e->path, "/" OSD_SHADOW_TMP);
    bool tmp_old = e->from[0] && strstr(e->from, "/" OSD_SHADOW_TMP);
    /* Cached folder listings follow the change first (entry by entry), so
     * the events below can carry their new versions. Changes to contents
     * or attributes need nothing: details are read at each request. */
    if (e->k == E_CREATE || e->k == E_DELETE || e->k == E_MOVE) {
        char d[PATH_MAX], d2[PATH_MAX];
        unsigned char type = DT_UNKNOWN;
        struct stat st;
        if (e->path[0] && lstat(e->path, &st) == 0)
            type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
        else if (e->isdir) type = DT_DIR;
        const char *nn = e->path[0] ? strrchr(e->path, '/') + 1 : "", *on = e->from[0] ? strrchr(e->from, '/') + 1 : "";
        if (e->path[0]) dir_of(e->path, d, sizeof d);
        if (e->from[0]) dir_of(e->from, d2, sizeof d2);
        if (e->k == E_CREATE) osd_listing_apply(d, 'c', nn, NULL, type);
        else if (e->k == E_DELETE) osd_listing_apply(d, 'd', nn, NULL, type);
        else if (e->path[0] && e->from[0] && !strcmp(d, d2)) osd_listing_apply(d, 'r', on, nn, type);
        else {
            if (e->from[0]) osd_listing_apply(d2, 'd', on, NULL, type);
            if (e->path[0]) osd_listing_apply(d, 'c', nn, NULL, type);
        }
    }
    if (osd_on_fs_event && !tmp_path) {
        if (e->k == E_CREATE) osd_on_fs_event('c', "", e->path);
        else if (e->k == E_DELETE) osd_on_fs_event('d', "", e->path);
        else if (e->k == E_MOVE && tmp_old) osd_on_fs_event('c', "", e->path);
        else if (e->k == E_MOVE) osd_on_fs_event('m', e->from, e->path);
    }
    if (!tmp_path && (e->k == E_CREATE || e->k == E_MOVE)) osd_shadows_file_seen(e->path);   /* only .osdobj files */
    /* A folder that was itself moved or deleted: its own listing is gone. */
    if ((e->k == E_DELETE || e->k == E_MOVE) && e->isdir) {
        if (e->from[0]) osd_listing_invalidate(e->from);
        if (e->k == E_DELETE && e->path[0]) osd_listing_invalidate(e->path);
    }
    if (trace) {
        static const char *k[] = { "CREATE", "DELETE", "MOVE", "ATTRIB", "WRITE", "OVERFLOW" };
        fprintf(stderr, "osdd: event %s%s %s%s%s\n", k[e->k], e->isdir ? " (dir)" : "", e->from,
                e->from[0] ? " -> " : "", e->path);
    }
    if (e->k == E_OVERFLOW) { stats.overflows++; scan_soon(); return; }
    if ((e->path[0] && osd_path_excluded(e->path)) || (e->from[0] && osd_path_excluded(e->from))) return;
    /* Our own temporary names (shadow rewrites) are not events to follow,
     * except that a rename from one onto an object is a rewrite of it. */
    bool tmp_from = e->from[0] && strstr(e->from, "/" OSD_SHADOW_TMP);
    if (e->path[0] && strstr(e->path, "/" OSD_SHADOW_TMP)) return;
    if (tmp_from && e->k == E_MOVE) {
        sobj *dst = osd_sobj_by_path(e->path);
        if (dst) probe(J_PROBE_OVER, e->path, dst);
        return;
    }

    if (e->k == E_MOVE) {
        sobj *src = e->from[0] ? osd_sobj_by_path(e->from) : NULL;
        sobj *dst = e->path[0] ? osd_sobj_by_path(e->path) : NULL;
        if (e->isdir && e->from[0] && e->path[0]) rename_prefix(e->from, e->path);
        if (!e->path[0]) {
            /* It left the folders we watch: find it by its ID. */
            if (src && OsdObject_data(src->o)->id[0]) {
                struct job *j = calloc(1, sizeof *j);
                j->kind = J_SEEK;
                j->target = src;
                memcpy(j->dup_id, OsdObject_data(src->o)->id, sizeof j->dup_id);
                snprintf(j->path, sizeof j->path, "%s", e->from);
                start_job(j);
            }
            return;
        }
        /* The new file of a pending displacement was renamed away: cancel. */
        for (size_t i = 0; i < ndisp;) {
            if (disp[i].paired && strcmp(disp[i].newpath, e->from) == 0) disp_remove(i);
            else i++;
        }
        if (src) {
            if (dst && dst != src) forget_object(dst);   /* a real replacement */
            char olddir[PATH_MAX], newdir[PATH_MAX];
            dir_of(e->from, olddir, sizeof olddir);
            dir_of(e->path, newdir, sizeof newdir);
            for (size_t i = 0; i < ndisp;) { if (disp[i].obj == src) disp_remove(i); else i++; }
            if (!e->isdir) osd_sobj_set_path(src, e->path);
            else if (strcmp(src->path, e->path) != 0) osd_sobj_set_path(src, e->path);
            watch_parent(e->path);
            if (strcmp(olddir, newdir) == 0 && !e->isdir) {
                disp = realloc(disp, (ndisp + 1) * sizeof *disp);
                struct displaced *d = &disp[ndisp++];
                memset(d, 0, sizeof *d);
                d->obj = src;
                snprintf(d->oldpath, sizeof d->oldpath, "%s", e->from);
                d->backup = backup_names && is_backup_name(e->from, e->path);
            }
        } else if (dst) {
            /* Something renamed onto an object: an atomic save, or a
             * replacement. Decided by whether the incoming file has an ID. */
            probe(J_PROBE_OVER, e->path, dst);
        } else {
            probe(J_PROBE_CREATE, e->path, NULL);
        }
        return;
    }

    if (e->k == E_CREATE) {
        for (size_t i = 0; i < ndisp; i++) {
            struct displaced *d = &disp[i];
            if (strcmp(d->oldpath, e->path) != 0) continue;
            if (d->backup) {             /* backup-names on: a save at once */
                transfer(d->obj, e->path, false);
                disp_remove(i);
            } else {
                d->paired = true;
                snprintf(d->newpath, sizeof d->newpath, "%s", e->path);
            }
            return;
        }
        if (!e->isdir) probe(J_PROBE_CREATE, e->path, NULL);
        return;
    }

    if (e->k == E_DELETE) {
        sobj *s = osd_sobj_by_path(e->path);
        for (size_t i = 0; i < ndisp;) {   /* the new file of a pairing removed: cancel */
            if (disp[i].paired && strcmp(disp[i].newpath, e->path) == 0) disp_remove(i);
            else i++;
        }
        for (size_t i = 0; i < ndups;) {
            if (strcmp(dups[i].newpath, e->path) == 0) dup_remove(i);
            else i++;
        }
        if (!s) return;
        for (size_t i = 0; i < ndisp; i++)
            if (disp[i].obj == s && disp[i].paired) {
                /* vim: the renamed original is deleted after the new file
                 * was written: the identity passes to the new file. */
                char np[PATH_MAX];
                snprintf(np, sizeof np, "%s", disp[i].newpath);
                disp_remove(i);
                transfer(s, np, false);
                return;
            }
        for (size_t i = 0; i < ndups; i++)
            if (dups[i].orig == s) {
                /* The original of a duplicate is gone: it was a move. */
                struct job *j = calloc(1, sizeof *j);
                j->kind = J_ADOPT;
                j->target = s;
                snprintf(j->path, sizeof j->path, "%s", dups[i].newpath);
                dup_remove(i);
                start_job(j);
                return;
            }
        delete_later(s);   /* unless a new file turns out to be it */
        drain_deletes();
        return;
    }

    /* An attribute arrived on a freshly created file: does it carry an ID
     * now? Not for the new half of a pending save, which is expected to
     * carry the object's ID (full vim copies attributes). */
    if (e->k == E_ATTRIB && !osd_sobj_by_path(e->path) && recent_take(e->path)) {
        bool known = false;
        for (size_t i = 0; i < ndups && !known; i++) known = strcmp(dups[i].newpath, e->path) == 0;
        for (size_t i = 0; i < ndisp && !known; i++) known = disp[i].paired && strcmp(disp[i].newpath, e->path) == 0;
        if (!known) probe(J_PROBE_CREATE, e->path, NULL);
    }
    /* E_ATTRIB, E_WRITE: a duplicate may now be a copy; a displaced
     * object that is modified is no longer part of a save. */
    for (size_t i = 0; i < ndups; i++)
        if (strcmp(dups[i].newpath, e->path) == 0 || (dups[i].orig && strcmp(dups[i].orig->path, e->path) == 0))
            dup_check(i);
    if (e->k == E_WRITE)
        for (size_t i = 0; i < ndisp;) {
            if (strcmp(disp[i].obj->path, e->path) == 0) disp_remove(i);
            else i++;
        }
}

/* ------------------------------------------------------ catch-up scan */

struct lrow { char id[OSD_ID_LEN + 1]; char vol[OSD_VOLID_MAX]; char rel[PATH_MAX]; char found[PATH_MAX]; bool ok; };

struct scan_job {
    struct lrow *rows;
    size_t n, cap;
    size_t missing;
    void (*done)(void *, const char *);
    void *arg;
    char report[256];
};

static void collect_row(const char *id, const char *vol, const char *rel, void *user)
{
    struct scan_job *s = user;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->rows = realloc(s->rows, s->cap * sizeof *s->rows);
    }
    struct lrow *r = &s->rows[s->n++];
    memset(r, 0, sizeof *r);
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->vol, sizeof r->vol, "%s", vol);
    snprintf(r->rel, sizeof r->rel, "%s", rel ? rel : "");
}

static struct scan_job *cur_scan;   /* nftw has no user pointer; scans never overlap */

static int scan_visit(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)ftw;
    if (osd_path_untracked(path)) return flag == FTW_D ? FTW_SKIP_SUBTREE : FTW_CONTINUE;
    if (flag == FTW_SL || flag == FTW_NS || flag == FTW_DNR) return FTW_CONTINUE;
    char id[OSD_ID_LEN + 1];
    if (!osd_attr_get_id(path, id)) return FTW_CONTINUE;
    for (size_t i = 0; i < cur_scan->n; i++) {
        struct lrow *r = &cur_scan->rows[i];
        if (!r->ok && !r->found[0] && strcmp(r->id, id) == 0) {
            snprintf(r->found, sizeof r->found, "%s", path);
            if (--cur_scan->missing == 0) return FTW_STOP;
        }
    }
    return FTW_CONTINUE;
}

static void scan_work(void *arg)
{
    struct scan_job *s = arg;
    osd_db_locations(db, collect_row, s);
    char vols[64][OSD_VOLID_MAX];
    size_t nvols = 0;
    for (size_t i = 0; i < s->n; i++) {
        struct lrow *r = &s->rows[i];
        osd_volume v;
        if (!osd_volume_for_uuid(r->vol, &v)) { r->ok = true; continue; }   /* not mounted: unavailable, not lost */
        char path[PATH_MAX + 8], id[OSD_ID_LEN + 1];
        snprintf(path, sizeof path, "%s%s%s", v.mnt, strcmp(v.mnt, "/") ? "/" : "", r->rel);
        if (osd_attr_get_id(path, id) && strcmp(id, r->id) == 0) {
            r->ok = true;
            snprintf(r->found, sizeof r->found, "%s", path);
            continue;
        }
        s->missing++;
        size_t k;
        for (k = 0; k < nvols && strcmp(vols[k], r->vol) != 0; k++) {}
        if (k == nvols && nvols < 64) snprintf(vols[nvols++], OSD_VOLID_MAX, "%s", r->vol);
    }
    /* Walk only the volumes where something is missing; never follow
     * symlinks or cross into other mounts. */
    size_t before = s->missing;
    for (size_t k = 0; k < nvols && s->missing; k++) {
        osd_volume v;
        if (!osd_volume_for_uuid(vols[k], &v)) continue;
        cur_scan = s;
        nftw(v.mnt, scan_visit, 32, FTW_PHYS | FTW_MOUNT | FTW_ACTIONRETVAL);
        cur_scan = NULL;
    }
    size_t found = 0;
    for (size_t i = 0; i < s->n; i++) {
        struct lrow *r = &s->rows[i];
        if (r->ok || !r->found[0]) continue;
        osd_volume v;
        if (osd_volume_for_uuid(r->vol, &v)) osd_db_set_location(db, r->id, r->vol, osd_volume_relpath(&v, r->found));
        found++;
    }
    snprintf(s->report, sizeof s->report, "%zu known objects; %zu had moved, %zu found, %zu still missing",
             s->n, before, found, s->missing);
}

static bool scan_running, scan_again;
struct scan_waiter { void (*done)(void *, const char *); void *arg; };
static struct scan_waiter waiters[32];
static size_t nwaiters;

static void scan_done(void *arg)
{
    struct scan_job *s = arg;
    stats.scans++;
    stats.missing = s->missing;
    for (size_t i = 0; i < s->n; i++) {
        struct lrow *r = &s->rows[i];
        if (!r->found[0]) continue;
        sobj *o = osd_sobj_by_id(r->id);
        if (o && strcmp(o->path, r->found) != 0) osd_sobj_set_path(o, r->found);   /* shadows follow */
        else if (!o && !r->ok) osd_shadows_target_moved(r->id, r->found);          /* not loaded: shadows still follow */
        watch_parent(r->found);
    }
    if (s->done) s->done(s->arg, s->report);
    free(s->rows);
    free(s);
    scan_running = false;
    if (scan_again) {
        /* Requests that came during this scan get the next one's result. */
        scan_again = false;
        struct scan_waiter w = nwaiters ? waiters[--nwaiters] : (struct scan_waiter){ NULL, NULL };
        osd_tracker_scan(w.done, w.arg);
    }
}

void osd_tracker_scan(void (*done)(void *, const char *), void *arg)
{
    if (scan_running) {
        if (done && nwaiters < 32) waiters[nwaiters++] = (struct scan_waiter){ done, arg };
        else if (done) done(arg, "too many scan requests");
        scan_again = true;
        return;
    }
    struct scan_job *s = calloc(1, sizeof *s);
    s->done = done;
    s->arg = arg;
    scan_running = true;
    osdo_defer(loop, "catch-up scan", scan_work, scan_done, s);
}

static int scan_timer = -1;

static void on_scan_timer(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    uint64_t n;
    ssize_t r = read(fd, &n, sizeof n);
    (void)r;
    osd_tracker_scan(NULL, NULL);
}

/* After an overflow, wait for the burst to settle, then scan once. */
static void scan_soon(void)
{
    struct itimerspec its = { .it_value = { 0, 500 * 1000 * 1000 } };
    timerfd_settime(scan_timer, 0, &its, NULL);
}

/* --------------------------------------------------------------- init */

void osd_tracker_get_stats(osd_tracker_stats *out) { *out = stats; }

void osd_tracker_init(osdo_loop *l, const osd_env *e, osd_db *d)
{
    loop = l;
    env = e;
    db = d;
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/backup-names", env->config_dir);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[16] = "";
        backup_names = fgets(line, sizeof line, f) && (line[0] == '1' || strncmp(line, "on", 2) == 0 || strncmp(line, "yes", 3) == 0);
        fclose(f);
    }
    osd_on_saved = on_saved;
    trace = getenv("OSD_TRACE") && *getenv("OSD_TRACE") == '1';
    helper_connect();
    stats.mode = helper_fd >= 0 ? "osd-watchd" : "inotify";
    scan_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    osdo_loop_watch_fd(loop, scan_timer, POLLIN, on_scan_timer, NULL);
    osd_tracker_scan(NULL, NULL);   /* catch up with what happened while we were not running */
}

void osd_tracker_shutdown(void)
{
    if (helper_fd >= 0) close(helper_fd);
    if (ino_fd >= 0) close(ino_fd);
    if (scan_timer >= 0) close(scan_timer);
    for (size_t i = 0; i < nreg_pending; i++) free(reg_pending[i]);
    free(reg_pending);
    free(watches);
    free(disp);
    free(dups);
    watches = NULL;
    nwatches = capwatches = 0;
}
