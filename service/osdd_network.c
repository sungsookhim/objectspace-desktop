/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* osdd: Network (0.8 step 3; v0.8-design 1). The Network folder (in My
 * System's Storages, beside Drives) holds a share object (OsdShare) for
 * each network share mounted now: the kernel's NFS, SMB and SSHFS mounts,
 * and GNOME's virtual file systems' (smb, sftp, dav, ftp...). A share is
 * known by its address ("net:smb://server/share", its volume's identity:
 * volumes.c), so its object, and the state of the objects on it, come back
 * when it is mounted again, wherever. A share no longer mounted is parked
 * (as a drive is), its title and settings kept.
 *
 * Others' changes on a share are not reported by the kernel: a folder
 * window open on one is listed again every five seconds (on a worker),
 * and what came or went is told as a local change would be. GNOME's
 * virtual file systems all live under one FUSE mount, so a share mounted
 * there does not show in mountinfo: its folder is looked at every five
 * seconds too. */
#define _GNU_SOURCE
#include "osdd.h"

#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/timerfd.h>
#include <sys/wait.h>

#define NET_PARKED "00000000-0000-4000-8000-00000000ae70"

void network_dir(char *out, size_t n)
{
    char comp[PATH_MAX], drv[PATH_MAX + 8];
    computer_dirs(comp, sizeof comp, drv, sizeof drv);
    snprintf(out, n, "%s/Network", comp);
}

/* "projects on fileserver" from "smb://user@fileserver/projects" */
static void share_title(const char *addr, char *out, size_t n)
{
    const char *p = strstr(addr, "://");
    p = p ? p + 3 : addr;
    const char *at = strchr(p, '@'), *sl = strchr(p, '/');
    if (at && (!sl || at < sl)) p = at + 1;
    sl = strchr(p, '/');
    char host[256];
    snprintf(host, sizeof host, "%.*s", sl ? (int)(sl - p) : (int)strlen(p), p);
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;   /* (a port: not in the title) */
    const char *last = sl ? strrchr(sl, '/') : NULL;
    if (last && last[1]) snprintf(out, n, "%s on %s", last + 1, host);
    else snprintf(out, n, "%s", host);
}

/* ---- a share's state, for its menu and notebook (OsdDrive's hook) */

bool share_query(const char *uuid, osd_drive_state *st, bool details)
{
    osd_volume v;
    snprintf(st->kind, sizeof st->kind, "network");
    if (!osd_volume_for_uuid(uuid, &v)) return true;   /* known, not connected */
    st->present = st->mounted = true;
    snprintf(st->device, sizeof st->device, "%.63s", uuid + 4);
    snprintf(st->fstype, sizeof st->fstype, "%s", v.fstype);
    snprintf(st->mount, sizeof st->mount, "%s", v.mnt);
    struct statvfs sv;   /* (details: the notebook's, asked for on a worker) */
    if (details && statvfs(v.mnt, &sv) == 0) {
        human_size((uint64_t)sv.f_blocks * sv.f_frsize, st->size, sizeof st->size);
        human_size((uint64_t)sv.f_bavail * sv.f_frsize, st->free, sizeof st->free);
    }
    return true;
}

/* ---- the Network folder kept in step with the shares mounted */

/* connect: a connection made from Objectspace (step 4): kept in Network
 * when not connected, its address to connect to again (with its user) */
struct share_obj { char id[OSD_ID_LEN + 1], uuid[OSD_VOLID_MAX], connect[PATH_MAX]; bool parked, seen; };
/* connections just made: their share's object marked when the sync meets it */
struct mark { char uuid[OSD_VOLID_MAX], address[PATH_MAX]; };
static struct mark *marks;
static size_t nmarks;
struct net_event { char folder[PATH_MAX]; char id[OSD_ID_LEN + 1]; char kind; };
struct net_sync {
    char dir[PATH_MAX], vol[OSD_VOLID_MAX], fid[OSD_ID_LEN + 1];
    osd_volumes vs;
    struct share_obj *objs;
    size_t nobjs;
    bool collecting_parked;
    struct net_event *ev;
    size_t nev;
    struct mark *marks;
    size_t nmarks;
};

static bool net_syncing, net_again;

static void net_collect(const char *vol, const char *id, void *user)
{
    struct net_sync *j = user;
    char loc[OSD_ID_LEN + 8];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    osd_props ps;
    osd_props_init(&ps);
    char *v = NULL;
    bool share = osd_db_get_class(db, vol, loc, "Objectspace", &ps) > 0 && osd_props_str(&ps, "class", &v) && !strcmp(v, "OsdShare");
    free(v);
    v = NULL;
    osd_props_free(&ps);
    if (!share) return;
    struct share_obj o;
    memset(&o, 0, sizeof o);
    snprintf(o.id, sizeof o.id, "%s", id);
    o.parked = j->collecting_parked;
    osd_props_init(&ps);
    if (osd_db_get_class(db, vol, loc, "OsdDrive", &ps) > 0 && osd_props_str(&ps, "uuid", &v)) snprintf(o.uuid, sizeof o.uuid, "%s", v);
    free(v);
    v = NULL;
    osd_props_free(&ps);
    osd_props_init(&ps);
    if (osd_db_get_class(db, vol, loc, "OsdShare", &ps) > 0 && osd_props_str(&ps, "connect", &v)) snprintf(o.connect, sizeof o.connect, "%s", v);
    free(v);
    osd_props_free(&ps);
    j->objs = realloc(j->objs, (j->nobjs + 1) * sizeof *j->objs);
    j->objs[j->nobjs++] = o;
}

static void net_event(struct net_sync *j, const char *id, char kind)
{
    j->ev = realloc(j->ev, (j->nev + 1) * sizeof *j->ev);
    struct net_event *e = &j->ev[j->nev++];
    snprintf(e->folder, sizeof e->folder, "%s", j->dir);
    snprintf(e->id, sizeof e->id, "%s", id);
    e->kind = kind;
}

static void put_str(const char *vol, const char *id, const char *cls, const char *prop, const char *v)
{
    char loc[OSD_ID_LEN + 8], key[PATH_MAX + 128];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    osd_db_key(key, sizeof key, loc, cls, prop);
    osd_db_put(db, vol, key, 's', v, (uint32_t)strlen(v));
}

static void net_sync_work(void *arg)
{
    struct net_sync *j = arg;
    osd_loaded d;
    osd_load(j->dir, true, &d);
    bool ok = !d.err && d.id[0] && S_ISDIR(d.st.st_mode);
    if (ok) {
        snprintf(j->vol, sizeof j->vol, "%s", d.vol);
        snprintf(j->fid, sizeof j->fid, "%s", d.id);
    }
    osd_loaded_free(&d);
    if (!ok) return;   /* no Network folder (first start not yet, or removed) */
    osd_central_in(j->vol, j->fid, net_collect, j);
    j->collecting_parked = true;
    osd_central_in(j->vol, NET_PARKED, net_collect, j);
    for (size_t i = 0; i < j->vs.n; i++) {
        const osd_volume *v = &j->vs.v[i];
        if (!osd_volume_is_net(v)) continue;
        struct share_obj *o = NULL;
        for (size_t k = 0; k < j->nobjs && !o; k++)
            if (!j->objs[k].seen && !strcmp(j->objs[k].uuid, v->uuid)) o = &j->objs[k];
        char newid[OSD_ID_LEN + 1];
        const char *mark = NULL;   /* (a connection just made from Objectspace: remembered) */
        for (size_t m = 0; m < j->nmarks && !mark; m++) if (!strcmp(j->marks[m].uuid, v->uuid)) mark = j->marks[m].address;
        if (!o) {   /* a share not seen before: its object */
            char title[300];
            share_title(v->uuid + 4, title, sizeof title);
            if (osd_central_create(j->vol, j->fid, "OsdShare", title, NULL, newid) != 0) continue;
            put_str(j->vol, newid, "OsdDrive", "uuid", v->uuid);
            if (mark) put_str(j->vol, newid, "OsdShare", "connect", mark);
            net_event(j, newid, 'c');
            continue;
        }
        o->seen = true;
        if (mark && strcmp(o->connect, mark)) { put_str(j->vol, o->id, "OsdShare", "connect", mark); net_event(j, o->id, 't'); }
        if (o->parked && osd_central_move(o->id, j->vol, j->fid, false, newid) == 0) net_event(j, o->id, 'c');   /* back */
    }
    for (size_t k = 0; k < j->nobjs; k++) {
        struct share_obj *o = &j->objs[k];
        if (o->seen) continue;
        char newid[OSD_ID_LEN + 1];
        if (o->connect[0]) {   /* a connection of the user's: stays in Network, not connected (its icon dark) */
            if (o->parked && osd_central_move(o->id, j->vol, j->fid, false, newid) == 0) net_event(j, o->id, 'c');
            continue;
        }
        if (!o->parked) net_event(j, o->id, 'p');   /* gone: to be parked (on the loop: net_sync_done) */
    }
}

static void net_sync_done(void *arg)
{
    struct net_sync *j = arg;
    for (size_t i = 0; i < j->nev; i++) {
        /* (out of memory first, then moved: an object in memory saved its
         * folder back after a move, the share's object stayed in Network) */
        osd_central_forget(j->ev[i].id);
        if (j->ev[i].kind == 'p') {   /* gone: parked, its title and settings kept */
            char newid[OSD_ID_LEN + 1];
            int rc = osd_central_move(j->ev[i].id, j->vol, NET_PARKED, false, newid);
            if (rc) { log_msg("network: %s could not be parked (%d)", j->ev[i].id, rc); continue; }
            j->ev[i].kind = 'd';
        }
        if (j->ev[i].kind != 't') on_central_change(j->ev[i].folder, j->ev[i].id, j->ev[i].kind);
    }
    for (size_t m = 0; m < j->nmarks; m++)   /* (met: done with) */
        for (size_t k = 0; k < nmarks; k++)
            if (!strcmp(marks[k].uuid, j->marks[m].uuid)) { marks[k] = marks[--nmarks]; break; }
    free(j->marks);
    drives_broadcast();   /* (shares' icons, lit or not: connected, disconnected) */
    osd_volumes_free(&j->vs);
    free(j->objs);
    free(j->ev);
    free(j);
    net_syncing = false;
    if (net_again) { net_again = false; network_changed(); }
}

void network_changed(void)
{
    if (net_syncing) { net_again = true; return; }
    net_syncing = true;
    struct net_sync *j = calloc(1, sizeof *j);
    network_dir(j->dir, sizeof j->dir);
    osd_volumes_snapshot(&j->vs);
    if (nmarks) {
        j->marks = malloc(nmarks * sizeof *j->marks);
        memcpy(j->marks, marks, nmarks * sizeof *marks);
        j->nmarks = nmarks;
    }
    osdo_defer(loop, "network sync", net_sync_work, net_sync_done, j);
}

static void share_forget(struct client *c, int64_t id, const char *target);

/* ---- Disconnect: {op:"share", target, action:"disconnect"}; Forget: action "forget" */

struct share_req { struct client *c; int64_t id; char target[PATH_MAX], uuid[OSD_VOLID_MAX], err[256]; };

static void disconnect_work(void *arg)
{
    struct share_req *q = arg;
    osd_loaded ld;
    osd_load(q->target, false, &ld);
    if (!ld.err) {
        const char *loc = ld.locator[0] ? ld.locator : ld.path;
        osd_props ps;
        osd_props_init(&ps);
        char *u = NULL;
        if (!strcmp(ld.obj_class, "OsdShare") && osd_db_get_class(db, ld.vol, loc, "OsdDrive", &ps) > 0 && osd_props_str(&ps, "uuid", &u))
            snprintf(q->uuid, sizeof q->uuid, "%s", u);
        free(u);
        osd_props_free(&ps);
    }
    osd_loaded_free(&ld);
    osd_volume v;
    if (!q->uuid[0]) { snprintf(q->err, sizeof q->err, "not a network share"); return; }
    if (!osd_volume_for_uuid(q->uuid, &v)) { snprintf(q->err, sizeof q->err, "not connected"); return; }
    /* GNOME's: through gio (as GNOME's Files does); the kernel's: umount
     * (the user's own mounts: "user" in fstab; else the system's to do) */
    bool gvfs = !strcmp(v.fstype, "gvfs");
    char *argv[] = { gvfs ? "gio" : "umount", gvfs ? "mount" : v.mnt, gvfs ? "-u" : NULL, v.mnt, NULL };
    if (!gvfs) argv[2] = NULL;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) { snprintf(q->err, sizeof q->err, "%s", strerror(errno)); return; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int e = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (e) { close(p[0]); snprintf(q->err, sizeof q->err, "%s could not be run", argv[0]); return; }
    char msg[256] = "";
    ssize_t n = read(p[0], msg, sizeof msg - 1);
    msg[n > 0 ? n : 0] = 0;
    close(p[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        msg[strcspn(msg, "\n")] = 0;
        snprintf(q->err, sizeof q->err, "%s", msg[0] ? msg : "it could not be disconnected");
    }
}

static void disconnect_done(void *arg)
{
    struct share_req *q = arg;
    if (!q->c->dead) {
        if (q->err[0]) reply_error(q->c, q->id, "failed", q->err);
        else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 2);
            cb_text(&b, "id"); cb_int(&b, q->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            send_frame(q->c, &b);
            cb_free(&b);
        }
    }
    client_unref(q->c);
    if (!q->err[0]) start_volume_job(NULL);   /* (gone: the volumes, then the Network folder) */
    free(q);
}

bool op_share(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *target = cv_text(msg, "target"), *action = cv_text(msg, "action");
    if (!target || !action || (strcmp(action, "disconnect") && strcmp(action, "forget")) || strlen(target) >= PATH_MAX) {
        reply_error(c, id, "bad request", "share needs a target and action \"disconnect\" or \"forget\"");
        return false;
    }
    if (!strcmp(action, "forget")) { share_forget(c, id, target); return false; }
    struct share_req *q = calloc(1, sizeof *q);
    q->c = c; c->refs++;
    q->id = id;
    snprintf(q->target, sizeof q->target, "%s", target);
    osdo_defer(loop, "share disconnect", disconnect_work, disconnect_done, q);
    return false;
}

/* ---- every five seconds: GNOME's shares, and windows open on a share */

static int tick_fd = -1;
static bool gvfs_reading, polling;

/* a folder on a share, as last listed */
struct polled { char *folder; char **names; size_t n; bool listed; };
static struct polled *pl;
static size_t npl;

struct poll_job {
    struct polled *f;   /* copies: folder, the names before */
    size_t n;
    struct { char **names; size_t n; bool ok; } *now;
};

static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void poll_work(void *arg)
{
    struct poll_job *j = arg;
    for (size_t i = 0; i < j->n; i++) {
        DIR *d = opendir(j->f[i].folder);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            j->now[i].names = realloc(j->now[i].names, (j->now[i].n + 1) * sizeof(char *));
            j->now[i].names[j->now[i].n++] = strdup(de->d_name);
        }
        closedir(d);
        qsort(j->now[i].names, j->now[i].n, sizeof(char *), by_str);
        j->now[i].ok = true;
    }
}

static void tell(const char *folder, const char *name, char kind)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", folder, name);
    struct stat st;
    unsigned char type = kind == 'c' && lstat(p, &st) == 0 ? (S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : DT_REG) : DT_UNKNOWN;
    osd_listing_apply(folder, kind, name, NULL, type);
    on_fs_event(kind, "", p);
}

static void poll_done(void *arg)
{
    struct poll_job *j = arg;
    for (size_t i = 0; i < j->n; i++) {
        struct polled *f = NULL;   /* (still polled: compare, keep the new) */
        for (size_t k = 0; k < npl && !f; k++) if (!strcmp(pl[k].folder, j->f[i].folder)) f = &pl[k];
        if (f && j->now[i].ok) {
            if (f->listed) {   /* (the first time: what is there now, nothing told) */
                size_t a = 0, b = 0;
                while (a < f->n || b < j->now[i].n) {
                    int c = a == f->n ? 1 : b == j->now[i].n ? -1 : strcmp(f->names[a], j->now[i].names[b]);
                    if (c < 0) tell(f->folder, f->names[a++], 'd');
                    else if (c > 0) tell(f->folder, j->now[i].names[b++], 'c');
                    else { a++; b++; }
                }
            }
            for (size_t k = 0; k < f->n; k++) free(f->names[k]);
            free(f->names);
            f->names = j->now[i].names;
            f->n = j->now[i].n;
            f->listed = true;
            j->now[i].names = NULL;
        }
        for (size_t k = 0; k < j->now[i].n && j->now[i].names; k++) free(j->now[i].names[k]);
        free(j->now[i].names);
        free(j->f[i].folder);
    }
    free(j->f);
    free(j->now);
    free(j);
    polling = false;
}

/* the folders open in windows that are on a share: polled; others dropped */
static void poll_shares(void)
{
    if (polling) return;
    size_t keep = 0;
    for (size_t k = 0; k < npl; k++) {
        bool open = false;
        for (struct sub *x = subs; x && !open; x = x->next)
            for (size_t i = 0; i < x->nfolders && !open; i++) open = !x->c->dead && !strcmp(x->folders[i], pl[k].folder);
        if (open) { pl[keep++] = pl[k]; continue; }
        for (size_t i = 0; i < pl[k].n; i++) free(pl[k].names[i]);
        free(pl[k].names);
        free(pl[k].folder);
    }
    npl = keep;
    for (struct sub *x = subs; x; x = x->next)
        for (size_t i = 0; i < x->nfolders; i++) {
            if (x->c->dead) continue;
            bool have = false;
            for (size_t k = 0; k < npl && !have; k++) have = !strcmp(pl[k].folder, x->folders[i]);
            osd_volume v;
            if (have || !osd_volume_for_path(x->folders[i], &v) || !osd_volume_is_net(&v)) continue;
            pl = realloc(pl, (npl + 1) * sizeof *pl);
            pl[npl++] = (struct polled){ strdup(x->folders[i]), NULL, 0, false };
        }
    if (!npl) return;
    struct poll_job *j = calloc(1, sizeof *j);
    j->n = npl;
    j->f = calloc(npl, sizeof *j->f);
    j->now = calloc(npl, sizeof *j->now);
    for (size_t k = 0; k < npl; k++) j->f[k].folder = strdup(pl[k].folder);
    polling = true;
    osdo_defer(loop, "share folders", poll_work, poll_done, j);
}

/* GNOME's shares: the ones in its folder now against the volumes known
 * (not against the last look: a scan that ran as a share went is put
 * right at the next look); different: the volumes again */
struct gvfs_job { char names[8192]; };

static int by_cstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* the names (one per line, sorted) of gvfs shares in a list */
static void sorted_lines(char **v, size_t n, char *out, size_t size)
{
    qsort(v, n, sizeof *v, by_cstr);
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && o + 2 < size; i++) o += (size_t)snprintf(out + o, size - o, "%s\n", v[i]);
}

static void gvfs_work(void *arg)
{
    struct gvfs_job *j = arg;
    char dir[PATH_MAX];
    osd_gvfs_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    char *v[256];
    size_t n = 0;
    struct dirent *de;
    char addr[PATH_MAX];
    while ((de = readdir(d)) && n < 256)
        if (osd_gvfs_address(de->d_name, addr, sizeof addr)) v[n++] = strdup(de->d_name);
    closedir(d);
    sorted_lines(v, n, j->names, sizeof j->names);
    for (size_t i = 0; i < n; i++) free(v[i]);
}

static void gvfs_done(void *arg)
{
    struct gvfs_job *j = arg;
    osd_volumes vs;
    osd_volumes_snapshot(&vs);
    char *v[256], known[8192];
    size_t n = 0;
    for (size_t i = 0; i < vs.n && n < 256; i++)
        if (!strcmp(vs.v[i].fstype, "gvfs")) v[n++] = strrchr(vs.v[i].mnt, '/') + 1;
    sorted_lines(v, n, known, sizeof known);
    if (strcmp(j->names, known)) start_volume_job(NULL);
    osd_volumes_free(&vs);
    free(j);
    gvfs_reading = false;
}

static void on_tick(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    uint64_t n;
    if (read(fd, &n, sizeof n) != sizeof n) return;
    if (!gvfs_reading) {
        gvfs_reading = true;
        osdo_defer(loop, "gvfs shares", gvfs_work, gvfs_done, calloc(1, sizeof(struct gvfs_job)));
    }
    poll_shares();
}

void network_init(void)
{
    char dir[PATH_MAX];
    network_dir(dir, sizeof dir);
    osd_listing_folder_icon(dir, "network.png");
    const char *e = getenv("OSD_NET_POLL_MS");   /* (tests: sooner) */
    long ms = e && atol(e) > 0 ? atol(e) : 5000;
    tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tick_fd < 0) return;
    struct itimerspec its = { .it_value = { ms / 1000, (ms % 1000) * 1000000L }, .it_interval = { ms / 1000, (ms % 1000) * 1000000L } };
    timerfd_settime(tick_fd, 0, &its, NULL);
    osdo_loop_watch_fd(loop, tick_fd, POLLIN, on_tick, NULL);
}


/* ---- connecting (0.8 step 4): {op:"connect", address, user, password,
 * remember} -> {folder}; errors: "password needed" (asked for: one is
 * needed and none was given), "refused" (the user or password), "not
 * found" (the server), "no share" (not on the server), "failed" */

struct conn_req {
    struct client *c;
    int64_t id;
    char address[PATH_MAX], user[256], domain[128], pass[256];
    bool remember, creds;
    char code[32], err[512], uuid[OSD_VOLID_MAX], mnt[PATH_MAX], remembered[PATH_MAX];
    bool open_reply;   /* (an open of a remembered share: answered as an open, {folder}) */
};

/* scheme, host, user and the rest of an address ("smb://dom;u@host/share") */
static void parse_address(const char *a, char *scheme, size_t sn, char *host, size_t hn, char *user, size_t un, char *dom, size_t dn,
                          char *rest, size_t rn)
{
    scheme[0] = host[0] = user[0] = dom[0] = rest[0] = 0;
    const char *p = strstr(a, "://");
    if (!p) return;
    snprintf(scheme, sn, "%.*s", (int)(p - a), a);
    p += 3;
    const char *sl = strchr(p, '/'), *at = strchr(p, '@');
    if (at && (!sl || at < sl)) {
        char who[300];
        snprintf(who, sizeof who, "%.*s", (int)(at - p), p);
        char *semi = strchr(who, ';');
        if (semi) { *semi = 0; snprintf(dom, dn, "%s", who); snprintf(user, un, "%s", semi + 1); }
        else snprintf(user, un, "%s", who);
        p = at + 1;
    }
    sl = strchr(p, '/');
    snprintf(host, hn, "%.*s", sl ? (int)(sl - p) : (int)strlen(p), p);
    snprintf(rest, rn, "%s", sl ? sl : "");
}

static void gvfs_names(char *out, size_t n)
{
    char dir[PATH_MAX];
    osd_gvfs_dir(dir, sizeof dir);
    out[0] = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    size_t o = 0;
    struct dirent *de;
    while ((de = readdir(d)) && o + 2 < n)
        if (de->d_name[0] != '.') o += (size_t)snprintf(out + o, n - o, "%s\n", de->d_name);
    closedir(d);
}

static bool ends_with_prompt(const char *buf, const char *what)
{
    const char *p = strstr(buf, what);
    return p != NULL;
}

/* gio mount ADDRESS, its questions answered: user, domain, password (once) */
static int run_gio_mount(struct conn_req *q, char *out, size_t on, bool *asked_pass, int *passes)
{
    int in[2], o[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(o, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, o[1], 1);
    posix_spawn_file_actions_adddup2(&fa, o[1], 2);
    char *argv[] = { "gio", "mount", q->address, NULL };
    pid_t pid;
    int e = posix_spawnp(&pid, "gio", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(o[1]);
    if (e) { close(in[1]); close(o[0]); return -1; }
    size_t len = 0, seen = 0;
    out[0] = 0;
    bool open_in = true;
    *asked_pass = false;
    *passes = 0;
    double t0 = (double)time(NULL);
    for (;;) {
        struct pollfd pf = { o[0], POLLIN, 0 };
        int r = poll(&pf, 1, 1000);
        if (r > 0) {
            ssize_t k = read(o[0], out + len, on - len - 1);
            if (k <= 0) break;
            len += (size_t)k;
            out[len] = 0;
        }
        if ((double)time(NULL) - t0 > 60) { kill(pid, SIGTERM); snprintf(q->code, sizeof q->code, "failed"); break; }
        /* its questions, each answered once as it is asked */
        const char *tail = out + seen;
        char ans[400];
        const char *nl = NULL;
        if (open_in && strstr(tail, "User [")) {
            snprintf(ans, sizeof ans, "%s\n", q->user);   /* (empty: its own default) */
            nl = strstr(tail, "User [");
        } else if (open_in && strstr(tail, "Domain [")) {
            snprintf(ans, sizeof ans, "%s\n", q->domain);
            nl = strstr(tail, "Domain [");
        } else if (open_in && ends_with_prompt(tail, "Password:")) {
            *asked_pass = true;
            (*passes)++;
            nl = strstr(tail, "Password:");
            if (!q->creds || *passes > 1) {   /* none given, or the one given refused: no more asking */
                close(in[1]);
                open_in = false;
                seen = (size_t)(nl - out) + 9;
                continue;
            }
            snprintf(ans, sizeof ans, "%s\n", q->pass);
        }
        if (nl) {
            seen = (size_t)(strchr(nl, ':') ? strchr(nl, ':') - out + 1 : (nl - out) + 1);
            ssize_t w = write(in[1], ans, strlen(ans));
            (void)w;
            explicit_bzero(ans, sizeof ans);
        }
        if (r == 0) {   /* nothing new: has it ended? */
            int st;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                if (open_in) close(in[1]);
                close(o[0]);
                return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
            }
        }
    }
    if (open_in) close(in[1]);
    close(o[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* the keyring's network password, as GNOME's Files keeps it (gvfs finds it
 * there when it connects again: no asking) */
static void keyring(bool store, const char *address, const char *pass)
{
    char scheme[32], host[256], user[256], dom[128], rest[PATH_MAX];
    parse_address(address, scheme, sizeof scheme, host, sizeof host, user, sizeof user, dom, sizeof dom, rest, sizeof rest);
    if (!host[0] || !user[0]) return;
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;
    char label[PATH_MAX + 32];
    snprintf(label, sizeof label, "%s", address);
    char *argv[16];
    int n = 0;
    argv[n++] = "secret-tool";
    argv[n++] = store ? "store" : "clear";
    if (store) { argv[n++] = "--label"; argv[n++] = label; }
    argv[n++] = "protocol"; argv[n++] = scheme;
    argv[n++] = "server"; argv[n++] = host;
    argv[n++] = "user"; argv[n++] = user;
    if (dom[0]) { argv[n++] = "domain"; argv[n++] = dom; }
    argv[n] = NULL;
    int in[2];
    if (pipe2(in, O_CLOEXEC)) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int e = posix_spawnp(&pid, "secret-tool", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    if (!e && store && pass) { ssize_t w = write(in[1], pass, strlen(pass)); (void)w; }
    close(in[1]);
    if (!e) waitpid(pid, NULL, 0);
}

/* gio's message in words a person reads */
static void explain(struct conn_req *q, const char *out, bool asked, int passes)
{
    char m[512];
    snprintf(m, sizeof m, "%s", out);
    if (asked && !q->creds) { snprintf(q->code, sizeof q->code, "password needed"); snprintf(q->err, sizeof q->err, "A user name and password are needed."); return; }
    if ((asked && passes > 1) || strcasestr(m, "permission denied") || strcasestr(m, "access denied") || strcasestr(m, "login") ||
        strcasestr(m, "authentication fail") || strcasestr(m, "logon failure")) {
        snprintf(q->code, sizeof q->code, "refused");
        snprintf(q->err, sizeof q->err, "The server refused the user name or password.");
    } else if (strcasestr(m, "not known") || (strcasestr(m, "not found") && strcasestr(m, "host")) || strcasestr(m, "no route") ||
               strcasestr(m, "could not resolve") || strcasestr(m, "failed to retrieve share list") || strcasestr(m, "unknown host")) {
        snprintf(q->code, sizeof q->code, "not found");
        snprintf(q->err, sizeof q->err, "The server was not found. Check its name, and that it is on.");
    } else if (strcasestr(m, "no such file") || strcasestr(m, "does not exist") || strcasestr(m, "not found")) {
        snprintf(q->code, sizeof q->code, "no share");
        snprintf(q->err, sizeof q->err, "The server has no such share or folder.");
    } else if (strcasestr(m, "refused")) {
        snprintf(q->code, sizeof q->code, "not found");
        snprintf(q->err, sizeof q->err, "The server did not answer (connection refused).");
    } else if (strcasestr(m, "not supported") || strcasestr(m, "unsupported")) {
        snprintf(q->code, sizeof q->code, "failed");
        snprintf(q->err, sizeof q->err, "This kind of address is not supported here (is GNOME's support for it installed? gvfs-smb, gvfs-nfs...)");
    } else {
        snprintf(q->code, sizeof q->code, "failed");
        const char *g = strstr(m, ": ");   /* "gio: ADDRESS: message" */
        const char *g2 = g ? strstr(g + 2, ": ") : NULL;
        snprintf(q->err, sizeof q->err, "It could not be connected: %s", g2 ? g2 + 2 : m[0] ? m : "no reason given");
        q->err[strcspn(q->err, "\n")] = 0;
    }
}

static void connect_work(void *arg)
{
    struct conn_req *q = arg;
    /* the address with its user (and domain), as it is remembered */
    char scheme[32], host[256], user[256], dom[128], rest[PATH_MAX];
    parse_address(q->address, scheme, sizeof scheme, host, sizeof host, user, sizeof user, dom, sizeof dom, rest, sizeof rest);
    if (!scheme[0] || !host[0]) {
        snprintf(q->code, sizeof q->code, "failed");
        snprintf(q->err, sizeof q->err, "That is not an address: write one as smb://server/share, sftp://host or nfs://server/export.");
        return;
    }
    if (q->creds && q->user[0]) {   /* "DOMAIN\\user" for SMB */
        char *bs = strchr(q->user, '\\');
        if (bs) { *bs = 0; snprintf(q->domain, sizeof q->domain, "%s", q->user); memmove(q->user, bs + 1, strlen(bs + 1) + 1); }
        snprintf(user, sizeof user, "%s", q->user);
        snprintf(dom, sizeof dom, "%s", q->domain);
    }
    snprintf(q->remembered, sizeof q->remembered, "%s://%s%s%s%s%s%s", scheme, dom[0] ? dom : "", dom[0] ? ";" : "", user, user[0] ? "@" : "",
             host, rest);
    char before[8192], after[8192], out[8192];
    gvfs_names(before, sizeof before);
    bool asked = false;
    int passes = 0;
    int rc = run_gio_mount(q, out, sizeof out, &asked, &passes);
    if (rc != 0) {
        if (!q->code[0]) explain(q, out, asked, passes);
        else snprintf(q->err, sizeof q->err, "It took too long; the server did not answer.");
        explicit_bzero(q->pass, sizeof q->pass);
        return;
    }
    /* which share it is: a folder new in GNOME's; else the one of that address */
    gvfs_names(after, sizeof after);
    char dir[PATH_MAX];
    osd_gvfs_dir(dir, sizeof dir);
    for (char *l = after, *e; *l; l = e + 1) {
        e = strchr(l, '\n');
        if (!e) break;
        *e = 0;
        char needle[PATH_MAX + 2];
        snprintf(needle, sizeof needle, "%s\n", l);
        char addr[PATH_MAX];
        if (!strstr(before, needle) && osd_gvfs_address(l, addr, sizeof addr)) {
            snprintf(q->uuid, sizeof q->uuid, "net:%s", addr);
            snprintf(q->mnt, sizeof q->mnt, "%s/%s", dir, l);
            break;
        }
    }
    if (!q->uuid[0]) {   /* (connected already) */
        osd_volumes vs;
        osd_volumes_scan(&vs, &env);
        char lhost[256];
        snprintf(lhost, sizeof lhost, "%s", host);
        for (char *c = lhost; *c; c++) *c = (char)tolower((unsigned char)*c);
        for (size_t i = 0; i < vs.n && !q->uuid[0]; i++) {
            const osd_volume *v = &vs.v[i];
            if (osd_volume_is_net(v) && strstr(v->uuid, lhost) && (!rest[0] || strstr(v->uuid, rest))) {
                snprintf(q->uuid, sizeof q->uuid, "%s", v->uuid);
                snprintf(q->mnt, sizeof q->mnt, "%s", v->mnt);
            }
        }
        osd_volumes_free(&vs);
    }
    if (q->remember && q->creds && q->pass[0]) keyring(true, q->remembered, q->pass);
    explicit_bzero(q->pass, sizeof q->pass);
    if (!q->mnt[0]) { snprintf(q->code, sizeof q->code, "failed"); snprintf(q->err, sizeof q->err, "It connected, but its folder was not found."); }
}

static void connect_done(void *arg)
{
    struct conn_req *q = arg;
    if (!q->code[0] && q->uuid[0]) {   /* remembered: its object marked at the next sync */
        marks = realloc(marks, (nmarks + 1) * sizeof *marks);
        snprintf(marks[nmarks].uuid, sizeof marks[nmarks].uuid, "%s", q->uuid);
        snprintf(marks[nmarks].address, sizeof marks[nmarks].address, "%s", q->remembered);
        nmarks++;
        start_volume_job(NULL);
    }
    if (!q->c->dead) {
        if (q->code[0]) reply_error(q->c, q->id, q->code, q->err);
        else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_int(&b, q->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result");
            cb_map(&b, 1);
            cb_text(&b, "folder"); cb_text(&b, q->mnt);   /* (the GUI opens a folder window) */
            send_frame(q->c, &b);
            cb_free(&b);
        }
    }
    client_unref(q->c);
    free(q);
}

static struct conn_req *new_conn(struct client *c, int64_t id, const char *address)
{
    struct conn_req *q = calloc(1, sizeof *q);
    q->c = c; c->refs++;
    q->id = id;
    snprintf(q->address, sizeof q->address, "%s", address);
    size_t l = strlen(q->address);   /* (trimmed) */
    while (l && (q->address[l - 1] == ' ' || q->address[l - 1] == '\n')) q->address[--l] = 0;
    return q;
}

bool op_connect(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *address = cv_text(msg, "address"), *user = cv_text(msg, "user"), *pass = cv_text(msg, "password");
    const char *target = cv_text(msg, "target");
    const cv *rem = cv_get(msg, "remember");
    char known[PATH_MAX] = "";
    if (!address && target) {   /* a remembered share's object ("vobj:ID"): its address */
        const char *oid = !strncmp(target, "vobj:", 5) ? target + 5 : target;
        char vol[OSD_VOLID_MAX], loc[OSD_ID_LEN + 8];
        if (strlen(oid) == OSD_ID_LEN && osd_db_vobj_vol(db, oid, vol, sizeof vol)) {
            snprintf(loc, sizeof loc, "vobj:%s", oid);
            osd_props ps;
            char *t = NULL;
            if (osd_db_get_class(db, vol, loc, "OsdShare", &ps) > 0 && osd_props_str(&ps, "connect", &t)) snprintf(known, sizeof known, "%s", t);
            free(t);
            osd_props_free(&ps);
        }
        if (known[0]) address = known;
    }
    if (!address || strlen(address) >= PATH_MAX) { reply_error(c, id, "bad request", "connect needs an address (or a share's target)"); return false; }
    struct conn_req *q = new_conn(c, id, address);
    q->remember = rem && rem->k == CV_BOOL && rem->b;
    if (user || pass) {
        q->creds = true;
        snprintf(q->user, sizeof q->user, "%s", user ? user : "");
        snprintf(q->pass, sizeof q->pass, "%s", pass ? pass : "");
    }
    osdo_defer(loop, "connect a share", connect_work, connect_done, q);
    return false;
}

/* An open of a share not connected: a remembered one connects first (its
 * password from the keyring, else "password needed" with its address). */
void share_open_connect(struct client *c, int64_t id, const char *address)
{
    osdo_defer(loop, "connect a share", connect_work, connect_done, new_conn(c, id, address));
}

/* Forget: its object gone, its password out of the keyring (a worker) */
struct forget_req { struct client *c; int64_t id; char obj[OSD_ID_LEN + 1], vol[OSD_VOLID_MAX], address[PATH_MAX], folder[PATH_MAX]; };

static void forget_work(void *arg)
{
    struct forget_req *q = arg;
    if (q->address[0]) keyring(false, q->address, NULL);
}

static void forget_done(void *arg)
{
    struct forget_req *q = arg;
    osd_central_forget(q->obj);
    osd_central_delete(q->obj);
    on_central_change(q->folder, q->obj, 'd');
    start_volume_job(NULL);   /* (connected still: shown again, as any share) */
    if (!q->c->dead) {
        cbuf b;
        cb_init(&b);
        cb_map(&b, 2);
        cb_text(&b, "id"); cb_int(&b, q->id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        send_frame(q->c, &b);
        cb_free(&b);
    }
    client_unref(q->c);
    free(q);
}

/* (target: "vobj:ID", the share's object) */
static void share_forget(struct client *c, int64_t id, const char *target)
{
    char vol[OSD_VOLID_MAX], loc[OSD_ID_LEN + 8];
    const char *oid = !strncmp(target, "vobj:", 5) ? target + 5 : target;
    if (strlen(oid) != OSD_ID_LEN || !osd_db_vobj_vol(db, oid, vol, sizeof vol)) { reply_error(c, id, "not found", "no such share"); return; }
    struct forget_req *q = calloc(1, sizeof *q);
    q->c = c; c->refs++;
    q->id = id;
    snprintf(q->obj, sizeof q->obj, "%s", oid);
    snprintf(loc, sizeof loc, "vobj:%s", oid);
    osd_props ps;
    char *t = NULL;
    if (osd_db_get_class(db, vol, loc, "OsdShare", &ps) > 0 && osd_props_str(&ps, "connect", &t)) snprintf(q->address, sizeof q->address, "%s", t);
    free(t);
    osd_props_free(&ps);
    network_dir(q->folder, sizeof q->folder);
    osdo_defer(loop, "forget a share", forget_work, forget_done, q);
}
