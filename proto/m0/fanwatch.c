/*
 * fanwatch - M0 tracking prototype.
 *
 * Watches whole filesystems with fanotify in FID mode and prints one JSON
 * line per event: event types, pid, the child file handle, the directory
 * handle and name, and for renames the old and new locations. Handles are
 * resolved to paths with open_by_handle_at(), and the child's user.osd.id
 * attribute is read when the file still exists.
 *
 * Needs CAP_SYS_ADMIN (filesystem marks) and CAP_DAC_READ_SEARCH
 * (open_by_handle_at). Run as root.
 *
 * usage: fanwatch -o out.jsonl MOUNTPOINT...
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define MAX_MNT 8

struct mnt {
    const char *path;
    int fd;
    fsid_t fsid;
    int marked;
};

static struct mnt mnts[MAX_MNT];
static int nmnt;
static FILE *out;
static volatile sig_atomic_t stop;

static void on_signal(int sig) { (void)sig; stop = 1; }

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* JSON string with escaping. */
static void jstr(const char *s)
{
    fputc('"', out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

static struct mnt *find_mnt(const __kernel_fsid_t *fsid)
{
    for (int i = 0; i < nmnt; i++)
        if (memcmp(&mnts[i].fsid, fsid, sizeof(*fsid)) == 0)
            return &mnts[i];
    return NULL;
}

static void handle_hex(const struct file_handle *h, char *buf, size_t n)
{
    size_t off = (size_t)snprintf(buf, n, "%x:", h->handle_type);
    for (unsigned i = 0; i < h->handle_bytes && off + 3 < n; i++)
        off += (size_t)snprintf(buf + off, n - off, "%02x", h->f_handle[i]);
}

/*
 * Print {"h":..., "path":..., "id":...} for a handle. "path" is the current
 * path, or an error string in angle brackets when the inode is gone.
 */
static void print_handle(const __kernel_fsid_t *fsid, struct file_handle *h,
                         int want_id)
{
    char hex[300], path[PATH_MAX], lp[64];
    handle_hex(h, hex, sizeof hex);
    fprintf(out, "{\"h\":");
    jstr(hex);

    struct mnt *m = find_mnt(fsid);
    if (!m) {
        fprintf(out, ",\"path\":\"<unknown fs>\"}");
        return;
    }
    int fd = open_by_handle_at(m->fd, h, O_PATH);
    if (fd < 0) {
        snprintf(path, sizeof path, "<%s>", strerror(errno));
        fprintf(out, ",\"path\":");
        jstr(path);
        fputc('}', out);
        return;
    }
    snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd);
    ssize_t r = readlink(lp, path, sizeof path - 1);
    path[r < 0 ? 0 : r] = 0;
    fprintf(out, ",\"path\":");
    jstr(path);
    if (want_id) {
        char id[256];
        ssize_t n = lgetxattr(path, "user.osd.id", id, sizeof id - 1);
        if (n >= 0) {
            id[n] = 0;
            fprintf(out, ",\"id\":");
            jstr(id);
        }
    }
    close(fd);
    fputc('}', out);
}

static void print_mask(unsigned long long mask)
{
    static const struct { unsigned long long bit; const char *name; } names[] = {
        { FAN_ACCESS, "ACCESS" }, { FAN_MODIFY, "MODIFY" },
        { FAN_ATTRIB, "ATTRIB" }, { FAN_CLOSE_WRITE, "CLOSE_WRITE" },
        { FAN_CLOSE_NOWRITE, "CLOSE_NOWRITE" }, { FAN_OPEN, "OPEN" },
        { FAN_MOVED_FROM, "MOVED_FROM" }, { FAN_MOVED_TO, "MOVED_TO" },
        { FAN_CREATE, "CREATE" }, { FAN_DELETE, "DELETE" },
        { FAN_DELETE_SELF, "DELETE_SELF" }, { FAN_MOVE_SELF, "MOVE_SELF" },
        { FAN_RENAME, "RENAME" }, { FAN_Q_OVERFLOW, "Q_OVERFLOW" },
        { FAN_ONDIR, "ONDIR" },
    };
    int first = 1;
    fputc('"', out);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (mask & names[i].bit) {
            fprintf(out, "%s%s", first ? "" : "|", names[i].name);
            first = 0;
        }
    fputc('"', out);
}

static void print_event(struct fanotify_event_metadata *m)
{
    fprintf(out, "{\"t\":%lld,\"mask\":", now_ms());
    print_mask(m->mask);
    fprintf(out, ",\"pid\":%d", m->pid);

    for (size_t off = m->metadata_len; off < m->event_len;) {
        struct fanotify_event_info_header *hdr =
            (struct fanotify_event_info_header *)((char *)m + off);
        if (hdr->len == 0) break;
        off += hdr->len;

        const char *key = NULL;
        int named = 0;
        switch (hdr->info_type) {
        case FAN_EVENT_INFO_TYPE_FID:          key = "fid"; break;
        case FAN_EVENT_INFO_TYPE_DFID:         key = "dir"; break;
        case FAN_EVENT_INFO_TYPE_DFID_NAME:    key = "dir"; named = 1; break;
        case FAN_EVENT_INFO_TYPE_OLD_DFID_NAME: key = "old"; named = 1; break;
        case FAN_EVENT_INFO_TYPE_NEW_DFID_NAME: key = "new"; named = 1; break;
        default: continue;
        }
        struct fanotify_event_info_fid *fid = (struct fanotify_event_info_fid *)hdr;
        struct file_handle *fh = (struct file_handle *)fid->handle;
        fprintf(out, ",\"%s\":", key);
        if (named) {
            const char *name = (const char *)fh->f_handle + fh->handle_bytes;
            fprintf(out, "{\"name\":");
            jstr(name);
            fprintf(out, ",\"d\":");
            print_handle(&fid->fsid, fh, 0);
            fputc('}', out);
        } else {
            print_handle(&fid->fsid, fh, strcmp(key, "fid") == 0);
        }
    }
    fprintf(out, "}\n");
}

int main(int argc, char **argv)
{
    const char *outpath = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "o:")) != -1) {
        if (opt == 'o') outpath = optarg;
        else {
            fprintf(stderr, "usage: fanwatch -o out.jsonl MOUNTPOINT...\n");
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: fanwatch -o out.jsonl MOUNTPOINT...\n");
        return 2;
    }
    out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) { perror(outpath); return 1; }
    setvbuf(out, NULL, _IOLBF, 0);

    int fan = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME_TARGET,
                            O_RDONLY | O_LARGEFILE);
    if (fan < 0) { perror("fanotify_init"); return 1; }

    unsigned long long mask = FAN_CREATE | FAN_DELETE | FAN_DELETE_SELF |
        FAN_MOVED_FROM | FAN_MOVED_TO | FAN_RENAME | FAN_MOVE_SELF |
        FAN_ATTRIB | FAN_CLOSE_WRITE | FAN_ONDIR;

    for (int i = optind; i < argc && nmnt < MAX_MNT; i++) {
        struct mnt *m = &mnts[nmnt++];
        struct statfs sf;
        m->path = argv[i];
        m->fd = open(m->path, O_RDONLY | O_DIRECTORY);
        if (m->fd < 0 || fstatfs(m->fd, &sf) < 0) {
            fprintf(out, "{\"mark\":");
            jstr(m->path);
            fprintf(out, ",\"error\":");
            jstr(strerror(errno));
            fprintf(out, "}\n");
            continue;
        }
        m->fsid = sf.f_fsid;
        if (fanotify_mark(fan, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask,
                          AT_FDCWD, m->path) < 0) {
            fprintf(out, "{\"mark\":");
            jstr(m->path);
            fprintf(out, ",\"error\":");
            jstr(strerror(errno));
            fprintf(out, "}\n");
            continue;
        }
        m->marked = 1;
        fprintf(out, "{\"mark\":");
        jstr(m->path);
        fprintf(out, ",\"ok\":true}\n");
    }
    fprintf(out, "{\"ready\":true,\"t\":%lld}\n", now_ms());

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    static char buf[256 * 1024] __attribute__((aligned(8)));
    while (!stop) {
        ssize_t len = read(fan, buf, sizeof buf);
        if (len < 0) {
            if (errno == EINTR) continue;
            perror("read");
            break;
        }
        struct fanotify_event_metadata *m = (struct fanotify_event_metadata *)buf;
        for (; FAN_EVENT_OK(m, len); m = FAN_EVENT_NEXT(m, len)) {
            if (m->vers != FANOTIFY_METADATA_VERSION) {
                fprintf(stderr, "fanotify metadata version mismatch\n");
                return 1;
            }
            print_event(m);
        }
    }
    fprintf(out, "{\"stopped\":true,\"t\":%lld}\n", now_ms());
    return 0;
}
