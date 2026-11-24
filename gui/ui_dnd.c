/*
 * Drag and drop through Wayland's data device, both ways (gui-design.md,
 * Drag and drop), and the file-descriptor watching it needs: the same
 * program may be the source and the target of a drag, so data is written
 * and read without blocking, from the main loop.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ui_internal.h"

/* ------------------------------------------------------------ fd watches */

struct watch { int fd; short events; void (*fn)(void *, int, short); void *arg; bool dead; };
static struct watch *watches;
static size_t nwatches, capwatches;

void ui_app_watch_fd(ui_app *a, int fd, short events, void (*fn)(void *arg, int fd, short revents), void *arg)
{
    (void)a;
    for (size_t i = 0; i < nwatches; i++)
        if (watches[i].fd == fd && !watches[i].dead) { watches[i].events = events; watches[i].fn = fn; watches[i].arg = arg; return; }
    if (nwatches == capwatches) {
        capwatches = capwatches ? capwatches * 2 : 8;
        watches = realloc(watches, capwatches * sizeof *watches);
    }
    watches[nwatches++] = (struct watch){ fd, events, fn, arg, false };
}

void ui_app_unwatch_fd(ui_app *a, int fd)
{
    (void)a;
    for (size_t i = 0; i < nwatches; i++)
        if (watches[i].fd == fd) watches[i].dead = true;   /* removed after this turn */
}

size_t ui_watch_fill(struct pollfd *pf, size_t max)
{
    size_t k = 0;
    for (size_t i = 0; i < nwatches; i++) {
        if (watches[i].dead) continue;
        if (k < max) pf[k++] = (struct pollfd){ watches[i].fd, watches[i].events, 0 };
    }
    return k;
}

void ui_watch_dispatch(const struct pollfd *pf, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        if (!pf[k].revents) continue;
        for (size_t i = 0; i < nwatches; i++)
            if (!watches[i].dead && watches[i].fd == pf[k].fd) { watches[i].fn(watches[i].arg, pf[k].fd, pf[k].revents); break; }
    }
    size_t j = 0;
    for (size_t i = 0; i < nwatches; i++)
        if (!watches[i].dead) watches[j++] = watches[i];
    nwatches = j;
}

/* Write `data` to fd without blocking, then close it. */
struct awrite { int fd; char *data; size_t len, off; };

static void awrite_ready(void *arg, int fd, short rev)
{
    struct awrite *w = arg;
    (void)rev;
    while (w->off < w->len) {
        ssize_t n = write(fd, w->data + w->off, w->len - w->off);
        if (n < 0 && errno == EAGAIN) return;   /* more when the reader drains */
        if (n <= 0) break;
        w->off += (size_t)n;
    }
    ui_app_unwatch_fd(NULL, fd);
    close(fd);
    free(w->data);
    free(w);
}

void ui_app_write_async(ui_app *a, int fd, const char *data, size_t len)
{
    struct awrite *w = calloc(1, sizeof *w);
    w->fd = fd;
    w->data = malloc(len ? len : 1);
    memcpy(w->data, data, len);
    w->len = len;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    ui_app_watch_fd(a, fd, POLLOUT, awrite_ready, w);
}

/* ---------------------------------------------------------------- source */

struct drag {
    ui_app *app;
    struct wl_data_source *src;
    struct wl_surface *icon;
    struct wl_buffer *icon_buf;
    ui_drag_send_fn send;
    ui_drag_end_fn end;
    void *ctx;
    uint32_t action;
    bool dropped;
};

static void drag_free(struct drag *d)
{
    wl_data_source_destroy(d->src);
    if (d->icon) wl_surface_destroy(d->icon);
    if (d->icon_buf) wl_buffer_destroy(d->icon_buf);
    free(d);
}

static void s_target(void *x, struct wl_data_source *s, const char *mime) { (void)x; (void)s; (void)mime; }
static void s_send(void *x, struct wl_data_source *s, const char *mime, int32_t fd)
{
    (void)s;
    struct drag *d = x;
    d->send(d->ctx, mime, fd);   /* the callback writes (ui_app_write_async) and closes */
}
static void s_cancelled(void *x, struct wl_data_source *s)
{
    (void)s;
    struct drag *d = x;
    if (d->end) d->end(d->ctx, d->dropped, d->action);
    drag_free(d);
}
static void s_drop_performed(void *x, struct wl_data_source *s) { (void)s; ((struct drag *)x)->dropped = true; }
static void s_finished(void *x, struct wl_data_source *s)
{
    (void)s;
    struct drag *d = x;
    if (d->end) d->end(d->ctx, true, d->action);
    drag_free(d);
}
static void s_action(void *x, struct wl_data_source *s, uint32_t a) { (void)s; ((struct drag *)x)->action = a; }
static const struct wl_data_source_listener source_listener = { s_target, s_send, s_cancelled, s_drop_performed, s_finished, s_action };

/* A small drag image: the item's icon, and how many are dragged. */
static struct wl_buffer *icon_buffer(ui_app *a, cairo_surface_t *img, int *w, int *h)
{
    *w = cairo_image_surface_get_width(img);
    *h = cairo_image_surface_get_height(img);
    int stride = cairo_image_surface_get_stride(img);
    size_t size = (size_t)stride * (size_t)*h;
    int fd = memfd_create("wps-drag", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) { if (fd >= 0) close(fd); return NULL; }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return NULL; }
    cairo_surface_flush(img);
    memcpy(p, cairo_image_surface_get_data(img), size);
    munmap(p, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, *w, *h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

bool ui_drag_start(ui_surface *origin, uint32_t serial, const char *const *mimes, size_t n, uint32_t actions,
                   cairo_surface_t *icon, int icon_scale, ui_drag_send_fn send, ui_drag_end_fn end, void *ctx)
{
    ui_app *a = origin->app;
    if (!a->ddm || !a->ddev) return false;
    struct drag *d = calloc(1, sizeof *d);
    d->app = a;
    d->send = send;
    d->end = end;
    d->ctx = ctx;
    d->src = wl_data_device_manager_create_data_source(a->ddm);
    for (size_t i = 0; i < n; i++) wl_data_source_offer(d->src, mimes[i]);
    if (a->ddm_version >= 3) wl_data_source_set_actions(d->src, actions);
    wl_data_source_add_listener(d->src, &source_listener, d);
    if (icon) {
        int w, h;
        d->icon_buf = icon_buffer(a, icon, &w, &h);
        if (d->icon_buf) {
            d->icon = wl_compositor_create_surface(a->comp);
            wl_surface_set_buffer_scale(d->icon, icon_scale);
            wl_surface_attach(d->icon, d->icon_buf, 0, 0);
            wl_surface_damage_buffer(d->icon, 0, 0, w, h);
            wl_surface_commit(d->icon);
        }
    }
    wl_data_device_start_drag(a->ddev, d->src, origin->wl, d->icon, serial);
    return true;
}

/* ---------------------------------------------------------------- target */

struct offer {
    struct wl_data_offer *o;
    char *mimes[16];
    size_t n;
    uint32_t src_actions, action;
    uint32_t start_action;   /* the compositor's first choice, before any key */
};

static void o_offer(void *x, struct wl_data_offer *o, const char *mime)
{
    (void)o;
    struct offer *f = x;
    if (f->n < 16) f->mimes[f->n++] = strdup(mime);
}
static void o_source_actions(void *x, struct wl_data_offer *o, uint32_t a) { (void)o; ((struct offer *)x)->src_actions = a; }
static void o_action(void *x, struct wl_data_offer *o, uint32_t a)
{
    (void)o;
    struct offer *f = x;
    if (!f->start_action && a) f->start_action = a;
    f->action = a;
}
static const struct wl_data_offer_listener offer_listener = { o_offer, o_source_actions, o_action };

static void offer_free(struct offer *f)
{
    if (!f) return;
    if (f->o) wl_data_offer_destroy(f->o);
    for (size_t i = 0; i < f->n; i++) free(f->mimes[i]);
    free(f);
}

static void d_data_offer(void *x, struct wl_data_device *dd, struct wl_data_offer *o)
{
    (void)dd; (void)x;
    struct offer *f = calloc(1, sizeof *f);
    f->o = o;
    wl_data_offer_add_listener(o, &offer_listener, f);
}

/* Ask the surface under the drag what it would do, and tell the source. */
static void negotiate(ui_app *a, uint32_t serial)
{
    struct offer *f = a->drag_offer;
    ui_surface *s = a->drag_focus;
    if (!f || !s) return;
    const char *mime = NULL;
    uint32_t act = s->ops->drag_motion ? s->ops->drag_motion(s, a->drag_x, a->drag_y, (const char *const *)f->mimes, f->n,
                                                              f->src_actions, a->mods, &mime) : 0;
    if (getenv("OSD_DEBUG_DND"))
        fprintf(stderr, "dnd: motion at %.0f,%.0f mods %s%s%s, source offers %u, compositor chose %u, we answer %u\n",
                a->drag_x, a->drag_y, a->mods & UI_MOD_CTRL ? "ctrl " : "", a->mods & UI_MOD_SHIFT ? "shift " : "",
                a->mods & UI_MOD_ALT ? "alt" : "", f->src_actions, f->action, act);
    wl_data_offer_accept(f->o, serial, act ? mime : NULL);
    if (a->ddm_version >= 3)
        wl_data_offer_set_actions(f->o, act ? (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE |
                                               WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) & f->src_actions : 0,
                                  act);
    a->drag_mime = mime;
}

static void d_enter(void *x, struct wl_data_device *dd, uint32_t serial, struct wl_surface *s, wl_fixed_t px, wl_fixed_t py,
                    struct wl_data_offer *o)
{
    (void)dd;
    ui_app *a = x;
    offer_free(a->drag_offer);
    a->drag_offer = o ? wl_data_offer_get_user_data(o) : NULL;
    a->drag_focus = s ? wl_surface_get_user_data(s) : NULL;
    /* Our own popups and layer parts are not drop targets: their window is. */
    if (a->drag_focus && a->drag_focus->popup) a->drag_focus = NULL;
    double m = a->drag_focus ? a->drag_focus->m : 0;   /* buffer -> window coordinates */
    a->drag_x = wl_fixed_to_double(px) - m;
    a->drag_y = wl_fixed_to_double(py) - m;
    a->drag_serial = serial;
    negotiate(a, serial);
}

static void d_leave(void *x, struct wl_data_device *dd)
{
    (void)dd;
    ui_app *a = x;
    if (a->drag_focus && a->drag_focus->ops->drag_leave) a->drag_focus->ops->drag_leave(a->drag_focus);
    a->drag_focus = NULL;
    offer_free(a->drag_offer);
    a->drag_offer = NULL;
}

static void d_motion(void *x, struct wl_data_device *dd, uint32_t t, wl_fixed_t px, wl_fixed_t py)
{
    (void)dd; (void)t;
    ui_app *a = x;
    double m = a->drag_focus ? a->drag_focus->m : 0;
    a->drag_x = wl_fixed_to_double(px) - m;
    a->drag_y = wl_fixed_to_double(py) - m;
    negotiate(a, a->drag_serial);
}

/* The dropped data arrives through a pipe, read as the loop allows. */
struct incoming {
    ui_app *app;
    ui_surface *s;
    struct offer *f;
    char *mime;
    double x, y;
    uint32_t action, start_action;
    unsigned mods;
    char *data;
    size_t len, cap;
};

static void incoming_read(void *arg, int fd, short rev)
{
    struct incoming *in = arg;
    (void)rev;
    for (;;) {
        if (in->len + 4096 > in->cap) { in->cap = in->cap ? in->cap * 2 : 8192; in->data = realloc(in->data, in->cap); }
        ssize_t n = read(fd, in->data + in->len, in->cap - in->len - 1);
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) break;
        in->len += (size_t)n;
    }
    in->data[in->len] = 0;
    ui_app_unwatch_fd(in->app, fd);
    close(fd);
    /* The surface may have closed meanwhile. */
    bool alive = false;
    for (ui_surface *s = in->app->surfaces; s; s = s->next) alive |= s == in->s;
    uint32_t done = 0;
    if (alive && in->s->ops->drop)
        done = in->s->ops->drop(in->s, in->x, in->y, in->mime, in->data, in->len, in->action, in->start_action, in->mods);
    if (in->f && in->app->ddm_version >= 3 && done && in->action) {
        /* An "ask" drop (GNOME makes every drag with the right button one)
         * is final only once the target says what it chose; finishing
         * before that is a protocol error, which ends the connection. */
        if (in->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) wl_data_offer_set_actions(in->f->o, done, done);
        wl_data_offer_finish(in->f->o);
    }
    /* Otherwise destroying the offer (below) cancels it. */
    offer_free(in->f);
    free(in->mime);
    free(in->data);
    free(in);
}

static void d_drop(void *x, struct wl_data_device *dd)
{
    (void)dd;
    ui_app *a = x;
    struct offer *f = a->drag_offer;
    ui_surface *s = a->drag_focus;
    if (s && s->ops->drag_leave) s->ops->drag_leave(s);
    a->drag_offer = NULL;
    a->drag_focus = NULL;
    if (!f || !s || !a->drag_mime) { offer_free(f); return; }
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) { offer_free(f); return; }
    struct incoming *in = calloc(1, sizeof *in);
    in->app = a;
    in->s = s;
    in->f = f;
    in->mime = strdup(a->drag_mime);
    in->x = a->drag_x;
    in->y = a->drag_y;
    in->action = f->action;
    in->start_action = f->start_action;
    in->mods = a->mods;
    if (getenv("OSD_DEBUG_DND"))
        fprintf(stderr, "dnd: drop, compositor's action %u (it began with %u; 1 copy, 2 move, 4 ask), mods %s%s%s\n",
                f->action, f->start_action,
                a->mods & UI_MOD_CTRL ? "ctrl " : "", a->mods & UI_MOD_SHIFT ? "shift " : "", a->mods & UI_MOD_ALT ? "alt" : "");
    wl_data_offer_receive(f->o, in->mime, p[1]);
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    ui_app_watch_fd(a, p[0], POLLIN, incoming_read, in);
}

/* ---- the clipboard (Wayland's selection). A new selection with text is
 * read at once, in the background, and kept: a paste (Ctrl+V, Shift+Insert)
 * takes it as it is (the selection comes when a window of ours gets the
 * keyboard, before any key). Copying offers ours to every program. */

static char *clip_text;            /* the selection's text, read */
static struct offer *clip_offer;   /* the selection now (NULL: none, or ours) */
static struct wl_data_source *clip_source;
static char *clip_source_text;
static ui_app *clip_app;

struct clip_read { char *buf; size_t len, cap; struct offer *f; };

static void clip_read_ready(void *arg, int fd, short rev)
{
    struct clip_read *r = arg;
    char b[4096];
    ssize_t n = (rev & POLLIN) ? read(fd, b, sizeof b) : 0;
    if (n > 0 && r->len + (size_t)n < 1 << 20) {   /* (at most 1 MB of text) */
        if (r->len + (size_t)n + 1 > r->cap) {
            r->cap = (r->len + (size_t)n + 1) * 2;
            r->buf = realloc(r->buf, r->cap);
        }
        memcpy(r->buf + r->len, b, (size_t)n);
        r->len += (size_t)n;
        return;
    }
    if (n < 0 && errno == EAGAIN) return;
    ui_app_unwatch_fd(clip_app, fd);
    close(fd);
    if (r->f == clip_offer && r->buf) {   /* still the selection: kept */
        r->buf[r->len] = 0;
        free(clip_text);
        clip_text = r->buf;
        r->buf = NULL;
    }
    free(r->buf);
    free(r);
}

static void d_selection(void *x, struct wl_data_device *dd, struct wl_data_offer *o)
{
    (void)dd;
    ui_app *a = x;
    offer_free(clip_offer);
    clip_offer = o ? wl_data_offer_get_user_data(o) : NULL;
    if (!clip_offer || clip_source) return;   /* (none, or ours: clip_text is it already) */
    static const char *const want[] = { "text/plain;charset=utf-8", "text/plain;charset=UTF-8", "UTF8_STRING", "text/plain", "TEXT", "STRING" };
    const char *mime = NULL;
    for (size_t w = 0; w < sizeof want / sizeof *want && !mime; w++)
        for (size_t i = 0; i < clip_offer->n && !mime; i++)
            if (!strcmp(clip_offer->mimes[i], want[w])) mime = want[w];
    if (!mime) return;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return;
    wl_data_offer_receive(clip_offer->o, mime, p[1]);
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    struct clip_read *r = calloc(1, sizeof *r);
    r->f = clip_offer;
    clip_app = a;
    ui_app_watch_fd(a, p[0], POLLIN, clip_read_ready, r);
    wl_display_flush(a->dpy);
}

static void c_send(void *x, struct wl_data_source *s, const char *mime, int32_t fd)
{
    (void)x; (void)s; (void)mime;
    if (clip_source_text && clip_app) ui_app_write_async(clip_app, fd, clip_source_text, strlen(clip_source_text));
    else close(fd);
}

static void c_cancelled(void *x, struct wl_data_source *s)
{
    (void)x;
    if (s == clip_source) clip_source = NULL;   /* another program's selection now */
    wl_data_source_destroy(s);
}

static const struct wl_data_source_listener clip_listener = { s_target, c_send, c_cancelled, NULL, NULL, NULL };

void ui_clipboard_set(const char *text)
{
    ui_app *a = clip_app;
    free(clip_text);
    clip_text = strdup(text ? text : "");
    if (!a || !a->ddm || !a->ddev) return;
    if (clip_source) wl_data_source_destroy(clip_source);
    free(clip_source_text);
    clip_source_text = strdup(clip_text);
    clip_source = wl_data_device_manager_create_data_source(a->ddm);
    static const char *const offers[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING" };
    for (size_t i = 0; i < sizeof offers / sizeof *offers; i++) wl_data_source_offer(clip_source, offers[i]);
    wl_data_source_add_listener(clip_source, &clip_listener, NULL);
    wl_data_device_set_selection(a->ddev, clip_source, a->last_serial);
    wl_display_flush(a->dpy);
}

const char *ui_clipboard_text(void) { return clip_text; }

static const struct wl_data_device_listener device_listener = { d_data_offer, d_enter, d_leave, d_motion, d_drop, d_selection };

void ui_dnd_init(ui_app *a)
{
    if (!a->ddm || !a->seat) return;
    a->ddev = wl_data_device_manager_get_data_device(a->ddm, a->seat);
    wl_data_device_add_listener(a->ddev, &device_listener, a);
    clip_app = a;
}

void ui_dnd_renegotiate(ui_app *a) { negotiate(a, a->drag_serial); }

/* ------------------------------------------------- fonts on dialog windows */

static void (*font_drop_fn)(const char *spec, unsigned mods);
void ui_set_font_drop(void (*fn)(const char *spec, unsigned mods)) { font_drop_fn = fn; }

void ui_font_dropped(const char *spec, unsigned mods)
{
    if (font_drop_fn && ui_font_valid(spec)) font_drop_fn(spec, mods);
}

uint32_t ui_font_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n,
                             uint32_t src_actions, unsigned mods, const char **mime)
{
    (void)s; (void)x; (void)y; (void)src_actions; (void)mods;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(mimes[i], UI_MIME_FONT) && font_drop_fn) { *mime = mimes[i]; return UI_DND_COPY; }
    return 0;
}

uint32_t ui_font_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len,
                      uint32_t action, uint32_t start_action, unsigned mods)
{
    (void)s; (void)x; (void)y; (void)action; (void)start_action;
    if (strcmp(mime, UI_MIME_FONT)) return 0;
    char spec[200];
    snprintf(spec, sizeof spec, "%.*s", (int)(len < sizeof spec ? strcspn(data, "\r\n") : 0), data);
    if (!ui_font_valid(spec)) return 0;
    ui_font_dropped(spec, mods);
    return UI_DND_COPY;
}
