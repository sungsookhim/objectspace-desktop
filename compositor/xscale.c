/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Sharp X11 programs on scaled screens (0.7 step 4; v0.6-design 7).
 *
 * Without it, an X11 program (XWayland) on a screen scaled to 2 draws at
 * 1 and is stretched: blurry. With it (the System's "Scale X11 programs
 * themselves", on), X11 programs live in device pixels: XWayland is told
 * the screens' places and sizes multiplied by the scale (an xdg-output of
 * our own, shown to XWayland alone: other programs see none), each X11
 * window is shown at 1/scale of its pixels (not stretched), the
 * compositor's places and sizes for X11 windows are multiplied and
 * divided as they go in and out, and the programs are asked to draw at
 * the scale: Xft.dpi (fonts, most toolkits) through xrdb, and GDK_SCALE
 * (GTK 3, Java, Chromium; a whole scale only) for programs the desktop
 * starts.
 *
 * One scale for all X11 programs: the largest screen's (as GNOME's
 * xwayland-native-scaling); on a screen with a smaller scale they are
 * drawn smaller. Off, or no screen scaled: 1, as before.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "comp.h"
#include "xdg-output-unstable-v1-protocol.h"

double comp_xscale = 1;
static bool enabled = true;
static struct wl_global *global;
static struct wl_list xouts;   /* struct xout.link */

struct xout {
    struct wl_list link;
    struct wl_resource *res, *output_res;
};

/* ---- the xdg-output XWayland sees */

static void send_one(struct xout *xo, bool done)
{
    struct wlr_output *o = wlr_output_from_resource(xo->output_res);
    if (!o) return;
    struct wlr_box b;
    wlr_output_layout_get_box(server.output_layout, o, &b);
    if (wlr_box_empty(&b)) return;
    double s = comp_xscale;
    zxdg_output_v1_send_logical_position(xo->res, (int32_t)lround(b.x * s), (int32_t)lround(b.y * s));
    zxdg_output_v1_send_logical_size(xo->res, (int32_t)lround(b.width * s), (int32_t)lround(b.height * s));
    int v = wl_resource_get_version(xo->res);
    if (v >= 2 && done) {   /* (once, at first) */
        zxdg_output_v1_send_name(xo->res, o->name);
        if (o->description) zxdg_output_v1_send_description(xo->res, o->description);
    }
    if (v < 3) zxdg_output_v1_send_done(xo->res);
    else if (wl_resource_get_version(xo->output_res) >= 2) wl_output_send_done(xo->output_res);
}

static void xout_destroy(struct wl_resource *res)
{
    struct xout *xo = wl_resource_get_user_data(res);
    if (!xo) return;
    wl_list_remove(&xo->link);
    free(xo);
}

static void req_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }

static const struct zxdg_output_v1_interface xout_impl = { .destroy = req_destroy };

static void get_xdg_output(struct wl_client *c, struct wl_resource *mgr, uint32_t id, struct wl_resource *output)
{
    struct wl_resource *res = wl_resource_create(c, &zxdg_output_v1_interface, wl_resource_get_version(mgr), id);
    if (!res) { wl_client_post_no_memory(c); return; }
    struct xout *xo = calloc(1, sizeof *xo);
    xo->res = res;
    xo->output_res = output;
    wl_resource_set_implementation(res, &xout_impl, xo, xout_destroy);
    wl_list_insert(&xouts, &xo->link);
    send_one(xo, true);
}

static const struct zxdg_output_manager_v1_interface mgr_impl = { .destroy = req_destroy, .get_xdg_output = get_xdg_output };

static void bind_mgr(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *res = wl_resource_create(c, &zxdg_output_manager_v1_interface, (int)version, id);
    if (!res) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(res, &mgr_impl, NULL, NULL);
}

/* Our xdg-output for XWayland alone (other programs have none, as before). */
static bool filter(const struct wl_client *client, const struct wl_global *g, void *data)
{
    (void)data;
    if (g != global) return true;
    return server.xwayland && server.xwayland->server && client == server.xwayland->server->client;
}

void xscale_init(void)
{
    wl_list_init(&xouts);
    global = wl_global_create(server.display, &zxdg_output_manager_v1_interface, 3, NULL, bind_mgr);
    wl_display_set_global_filter(server.display, filter, NULL);
}

/* ---- the scale */

/* Xft.dpi for X11 programs (xrdb, when installed): 96 times the scale.
 * Scaled, xterm a scalable font too (its own, a bitmap one, stays the
 * size of its pixels: tiny at 2): Latin monospace families named first;
 * "Monospace" alone in a Korean (Chinese, Japanese) locale is the CJK
 * one, whose widest letters made every cell double width (the owner's,
 * on the VM in Korean). Wide characters come from fontconfig's others. */
static void set_xft_dpi(void)
{
    if (!server.xwayland || !server.xwayland->display_name) return;
    char cmd[512];
    snprintf(cmd, sizeof cmd, "command -v xrdb >/dev/null 2>&1 && printf 'Xft.dpi: %d\\n%s' | DISPLAY=%s xrdb -merge", (int)lround(96 * comp_xscale),
             comp_xscale != 1 ? "XTerm*faceName: Noto Sans Mono,DejaVu Sans Mono,Liberation Mono,Adwaita Mono,Monospace\\nXTerm*faceSize: 10\\n" : "", server.xwayland->display_name);
    char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    comp_spawn(argv, NULL, NULL);
}

/* The desktop told the scale, and GDK_SCALE for the programs it starts:
 * the scale when every screen has that one, a whole one (GTK 3 takes it
 * on Wayland too, where it would be wrong on a screen at another), else
 * none. "X TAB scale TAB gdk" */
static void tell_desktop(void)
{
    int gdk = 0;
    if (comp_xscale >= 2 && fabs(comp_xscale - lround(comp_xscale)) < 0.001) {
        gdk = (int)lround(comp_xscale);
        struct output *o;
        wl_list_for_each(o, &server.outputs, link)
            if (fabs(o->wlr_output->scale - comp_xscale) > 0.001) gdk = 0;
    }
    char line[64];
    snprintf(line, sizeof line, "X\t%g\t%d\n", comp_xscale, gdk);
    desktop_send(line);
}

/* The scale X11 programs draw at now: the largest screen's, or 1. When it
 * changed: XWayland told the screens again, X11 windows placed again, the
 * desktop told (for GDK_SCALE in what it starts). */
void xscale_update(void)
{
    static bool env_read;
    if (!env_read) {   /* OSD_X11_SCALE=0: off from the start (the System's setting comes later) */
        env_read = true;
        const char *e = getenv("OSD_X11_SCALE");
        if (e && !strcmp(e, "0")) enabled = false;
    }
    double s = 1;
    if (enabled) {
        struct output *o;
        wl_list_for_each(o, &server.outputs, link) if (o->wlr_output->scale > s) s = o->wlr_output->scale;
    }
    bool changed = fabs(s - comp_xscale) > 0.001;
    double old = comp_xscale;
    comp_xscale = s;
    struct xout *xo;
    if (xouts.next) wl_list_for_each(xo, &xouts, link) send_one(xo, false);   /* (places too: a screen moved; none before xscale_init) */
    if (!changed) return;
    fprintf(stderr, "osd-compositor: X11 programs at scale %g\n", s);
    comp_x11_rescaled_from(old);
    set_xft_dpi();
    tell_desktop();
}

void xscale_set_enabled(bool on)
{
    if (on == enabled) return;
    enabled = on;
    xscale_update();
}

/* XWayland ready: its Xft.dpi, and the desktop told the scale. */
void xscale_ready(void)
{
    if (comp_xscale != 1) set_xft_dpi();
    tell_desktop();
}
