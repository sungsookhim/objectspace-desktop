/* Objects' own icons (icons.h). */
#define _GNU_SOURCE
#include "icons.h"

#include <errno.h>
#include <stdbool.h>
#include <strings.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define SIZE 128
#define CACHE 256

static ui_app *app;
static struct { char *path; cairo_surface_t *s; uint64_t used; } cache[CACHE];
static uint64_t tick;

void icons_init(ui_app *a) { app = a; }

static void data_dir(const char *sub, char *out, size_t n)
{
    const char *x = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/wps/%s", x, sub);
    else snprintf(out, n, "%s/.local/share/wps/%s", home ? home : "/tmp", sub);
    /* made, parents and all */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", out);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

const char *icons_library_dir(void)
{
    static char dir[PATH_MAX];
    const char *t = getenv("WPS_ICON_LIBRARY");   /* tests */
    if (t && *t) { snprintf(dir, sizeof dir, "%s", t); mkdir(dir, 0755); }
    else data_dir("Icons", dir, sizeof dir);
    return dir;
}

cairo_surface_t *icons_get(const char *png)
{
    if (!png || !*png) return NULL;
    size_t victim = 0;
    for (size_t i = 0; i < CACHE; i++) {
        if (cache[i].path && !strcmp(cache[i].path, png)) { cache[i].used = ++tick; return cache[i].s; }
        if (!cache[i].path) { victim = i; break; }
        if (cache[i].used < cache[victim].used) victim = i;
    }
    cairo_surface_t *s = cairo_image_surface_create_from_png(png);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); s = NULL; }
    /* (a failure is cached too: the file is not read again each frame) */
    free(cache[victim].path);
    if (cache[victim].s) cairo_surface_destroy(cache[victim].s);
    cache[victim].path = strdup(png);
    cache[victim].s = s;
    cache[victim].used = ++tick;
    return s;
}

/* ------------------------------------------------------------- import */

struct job {
    char out[PATH_MAX], tmp[PATH_MAX + 16];
    pid_t pid;
    int fd;
    void (*done)(void *ctx, const char *png);
    void *ctx;
    bool quiet;              /* a preview: a picture that cannot be read is just left out */
};

static void finished(void *arg, int fd, short rev)
{
    (void)rev;
    struct job *j = arg;
    ui_app_unwatch_fd(app, fd);
    close(fd);
    int st = 0;
    waitpid(j->pid, &st, 0);
    struct stat sb;
    bool ok = WIFEXITED(st) && !WEXITSTATUS(st) && stat(j->tmp, &sb) == 0 && sb.st_size > 0 && rename(j->tmp, j->out) == 0;
    if (!ok) {
        unlink(j->tmp);
        if (!j->quiet) fprintf(stderr, "wps-shell: cannot make an icon of that image\n");
    }
    j->done(j->ctx, ok ? j->out : NULL);
    free(j);
}

static void convert(struct job *j, const char *src, int size);

void icons_import(const char *src, void (*done)(void *ctx, const char *png), void *ctx)
{
    struct job *j = calloc(1, sizeof *j);
    j->done = done;
    j->ctx = ctx;
    char dir[PATH_MAX];
    data_dir("icons", dir, sizeof dir);
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char stem[64];
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    for (char *p = stem; *p; p++) if (*p == '/' || *p == ' ') *p = '_';
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(j->out, sizeof j->out, "%s/%s-%lld%03ld.png", dir, stem, (long long)ts.tv_sec, ts.tv_nsec / 1000000);
    convert(j, src, SIZE);
}

/* src to j->out (through j->tmp) by glycin-thumbnailer, then j->done. */
static void convert(struct job *j, const char *src, int size)
{
    snprintf(j->tmp, sizeof j->tmp, "%s.tmp", j->out);
    /* glycin takes a URL: the path, its odd characters escaped */
    char uri[3 * PATH_MAX + 8] = "file://";
    size_t u = strlen(uri);
    for (const unsigned char *p = (const unsigned char *)src; *p && u < sizeof uri - 4; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p))
            uri[u++] = (char)*p;
        else
            u += (size_t)snprintf(uri + u, sizeof uri - u, "%%%02X", *p);
    }
    uri[u] = 0;
    char sz[16];
    snprintf(sz, sizeof sz, "%d", size);
    char *argv[] = { "glycin-thumbnailer", "-i", uri, "-o", j->tmp, "-s", sz, NULL };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    int e = posix_spawnp(&j->pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    j->fd = e ? -1 : (int)syscall(SYS_pidfd_open, j->pid, 0);
    if (e || j->fd < 0) {
        if (!e) waitpid(j->pid, NULL, 0);
        fprintf(stderr, "wps-shell: glycin-thumbnailer is needed to make icons\n");
        j->done(j->ctx, NULL);
        free(j);
        return;
    }
    ui_app_watch_fd(app, j->fd, POLLIN, finished, j);
}

/* ----------------------------------------------------------- previews */

static uint64_t fnv(const char *s, uint64_t h)
{
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

bool icons_preview(const char *src, int size, char *out, size_t n, void (*done)(void *ctx, const char *png), void *ctx)
{
    size_t l = strlen(src);
    if (l > 4 && !strcasecmp(src + l - 4, ".png")) { snprintf(out, n, "%s", src); return true; }
    struct stat st;
    if (stat(src, &st) != 0) return false;
    const char *x = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char dir[PATH_MAX];
    if (x && *x) snprintf(dir, sizeof dir, "%s/wps/icon-previews", x);
    else snprintf(dir, sizeof dir, "%s/.cache/wps/icon-previews", home ? home : "/tmp");
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
    char key[64];
    snprintf(key, sizeof key, "%lld-%d", (long long)st.st_mtime, size);
    snprintf(out, n, "%s/%016llx.png", dir, (unsigned long long)fnv(key, fnv(src, 1469598103934665603ull)));
    if (access(out, R_OK) == 0) return true;
    struct job *j = calloc(1, sizeof *j);
    j->done = done;
    j->ctx = ctx;
    j->quiet = true;
    snprintf(j->out, sizeof j->out, "%s", out);
    convert(j, src, size);
    return false;
}
