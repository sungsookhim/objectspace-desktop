/*
 * wps: WPS for scripts (docs/cli-design.md 2). Plain text out, one value a
 * line; exit status 0 on success, 1 otherwise (a message on stderr). The
 * same requests as wpsctl's, without its JSON:
 *
 *   wps open PATH...                 as a double click would
 *   wps get PATH NAME                a property's value
 *   wps set PATH 'NAME=VALUE;...'    properties (class:WPFolder, type:image/png too)
 *   wps create FOLDER CLASS TITLE    a new object; prints its path
 *   wps shadow PATH FOLDER           a shadow; prints its path
 *   wps say [--stay] TEXT            a message at the bottom of the desktop
 *   wps notify TITLE [TEXT]          a notification
 *   wps ask TEXT [BUTTON...]         a WPS dialog; prints the button chosen
 *   wps folder                       the folder it was started from ($WPS_FOLDER)
 *   wps selection                    the active folder window's selected objects
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include "cbor.h"

static int sock = -1;

static bool io(void *buf, size_t n, bool wr)
{
    unsigned char *b = buf;
    while (n) {
        ssize_t r = wr ? send(sock, b, n, MSG_NOSIGNAL) : read(sock, b, n);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r;
        n -= (size_t)r;
    }
    return true;
}

static void die(const char *fmt, const char *a)
{
    fprintf(stderr, "wps: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    exit(1);
}

static void connect_wpsd(void)
{
    char path[4096];
    const char *s = getenv("WPS_SOCKET"), *rt = getenv("WPS_RUNTIME_DIR"), *xr = getenv("XDG_RUNTIME_DIR");
    if (s && *s) snprintf(path, sizeof path, "%s", s);
    else if (rt && *rt) snprintf(path, sizeof path, "%s/wps.sock", rt);
    else if (xr && *xr) snprintf(path, sizeof path, "%s/wps.sock", xr);
    else snprintf(path, sizeof path, "/tmp/wps-%u/wps.sock", (unsigned)getuid());
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof sa.sun_path) die("socket path too long: %s", path);
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0 || connect(sock, (struct sockaddr *)&sa, sizeof sa) != 0) die("WPS is not running (no %s)", path);
}

/* One request (a map whose first pair is "op"), its reply's result. */
static cv reply;
static const cv *request(cbuf *b)
{
    unsigned char h[4] = { (unsigned char)(b->n >> 24), (unsigned char)(b->n >> 16), (unsigned char)(b->n >> 8), (unsigned char)b->n };
    if (!io(h, 4, true) || !io(b->d, b->n, true)) die("%s", "lost the connection to WPS");
    cb_free(b);
    for (;;) {
        if (!io(h, 4, false)) die("%s", "lost the connection to WPS");
        size_t n = (size_t)h[0] << 24 | (size_t)h[1] << 16 | (size_t)h[2] << 8 | h[3];
        unsigned char *buf = malloc(n ? n : 1);
        if (!buf || !io(buf, n, false)) die("%s", "lost the connection to WPS");
        int rc = cv_decode(buf, n, &reply);
        free(buf);
        if (rc) die("%s", "a reply WPS could not read");
        int64_t id;
        if (!cv_int(&reply, "id", &id) || id != 1) { cv_free(&reply); continue; }   /* (events) */
        const cv *ok = cv_get(&reply, "ok");
        if (!ok || ok->k != CV_BOOL || !ok->b) {
            const char *m = cv_text(&reply, "message");
            die("%s", m ? m : cv_text(&reply, "error") ? cv_text(&reply, "error") : "it was refused");
        }
        return cv_get(&reply, "result");
    }
}

static void begin(cbuf *b, const char *op, size_t pairs)
{
    cb_init(b);
    cb_map(b, 2 + pairs);
    cb_text(b, "id"); cb_int(b, 1);
    cb_text(b, "op"); cb_text(b, op);
}

/* A path as wpsd takes it: absolute; class: and type: scopes as given. */
static const char *abs_path(const char *p, char *out, size_t n)
{
    if (p[0] == '/' || !strncmp(p, "class:", 6) || !strncmp(p, "type:", 5) || !strncmp(p, "vobj:", 5)) return p;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return p;
    if (!strcmp(p, ".")) snprintf(out, n, "%s", cwd);
    else snprintf(out, n, "%s/%s", cwd, p);
    return out;
}

static void print_path(const cv *res)
{
    if (res && res->k == CV_TEXT) { puts(res->s); return; }   /* (a shadow: its link's path) */
    const cv *o = res && res->k == CV_MAP ? (cv_get(res, "object") ? cv_get(res, "object") : res) : NULL;
    const char *p = o ? cv_text(o, "path") : NULL;
    if (!p || !*p) p = o ? cv_text(o, "address") : NULL;
    if (p) puts(p);
}

static int open_one(const char *target)
{
    char a[PATH_MAX];
    const char *t = abs_path(target, a, sizeof a);
    /* on this session's display (wpsd may have started before it) */
    char disp[4096] = "";
    const char *wd = getenv("WAYLAND_DISPLAY"), *rt = getenv("XDG_RUNTIME_DIR"), *x11 = getenv("DISPLAY");
    if (wd && *wd) snprintf(disp, sizeof disp, "%s%s%s", wd[0] == '/' || !rt ? "" : rt, wd[0] == '/' || !rt ? "" : "/", wd);
    bool x = x11 && *x11;
    cbuf b;
    begin(&b, "open", 2 + (disp[0] ? 1 : 0) + (x ? 1 : 0));
    cb_text(&b, "target"); cb_text(&b, t);
    cb_text(&b, "confirm"); cb_bool(&b, false);
    if (disp[0]) { cb_text(&b, "display"); cb_text(&b, disp); }
    if (x) { cb_text(&b, "x11"); cb_text(&b, x11); }
    const cv *res = request(&b);
    const char *folder = res && res->k == CV_MAP ? cv_text(res, "folder") : NULL;
    if (res && res->k == CV_MAP && cv_get(res, "needs_confirmation")) die("%s: owned by another user; open it from its folder (WPS asks first)", t);
    if (folder) {   /* a folder: its window, by the desktop */
        char f[PATH_MAX];
        snprintf(f, sizeof f, "%s", folder);
        cv_free(&reply);
        begin(&b, "show", 1);
        cb_text(&b, "target"); cb_text(&b, f);
        request(&b);
    }
    cv_free(&reply);
    return 0;
}

static int notify(const char *title, const char *text)
{
    sd_bus *bus = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL, *r = NULL;
    int e = sd_bus_open_user(&bus);
    if (e >= 0) e = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                   "org.freedesktop.Notifications", "Notify");
    if (e >= 0) e = sd_bus_message_append(m, "susss", "wps", 0u, "utilities-terminal", title, text ? text : "");
    if (e >= 0) e = sd_bus_message_append(m, "as", 0);
    if (e >= 0) e = sd_bus_message_append(m, "a{sv}", 0);
    if (e >= 0) e = sd_bus_message_append(m, "i", -1);
    if (e >= 0) e = sd_bus_call(bus, m, 5000000, &err, &r);
    if (e < 0) fprintf(stderr, "wps: no notification: %s\n", err.message ? err.message : strerror(-e));
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    sd_bus_message_unref(r);
    sd_bus_unref(bus);
    return e < 0;
}

static void usage(void)
{
    fputs("usage: wps open PATH... | get PATH NAME | set PATH 'NAME=VALUE;...' | create FOLDER CLASS TITLE\n"
          "       wps shadow PATH FOLDER | say [--stay] TEXT | notify TITLE [TEXT] | ask TEXT [BUTTON...]\n"
          "       wps folder | selection\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2) usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "--version")) { printf("wps %s\n", WPS_VERSION); return 0; }
    if (!strcmp(cmd, "folder")) {
        const char *f = getenv("WPS_FOLDER");
        if (!f || !*f) { fprintf(stderr, "wps: not started from a folder (WPS_FOLDER is not set)\n"); return 1; }
        puts(f);
        return 0;
    }
    if (!strcmp(cmd, "notify")) {
        if (argc < 3) usage();
        return notify(argv[2], argc > 3 ? argv[3] : NULL);
    }
    connect_wpsd();
    cbuf b;
    char a[PATH_MAX], a2[PATH_MAX];
    if (!strcmp(cmd, "open")) {
        if (argc < 3) usage();
        for (int i = 2; i < argc; i++) open_one(argv[i]);
        return 0;
    }
    if (!strcmp(cmd, "get")) {
        if (argc != 4) usage();
        begin(&b, "get", 2);
        cb_text(&b, "target"); cb_text(&b, abs_path(argv[2], a, sizeof a));
        cb_text(&b, "name"); cb_text(&b, argv[3]);
        const cv *res = request(&b);
        if (res && res->k == CV_TEXT) puts(res->s);
        return 0;
    }
    if (!strcmp(cmd, "set")) {
        if (argc != 4) usage();
        begin(&b, "set", 2);
        cb_text(&b, "target"); cb_text(&b, abs_path(argv[2], a, sizeof a));
        cb_text(&b, "setup"); cb_text(&b, argv[3]);
        request(&b);
        return 0;
    }
    if (!strcmp(cmd, "create")) {
        if (argc != 5) usage();
        begin(&b, "create", 3);
        cb_text(&b, "folder"); cb_text(&b, abs_path(argv[2], a, sizeof a));
        cb_text(&b, "class"); cb_text(&b, argv[3]);
        cb_text(&b, "title"); cb_text(&b, argv[4]);
        print_path(request(&b));
        return 0;
    }
    if (!strcmp(cmd, "shadow")) {
        if (argc != 4) usage();
        begin(&b, "shadow", 2);
        cb_text(&b, "target"); cb_text(&b, abs_path(argv[2], a, sizeof a));
        cb_text(&b, "folder"); cb_text(&b, abs_path(argv[3], a2, sizeof a2));
        print_path(request(&b));
        return 0;
    }
    if (!strcmp(cmd, "say")) {
        bool stay = argc > 2 && !strcmp(argv[2], "--stay");
        if (argc != (stay ? 4 : 3)) usage();
        begin(&b, "say", 2);
        cb_text(&b, "text"); cb_text(&b, argv[stay ? 3 : 2]);
        cb_text(&b, "stay"); cb_bool(&b, stay);
        request(&b);
        return 0;
    }
    if (!strcmp(cmd, "ask")) {
        if (argc < 3) usage();
        begin(&b, "ask", 2);
        cb_text(&b, "text"); cb_text(&b, argv[2]);
        cb_text(&b, "buttons");
        cb_array(&b, argc > 3 ? (size_t)(argc - 3) : 1);
        if (argc > 3) for (int i = 3; i < argc; i++) cb_text(&b, argv[i]);
        else cb_text(&b, "OK");
        const cv *res = request(&b);
        const char *v = res && res->k == CV_MAP ? cv_text(res, "value") : NULL;
        if (!v || !*v) return 1;   /* (closed without a choice) */
        puts(v);
        return 0;
    }
    if (!strcmp(cmd, "selection")) {
        if (argc != 2) usage();
        begin(&b, "selection", 0);
        const cv *res = request(&b), *ps = res && res->k == CV_MAP ? cv_get(res, "paths") : NULL;
        for (size_t i = 0; ps && ps->k == CV_ARRAY && i < ps->n; i++)
            if (ps->items[i].k == CV_TEXT) puts(ps->items[i].s);
        return 0;
    }
    usage();
}
