/* The trash, as the freedesktop.org Trash specification has it (trash.h). */
#define _GNU_SOURCE
#include "trash.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "shadows.h"   /* wps_in_trash */

/* The top of the filesystem `path` is on: the highest folder on the same
 * device. */
static bool topdir_of(const char *path, dev_t dev, char *out, size_t n)
{
    snprintf(out, n, "%s", path);
    for (;;) {
        char *s = strrchr(out, '/');
        if (!s) return false;
        char parent[PATH_MAX];
        if (s == out) snprintf(parent, sizeof parent, "/");
        else snprintf(parent, sizeof parent, "%.*s", (int)(s - out), out);
        struct stat st;
        if (stat(parent, &st) != 0 || st.st_dev != dev) return true;   /* out is the top */
        if (s == out) { snprintf(out, n, "/"); return true; }
        *s = 0;
    }
}

static int mkdirs(const char *p, mode_t mode)
{
    char b[PATH_MAX];
    snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(b, mode) != 0 && errno != EEXIST) return -1;
            *s = '/';
        }
    return mkdir(b, mode) != 0 && errno != EEXIST ? -1 : 0;
}

/* A trash folder with files/ and info/ in it, made if need be. */
static bool ready(const char *trash)
{
    char f[PATH_MAX + 8], i[PATH_MAX + 8];
    snprintf(f, sizeof f, "%s/files", trash);
    snprintf(i, sizeof i, "%s/info", trash);
    if (mkdirs(f, 0700) != 0 || mkdirs(i, 0700) != 0) return false;
    return access(f, W_OK) == 0 && access(i, W_OK) == 0;
}

/* The trash for an item on device `dev`: into `trash`, and the folder its
 * info's Path is relative to (empty: absolute). */
static bool trash_for(const char *path, dev_t dev, char *trash, size_t n, char *rel_to, size_t rn)
{
    rel_to[0] = 0;
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char hd[PATH_MAX];
    if (dh && *dh) snprintf(hd, sizeof hd, "%s", dh);
    else if (home && *home) snprintf(hd, sizeof hd, "%s/.local/share", home);
    else return false;
    struct stat hs;
    if (mkdirs(hd, 0700) == 0 && stat(hd, &hs) == 0 && hs.st_dev == dev) {
        snprintf(trash, n, "%s/Trash", hd);
        return ready(trash);
    }
    char top[PATH_MAX];
    if (!topdir_of(path, dev, top, sizeof top)) return false;
    const char *sep = strcmp(top, "/") ? "/" : "";
    char admin[PATH_MAX + 16];
    snprintf(admin, sizeof admin, "%s%s.Trash", top, sep);
    struct stat as;
    if (lstat(admin, &as) == 0 && S_ISDIR(as.st_mode) && (as.st_mode & S_ISVTX)) {
        snprintf(trash, n, "%s/%u", admin, (unsigned)getuid());
        if (ready(trash)) { snprintf(rel_to, rn, "%s", top); return true; }
    }
    snprintf(trash, n, "%s%s.Trash-%u", top, sep, (unsigned)getuid());
    struct stat ts;
    if (lstat(trash, &ts) == 0 && (!S_ISDIR(ts.st_mode) || ts.st_uid != getuid())) return false;   /* not ours */
    if (!ready(trash)) return false;
    snprintf(rel_to, rn, "%s", top);
    return true;
}

/* Percent-encode a path for the info file (keeping '/'). */
static void encode(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p))
            out[o++] = (char)*p;
        else
            o += (size_t)snprintf(out + o, n - o, "%%%02X", *p);
    }
    out[o] = 0;
}

int wps_trash(const char *path, char *out, size_t n)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -errno;
    if (wps_in_trash(path)) return -EXDEV;   /* deleting from the trash is for good */
    char trash[PATH_MAX], rel_to[PATH_MAX];
    if (!trash_for(path, st.st_dev, trash, sizeof trash, rel_to, sizeof rel_to)) return -EXDEV;
    struct stat tst;
    char files[PATH_MAX + 8];
    snprintf(files, sizeof files, "%s/files", trash);
    if (stat(files, &tst) != 0 || tst.st_dev != st.st_dev) return -EXDEV;
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    /* Path: absolute in the home trash, else relative to the top. */
    const char *p = path;
    size_t tl = strlen(rel_to);
    if (rel_to[0] && strcmp(rel_to, "/") && !strncmp(path, rel_to, tl) && path[tl] == '/') p = path + tl + 1;
    else if (rel_to[0] && !strcmp(rel_to, "/")) p = path + 1;
    char enc[PATH_MAX * 3], when[32], info[PATH_MAX * 2], dst[PATH_MAX * 2], name[NAME_MAX + 16];
    encode(p, enc, sizeof enc);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
    for (int k = 1; k < 10000; k++) {
        /* name, then "name.2", "name.3" ... (as GLib does) */
        if (k == 1) snprintf(name, sizeof name, "%s", base);
        else snprintf(name, sizeof name, "%.*s.%d", NAME_MAX - 8, base, k);
        snprintf(info, sizeof info, "%s/info/%s.trashinfo", trash, name);
        int fd = open(info, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return -EXDEV;
        }
        char body[PATH_MAX * 3 + 128];
        int bl = snprintf(body, sizeof body, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, when);
        bool ok = write(fd, body, (size_t)bl) == bl;
        close(fd);
        if (!ok) { unlink(info); return -EIO; }
        snprintf(dst, sizeof dst, "%s/files/%s", trash, name);
        if (renameat2(AT_FDCWD, path, AT_FDCWD, dst, RENAME_NOREPLACE) == 0) {
            snprintf(out, n, "%s", dst);
            return 0;
        }
        int e = errno;
        unlink(info);
        if (e == EEXIST) continue;   /* a leftover without its info file */
        return e == ENOENT ? -ENOENT : e == EXDEV ? -EXDEV : -e;
    }
    return -EEXIST;
}

void wps_trash_forget_info(const char *path)
{
    /* .../files/NAME -> .../info/NAME.trashinfo, for a direct entry only */
    const char *s = strrchr(path, '/');
    if (!s || s - path < 6 || strncmp(s - 6, "/files", 6)) return;
    char info[PATH_MAX * 2];
    snprintf(info, sizeof info, "%.*s/info%s.trashinfo", (int)(s - 6 - path), path, s);
    unlink(info);
}
