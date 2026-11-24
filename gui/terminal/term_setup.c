/* Objectspace Terminal's looks from Objectspace's settings objects (osdd's "setup"): the
 * scheme in use (the Scheme Palette's: colours, frame sizes), the System's
 * frame options, and its Terminal font. Asked once, as the window opens;
 * without osdd (started by hand, outside Objectspace) Classic and the defaults. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "cbor.h"
#include "term_setup.h"
#include "ui.h"

static bool io(int fd, void *buf, size_t n, bool wr)
{
    unsigned char *b = buf;
    while (n) {
        ssize_t r = wr ? send(fd, b, n, MSG_NOSIGNAL) : read(fd, b, n);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r;
        n -= (size_t)r;
    }
    return true;
}

static int connect_osdd(void)
{
    char path[4096];
    const char *s = getenv("OSD_SOCKET"), *rt = getenv("OSD_RUNTIME_DIR"), *xr = getenv("XDG_RUNTIME_DIR");
    if (s && *s) snprintf(path, sizeof path, "%s", s);
    else if (rt && *rt) snprintf(path, sizeof path, "%s/objectspace.sock", rt);
    else if (xr && *xr) snprintf(path, sizeof path, "%s/objectspace.sock", xr);
    else snprintf(path, sizeof path, "/tmp/objectspace-%u/objectspace.sock", (unsigned)getuid());
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof sa.sun_path) return -1;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval tv = { 2, 0 };   /* (a busy or stuck osdd: the defaults after 2 s) */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

/* A settings object's value: its "settings" map's key. */
static const char *setting(const cv *objects, const char *cls, const char *key)
{
    const cv *o = cv_get(objects, cls), *st = o ? cv_get(o, "settings") : NULL;
    return st ? cv_text(st, key) : NULL;
}

bool term_setup_read(char *font, size_t fn)
{
    int fd = connect_osdd();
    if (fd < 0) return false;
    cbuf b;
    cb_init(&b);
    cb_map(&b, 2);
    cb_text(&b, "id"); cb_int(&b, 1);
    cb_text(&b, "op"); cb_text(&b, "setup");
    unsigned char h[4] = { (unsigned char)(b.n >> 24), (unsigned char)(b.n >> 16), (unsigned char)(b.n >> 8), (unsigned char)b.n };
    bool ok = io(fd, h, 4, true) && io(fd, b.d, b.n, true);
    cb_free(&b);
    cv m = { 0 };
    bool got = false;
    for (int tries = 0; ok && !got && tries < 16; tries++) {   /* (events may come first) */
        if (!io(fd, h, 4, false)) break;
        size_t n = (size_t)h[0] << 24 | (size_t)h[1] << 16 | (size_t)h[2] << 8 | h[3];
        if (n > (64u << 20)) break;
        unsigned char *buf = malloc(n ? n : 1);
        if (!buf || !io(fd, buf, n, false)) { free(buf); break; }
        int rc = cv_decode(buf, n, &m);
        free(buf);
        if (rc) break;
        int64_t id;
        if (cv_int(&m, "id", &id) && id == 1) got = true;
        else cv_free(&m);
    }
    close(fd);
    if (!got) return false;
    const cv *res = cv_get(&m, "result"), *objs = res ? cv_get(res, "objects") : NULL, *sch = res ? cv_get(res, "schemes") : NULL;
    /* every scheme defined, the Scheme Palette's used */
    for (size_t i = 0; sch && sch->k == CV_ARRAY && i < sch->n; i++) {
        const char *t = cv_text(&sch->items[i], "title"), *v = cv_text(&sch->items[i], "values");
        if (t && *t && v) ui_scheme_define(t, "classic", v);
    }
    const char *use = objs ? setting(objs, "OsdSchemePalette", "scheme") : NULL;
    if (!use || !ui_scheme_use(use)) ui_scheme_use("classic");
    /* the System's frame options (corners, border, title layout ...) */
    const cv *so = objs ? cv_get(objs, "OsdSystem") : NULL, *st = so ? cv_get(so, "settings") : NULL;
    for (size_t i = 0; st && st->k == CV_MAP && i < st->n; i++) {
        const cv *k = &st->items[2 * i], *v = &st->items[2 * i + 1];
        if (k->k == CV_TEXT && v->k == CV_TEXT) ui_scheme_set_option(k->s, v->s);
    }
    const char *tf = objs ? setting(objs, "OsdSystem", "font_terminal") : NULL;
    if (tf && *tf && ui_font_valid(tf)) snprintf(font, fn, "%s", tf);
    cv_free(&m);
    return true;
}
