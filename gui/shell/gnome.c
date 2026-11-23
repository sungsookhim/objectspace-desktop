/* Settings shared with GNOME (gnome.h). */
#define _GNU_SOURCE
#include "gnome.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static ui_app *app;
static bool both;
static void (*wps_set)(const char *cls, const char *key, const char *value);
static void (*font_changed)(void);
static bool read_wps_once;    /* the objects' values have been read at least once */

/* GNOME's values we follow (text as gsettings prints it, unquoted). */
static struct { const char *schema, *key; char val[256]; } g[] = {
    { "org.gnome.desktop.interface", "color-scheme", "" },
    { "org.gnome.desktop.a11y.interface", "high-contrast", "" },
    { "org.gnome.desktop.peripherals.mouse", "left-handed", "" },
    { "org.gnome.desktop.peripherals.mouse", "double-click", "" },
    { "org.gnome.desktop.sound", "event-sounds", "" },
    { "org.gnome.desktop.interface", "font-name", "" },
    { "org.gnome.desktop.interface", "text-scaling-factor", "" },
};
#define NG (sizeof g / sizeof *g)
enum { G_SCHEME, G_CONTRAST, G_LEFT, G_DOUBLE, G_SOUNDS, G_FONT, G_SCALE };

/* The WPS objects' values. */
static char w_scheme[32], w_left[8], w_double[16], w_sounds[8];

static pid_t mon_pid[4];
static int mon_fd[4] = { -1, -1, -1, -1 };

const char *gnome_value(const char *key)
{
    for (size_t i = 0; i < NG; i++) if (!strcmp(g[i].key, key)) return g[i].val;
    return NULL;
}

static bool off(void) { return getenv("WPS_NO_GSETTINGS") != NULL; }

bool gnome_get(const char *schema, const char *key, char *out, size_t n)
{
    char cmd[512], buf[4096 + 64] = "";
    snprintf(cmd, sizeof cmd, "gsettings get %s %s 2>/dev/null", schema, key);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    bool ok = fgets(buf, sizeof buf, p) != NULL;
    pclose(p);
    if (!ok) return false;
    buf[strcspn(buf, "\n")] = 0;
    char *s = buf;
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '\'' && s[l - 1] == '\'') { s[l - 1] = 0; s++; }
    if (!strncmp(s, "uint32 ", 7)) s += 7;
    snprintf(out, n, "%s", s);
    return true;
}

static void gget(size_t i) { gnome_get(g[i].schema, g[i].key, g[i].val, sizeof g[i].val); }

/* gsettings set, for values we make (enums, numbers, booleans). */
static void gset(size_t i, const char *value)
{
    if (!strcmp(g[i].val, value)) return;
    snprintf(g[i].val, sizeof g[i].val, "%s", value);   /* what GNOME will say: no echo */
    bool str = i == G_SCHEME;
    char arg[300];
    snprintf(arg, sizeof arg, str ? "'%s'" : "%s", value);
    char *argv[] = { "gsettings", "set", (char *)g[i].schema, (char *)g[i].key, arg, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0) waitpid(pid, NULL, 0);
}

/* GNOME's font: "Family Name 11" in points; WPS draws in pixels (96 dpi). */
static void apply_font(void)
{
    char fam[256];
    snprintf(fam, sizeof fam, "%s", g[G_FONT].val);
    char *sp = strrchr(fam, ' ');
    double pt = sp ? atof(sp + 1) : 0, scale = atof(g[G_SCALE].val);
    if (!sp || pt <= 0) return;
    *sp = 0;
    if (scale <= 0) scale = 1;
    double px = pt * 96.0 / 72.0 * scale;
    double before = ui_scheme_now.font_px;
    ui_text_set_font(fam, (double)(int)(px + 0.5));
    if (ui_scheme_now.font_px != before && font_changed) font_changed();
}

/* What GNOME says, as the WPS objects would say it. */
static const char *scheme_of_gnome(void)
{
    if (!strcmp(g[G_CONTRAST].val, "true")) return "high contrast";
    return !strcmp(g[G_SCHEME].val, "prefer-dark") ? "dark" : "classic";
}

/* GNOME -> WPS: the objects take GNOME's values where they differ. */
static void to_wps(void)
{
    if (!both || !wps_set || !read_wps_once) return;
    const char *sch = scheme_of_gnome();
    if (strcmp(w_scheme, sch)) { snprintf(w_scheme, sizeof w_scheme, "%s", sch); wps_set("WPSchemePalette", "scheme", sch); }
    const char *lh = !strcmp(g[G_LEFT].val, "true") ? "1" : "0";
    if (g[G_LEFT].val[0] && strcmp(w_left, lh)) { snprintf(w_left, sizeof w_left, "%s", lh); wps_set("WPMouse", "left_handed", lh); }
    if (atoi(g[G_DOUBLE].val) > 0 && strcmp(w_double, g[G_DOUBLE].val)) {
        snprintf(w_double, sizeof w_double, "%s", g[G_DOUBLE].val);
        wps_set("WPMouse", "double_click_ms", g[G_DOUBLE].val);
    }
    const char *es = !strcmp(g[G_SOUNDS].val, "false") ? "0" : "1";
    if (g[G_SOUNDS].val[0] && strcmp(w_sounds, es)) { snprintf(w_sounds, sizeof w_sounds, "%s", es); wps_set("WPSound", "enabled", es); }
}

/* WPS -> GNOME. */
static void to_gnome(void)
{
    if (!both) return;
    if (!strcasecmp(w_scheme, "high contrast")) gset(G_CONTRAST, "true");
    else if (w_scheme[0]) {
        gset(G_CONTRAST, "false");
        bool dark = !strcasecmp(w_scheme, "dark");
        /* classic is GNOME's default, unless GNOME already says light */
        if (dark || strcmp(g[G_SCHEME].val, "prefer-light")) gset(G_SCHEME, dark ? "prefer-dark" : "default");
    }
    if (w_left[0]) gset(G_LEFT, !strcmp(w_left, "1") ? "true" : "false");
    if (atoi(w_double) > 0) gset(G_DOUBLE, w_double);
    if (w_sounds[0]) gset(G_SOUNDS, !strcmp(w_sounds, "1") ? "true" : "false");
}

void gnome_wps_value(const char *cls, const char *key, const char *value)
{
    if (!strcmp(cls, "WPSchemePalette") && !strcmp(key, "scheme")) snprintf(w_scheme, sizeof w_scheme, "%s", value);
    else if (!strcmp(cls, "WPMouse") && !strcmp(key, "left_handed")) snprintf(w_left, sizeof w_left, "%s", value);
    else if (!strcmp(cls, "WPMouse") && !strcmp(key, "double_click_ms")) snprintf(w_double, sizeof w_double, "%s", value);
    else if (!strcmp(cls, "WPSound") && !strcmp(key, "enabled")) snprintf(w_sounds, sizeof w_sounds, "%s", value);
}

void gnome_wps_read(void)
{
    if (off()) return;
    if (!read_wps_once) {
        /* the first time: GNOME's values win */
        read_wps_once = true;
        to_wps();
        return;
    }
    to_gnome();
}

/* ------------------------------------------------------------ following */

static void read_all(void)
{
    for (size_t i = 0; i < NG; i++) gget(i);
}

static void on_monitor(void *arg, int fd, short rev)
{
    (void)arg;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0 || (rev & (POLLHUP | POLLERR))) {
        ui_app_unwatch_fd(app, fd);
        close(fd);
        for (int i = 0; i < 4; i++) if (mon_fd[i] == fd) mon_fd[i] = -1;
        return;
    }
    read_all();
    apply_font();
    to_wps();
}

static void monitor(int i, const char *schema)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return;
    char *argv[] = { "gsettings", "monitor", (char *)schema, NULL };
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

void gnome_start(ui_app *a, bool both_ways, void (*set)(const char *cls, const char *key, const char *value),
                 void (*font)(void))
{
    app = a;
    both = both_ways;
    wps_set = set;
    font_changed = font;
    if (off()) return;
    read_all();
    apply_font();
    monitor(0, "org.gnome.desktop.interface");
    if (both) {
        monitor(1, "org.gnome.desktop.a11y.interface");
        monitor(2, "org.gnome.desktop.peripherals.mouse");
        monitor(3, "org.gnome.desktop.sound");
    }
}

void gnome_stop(void)
{
    for (int i = 0; i < 4; i++) {
        if (mon_fd[i] >= 0) { ui_app_unwatch_fd(app, mon_fd[i]); close(mon_fd[i]); mon_fd[i] = -1; }
        if (mon_pid[i] > 0) { kill(mon_pid[i], SIGTERM); waitpid(mon_pid[i], NULL, 0); mon_pid[i] = 0; }
    }
}
