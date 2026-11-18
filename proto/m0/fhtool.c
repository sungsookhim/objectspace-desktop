/*
 * fhtool - small helpers for the M0 scenarios.
 *
 *   fhtool handle PATH               print "type:hex inode" for PATH (no follow)
 *   fhtool open MOUNTDIR HANDLE      open by handle; print the path or the error
 *   fhtool rename-noreplace OLD NEW  renameat2(RENAME_NOREPLACE); print ok or error
 *
 * Exit status: 0 on success, 1 on a reported failure, 2 on bad usage.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int cmd_handle(const char *path)
{
    struct { struct file_handle fh; unsigned char buf[MAX_HANDLE_SZ]; } h;
    int mount_id;
    struct stat st;
    h.fh.handle_bytes = MAX_HANDLE_SZ;
    if (name_to_handle_at(AT_FDCWD, path, &h.fh, &mount_id, 0) < 0) {
        printf("error %s\n", strerror(errno));
        return 1;
    }
    if (lstat(path, &st) < 0) {
        printf("error %s\n", strerror(errno));
        return 1;
    }
    printf("%x:", h.fh.handle_type);
    for (unsigned i = 0; i < h.fh.handle_bytes; i++)
        printf("%02x", h.fh.f_handle[i]);
    printf(" %llu\n", (unsigned long long)st.st_ino);
    return 0;
}

static int cmd_open(const char *mountdir, const char *hex)
{
    struct { struct file_handle fh; unsigned char buf[MAX_HANDLE_SZ]; } h;
    unsigned type;
    const char *colon = strchr(hex, ':');
    if (!colon || sscanf(hex, "%x:", &type) != 1) return 2;
    size_t n = strlen(colon + 1) / 2;
    if (n > MAX_HANDLE_SZ) return 2;
    for (size_t i = 0; i < n; i++)
        sscanf(colon + 1 + 2 * i, "%2hhx", &h.fh.f_handle[i]);
    h.fh.handle_type = (int)type;
    h.fh.handle_bytes = (unsigned)n;

    int mfd = open(mountdir, O_RDONLY | O_DIRECTORY);
    if (mfd < 0) { printf("error %s\n", strerror(errno)); return 1; }
    int fd = open_by_handle_at(mfd, &h.fh, O_PATH);
    if (fd < 0) { printf("error %s\n", strerror(errno)); return 1; }
    char lp[64], path[PATH_MAX];
    snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd);
    ssize_t r = readlink(lp, path, sizeof path - 1);
    path[r < 0 ? 0 : r] = 0;
    printf("ok %s\n", path);
    return 0;
}

static int cmd_rename(const char *a, const char *b)
{
    if (renameat2(AT_FDCWD, a, AT_FDCWD, b, RENAME_NOREPLACE) < 0) {
        printf("error %s\n", strerror(errno));
        return 1;
    }
    printf("ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "handle") == 0) return cmd_handle(argv[2]);
    if (argc == 4 && strcmp(argv[1], "open") == 0) return cmd_open(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "rename-noreplace") == 0) return cmd_rename(argv[2], argv[3]);
    fprintf(stderr, "usage: fhtool handle PATH | open MOUNTDIR HANDLE | rename-noreplace OLD NEW\n");
    return 2;
}
