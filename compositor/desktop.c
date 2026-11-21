/*
 * WPS's desktop process, as the compositor runs it (comp.h).
 *
 * The same arrangement as the GNOME extension's (gnome-extension/
 * wps@wps-linux), so wps-shell needs nothing new: the compositor starts
 * `COMMAND --events-stdin` on a Wayland connection of its own (a socket
 * pair: only that process can be the desktop), writes to its standard
 * input
 *     <event> TAB <app ID> TAB <title> TAB <x> TAB <y> TAB <w> TAB <h>
 *     W TAB add|change|remove TAB <id> TAB <app ID> TAB <title> TAB <minimized>
 *         TAB <focused> TAB <icon PNG> TAB <maximized>
 *     K TAB window-list
 *     I TAB <index> TAB <count> TAB xkb|ibus TAB <name>   the input source now
 * and reads commands from its standard output, one a line:
 *     activate|restore|minimize|maximize|close|terminate TAB <id>
 *     lock   find   shutdown TAB poweroff|reboot|logout
 *     keyboard TAB next|prev|<index>   panel TAB top|bottom (the WarpCenter's edge)
 * If it ends, it is started again after a delay that grows while it
 * keeps failing (from a second up to half a minute).
 *
 * App icons: the app's .desktop file's Icon, as a PNG from the icon
 * themes (an SVG made into one by glycin-thumbnailer, in
 * $XDG_RUNTIME_DIR/wps-icons).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "comp.h"

static struct {
    char *command;
    struct wl_client *client;
    struct wl_listener client_destroy;
    int in_fd, out_fd;              /* its standard input (we write), output (we read) */
    struct wl_event_source *out_src, *in_src, *restart;
    char *queue;                    /* lines not yet written */
    size_t qlen, qcap;
    char rbuf[4096];
    size_t rlen;
    int delay_ms;
    double started;
    bool stopping;
} d = { .in_fd = -1, .out_fd = -1 };

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

bool desktop_is(struct wl_client *c) { return c && c == d.client; }

/* --------------------------------------------------------------- output */

static int on_writable(int fd, uint32_t mask, void *data);

static void flush(void)
{
    while (d.qlen && d.in_fd >= 0) {
        ssize_t w = write(d.in_fd, d.queue, d.qlen);
        if (w < 0) {
            if (errno == EAGAIN) break;
            d.qlen = 0;   /* gone */
            return;
        }
        memmove(d.queue, d.queue + w, d.qlen - (size_t)w);
        d.qlen -= (size_t)w;
    }
    if (d.in_fd < 0) return;
    if (d.qlen && !d.in_src) d.in_src = wl_event_loop_add_fd(server.loop, d.in_fd, WL_EVENT_WRITABLE, on_writable, NULL);
    else if (!d.qlen && d.in_src) { wl_event_source_remove(d.in_src); d.in_src = NULL; }
}

static int on_writable(int fd, uint32_t mask, void *data) { (void)fd; (void)mask; (void)data; flush(); return 0; }

static void send_line(const char *line)
{
    if (d.in_fd < 0) return;
    size_t n = strlen(line);
    if (d.qlen + n > 256 * 1024) return;   /* it is not reading: dropped */
    if (d.qlen + n > d.qcap) {
        d.qcap = (d.qlen + n) * 2;
        d.queue = realloc(d.queue, d.qcap);
    }
    memcpy(d.queue + d.qlen, line, n);
    d.qlen += n;
    flush();
}

static void clean(char *out, size_t n, const char *s)
{
    snprintf(out, n, "%s", s ? s : "");
    for (char *p = out; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
}

static struct wlr_box box_of(struct toplevel *t)
{
    struct wlr_box b = { t->tree->node.x, t->tree->node.y, t->fw, t->fh };
    if (!t->ssd) { struct wlr_box g = view_geo(t); b.width = g.width; b.height = g.height; }
    return b;
}

void desktop_event(struct toplevel *t, const char *name)
{
    if (d.in_fd < 0 || desktop_is(view_client(t))) return;   /* WPS's own raise theirs */
    char app[256], title[512], line[1024];
    clean(app, sizeof app, view_app(t));
    clean(title, sizeof title, view_title(t));
    struct wlr_box b = box_of(t);
    snprintf(line, sizeof line, "%s\t%s\t%s\t%d\t%d\t%d\t%d\n", name, app, title, b.x, b.y, b.width, b.height);
    send_line(line);
}

/* ------------------------------------------------------------- app icons */

static struct icon { char *app, *png; bool asked; } *icons;
static size_t nicons;

static bool exists(const char *p) { return access(p, R_OK) == 0; }

static void quiet(void *arg)
{
    (void)arg;
    int nul = open("/dev/null", O_RDWR);
    dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
}

static bool desktop_file_icon(const char *app, char *out, size_t n)
{
    const char *x = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char user[PATH_MAX];
    if (x && *x) snprintf(user, sizeof user, "%s", x);
    else snprintf(user, sizeof user, "%s/.local/share", home ? home : "/nonexistent");
    const char *dirs[] = { user, "/usr/local/share", "/usr/share", "/var/lib/flatpak/exports/share" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/applications/%s.desktop", dirs[i], app);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char line[1024];
        bool found = false, entry = false;
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '[') entry = !strncmp(line, "[Desktop Entry]", 15);
            else if (entry && !strncmp(line, "Icon=", 5)) {
                snprintf(out, n, "%s", line + 5);
                out[strcspn(out, "\r\n")] = 0;
                found = true;
                break;
            }
        }
        fclose(f);
        if (found) return true;
    }
    snprintf(out, n, "%s", app);   /* many apps' icons are named after their ID */
    return true;
}

static bool theme_file(const char *name, char *png, size_t n, char *svg, size_t m)
{
    *png = *svg = 0;
    if (name[0] == '/') {
        size_t l = strlen(name);
        if (l > 4 && !strcmp(name + l - 4, ".png")) snprintf(png, n, "%s", name);
        else snprintf(svg, m, "%s", name);
        return exists(name);
    }
    static const char *const sizes[] = { "64x64", "48x48", "128x128", "256x256", "96x96", "32x32" };
    const char *themes[] = { "hicolor", "Adwaita", "AdwaitaLegacy" };
    const char *x = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char user[PATH_MAX];
    if (x && *x) snprintf(user, sizeof user, "%s/icons", x);
    else snprintf(user, sizeof user, "%s/.local/share/icons", home ? home : "/nonexistent");
    const char *roots[] = { user, "/usr/share/icons", "/var/lib/flatpak/exports/share/icons" };
    char p[PATH_MAX];
    for (size_t r = 0; r < 3; r++)
        for (size_t t = 0; t < 3; t++) {
            for (size_t z = 0; z < sizeof sizes / sizeof *sizes; z++) {
                snprintf(p, sizeof p, "%s/%s/%s/apps/%s.png", roots[r], themes[t], sizes[z], name);
                if (exists(p)) { snprintf(png, n, "%s", p); return true; }
            }
            snprintf(p, sizeof p, "%s/%s/scalable/apps/%s.svg", roots[r], themes[t], name);
            if (!*svg && exists(p)) snprintf(svg, m, "%s", p);
        }
    snprintf(p, sizeof p, "/usr/share/pixmaps/%s.png", name);
    if (exists(p)) { snprintf(png, n, "%s", p); return true; }
    return *svg != 0;
}

static void resend_all(void);

static int icons_ready(void *data)
{
    (void)data;
    resend_all();   /* made by now (or not: then still none) */
    return 0;
}

/* The PNG for an app's icon ("" while there is none, or it is being made). */
static const char *icon_for(const char *app)
{
    if (!app || !*app || !strncmp(app, "org.wps.", 8)) return "";   /* WPS's own: WPS's icons */
    for (size_t i = 0; i < nicons; i++)
        if (!strcmp(icons[i].app, app)) {
            if (!icons[i].png[0] || exists(icons[i].png)) return icons[i].png;
            return "";
        }
    icons = realloc(icons, (nicons + 1) * sizeof *icons);
    struct icon *ic = &icons[nicons++];
    ic->app = strdup(app);
    ic->png = strdup("");
    ic->asked = true;
    char name[512], png[PATH_MAX], svg[PATH_MAX];
    desktop_file_icon(app, name, sizeof name);
    if (!theme_file(name, png, sizeof png, svg, sizeof svg)) return "";
    if (*png) { free(ic->png); ic->png = strdup(png); return ic->png; }
    /* an SVG: made into a PNG once, by glycin (the compositor never waits) */
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char dir[PATH_MAX], out[PATH_MAX + 64], safe[256];
    snprintf(dir, sizeof dir, "%s/wps-icons", rt ? rt : "/tmp");
    mkdir(dir, 0700);
    clean(safe, sizeof safe, app);
    for (char *p = safe; *p; p++) if (*p == '/' || *p == ' ') *p = '_';
    snprintf(out, sizeof out, "%s/%s.png", dir, safe);
    free(ic->png);
    ic->png = strdup(out);
    if (exists(out)) return ic->png;
    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof uri, "file://%s", svg);
    char *argv[] = { "glycin-thumbnailer", "-i", uri, "-o", out, "-s", "64", NULL };
    comp_spawn(argv, quiet, NULL);
    struct wl_event_source *t = wl_event_loop_add_timer(server.loop, icons_ready, NULL);
    wl_event_source_timer_update(t, 1500);   /* (a few timers at most: one per app) */
    return "";
}

/* -------------------------------------------------------- window lines */

static bool listable(struct toplevel *t)
{
    if (!t->mapped || !view_app(t)) return t->mapped;
    const char *a = view_app(t);
    bool wps_own = desktop_is(view_client(t));
    return !(wps_own && (!strcmp(a, "org.wps.Desktop") || !strcmp(a, "org.wps.WindowList") || !strcmp(a, "org.wps.LaunchPad") ||
                         !strcmp(a, "org.wps.WarpCenter") || !strcmp(a, "org.wps.Notice")));
}

void desktop_window(struct toplevel *t, const char *change)
{
    if (d.in_fd < 0) return;
    char line[2048];
    if (!strcmp(change, "remove") || !listable(t)) {
        if (!t->listed) return;
        t->listed = false;
        free(t->sent);
        t->sent = NULL;
        snprintf(line, sizeof line, "W\tremove\t%u\t\t\t0\t0\t\t0\n", t->id);
        send_line(line);
        return;
    }
    char app[256], title[512], fields[1536];
    clean(app, sizeof app, view_app(t));
    clean(title, sizeof title, view_title(t));
    /* (then where it is: x, y of its frame, and its size, for the desktop
     * to put its own windows back at the next login) */
    struct wlr_box g = comp_window_geometry(t);
    snprintf(fields, sizeof fields, "%u\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d", t->id, app, title, t->minimized, t->activated,
             icon_for(view_app(t)), t->maximized, g.x, g.y, g.width, g.height);
    if (t->sent && !strcmp(t->sent, fields)) return;   /* nothing new */
    snprintf(line, sizeof line, "W\t%s\t%s\n", t->listed ? "change" : "add", fields);
    t->listed = true;
    free(t->sent);
    t->sent = strdup(fields);
    send_line(line);
}

static void resend_all(void)
{
    struct toplevel *t;
    wl_list_for_each(t, &server.toplevels, link) {
        free(t->sent);
        t->sent = NULL;
        if (t->mapped) desktop_window(t, "change");
    }
}

bool desktop_key(const char *what)
{
    if (d.in_fd < 0) return false;
    char line[64];
    snprintf(line, sizeof line, "K\t%s\n", what);
    send_line(line);
    return true;
}

void desktop_input_source(int index, int count, bool ibus, const char *name)
{
    if (d.in_fd < 0) return;
    char line[256];
    snprintf(line, sizeof line, "I\t%d\t%d\t%s\t%s\n", index, count, ibus ? "ibus" : "xkb", name);
    send_line(line);
}

/* ------------------------------------------------------------ commands */

static struct toplevel *by_id(const char *id)
{
    unsigned n = (unsigned)strtoul(id, NULL, 10);
    struct toplevel *t;
    wl_list_for_each(t, &server.toplevels, link) if (t->id == n) return t;
    return NULL;
}

static void command(char *line)
{
    char *tab = strchr(line, '\t');
    const char *arg = tab ? tab + 1 : "";
    if (tab) *tab = 0;
    DBG("desktop command %s %s\n", line, arg);
    if (!strcmp(line, "shutdown")) {
        /* the desktop asked already (its Shut Down dialog) */
        if (!strcmp(arg, "logout")) { wl_display_terminate(server.display); return; }
        if (getenv("WPS_SESSION")) {   /* the WPS session: the machine too */
            wl_display_terminate(server.display);
            char *argv[] = { "systemctl", !strcmp(arg, "reboot") ? "reboot" : "poweroff", NULL };
            comp_spawn(argv, NULL, NULL);
        } else {
            fprintf(stderr, "wps-compositor: %s: only in the WPS session (nested, it ends only this)\n", arg);
            wl_display_terminate(server.display);
        }
        return;
    }
    if (!strcmp(line, "lock")) { lock_screen(); return; }
    if (!strcmp(line, "keyboard")) {
        if (!strcmp(arg, "next")) keymap_cycle(1);
        else if (!strcmp(arg, "prev")) keymap_cycle(-1);
        else if (!strcmp(arg, "report")) keymap_report();
        else keymap_select(atoi(arg));
        return;
    }
    if (!strcmp(line, "panel")) { comp_panel_edge(!strcmp(arg, "top")); return; }
    if (!strcmp(line, "place")) {   /* "place APP TITLE X Y": the desktop's next such window goes there */
        char app[128], title[256] = "";
        int x, y;
        const char *t1 = strchr(arg, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2 || (size_t)(t1 - arg) >= sizeof app) return;
        snprintf(app, sizeof app, "%.*s", (int)(t1 - arg), arg);
        snprintf(title, sizeof title, "%.*s", (int)(t2 - t1 - 1), t1 + 1);
        if (sscanf(t2 + 1, "%d\t%d", &x, &y) == 2) comp_place_next(app, title, x, y);
        return;
    }
    if (!strcmp(line, "find")) return;   /* WPS's own Find answers it */
    /* the desktop's scheme, font and frame options: the frames follow */
    if (!strcmp(line, "scheme")) { if (ui_scheme_use(arg)) comp_redraw_frames(); return; }
    if (!strcmp(line, "frame")) {
        char key[64];
        const char *eq = strchr(arg, '=');
        if (!eq || (size_t)(eq - arg) >= sizeof key) return;
        snprintf(key, sizeof key, "%.*s", (int)(eq - arg), arg);
        if (ui_scheme_set_option(key, eq + 1)) comp_redraw_frames();
        return;
    }
    /* the Mouse object's settings (its keys for copy, shadow and the drop
     * menu show on the pointer during a drag) */
    if (!strcmp(line, "mouse")) {
        char key[64];
        const char *eq = strchr(arg, '=');
        if (!eq || (size_t)(eq - arg) >= sizeof key) return;
        snprintf(key, sizeof key, "%.*s", (int)(eq - arg), arg);
        ui_mouse_set(key, eq + 1);
        return;
    }
    if (!strcmp(line, "font")) {
        double px = atof(arg);
        const char *fam = strchr(arg, ' ');
        if (px > 0 && fam) { ui_text_set_font(fam + 1, px); comp_redraw_frames(); }
        return;
    }
    struct toplevel *t = by_id(arg);
    if (!t) return;
    if (!strcmp(line, "activate")) comp_focus(t);
    else if (!strcmp(line, "minimize")) comp_minimize(t, true);
    else if (!strcmp(line, "maximize")) { comp_maximize(t, true); comp_focus(t); }
    else if (!strcmp(line, "restore")) {
        if (t->minimized) comp_minimize(t, false);
        else if (t->maximized) comp_maximize(t, false);
        comp_focus(t);
    } else if (!strcmp(line, "close")) comp_close(t);
    else if (!strcmp(line, "terminate")) {
        /* an X11 window: its own process (_NET_WM_PID), never XWayland's,
         * which would end every X11 program */
        pid_t pid = 0;
        if (t->xw) pid = t->xw->pid;
        else wl_client_get_credentials(view_client(t), &pid, NULL, NULL);
        DBG("terminate %u: pid %d\n", t->id, (int)pid);
        if (pid > 0 && pid != getpid() && !desktop_is(view_client(t))) kill(pid, SIGKILL);
        else if (t->xw) fprintf(stderr, "wps-compositor: terminate: the X11 program gives no process ID; closing instead\n"), comp_close(t);
    }
}

static void stopped(void);

static int on_readable(int fd, uint32_t mask, void *data)
{
    (void)data;
    ssize_t n = read(fd, d.rbuf + d.rlen, sizeof d.rbuf - 1 - d.rlen);
    if (n <= 0 && !(n < 0 && errno == EAGAIN)) { stopped(); return 0; }
    if (n > 0) d.rlen += (size_t)n;
    d.rbuf[d.rlen] = 0;
    char *line = d.rbuf, *nl;
    while ((nl = strchr(line, '\n'))) {
        *nl = 0;
        command(line);
        line = nl + 1;
    }
    d.rlen = strlen(line);
    memmove(d.rbuf, line, d.rlen + 1);
    if (d.rlen == sizeof d.rbuf - 1) d.rlen = 0;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) stopped();
    return 0;
}

/* -------------------------------------------------------- start, again */

static void start(void);

static int on_restart(void *data) { (void)data; start(); return 0; }

static void stopped(void)
{
    if (d.out_src) { wl_event_source_remove(d.out_src); d.out_src = NULL; }
    if (d.in_src) { wl_event_source_remove(d.in_src); d.in_src = NULL; }
    if (d.out_fd >= 0) close(d.out_fd);
    if (d.in_fd >= 0) close(d.in_fd);
    d.out_fd = d.in_fd = -1;
    d.qlen = 0;
    if (d.stopping) return;
    /* quickly again if it ran a while; longer while it keeps failing */
    d.delay_ms = now_ms() - d.started > 30000 ? 1000 : d.delay_ms ? (d.delay_ms * 2 > 30000 ? 30000 : d.delay_ms * 2) : 1000;
    fprintf(stderr, "wps-compositor: the desktop ended; started again in %d ms\n", d.delay_ms);
    if (!d.restart) d.restart = wl_event_loop_add_timer(server.loop, on_restart, NULL);
    wl_event_source_timer_update(d.restart, d.delay_ms);
}

static void on_client_destroy(struct wl_listener *l, void *data)
{
    (void)l; (void)data;
    wl_list_remove(&d.client_destroy.link);
    d.client = NULL;
}

/* In the desktop's process: its standard input and output are the pipes,
 * and its Wayland connection the socket. */
static void child_fds(void *arg)
{
    int *fds = arg;
    dup2(fds[0], 0);
    dup2(fds[1], 1);
    int fd = dup(fds[2]);   /* without close-on-exec */
    char num[16];
    snprintf(num, sizeof num, "%d", fd);
    setenv("WAYLAND_SOCKET", num, 1);
}

static void start(void)
{
    int sv[2], in[2], out[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) || pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC)) {
        fprintf(stderr, "wps-compositor: cannot start the desktop: %s\n", strerror(errno));
        return;
    }
    d.started = now_ms();
    int fds[3] = { in[0], out[1], sv[1] };
    char *cmd;
    if (asprintf(&cmd, "exec %s --events-stdin", d.command) < 0) cmd = NULL;
    char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    bool ok = cmd && comp_spawn(argv, child_fds, fds);
    free(cmd);
    close(sv[1]);
    close(in[0]);
    close(out[1]);
    if (!ok) { close(sv[0]); close(in[1]); close(out[0]); return; }
    d.client = wl_client_create(server.display, sv[0]);
    if (d.client) {
        d.client_destroy.notify = on_client_destroy;
        wl_client_add_destroy_listener(d.client, &d.client_destroy);
    }
    d.in_fd = in[1];
    d.out_fd = out[0];
    fcntl(d.in_fd, F_SETFL, fcntl(d.in_fd, F_GETFL) | O_NONBLOCK);
    fcntl(d.out_fd, F_SETFL, fcntl(d.out_fd, F_GETFL) | O_NONBLOCK);
    d.out_src = wl_event_loop_add_fd(server.loop, d.out_fd, WL_EVENT_READABLE, on_readable, NULL);
    fprintf(stderr, "wps-compositor: started the desktop: %s\n", d.command);
    /* what is open already */
    struct toplevel *t;
    wl_list_for_each(t, &server.toplevels, link) {
        t->listed = false;
        free(t->sent);
        t->sent = NULL;
        if (t->mapped) desktop_window(t, "add");
    }
}

void desktop_start(const char *command)
{
    free(d.command);
    d.command = strdup(command);
    start();
}

void desktop_stop(void)
{
    d.stopping = true;
    pid_t pid = 0;   /* (not our child: its own connection says who it is) */
    if (d.client) wl_client_get_credentials(d.client, &pid, NULL, NULL);
    if (pid > 0) kill(pid, SIGTERM);
    stopped();
}
