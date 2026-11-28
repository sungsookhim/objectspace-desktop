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

#include <dirent.h>
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

struct share_obj { char id[OSD_ID_LEN + 1], uuid[OSD_VOLID_MAX]; bool parked, seen; };
struct net_event { char folder[PATH_MAX]; char id[OSD_ID_LEN + 1]; char kind; };
struct net_sync {
    char dir[PATH_MAX], vol[OSD_VOLID_MAX], fid[OSD_ID_LEN + 1];
    osd_volumes vs;
    struct share_obj *objs;
    size_t nobjs;
    bool collecting_parked;
    struct net_event *ev;
    size_t nev;
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
        if (!o) {   /* a share not seen before: its object */
            char title[300];
            share_title(v->uuid + 4, title, sizeof title);
            if (osd_central_create(j->vol, j->fid, "OsdShare", title, NULL, newid) != 0) continue;
            put_str(j->vol, newid, "OsdDrive", "uuid", v->uuid);
            net_event(j, newid, 'c');
            continue;
        }
        o->seen = true;
        if (o->parked && osd_central_move(o->id, j->vol, j->fid, false, newid) == 0) net_event(j, o->id, 'c');   /* back */
    }
    for (size_t k = 0; k < j->nobjs; k++) {   /* gone: to be parked (on the loop: net_sync_done) */
        struct share_obj *o = &j->objs[k];
        if (!o->seen && !o->parked) net_event(j, o->id, 'p');
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
        on_central_change(j->ev[i].folder, j->ev[i].id, j->ev[i].kind);
    }
    if (j->nev) drives_broadcast();   /* (shares' icons, lit or not) */
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
    osdo_defer(loop, "network sync", net_sync_work, net_sync_done, j);
}

/* ---- Disconnect: {op:"share", target, action:"disconnect"} */

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
    if (!target || !action || strcmp(action, "disconnect") || strlen(target) >= PATH_MAX) {
        reply_error(c, id, "bad request", "share needs a target and action \"disconnect\"");
        return false;
    }
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
