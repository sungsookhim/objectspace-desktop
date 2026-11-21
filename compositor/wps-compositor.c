/*
 * wps-compositor (G5, stage 2): a Wayland compositor on wlroots for the
 * WPS session.
 *
 * Every program that agrees to it (xdg-decoration, server side) gets WPS's
 * own frame, drawn by the same code as the toolkit's windows
 * (gui/ui_framepaint.c): title bar, system menu, minimize, maximize and
 * close buttons, sizing border. Programs that draw their own title bars
 * (GTK 4, libadwaita, WPS's own) are left as they are (gui-design.md:
 * Stage 2). The window manager: click to focus and raise, move by the title
 * bar, size by the border, maximize by the button or a double click on the
 * title (OS/2), Alt+Tab, Alt+F4. WPS's desktop (app ID org.wps.Desktop)
 * stays below all windows and covers the screen, the LaunchPad
 * (org.wps.LaunchPad) above them.
 *
 * usage: wps-compositor [-d DESKTOP-COMMAND] [-s COMMAND]
 *   -d: WPS's desktop (wps-shell --desktop FOLDER): started, and started
 *       again if it ends, as the GNOME extension does (desktop.c).
 *   -s: run COMMAND (sh -c) once the compositor is up, with WAYLAND_DISPLAY.
 * Runs nested in a Wayland or X11 session, or on its own on a console
 * (wlroots chooses: WLR_BACKENDS). Ctrl+Alt+BackSpace ends it.
 *
 * Tests: WPS_COMP_HEADLESS=WxH runs with no display, one virtual screen;
 * WPS_COMP_SCRIPT=file plays timed input and takes screenshots:
 *   <seconds> move X Y | click X Y [right] | down X Y | up X Y | dclick X Y
 *   <seconds> key [ctrl+][alt+][shift+]NAME   (an xkb key name: Tab, F4, a)
 *   <seconds> shot FILE.png | windows | hang SECONDS | quit
 */
#define _GNU_SOURCE
#include <assert.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/prctl.h>
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "comp.h"

struct server server;
bool comp_debug;   /* WPS_COMP_DEBUG */
static double start_ms;   /* (the session log says when, from here) */
static double now_ms(void);
double comp_uptime(void) { return (now_ms() - start_ms) / 1000; }
/* For the heartbeat in the log: what happened since the start. */
static unsigned long n_frames, n_keys, n_buttons, n_motions;

static bool from_desktop(struct toplevel *t);
static ui_rect client_rect(struct toplevel *t);

/* --------------------------------------------------------------- views */

/* A window is a Wayland one (xdg) or an X11 one (XWayland); these say
 * what both are, and do to either what the window manager does. */

const char *view_title(struct toplevel *t) { return t->xdg ? t->xdg->title : t->xw->title; }
const char *view_app(struct toplevel *t) { return t->xdg ? t->xdg->app_id : t->xw->class; }

struct wl_client *view_client(struct toplevel *t)
{
    if (t->xdg) return wl_resource_get_client(t->xdg->resource);
    return t->xw->surface && t->xw->surface->resource ? wl_resource_get_client(t->xw->surface->resource) : NULL;
}

struct wlr_box view_geo(struct toplevel *t)
{
    if (t->xdg) return t->xdg->base->geometry;
    return (struct wlr_box){ 0, 0, t->xw->width, t->xw->height };
}

static struct wlr_surface *view_surface(struct toplevel *t) { return t->xdg ? t->xdg->base->surface : t->xw->surface; }
static bool view_ready(struct toplevel *t) { return t->xdg ? t->xdg->base->initialized : true; }
static void view_schedule(struct toplevel *t) { if (t->xdg) wlr_xdg_surface_schedule_configure(t->xdg->base); }

/* An X11 window is told where it is on the screen (its menus open there). */
static void x11_configure(struct toplevel *t, int w, int h)
{
    ui_rect c = t->ssd ? client_rect(t) : (ui_rect){ 0, 0, 0, 0 };
    wlr_xwayland_surface_configure(t->xw, (int16_t)(t->tree->node.x + c.x), (int16_t)(t->tree->node.y + c.y),
                                   (uint16_t)(w > 0 ? w : t->xw->width), (uint16_t)(h > 0 ? h : t->xw->height));
}

static void view_move(struct toplevel *t, int x, int y)
{
    wlr_scene_node_set_position(&t->tree->node, x, y);
    if (t->xw) x11_configure(t, 0, 0);
}

static void view_set_size(struct toplevel *t, int w, int h)
{
    if (t->xdg) wlr_xdg_toplevel_set_size(t->xdg, w, h);
    else if (w > 0 && h > 0) x11_configure(t, w, h);
}

static void view_activate(struct toplevel *t, bool on)
{
    if (t->xdg) { wlr_xdg_toplevel_set_activated(t->xdg, on); return; }
    wlr_xwayland_surface_activate(t->xw, on);
    if (on) wlr_xwayland_surface_restack(t->xw, NULL, XCB_STACK_MODE_ABOVE);
}

static void view_set_fullscreen(struct toplevel *t, bool on)
{
    if (t->xdg) wlr_xdg_toplevel_set_fullscreen(t->xdg, on);
    else wlr_xwayland_surface_set_fullscreen(t->xw, on);
}

static void view_set_maximized(struct toplevel *t, bool on)
{
    if (t->xdg) wlr_xdg_toplevel_set_maximized(t->xdg, on);
    else wlr_xwayland_surface_set_maximized(t->xw, on, on);
}

static void view_close(struct toplevel *t)
{
    if (t->xdg) wlr_xdg_toplevel_send_close(t->xdg);
    else wlr_xwayland_surface_close(t->xw);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------- frames */

/* A Cairo image as a wlroots buffer, for the scene. */
struct cairo_buffer { struct wlr_buffer base; cairo_surface_t *surface; };

static void cb_destroy(struct wlr_buffer *b)
{
    struct cairo_buffer *c = wl_container_of(b, c, base);
    cairo_surface_destroy(c->surface);
    free(c);
}

static bool cb_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
    struct cairo_buffer *c = wl_container_of(b, c, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
    *data = cairo_image_surface_get_data(c->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)cairo_image_surface_get_stride(c->surface);
    return true;
}

static void cb_end(struct wlr_buffer *b) { (void)b; }

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cb_destroy, .begin_data_ptr_access = cb_begin, .end_data_ptr_access = cb_end,
};

static ui_frame_geom geom_of(struct toplevel *t, int w, int h)
{
    return (ui_frame_geom){ .w = w, .h = h, .maximized = t->maximized, .activated = t->activated,
                            .pointer_inside = t->hover != UI_HIT_NONE, .hover = t->hover, .pressed = t->pressed };
}

/* The frame's size around a client geometry of cw x ch. */
static void frame_size(struct toplevel *t, int cw, int ch, int *w, int *h)
{
    double b = t->maximized ? 0 : ui_scheme_now.border;
    *w = cw + (int)(2 * b);
    *h = ch + (int)(2 * b + ui_scheme_now.title_h);
}

static ui_rect client_rect(struct toplevel *t)
{
    if (!t->ssd || t->fullscreen) return (ui_rect){ 0, 0, 0, 0 };
    ui_frame_geom g = geom_of(t, t->fw, t->fh);
    return ui_frame_geom_client(&g);
}

static void draw_frame(struct toplevel *t)
{
    if (!t->ssd || !t->mapped || t->fullscreen) {
        wlr_scene_node_set_enabled(&t->frame->node, false);
        if (t->fullscreen && t->client) {   /* the program's surfaces alone */
            struct wlr_box geo = view_geo(t);
            wlr_scene_node_set_position(&t->client->node, -geo.x, -geo.y);
        }
        return;
    }
    struct wlr_box geo = view_geo(t);
    int w, h;
    frame_size(t, geo.width, geo.height, &w, &h);
    t->fw = w;
    t->fh = h;
    if (w <= 0 || h <= 0) return;
    /* drawn at the scale of the screen it is on (sharp on a scaled one),
     * shown at its size */
    double sc = comp_scale_at(t->tree->node.x + w / 2.0, t->tree->node.y + h / 2.0);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(w * sc), (int)ceil(h * sc));
    cairo_t *cr = cairo_create(s);
    cairo_scale(cr, sc, sc);
    ui_frame_geom g = geom_of(t, w, h);
    const char *title = view_title(t) && *view_title(t) ? view_title(t) : view_app(t) ? view_app(t) : "";
    ui_frame_geom_draw(&g, cr, (ui_rect){ 0, 0, w, h }, title);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    struct cairo_buffer *cb = calloc(1, sizeof *cb);
    wlr_buffer_init(&cb->base, &cairo_buffer_impl, cairo_image_surface_get_width(s), cairo_image_surface_get_height(s));
    cb->surface = s;
    wlr_scene_buffer_set_buffer(t->frame, &cb->base);
    wlr_scene_buffer_set_dest_size(t->frame, w, h);
    wlr_buffer_drop(&cb->base);
    wlr_scene_node_set_enabled(&t->frame->node, true);
    /* the program's surfaces go in the frame's client area */
    ui_rect c = client_rect(t);
    if (t->client) wlr_scene_node_set_position(&t->client->node, (int)c.x - geo.x, (int)c.y - geo.y);
}

/* The window's box on screen: its frame, or the program's own geometry. */
static struct wlr_box window_box(struct toplevel *t)
{
    struct wlr_box geo = view_geo(t);
    struct wlr_box b = { t->tree->node.x, t->tree->node.y, geo.width, geo.height };
    if (t->ssd && !t->fullscreen) { b.width = t->fw; b.height = t->fh; }
    return b;
}

/* ------------------------------------------------------------- focus */

static void set_activated(struct toplevel *t, bool on)
{
    if (t->activated == on) return;
    t->activated = on;
    if (t->fullscreen && t->mapped) {   /* above all only while active */
        wlr_scene_node_reparent(&t->tree->node, on ? server.layer_fullscreen : server.layer_normal);
        wlr_scene_node_raise_to_top(&t->tree->node);
    }
    view_activate(t, on);
    draw_frame(t);
    desktop_window(t, "change");
}

static void focus(struct toplevel *t)
{
    struct server *s = &server;
    if (!t) return;
    if (t->kind == KIND_DESKTOP) {   /* the desktop takes the keyboard, but stays below */
    } else {
        wlr_scene_node_raise_to_top(&t->tree->node);
    }
    struct toplevel *prev = wl_list_empty(&s->toplevels) ? NULL : wl_container_of(s->toplevels.next, prev, link);
    if (prev && prev != t) set_activated(prev, false);
    wl_list_remove(&t->link);
    wl_list_insert(&s->toplevels, &t->link);
    set_activated(t, true);
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
    DBG("focus '%s' (%s)%s\n", view_title(t) ? view_title(t) : "", view_app(t) ? view_app(t) : "", kb ? "" : " - no keyboard on the seat");
    if (kb) wlr_seat_keyboard_notify_enter(s->seat, view_surface(t), kb->keycodes, kb->num_keycodes, &kb->modifiers);
}

static void minimize(struct toplevel *t, bool on)
{
    if (t->minimized == on) { if (!on) focus(t); return; }
    t->minimized = on;
    if (t->xw) wlr_xwayland_surface_set_minimized(t->xw, on);
    desktop_event(t, on ? "window.minimize" : "window.restore");
    desktop_window(t, "change");
    wlr_scene_node_set_enabled(&t->tree->node, !on);
    if (on) {
        set_activated(t, false);
        /* the next window takes the keyboard */
        struct toplevel *n;
        wl_list_for_each(n, &server.toplevels, link)
            if (n != t && n->mapped && !n->minimized && n->kind == KIND_NORMAL) { focus(n); return; }
        wlr_seat_keyboard_notify_clear_focus(server.seat);
    } else {
        focus(t);
    }
}

static struct wlr_output *output_at_window(struct toplevel *t)
{
    struct wlr_box b = window_box(t);
    struct wlr_output *o = wlr_output_layout_output_at(server.output_layout, b.x + b.width / 2.0, b.y + b.height / 2.0);
    return o ? o : wlr_output_layout_output_at(server.output_layout, server.cursor->x, server.cursor->y);
}

static void set_fullscreen(struct toplevel *t, bool on);

static void maximize(struct toplevel *t, bool on)
{
    if (t->fullscreen) set_fullscreen(t, false);
    if (t->maximized == on || t->kind != KIND_NORMAL) { view_schedule(t); return; }
    struct wlr_output *o = output_at_window(t);
    if (!o) return;
    struct wlr_box ob;
    wlr_output_layout_get_box(server.output_layout, o, &ob);
    if (on) {
        t->restore = window_box(t);
        t->maximized = true;
        int cw = ob.width, ch = ob.height;
        if (t->ssd) ch -= (int)ui_scheme_now.title_h;   /* no border when maximized */
        view_move(t, ob.x, ob.y);
        view_set_size(t, cw, ch);
    } else {
        t->maximized = false;
        int cw = t->restore.width, ch = t->restore.height;
        if (t->ssd) {
            cw -= (int)(2 * ui_scheme_now.border);
            ch -= (int)(2 * ui_scheme_now.border + ui_scheme_now.title_h);
        }
        view_move(t, t->restore.x, t->restore.y);
        view_set_size(t, cw, ch);
    }
    view_set_maximized(t, on);
    desktop_event(t, on ? "window.maximize" : "window.restore");
    desktop_window(t, "change");
}

/* Fullscreen (a video, a presentation, a game, asked by the program): no
 * frame, the whole screen it is on, above everything (the LaunchPad too)
 * while it is the active window; back below when another is activated.
 * Leaving it, the window has its box (or maximized size) again. */
static void set_fullscreen(struct toplevel *t, bool on)
{
    if (t->kind != KIND_NORMAL || t->fullscreen == on) { view_set_fullscreen(t, t->fullscreen); view_schedule(t); return; }
    struct wlr_output *o = output_at_window(t);
    if (!o) return;
    struct wlr_box ob;
    wlr_output_layout_get_box(server.output_layout, o, &ob);
    if (on) {
        t->fs_restore = window_box(t);
        t->fullscreen = true;
        if (t->activated) wlr_scene_node_reparent(&t->tree->node, server.layer_fullscreen);
        draw_frame(t);
        view_move(t, ob.x, ob.y);
        view_set_size(t, ob.width, ob.height);
    } else {
        t->fullscreen = false;
        wlr_scene_node_reparent(&t->tree->node, server.layer_normal);
        wlr_scene_node_raise_to_top(&t->tree->node);
        draw_frame(t);
        if (t->maximized) {
            int ch = ob.height - (t->ssd ? (int)ui_scheme_now.title_h : 0);
            view_move(t, ob.x, ob.y);
            view_set_size(t, ob.width, ch);
        } else {
            int cw = t->fs_restore.width, ch = t->fs_restore.height;
            if (t->ssd) {
                cw -= (int)(2 * ui_scheme_now.border);
                ch -= (int)(2 * ui_scheme_now.border + ui_scheme_now.title_h);
            }
            view_move(t, t->fs_restore.x, t->fs_restore.y);
            view_set_size(t, cw, ch);
        }
    }
    view_set_fullscreen(t, on);
    DBG("fullscreen %s '%s'\n", on ? "on" : "off", view_title(t) ? view_title(t) : "");
    desktop_window(t, "change");
}

/* ------------------------------------------------------- what is where */

/* The window under (x, y), with the surface (and its local position)
 * when it is the program's, or the frame part when it is ours. */
static struct toplevel *window_at(double x, double y, struct wlr_surface **surface, double *sx, double *sy, ui_hit *part)
{
    *surface = NULL;
    *part = UI_HIT_NONE;
    struct wlr_scene_node *node = wlr_scene_node_at(&server.scene->tree.node, x, y, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
    struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
    if (node->data) {   /* a frame */
        struct tag *tg = node->data;
        struct toplevel *t = tg->t;
        ui_frame_geom g = geom_of(t, t->fw, t->fh);
        *part = ui_frame_geom_hit(&g, *sx, *sy);
        return t;
    }
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buf);
    if (!ss) return NULL;
    *surface = ss->surface;
    struct wlr_scene_tree *tree = node->parent;
    while (tree && !tree->node.data) tree = tree->node.parent;
    if (!tree) return NULL;
    struct tag *tg = tree->node.data;
    return tg ? tg->t : NULL;
}

/* ---------------------------------------------------------- system menu */

/* A framed window's system menu (as the toolkit's frames have): Restore or
 * Maximize, Minimize, Close; drawn by the compositor, under the system
 * menu button. */
#define MENU_ITEM_H 22
#define MENU_W 150
static struct {
    struct wlr_scene_buffer *buf;
    struct toplevel *t;
    int x, y, w, h;
    int hot;
} menu = { .hot = -1 };

static const char *menu_label(int i)
{
    switch (i) {
    case 0: return menu.t && menu.t->maximized ? "Restore" : "Maximize";
    case 1: return "Minimize";
    case 3: return "Close";
    default: return NULL;   /* 2: a separator */
    }
}

static int menu_y(int i) { return 3 + (i > 2 ? 2 * MENU_ITEM_H + 8 : i * MENU_ITEM_H); }

static void menu_draw(void)
{
    const ui_scheme *k = &ui_scheme_now;
    double sc = comp_scale_at(menu.buf->node.x + menu.w / 2.0, menu.buf->node.y + menu.h / 2.0);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(menu.w * sc), (int)ceil(menu.h * sc));
    cairo_t *cr = cairo_create(s);
    cairo_scale(cr, sc, sc);
    ui_fill(cr, (ui_rect){ 0, 0, menu.w, menu.h }, k->menu_bg);
    ui_bevel(cr, (ui_rect){ 0, 0, menu.w, menu.h }, true);
    for (int i = 0; i < 4; i++) {
        const char *l = menu_label(i);
        int y = menu_y(i);
        if (!l) {
            ui_fill(cr, (ui_rect){ 4, y + 3, menu.w - 8, 1 }, k->shadow);
            ui_fill(cr, (ui_rect){ 4, y + 4, menu.w - 8, 1 }, k->light);
            continue;
        }
        if (i == menu.hot) ui_fill(cr, (ui_rect){ 3, y, menu.w - 6, MENU_ITEM_H }, k->menu_hilite);
        ui_set_color(cr, i == menu.hot ? k->menu_hilite_text : k->menu_text);
        ui_text_draw(cr, UI_FONT_REGULAR, 16, y + (MENU_ITEM_H + ui_text_ascent()) / 2 - 1, l, menu.w - 20);
    }
    cairo_destroy(cr);
    cairo_surface_flush(s);
    struct cairo_buffer *cb = calloc(1, sizeof *cb);
    wlr_buffer_init(&cb->base, &cairo_buffer_impl, cairo_image_surface_get_width(s), cairo_image_surface_get_height(s));
    cb->surface = s;
    wlr_scene_buffer_set_buffer(menu.buf, &cb->base);
    wlr_scene_buffer_set_dest_size(menu.buf, menu.w, menu.h);
    wlr_buffer_drop(&cb->base);
}

static void menu_close(void)
{
    if (!menu.t) return;
    menu.t = NULL;
    wlr_scene_node_set_enabled(&menu.buf->node, false);
}

static void menu_open(struct toplevel *t, bool keyboard)
{
    if (!t->ssd || !menu.buf) return;
    menu.t = t;
    menu.hot = keyboard ? 0 : -1;
    ui_frame_geom g = geom_of(t, t->fw, t->fh);
    ui_rect b = ui_frame_geom_button(&g, 0);
    menu.w = MENU_W;
    menu.h = menu_y(3) + MENU_ITEM_H + 3;
    menu.x = t->tree->node.x + (int)b.x;
    menu.y = t->tree->node.y + (int)(b.y + b.h);
    wlr_scene_node_set_position(&menu.buf->node, menu.x, menu.y);
    wlr_scene_node_raise_to_top(&menu.buf->node);
    wlr_scene_node_set_enabled(&menu.buf->node, true);
    menu_draw();
}

static int menu_item_at(double x, double y)
{
    if (x < menu.x || x >= menu.x + menu.w || y < menu.y || y >= menu.y + menu.h) return -2;   /* outside */
    for (int i = 0; i < 4; i++)
        if (menu_label(i) && y - menu.y >= menu_y(i) && y - menu.y < menu_y(i) + MENU_ITEM_H) return i;
    return -1;
}

static void frame_button(struct toplevel *t, ui_hit part);

static void menu_choose(int i)
{
    struct toplevel *t = menu.t;
    menu_close();
    if (!t || i < 0) return;
    frame_button(t, i == 0 ? UI_HIT_MAX : i == 1 ? UI_HIT_MIN : UI_HIT_CLOSE);
}

/* Keys while the menu is open: true if taken. */
static bool menu_key(xkb_keysym_t sym)
{
    if (!menu.t) return false;
    static const int order[] = { 0, 1, 3 };
    int at = 0;
    for (int k = 0; k < 3; k++) if (order[k] == menu.hot) at = k;
    switch (sym) {
    case XKB_KEY_Down: menu.hot = order[(at + 1) % 3]; menu_draw(); return true;
    case XKB_KEY_Up: menu.hot = order[(at + 2) % 3]; menu_draw(); return true;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space: menu_choose(menu.hot); return true;
    case XKB_KEY_Escape: menu_close(); return true;
    default: return true;   /* nothing else while it is open */
    }
}

/* -------------------------------------------------------------- pointer */

static const char *edge_cursor(uint32_t e)
{
    switch (e) {
    case WLR_EDGE_TOP: return "n-resize";
    case WLR_EDGE_BOTTOM: return "s-resize";
    case WLR_EDGE_LEFT: return "w-resize";
    case WLR_EDGE_RIGHT: return "e-resize";
    case WLR_EDGE_TOP | WLR_EDGE_LEFT: return "nw-resize";
    case WLR_EDGE_TOP | WLR_EDGE_RIGHT: return "ne-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT: return "sw-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT: return "se-resize";
    default: return "default";
    }
}

static uint32_t edges_at(struct toplevel *t, double lx, double ly)
{
    double c = ui_scheme_now.border * 4;   /* corners are easier to grab */
    uint32_t e = 0;
    if (ly < c) e |= WLR_EDGE_TOP;
    if (ly >= t->fh - c) e |= WLR_EDGE_BOTTOM;
    if (lx < c) e |= WLR_EDGE_LEFT;
    if (lx >= t->fw - c) e |= WLR_EDGE_RIGHT;
    return e;
}

static void set_cursor(const char *name)
{
    if (server.cursor_image && !strcmp(server.cursor_image, name)) return;
    server.cursor_image = name;
    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, name);
}

static void set_hover(struct toplevel *t, ui_hit h)
{
    if (!t || t->hover == h) return;
    t->hover = h;
    draw_frame(t);
}

static struct toplevel *hovered;
static void drag_motion(uint32_t time);

static void begin_grab(struct toplevel *t, enum mode m, uint32_t edges)
{
    if (t->kind != KIND_NORMAL && t->kind != KIND_PALETTE) return;
    if (m == MODE_RESIZE && t->kind != KIND_NORMAL) return;
    server.grabbed = t;
    server.mode = m;
    struct wlr_box b = window_box(t);
    server.press_x = server.cursor->x;
    server.press_y = server.cursor->y;
    server.grab_x = server.cursor->x - (m == MODE_MOVE ? b.x : 0);
    server.grab_y = server.cursor->y - (m == MODE_MOVE ? b.y : 0);
    server.grab_box = b;
    server.resize_edges = edges;
    /* The pointer is the compositor's while it moves or sizes: the program
     * is told it left (as GNOME does), so it does not wait for a release
     * it will never get (its own buttons then did nothing). */
    wlr_seat_pointer_clear_focus(server.seat);
}

static void do_resize(void)
{
    struct toplevel *t = server.grabbed;
    double dx = server.cursor->x - server.grab_x, dy = server.cursor->y - server.grab_y;
    struct wlr_box b = server.grab_box;
    int x0 = b.x, y0 = b.y, x1 = b.x + b.width, y1 = b.y + b.height;
    int minw = t->ssd ? 160 : 60, minh = t->ssd ? 80 : 40;
    if (server.resize_edges & WLR_EDGE_LEFT) { x0 += (int)dx; if (x0 > x1 - minw) x0 = x1 - minw; }
    if (server.resize_edges & WLR_EDGE_RIGHT) { x1 += (int)dx; if (x1 < x0 + minw) x1 = x0 + minw; }
    if (server.resize_edges & WLR_EDGE_TOP) { y0 += (int)dy; if (y0 > y1 - minh) y0 = y1 - minh; }
    if (server.resize_edges & WLR_EDGE_BOTTOM) { y1 += (int)dy; if (y1 < y0 + minh) y1 = y0 + minh; }
    int cw = x1 - x0, ch = y1 - y0;
    if (t->ssd) {
        cw -= (int)(2 * ui_scheme_now.border);
        ch -= (int)(2 * ui_scheme_now.border + ui_scheme_now.title_h);
    }
    view_move(t, x0, y0);
    view_set_size(t, cw, ch);
}

static void process_motion(uint32_t time)
{
    struct server *s = &server;
    if (lock_active()) { set_cursor("default"); return; }   /* no program sees the pointer */
    if (s->seat->drag) { drag_motion(time); return; }
    if (s->mode == MODE_MOVE && s->grabbed->maximized) {
        /* a maximized window's title dragged (not just pressed): restored
         * under the pointer, at the same place across its title */
        if (fabs(s->cursor->x - s->press_x) < 5 && fabs(s->cursor->y - s->press_y) < 5) return;
        struct toplevel *t = s->grabbed;
        struct wlr_box b = window_box(t);
        double fx = (s->press_x - b.x) / (b.width ? b.width : 1);
        maximize(t, false);
        b = t->restore;
        s->grab_x = fx * b.width;
        s->grab_y = 8;
    }
    if (s->mode == MODE_MOVE) {
        view_move(s->grabbed, (int)(s->cursor->x - s->grab_x), (int)(s->cursor->y - s->grab_y));
        return;
    }
    if (s->mode == MODE_RESIZE) { do_resize(); return; }
    if (menu.t) {
        int i = menu_item_at(s->cursor->x, s->cursor->y);
        if (i >= -1 && i != menu.hot) { menu.hot = i; menu_draw(); }
        set_cursor("default");
        return;
    }
    double sx, sy;
    struct wlr_surface *surface;
    ui_hit part;
    struct toplevel *t = window_at(s->cursor->x, s->cursor->y, &surface, &sx, &sy, &part);
    if (s->mode == MODE_BUTTON) {   /* a frame button held: it shows pressed only while under the pointer */
        set_hover(s->grabbed, t == s->grabbed ? part : UI_HIT_NONE);
        return;
    }
    if (hovered && hovered != t) set_hover(hovered, UI_HIT_NONE);
    hovered = t && !surface ? t : NULL;
    if (t && !surface) {
        set_hover(t, part);
        set_cursor(part == UI_HIT_BORDER && !t->maximized ? edge_cursor(edges_at(t, sx, sy)) : "default");
        wlr_seat_pointer_clear_focus(s->seat);
        return;
    }
    if (!surface) {
        set_cursor("default");
        wlr_seat_pointer_clear_focus(s->seat);
        return;
    }
    s->cursor_image = NULL;   /* the program sets its own */
    wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
}

/* While something is dragged (drag and drop between or within programs):
 * the pointer goes to the program surface under it (the drag's grab turns
 * that into the drag's enter, motion and leave); WPS's frames take no part. */
/* What the drop would do, on the pointer (as GNOME shows it): a copy with
 * Ctrl, a shadow with Ctrl+Shift (WPS's keys, as OS/2's), nothing where
 * the place under it would not take it. */
static void drag_cursor(void)
{
    struct server *s = &server;
    struct wlr_drag *d = s->seat->drag;
    if (!d) return;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
    uint32_t m = kb ? wlr_keyboard_get_modifiers(kb) : 0;
    unsigned mods = (m & WLR_MODIFIER_CTRL ? UI_MOD_CTRL : 0) | (m & WLR_MODIFIER_SHIFT ? UI_MOD_SHIFT : 0) |
                    (m & WLR_MODIFIER_ALT ? UI_MOD_ALT : 0);
    int how = ui_mouse_drop_how(mods);   /* the Mouse object's mappings */
    if (!d->focus || !d->source || !d->source->accepted) set_cursor("no-drop");
    else if (how == 2) set_cursor("alias");
    else if (how == 1) set_cursor("copy");
    else if (how < 0) set_cursor("context-menu");
    else set_cursor("default");
}

static void drag_motion(uint32_t time)
{
    struct server *s = &server;
    if (s->drag_icon) wlr_scene_node_set_position(&s->drag_icon->node, (int)s->cursor->x, (int)s->cursor->y);
    drag_cursor();
    double sx, sy;
    struct wlr_surface *surface;
    ui_hit part;
    window_at(s->cursor->x, s->cursor->y, &surface, &sx, &sy, &part);
    if (!surface) { wlr_seat_pointer_clear_focus(s->seat); return; }
    wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
}

static void frame_button(struct toplevel *t, ui_hit part)
{
    switch (part) {
    case UI_HIT_MIN: minimize(t, true); break;
    case UI_HIT_MAX: maximize(t, !t->maximized); break;
    case UI_HIT_CLOSE: view_close(t); break;
    case UI_HIT_SYSMENU: menu_open(t, false); break;
    default: break;
    }
}

static bool menu_release_ignored;   /* the release of the click that opened it */

static void process_button(uint32_t time, uint32_t button, bool pressed)
{
    struct server *s = &server;
    if (lock_active()) return;
    if (s->seat->drag) {   /* the release drops it */
        wlr_seat_pointer_notify_button(s->seat, time, button,
                                       pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        return;
    }
    if (menu.t) {
        int i = menu_item_at(s->cursor->x, s->cursor->y);
        if (!pressed && menu_release_ignored) { menu_release_ignored = false; if (i < 0) return; }
        if (!pressed) { if (i >= 0) menu_choose(i); return; }
        if (i == -2) menu_close();   /* a click elsewhere: closed, nothing else */
        return;
    }
    if (!pressed) {
        if (s->mode == MODE_BUTTON && s->grabbed) {
            struct toplevel *t = s->grabbed;
            ui_hit p = t->pressed;
            t->pressed = UI_HIT_NONE;
            draw_frame(t);
            if (t->hover == p) frame_button(t, p);
            s->mode = MODE_PASS;
            s->grabbed = NULL;
            return;
        }
        if (s->mode != MODE_PASS) {
            s->mode = MODE_PASS;
            s->grabbed = NULL;
            process_motion(time);   /* the pointer enters what is under it again */
            return;
        }
        wlr_seat_pointer_notify_button(s->seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
        return;
    }
    double sx, sy;
    struct wlr_surface *surface;
    ui_hit part;
    struct toplevel *t = window_at(s->cursor->x, s->cursor->y, &surface, &sx, &sy, &part);
    DBG("button %#x at %.0f,%.0f on %s%s%s\n", button, s->cursor->x, s->cursor->y, t ? "'" : "nothing",
        t ? (view_title(t) ? view_title(t) : "") : "", t ? (surface ? "' (its surface)" : "' (WPS's frame)") : "");
    if (t && !t->activated) focus(t);
    else if (t && t->kind == KIND_NORMAL) wlr_scene_node_raise_to_top(&t->tree->node);
    if (t && !surface && button == 0x110 /* BTN_LEFT */) {
        double now = now_ms();
        bool dbl = s->last_click_on == t && now - s->last_click_ms < 400;
        s->last_click_on = t;
        s->last_click_ms = now;
        switch (part) {
        case UI_HIT_TITLE:
            if (dbl) { maximize(t, !t->maximized); s->last_click_on = NULL; }   /* as on OS/2 */
            else begin_grab(t, MODE_MOVE, 0);
            return;
        case UI_HIT_BORDER:
            if (!t->maximized) begin_grab(t, MODE_RESIZE, edges_at(t, sx, sy));
            return;
        case UI_HIT_SYSMENU:
            if (dbl) { menu_close(); view_close(t); return; }   /* OS/2 */
            menu_open(t, false);
            menu_release_ignored = true;
            return;
        case UI_HIT_MIN: case UI_HIT_MAX: case UI_HIT_CLOSE:
            t->pressed = part;
            draw_frame(t);
            s->mode = MODE_BUTTON;
            s->grabbed = t;
            return;
        default: return;
        }
    }
    if (t && !surface) return;
    wlr_seat_pointer_notify_button(s->seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED);
}

static void on_cursor_motion(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_motion_event *e = data;
    n_motions++;
    idle_activity();
    wlr_cursor_move(server.cursor, &e->pointer->base, e->delta_x, e->delta_y);
    process_motion(e->time_msec);
}

static void on_cursor_motion_absolute(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_motion_absolute_event *e = data;
    n_motions++;
    idle_activity();
    wlr_cursor_warp_absolute(server.cursor, &e->pointer->base, e->x, e->y);
    process_motion(e->time_msec);
}

static void on_cursor_button(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_button_event *e = data;
    n_buttons++;
    /* a click that wakes the screens does nothing else (nor its release) */
    static uint32_t swallowed;
    bool pressed = e->state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (idle_activity() && pressed) { swallowed = e->button; return; }
    if (!pressed && swallowed == e->button) { swallowed = 0; return; }
    process_button(e->time_msec, e->button, e->state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void on_cursor_axis(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_axis_event *e = data;
    if (idle_activity()) return;
    wlr_seat_pointer_notify_axis(server.seat, e->time_msec, e->orientation, e->delta, e->delta_discrete, e->source,
                                 e->relative_direction);
}

static void on_cursor_frame(struct wl_listener *l, void *data)
{
    (void)l; (void)data;
    wlr_seat_pointer_notify_frame(server.seat);
}

/* ------------------------------------------------------------- keyboard */

static void cycle_windows(void)
{
    /* Alt+Tab: the window that has been in front longest comes forward */
    struct toplevel *t, *last = NULL;
    wl_list_for_each(t, &server.toplevels, link)
        if (t->mapped && t->kind == KIND_NORMAL) last = t;
    if (!last) return;
    if (last->minimized) minimize(last, false);
    else focus(last);
}

/* The compositor's own keys: true if it took the key. */
static bool shortcut(uint32_t mods, xkb_keysym_t sym)
{
    if (menu_key(sym)) return true;
    if ((sym == XKB_KEY_Escape && (mods & WLR_MODIFIER_SHIFT)) || (sym == XKB_KEY_space && (mods & WLR_MODIFIER_ALT))) {
        /* the front window's system menu, if it has WPS's frame (others have their own) */
        struct toplevel *f = wl_list_empty(&server.toplevels) ? NULL : wl_container_of(server.toplevels.next, f, link);
        if (f && f->ssd && f->activated) { menu_open(f, true); return true; }
        return false;
    }
    if ((mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT) && sym == XKB_KEY_BackSpace) {
        wl_display_terminate(server.display);
        return true;
    }
    if ((mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT)) == WLR_MODIFIER_CTRL && sym == XKB_KEY_Escape) {
        /* OS/2's Window List: the desktop opens it, the compositor gives it the keyboard */
        if (desktop_key("window-list")) {
            server.focus_window_list = true;
            struct toplevel *t;   /* already open: forward */
            wl_list_for_each(t, &server.toplevels, link)
                if (t->mapped && view_app(t) && !strcmp(view_app(t), "org.wps.WindowList") && from_desktop(t)) {
                    if (t->minimized) minimize(t, false); else focus(t);
                    break;
                }
        }
        return true;
    }
    if ((mods & WLR_MODIFIER_LOGO) && (sym == XKB_KEY_l || sym == XKB_KEY_L)) { lock_screen(); return true; }   /* as in GNOME */
    if ((mods & WLR_MODIFIER_LOGO) && sym == XKB_KEY_space) { keymap_cycle(mods & WLR_MODIFIER_SHIFT ? -1 : 1); return true; }
    /* Ctrl+Alt+F1..F12 (xkb makes them XF86Switch_VT_n): another console */
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        if (server.session) wlr_session_change_vt(server.session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
        return true;
    }
    if (!(mods & WLR_MODIFIER_ALT)) return false;
    struct toplevel *front = wl_list_empty(&server.toplevels) ? NULL : wl_container_of(server.toplevels.next, front, link);
    switch (sym) {
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: cycle_windows(); return true;
    case XKB_KEY_F4: if (front && front->activated) view_close(front); return true;
    default: return false;
    }
}

static void on_key(struct wl_listener *l, void *data)
{
    struct keyboard *k = wl_container_of(l, k, key);
    struct wlr_keyboard_key_event *e = data;
    uint32_t keycode = e->keycode + 8;
    const xkb_keysym_t *syms;
    int n = xkb_state_key_get_syms(k->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t mods = wlr_keyboard_get_modifiers(k->wlr_keyboard);
    n_keys++;
    /* a key that wakes the screens is not typed into anything */
    if (idle_activity() && e->state == WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (comp_debug && e->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        char name[64] = "?";
        if (n > 0) xkb_keysym_get_name(syms[0], name, sizeof name);
        DBG("key %s (code %u, mods %#x) from %s, keyboard focus %s\n", name, keycode, mods,
            k->wlr_keyboard->base.name ? k->wlr_keyboard->base.name : "?",
            server.seat->keyboard_state.focused_surface ? "a window" : "none");
    }
    if (lock_active()) {   /* the lock screen has every key (consoles still switch) */
        if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED)
            for (int i = 0; i < n; i++) {
                if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) { shortcut(mods, syms[i]); continue; }
                char utf8[8] = "";
                xkb_state_key_get_utf8(k->wlr_keyboard->xkb_state, keycode, utf8, sizeof utf8);
                lock_key(syms[i], utf8);
            }
        return;
    }
    bool handled = false;
    if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED)
        for (int i = 0; i < n; i++) handled |= shortcut(mods, syms[i]);
    if (handled) return;
    wlr_seat_set_keyboard(server.seat, k->wlr_keyboard);
    wlr_seat_keyboard_notify_key(server.seat, e->time_msec, e->keycode, e->state);
}

static void on_modifiers(struct wl_listener *l, void *data)
{
    (void)data;
    struct keyboard *k = wl_container_of(l, k, modifiers);
    wlr_seat_set_keyboard(server.seat, k->wlr_keyboard);
    if (server.seat->drag) {
        /* A drag holds the keyboard and passes no keys on: the modifiers
         * go to the focused program all the same (WPS chooses move, copy
         * or shadow by them), and the place under the pointer is asked
         * again with them. */
        wlr_seat_keyboard_send_modifiers(server.seat, &k->wlr_keyboard->modifiers);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        drag_motion((uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
        return;
    }
    wlr_seat_keyboard_notify_modifiers(server.seat, &k->wlr_keyboard->modifiers);
}

static void on_keyboard_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct keyboard *k = wl_container_of(l, k, destroy);
    wl_list_remove(&k->modifiers.link);
    wl_list_remove(&k->key.link);
    wl_list_remove(&k->destroy.link);
    wl_list_remove(&k->link);
    free(k);
}

static void new_keyboard(struct wlr_input_device *dev)
{
    struct wlr_keyboard *wk = wlr_keyboard_from_input_device(dev);
    struct keyboard *k = calloc(1, sizeof *k);
    k->server = &server;
    k->wlr_keyboard = wk;
    struct xkb_keymap *km = keymap_new();   /* GNOME's layouts (keymap.c) */
    wlr_keyboard_set_keymap(wk, km);
    xkb_keymap_unref(km);
    wlr_keyboard_set_repeat_info(wk, 25, 400);
    k->modifiers.notify = on_modifiers;
    wl_signal_add(&wk->events.modifiers, &k->modifiers);
    k->key.notify = on_key;
    wl_signal_add(&wk->events.key, &k->key);
    k->destroy.notify = on_keyboard_destroy;
    wl_signal_add(&dev->events.destroy, &k->destroy);
    wlr_seat_set_keyboard(server.seat, wk);
    wl_list_insert(&server.keyboards, &k->link);
}

static void on_new_input(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_input_device *dev = data;
    static const char *const types[] = { "keyboard", "pointer", "touch", "tablet", "tablet pad", "switch" };
    fprintf(stderr, "wps-compositor: %.3f input: %s (%s)\n", (now_ms() - start_ms) / 1000, dev->name ? dev->name : "?",
            dev->type < sizeof types / sizeof *types ? types[dev->type] : "other");
    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) new_keyboard(dev);
    else if (dev->type == WLR_INPUT_DEVICE_POINTER) wlr_cursor_attach_input_device(server.cursor, dev);
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server.keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server.seat, caps);
}

static void on_request_cursor(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_pointer_request_set_cursor_event *e = data;
    if (server.seat->pointer_state.focused_client == e->seat_client)
        wlr_cursor_set_surface(server.cursor, e->surface, e->hotspot_x, e->hotspot_y);
}

/* A program starts dragging: only from a button it holds down (the
 * press's serial), as for moves; else the drag is refused. */
static void on_request_start_drag(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_request_start_drag_event *e = data;
    if (wlr_seat_validate_pointer_grab_serial(server.seat, e->origin, e->serial)) {
        wlr_seat_start_pointer_drag(server.seat, e->drag, e->serial);
        return;
    }
    DBG("drag refused (serial %u not a held button's)\n", e->serial);
    if (e->drag->source) wlr_data_source_destroy(e->drag->source);
}

/* The pointer back to what is under it (its program sets its own again). */
static void after_drag(void *data)
{
    (void)data;
    server.cursor_image = NULL;
    set_cursor("default");
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    process_motion((uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
}

static void on_drag_destroy(struct wl_listener *l, void *data)
{
    (void)l; (void)data;
    wl_list_remove(&server.drag_destroy.link);
    server.drag_icon = NULL;   /* (the scene's icon node goes with the icon) */
    DBG("drag ended\n");
    wl_event_loop_add_idle(server.loop, after_drag, NULL);   /* once the drag is gone */
}

static void on_start_drag(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_drag *drag = data;
    struct server *s = &server;
    DBG("drag started\n");
    if (s->mode == MODE_BUTTON && s->grabbed) { s->grabbed->pressed = UI_HIT_NONE; draw_frame(s->grabbed); }
    s->mode = MODE_PASS;
    s->grabbed = NULL;
    if (hovered) { set_hover(hovered, UI_HIT_NONE); hovered = NULL; }
    s->drag_icon = drag->icon ? wlr_scene_drag_icon_create(s->layer_drag, drag->icon) : NULL;
    if (s->drag_icon) wlr_scene_node_set_position(&s->drag_icon->node, (int)s->cursor->x, (int)s->cursor->y);
    s->drag_destroy.notify = on_drag_destroy;
    wl_signal_add(&drag->events.destroy, &s->drag_destroy);
}

static void on_request_set_selection(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_request_set_selection_event *e = data;
    wlr_seat_set_selection(server.seat, e->source, e->serial);
}

/* The primary selection: what is selected, pasted with the middle button
 * (terminals, X11 programs). */
static void on_request_set_primary(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_request_set_primary_selection_event *e = data;
    wlr_seat_set_primary_selection(server.seat, e->source, e->serial);
}

/* -------------------------------------------------------------- outputs */

static const char *shot_path;   /* a screenshot wanted at the next frame */

static void write_shot(struct wlr_buffer *buf, const char *path)
{
    void *data;
    uint32_t fmt;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
        fprintf(stderr, "wps-compositor: cannot read the screen for %s\n", path);
        return;
    }
    cairo_surface_t *s = cairo_image_surface_create_for_data(data, fmt == DRM_FORMAT_ARGB8888 ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
                                                             buf->width, buf->height, (int)stride);
    cairo_surface_write_to_png(s, path);
    cairo_surface_destroy(s);
    wlr_buffer_end_data_ptr_access(buf);
    fprintf(stderr, "wps-compositor: screenshot %s\n", path);
}

static void on_output_frame(struct wl_listener *l, void *data)
{
    (void)data;
    struct output *o = wl_container_of(l, o, frame);
    struct wlr_scene_output *so = wlr_scene_get_scene_output(server.scene, o->wlr_output);
    double t0 = now_ms();
    /* No faster than the screen refreshes. Some screens (a VM's) say a
     * frame was shown at once, without waiting for the refresh: programs,
     * told so, drew again at once (Firefox: ~1300 frames a second), until
     * VirtualBox's 3D gave out (black areas, frames stopped). A frame that
     * comes too soon waits for the rest of the refresh period. */
    double period = o->wlr_output->refresh > 0 ? 1e6 / o->wlr_output->refresh : 1000.0 / 60;
    if (!shot_path && t0 - o->last_frame_ms < period - 1) {
        wl_event_source_timer_update(o->pace, (int)(period - (t0 - o->last_frame_ms)) + 1);
        return;
    }
    if (!shot_path && !wlr_scene_output_needs_frame(so)) {
        /* nothing changed: no new frame (the scene asks for one when
         * something does). Committing anyway made the loop spin, frame
         * after frame, where a screen flips at once (a VM: ~49,000 a
         * second), so that input was hardly handled. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(so, &now);
        return;
    }
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    if (wlr_scene_output_build_state(so, &st, NULL)) {
        if (shot_path && st.buffer) { write_shot(st.buffer, shot_path); shot_path = NULL; }
        if (!wlr_output_commit_state(o->wlr_output, &st)) {
            static int failed;   /* (said a few times, not every frame) */
            if (failed++ < 5) fprintf(stderr, "wps-compositor: %.3f %s: the screen could not be updated\n",
                                      (now_ms() - start_ms) / 1000, o->wlr_output->name);
        }
    }
    wlr_output_state_finish(&st);
    o->last_frame_ms = now_ms();
    /* how long frames take (a slow renderer makes everything seem dead):
     * the first, and the slow ones, now and then */
    double took = now_ms() - t0;
    static int frames, slow;
    n_frames++;
    if (!frames++) fprintf(stderr, "wps-compositor: %.3f first frame (%.0f ms)\n", (now_ms() - start_ms) / 1000, took);
    else if (took > 200 && slow++ < 20) fprintf(stderr, "wps-compositor: %.3f a frame took %.0f ms\n", (now_ms() - start_ms) / 1000, took);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(so, &now);
}

/* Programs' GPU buffers: wlroots first imports each into the graphics card
 * and closes it again, to see it is good. VirtualBox's card (vmwgfx) will
 * not close such a handle ("Failed to close buffer handle"): every buffer
 * then counted as bad, and GTK 4 programs (which offer theirs so that a bad
 * one ends the connection) were disconnected at once; and each handle not
 * closed kept its buffer in video memory, until drawing broke (Firefox).
 * There the renderer's own import is the only check (as GNOME's). */
static bool dmabuf_ok(struct wlr_dmabuf_attributes *attribs, void *data)
{
    (void)attribs; (void)data;
    return true;
}

static void init_buffers(struct wlr_renderer *r, struct wl_display *display)
{
    int fd = wlr_renderer_get_drm_fd(r);
    drmVersionPtr v = fd >= 0 ? drmGetVersion(fd) : NULL;
    bool vmwgfx = v && v->name && !strcmp(v->name, "vmwgfx");
    if (v) drmFreeVersion(v);
    if (!vmwgfx && !getenv("WPS_COMP_NO_DMABUF_CHECK")) {
        wlr_renderer_init_wl_display(r, display);
        return;
    }
    wlr_renderer_init_wl_shm(r, display);
    if (wlr_renderer_get_texture_formats(r, WLR_BUFFER_CAP_DMABUF)) {
        struct wlr_linux_dmabuf_v1 *dm = wlr_linux_dmabuf_v1_create_with_renderer(display, 4, r);
        if (dm) wlr_linux_dmabuf_v1_set_check_dmabuf_callback(dm, dmabuf_ok, NULL);
    }
    fprintf(stderr, "wps-compositor: %s: programs' GPU buffers are not checked by the card\n",
            vmwgfx ? "vmwgfx (VirtualBox, VMware)" : "WPS_COMP_NO_DMABUF_CHECK");
}

static int on_pace(void *data)
{
    struct output *o = data;
    wlr_output_schedule_frame(o->wlr_output);   /* now it is time */
    return 0;
}

static void on_output_request_state(struct wl_listener *l, void *data)
{
    struct output *o = wl_container_of(l, o, request_state);
    const struct wlr_output_event_request_state *e = data;
    wlr_output_commit_state(o->wlr_output, e->state);
}

static void on_output_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct output *o = wl_container_of(l, o, destroy);
    if (o->pace) wl_event_source_remove(o->pace);
    wl_list_remove(&o->frame.link);
    wl_list_remove(&o->request_state.link);
    wl_list_remove(&o->destroy.link);
    wl_list_remove(&o->link);
    free(o);
}

static void place_desktop(struct toplevel *t);

/* A screen's scale: WPS_SCALE, else GNOME's for it (Settings, Displays:
 * ~/.config/monitors.xml, the logical monitor whose monitor has this
 * connector), else 1. */
static double screen_scale(const char *connector)
{
    const char *env = getenv("WPS_SCALE");
    if (env && atof(env) >= 0.5 && atof(env) <= 4) return atof(env);
    char path[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(path, sizeof path, "%s/monitors.xml", xdg);
    else snprintf(path, sizeof path, "%s/.config/monitors.xml", home ? home : "");
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    /* the first configuration that names this connector (GNOME writes the
     * current one; others are for other sets of screens) */
    char want[160];
    snprintf(want, sizeof want, "<connector>%s</connector>", connector);
    for (char *conf = strstr(buf, "<configuration>"); conf; conf = strstr(conf + 1, "<configuration>")) {
        char *end = strstr(conf, "</configuration>");
        if (!end) break;
        for (char *lm = strstr(conf, "<logicalmonitor>"); lm && lm < end; lm = strstr(lm + 1, "<logicalmonitor>")) {
            char *lend = strstr(lm, "</logicalmonitor>");
            if (!lend || lend > end) break;
            char *c = strstr(lm, want);
            if (!c || c > lend) continue;
            char *sc = strstr(lm, "<scale>");
            double v = sc && sc < lend ? atof(sc + 7) : 1;
            return v >= 0.5 && v <= 4 ? v : 1;
        }
    }
    return 1;
}

double comp_scale_at(double x, double y)
{
    struct wlr_output *o = wlr_output_layout_output_at(server.output_layout, x, y);
    if (!o) o = wlr_output_layout_output_at(server.output_layout, server.cursor->x, server.cursor->y);
    return o && o->scale > 0 ? o->scale : 1;
}

static void on_new_output(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_output *wo = data;
    wlr_output_init_render(wo, server.allocator, server.renderer);
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    double scale = screen_scale(wo->name);
    if (scale != 1) wlr_output_state_set_scale(&st, (float)scale);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
    if (mode) wlr_output_state_set_mode(&st, mode);
    /* no modes (headless): its size at 60 Hz. A refresh rate of 0 is
     * divided by: GTK 4 died of SIGFPE in its frame callback. */
    /* (nested in another session, the window starts at WPS_COMP_SIZE or 1280x800) */
    else {
        int w = wo->width > 0 ? wo->width : 1280, h = wo->height > 0 ? wo->height : 800;
        const char *sz = getenv("WPS_COMP_SIZE");
        if (sz && !wlr_output_is_headless(wo)) sscanf(sz, "%dx%d", &w, &h);
        /* a refresh rate only where one can be set (headless); a nested
         * window takes a size alone */
        if (wlr_output_is_headless(wo)) wlr_output_state_set_custom_mode(&st, w, h, 60000);
        else if (sz || wo->width <= 0) wlr_output_state_set_custom_mode(&st, w, h, 0);
    }
    bool ok = wlr_output_commit_state(wo, &st);
    wlr_output_state_finish(&st);
    fprintf(stderr, "wps-compositor: %.3f screen %s: %dx%d@%.1f Hz, scale %g%s\n", (now_ms() - start_ms) / 1000, wo->name,
            wo->width, wo->height, wo->refresh / 1000.0, wo->scale, ok ? "" : " (could not be set up)");
    wlr_xcursor_manager_load(server.cursor_mgr, wo->scale);   /* the pointer, sharp at its scale */
    struct output *o = calloc(1, sizeof *o);
    o->server = &server;
    o->wlr_output = wo;
    o->frame.notify = on_output_frame;
    wl_signal_add(&wo->events.frame, &o->frame);
    o->request_state.notify = on_output_request_state;
    wl_signal_add(&wo->events.request_state, &o->request_state);
    o->destroy.notify = on_output_destroy;
    wl_signal_add(&wo->events.destroy, &o->destroy);
    o->pace = wl_event_loop_add_timer(server.loop, on_pace, o);
    wl_list_insert(&server.outputs, &o->link);
    /* A screen with no physical size (headless, some virtual ones): one
     * as at 96 dpi. Programs divide by it (GTK did, and died of SIGFPE). */
    if (wo->phys_width <= 0 || wo->phys_height <= 0) {
        wo->phys_width = (int)(wo->width * 25.4 / 96);
        wo->phys_height = (int)(wo->height * 25.4 / 96);
    }
    /* The pointer drawn into the picture, not by the graphics card's cursor
     * plane: virtual machines' cards (screens named Virtual-N: VirtualBox,
     * VMware, QEMU) show no pointer that way; WPS_COMP_SOFTWARE_CURSOR=1
     * asks for it anywhere. */
    if (getenv("WPS_COMP_SOFTWARE_CURSOR") || !strncmp(wo->name, "Virtual-", 8)) {
        wlr_output_lock_software_cursors(wo, true);
        fprintf(stderr, "wps-compositor: %s: the pointer is drawn in software\n", wo->name);
    }
    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(server.output_layout, wo);
    struct wlr_scene_output *so = wlr_scene_output_create(server.scene, wo);
    wlr_scene_output_layout_add_output(server.scene_layout, lo, so);
    struct toplevel *t;
    wl_list_for_each(t, &server.toplevels, link) if (t->kind == KIND_DESKTOP) place_desktop(t);
    struct wlr_box all;
    wlr_output_layout_get_box(server.output_layout, NULL, &all);
    wlr_scene_node_set_position(&server.background->node, all.x, all.y);
    wlr_scene_rect_set_size(server.background, all.width, all.height);
    lock_layout();
}

/* ------------------------------------------------------------ windows */

static struct wlr_box first_output_box(void)
{
    struct wlr_box b = { 0, 0, 1280, 800 };
    if (!wl_list_empty(&server.outputs)) {
        struct output *o = wl_container_of(server.outputs.next, o, link);
        wlr_output_layout_get_box(server.output_layout, o->wlr_output, &b);
    }
    return b;
}

static void place_desktop(struct toplevel *t)
{
    struct wlr_box b = first_output_box();
    view_move(t, b.x, b.y);
    view_set_size(t, b.width, b.height);
}

static struct tag *tag_new(int type, struct toplevel *t)
{
    struct tag *g = calloc(1, sizeof *g);
    g->type = type;
    g->t = t;
    return g;
}

/* WPS's desktop and LaunchPad: only from the desktop process the
 * compositor started (another program naming itself so is a window). */
static bool from_desktop(struct toplevel *t) { return desktop_is(view_client(t)); }

static enum kind kind_of(struct toplevel *t)
{
    const char *app = view_app(t) ? view_app(t) : "";
    if (!from_desktop(t) && !getenv("WPS_COMP_TRUST_APP_IDS")) return KIND_NORMAL;
    return !strcmp(app, "org.wps.Desktop") ? KIND_DESKTOP : !strcmp(app, "org.wps.LaunchPad") ? KIND_PALETTE : KIND_NORMAL;
}

static void on_map(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, map);
    t->mapped = true;
    DBG("map '%s' (%s)\n", view_title(t) ? view_title(t) : "", view_app(t) ? view_app(t) : "");
    t->kind = kind_of(t);
    if (t->kind != KIND_NORMAL) t->ssd = false;
    wlr_scene_node_reparent(&t->tree->node, t->kind == KIND_DESKTOP ? server.layer_desktop
                                           : t->kind == KIND_PALETTE ? server.layer_palette : server.layer_normal);
    draw_frame(t);
    struct wlr_box ob = first_output_box();
    struct wlr_box geo = view_geo(t);
    if (t->kind == KIND_DESKTOP) {
        place_desktop(t);
    } else if (t->kind == KIND_PALETTE) {   /* the LaunchPad: bottom centre */
        view_move(t, ob.x + (ob.width - geo.width) / 2, ob.y + ob.height - geo.height - 8);
    } else {
        /* a new window: centred, or cascaded from the one in front */
        struct wlr_box b = { 0, 0, t->ssd ? t->fw : geo.width, t->ssd ? t->fh : geo.height };
        int x = ob.x + (ob.width - b.width) / 2, y = ob.y + (ob.height - b.height) / 3;
        struct toplevel *o;
        wl_list_for_each(o, &server.toplevels, link)
            if (o != t && o->mapped && o->kind == KIND_NORMAL && o->tree->node.x == x && o->tree->node.y == y) { x += 24; y += 24; }
        if (y < ob.y) y = ob.y;
        view_move(t, x, y);
    }
    focus(t);
    if (t->kind == KIND_NORMAL && (t->xdg ? t->xdg->requested.fullscreen : t->xw->fullscreen)) set_fullscreen(t, true);
    desktop_window(t, "add");
    desktop_event(t, "window.open");
    server.focus_window_list = false;
}

static void on_unmap(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, unmap);
    t->mapped = false;
    t->fullscreen = false;
    if (server.grabbed == t) { server.mode = MODE_PASS; server.grabbed = NULL; }
    if (hovered == t) hovered = NULL;
    if (server.last_click_on == t) server.last_click_on = NULL;
    if (menu.t == t) menu_close();
    desktop_event(t, "window.close");
    desktop_window(t, "remove");
    bool was_front = t->activated;
    t->activated = false;
    if (was_front) {
        struct toplevel *n;
        wl_list_for_each(n, &server.toplevels, link)
            if (n != t && n->mapped && !n->minimized && n->kind == KIND_NORMAL) { focus(n); return; }
    }
}

static void set_decoration_mode(struct toplevel *t)
{
    if (!t->deco || !view_ready(t)) return;
    /* WPS's frame, unless it is WPS's desktop or LaunchPad (no frame at all) */
    wlr_xdg_toplevel_decoration_v1_set_mode(t->deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void on_commit(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, commit);
    if (t->xdg && t->xdg->base->initial_commit) {
        set_decoration_mode(t);
        view_set_size(t, 0, 0);   /* its own size */
        return;
    }
    struct wlr_box geo = view_geo(t);
    int w, h;
    frame_size(t, geo.width, geo.height, &w, &h);
    if (t->ssd && !t->fullscreen && (w != t->fw || h != t->fh)) draw_frame(t);
    else if ((!t->ssd || t->fullscreen) && t->client) wlr_scene_node_set_position(&t->client->node, -geo.x, -geo.y);
}

static void on_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, destroy);
    DBG("destroy '%s'\n", view_title(t) ? view_title(t) : "");
    struct wl_listener *ls[] = { &t->map, &t->unmap, &t->commit, &t->destroy, &t->request_move, &t->request_resize,
                                 &t->request_maximize, &t->request_minimize, &t->request_fullscreen, &t->set_title, &t->set_app_id };
    for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) wl_list_remove(&ls[i]->link);
    if (t->deco) {
        wl_list_remove(&t->deco_request_mode.link);
        wl_list_remove(&t->deco_destroy.link);
    }
    if (t->xw) {
        struct wl_listener *xl[] = { &t->associate, &t->dissociate, &t->request_configure, &t->request_activate, &t->set_decorations };
        for (size_t i = 0; i < sizeof xl / sizeof *xl; i++) wl_list_remove(&xl[i]->link);
        t->xw->data = NULL;
    }
    if (hovered == t) hovered = NULL;
    if (server.grabbed == t) { server.grabbed = NULL; server.mode = MODE_PASS; }
    if (server.last_click_on == t) server.last_click_on = NULL;
    free(t->sent);
    free(t->tree->node.data);
    free(t->frame->node.data);
    t->tree->node.data = t->frame->node.data = NULL;
    wlr_scene_node_destroy(&t->tree->node);   /* the frame with it */
    wl_list_remove(&t->link);
    free(t);
}

/* A program asks to be moved or sized from its own title bar: only while
 * a button is still held (the request comes after the press; after a
 * quick click it can come after the release, and a move begun then never
 * ended: the window followed the pointer, the next click only ended it),
 * and for that press (Wayland: its serial). */
static bool may_grab(struct toplevel *t, uint32_t serial, bool check_serial)
{
    if (server.seat->pointer_state.button_count == 0 || server.mode != MODE_PASS) return false;
    if (check_serial && !wlr_seat_validate_pointer_grab_serial(server.seat, view_surface(t), serial)) return false;
    return true;
}

static void on_request_move(struct wl_listener *l, void *data)
{
    struct toplevel *t = wl_container_of(l, t, request_move);
    if (t->xdg) {
        struct wlr_xdg_toplevel_move_event *e = data;
        if (!may_grab(t, e->serial, true)) return;
    } else if (!may_grab(t, 0, false)) {
        return;
    }
    begin_grab(t, MODE_MOVE, 0);
}

static void on_request_resize(struct wl_listener *l, void *data)
{
    struct toplevel *t = wl_container_of(l, t, request_resize);
    struct wlr_xdg_toplevel_resize_event *e = data;
    if (!may_grab(t, e->serial, true)) return;
    begin_grab(t, MODE_RESIZE, e->edges);
}

static void on_request_maximize(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, request_maximize);
    if (!view_ready(t)) return;
    if (kind_of(t) != KIND_NORMAL) { view_schedule(t); return; }   /* the desktop asks: it is placed */
    maximize(t, t->xdg ? t->xdg->requested.maximized : !t->maximized);
}

static void on_request_minimize(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, request_minimize);
    if (t->mapped) minimize(t, true);
}

static void on_request_fullscreen(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, request_fullscreen);
    bool want = t->xdg ? t->xdg->requested.fullscreen : t->xw->fullscreen;
    if (t->mapped) set_fullscreen(t, want);
    else if (view_ready(t)) view_schedule(t);   /* before it shows: done as it maps */
}

static void on_set_title(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, set_title);
    draw_frame(t);
    desktop_window(t, "change");
}

static void on_set_app_id(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, set_app_id);
    draw_frame(t);   /* no title yet: the app ID is shown */
    desktop_window(t, "change");
}

static void on_new_toplevel(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_toplevel *xt = data;
    DBG("new toplevel %p\n", (void *)xt);
    struct toplevel *t = calloc(1, sizeof *t);
    t->server = &server;
    t->xdg = xt;
    t->tree = wlr_scene_tree_create(server.layer_normal);
    t->tree->node.data = tag_new(2, t);
    t->frame = wlr_scene_buffer_create(t->tree, NULL);
    t->frame->node.data = tag_new(1, t);
    wlr_scene_node_set_enabled(&t->frame->node, false);
    t->client = wlr_scene_xdg_surface_create(t->tree, xt->base);
    xt->base->data = t->client;
    t->hover = t->pressed = UI_HIT_NONE;
    static unsigned next_id = 1;
    t->id = next_id++;
#define ON(sig, field, fn) do { t->field.notify = fn; wl_signal_add(&(sig), &t->field); } while (0)
    ON(xt->base->surface->events.map, map, on_map);
    ON(xt->base->surface->events.unmap, unmap, on_unmap);
    ON(xt->base->surface->events.commit, commit, on_commit);
    ON(xt->events.destroy, destroy, on_destroy);
    ON(xt->events.request_move, request_move, on_request_move);
    ON(xt->events.request_resize, request_resize, on_request_resize);
    ON(xt->events.request_maximize, request_maximize, on_request_maximize);
    ON(xt->events.request_minimize, request_minimize, on_request_minimize);
    ON(xt->events.request_fullscreen, request_fullscreen, on_request_fullscreen);
    ON(xt->events.set_title, set_title, on_set_title);
    ON(xt->events.set_app_id, set_app_id, on_set_app_id);
#undef ON
    wl_list_insert(server.toplevels.prev, &t->link);
}

/* ---------------------------------------------------------------- X11 */

/* X11 programs through XWayland. Managed windows are windows like the
 * others (framed unless they ask for no decorations); override-redirect
 * ones (X11 menus, tooltips) are shown where they say, above all. */

static void x_associate(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, associate);
    struct wlr_surface *s = t->xw->surface;
    t->client = wlr_scene_subsurface_tree_create(t->tree, s);
    t->map.notify = on_map;
    wl_signal_add(&s->events.map, &t->map);
    t->unmap.notify = on_unmap;
    wl_signal_add(&s->events.unmap, &t->unmap);
    t->commit.notify = on_commit;
    wl_signal_add(&s->events.commit, &t->commit);
}

static void x_dissociate(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, dissociate);
    wl_list_remove(&t->map.link);
    wl_list_remove(&t->unmap.link);
    wl_list_remove(&t->commit.link);
    wl_list_init(&t->map.link);
    wl_list_init(&t->unmap.link);
    wl_list_init(&t->commit.link);
    t->client = NULL;   /* destroyed with its surface */
}

static void x_request_configure(struct wl_listener *l, void *data)
{
    struct toplevel *t = wl_container_of(l, t, request_configure);
    struct wlr_xwayland_surface_configure_event *e = data;
    if (!t->mapped) {
        /* where it asks to be (placed when it is shown) */
        ui_rect c = t->ssd ? client_rect(t) : (ui_rect){ 0, 0, 0, 0 };
        wlr_scene_node_set_position(&t->tree->node, e->x - (int)c.x, e->y - (int)c.y);
        wlr_xwayland_surface_configure(t->xw, e->x, e->y, e->width, e->height);
        return;
    }
    if (t->maximized) { x11_configure(t, 0, 0); return; }   /* it stays as it is */
    x11_configure(t, e->width, e->height);   /* its size; it stays where it is */
}

static void x_request_activate(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, request_activate);
    if (t->mapped) comp_focus(t);
}

static void x_request_resize(struct wl_listener *l, void *data)
{
    struct toplevel *t = wl_container_of(l, t, request_resize);
    struct wlr_xwayland_resize_event *e = data;
    if (!may_grab(t, 0, false)) return;
    begin_grab(t, MODE_RESIZE, e->edges);
}

static void x_request_minimize(struct wl_listener *l, void *data)
{
    struct toplevel *t = wl_container_of(l, t, request_minimize);
    struct wlr_xwayland_minimize_event *e = data;
    if (t->mapped) minimize(t, e->minimize);
}

static void x_set_decorations(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, set_decorations);
    bool want = t->xw->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    if (want == t->ssd) return;
    t->ssd = want;
    draw_frame(t);
}

static void x_set_class(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, set_app_id);
    draw_frame(t);
    desktop_window(t, "change");
}

/* An override-redirect window: no frame, no focus, where it says. */
struct unmanaged {
    struct wlr_xwayland_surface *xw;
    struct wlr_scene_tree *tree;
    struct wl_listener associate, dissociate, map, unmap, set_geometry, destroy;
};

static void u_map(struct wl_listener *l, void *data)
{
    (void)data;
    struct unmanaged *u = wl_container_of(l, u, map);
    u->tree = wlr_scene_subsurface_tree_create(server.layer_palette, u->xw->surface);
    wlr_scene_node_set_position(&u->tree->node, u->xw->x, u->xw->y);
    if (wlr_xwayland_surface_override_redirect_wants_focus(u->xw)) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(server.seat);
        if (kb) wlr_seat_keyboard_notify_enter(server.seat, u->xw->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
}

static void u_unmap(struct wl_listener *l, void *data)
{
    (void)data;
    struct unmanaged *u = wl_container_of(l, u, unmap);
    if (u->tree) wlr_scene_node_destroy(&u->tree->node);
    u->tree = NULL;
}

static void u_associate(struct wl_listener *l, void *data)
{
    (void)data;
    struct unmanaged *u = wl_container_of(l, u, associate);
    u->map.notify = u_map;
    wl_signal_add(&u->xw->surface->events.map, &u->map);
    u->unmap.notify = u_unmap;
    wl_signal_add(&u->xw->surface->events.unmap, &u->unmap);
}

static void u_dissociate(struct wl_listener *l, void *data)
{
    (void)data;
    struct unmanaged *u = wl_container_of(l, u, dissociate);
    wl_list_remove(&u->map.link);
    wl_list_remove(&u->unmap.link);
    wl_list_init(&u->map.link);
    wl_list_init(&u->unmap.link);
}

static void u_set_geometry(struct wl_listener *l, void *data)
{
    (void)data;
    struct unmanaged *u = wl_container_of(l, u, set_geometry);
    if (u->tree) wlr_scene_node_set_position(&u->tree->node, u->xw->x, u->xw->y);
}

static void u_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct unmanaged *u = wl_container_of(l, u, destroy);
    wl_list_remove(&u->associate.link);
    wl_list_remove(&u->dissociate.link);
    wl_list_remove(&u->map.link);
    wl_list_remove(&u->unmap.link);
    wl_list_remove(&u->set_geometry.link);
    wl_list_remove(&u->destroy.link);
    free(u);
}

static void on_new_xsurface(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xwayland_surface *xs = data;
    DBG("new X11 window %u%s\n", xs->window_id, xs->override_redirect ? " (override-redirect)" : "");
    if (xs->override_redirect) {
        struct unmanaged *u = calloc(1, sizeof *u);
        u->xw = xs;
        wl_list_init(&u->map.link);
        wl_list_init(&u->unmap.link);
        u->associate.notify = u_associate;
        wl_signal_add(&xs->events.associate, &u->associate);
        u->dissociate.notify = u_dissociate;
        wl_signal_add(&xs->events.dissociate, &u->dissociate);
        u->set_geometry.notify = u_set_geometry;
        wl_signal_add(&xs->events.set_geometry, &u->set_geometry);
        u->destroy.notify = u_destroy;
        wl_signal_add(&xs->events.destroy, &u->destroy);
        return;
    }
    struct toplevel *t = calloc(1, sizeof *t);
    t->server = &server;
    t->xw = xs;
    xs->data = t;
    t->ssd = xs->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    t->tree = wlr_scene_tree_create(server.layer_normal);
    t->tree->node.data = tag_new(2, t);
    t->frame = wlr_scene_buffer_create(t->tree, NULL);
    t->frame->node.data = tag_new(1, t);
    wlr_scene_node_set_enabled(&t->frame->node, false);
    t->hover = t->pressed = UI_HIT_NONE;
    static unsigned next_x11 = 1000000;   /* (apart from Wayland windows' IDs) */
    t->id = next_x11++;
    wl_list_init(&t->map.link);
    wl_list_init(&t->unmap.link);
    wl_list_init(&t->commit.link);
#define ON(sig, field, fn) do { t->field.notify = fn; wl_signal_add(&(sig), &t->field); } while (0)
    ON(xs->events.associate, associate, x_associate);
    ON(xs->events.dissociate, dissociate, x_dissociate);
    ON(xs->events.destroy, destroy, on_destroy);
    ON(xs->events.request_configure, request_configure, x_request_configure);
    ON(xs->events.request_activate, request_activate, x_request_activate);
    ON(xs->events.request_move, request_move, on_request_move);
    ON(xs->events.request_resize, request_resize, x_request_resize);
    ON(xs->events.request_maximize, request_maximize, on_request_maximize);
    ON(xs->events.request_minimize, request_minimize, x_request_minimize);
    ON(xs->events.request_fullscreen, request_fullscreen, on_request_fullscreen);
    ON(xs->events.set_title, set_title, on_set_title);
    ON(xs->events.set_class, set_app_id, x_set_class);
    ON(xs->events.set_decorations, set_decorations, x_set_decorations);
#undef ON
    wl_list_insert(server.toplevels.prev, &t->link);
}

static void on_xwayland_ready(struct wl_listener *l, void *data)
{
    (void)l; (void)data;
    wlr_xwayland_set_seat(server.xwayland, server.seat);
    /* X11 programs' default pointer: the same as ours */
    struct wlr_xcursor *xc = wlr_xcursor_manager_get_xcursor(server.cursor_mgr, "default", 1);
    if (xc && xc->image_count) {
        struct wlr_xcursor_image *im = xc->images[0];
        struct wlr_buffer *b = wlr_xcursor_image_get_buffer(im);
        if (b) wlr_xwayland_set_cursor(server.xwayland, b, (int32_t)im->hotspot_x, (int32_t)im->hotspot_y);
    }
    DBG("XWayland ready on %s\n", server.xwayland->display_name);
}

/* Popups (menus): in their parent's tree, kept on the screen. */
struct popup { struct wlr_xdg_popup *xdg; struct wl_listener commit, destroy; };

static void on_popup_commit(struct wl_listener *l, void *data)
{
    (void)data;
    struct popup *p = wl_container_of(l, p, commit);
    if (!p->xdg->base->initial_commit) return;
    /* the screen, in the parent's surface coordinates */
    struct wlr_scene_tree *pt = p->xdg->parent ? p->xdg->parent->data : NULL;
    int lx = 0, ly = 0;
    if (pt) wlr_scene_node_coords(&pt->node, &lx, &ly);
    struct wlr_box ob = first_output_box();
    struct wlr_box box = { ob.x - lx, ob.y - ly, ob.width, ob.height };
    wlr_xdg_popup_unconstrain_from_box(p->xdg, &box);
    wlr_xdg_surface_schedule_configure(p->xdg->base);
}

static void on_popup_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct popup *p = wl_container_of(l, p, destroy);
    wl_list_remove(&p->commit.link);
    wl_list_remove(&p->destroy.link);
    free(p);
}

static void on_new_popup(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_popup *xp = data;
    struct wlr_xdg_surface *parent = xp->parent ? wlr_xdg_surface_try_from_wlr_surface(xp->parent) : NULL;
    if (!parent || !parent->data) return;
    struct wlr_scene_tree *pt = parent->data;
    xp->base->data = wlr_scene_xdg_surface_create(pt, xp->base);
    struct popup *p = calloc(1, sizeof *p);
    p->xdg = xp;
    p->commit.notify = on_popup_commit;
    wl_signal_add(&xp->base->surface->events.commit, &p->commit);
    p->destroy.notify = on_popup_destroy;
    wl_signal_add(&xp->events.destroy, &p->destroy);
}

/* Decorations: WPS's frame for every program that agrees to it. */
static void on_deco_request_mode(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, deco_request_mode);
    set_decoration_mode(t);
}

static void on_deco_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, deco_destroy);
    wl_list_remove(&t->deco_request_mode.link);
    wl_list_remove(&t->deco_destroy.link);
    t->deco = NULL;
    t->ssd = false;
    draw_frame(t);
}

static void on_new_decoration(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_toplevel_decoration_v1 *d = data;
    struct wlr_scene_tree *client = d->toplevel->base->data;
    struct toplevel *t = NULL, *x;
    wl_list_for_each(x, &server.toplevels, link) if (x->client == client) t = x;
    if (!t) return;
    t->deco = d;
    t->ssd = true;
    t->deco_request_mode.notify = on_deco_request_mode;
    wl_signal_add(&d->events.request_mode, &t->deco_request_mode);
    t->deco_destroy.notify = on_deco_destroy;
    wl_signal_add(&d->events.destroy, &t->deco_destroy);
    set_decoration_mode(t);
}

/* GTK 3 programs never use xdg-decoration but do KDE's older server
 * decoration protocol: with "server" as its default, a GTK 3 window with
 * an ordinary title bar (no header bar) asks for it and gets WPS's frame
 * (Firefox with its title bar on, Thunderbird, GIMP 2, ...); one with a
 * header bar asks for "client" and keeps its own. */
struct kde_deco { struct wlr_server_decoration *d; struct wl_listener mode, destroy; };
static struct wl_listener new_kde;   /* the manager's (removed at the end) */

static void kde_apply(struct kde_deco *k)
{
    struct toplevel *t = comp_toplevel_of(k->d->surface);
    if (!t || !t->xdg || t->deco) return;   /* (xdg-decoration, if both: that one) */
    bool want = k->d->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER && (!t->mapped || t->kind == KIND_NORMAL);
    if (t->ssd == want) return;
    t->ssd = want;
    DBG("'%s' asks for %s (KDE's protocol)\n", view_title(t) ? view_title(t) : "", want ? "WPS's frame" : "its own frame");
    draw_frame(t);
    if (!want && t->client) {
        struct wlr_box geo = view_geo(t);
        wlr_scene_node_set_position(&t->client->node, -geo.x, -geo.y);
    }
}

static void on_kde_mode(struct wl_listener *l, void *data)
{
    (void)data;
    struct kde_deco *k = wl_container_of(l, k, mode);
    kde_apply(k);
}

static void on_kde_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct kde_deco *k = wl_container_of(l, k, destroy);
    wl_list_remove(&k->mode.link);
    wl_list_remove(&k->destroy.link);
    free(k);
}

static void on_new_kde_decoration(struct wl_listener *l, void *data)
{
    (void)l;
    struct kde_deco *k = calloc(1, sizeof *k);
    k->d = data;
    k->mode.notify = on_kde_mode;
    wl_signal_add(&k->d->events.mode, &k->mode);
    k->destroy.notify = on_kde_destroy;
    wl_signal_add(&k->d->events.destroy, &k->destroy);
    kde_apply(k);
}

/* ------------------------------------------------------------ spawning */

bool comp_spawn(char *const argv[], void (*prepare)(void *), void *arg)
{
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (fork() == 0) {   /* the program: init's child, not ours */
            setsid();
            if (prepare) prepare(arg);
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);   /* the go-between, at once */
    return true;
}

/* ------------------------------------------------------------ for desktop.c */

void comp_focus(struct toplevel *t)
{
    if (!t->mapped) return;
    if (t->minimized) minimize(t, false);
    else focus(t);
}
void comp_minimize(struct toplevel *t, bool on) { if (t->mapped) minimize(t, on); }
void comp_maximize(struct toplevel *t, bool on)
{
    if (!t->mapped) return;
    if (t->minimized) minimize(t, false);
    if (t->kind == KIND_NORMAL) maximize(t, on);
}
void comp_close(struct toplevel *t) { view_close(t); }

void comp_before_lock(void)
{
    menu_close();
    server.mode = MODE_PASS;
    server.grabbed = NULL;
    wlr_seat_keyboard_notify_clear_focus(server.seat);
    wlr_seat_pointer_clear_focus(server.seat);
    set_cursor("default");
}

void comp_after_unlock(void)
{
    /* the window that had the keyboard has it again (still the front of
     * the list: nothing was focused while locked), else a normal window */
    struct toplevel *t, *pick = NULL;
    wl_list_for_each(t, &server.toplevels, link)
        if (t->mapped && !t->minimized && (t->kind == KIND_NORMAL || !pick)) { pick = t; if (t->kind == KIND_NORMAL || t->activated) break; }
    if (pick) { pick->activated = false; focus(pick); }
}

void comp_redraw_frames(void)
{
    struct toplevel *t;
    wl_list_for_each(t, &server.toplevels, link) {
        if (!t->mapped) continue;
        /* the frame's size may change (border, title bar): the program's size stays */
        struct wlr_box g = view_geo(t);
        draw_frame(t);
        if (t->xw) x11_configure(t, g.width, g.height);
    }
}

struct toplevel *comp_toplevel_of(struct wlr_surface *s)
{
    struct wlr_xdg_toplevel *xt = s ? wlr_xdg_toplevel_try_from_wlr_surface(s) : NULL;
    struct wlr_xwayland_surface *xs = s && !xt ? wlr_xwayland_surface_try_from_wlr_surface(s) : NULL;
    struct toplevel *t;
    if (xt || xs) wl_list_for_each(t, &server.toplevels, link) if ((xt && t->xdg == xt) || (xs && t->xw == xs)) return t;
    return NULL;
}

/* xdg-activation: a program asks for one of its windows to come forward
 * (WPS's windows ask so when opened again; any program's own token). */
static void on_request_activate(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_activation_v1_request_activate_event *e = data;
    struct toplevel *t = comp_toplevel_of(e->surface);
    if (t && t->mapped) comp_focus(t);
}

/* -------------------------------------------------------------- script */

struct step { double at; char line[512]; };
static struct step *steps;
static size_t nsteps, next_step;
static struct wl_event_source *script_timer;

static void log_windows(void)
{
    struct toplevel *t;
    wl_list_for_each(t, &server.toplevels, link) {
        struct wlr_box b = window_box(t);
        fprintf(stderr, "wps-compositor: window '%s' app %s %dx%d+%d+%d%s%s%s%s%s\n", view_title(t) ? view_title(t) : "",
                view_app(t) ? view_app(t) : "", b.width, b.height, b.x, b.y, t->ssd ? " framed" : "",
                t->activated ? " active" : "", t->minimized ? " minimized" : "", t->maximized ? " maximized" : "",
                t->kind == KIND_DESKTOP ? " desktop" : t->kind == KIND_PALETTE ? " palette" : "");
    }
}

/* The keycode (xkb) that types sym without modifiers, or 0. */
static uint32_t keycode_for(struct wlr_keyboard *kb, xkb_keysym_t sym)
{
    struct xkb_keymap *km = kb->keymap;
    for (xkb_keycode_t k = xkb_keymap_min_keycode(km); k <= xkb_keymap_max_keycode(km); k++) {
        const xkb_keysym_t *syms;
        int n = xkb_keymap_key_get_syms_by_level(km, k, 0, 0, &syms);
        for (int i = 0; i < n; i++) if (syms[i] == sym) return k;
    }
    for (xkb_keycode_t k = xkb_keymap_min_keycode(km); k <= xkb_keymap_max_keycode(km); k++) {   /* upper case: level 1 */
        const xkb_keysym_t *syms;
        int n = xkb_keymap_key_get_syms_by_level(km, k, 0, 1, &syms);
        for (int i = 0; i < n; i++) if (syms[i] == sym) return k;
    }
    return 0;
}

static void script_key(const char *combo)
{
    uint32_t mods = 0;
    char name[64];
    const char *p = combo;
    for (;;) {
        if (!strncmp(p, "ctrl+", 5)) { mods |= WLR_MODIFIER_CTRL; p += 5; }
        else if (!strncmp(p, "alt+", 4)) { mods |= WLR_MODIFIER_ALT; p += 4; }
        else if (!strncmp(p, "shift+", 6)) { mods |= WLR_MODIFIER_SHIFT; p += 6; }
        else if (!strncmp(p, "super+", 6)) { mods |= WLR_MODIFIER_LOGO; p += 6; }
        else break;
    }
    snprintf(name, sizeof name, "%s", p);
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol) sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    struct wlr_keyboard *kb = server.script_kb;
    if (!kb) { if (!shortcut(mods, sym)) fprintf(stderr, "wps-compositor: script: no keyboard for %s\n", combo); return; }
    /* typed on the test keyboard, as a hand types: the modifiers, the key */
    static const struct { uint32_t mod; const char *key; } mk[] = {
        { WLR_MODIFIER_CTRL, "Control_L" }, { WLR_MODIFIER_ALT, "Alt_L" }, { WLR_MODIFIER_SHIFT, "Shift_L" },
        { WLR_MODIFIER_LOGO, "Super_L" } };
    uint32_t codes[5];
    size_t n = 0;
    for (size_t i = 0; i < 4; i++)
        if (mods & mk[i].mod) codes[n++] = keycode_for(kb, xkb_keysym_from_name(mk[i].key, XKB_KEYSYM_NO_FLAGS));
    codes[n++] = keycode_for(kb, sym);
    if (!codes[n - 1]) { fprintf(stderr, "wps-compositor: script: no key for %s\n", combo); return; }
    uint32_t t = (uint32_t)now_ms();
    for (size_t i = 0; i < n; i++)
        wlr_keyboard_notify_key(kb, &(struct wlr_keyboard_key_event){ t, codes[i] - 8, true, WL_KEYBOARD_KEY_STATE_PRESSED });
    for (size_t i = n; i-- > 0;)
        wlr_keyboard_notify_key(kb, &(struct wlr_keyboard_key_event){ t, codes[i] - 8, true, WL_KEYBOARD_KEY_STATE_RELEASED });
}

static void script_step(const char *line)
{
    char cmd[32] = "", arg[480] = "";
    double x = 0, y = 0;
    sscanf(line, "%31s %479[^\n]", cmd, arg);
    uint32_t t = (uint32_t)now_ms();
    if (!strcmp(cmd, "move") || !strcmp(cmd, "click") || !strcmp(cmd, "down") || !strcmp(cmd, "up") || !strcmp(cmd, "dclick")) {
        char extra[16] = "";
        sscanf(arg, "%lf %lf %15s", &x, &y, extra);
        idle_activity();   /* as input from a hand */
        wlr_cursor_warp_closest(server.cursor, NULL, x, y);
        process_motion(t);
        uint32_t btn = !strcmp(extra, "right") ? 0x111 : 0x110;
        if (!strcmp(cmd, "click") || !strcmp(cmd, "down") || !strcmp(cmd, "dclick")) process_button(t, btn, true);
        if (!strcmp(cmd, "click") || !strcmp(cmd, "up") || !strcmp(cmd, "dclick")) process_button(t, btn, false);
        if (!strcmp(cmd, "dclick")) { process_button(t, btn, true); process_button(t, btn, false); }
    } else if (!strcmp(cmd, "key")) {
        script_key(arg);
    } else if ((!strcmp(cmd, "press") || !strcmp(cmd, "release")) && server.script_kb) {
        /* a key held (press) or let go (release) by name, e.g. Control_L
         * through a drag */
        uint32_t code = keycode_for(server.script_kb, xkb_keysym_from_name(arg, XKB_KEYSYM_NO_FLAGS));
        if (code)
            wlr_keyboard_notify_key(server.script_kb, &(struct wlr_keyboard_key_event){ t, code - 8, true,
                                    !strcmp(cmd, "press") ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED });
        else fprintf(stderr, "wps-compositor: script: no key %s\n", arg);
    } else if (!strcmp(cmd, "shot")) {
        static char path[480];
        snprintf(path, sizeof path, "%s", arg);
        shot_path = path;
        struct output *o;
        wl_list_for_each(o, &server.outputs, link) wlr_output_schedule_frame(o->wlr_output);
    } else if (!strcmp(cmd, "windows")) {
        log_windows();
    } else if (!strcmp(cmd, "hang")) {   /* the loop stuck on purpose (tests the watchdog) */
        sleep((unsigned)atoi(arg));
    } else if (!strcmp(cmd, "quit")) {
        wl_display_terminate(server.display);
    }
}

static int on_script_timer(void *data)
{
    (void)data;
    double t = (now_ms() - start_ms) / 1000;
    while (next_step < nsteps && steps[next_step].at <= t) script_step(steps[next_step++].line);
    if (next_step < nsteps) wl_event_source_timer_update(script_timer, 20);
    return 0;
}

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "wps-compositor: no script %s\n", path); return; }
    char line[600];
    while (fgets(line, sizeof line, f)) {
        double at;
        int n = 0;
        if (line[0] == '#' || sscanf(line, "%lf %n", &at, &n) < 1) continue;
        steps = realloc(steps, (nsteps + 1) * sizeof *steps);
        steps[nsteps].at = at;
        snprintf(steps[nsteps].line, sizeof steps[nsteps].line, "%s", line + n);
        nsteps++;
    }
    fclose(f);
    script_timer = wl_event_loop_add_timer(server.loop, on_script_timer, NULL);
    wl_event_source_timer_update(script_timer, 20);
}

/* --------------------------------------------------------------- the log */

/* Every 5 s: the loop is running, and what it has seen (input that never
 * comes shows as counts that stay 0). */
static struct wl_event_source *heartbeat;
static int on_heartbeat(void *data)
{
    (void)data;
    int windows = 0;
    struct toplevel *t;
    wl_list_for_each(t, &server.toplevels, link) windows += t->mapped;
    fprintf(stderr, "wps-compositor: %.1f alive: %lu frames, %lu keys, %lu clicks, %lu pointer moves, %d windows%s\n",
            comp_uptime(), n_frames, n_keys, n_buttons, n_motions, windows,
            server.session && !server.session->active ? ", session NOT active" : "");
    wl_event_source_timer_update(heartbeat, 5000);
    return 0;
}

/* The watchdog (WPS_COMP_DEBUG): a thread that sees whether the loop
 * still turns (a tick each second); stuck for 8 s, it has gdb write every
 * thread's stack to the log, once: where it is stuck. */
static atomic_ulong loop_ticks;
static struct wl_event_source *tick_src;
static int on_tick(void *data) { (void)data; atomic_fetch_add(&loop_ticks, 1); wl_event_source_timer_update(tick_src, 1000); return 0; }

static void *watchdog(void *arg)
{
    (void)arg;
    unsigned long last = (unsigned long)-1;
    int still = 0;
    for (;;) {
        sleep(1);
        unsigned long now = atomic_load(&loop_ticks);
        if (now != last) { last = now; still = 0; continue; }
        if (++still != 8) continue;
        fprintf(stderr, "wps-compositor: %.1f the loop has not turned for 8 s: its threads' stacks follow (gdb)\n", comp_uptime());
        char cmd[256];
        snprintf(cmd, sizeof cmd, "gdb -q -batch -p %d -ex 'thread apply all bt' 1>&2 2>/dev/null < /dev/null", (int)getpid());
        if (system(cmd) != 0) fprintf(stderr, "wps-compositor: gdb could not show them (installed? sudo dnf install gdb)\n");
        fprintf(stderr, "wps-compositor: (end of the stacks)\n");
    }
    return NULL;
}

static struct wl_listener session_active;
static void on_session_active(struct wl_listener *l, void *data)
{
    (void)l; (void)data;
    fprintf(stderr, "wps-compositor: %.3f session active: %s\n", comp_uptime(), server.session->active ? "yes" : "no");
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *startup = NULL, *desktop = NULL;
    int c;
    while ((c = getopt(argc, argv, "s:d:h")) != -1) {
        if (c == 's') startup = optarg;
        else if (c == 'd') desktop = optarg;
        else { fprintf(stderr, "usage: %s [-d DESKTOP-COMMAND] [-s COMMAND]\n", argv[0]); return c == 'h' ? 0 : 2; }
    }
    comp_debug = getenv("WPS_COMP_DEBUG") != NULL;
    wlr_log_init(getenv("WPS_COMP_DEBUG") && !strcmp(getenv("WPS_COMP_DEBUG"), "wlr") ? WLR_DEBUG : WLR_ERROR, NULL);
    ui_scheme_init();
    ui_text_init();
    struct server *s = &server;
    start_ms = now_ms();
    s->display = wl_display_create();
    s->loop = wl_display_get_event_loop(s->display);
    const char *headless = getenv("WPS_COMP_HEADLESS");
    int hw = 0, hh = 0;
    if (headless && sscanf(headless, "%dx%d", &hw, &hh) == 2) {
        s->backend = wlr_headless_backend_create(s->loop);
        s->renderer = wlr_pixman_renderer_create();   /* readable for screenshots */
    } else {
        s->backend = wlr_backend_autocreate(s->loop, &s->session);
        if (s->backend) s->renderer = wlr_renderer_autocreate(s->backend);
        /* VirtualBox's (and VMware's) 3D, vmwgfx: windows' pictures went
         * stale in places as they changed (Firefox's "burn", windows black
         * or see-through when dragged), and stayed right with the software
         * renderer. There it draws in software unless WLR_RENDERER asks. */
        if (s->renderer && !getenv("WLR_RENDERER") && !wlr_renderer_is_pixman(s->renderer)) {
            int fd = wlr_renderer_get_drm_fd(s->renderer);
            drmVersionPtr v = fd >= 0 ? drmGetVersion(fd) : NULL;
            bool vmwgfx = v && v->name && !strcmp(v->name, "vmwgfx");
            if (v) drmFreeVersion(v);
            if (vmwgfx) {
                fprintf(stderr, "wps-compositor: vmwgfx (VirtualBox, VMware): drawing in software "
                                "(WLR_RENDERER=gles2 for the GPU)\n");
                wlr_renderer_destroy(s->renderer);
                s->renderer = wlr_pixman_renderer_create();
            }
        }
        if (s->backend && !s->renderer) {
            /* no GPU drawing (a virtual machine without 3D, a missing
             * driver): the software renderer, slower but the same picture */
            fprintf(stderr, "wps-compositor: no GPU renderer: drawing in software (pixman)\n");
            s->renderer = wlr_pixman_renderer_create();
        }
    }
    if (!s->backend || !s->renderer) { fprintf(stderr, "wps-compositor: no backend or renderer\n"); return 1; }
    fprintf(stderr, "wps-compositor: renderer %s%s\n", wlr_renderer_is_pixman(s->renderer) ? "pixman (software)" : "GPU",
            s->session ? ", on a console (libseat)" : "");
    init_buffers(s->renderer, s->display);
    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator) { fprintf(stderr, "wps-compositor: no allocator\n"); return 1; }
    s->compositor = wlr_compositor_create(s->display, 6, s->renderer);   /* 6: surfaces are told their preferred scale */
    wlr_subcompositor_create(s->display);
    wlr_data_device_manager_create(s->display);
    wlr_primary_selection_v1_device_manager_create(s->display);
    /* scaled screens: programs draw at the screen's scale, fractional too
     * (the scene tells each surface its screen's scale) */
    wlr_viewporter_create(s->display);
    wlr_fractional_scale_manager_v1_create(s->display, 1);
    /* Clipboard managers and helpers reading and setting the clipboard
     * without a window (VirtualBox's shared clipboard, wl-clipboard,
     * clipboard histories): both the wlroots and the ext protocol. */
    wlr_data_control_manager_v1_create(s->display);
    wlr_ext_data_control_manager_v1_create(s->display, 1);

    s->output_layout = wlr_output_layout_create(s->display);
    wl_list_init(&s->outputs);
    s->new_output.notify = on_new_output;
    wl_signal_add(&s->backend->events.new_output, &s->new_output);
    s->scene = wlr_scene_create();
    s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->output_layout);
    static const float teal[4] = { 0.0f, 0.5f, 0.5f, 1.0f };   /* OS/2's desktop colour */
    s->background = wlr_scene_rect_create(&s->scene->tree, 0, 0, teal);
    s->layer_desktop = wlr_scene_tree_create(&s->scene->tree);
    s->layer_normal = wlr_scene_tree_create(&s->scene->tree);
    s->layer_palette = wlr_scene_tree_create(&s->scene->tree);
    s->layer_fullscreen = wlr_scene_tree_create(&s->scene->tree);
    s->layer_drag = wlr_scene_tree_create(&s->scene->tree);
    menu.buf = wlr_scene_buffer_create(&s->scene->tree, NULL);   /* above everything */
    wlr_scene_node_set_enabled(&menu.buf->node, false);

    wl_list_init(&s->toplevels);
    s->xdg_shell = wlr_xdg_shell_create(s->display, 6);
    s->new_toplevel.notify = on_new_toplevel;
    wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_toplevel);
    s->new_popup.notify = on_new_popup;
    wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_popup);
    s->activation = wlr_xdg_activation_v1_create(s->display);
    s->request_activate.notify = on_request_activate;
    wl_signal_add(&s->activation->events.request_activate, &s->request_activate);
    s->deco_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
    {   /* KDE's server decoration protocol, for GTK 3 programs */
        struct wlr_server_decoration_manager *kd = wlr_server_decoration_manager_create(s->display);
        wlr_server_decoration_manager_set_default_mode(kd, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
        new_kde.notify = on_new_kde_decoration;
        wl_signal_add(&kd->events.new_decoration, &new_kde);
    }
    s->new_decoration.notify = on_new_decoration;
    wl_signal_add(&s->deco_mgr->events.new_toplevel_decoration, &s->new_decoration);

    s->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(s->cursor, s->output_layout);
    const char *cs = getenv("XCURSOR_SIZE");
    s->cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), cs && atoi(cs) > 0 ? (uint32_t)atoi(cs) : 24);
#define ON(sig, field, fn) do { s->field.notify = fn; wl_signal_add(&(sig), &s->field); } while (0)
    ON(s->cursor->events.motion, cursor_motion, on_cursor_motion);
    ON(s->cursor->events.motion_absolute, cursor_motion_absolute, on_cursor_motion_absolute);
    ON(s->cursor->events.button, cursor_button, on_cursor_button);
    ON(s->cursor->events.axis, cursor_axis, on_cursor_axis);
    ON(s->cursor->events.frame, cursor_frame, on_cursor_frame);
    wl_list_init(&s->keyboards);
    ON(s->backend->events.new_input, new_input, on_new_input);
    s->seat = wlr_seat_create(s->display, "seat0");
    ON(s->seat->events.request_set_cursor, request_cursor, on_request_cursor);
    ON(s->seat->events.request_set_selection, request_set_selection, on_request_set_selection);
    ON(s->seat->events.request_set_primary_selection, request_set_primary, on_request_set_primary);
    ON(s->seat->events.request_start_drag, request_start_drag, on_request_start_drag);
    ON(s->seat->events.start_drag, start_drag, on_start_drag);
#undef ON
    s->mode = MODE_PASS;

    const char *socket = wl_display_add_socket_auto(s->display);
    if (!socket) { fprintf(stderr, "wps-compositor: no socket\n"); return 1; }
    keymap_init();   /* before the keyboards come (as the backend starts) */
    if (!wlr_backend_start(s->backend)) { fprintf(stderr, "wps-compositor: the backend did not start\n"); return 1; }
    if (hw > 0) {
        wlr_headless_add_output(s->backend, (unsigned)hw, (unsigned)hh);
        /* a keyboard (headless has none): for the scripts' keys, and
         * XWayland, which will not start without one */
        static const struct wlr_keyboard_impl impl = { .name = "wps-test-keyboard" };
        static struct wlr_keyboard kb;
        wlr_keyboard_init(&kb, &impl, "wps-test-keyboard");
        on_new_input(NULL, &kb.base);
        s->script_kb = &kb;
    }
    setenv("WAYLAND_DISPLAY", socket, true);
    unsetenv("DISPLAY");
    /* X11 programs: XWayland, started when the first one connects */
    if (!getenv("WPS_COMP_NO_X11")) {
        /* In the WPS session it starts at once and stays: started when the
         * first X11 program comes, wlroots ends it again some seconds after
         * the last goes, and IBus's X11 part (ibus-x11 --kill-daemon) took
         * IBus down with it (Hangul stopped after a few letters). Nested
         * and in tests it starts when needed. */
        s->xwayland = wlr_xwayland_create(s->display, s->compositor, s->session == NULL);
        if (s->xwayland) {
            s->new_xsurface.notify = on_new_xsurface;
            wl_signal_add(&s->xwayland->events.new_surface, &s->new_xsurface);
            s->xwayland_ready.notify = on_xwayland_ready;
            wl_signal_add(&s->xwayland->events.ready, &s->xwayland_ready);
            setenv("DISPLAY", s->xwayland->display_name, true);
            fprintf(stderr, "wps-compositor: DISPLAY=%s\n", s->xwayland->display_name);
        } else {
            fprintf(stderr, "wps-compositor: no XWayland: X11 programs will not run\n");
        }
    }
    fprintf(stderr, "wps-compositor: WAYLAND_DISPLAY=%s\n", socket);
    /* (the capabilities are the devices': on_new_input sets them as each
     * comes; setting pointer-only here, after the backend had added the
     * keyboards, kept every program from asking for the keyboard) */
    wlr_seat_set_capabilities(s->seat, WL_SEAT_CAPABILITY_POINTER |
                              (wl_list_empty(&s->keyboards) ? 0 : WL_SEAT_CAPABILITY_KEYBOARD));
    if (startup) {
        char *argv_sh[] = { "/bin/sh", "-c", (char *)startup, NULL };
        comp_spawn(argv_sh, NULL, NULL);
    }
    set_cursor("default");   /* a pointer to be seen from the start (nothing sets one before the first motion) */
    idle_init();
    if (desktop) desktop_start(desktop);
    if (s->session) {
        fprintf(stderr, "wps-compositor: session active: %s\n", s->session->active ? "yes" : "no");
        session_active.notify = on_session_active;
        wl_signal_add(&s->session->events.active, &session_active);
    }
    if (!getenv("WPS_COMP_HEADLESS") || comp_debug) {
        heartbeat = wl_event_loop_add_timer(s->loop, on_heartbeat, NULL);
        wl_event_source_timer_update(heartbeat, 5000);
    }
    if (comp_debug) {
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);   /* (gdb, from the watchdog, may look) */
        tick_src = wl_event_loop_add_timer(s->loop, on_tick, NULL);
        wl_event_source_timer_update(tick_src, 1000);
        pthread_t th;
        if (pthread_create(&th, NULL, watchdog, NULL) == 0) pthread_detach(th);
    }
    const char *script = getenv("WPS_COMP_SCRIPT");
    if (script) load_script(script);
    wl_display_run(s->display);
    desktop_stop();
    if (s->session) wl_list_remove(&session_active.link);
    if (s->xwayland) {
        wl_list_remove(&s->new_xsurface.link);
        wl_list_remove(&s->xwayland_ready.link);
        wlr_xwayland_destroy(s->xwayland);
    }
    idle_fini();
    keymap_fini();
    if (new_kde.link.next) wl_list_remove(&new_kde.link);
    wl_display_destroy_clients(s->display);
    struct wl_listener *ls[] = { &s->cursor_motion, &s->cursor_motion_absolute, &s->cursor_button, &s->cursor_axis,
                                 &s->cursor_frame, &s->new_input, &s->request_cursor, &s->request_set_selection, &s->request_set_primary,
                                 &s->request_start_drag, &s->start_drag,
                                 &s->new_output, &s->new_toplevel, &s->new_popup, &s->new_decoration, &s->request_activate };
    for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) wl_list_remove(&ls[i]->link);
    wlr_scene_node_destroy(&s->scene->tree.node);
    wlr_xcursor_manager_destroy(s->cursor_mgr);
    wlr_cursor_destroy(s->cursor);
    wlr_allocator_destroy(s->allocator);
    wlr_renderer_destroy(s->renderer);
    wlr_backend_destroy(s->backend);
    wl_display_destroy(s->display);
    return 0;
}
