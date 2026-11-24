/* The Wayland side: connection, surfaces, buffers, damage-based drawing,
 * input. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "ui_internal.h"

double ui_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int ui_app_scale_override(void)
{
    const char *s = getenv("WPS_UI_SCALE");
    return s ? atoi(s) : 0;
}

/* ---------------------------------------------------------------- buffers */

static void buf_release(void *d, struct wl_buffer *wb)
{
    (void)wb;
    struct ui_buffer *b = d;
    b->busy = false;
}
static const struct wl_buffer_listener buf_listener = { buf_release };

static void buf_free(struct ui_buffer *b)
{
    if (b->wb) wl_buffer_destroy(b->wb);
    if (b->data) munmap(b->data, b->size);
    memset(b, 0, sizeof *b);
    b->frame = -1;
}

static struct ui_buffer *buf_get(ui_surface *s, int w, int h)
{
    for (int i = 0; i < UI_BUFFERS; i++) {
        struct ui_buffer *b = &s->bufs[i];
        if (b->busy) continue;
        if (b->wb && (b->w != w || b->h != h)) buf_free(b);
        if (!b->wb) {
            int stride = w * 4;
            b->size = (size_t)stride * (size_t)h;
            int fd = memfd_create("wps-ui", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, (off_t)b->size) < 0) { if (fd >= 0) close(fd); return NULL; }
            b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (b->data == MAP_FAILED) { b->data = NULL; close(fd); return NULL; }
            struct wl_shm_pool *pool = wl_shm_create_pool(s->app->shm, fd, (int32_t)b->size);
            b->wb = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
            wl_buffer_add_listener(b->wb, &buf_listener, b);
            wl_shm_pool_destroy(pool);
            close(fd);
            b->w = w;
            b->h = h;
            b->frame = -1;
        }
        return b;
    }
    return NULL;
}

/* ----------------------------------------------------------------- damage */

/* The buffer: the window and its shadow margin, in device pixels. */
static int buf_w(ui_surface *s) { return (int)lround((s->w + 2 * s->m) * s->scale); }
static int buf_h(ui_surface *s) { return (int)lround((s->h + 2 * s->m) * s->scale); }

static ui_irect to_dev(ui_surface *s, ui_rect r)
{
    int k = s->scale;
    r.x += s->m;   /* window coordinates -> buffer */
    r.y += s->m;
    int x0 = (int)floor(r.x * k), y0 = (int)floor(r.y * k);
    int x1 = (int)ceil((r.x + r.w) * k), y1 = (int)ceil((r.y + r.h) * k);
    ui_irect o = { x0, y0, x1 - x0, y1 - y0 };
    return o;
}

static void want_draw(ui_surface *s)
{
    s->needs_draw = true;
    uint64_t one = 1;
    ssize_t w = write(s->app->wake_fd, &one, sizeof one);   /* draw at the end of this loop turn */
    (void)w;
}

void ui_surface_damage(ui_surface *s, ui_rect r)
{
    ui_damage_add(&s->cur, to_dev(s, r), buf_w(s), buf_h(s));
    s->cur_any = true;
    want_draw(s);
}

void ui_surface_damage_all(ui_surface *s)
{
    s->cur.full = true;
    s->cur_any = true;
    want_draw(s);
}

void ui_surface_scroll(ui_surface *s, ui_rect area, double dy)
{
    int d = (int)lround(dy * s->scale);
    if (!d) return;
    ui_damage_scroll(&s->cur, to_dev(s, area), d);
    s->cur_any = true;
    want_draw(s);
}

void ui_surface_set_continuous(ui_surface *s, bool on)
{
    s->continuous = on;
    if (on) want_draw(s);
}

void ui_surface_stats(ui_surface *s, const ui_frame_stat **st, size_t *n)
{
    *st = s->stats;
    *n = s->nstats;
}

/* ---------------------------------------------------------------- drawing */

static void frame_done(void *d, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_listener = { frame_done };

static void move_area(struct ui_buffer *b, ui_irect a, int dy)
{
    unsigned char *px = b->data;
    size_t stride = (size_t)b->w * 4, row = (size_t)a.w * 4;
    if (a.x < 0 || a.y < 0 || a.x + a.w > b->w || a.y + a.h > b->h) return;
    if (dy > 0)
        for (int y = a.y; y < a.y + a.h - dy; y++)
            memcpy(px + (size_t)y * stride + (size_t)a.x * 4, px + (size_t)(y + dy) * stride + (size_t)a.x * 4, row);
    else
        for (int y = a.y + a.h - 1; y >= a.y - dy; y--)
            memcpy(px + (size_t)y * stride + (size_t)a.x * 4, px + (size_t)(y + dy) * stride + (size_t)a.x * 4, row);
}

/* Shadows (System object): a margin around a toplevel window that is not
 * maximized or frameless; none for menus. */
static double margin_for(ui_surface *s)
{
    if (!s->top || s->frameless || s->maximized) return 0;
    return ui_scheme_now.shadows == 2 ? 18 : ui_scheme_now.shadows == 1 ? 10 : 0;
}

/* The shadow in the margin: soft, a little below the window. */
static void draw_shadow(ui_surface *s, cairo_t *cr)
{
    double m = s->m, dy = m / 4, a = ui_scheme_now.shadows == 2 ? 0.42 : 0.26;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_line_width(cr, 1);
    for (double i = m; i >= 1; i--) {
        double f = 1 - i / m;
        cairo_set_source_rgba(cr, 0, 0, 0, a * f * f / 3);
        cairo_rectangle(cr, m - i + 0.5, m - i + dy + 0.5, s->w + 2 * i - 1, s->h + 2 * i - 1);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static void render(ui_surface *s)
{
    if (!s->configured || s->destroyed || s->frame_pending) return;
    double now = ui_now_ms();
    if (s->ops->frame) s->ops->frame(s, now);
    double m = margin_for(s);
    if (m != s->m) {   /* shadows switched, or maximized: everything again */
        s->m = m;
        s->cur.full = true;
        s->cur_any = true;
        if (s->layer) s->layer_dirty = true;
    }
    if (!s->cur_any && !s->continuous && !s->layer_dirty) { s->needs_draw = false; return; }
    int bw = buf_w(s), bh = buf_h(s);
    if (bw <= 0 || bh <= 0) return;
    struct ui_buffer *b = buf_get(s, bw, bh);
    if (!b) return;   /* all buffers with the compositor: try at the next release */
    double t0 = ui_now_ms();

    if (s->nhist == UI_DAMAGE_HISTORY) {
        memmove(s->hist, s->hist + 1, sizeof s->hist[0] * (UI_DAMAGE_HISTORY - 1));
        s->nhist--;
    }
    s->hist[s->nhist++] = s->cur;
    memset(&s->cur, 0, sizeof s->cur);
    s->cur_any = false;
    s->needs_draw = false;
    int age = b->frame < 0 ? 0 : (int)(s->frame_no - b->frame);
    ui_damage_plan plan;
    ui_damage_make_plan(s->hist, s->nhist, age, bw, bh, &plan);
    if (plan.move) move_area(b, plan.area, plan.dy);
    double t_moved = ui_now_ms();

    double drawn = 0;
    if (plan.nrects) {
        cairo_surface_t *cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32, bw, bh, bw * 4);
        cairo_surface_set_device_scale(cs, s->scale, s->scale);
        cairo_t *cr = cairo_create(cs);
        if (s->m > 0 && plan.full) draw_shadow(s, cr);
        cairo_translate(cr, s->m, s->m);   /* the window inside its margin */
        /* Each rectangle on its own: a scroll damages a thin strip and the
         * scroll bar, and their bounding box would be the whole list. */
        for (int i = 0; i < plan.nrects; i++) {
            ui_irect r = plan.rects[i];
            ui_rect clip = { (double)r.x / s->scale - s->m, (double)r.y / s->scale - s->m, (double)r.w / s->scale,
                             (double)r.h / s->scale };
            /* only the window: the margin holds the shadow */
            double x0 = fmax(clip.x, 0), y0 = fmax(clip.y, 0), x1 = fmin(clip.x + clip.w, s->w), y1 = fmin(clip.y + clip.h, s->h);
            if (x1 <= x0 || y1 <= y0) continue;
            clip = (ui_rect){ x0, y0, x1 - x0, y1 - y0 };
            cairo_save(cr);
            cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
            cairo_clip(cr);
            s->ops->draw(s, cr, clip);
            cairo_restore(cr);
            drawn += (double)r.w * r.h;
        }
        cairo_destroy(cr);
        cairo_surface_flush(cs);
        cairo_surface_destroy(cs);
    }
    /* What the compositor must update is what differs from the frame on
     * screen, which is this frame's changes alone; after a scroll, that is
     * the whole scrolled area, not only the strip redrawn. */
    const ui_damage_frame *now_f = &s->hist[s->nhist - 1];
    if (now_f->full) {
        wl_surface_damage_buffer(s->wl, 0, 0, bw, bh);
    } else {
        for (int i = 0; i < now_f->nrects; i++)
            wl_surface_damage_buffer(s->wl, now_f->rects[i].x, now_f->rects[i].y, now_f->rects[i].w, now_f->rects[i].h);
        if (now_f->scrolled)
            wl_surface_damage_buffer(s->wl, now_f->area.x, now_f->area.y, now_f->area.w, now_f->area.h);
    }
    double layer_drawn = 0;
    if (s->layer && s->layer_dirty) {
        layer_drawn = ui_layer_render(s->layer);
        if (layer_drawn < 0) layer_drawn = 0;   /* still dirty: drawn at the next frame */
    }
    double t1 = ui_now_ms();
    b->frame = s->frame_no++;
    b->busy = true;
    if (s->top && (s->geo_m != s->m || s->geo_w != s->w || s->geo_h != s->h)) {
        /* The window is the part inside the margin: what the compositor
         * moves, tiles and maximizes, and where input goes. */
        xdg_surface_set_window_geometry(s->xs, (int)s->m, (int)s->m, (int)s->w, (int)s->h);
        struct wl_region *in = wl_compositor_create_region(s->app->comp);
        wl_region_add(in, (int)s->m, (int)s->m, (int)s->w, (int)s->h);
        wl_surface_set_input_region(s->wl, in);
        wl_region_destroy(in);
        s->geo_m = s->m;
        s->geo_w = s->w;
        s->geo_h = s->h;
    }
    /* The window is opaque inside its margin (square corners, nothing
     * translucent): the compositor need not draw what is behind it, nor
     * blend it, which reads the screen back (slow on some screens: a
     * virtual machine's took seconds a frame). */
    bool opaque = (ui_scheme_now.corner_radius <= 0 || s->maximized || !s->top) && !ui_scheme_now.translucency;
    char key[64] = "";
    if (opaque) snprintf(key, sizeof key, "%d %d %d %d", (int)s->m, (int)s->m, (int)s->w, (int)s->h);
    if (strcmp(key, s->opaque_key)) {
        if (opaque) {
            struct wl_region *op = wl_compositor_create_region(s->app->comp);
            wl_region_add(op, (int)s->m, (int)s->m, (int)s->w, (int)s->h);
            wl_surface_set_opaque_region(s->wl, op);
            wl_region_destroy(op);
        } else {
            wl_surface_set_opaque_region(s->wl, NULL);
        }
        snprintf(s->opaque_key, sizeof s->opaque_key, "%s", key);
    }
    wl_surface_set_buffer_scale(s->wl, s->scale);
    wl_surface_attach(s->wl, b->wb, 0, 0);
    s->frame_cb = wl_surface_frame(s->wl);
    wl_callback_add_listener(s->frame_cb, &frame_listener, s);
    s->frame_pending = true;
    wl_surface_commit(s->wl);

    if (s->nstats == s->capstats) {
        s->capstats = s->capstats ? s->capstats * 2 : 1024;
        s->stats = realloc(s->stats, s->capstats * sizeof *s->stats);
    }
    s->stats[s->nstats++] = (ui_frame_stat){ t1 - t0, t_moved - t0, s->last_draw_t > 0 ? t0 - s->last_draw_t : 0,
                                             drawn / ((double)bw * bh) + layer_drawn, plan.full };
    s->last_draw_t = t0;
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
    (void)t;
    wl_callback_destroy(cb);
    ui_surface *s = d;
    s->frame_cb = NULL;
    s->frame_pending = false;
    if (s->cur_any || s->continuous || s->layer_dirty) want_draw(s);
}

/* ---------------------------------------------------------------- surfaces */

static void surf_enter(void *d, struct wl_surface *w, struct wl_output *o) { (void)d; (void)w; (void)o; }
static void surf_leave(void *d, struct wl_surface *w, struct wl_output *o) { (void)d; (void)w; (void)o; }
static void surf_scale(void *d, struct wl_surface *w, int32_t f)
{
    (void)w;
    ui_surface *s = d;
    if (ui_app_scale_override() > 0 || f <= 0 || f == s->scale) return;
    s->scale = f;
    ui_surface_damage_all(s);
}
static void surf_transform(void *d, struct wl_surface *w, uint32_t t) { (void)d; (void)w; (void)t; }
static const struct wl_surface_listener surf_listener = { surf_enter, surf_leave, surf_scale, surf_transform };

static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial)
{
    ui_surface *s = d;
    xdg_surface_ack_configure(x, serial);
    bool first = !s->configured;
    s->configured = true;
    ui_surface_damage_all(s);
    if (first && s->popup) ui_a11y_shown(s);   /* a menu, for screen readers */
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
    (void)t;
    ui_surface *s = d;
    bool was_active = s->activated;
    s->maximized = s->activated = false;
    uint32_t *st;
    wl_array_for_each(st, states) {
        if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED) s->maximized = true;
        if (*st == XDG_TOPLEVEL_STATE_ACTIVATED) s->activated = true;
    }
    if (w > 0 && h > 0 && (w != s->w || h != s->h)) {
        s->w = w;
        s->h = h;
        if (s->ops->resized) s->ops->resized(s, w, h);
    }
    if (was_active && !s->activated) ui_menu_close_all();
    if (was_active != s->activated && s->ops->focus) s->ops->focus(s, s->activated);
    if (was_active != s->activated) ui_a11y_activated(s, s->activated);
}
static void top_close(void *d, struct xdg_toplevel *t)
{
    (void)t;
    ui_surface *s = d;
    if (s->ops->close) s->ops->close(s);
}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

static void pop_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)p; (void)x; (void)y;
    ui_surface *s = d;
    if (w > 0 && h > 0) { s->w = w; s->h = h; }
}
static void pop_done(void *d, struct xdg_popup *p)
{
    (void)p;
    ui_surface *s = d;
    if (s->ops->dismissed) s->ops->dismissed(s);
}
static void pop_repositioned(void *d, struct xdg_popup *p, uint32_t token) { (void)d; (void)p; (void)token; }
static const struct xdg_popup_listener pop_listener = { pop_configure, pop_done, pop_repositioned };

static ui_surface *surface_new(ui_app *a, const ui_surface_ops *ops, void *data, double w, double h)
{
    ui_surface *s = calloc(1, sizeof *s);
    s->app = a;
    s->ops = ops;
    s->data = data;
    s->w = w;
    s->h = h;
    int so = ui_app_scale_override();
    s->scale = so > 0 ? so : 1;
    for (int i = 0; i < UI_BUFFERS; i++) s->bufs[i].frame = -1;
    s->wl = wl_compositor_create_surface(a->comp);
    wl_surface_set_user_data(s->wl, s);
    if (a->comp_version >= 6) wl_surface_add_listener(s->wl, &surf_listener, s);
    s->xs = xdg_wm_base_get_xdg_surface(a->wm, s->wl);
    xdg_surface_add_listener(s->xs, &xs_listener, s);
    s->next = a->surfaces;
    a->surfaces = s;
    ui_a11y_surface_new(s);
    return s;
}

ui_surface *ui_window_new(ui_app *a, const ui_surface_ops *ops, void *data, const char *app_id, double w, double h)
{
    ui_surface *s = surface_new(a, ops, data, w, h);
    s->top = xdg_surface_get_toplevel(s->xs);
    xdg_toplevel_add_listener(s->top, &top_listener, s);
    if (app_id) xdg_toplevel_set_app_id(s->top, app_id);
    xdg_toplevel_set_min_size(s->top, 160, 100);
    s->activated = true;
    wl_surface_commit(s->wl);
    return s;
}

void ui_window_set_size(ui_surface *s, double w, double h, bool fixed)
{
    if (!s->top) return;
    if (fixed) {
        xdg_toplevel_set_min_size(s->top, (int32_t)w, (int32_t)h);
        xdg_toplevel_set_max_size(s->top, (int32_t)w, (int32_t)h);
    }
    if (w == s->w && h == s->h) return;
    s->w = w;
    s->h = h;
    if (s->ops->resized) s->ops->resized(s, w, h);
    ui_surface_damage_all(s);
}

/* The Dialogs font (or another look) changed: every window lays itself
 * out again at its size and is drawn again. */
void ui_app_restyle(ui_app *a)
{
    for (ui_surface *s = a->surfaces; s; s = s->next) {
        if (s->ops->resized && s->w > 0 && s->h > 0) s->ops->resized(s, s->w, s->h);
        ui_surface_damage_all(s);
    }
}

static struct xdg_positioner *positioner(ui_surface *s, double w, double h)
{
    ui_rect anchor = s->pop_anchor;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(s->app->wm);
    xdg_positioner_set_size(pos, (int32_t)w, (int32_t)h);
    xdg_positioner_set_anchor_rect(pos, (int32_t)anchor.x, (int32_t)anchor.y, anchor.w < 1 ? 1 : (int32_t)anchor.w,
                                   anchor.h < 1 ? 1 : (int32_t)anchor.h);
    if (s->pop_side) {
        /* beside the anchor, out to that side, its top edges lined up (a
         * vertical LaunchPad's drawers) */
        xdg_positioner_set_anchor(pos, s->pop_side < 0 ? XDG_POSITIONER_ANCHOR_TOP_LEFT : XDG_POSITIONER_ANCHOR_TOP_RIGHT);
        xdg_positioner_set_gravity(pos, s->pop_side < 0 ? XDG_POSITIONER_GRAVITY_BOTTOM_LEFT : XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    } else if (s->pop_above) {
        /* above the anchor, its left edges lined up */
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP_RIGHT);
    } else {
        xdg_positioner_set_anchor(pos, anchor.w > 1 ? XDG_POSITIONER_ANCHOR_TOP_RIGHT : XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    }
    xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                                                  XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                                  XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                  XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    return pos;
}

static ui_surface *popup_new(ui_app *a, ui_surface *parent, const ui_surface_ops *ops, void *data, ui_rect anchor,
                             double w, double h, uint32_t serial, bool above);
static int side_next;   /* (ui_popup_new_beside's side, for popup_new) */

ui_surface *ui_popup_new(ui_app *a, ui_surface *parent, const ui_surface_ops *ops, void *data, ui_rect anchor,
                         double w, double h, uint32_t serial)
{
    return popup_new(a, parent, ops, data, anchor, w, h, serial, false);
}

ui_surface *ui_popup_new_above(ui_app *a, ui_surface *parent, const ui_surface_ops *ops, void *data, ui_rect anchor,
                               double w, double h, uint32_t serial)
{
    return popup_new(a, parent, ops, data, anchor, w, h, serial, true);
}

ui_surface *ui_popup_new_beside(ui_app *a, ui_surface *parent, const ui_surface_ops *ops, void *data, ui_rect anchor,
                                double w, double h, uint32_t serial, bool left)
{
    side_next = left ? -1 : 1;
    ui_surface *s = popup_new(a, parent, ops, data, anchor, w, h, serial, false);
    side_next = 0;
    return s;
}

void ui_popup_set_size(ui_surface *s, double w, double h)
{
    if (!s->popup || (w == s->w && h == s->h)) return;
    s->w = w;
    s->h = h;
    if (xdg_popup_get_version(s->popup) >= XDG_POPUP_REPOSITION_SINCE_VERSION) {
        struct xdg_positioner *pos = positioner(s, w, h);
        xdg_popup_reposition(s->popup, pos, 0);
        xdg_positioner_destroy(pos);
    }
    if (s->ops->resized) s->ops->resized(s, w, h);
    ui_surface_damage_all(s);
}

static ui_surface *popup_new(ui_app *a, ui_surface *parent, const ui_surface_ops *ops, void *data, ui_rect anchor,
                             double w, double h, uint32_t serial, bool above)
{
    ui_surface *s = surface_new(a, ops, data, w, h);
    s->parent = parent;
    s->scale = parent->scale;
    s->pop_anchor = anchor;
    s->pop_above = above;
    s->pop_side = side_next;
    struct xdg_positioner *pos = positioner(s, w, h);
    s->popup = xdg_surface_get_popup(s->xs, parent->xs, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(s->popup, &pop_listener, s);
    /* Serial 0: no grab. The program closes its own menus on presses
     * elsewhere and when its window loses focus, so a menu opened after a
     * round trip to the service (when the button is no longer held, and a
     * grab would be refused) still works. */
    if (serial) xdg_popup_grab(s->popup, a->seat, serial);
    wl_surface_commit(s->wl);
    return s;
}

void ui_surface_destroy(ui_surface *s)
{
    ui_app *a = s->app;
    if (a->ptr_focus == s) a->ptr_focus = NULL;
    if (a->key_focus == s) a->key_focus = NULL;
    ui_textinput_surface_gone(s);
    ui_a11y_surface_gone(s);
    for (ui_surface **pp = &a->surfaces; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    ui_menu_forget_surface(s);   /* its menus go first: popups are destroyed topmost first */
    if (s->layer) ui_layer_free(s->layer);
    if (s->frame_cb) wl_callback_destroy(s->frame_cb);
    if (s->popup) xdg_popup_destroy(s->popup);
    if (s->top) xdg_toplevel_destroy(s->top);
    xdg_surface_destroy(s->xs);
    wl_surface_destroy(s->wl);
    for (int i = 0; i < UI_BUFFERS; i++) buf_free(&s->bufs[i]);
    free(s->stats);
    free(s);
}

void *ui_surface_data(ui_surface *s) { return s->data; }
ui_app *ui_surface_app(ui_surface *s) { return s->app; }
double ui_surface_width(ui_surface *s) { return s->w; }
double ui_surface_height(ui_surface *s) { return s->h; }
int ui_surface_scale(ui_surface *s) { return s->scale; }

void ui_window_set_title(ui_surface *s, const char *title)
{
    snprintf(s->title, sizeof s->title, "%s", title);
    if (s->top) xdg_toplevel_set_title(s->top, title);
    ui_frame_damage(s);
}
const char *ui_window_title(ui_surface *s) { return s->title; }
void ui_window_move(ui_surface *s, uint32_t serial) { xdg_toplevel_move(s->top, s->app->seat, serial); }
void ui_window_resize(ui_surface *s, uint32_t serial, unsigned edges) { xdg_toplevel_resize(s->top, s->app->seat, serial, edges); }
static ui_action_fn action_hook;
void ui_set_action_hook(ui_action_fn fn) { action_hook = fn; }
void ui_action(const char *name, ui_surface *s, ui_rect at) { if (action_hook) action_hook(name, s, at); }

static ui_rect whole(ui_surface *s) { return (ui_rect){ 0, 0, s->w, s->h }; }

void ui_window_minimize(ui_surface *s)
{
    ui_action("window.minimize", s, whole(s));
    xdg_toplevel_set_minimized(s->top);
}
void ui_window_toggle_maximize(ui_surface *s)
{
    ui_action(s->maximized ? "window.restore" : "window.maximize", s, whole(s));
    if (s->maximized) xdg_toplevel_unset_maximized(s->top);
    else xdg_toplevel_set_maximized(s->top);
}
bool ui_window_maximized(ui_surface *s) { return s->maximized; }
void ui_window_set_maximized(ui_surface *s) { if (s->top) xdg_toplevel_set_maximized(s->top); }
void ui_window_set_frameless(ui_surface *s, bool on) { s->frameless = on; ui_surface_damage_all(s); }
bool ui_window_active(ui_surface *s) { return s->activated; }

/* ----------------------------------------------------------------- cursor */

void ui_set_cursor(ui_app *a, const char *name)
{
    if (!a->pointer || !a->enter_serial) return;
    int scale = a->ptr_focus ? a->ptr_focus->scale : 1;
    if (!a->cursor_theme || a->cursor_scale != scale) {
        if (a->cursor_theme) wl_cursor_theme_destroy(a->cursor_theme);
        const char *sz = getenv("XCURSOR_SIZE");
        a->cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), (sz ? atoi(sz) : 24) * scale, a->shm);
        a->cursor_scale = scale;
        a->cursor_name = NULL;
    }
    if (a->cursor_name && !strcmp(a->cursor_name, name)) return;
    struct wl_cursor *c = a->cursor_theme ? wl_cursor_theme_get_cursor(a->cursor_theme, name) : NULL;
    if (!c && a->cursor_theme && !strcmp(name, "col-resize")) c = wl_cursor_theme_get_cursor(a->cursor_theme, "sb_h_double_arrow");   /* (older themes' name) */
    if (!c && a->cursor_theme) c = wl_cursor_theme_get_cursor(a->cursor_theme, "left_ptr");
    if (!c) return;
    struct wl_cursor_image *img = c->images[0];
    if (!a->cursor_surface) a->cursor_surface = wl_compositor_create_surface(a->comp);
    wl_surface_set_buffer_scale(a->cursor_surface, scale);
    wl_surface_attach(a->cursor_surface, wl_cursor_image_get_buffer(img), 0, 0);
    wl_surface_damage_buffer(a->cursor_surface, 0, 0, (int32_t)img->width, (int32_t)img->height);
    wl_surface_commit(a->cursor_surface);
    wl_pointer_set_cursor(a->pointer, a->enter_serial, a->cursor_surface, (int32_t)img->hotspot_x / scale,
                          (int32_t)img->hotspot_y / scale);
    a->cursor_name = name;
}

/* ----------------------------------------------------------------- pointer */

void (*ui_press_hook)(ui_surface *s);

static void send_ptr(ui_app *a, ui_pointer_event *e)
{
    ui_surface *s = a->ptr_focus;
    if (!s || !s->ops->pointer) return;
    e->x = a->px - s->m;   /* buffer -> window coordinates */
    e->y = a->py - s->m;
    e->mods = a->mods;
    s->ops->pointer(s, e);
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *w, wl_fixed_t x, wl_fixed_t y)
{
    (void)p;
    ui_app *a = d;
    a->ptr_focus = w ? wl_surface_get_user_data(w) : NULL;
    a->enter_serial = serial;
    a->cursor_name = NULL;
    a->px = wl_fixed_to_double(x);
    a->py = wl_fixed_to_double(y);
    ui_set_cursor(a, "default");
    ui_pointer_event e = { .kind = UI_ENTER, .serial = serial };
    send_ptr(a, &e);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *w)
{
    (void)p; (void)w;
    ui_app *a = d;
    ui_pointer_event e = { .kind = UI_LEAVE, .serial = serial };
    send_ptr(a, &e);
    a->ptr_focus = NULL;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)p; (void)t;
    ui_app *a = d;
    a->px = wl_fixed_to_double(x);
    a->py = wl_fixed_to_double(y);
    ui_pointer_event e = { .kind = UI_MOTION };
    send_ptr(a, &e);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
    (void)p; (void)t;
    ui_app *a = d;
    a->last_serial = serial;
    int b = button == BTN_LEFT ? UI_BTN1 : button == BTN_RIGHT ? UI_BTN2 : button == BTN_MIDDLE ? UI_BTN3 : 0;
    if (!b) return;
    if (ui_mouse.left_handed && b != UI_BTN3) b = b == UI_BTN1 ? UI_BTN2 : UI_BTN1;
    ui_pointer_event e = { .kind = state == WL_POINTER_BUTTON_STATE_PRESSED ? UI_PRESS : UI_RELEASE,
                           .button = b, .clicks = 1, .serial = serial };
    if (e.kind == UI_PRESS) {
        double now = ui_now_ms();
        if (b == a->last_button && now - a->last_click_t < ui_mouse.double_click_ms && fabs(a->px - a->last_click_x) < 4 &&
            fabs(a->py - a->last_click_y) < 4)
            e.clicks = 2;
        a->last_button = e.clicks == 2 ? 0 : b;   /* a third click starts again */
        a->last_click_t = now;
        a->last_click_x = a->px;
        a->last_click_y = a->py;
        /* A press outside an open menu closes it and is used up, except the
         * second click of a double click: the first opened the menu (the
         * system menu button), and the double click means Close. */
        if (ui_press_hook && a->ptr_focus) ui_press_hook(a->ptr_focus);
        if (a->ptr_focus && ui_menu_press_outside(a->ptr_focus) && e.clicks != 2) {
            a->swallow_release = b;
            return;
        }
    } else if (a->swallow_release == b) {
        a->swallow_release = 0;
        return;
    }
    send_ptr(a, &e);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
    (void)p; (void)t;
    ui_app *a = d;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    double k = a->axis_source == WL_POINTER_AXIS_SOURCE_WHEEL ? ui_mouse.wheel_speed : 1.0;
    ui_pointer_event e = { .kind = UI_WHEEL, .wheel = wl_fixed_to_double(v) * k };
    send_ptr(a, &e);
}
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)p; ((ui_app *)d)->axis_source = (int)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t x) { (void)d; (void)p; (void)t; (void)x; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t x, int32_t v) { (void)d; (void)p; (void)x; (void)v; }
static void p_axis_v120(void *d, struct wl_pointer *p, uint32_t x, int32_t v) { (void)d; (void)p; (void)x; (void)v; }
static void p_axis_rel(void *d, struct wl_pointer *p, uint32_t x, uint32_t r) { (void)d; (void)p; (void)x; (void)r; }
static const struct wl_pointer_listener p_listener = {
    .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis, .frame = p_frame,
    .axis_source = p_axis_source, .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
    .axis_value120 = p_axis_v120, .axis_relative_direction = p_axis_rel,
};

/* ---------------------------------------------------------------- keyboard */

static void send_key(ui_app *a, uint32_t key, bool repeat)
{
    ui_surface *s = ui_menu_key_surface();   /* an open menu takes the keys */
    if (!s) s = a->key_focus;
    if (!s || !s->ops->key || !a->kstate) return;
    ui_key_event e = { .sym = xkb_state_key_get_one_sym(a->kstate, key + 8), .mods = a->mods, .repeat = repeat };
    xkb_state_key_get_utf8(a->kstate, key + 8, e.utf8, sizeof e.utf8);
    /* The system menu from the keyboard, as OS/2: Alt+Space or Shift+Esc,
     * in any framed window. */
    if (s->top && !s->frameless && !repeat &&
        ((e.sym == XKB_KEY_space && (e.mods & UI_MOD_ALT)) || (e.sym == XKB_KEY_Escape && (e.mods & UI_MOD_SHIFT)))) {
        ui_frame_system_menu(s, a->last_serial);
        return;
    }
    s->ops->key(s, &e);
}

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
    (void)k;
    ui_app *a = d;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    if (a->kstate) xkb_state_unref(a->kstate);
    if (a->keymap) xkb_keymap_unref(a->keymap);
    a->keymap = xkb_keymap_new_from_string(a->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    a->kstate = a->keymap ? xkb_state_new(a->keymap) : NULL;
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w, struct wl_array *keys)
{
    (void)k; (void)s; (void)keys;
    ui_app *a = d;
    a->key_focus = w ? wl_surface_get_user_data(w) : NULL;
    if (a->key_focus && a->key_focus == a->ti_surface && a->ti_entry) ui_ibus_focus(a, true);
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w)
{
    (void)k; (void)s; (void)w;
    ui_app *a = d;
    a->key_focus = NULL;
    ui_ibus_focus(a, false);
    a->repeat_key = 0;
    struct itimerspec off = { 0 };
    timerfd_settime(a->repeat_fd, 0, &off, NULL);
}
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
    (void)k; (void)t;
    ui_app *a = d;
    a->last_serial = serial;
    struct itimerspec its = { 0 };
    /* an entry being typed in: IBus first (Hangul, ...), where the session uses it */
    uint32_t sym = a->kstate ? xkb_state_key_get_one_sym(a->kstate, key + 8) : 0;
    bool to_ibus = !ui_menu_key_surface();
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED && to_ibus) ui_ibus_key(a, key, sym, false);
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && to_ibus && ui_ibus_key(a, key, sym, true)) {
        a->repeat_key = 0;
        timerfd_settime(a->repeat_fd, 0, &its, NULL);
        return;
    }
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        send_key(a, key, false);
        if (a->keymap && xkb_keymap_key_repeats(a->keymap, key + 8) && a->repeat_rate > 0) {
            a->repeat_key = key;
            its.it_value.tv_nsec = (long)a->repeat_delay * 1000000L;
            its.it_value.tv_sec = its.it_value.tv_nsec / 1000000000L;
            its.it_value.tv_nsec %= 1000000000L;
            its.it_interval.tv_nsec = 1000000000L / a->repeat_rate;
        }
        timerfd_settime(a->repeat_fd, 0, &its, NULL);
    } else if (key == a->repeat_key) {
        a->repeat_key = 0;
        timerfd_settime(a->repeat_fd, 0, &its, NULL);
    }
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    (void)k; (void)s;
    ui_app *a = d;
    if (!a->kstate) return;
    xkb_state_update_mask(a->kstate, dep, lat, lock, 0, 0, grp);
    unsigned before = a->mods;
    a->mods = 0;
    if (xkb_state_mod_name_is_active(a->kstate, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) a->mods |= UI_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(a->kstate, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) a->mods |= UI_MOD_CTRL;
    if (xkb_state_mod_name_is_active(a->kstate, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) a->mods |= UI_MOD_ALT;
    if (a->mods != before && a->drag_offer) ui_dnd_renegotiate(a);   /* Ctrl pressed mid-drag: copy, at once */
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    (void)k;
    ui_app *a = d;
    a->repeat_rate = rate;
    a->repeat_delay = delay;
}
static const struct wl_keyboard_listener k_listener = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
    ui_app *a = d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &p_listener, a);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &k_listener, a);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* --------------------------------------------------------------------- app */

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { (void)d; xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    ui_app *a = d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        a->comp_version = ver < 6 ? (int)ver : 6;
        a->comp = wl_registry_bind(r, name, &wl_compositor_interface, (uint32_t)a->comp_version);
    } else if (!strcmp(iface, xdg_activation_v1_interface.name)) {
        a->activation = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
        a->ddm_version = ver < 3 ? (int)ver : 3;
        a->ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, (uint32_t)a->ddm_version);
    } else if (!strcmp(iface, wl_subcompositor_interface.name)) {
        a->subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        a->viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        a->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
        xdg_wm_base_add_listener(a->wm, &wm_listener, a);
    } else if (!strcmp(iface, wl_seat_interface.name) && !a->seat) {
        a->seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 8 ? ver : 8);
        wl_seat_add_listener(a->seat, &seat_listener, a);
    } else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name)) {
        ui_textinput_bind(a, r, name, ver);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

ui_app *ui_app_new(void)
{
    ui_scheme_init();
    ui_text_init();
    ui_mouse_load();
    ui_app *a = calloc(1, sizeof *a);
    a->dpy = wl_display_connect(NULL);
    if (!a->dpy) { fprintf(stderr, "wps-ui: no Wayland display\n"); free(a); return NULL; }
    a->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    a->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    a->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    a->repeat_rate = 25;
    a->repeat_delay = 400;
    pthread_mutex_init(&a->post_mu, NULL);
    a->reg = wl_display_get_registry(a->dpy);
    wl_registry_add_listener(a->reg, &reg_listener, a);
    wl_display_roundtrip(a->dpy);
    wl_display_roundtrip(a->dpy);
    if (!a->comp || !a->shm || !a->wm || !a->seat) {
        fprintf(stderr, "wps-ui: the compositor lacks wl_compositor, wl_shm, xdg_wm_base or a seat\n");
        return NULL;
    }
    ui_dnd_init(a);
    ui_textinput_seat(a);
    ui_a11y_init(a);
    return a;
}

uint32_t ui_app_input_serial(ui_app *a) { return a->last_serial; }

void ui_app_post(ui_app *a, void (*fn)(void *), void *arg)
{
    struct post *p = malloc(sizeof *p);
    p->next = NULL;
    p->fn = fn;
    p->arg = arg;
    pthread_mutex_lock(&a->post_mu);
    if (a->posts_tail) a->posts_tail->next = p;
    else a->posts = p;
    a->posts_tail = p;
    pthread_mutex_unlock(&a->post_mu);
    uint64_t one = 1;
    ssize_t w = write(a->wake_fd, &one, sizeof one);
    (void)w;
}

void ui_app_quit(ui_app *a) { a->running = false; }

void ui_app_run(ui_app *a)
{
    a->running = true;
    int wfd = wl_display_get_fd(a->dpy);
    while (a->running && a->surfaces) {
        while (wl_display_prepare_read(a->dpy) != 0) wl_display_dispatch_pending(a->dpy);
        wl_display_flush(a->dpy);
        struct pollfd pf[3 + 64] = { { wfd, POLLIN, 0 }, { a->wake_fd, POLLIN, 0 }, { a->repeat_fd, POLLIN, 0 } };
        size_t nw = ui_watch_fill(pf + 3, 64);
        int r = poll(pf, 3 + nw, -1);
        if (r > 0 && (pf[0].revents & POLLIN)) wl_display_read_events(a->dpy);
        else wl_display_cancel_read(a->dpy);
        if (pf[0].revents & (POLLERR | POLLHUP)) { fprintf(stderr, "wps-ui: lost the display\n"); break; }
        wl_display_dispatch_pending(a->dpy);
        if (pf[1].revents & POLLIN) {
            uint64_t x;
            ssize_t rd = read(a->wake_fd, &x, sizeof x);
            (void)rd;
            pthread_mutex_lock(&a->post_mu);
            struct post *p = a->posts;
            a->posts = a->posts_tail = NULL;
            pthread_mutex_unlock(&a->post_mu);
            while (p) {
                struct post *n = p->next;
                p->fn(p->arg);
                free(p);
                p = n;
            }
        }
        if (pf[2].revents & POLLIN) {
            uint64_t x;
            ssize_t rd = read(a->repeat_fd, &x, sizeof x);
            (void)rd;
            if (a->repeat_key) send_key(a, a->repeat_key, true);
        }
        if (nw) ui_watch_dispatch(pf + 3, nw);
        /* Draw what changed, once per turn and at most once per frame. */
        for (ui_surface *s = a->surfaces, *n; s; s = n) {
            n = s->next;
            if (s->needs_draw || s->continuous) render(s);
        }
    }
    if (getenv("WPS_UI_DEBUG_EXIT"))
        fprintf(stderr, "wps-ui: main loop ended: %s\n", !a->surfaces ? "no windows left" : !a->running ? "quit requested" : "the display was lost");
    wl_display_flush(a->dpy);
}

size_t ui_surface_verify(ui_surface *s, const char *png_prefix)
{
    if (s->cur_any || s->layer_dirty) return SIZE_MAX;   /* changes not drawn yet */
    struct ui_buffer *b = NULL;
    for (int i = 0; i < UI_BUFFERS; i++)
        if (s->bufs[i].wb && s->bufs[i].frame == s->frame_no - 1) b = &s->bufs[i];
    /* the window's part of the buffer (inside any shadow margin) */
    int bw = (int)lround(s->w * s->scale), bh = (int)lround(s->h * s->scale), om = (int)lround(s->m * s->scale);
    if (!b || b->w != buf_w(s) || b->h != buf_h(s)) return SIZE_MAX;
    size_t stride = (size_t)bw * 4, n = stride * (size_t)bh;
    /* What is shown: the window's buffer, with the layer's slice on it. */
    cairo_surface_t *shown = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    cairo_surface_flush(shown);
    unsigned char *sp = cairo_image_surface_get_data(shown);
    int sstride = cairo_image_surface_get_stride(shown);
    size_t bstride = (size_t)b->w * 4;
    for (int y = 0; y < bh; y++)
        memcpy(sp + (size_t)y * (size_t)sstride, (unsigned char *)b->data + (size_t)(y + om) * bstride + (size_t)om * 4, stride);
    if (s->layer && !ui_layer_paste_shown(s->layer, sp, sstride)) { cairo_surface_destroy(shown); return SIZE_MAX; }
    cairo_surface_mark_dirty(shown);
    /* What should be shown: everything drawn afresh. */
    cairo_surface_t *exp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    cairo_surface_set_device_scale(exp, s->scale, s->scale);
    cairo_t *cr = cairo_create(exp);
    s->ops->draw(s, cr, (ui_rect){ 0, 0, s->w, s->h });
    if (s->layer) ui_layer_draw_fresh(s->layer, cr);
    cairo_destroy(cr);
    cairo_surface_flush(exp);
    unsigned char *ep = cairo_image_surface_get_data(exp);
    int estride = cairo_image_surface_get_stride(exp);
    size_t diff = 0;
    for (int y = 0; y < bh; y++) {
        const uint32_t *a = (const uint32_t *)(sp + (size_t)y * (size_t)sstride);
        const uint32_t *e = (const uint32_t *)(ep + (size_t)y * (size_t)estride);
        for (int x = 0; x < bw; x++) diff += (a[x] & 0xffffff) != (e[x] & 0xffffff);   /* colour only */
    }
    if (diff && png_prefix) {
        char p[4096];
        snprintf(p, sizeof p, "%s-shown.png", png_prefix);
        cairo_surface_write_to_png(shown, p);
        snprintf(p, sizeof p, "%s-expected.png", png_prefix);
        cairo_surface_write_to_png(exp, p);
    }
    (void)n;
    cairo_surface_destroy(shown);
    cairo_surface_destroy(exp);
    return diff;
}

uint32_t ui_surface_test_drop(ui_surface *s, double x, double y, const char *mime, const char *data, unsigned mods)
{
    if (!s || !s->ops || !s->ops->drop) return 0;
    if (s->ops->drag_motion) {
        const char *mimes[1] = { mime }, *chosen = NULL;
        if (!s->ops->drag_motion(s, x, y, mimes, 1, UI_DND_COPY, mods, &chosen) || !chosen) return 0;
    }
    return s->ops->drop(s, x, y, mime, data, strlen(data), UI_DND_COPY, 0, mods);
}

bool ui_surface_snapshot(ui_surface *s, const char *png)
{
    struct ui_buffer *b = NULL;
    for (int i = 0; i < UI_BUFFERS; i++)
        if (s->bufs[i].wb && s->bufs[i].frame == s->frame_no - 1) b = &s->bufs[i];
    if (!b) return false;
    cairo_surface_t *cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32, b->w, b->h, b->w * 4);
    bool ok = cairo_surface_write_to_png(cs, png) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(cs);
    return ok;
}

void ui_surface_send_key(ui_surface *s, const ui_key_event *e)
{
    if (s && s->ops->key) s->ops->key(s, e);
}

/* ------------------------------------------------------------ activation */

struct act { struct xdg_activation_token_v1 *tok; ui_app *app; struct wl_surface *target; ui_surface *s; };

static void token_done(void *data, struct xdg_activation_token_v1 *t, const char *token)
{
    struct act *x = data;
    for (ui_surface *s = x->app->surfaces; s; s = s->next)
        if (s == x->s) xdg_activation_v1_activate(x->app->activation, token, s->wl);   /* still there */
    xdg_activation_token_v1_destroy(t);
    free(x);
}
static const struct xdg_activation_token_v1_listener token_listener = { token_done };

/* A token for something this program starts (its window then comes to the
 * front): asked with the last input's serial and the focused surface;
 * fn gets it, or NULL at once with no xdg-activation. */
struct tok_req { ui_app *app; void (*fn)(void *ctx, const char *token); void *ctx; };

static void got_token(void *data, struct xdg_activation_token_v1 *t, const char *token)
{
    struct tok_req *x = data;
    x->fn(x->ctx, token);
    xdg_activation_token_v1_destroy(t);
    free(x);
}
static const struct xdg_activation_token_v1_listener got_token_listener = { got_token };

void ui_activation_token(ui_app *a, void (*fn)(void *ctx, const char *token), void *ctx)
{
    if (!a->activation) { fn(ctx, NULL); return; }
    struct tok_req *x = calloc(1, sizeof *x);
    x->app = a;
    x->fn = fn;
    x->ctx = ctx;
    struct xdg_activation_token_v1 *t = xdg_activation_v1_get_activation_token(a->activation);
    xdg_activation_token_v1_add_listener(t, &got_token_listener, x);
    if (a->last_serial) xdg_activation_token_v1_set_serial(t, a->last_serial, a->seat);
    ui_surface *from = a->key_focus ? a->key_focus : a->ptr_focus;
    if (from) xdg_activation_token_v1_set_surface(t, from->wl);
    xdg_activation_token_v1_commit(t);
}

void ui_window_activate(ui_surface *s)
{
    ui_app *a = s->app;
    if (!a->activation) return;
    struct act *x = calloc(1, sizeof *x);
    x->app = a;
    x->s = s;
    x->tok = xdg_activation_v1_get_activation_token(a->activation);
    xdg_activation_token_v1_add_listener(x->tok, &token_listener, x);
    if (a->last_serial) xdg_activation_token_v1_set_serial(x->tok, a->last_serial, a->seat);
    ui_surface *from = a->key_focus ? a->key_focus : a->ptr_focus;
    if (from) xdg_activation_token_v1_set_surface(x->tok, from->wl);
    xdg_activation_token_v1_commit(x->tok);
}
