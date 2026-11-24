/*
 * watchd - prototype of osd-watchd's per-user event filtering.
 *
 * Watches whole filesystems with fanotify (as fanwatch does) and serves
 * clients over a Unix socket. A client registers a folder or file by
 * sending an OPEN FILE DESCRIPTOR for it (SCM_RIGHTS): the kernel already
 * checked that the client could open it. The helper then forwards only
 * events about registered folders (and their direct entries) or registered
 * files. Before forwarding, it re-checks access under the client's own
 * credentials whenever any folder's permissions or location may have
 * changed, so revoking access stops the events.
 *
 * Renames that cross from a visible folder into an invisible one are
 * forwarded with the invisible side hidden.
 *
 * Protocol (lines):
 *   client -> helper  "REG\n" with one fd attached
 *   helper -> client  {"reg":N,"ok":true,"kind":"dir"|"file","path":...}
 *                     {"reg":-1,"ok":false,"error":...}
 *                     {"revoked":N}
 *                     {"overflow":N}   N lines were dropped; rescan
 *                     {"mask":...,"dir":{"reg":N,"name":...},"old":...,"new":...,"self":N}
 *
 * usage: watchd -s SOCKET MOUNTPOINT...    (run as root)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
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

#define MAX_MNT 8
#define MAX_CLIENTS 64
#define MAX_REGS 256
#define MAX_GROUPS 64
#define HSZ 128

struct mnt { const char *path; int fd; __kernel_fsid_t fsid; };

struct reg {
    int used, isdir, ok;
    unsigned long gen;              /* access checked at this generation */
    __kernel_fsid_t fsid;
    int htype; unsigned hlen; unsigned char h[HSZ];
    char path[PATH_MAX];            /* at registration; for the client only */
};

struct client {
    int fd;
    uid_t uid; gid_t gid;
    gid_t groups[MAX_GROUPS]; int ngroups;
    struct reg regs[MAX_REGS];
    unsigned long forwarded;
    unsigned long dropped;      /* lines lost since the last overflow notice */
    unsigned long dropped_total;
};

static struct mnt mnts[MAX_MNT];
static int nmnt;
static struct client *clients[MAX_CLIENTS];
static unsigned long access_gen = 1;  /* bumped when folder permissions/locations change */
static gid_t root_groups[MAX_GROUPS];
static int root_ngroups;
static volatile sig_atomic_t stop;
static unsigned long stat_events, stat_forwarded, stat_checks;

static void on_signal(int s) { (void)s; stop = 1; }

/* ---------- output helpers */

struct buf { char d[8192]; size_t n; };

static void bput(struct buf *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l < sizeof b->d) { memcpy(b->d + b->n, s, l); b->n += l; }
}

static void bjstr(struct buf *b, const char *s)
{
    char tmp[8];
    bput(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = (char)c; tmp[2] = 0; bput(b, tmp); }
        else if (c < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", c); bput(b, tmp); }
        else { tmp[0] = (char)c; tmp[1] = 0; bput(b, tmp); }
    }
    bput(b, "\"");
}

/*
 * Never block on a slow client. If its socket is full, the line is dropped
 * and counted; as soon as the client has room again it first receives
 * {"overflow":N}, which tells its service to rescan what it registered.
 */
static void send_line(struct client *c, struct buf *b)
{
    bput(b, "\n");
    if (c->dropped) {
        char note[64];
        int n = snprintf(note, sizeof note, "{\"overflow\":%lu}\n", c->dropped);
        if (send(c->fd, note, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
            c->dropped++;
            c->dropped_total++;
            return;
        }
        c->dropped = 0;
    }
    if (send(c->fd, b->d, b->n, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
        c->dropped++;
        c->dropped_total++;
    }
}

/* ---------- credentials and access checks */

static int read_peer(struct client *c)
{
    struct ucred cr;
    socklen_t len = sizeof cr;
    if (getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) < 0) return -1;
    c->uid = cr.uid;
    c->gid = cr.gid;
    /* Supplementary groups from /proc. A real helper would use
     * SO_PEERGROUPS (Linux 4.13+); this is a prototype. */
    char p[64], line[1024];
    snprintf(p, sizeof p, "/proc/%d/status", cr.pid);
    FILE *f = fopen(p, "r");
    c->ngroups = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "Groups:", 7) == 0) {
            char *s = line + 7, *end;
            for (;;) {
                long g = strtol(s, &end, 10);
                if (end == s || c->ngroups >= MAX_GROUPS) break;
                c->groups[c->ngroups++] = (gid_t)g;
                s = end;
            }
        }
    fclose(f);
    return 0;
}

/*
 * Can the client open `path`? The helper is single-threaded, so switching
 * this thread's filesystem credentials is safe. Raw syscalls change only the
 * calling thread (glibc's wrappers would broadcast to all threads).
 * Switching fsuid away from 0 also drops the DAC override capabilities, so
 * the kernel checks permissions exactly as for the client.
 */
static int client_can_open(struct client *c, const char *path, int isdir)
{
    stat_checks++;
    syscall(SYS_setgroups, c->ngroups, c->groups);
    syscall(SYS_setfsgid, c->gid);
    syscall(SYS_setfsuid, c->uid);
    int fd = open(path, isdir ? (O_RDONLY | O_DIRECTORY) : O_PATH);
    int ok = fd >= 0;
    if (fd >= 0) close(fd);
    syscall(SYS_setfsuid, 0);
    syscall(SYS_setfsgid, 0);
    syscall(SYS_setgroups, root_ngroups, root_groups);
    return ok;
}

static struct mnt *find_mnt(const __kernel_fsid_t *fsid)
{
    for (int i = 0; i < nmnt; i++)
        if (memcmp(&mnts[i].fsid, fsid, sizeof *fsid) == 0) return &mnts[i];
    return NULL;
}

/* Current path of a registration, through its handle. */
static int reg_path(struct reg *r, char *path, size_t n)
{
    struct mnt *m = find_mnt(&r->fsid);
    if (!m) return -1;
    struct { struct file_handle fh; unsigned char b[HSZ]; } h;
    h.fh.handle_type = r->htype;
    h.fh.handle_bytes = r->hlen;
    memcpy(h.fh.f_handle, r->h, r->hlen);
    int fd = open_by_handle_at(m->fd, &h.fh, O_PATH);
    if (fd < 0) return -1;
    char lp[64];
    snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd);
    ssize_t l = readlink(lp, path, n - 1);
    close(fd);
    if (l < 0) return -1;
    path[l] = 0;
    return 0;
}

/* Is this registration still accessible to its client? Cached per generation. */
static int reg_valid(struct client *c, int idx)
{
    struct reg *r = &c->regs[idx];
    if (!r->used) return 0;
    if (r->gen == access_gen) return r->ok;
    char path[PATH_MAX];
    r->ok = reg_path(r, path, sizeof path) == 0 && client_can_open(c, path, r->isdir);
    r->gen = access_gen;
    if (!r->ok) {
        struct buf b = { .n = 0 };
        char tmp[64];
        snprintf(tmp, sizeof tmp, "{\"revoked\":%d}", idx);
        bput(&b, tmp);
        send_line(c, &b);
        r->used = 0;
    }
    return r->ok;
}

/* ---------- registration */

static void reply_err(struct client *c, const char *why)
{
    struct buf b = { .n = 0 };
    bput(&b, "{\"reg\":-1,\"ok\":false,\"error\":");
    bjstr(&b, why);
    bput(&b, "}");
    send_line(c, &b);
}

static void do_register(struct client *c, int fd)
{
    struct stat st;
    struct statfs sf;
    if (fstat(fd, &st) < 0 || fstatfs(fd, &sf) < 0) { reply_err(c, strerror(errno)); return; }
    int isdir = S_ISDIR(st.st_mode);
    if (!isdir && !S_ISREG(st.st_mode)) { reply_err(c, "not a folder or regular file"); return; }

    __kernel_fsid_t fsid;
    memcpy(&fsid, &sf.f_fsid, sizeof fsid);
    if (!find_mnt(&fsid)) { reply_err(c, "filesystem not watched"); return; }

    struct { struct file_handle fh; unsigned char b[HSZ]; } h;
    int mid;
    h.fh.handle_bytes = HSZ;
    if (name_to_handle_at(fd, "", &h.fh, &mid, AT_EMPTY_PATH) < 0) { reply_err(c, strerror(errno)); return; }

    char lp[64], path[PATH_MAX];
    snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd);
    ssize_t l = readlink(lp, path, sizeof path - 1);
    if (l < 0) { reply_err(c, strerror(errno)); return; }
    path[l] = 0;

    /* Holding an fd is not enough: access may have been revoked since the
     * client opened it. Check now, as the client. */
    if (!client_can_open(c, path, isdir)) { reply_err(c, "no access"); return; }

    int idx = -1;
    for (int i = 0; i < MAX_REGS; i++) if (!c->regs[i].used) { idx = i; break; }
    if (idx < 0) { reply_err(c, "too many registrations"); return; }
    struct reg *r = &c->regs[idx];
    memset(r, 0, sizeof *r);
    r->used = 1; r->isdir = isdir; r->ok = 1; r->gen = access_gen;
    r->fsid = fsid; r->htype = h.fh.handle_type; r->hlen = h.fh.handle_bytes;
    memcpy(r->h, h.fh.f_handle, r->hlen);
    snprintf(r->path, sizeof r->path, "%s", path);

    struct buf b = { .n = 0 };
    char tmp[64];
    snprintf(tmp, sizeof tmp, "{\"reg\":%d,\"ok\":true,\"kind\":\"%s\",\"path\":", idx, isdir ? "dir" : "file");
    bput(&b, tmp);
    bjstr(&b, path);
    bput(&b, "}");
    send_line(c, &b);
}

static void client_read(int slot)
{
    struct client *c = clients[slot];
    char data[256];
    union { char b[CMSG_SPACE(sizeof(int) * 4)]; struct cmsghdr align; } cbuf;
    struct iovec iov = { data, sizeof data };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = cbuf.b, .msg_controllen = sizeof cbuf.b };
    ssize_t n = recvmsg(c->fd, &msg, MSG_CMSG_CLOEXEC);
    if (n <= 0) {
        close(c->fd);
        free(c);
        clients[slot] = NULL;
        return;
    }
    int fd = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int nfds = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            int *fds = (int *)CMSG_DATA(cm);
            fd = fds[0];
            for (int i = 1; i < nfds; i++) close(fds[i]);
        }
    if (n >= 3 && memcmp(data, "REG", 3) == 0) {
        if (fd < 0) reply_err(c, "REG needs an attached file descriptor");
        else do_register(c, fd);
    } else {
        reply_err(c, "unknown request");
    }
    if (fd >= 0) close(fd);
}

/* ---------- event routing */

struct loc { int present; __kernel_fsid_t fsid; struct file_handle *fh; const char *name; };

static int handle_eq(const struct reg *r, const __kernel_fsid_t *fsid, const struct file_handle *fh)
{
    return r->used && memcmp(&r->fsid, fsid, sizeof *fsid) == 0 && r->htype == fh->handle_type &&
           r->hlen == fh->handle_bytes && memcmp(r->h, fh->f_handle, r->hlen) == 0;
}

/* Index of a valid registration of `c` matching the handle, or -1. */
static int match(struct client *c, const struct loc *l)
{
    if (!l->present) return -1;
    for (int i = 0; i < MAX_REGS; i++)
        if (handle_eq(&c->regs[i], &l->fsid, l->fh) && reg_valid(c, i)) return i;
    return -1;
}

static const char *mask_str(unsigned long long m, char *out, size_t n)
{
    static const struct { unsigned long long bit; const char *name; } names[] = {
        { FAN_ATTRIB, "ATTRIB" }, { FAN_CLOSE_WRITE, "CLOSE_WRITE" },
        { FAN_MOVED_FROM, "MOVED_FROM" }, { FAN_MOVED_TO, "MOVED_TO" },
        { FAN_CREATE, "CREATE" }, { FAN_DELETE, "DELETE" },
        { FAN_DELETE_SELF, "DELETE_SELF" }, { FAN_MOVE_SELF, "MOVE_SELF" },
        { FAN_RENAME, "RENAME" }, { FAN_Q_OVERFLOW, "Q_OVERFLOW" }, { FAN_ONDIR, "ONDIR" },
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (m & names[i].bit) {
            if (out[0]) strncat(out, "|", n - strlen(out) - 1);
            strncat(out, names[i].name, n - strlen(out) - 1);
        }
    return out;
}

static void put_loc(struct buf *b, const char *key, int reg, const struct loc *l)
{
    char tmp[64];
    if (!l->present) return;
    snprintf(tmp, sizeof tmp, ",\"%s\":", key);
    bput(b, tmp);
    if (reg < 0) { bput(b, "\"<hidden>\""); return; }
    snprintf(tmp, sizeof tmp, "{\"reg\":%d,\"name\":", reg);
    bput(b, tmp);
    bjstr(b, l->name ? l->name : "");
    bput(b, "}");
}

static void route(struct fanotify_event_metadata *m)
{
    struct loc self = { 0 }, dir = { 0 }, old = { 0 }, new = { 0 };
    stat_events++;

    for (size_t off = m->metadata_len; off < m->event_len;) {
        struct fanotify_event_info_header *hdr = (void *)((char *)m + off);
        if (!hdr->len) break;
        off += hdr->len;
        struct fanotify_event_info_fid *fid = (void *)hdr;
        struct file_handle *fh = (struct file_handle *)fid->handle;
        const char *name = (const char *)fh->f_handle + fh->handle_bytes;
        struct loc *l = NULL;
        switch (hdr->info_type) {
        case FAN_EVENT_INFO_TYPE_FID: l = &self; name = NULL; break;
        case FAN_EVENT_INFO_TYPE_DFID: l = &dir; name = NULL; break;
        case FAN_EVENT_INFO_TYPE_DFID_NAME: l = &dir; break;
        case FAN_EVENT_INFO_TYPE_OLD_DFID_NAME: l = &old; break;
        case FAN_EVENT_INFO_TYPE_NEW_DFID_NAME: l = &new; break;
        default: continue;
        }
        l->present = 1; l->fsid = fid->fsid; l->fh = fh; l->name = name;
    }

    /* A folder whose permissions or location changed may change what every
     * client may see: invalidate all cached access checks. */
    if ((m->mask & FAN_ONDIR) && (m->mask & (FAN_ATTRIB | FAN_RENAME | FAN_MOVE_SELF | FAN_MOVED_TO)))
        access_gen++;

    char ms[160];
    mask_str(m->mask, ms, sizeof ms);
    for (int s = 0; s < MAX_CLIENTS; s++) {
        struct client *c = clients[s];
        if (!c) continue;
        int rs = match(c, &self), rd = match(c, &dir), ro = match(c, &old), rn = match(c, &new);
        if (rs < 0 && rd < 0 && ro < 0 && rn < 0) continue;
        struct buf b = { .n = 0 };
        bput(&b, "{\"mask\":");
        bjstr(&b, ms);
        if (rs >= 0) { char t[32]; snprintf(t, sizeof t, ",\"self\":%d", rs); bput(&b, t); }
        if (rd >= 0) put_loc(&b, "dir", rd, &dir);
        put_loc(&b, "old", ro, &old);
        put_loc(&b, "new", rn, &new);
        bput(&b, "}");
        send_line(c, &b);
        c->forwarded++;
        stat_forwarded++;
    }
}

/* ---------- main */

int main(int argc, char **argv)
{
    const char *sockpath = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "s:")) != -1)
        if (opt == 's') sockpath = optarg;
    if (!sockpath || optind >= argc) {
        fprintf(stderr, "usage: watchd -s SOCKET MOUNTPOINT...\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    root_ngroups = getgroups(MAX_GROUPS, root_groups);
    if (root_ngroups < 0) root_ngroups = 0;

    int fan = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME_TARGET, O_RDONLY | O_LARGEFILE);
    if (fan < 0) { perror("fanotify_init"); return 1; }
    unsigned long long mask = FAN_CREATE | FAN_DELETE | FAN_DELETE_SELF | FAN_MOVED_FROM |
        FAN_MOVED_TO | FAN_RENAME | FAN_MOVE_SELF | FAN_ATTRIB | FAN_CLOSE_WRITE | FAN_ONDIR;
    for (int i = optind; i < argc && nmnt < MAX_MNT; i++) {
        struct mnt *m = &mnts[nmnt];
        struct statfs sf;
        m->path = argv[i];
        m->fd = open(m->path, O_RDONLY | O_DIRECTORY);
        if (m->fd < 0 || fstatfs(m->fd, &sf) < 0 ||
            fanotify_mark(fan, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask, AT_FDCWD, m->path) < 0) {
            fprintf(stderr, "watchd: %s: %s\n", m->path, strerror(errno));
            continue;
        }
        memcpy(&m->fsid, &sf.f_fsid, sizeof m->fsid);
        nmnt++;
    }

    int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sockpath);
    unlink(sockpath);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 16) < 0) {
        perror("socket");
        return 1;
    }
    chmod(sockpath, 0666);

    struct sigaction sig = { .sa_handler = on_signal };
    sigaction(SIGINT, &sig, NULL);
    sigaction(SIGTERM, &sig, NULL);
    printf("ready %d filesystems\n", nmnt);

    static char evbuf[256 * 1024] __attribute__((aligned(8)));
    while (!stop) {
        struct pollfd pf[2 + MAX_CLIENTS];
        int slot_of[2 + MAX_CLIENTS], n = 0;
        pf[n].fd = fan; pf[n].events = POLLIN; slot_of[n++] = -1;
        pf[n].fd = ls; pf[n].events = POLLIN; slot_of[n++] = -2;
        for (int s = 0; s < MAX_CLIENTS; s++)
            if (clients[s]) {
                pf[n].fd = clients[s]->fd;
                /* A client that lost lines is told as soon as it has room,
                 * even if no further events arrive. */
                pf[n].events = POLLIN | (clients[s]->dropped ? POLLOUT : 0);
                slot_of[n++] = s;
            }
        if (poll(pf, (nfds_t)n, -1) < 0) { if (errno == EINTR) continue; break; }

        if (pf[0].revents & POLLIN) {
            ssize_t len = read(fan, evbuf, sizeof evbuf);
            struct fanotify_event_metadata *m = (void *)evbuf;
            for (; len > 0 && FAN_EVENT_OK(m, len); m = FAN_EVENT_NEXT(m, len)) route(m);
        }
        if (pf[1].revents & POLLIN) {
            int cfd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
            int slot = -1;
            for (int s = 0; s < MAX_CLIENTS; s++) if (!clients[s]) { slot = s; break; }
            if (cfd >= 0 && slot >= 0) {
                struct client *c = calloc(1, sizeof *c);
                c->fd = cfd;
                int sz = 4 << 20;  /* room for bursts; SO_SNDBUFFORCE needs root */
                setsockopt(cfd, SOL_SOCKET, SO_SNDBUFFORCE, &sz, sizeof sz);
                if (read_peer(c) < 0) { close(cfd); free(c); }
                else clients[slot] = c;
            } else if (cfd >= 0) close(cfd);
        }
        for (int i = 2; i < n; i++) {
            struct client *c = clients[slot_of[i]];
            if (c && (pf[i].revents & POLLOUT) && c->dropped) {
                char note[64];
                int len = snprintf(note, sizeof note, "{\"overflow\":%lu}\n", c->dropped);
                if (send(c->fd, note, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) c->dropped = 0;
            }
            if (c && pf[i].revents & (POLLIN | POLLHUP)) client_read(slot_of[i]);
        }
    }
    unsigned long dropped = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i]) dropped += clients[i]->dropped_total;
    printf("stats events=%lu forwarded=%lu access_checks=%lu dropped(open clients)=%lu\n",
           stat_events, stat_forwarded, stat_checks, dropped);
    unlink(sockpath);
    return 0;
}
