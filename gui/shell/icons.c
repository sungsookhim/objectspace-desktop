/* Objects' own icons (icons.h). */
#define _GNU_SOURCE
#include "icons.h"

#ifndef WPS_ICONDIR
#define WPS_ICONDIR "/usr/share/wps/icons"
#endif

#include <math.h>
#include "wpsicon.h"
#include "wps_themes.h"
#include "mime.h"

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
static char import_error[200];
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
        if (!j->quiet) {
            if (WIFEXITED(st) && WEXITSTATUS(st))
                snprintf(import_error, sizeof import_error, "glycin-thumbnailer could not read it (exit %d; see the log)", WEXITSTATUS(st));
            else snprintf(import_error, sizeof import_error, "glycin-thumbnailer made nothing (see the log)");
            fprintf(stderr, "wps-shell: cannot make an icon of that image: %s\n", import_error);
        }
    }
    j->done(j->ctx, ok ? j->out : NULL);
    free(j);
}

static void convert(struct job *j, const char *src, int size);

const char *icons_import_error(void) { return import_error[0] ? import_error : "it cannot be read"; }

/* A PNG: made by cairo itself, scaled down to SIZE (no glycin needed). */
static bool import_png(const char *src, const char *out)
{
    cairo_surface_t *in = cairo_image_surface_create_from_png(src);
    if (cairo_surface_status(in) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(in); return false; }
    int w = cairo_image_surface_get_width(in), h = cairo_image_surface_get_height(in);
    double sc = w > SIZE || h > SIZE ? (double)SIZE / (w > h ? w : h) : 1;
    int ow = (int)(w * sc + 0.5), oh = (int)(h * sc + 0.5);
    cairo_surface_t *o = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ow > 0 ? ow : 1, oh > 0 ? oh : 1);
    cairo_t *cr = cairo_create(o);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, in, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_destroy(cr);
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    bool ok = cairo_surface_write_to_png(o, tmp) == CAIRO_STATUS_SUCCESS && rename(tmp, out) == 0;
    if (!ok) unlink(tmp);
    cairo_surface_destroy(o);
    cairo_surface_destroy(in);
    return ok;
}

struct posted { void (*done)(void *ctx, const char *png); void *ctx; char out[PATH_MAX]; bool ok; };

static void post_done(void *arg)
{
    struct posted *d = arg;
    d->done(d->ctx, d->ok ? d->out : NULL);
    free(d);
}

void icons_import(const char *src, void (*done)(void *ctx, const char *png), void *ctx)
{
    import_error[0] = 0;
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
    size_t l = strlen(src);
    if (l > 8 && !strcasecmp(src + l - 8, ".wpsicon")) {
        /* an icon package (0.4): kept as it is, all its sizes (and SVG) */
        wpsicon *w = wpsicon_open(src);
        bool ok = w != NULL;
        wpsicon_free(w);
        snprintf(j->out + strlen(j->out) - 4, 9, ".wpsicon");
        if (ok) {
            FILE *in = fopen(src, "rb"), *o = fopen(j->out, "wb");
            char buf[65536];
            size_t k;
            ok = in && o;
            while (ok && (k = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, k, o) == k;
            if (in) fclose(in);
            if (o && fclose(o) != 0) ok = false;
        }
        if (!ok) snprintf(import_error, sizeof import_error, "not an icon package that can be read");
        struct posted *d = calloc(1, sizeof *d);
        *d = (struct posted){ done, ctx, "", ok };
        snprintf(d->out, sizeof d->out, "%s", ok ? j->out : "");
        free(j);
        ui_app_post(app, post_done, d);
        return;
    }
    if (l > 4 && !strcasecmp(src + l - 4, ".png") && import_png(src, j->out)) {   /* (later, as glycin's would be) */
        struct posted *d = calloc(1, sizeof *d);
        *d = (struct posted){ done, ctx, "", true };
        snprintf(d->out, sizeof d->out, "%s", j->out);
        free(j);
        ui_app_post(app, post_done, d);
        return;
    }
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
    if (j->quiet) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);   /* (previews: no noise) */
    int e = posix_spawnp(&j->pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    j->fd = e ? -1 : (int)syscall(SYS_pidfd_open, j->pid, 0);
    if (e || j->fd < 0) {
        if (!e) waitpid(j->pid, NULL, 0);
        snprintf(import_error, sizeof import_error, "glycin-thumbnailer is not installed");
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
    if (icons_object_png(src, size, out, n)) return true;   /* a package (0.4): drawn at once */
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

/* ---------------------------------------------------- icon themes (0.4) */

static char default_theme[128] = "WPS (flat)";
static struct { char *key; cairo_surface_t *s; unsigned long used; } tcache[256];
static unsigned long ttick;

void icons_set_default_theme(const char *theme)
{
    if (theme && *theme) snprintf(default_theme, sizeof default_theme, "%s", theme);
}

const char *icons_default_theme(void) { return default_theme; }

/* a package's path in a theme (by name or id), or "" */
static bool package_in(const char *theme, const char *name, char *out, size_t n)
{
    char dir[PATH_MAX];
    if (!wps_icon_theme_dir(theme, dir, sizeof dir)) return false;
    snprintf(out, n, "%s/%s.wpsicon", dir, name);
    return access(out, R_OK) == 0;
}

static cairo_surface_t *package_at(const char *path, int px)
{
    char key[PATH_MAX + 32];
    snprintf(key, sizeof key, "%s@%d", path, px);
    size_t victim = 0;
    for (size_t i = 0; i < sizeof tcache / sizeof *tcache; i++) {
        if (tcache[i].key && !strcmp(tcache[i].key, key)) { tcache[i].used = ++ttick; return tcache[i].s; }
        if (!tcache[i].key) { victim = i; break; }
        if (tcache[i].used < tcache[victim].used) victim = i;
    }
    wpsicon *w = wpsicon_open(path);
    cairo_surface_t *s = w ? wpsicon_render(w, px, NULL) : NULL;   /* (drawn once per size: cached) */
    wpsicon_free(w);
    free(tcache[victim].key);
    if (tcache[victim].s) cairo_surface_destroy(tcache[victim].s);
    tcache[victim].key = strdup(key);
    tcache[victim].s = s;
    tcache[victim].used = ++ttick;
    return s;
}

/* WPS's 0.2 picture of a name (installed, or a build tree's data/icons) */
static bool classic_png(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s.png", WPS_ICONDIR, name);
    if (access(out, R_OK) == 0) return true;
    char exe[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (l <= 0) return false;
    exe[l] = 0;
    for (int up = 0; up < 2; up++) { char *s = strrchr(exe, '/'); if (s) *s = 0; }
    snprintf(out, n, "%s/data/icons/%s.png", exe, name);
    return access(out, R_OK) == 0;
}

cairo_surface_t *icons_theme(const char *theme, const char *name, int px)
{
    char p[PATH_MAX + 64];
    if (!name || !*name) return NULL;
    if (package_in(theme && *theme ? theme : default_theme, name, p, sizeof p) || package_in("wps-flat", name, p, sizeof p))
        return package_at(p, px);
    /* no theme has it (none installed: a checkout without make icons) */
    if (classic_png(name, p, sizeof p)) return icons_get(p);
    return NULL;
}

cairo_surface_t *icons_object(const char *theme, const char *icon)
{
    if (!icon || !*icon) return NULL;
    if (!strncmp(icon, "theme:", 6)) return icons_theme(theme, icon + 6, 64);   /* (64: sharp at 32, and at scale 2) */
    size_t l = strlen(icon);
    if (l > 8 && !strcmp(icon + l - 8, ".wpsicon")) return package_at(icon, 64);   /* its own, a package */
    return icons_get(icon);
}

const char *icons_name_for_type(const char *m) { return wps_icon_name_for_type(m); }

/* $XDG_CACHE_HOME/wps/theme-icons, made (each level: a new home has no
 * ~/.cache yet) */
static void theme_cache_dir(char *dir, size_t n)
{
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (cache && *cache) snprintf(dir, n, "%s/wps/theme-icons", cache);
    else snprintf(dir, n, "%s/.cache/wps/theme-icons", home && *home ? home : "/tmp");
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0700); *c = '/'; }
    mkdir(dir, 0700);
}

bool icons_theme_png(const char *theme, const char *name, int px, char *out, size_t n)
{
    cairo_surface_t *s = icons_theme(theme, name, px);
    if (!s) return false;
    char dir[PATH_MAX];
    theme_cache_dir(dir, sizeof dir);
    char safe[128];
    snprintf(safe, sizeof safe, "%s", theme && *theme ? theme : default_theme);
    for (char *c = safe; *c; c++) if (*c == '/' || *c == ' ' || *c == '(' || *c == ')') *c = '_';
    snprintf(out, n, "%s/%s-%s-%d.png", dir, safe, name, px);
    if (access(out, R_OK) == 0) return true;
    return cairo_surface_write_to_png(s, out) == CAIRO_STATUS_SUCCESS;
}

bool icons_draw_theme(cairo_t *cr, const char *theme, const char *name, ui_rect r, double pad)
{
    double side = fmin(r.w, r.h) - 2 * pad;
    if (side < 4) return false;
    double dx = side, dy = 0;
    cairo_user_to_device_distance(cr, &dx, &dy);   /* its pixels on the screen: drawn at that size, sharp */
    int px = (int)(fabs(dx) + 0.5);
    cairo_surface_t *s = px > 0 ? icons_theme(theme, name, px) : NULL;
    if (!s) return false;
    cairo_save(cr);
    cairo_translate(cr, r.x + (r.w - side) / 2, r.y + (r.h - side) / 2);
    cairo_scale(cr, side / px, side / px);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return true;
}

cairo_surface_t *icons_item(const char *theme, const char *icon, ui_kind kind, const char *name)
{
    cairo_surface_t *s = icons_object(theme, icon);
    if (s) return s;
    const char *nm = kind == UI_KIND_FOLDER ? "folder" : kind == UI_KIND_PROGRAM ? "program" : NULL;
    if (!nm && kind == UI_KIND_FILE) {
        const char *sl = name ? strrchr(name, '/') : NULL;
        nm = icons_name_for_type(name ? mime_of_name(sl ? sl + 1 : name) : NULL);
    }
    return nm ? icons_theme(theme, nm, 64) : NULL;
}

bool icons_object_png(const char *icon, int px, char *out, size_t n)
{
    if (!icon) return false;
    if (!strncmp(icon, "theme:", 6)) return icons_theme_png(NULL, icon + 6, px, out, n);
    size_t l = strlen(icon);
    if (l < 9 || strcmp(icon + l - 8, ".wpsicon")) return false;
    cairo_surface_t *s = package_at(icon, px);
    if (!s) return false;
    char dir[PATH_MAX];
    theme_cache_dir(dir, sizeof dir);
    unsigned long h = 5381;   /* (by its path: an imported package's name is new each time) */
    for (const char *c = icon; *c; c++) h = h * 33 + (unsigned char)*c;
    snprintf(out, n, "%s/pkg-%lx-%d.png", dir, h, px);
    if (access(out, R_OK) == 0) return true;
    return cairo_surface_write_to_png(s, out) == CAIRO_STATUS_SUCCESS;
}
