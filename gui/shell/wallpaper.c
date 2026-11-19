/* The desktop's background, as GNOME has it (wallpaper.h). */
#define _GNU_SOURCE
#include "wallpaper.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static ui_app *app;
static void (*on_changed)(void *ctx);
static void *changed_ctx;

static struct {
    char uri[PATH_MAX + 16];      /* the picture, or "" for colour only */
    char options[32];             /* zoom, scaled, stretched, centered, wallpaper, spanned, none */
    double c1[3], c2[3];          /* primary and secondary colours */
    char shading[16];             /* solid, horizontal, vertical */
} want;

static cairo_surface_t *src;      /* the decoded picture */
static char src_uri[sizeof want.uri];
static int src_size;              /* the largest side it was decoded for (0: full size) */
static cairo_surface_t *comp;     /* the background at the desktop's size, in pixels */
static double lw, lh;             /* the desktop, logical */
static int scale = 1;
static char showing[PATH_MAX + 16] = "color";

static pid_t dec_pid;             /* a decode under way */
static int dec_fd = -1;           /* its pidfd */
static char dec_uri[sizeof want.uri], dec_tmp[PATH_MAX + 64], dec_out[PATH_MAX + 64];
static int dec_size;
static bool dec_again;            /* something else is wanted once this one ends */

static pid_t mon_pid[2];
static int mon_fd[2] = { -1, -1 };

/* --------------------------------------------------------------- settings */

/* `gsettings get schema key`, unquoted, into out. */
static bool gsetting(const char *schema, const char *key, char *out, size_t n)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "gsettings get %s %s 2>/dev/null", schema, key);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char buf[PATH_MAX + 64] = "";
    bool ok = fgets(buf, sizeof buf, p) != NULL;
    pclose(p);
    if (!ok) return false;
    buf[strcspn(buf, "\n")] = 0;
    char *s = buf;
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '\'' && s[l - 1] == '\'') { s[l - 1] = 0; s++; }
    snprintf(out, n, "%s", s);
    return true;
}

static void parse_color(const char *s, double c[3])
{
    unsigned r, g, b;
    if (s && sscanf(s, "#%02x%02x%02x", &r, &g, &b) == 3) { c[0] = r / 255.0; c[1] = g / 255.0; c[2] = b / 255.0; }
}

static void read_settings(void)
{
    char v[PATH_MAX + 64];
    memset(&want, 0, sizeof want);
    snprintf(want.options, sizeof want.options, "zoom");
    snprintf(want.shading, sizeof want.shading, "solid");
    want.c1[0] = 0.0; want.c1[1] = 0.5; want.c1[2] = 0.5;   /* OS/2's desktop teal, when GNOME says nothing */
    const char *fixed = getenv("WPS_WALLPAPER");          /* tests: this picture, no GNOME settings */
    if (fixed) {
        if (*fixed) snprintf(want.uri, sizeof want.uri, "%s%s", fixed[0] == '/' ? "file://" : "", fixed);
        return;
    }
    bool dark = gsetting("org.gnome.desktop.interface", "color-scheme", v, sizeof v) && !strcmp(v, "prefer-dark");
    if (!(dark && gsetting("org.gnome.desktop.background", "picture-uri-dark", v, sizeof v) && *v) &&
        !gsetting("org.gnome.desktop.background", "picture-uri", v, sizeof v))
        v[0] = 0;
    snprintf(want.uri, sizeof want.uri, "%s", v);
    if (gsetting("org.gnome.desktop.background", "picture-options", v, sizeof v) && *v) snprintf(want.options, sizeof want.options, "%s", v);
    if (gsetting("org.gnome.desktop.background", "primary-color", v, sizeof v)) parse_color(v, want.c1);
    memcpy(want.c2, want.c1, sizeof want.c2);
    if (gsetting("org.gnome.desktop.background", "secondary-color", v, sizeof v)) parse_color(v, want.c2);
    if (gsetting("org.gnome.desktop.background", "color-shading-type", v, sizeof v) && *v) snprintf(want.shading, sizeof want.shading, "%s", v);
    if (!strcmp(want.options, "none")) want.uri[0] = 0;
}

/* "file:///a%20b" -> "/a b" */
static bool uri_to_path(const char *u, char *out, size_t n)
{
    if (u[0] == '/') { snprintf(out, n, "%s", u); return true; }
    if (strncmp(u, "file://", 7)) return false;
    size_t o = 0;
    for (const char *p = u + 7; *p && o + 1 < n; p++) {
        unsigned v;
        if (*p == '%' && sscanf(p + 1, "%2x", &v) == 1) { out[o++] = (char)v; p += 2; }
        else out[o++] = *p;
    }
    out[o] = 0;
    return true;
}

/* A GNOME background XML (slide shows, day and night): its first picture. */
static bool xml_first_file(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "re");
    if (!f) return false;
    char *buf = malloc(1 << 20);
    size_t len = buf ? fread(buf, 1, (1 << 20) - 1, f) : 0;
    fclose(f);
    if (!buf) return false;
    buf[len] = 0;
    bool ok = false;
    char *a = strstr(buf, "<file>");
    if (a) {
        a += 6;
        char *sz = strstr(a, "<size");
        char *end = strstr(a, "</file>");
        if (sz && end && sz < end) { a = strchr(sz, '>'); a = a ? a + 1 : NULL; end = a ? strstr(a, "</size>") : NULL; }
        if (a && end) {
            while (*a == ' ' || *a == '\n' || *a == '\t') a++;
            while (end > a && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\t')) end--;
            snprintf(out, n, "%.*s", (int)(end - a), a);
            ok = true;
        }
    }
    free(buf);
    return ok;
}

/* ------------------------------------------------------------- composing */

static void compose(void)
{
    int pw = (int)ceil(lw * scale), ph = (int)ceil(lh * scale);
    if (pw <= 0 || ph <= 0) return;
    if (comp) cairo_surface_destroy(comp);
    comp = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw, ph);
    cairo_t *cr = cairo_create(comp);
    if (!strcmp(want.shading, "solid")) {
        cairo_set_source_rgb(cr, want.c1[0], want.c1[1], want.c1[2]);
    } else {
        bool horiz = !strcmp(want.shading, "horizontal");
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, horiz ? pw : 0, horiz ? 0 : ph);
        cairo_pattern_add_color_stop_rgb(g, 0, want.c1[0], want.c1[1], want.c1[2]);
        cairo_pattern_add_color_stop_rgb(g, 1, want.c2[0], want.c2[1], want.c2[2]);
        cairo_set_source(cr, g);
        cairo_pattern_destroy(g);
    }
    cairo_paint(cr);
    snprintf(showing, sizeof showing, "color");
    if (src && want.uri[0] && !strcmp(src_uri, want.uri)) {
        double sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src);
        const char *o = want.options;
        cairo_save(cr);
        if (!strcmp(o, "wallpaper")) {
            cairo_pattern_t *p = cairo_pattern_create_for_surface(src);
            cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
            cairo_set_source(cr, p);
            cairo_pattern_destroy(p);
            cairo_paint(cr);
        } else {
            double sx, sy;
            if (!strcmp(o, "stretched")) { sx = pw / sw; sy = ph / sh; }
            else if (!strcmp(o, "scaled")) sx = sy = fmin(pw / sw, ph / sh);
            else if (!strcmp(o, "centered")) sx = sy = (double)scale * (src_size ? 1.0 : 1.0);
            else sx = sy = fmax(pw / sw, ph / sh);   /* zoom, spanned */
            cairo_translate(cr, (pw - sw * sx) / 2, (ph - sh * sy) / 2);
            cairo_scale(cr, sx, sy);
            cairo_set_source_surface(cr, src, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
        }
        cairo_restore(cr);
        char path[PATH_MAX];
        if (uri_to_path(want.uri, path, sizeof path)) snprintf(showing, sizeof showing, "%s", path);
    }
    cairo_destroy(cr);
    cairo_surface_flush(comp);
}

static void changed(void)
{
    compose();
    if (on_changed) on_changed(changed_ctx);
}

/* -------------------------------------------------------------- decoding */

static uint64_t fnv(const char *s, uint64_t h)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

static bool cache_dir(char *out, size_t n)
{
    const char *c = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (c && *c) snprintf(out, n, "%s/wps", c);
    else if (home) snprintf(out, n, "%s/.cache/wps", home);
    else return false;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", out);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
    return true;
}

static void loaded(cairo_surface_t *img, const char *uri, int size)
{
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        return;
    }
    if (src) cairo_surface_destroy(src);
    src = img;
    snprintf(src_uri, sizeof src_uri, "%s", uri);
    src_size = size;
    changed();
}

static void want_picture(void);

static void decoded(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    siginfo_t si;
    memset(&si, 0, sizeof si);
    waitid(P_PIDFD, (id_t)fd, &si, WEXITED | WNOHANG);
    ui_app_unwatch_fd(app, fd);
    close(fd);
    dec_fd = -1;
    dec_pid = 0;
    if (si.si_code == CLD_EXITED && si.si_status == 0 && rename(dec_tmp, dec_out) == 0)
        loaded(cairo_image_surface_create_from_png(dec_out), dec_uri, dec_size);
    else {
        unlink(dec_tmp);
        fprintf(stderr, "wps-shell: cannot read the background picture %s\n", dec_uri);
        snprintf(src_uri, sizeof src_uri, "%s", dec_uri);   /* do not try it again and again */
        src_size = dec_size;
    }
    if (dec_again) { dec_again = false; want_picture(); }
}

/* Decode want.uri at a size that covers the desktop, unless that is done. */
static void want_picture(void)
{
    if (!want.uri[0]) return;
    if (dec_fd >= 0) { dec_again = true; return; }
    int need = (int)ceil(fmax(lw, lh) * scale);
    if (need <= 0) return;
    need = (need + 511) / 512 * 512;   /* fewer distinct sizes, fewer decodes */
    if (!strcmp(src_uri, want.uri) && (src_size == 0 || src_size >= need)) return;
    char path[PATH_MAX], pic[PATH_MAX], uri[sizeof want.uri];
    snprintf(uri, sizeof uri, "%s", want.uri);
    if (!uri_to_path(uri, path, sizeof path)) return;
    size_t pl = strlen(path);
    if (pl > 4 && !strcmp(path + pl - 4, ".xml")) {
        if (!xml_first_file(path, pic, sizeof pic)) return;
        snprintf(path, sizeof path, "%s", pic);
        snprintf(uri, sizeof uri, "file://%s", pic);
        pl = strlen(path);
    }
    if (pl > 4 && !strcasecmp(path + pl - 4, ".png")) {
        snprintf(src_uri, sizeof src_uri, "%s", want.uri);
        loaded(cairo_image_surface_create_from_png(path), want.uri, 0);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) return;
    char dir[PATH_MAX], key[64];
    if (!cache_dir(dir, sizeof dir)) return;
    snprintf(key, sizeof key, "%lld-%d", (long long)st.st_mtime, need);
    uint64_t h = fnv(key, fnv(path, 1469598103934665603ull));
    snprintf(dec_out, sizeof dec_out, "%s/background-%016llx.png", dir, (unsigned long long)h);
    if (access(dec_out, R_OK) == 0) {   /* decoded before */
        loaded(cairo_image_surface_create_from_png(dec_out), want.uri, need);
        return;
    }
    snprintf(dec_tmp, sizeof dec_tmp, "%s.%d.tmp", dec_out, (int)getpid());
    snprintf(dec_uri, sizeof dec_uri, "%s", want.uri);
    dec_size = need;
    char sz[16];
    snprintf(sz, sizeof sz, "%d", need);
    char *argv[] = { "glycin-thumbnailer", "-i", uri, "-o", dec_tmp, "-s", sz, NULL };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int e = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (e) {
        fprintf(stderr, "wps-shell: glycin-thumbnailer is not installed: the background is a colour\n");
        snprintf(src_uri, sizeof src_uri, "%s", want.uri);
        return;
    }
    dec_pid = pid;
    dec_fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (dec_fd < 0) { waitpid(pid, NULL, 0); dec_pid = 0; return; }
    ui_app_watch_fd(app, dec_fd, POLLIN, decoded, NULL);
}

/* ------------------------------------------------------------- following */

static void reload(void)
{
    read_settings();
    changed();         /* the colour at once */
    want_picture();    /* the picture when it is ready */
}

static void on_monitor(void *arg, int fd, short rev)
{
    (void)arg;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0 || (rev & (POLLHUP | POLLERR))) {
        ui_app_unwatch_fd(app, fd);
        close(fd);
        for (int i = 0; i < 2; i++) if (mon_fd[i] == fd) mon_fd[i] = -1;
        return;
    }
    reload();
}

static void monitor(int i, const char *schema, const char *key)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return;
    char *argv[] = { "gsettings", "monitor", (char *)schema, (char *)key, NULL };
    if (!key) argv[3] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    int e = posix_spawnp(&mon_pid[i], argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (e) { close(p[0]); mon_pid[i] = 0; return; }
    mon_fd[i] = p[0];
    ui_app_watch_fd(app, p[0], POLLIN, on_monitor, NULL);
}

void wallpaper_start(ui_app *a, void (*fn)(void *ctx), void *ctx)
{
    app = a;
    on_changed = fn;
    changed_ctx = ctx;
    read_settings();
    if (!getenv("WPS_WALLPAPER")) {
        monitor(0, "org.gnome.desktop.background", NULL);
        monitor(1, "org.gnome.desktop.interface", "color-scheme");
    }
}

void wallpaper_stop(void)
{
    for (int i = 0; i < 2; i++) {
        if (mon_fd[i] >= 0) { ui_app_unwatch_fd(app, mon_fd[i]); close(mon_fd[i]); mon_fd[i] = -1; }
        if (mon_pid[i] > 0) { kill(mon_pid[i], SIGTERM); waitpid(mon_pid[i], NULL, 0); mon_pid[i] = 0; }
    }
    if (dec_fd >= 0) {
        ui_app_unwatch_fd(app, dec_fd);
        close(dec_fd);
        dec_fd = -1;
        kill(dec_pid, SIGTERM);
        waitpid(dec_pid, NULL, 0);
        unlink(dec_tmp);
    }
    if (src) cairo_surface_destroy(src);
    if (comp) cairo_surface_destroy(comp);
    src = comp = NULL;
    on_changed = NULL;
}

void wallpaper_set_size(double w, double h, int sc)
{
    if (w == lw && h == lh && sc == scale && comp) return;
    lw = w;
    lh = h;
    scale = sc < 1 ? 1 : sc;
    compose();
    want_picture();
}

void wallpaper_draw(cairo_t *cr, ui_rect clip)
{
    cairo_save(cr);
    cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
    cairo_clip(cr);
    if (comp) {
        cairo_scale(cr, 1.0 / scale, 1.0 / scale);
        cairo_set_source_surface(cr, comp, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    } else {
        cairo_set_source_rgb(cr, want.c1[0], want.c1[1], want.c1[2]);
    }
    cairo_paint(cr);
    cairo_restore(cr);
}

const char *wallpaper_showing(void) { return showing; }
