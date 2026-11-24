/*
 * A test client for osd-compositor: one xdg toplevel that asks for a
 * server-side frame (xdg-decoration) and fills itself with a colour, at
 * whatever size it is configured to (400x300 when the compositor leaves
 * it the choice). It logs every configure and exits when asked to close.
 *
 * usage: ssd-client TITLE [RRGGBB] [csd]   (csd: no decoration asked for)
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct wl_surface *surf;
static struct xdg_surface *xs;
static struct xdg_toplevel *top;
static int w = 400, h = 300, pend_w, pend_h;
static uint32_t color = 0xff3060a0;
static bool running = true, maximized;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
        deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void ping(void *d, struct xdg_wm_base *b, uint32_t s) { (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = { ping };

static void draw(void)
{
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("ssd-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) return;
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[y * w + x] = ((x / 20 + y / 20) % 2) ? color : (color & 0xff000000) | ((color & 0xfefefe) >> 1);
    munmap(px, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, w, h);
    wl_surface_commit(surf);
}

static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(x, serial);
    if (pend_w > 0 && pend_h > 0) { w = pend_w; h = pend_h; }
    fprintf(stderr, "ssd-client: configured %dx%d%s\n", w, h, maximized ? " maximized" : "");
    draw();
}
static const struct xdg_surface_listener xs_l = { xs_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t cw, int32_t ch, struct wl_array *states)
{
    (void)d; (void)t;
    pend_w = cw;
    pend_h = ch;
    maximized = false;
    uint32_t *s;
    wl_array_for_each(s, states) if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED) maximized = true;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; fprintf(stderr, "ssd-client: asked to close\n"); running = false; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t a, int32_t b) { (void)d; (void)t; (void)a; (void)b; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static void deco_mode(void *d, struct zxdg_toplevel_decoration_v1 *z, uint32_t mode)
{
    (void)d; (void)z;
    fprintf(stderr, "ssd-client: decoration %s\n", mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ? "server side" : "client side");
}
static const struct zxdg_toplevel_decoration_v1_listener deco_l = { deco_mode };

int main(int argc, char **argv)
{
    const char *title = argc > 1 ? argv[1] : "Test";
    if (argc > 2) color = 0xff000000 | (uint32_t)strtoul(argv[2], NULL, 16);
    bool csd = argc > 3 && !strcmp(argv[3], "csd");
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "ssd-client: no display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "ssd-client: missing globals\n"); return 1; }
    xdg_wm_base_add_listener(wm, &wm_l, NULL);
    surf = wl_compositor_create_surface(comp);
    xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, title);
    xdg_toplevel_set_app_id(top, "io.github.sungsookhim.Objectspace.test.ssd");
    if (deco_mgr && !csd) {
        struct zxdg_toplevel_decoration_v1 *deco = zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr, top);
        zxdg_toplevel_decoration_v1_add_listener(deco, &deco_l, NULL);
        zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(surf);
    while (running && wl_display_dispatch(dpy) != -1) {}
    wl_display_disconnect(dpy);
    return 0;
}
