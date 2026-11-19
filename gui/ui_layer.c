/*
 * Scroll layers: scrolling without moving pixels (gui-design.md, G1
 * findings).
 *
 * A layer shows a window of tall content (a folder's rows) in an area of
 * its surface. Its buffer is a ring twice as tall as the area (H): content
 * row y lives at ring rows (y mod H) and (y mod H) + H, so any window of H
 * rows is one contiguous slice. Scrolling by dy draws only the dy rows that
 * came into view (and copies them to their twin rows); one subsurface
 * shows the slice through wp_viewporter, so the rest is never redrawn or
 * moved. (Two subsurfaces showing the two halves of a plain ring made
 * GNOME upload the buffer twice per frame: 48 fps instead of 58 at 2x full
 * screen.) Damage is kept in content coordinates, so a changed row stays
 * where it is while the view scrolls.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ui_internal.h"
#include "viewporter-client-protocol.h"

#define RINGS 3

struct ring {
    struct wl_buffer *wb;
    void *data;
    size_t size;
    int w, h;
    bool busy;
    long frame;       /* -1: contents unknown */
    int top;          /* content row (device) at the window's start when drawn */
};

struct ui_layer {
    ui_surface *s;
    ui_layer_draw_fn draw;
    void *ctx;
    ui_rect area;     /* on the surface; whole logical pixels */
    int offset;       /* content y at the top of the area (logical) */
    int scale;
    struct wl_surface *part;
    struct wl_subsurface *sub;
    struct wp_viewport *vp;
    struct ring rings[RINGS];
    ui_damage_frame hist[UI_DAMAGE_HISTORY];
    int nhist;
    ui_damage_frame cur;
    long frame_no;
};

static void ring_release(void *d, struct wl_buffer *wb)
{
    (void)wb;
    ((struct ring *)d)->busy = false;
}
static const struct wl_buffer_listener ring_listener = { ring_release };

static void ring_free(struct ring *r)
{
    if (r->wb) wl_buffer_destroy(r->wb);
    if (r->data) munmap(r->data, r->size);
    memset(r, 0, sizeof *r);
    r->frame = -1;
}

static struct ring *ring_get(ui_layer *l, int w, int h)
{
    for (int i = 0; i < RINGS; i++) {
        struct ring *r = &l->rings[i];
        if (r->busy) continue;
        if (r->wb && (r->w != w || r->h != h)) ring_free(r);
        if (!r->wb) {
            r->size = (size_t)w * 4 * (size_t)h;
            int fd = memfd_create("wps-layer", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, (off_t)r->size) < 0) { if (fd >= 0) close(fd); return NULL; }
            r->data = mmap(NULL, r->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (r->data == MAP_FAILED) { r->data = NULL; close(fd); return NULL; }
            struct wl_shm_pool *pool = wl_shm_create_pool(l->s->app->shm, fd, (int32_t)r->size);
            r->wb = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
            wl_buffer_add_listener(r->wb, &ring_listener, r);
            wl_shm_pool_destroy(pool);
            close(fd);
            r->w = w;
            r->h = h;
            r->frame = -1;
        }
        return r;
    }
    return NULL;
}

ui_layer *ui_layer_new(ui_surface *s, ui_layer_draw_fn draw, void *ctx)
{
    ui_app *a = s->app;
    if (!a->subcomp || !a->viewporter) return NULL;
    ui_layer *l = calloc(1, sizeof *l);
    l->s = s;
    l->draw = draw;
    l->ctx = ctx;
    l->scale = s->scale;
    for (int i = 0; i < RINGS; i++) l->rings[i].frame = -1;
    struct wl_region *none = wl_compositor_create_region(a->comp);
    l->part = wl_compositor_create_surface(a->comp);
    l->sub = wl_subcompositor_get_subsurface(a->subcomp, l->part, s->wl);
    wl_subsurface_set_sync(l->sub);
    wl_surface_set_input_region(l->part, none);   /* input goes to the window */
    l->vp = wp_viewporter_get_viewport(a->viewporter, l->part);
    wl_region_destroy(none);
    s->layer = l;
    return l;
}

void ui_layer_free(ui_layer *l)
{
    if (!l) return;
    if (l->s->layer == l) l->s->layer = NULL;
    wp_viewport_destroy(l->vp);
    wl_subsurface_destroy(l->sub);
    wl_surface_destroy(l->part);
    for (int i = 0; i < RINGS; i++) ring_free(&l->rings[i]);
    free(l);
}

static void want(ui_layer *l)
{
    l->s->layer_dirty = true;
    ui_surface_damage(l->s, (ui_rect){ 0, 0, 0, 0 });   /* schedule a frame */
}

void ui_layer_set_area(ui_layer *l, ui_rect area)
{
    area.x = (int)area.x;
    area.y = (int)area.y;
    area.w = (int)area.w;
    area.h = (int)area.h;
    if (area.x == l->area.x && area.y == l->area.y && area.w == l->area.w && area.h == l->area.h) return;
    l->area = area;
    l->cur.full = true;
    want(l);
}

void ui_layer_set_offset(ui_layer *l, double y)
{
    int o = (int)y;
    if (o == l->offset) return;
    l->offset = o;
    want(l);
}

void ui_layer_damage(ui_layer *l, ui_rect r)
{
    int k = l->scale;
    ui_irect d = { (int)((r.x - l->area.x) * k), (int)(r.y * k), (int)(r.w * k + 0.999), (int)(r.h * k + 0.999) };
    ui_damage_add(&l->cur, d, (int)(l->area.w * k), INT_MAX / 2);
    want(l);
}

void ui_layer_damage_all(ui_layer *l)
{
    l->cur.full = true;
    want(l);
}

/* Draw content rows [y0, y1) (device, content coordinates) into the ring's
 * first half, then copy them to their twins in the second. */
static void draw_rows(ui_layer *l, struct ring *r, cairo_t *cr, ui_irect rect)
{
    int H = r->h / 2, k = l->scale;
    int y0 = rect.y, y1 = rect.y + rect.h;
    while (y0 < y1) {
        int wrap = y0 / H;
        int end = (wrap + 1) * H < y1 ? (wrap + 1) * H : y1;
        /* Content logical (x, y) maps to ring (x - area.x, y - wrap * area.h). */
        ui_rect clip = { l->area.x + (double)rect.x / k, (double)y0 / k, (double)rect.w / k, (double)(end - y0) / k };
        cairo_save(cr);
        cairo_translate(cr, -l->area.x, -(double)wrap * l->area.h);
        cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
        cairo_clip(cr);
        l->draw(l->ctx, cr, clip);
        cairo_restore(cr);
        cairo_surface_flush(cairo_get_target(cr));
        size_t stride = (size_t)r->w * 4;
        unsigned char *px = r->data;
        int from = y0 - wrap * H;
        memcpy(px + (size_t)(from + H) * stride, px + (size_t)from * stride, (size_t)(end - y0) * stride);
        cairo_surface_mark_dirty(cairo_get_target(cr));
        y0 = end;
    }
}

/* Called by the surface's render, before it commits (the subsurfaces are
 * synchronised, so everything appears together). Returns the fraction of
 * the layer drawn. */
double ui_layer_render(ui_layer *l)
{
    ui_surface *s = l->s;
    if (s->scale != l->scale) { l->scale = s->scale; l->cur.full = true; }
    int k = l->scale, W = (int)l->area.w * k, H = (int)l->area.h * k;
    if (W <= 0 || H <= 0) return 0;
    struct ring *r = ring_get(l, W, 2 * H);
    if (!r) return -1;   /* all rings with the compositor: next frame */
    if (l->nhist == UI_DAMAGE_HISTORY) {
        memmove(l->hist, l->hist + 1, sizeof l->hist[0] * (UI_DAMAGE_HISTORY - 1));
        l->nhist--;
    }
    l->hist[l->nhist++] = l->cur;
    memset(&l->cur, 0, sizeof l->cur);
    s->layer_dirty = false;

    int top = l->offset * k;
    int age = r->frame < 0 ? 0 : (int)(l->frame_no - r->frame);
    bool full = age <= 0 || age > l->nhist || abs(top - r->top) >= H;
    for (int i = l->nhist - age; !full && i < l->nhist; i++) full = l->hist[i].full;
    ui_irect todo[UI_DAMAGE_RECTS * UI_DAMAGE_HISTORY + 1];
    int n = 0;
    ui_irect window = { 0, top, W, H };
    if (full) {
        todo[n++] = window;
    } else {
        /* Rows that came into view, and rows changed since this ring was drawn. */
        if (top > r->top) todo[n++] = (ui_irect){ 0, r->top + H, W, top - r->top };
        else if (top < r->top) todo[n++] = (ui_irect){ 0, top, W, r->top - top };
        for (int i = l->nhist - age; i < l->nhist; i++)
            for (int j = 0; j < l->hist[i].nrects; j++) {
                ui_irect d = l->hist[i].rects[j];
                int y0 = d.y > top ? d.y : top, y1 = d.y + d.h < top + H ? d.y + d.h : top + H;
                if (y1 > y0) todo[n++] = (ui_irect){ d.x, y0, d.w, y1 - y0 };
            }
    }
    double drawn = 0;
    if (n) {
        cairo_surface_t *cs = cairo_image_surface_create_for_data(r->data, CAIRO_FORMAT_RGB24, W, 2 * H, W * 4);
        cairo_surface_set_device_scale(cs, k, k);
        cairo_t *cr = cairo_create(cs);
        for (int i = 0; i < n; i++) {
            draw_rows(l, r, cr, todo[i]);
            drawn += (double)todo[i].w * todo[i].h;
        }
        cairo_destroy(cr);
        cairo_surface_flush(cs);
        cairo_surface_destroy(cs);
    }
    r->top = top;
    r->frame = l->frame_no++;
    r->busy = true;

    /* Show the slice [r0, r0 + H): one contiguous window of the content. */
    int r0 = ((top % H) + H) % H;
    wl_subsurface_set_position(l->sub, (int)(l->area.x + l->s->m), (int)(l->area.y + l->s->m));   /* inside any shadow margin */
    wl_surface_attach(l->part, r->wb, 0, 0);
    wp_viewport_set_source(l->vp, wl_fixed_from_int(0), wl_fixed_from_int(r0), wl_fixed_from_int(W), wl_fixed_from_int(H));
    wp_viewport_set_destination(l->vp, (int)l->area.w, (int)l->area.h);
    /* The view moved, so all of it differs from what is on screen; only
     * the slice shown, not the whole doubled buffer. */
    wl_surface_damage_buffer(l->part, 0, r0, W, H);
    wl_surface_commit(l->part);
    return drawn / ((double)W * H);
}

bool ui_layer_paste_shown(ui_layer *l, unsigned char *img, int stride)
{
    int k = l->scale, W = (int)l->area.w * k, H = (int)l->area.h * k;
    struct ring *r = NULL;
    for (int i = 0; i < RINGS; i++)
        if (l->rings[i].wb && l->rings[i].frame == l->frame_no - 1) r = &l->rings[i];
    if (!r || r->w != W || r->h != 2 * H) return false;
    int r0 = ((r->top % H) + H) % H;
    int x0 = (int)l->area.x * k, y0 = (int)l->area.y * k;
    for (int y = 0; y < H; y++)
        memcpy(img + (size_t)(y0 + y) * (size_t)stride + (size_t)x0 * 4,
               (unsigned char *)r->data + (size_t)(r0 + y) * (size_t)W * 4, (size_t)W * 4);
    return true;
}

void ui_layer_draw_fresh(ui_layer *l, cairo_t *cr)
{
    ui_rect clip = { l->area.x, l->offset, l->area.w, l->area.h };
    cairo_save(cr);
    cairo_rectangle(cr, l->area.x, l->area.y, l->area.w, l->area.h);
    cairo_clip(cr);
    cairo_translate(cr, 0, l->area.y - l->offset);
    l->draw(l->ctx, cr, clip);
    cairo_restore(cr);
}
