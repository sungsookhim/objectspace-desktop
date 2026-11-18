/*
 * wps-watchd - the system helper that watches whole filesystems with
 * fanotify and forwards each user's service only the events it may see
 * (design: Change tracking, Per-user filtering in the helper).
 *
 * Validated as a prototype in M0 (proto/m0/watchd.c); this version speaks
 * CBOR, indexes registrations by file handle, marks a filesystem when the
 * first folder on it is registered (or, where the kernel refuses
 * filesystem marks, as on btrfs subvolumes, marks each registered folder),
 * and reads client groups with SO_PEERGROUPS.
 *
 * Protocol (SOCK_SEQPACKET, one CBOR map per message):
 *   client -> helper  {op:"reg"} with one open file descriptor attached
 *   helper -> client  {reg:N, ok:true, path}  |  {reg:-1, ok:false, error}
 *                     {revoked:N}   access to registration N was lost
 *                     {overflow:N}  N messages were dropped: rescan
 *                     {mask, dir:{reg,name}, old:{reg,name}|"<hidden>",
 *                      new:{reg,name}|"<hidden>", self:N}
 *
 * usage: wps-watchd [-s SOCKET]      (root; default /run/wps-watchd.sock)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include "cbor.h"

#define MAX_CLIENTS 256
#define MAX_GROUPS 128
#define MAX_FS 64
#define HSZ 128

/* How a filesystem is watched: one filesystem-wide mark, or (where the
 * kernel refuses that, e.g. btrfs subvolumes) one mark per registered
 * folder. */
struct fsmark { __kernel_fsid_t fsid; char mnt[PATH_MAX]; bool per_folder; };

struct reg {
    struct reg *hnext;          /* hash chain */
    struct client *c;
    int idx;                    /* the client's registration number */
    bool isdir, ok;
    unsigned long gen;          /* access checked at this generation */
    __kernel_fsid_t fsid;
    int htype;
    unsigned hlen;
    unsigned char h[HSZ];
};

struct client {
    int fd;
    uid_t uid;
    gid_t gid;
    gid_t groups[MAX_GROUPS];
    int ngroups;
    struct reg **regs;          /* by idx */
    int nregs, capregs;
    unsigned long dropped;
};

static struct fsmark fss[MAX_FS];
static int nfs;
static struct client *clients[MAX_CLIENTS];
static struct reg *table[4096];
static unsigned long access_gen = 1;
static gid_t root_groups[MAX_GROUPS];
static int root_ngroups;
static int fan = -1;
static volatile sig_atomic_t stop;
static unsigned long st_events, st_forwarded, st_checks;

static void on_signal(int s) { (void)s; stop = 1; }

static unsigned hkey(const __kernel_fsid_t *fsid, int type, const unsigned char *h, unsigned n)
{
    uint32_t x = 2166136261u;
    const unsigned char *f = (const unsigned char *)fsid;
    for (size_t i = 0; i < sizeof *fsid; i++) x = (x ^ f[i]) * 16777619u;
    x = (x ^ (unsigned)type) * 16777619u;
    for (unsigned i = 0; i < n; i++) x = (x ^ h[i]) * 16777619u;
    return x % (sizeof table / sizeof table[0]);
}

static struct fsmark *fs_of(const __kernel_fsid_t *fsid)
{
    for (int i = 0; i < nfs; i++)
        if (!memcmp(&fss[i].fsid, fsid, sizeof *fsid)) return &fss[i];
    return NULL;
}

/* --------------------------------------------------------------- sending */

static void send_msg(struct client *c, cbuf *b)
{
    if (b->oom) return;
    if (c->dropped) {
        cbuf n;
        cb_init(&n);
        cb_map(&n, 1);
        cb_text(&n, "overflow"); cb_uint(&n, c->dropped);
        if (send(c->fd, n.d, n.n, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) { c->dropped++; cb_free(&n); return; }
        c->dropped = 0;
        cb_free(&n);
    }
    /* Never wait for a slow client: drop and count (design: Responsiveness). */
    if (b->n && send(c->fd, b->d, b->n, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) c->dropped++;
}

/* --------------------------------------------------- credentials, access */

static void read_peer(struct client *c)
{
    struct ucred cr;
    socklen_t len = sizeof cr;
    getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &cr, &len);
    c->uid = cr.uid;
    c->gid = cr.gid;
    gid_t g[MAX_GROUPS];
    len = sizeof g;
    if (getsockopt(c->fd, SOL_SOCKET, SO_PEERGROUPS, g, &len) == 0) {
        c->ngroups = (int)(len / sizeof(gid_t));
        memcpy(c->groups, g, len);
    }
}

/* Open `path` with the client's filesystem credentials. The helper is
 * single-threaded; raw syscalls change only this thread, and moving fsuid
 * away from 0 drops the DAC override capabilities. */
static bool client_can_open(struct client *c, const char *path, bool isdir)
{
    st_checks++;
    syscall(SYS_setgroups, c->ngroups, c->groups);
    syscall(SYS_setfsgid, c->gid);
    syscall(SYS_setfsuid, c->uid);
    int fd = open(path, isdir ? (O_RDONLY | O_DIRECTORY | O_CLOEXEC) : (O_PATH | O_CLOEXEC));
    bool ok = fd >= 0;
    if (fd >= 0) close(fd);
    syscall(SYS_setfsuid, 0);
    syscall(SYS_setfsgid, 0);
    syscall(SYS_setgroups, root_ngroups, root_groups);
    return ok;
}

/* An fd on the filesystem, for open_by_handle_at. Opened only when needed
 * and closed at once: holding one would keep the volume from unmounting. */
static int fs_open(struct fsmark *f)
{
    /* Not O_PATH: open_by_handle_at refuses an O_PATH mount fd (EBADF). */
    int fd = open(f->mnt, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct statfs sf;
    if (fstatfs(fd, &sf) < 0 || memcmp(&sf.f_fsid, &f->fsid, sizeof f->fsid)) { close(fd); return -1; }
    return fd;
}

static bool reg_path(struct reg *r, char *out, size_t n)
{
    struct fsmark *f = fs_of(&r->fsid);
    if (!f) return false;
    int mfd = fs_open(f);
    if (mfd < 0) {
        if (getenv("WPS_TRACE")) printf("wps-watchd: fs_open(%s) failed: %s\n", f->mnt, strerror(errno));
        return false;
    }
    _Alignas(struct file_handle) unsigned char buf[sizeof(struct file_handle) + HSZ];
    struct file_handle *fh = (struct file_handle *)buf;
    fh->handle_type = r->htype;
    fh->handle_bytes = r->hlen;
    memcpy(buf + offsetof(struct file_handle, f_handle), r->h, r->hlen);
    int fd = open_by_handle_at(mfd, fh, O_PATH | O_CLOEXEC);
    int e = errno;
    close(mfd);
    if (fd < 0) {
        if (getenv("WPS_TRACE")) printf("wps-watchd: open_by_handle_at via %s failed: %s\n", f->mnt, strerror(e));
        return false;
    }
    char lp[64];
    snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd);
    ssize_t l = readlink(lp, out, n - 1);
    close(fd);
    if (l < 0) return false;
    out[l] = 0;
    return true;
}

static void reg_unlink(struct reg *r)
{
    unsigned k = hkey(&r->fsid, r->htype, r->h, r->hlen);
    for (struct reg **p = &table[k]; *p; p = &(*p)->hnext)
        if (*p == r) { *p = r->hnext; break; }
}

/* Still accessible to its client? Cached per generation. A registration
 * that fails is revoked and freed (the caller must not use it then). */
static bool reg_valid(struct reg *r)
{
    if (r->gen == access_gen) return r->ok;
    char path[PATH_MAX] = "";
    bool found = reg_path(r, path, sizeof path);
    bool ok = found && client_can_open(r->c, path, r->isdir);
    if (getenv("WPS_TRACE")) printf("wps-watchd: recheck reg %d: found=%d path=%s ok=%d\n", r->idx, found, path, ok);
    r->gen = access_gen;
    r->ok = ok;
    if (ok) return true;
    cbuf b;
    cb_init(&b);
    cb_map(&b, 1);
    cb_text(&b, "revoked"); cb_int(&b, r->idx);
    send_msg(r->c, &b);
    cb_free(&b);
    r->c->regs[r->idx] = NULL;
    reg_unlink(r);
    free(r);
    return false;
}

/* --------------------------------------------------------- registration */

static void reply_reg(struct client *c, int idx, const char *path, const char *error)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "reg"); cb_int(&b, idx);
    cb_text(&b, "ok"); cb_bool(&b, error == NULL);
    if (error) { cb_text(&b, "error"); cb_text(&b, error); }
    else { cb_text(&b, "path"); cb_text(&b, path); }
    send_msg(c, &b);
    cb_free(&b);
}

static const unsigned long long MASK = FAN_CREATE | FAN_DELETE | FAN_DELETE_SELF | FAN_MOVED_FROM | FAN_MOVED_TO |
                                 FAN_RENAME | FAN_MOVE_SELF | FAN_ATTRIB | FAN_CLOSE_WRITE | FAN_ONDIR;

static void do_register(struct client *c, int fd)
{
    struct stat st;
    struct statfs sf;
    if (fstat(fd, &st) < 0 || fstatfs(fd, &sf) < 0) { reply_reg(c, -1, NULL, strerror(errno)); return; }
    bool isdir = S_ISDIR(st.st_mode);
    if (!isdir && !S_ISREG(st.st_mode)) { reply_reg(c, -1, NULL, "not a folder or regular file"); return; }
    __kernel_fsid_t fsid;
    memcpy(&fsid, &sf.f_fsid, sizeof fsid);

    _Alignas(struct file_handle) unsigned char buf[sizeof(struct file_handle) + HSZ];
    struct file_handle *fh = (struct file_handle *)buf;
    fh->handle_bytes = HSZ;
    int mid;
    if (name_to_handle_at(fd, "", fh, &mid, AT_EMPTY_PATH) < 0) { reply_reg(c, -1, NULL, strerror(errno)); return; }
    char lp[64], path[PATH_MAX];
    snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd);
    ssize_t l = readlink(lp, path, sizeof path - 1);
    if (l < 0) { reply_reg(c, -1, NULL, strerror(errno)); return; }
    path[l] = 0;
    /* A descriptor may predate a permission change: check now, as them. */
    if (!client_can_open(c, path, isdir)) { reply_reg(c, -1, NULL, "no access"); return; }

    /* Mark the filesystem the first time anyone registers on it. */
    struct fsmark *f = fs_of(&fsid);
    if (!f) {
        if (nfs == MAX_FS) { reply_reg(c, -1, NULL, "too many filesystems"); return; }
        bool per_folder = false;
        if (fanotify_mark(fan, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, MASK, fd, NULL) < 0) {
            if (errno != EXDEV && errno != EINVAL) { reply_reg(c, -1, NULL, strerror(errno)); return; }
            per_folder = true;   /* btrfs subvolume and similar */
        }
        f = &fss[nfs++];
        f->fsid = fsid;
        f->per_folder = per_folder;
        /* Remember the mount point (not an fd: that would pin the volume). */
        snprintf(f->mnt, sizeof f->mnt, "%s", path);
        FILE *mi = fopen("/proc/self/mountinfo", "r");
        char line[8192], mp[PATH_MAX];
        size_t best = 0;
        while (mi && fgets(line, sizeof line, mi))
            if (sscanf(line, "%*u %*u %*u:%*u %*s %4095s", mp) == 1) {
                size_t l = strlen(mp);
                if ((l == 1 || (strncmp(path, mp, l) == 0 && (path[l] == '/' || path[l] == 0))) && l >= best) {
                    struct statfs mf;
                    if (statfs(mp, &mf) == 0 && !memcmp(&mf.f_fsid, &fsid, sizeof fsid)) {
                        snprintf(f->mnt, sizeof f->mnt, "%s", mp);
                        best = l;
                    }
                }
            }
        if (mi) fclose(mi);
        printf("wps-watchd: watching filesystem of %s %s\n", path, per_folder ? "per folder" : "as a whole");
    }
    if (f->per_folder &&
        fanotify_mark(fan, FAN_MARK_ADD, MASK | (isdir ? FAN_EVENT_ON_CHILD : 0), fd, NULL) < 0) {
        reply_reg(c, -1, NULL, strerror(errno));
        return;
    }

    if (c->nregs == c->capregs) {
        int ncap = c->capregs ? c->capregs * 2 : 64;
        struct reg **n = realloc(c->regs, (size_t)ncap * sizeof *n);
        if (!n) { reply_reg(c, -1, NULL, "out of memory"); return; }
        c->regs = n;
        c->capregs = ncap;
    }
    struct reg *r = calloc(1, sizeof *r);
    r->c = c;
    r->idx = c->nregs;
    r->isdir = isdir;
    r->ok = true;
    r->gen = access_gen;
    r->fsid = fsid;
    r->htype = fh->handle_type;
    r->hlen = fh->handle_bytes;
    memcpy(r->h, buf + offsetof(struct file_handle, f_handle), r->hlen);
    unsigned k = hkey(&fsid, r->htype, r->h, r->hlen);
    r->hnext = table[k];
    table[k] = r;
    c->regs[c->nregs++] = r;
    reply_reg(c, r->idx, path, NULL);
}

static void client_free(int slot)
{
    struct client *c = clients[slot];
    for (int i = 0; i < c->nregs; i++)
        if (c->regs[i]) { reg_unlink(c->regs[i]); free(c->regs[i]); }
    free(c->regs);
    close(c->fd);
    free(c);
    clients[slot] = NULL;
}

static void client_read(int slot)
{
    struct client *c = clients[slot];
    unsigned char data[1024];
    union { char b[CMSG_SPACE(sizeof(int) * 4)]; struct cmsghdr align; } u;
    struct iovec iov = { data, sizeof data };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b };
    ssize_t n = recvmsg(c->fd, &m, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN)) { client_free(slot); return; }
    if (n < 0) return;
    int fd = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int nf = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            int *fds = (int *)CMSG_DATA(cm);
            fd = fds[0];
            for (int i = 1; i < nf; i++) close(fds[i]);
        }
    cv req;
    const char *op = NULL;
    if (cv_decode(data, (size_t)n, &req) == 0) op = cv_text(&req, "op");
    if (op && !strcmp(op, "reg")) {
        if (fd < 0) reply_reg(c, -1, NULL, "a registration needs a file descriptor");
        else do_register(c, fd);
    } else {
        reply_reg(c, -1, NULL, "unknown request");
    }
    if (op) cv_free(&req);
    if (fd >= 0) close(fd);
}

/* --------------------------------------------------------------- routing */

struct loc { bool present; __kernel_fsid_t fsid; struct file_handle *fh; const unsigned char *hb; const char *name; };

/* This client's valid registration for the handle, or NULL. */
static struct reg *match(struct client *c, const struct loc *l)
{
    if (!l->present) return NULL;
    unsigned k = hkey(&l->fsid, l->fh->handle_type, l->hb, l->fh->handle_bytes);
    for (struct reg *r = table[k]; r; r = r->hnext) {
        if (r->c != c || r->htype != l->fh->handle_type || r->hlen != l->fh->handle_bytes ||
            memcmp(&r->fsid, &l->fsid, sizeof r->fsid) || memcmp(r->h, l->hb, r->hlen)) continue;
        return reg_valid(r) ? r : NULL;
    }
    return NULL;
}

static void put_loc(cbuf *b, const char *key, struct reg *r, const struct loc *l)
{
    cb_text(b, key);
    if (!r) { cb_text(b, "<hidden>"); return; }
    cb_map(b, 2);
    cb_text(b, "reg"); cb_int(b, r->idx);
    cb_text(b, "name"); cb_text(b, l->name ? l->name : "");
}

static void mask_str(unsigned long long m, char *out, size_t n)
{
    static const struct { unsigned long long bit; const char *name; } names[] = {
        { FAN_ATTRIB, "ATTRIB" }, { FAN_CLOSE_WRITE, "CLOSE_WRITE" }, { FAN_MOVED_FROM, "MOVED_FROM" },
        { FAN_MOVED_TO, "MOVED_TO" }, { FAN_CREATE, "CREATE" }, { FAN_DELETE, "DELETE" },
        { FAN_DELETE_SELF, "DELETE_SELF" }, { FAN_MOVE_SELF, "MOVE_SELF" }, { FAN_RENAME, "RENAME" },
        { FAN_ONDIR, "ONDIR" },
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (m & names[i].bit) {
            if (out[0]) strncat(out, "|", n - strlen(out) - 1);
            strncat(out, names[i].name, n - strlen(out) - 1);
        }
}

static void route(struct fanotify_event_metadata *m)
{
    struct loc self = { 0 }, dir = { 0 }, old = { 0 }, nw = { 0 };
    st_events++;
    if (m->mask & FAN_Q_OVERFLOW) {
        for (int s = 0; s < MAX_CLIENTS; s++)
            if (clients[s]) clients[s]->dropped++;   /* everyone rescans */
        return;
    }
    for (size_t off = m->metadata_len; off < m->event_len;) {
        struct fanotify_event_info_header *hdr = (void *)((char *)m + off);
        if (!hdr->len) break;
        off += hdr->len;
        struct fanotify_event_info_fid *fid = (void *)hdr;
        struct file_handle *fh = (struct file_handle *)fid->handle;
        const unsigned char *hb = (const unsigned char *)fh + offsetof(struct file_handle, f_handle);
        struct loc *l = NULL;
        const char *name = (const char *)hb + fh->handle_bytes;
        switch (hdr->info_type) {
        case FAN_EVENT_INFO_TYPE_FID: l = &self; name = NULL; break;
        case FAN_EVENT_INFO_TYPE_DFID: l = &dir; name = NULL; break;
        case FAN_EVENT_INFO_TYPE_DFID_NAME: l = &dir; break;
        case FAN_EVENT_INFO_TYPE_OLD_DFID_NAME: l = &old; break;
        case FAN_EVENT_INFO_TYPE_NEW_DFID_NAME: l = &nw; break;
        default: continue;
        }
        *l = (struct loc){ true, fid->fsid, fh, hb, name };
    }
    /* A folder's permissions or place changed: what each user may see can
     * change, so every cached access check is redone on next use. */
    if ((m->mask & FAN_ONDIR) && (m->mask & (FAN_ATTRIB | FAN_RENAME | FAN_MOVE_SELF | FAN_MOVED_TO)))
        access_gen++;
    char ms[160];
    mask_str(m->mask, ms, sizeof ms);
    if (getenv("WPS_TRACE"))
        printf("wps-watchd: event %s self=%d dir=%d old=%d new=%d\n", ms, self.present, dir.present, old.present, nw.present);
    for (int s = 0; s < MAX_CLIENTS; s++) {
        struct client *c = clients[s];
        if (!c) continue;
        struct reg *rs = match(c, &self), *rd = match(c, &dir), *ro = match(c, &old), *rn = match(c, &nw);
        if (!rs && !rd && !ro && !rn) continue;
        cbuf b;
        cb_init(&b);
        size_t fields = 1 + (rs != NULL) + (rd != NULL) + old.present + nw.present;
        cb_map(&b, fields);
        cb_text(&b, "mask"); cb_text(&b, ms);
        if (rs) { cb_text(&b, "self"); cb_int(&b, rs->idx); }
        if (rd) put_loc(&b, "dir", rd, &dir);
        if (old.present) put_loc(&b, "old", ro, &old);
        if (nw.present) put_loc(&b, "new", rn, &nw);
        send_msg(c, &b);
        cb_free(&b);
        st_forwarded++;
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    const char *sock = "/run/wps-watchd.sock";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) sock = argv[++i];
        else { fprintf(stderr, "usage: wps-watchd [-s SOCKET]\n"); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    root_ngroups = getgroups(MAX_GROUPS, root_groups);
    if (root_ngroups < 0) root_ngroups = 0;
    /* Unlimited marks: per-folder marks are one per registered folder. */
    fan = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME_TARGET | FAN_CLOEXEC | FAN_UNLIMITED_MARKS,
                        O_RDONLY | O_LARGEFILE);
    if (fan < 0) { perror("wps-watchd: fanotify_init (needs root)"); return 1; }

    int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(sock) >= sizeof sa.sun_path) {
        fprintf(stderr, "wps-watchd: socket path too long: %s\n", sock);
        return 1;
    }
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    unlink(sock);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 64) < 0) {
        perror("wps-watchd: socket");
        return 1;
    }
    chmod(sock, 0666);   /* every user's service may connect; the filtering is per user */
    struct sigaction sig = { .sa_handler = on_signal };
    sigaction(SIGINT, &sig, NULL);
    sigaction(SIGTERM, &sig, NULL);
    signal(SIGPIPE, SIG_IGN);
    printf("wps-watchd: ready on %s\n", sock);

    static unsigned char ev[256 * 1024] __attribute__((aligned(8)));
    while (!stop) {
        struct pollfd pf[2 + MAX_CLIENTS];
        int slot[2 + MAX_CLIENTS], n = 0;
        pf[n].fd = fan; pf[n].events = POLLIN; slot[n++] = -1;
        pf[n].fd = ls; pf[n].events = POLLIN; slot[n++] = -2;
        for (int s = 0; s < MAX_CLIENTS; s++)
            if (clients[s]) {
                pf[n].fd = clients[s]->fd;
                pf[n].events = POLLIN | (clients[s]->dropped ? POLLOUT : 0);
                slot[n++] = s;
            }
        if (poll(pf, (nfds_t)n, -1) < 0) { if (errno == EINTR) continue; break; }
        if (pf[0].revents & POLLIN) {
            ssize_t len = read(fan, ev, sizeof ev);
            /* Records are only 4-byte aligned in the stream; copy each into
             * an aligned buffer before reading its 8-byte fields. */
            static _Alignas(8) unsigned char one[64 * 1024];
            for (size_t o = 0; len > 0 && o + FAN_EVENT_METADATA_LEN <= (size_t)len;) {
                uint32_t el;
                memcpy(&el, ev + o + offsetof(struct fanotify_event_metadata, event_len), sizeof el);
                if (el < FAN_EVENT_METADATA_LEN || o + el > (size_t)len || el > sizeof one) break;
                memcpy(one, ev + o, el);
                struct fanotify_event_metadata *m = (struct fanotify_event_metadata *)one;
                route(m);
                if (m->fd >= 0) close(m->fd);
                o += el;
            }
        }
        if (pf[1].revents & POLLIN) {
            int cfd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
            int s;
            for (s = 0; s < MAX_CLIENTS && clients[s]; s++) {}
            if (cfd >= 0 && s < MAX_CLIENTS) {
                struct client *c = calloc(1, sizeof *c);
                c->fd = cfd;
                int sz = 4 << 20;
                setsockopt(cfd, SOL_SOCKET, SO_SNDBUFFORCE, &sz, sizeof sz);
                read_peer(c);
                clients[s] = c;
            } else if (cfd >= 0) {
                close(cfd);
            }
        }
        for (int i = 2; i < n; i++) {
            struct client *c = clients[slot[i]];
            if (!c) continue;
            if ((pf[i].revents & POLLOUT) && c->dropped) {
                cbuf b;
                cb_init(&b);
                send_msg(c, &b);   /* sends only the pending overflow notice */
                cb_free(&b);
            }
            if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) client_read(slot[i]);
        }
    }
    printf("wps-watchd: stopped (events %lu, forwarded %lu, access checks %lu)\n", st_events, st_forwarded, st_checks);
    unlink(sock);
    return 0;
}
