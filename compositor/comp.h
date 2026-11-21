/* wps-compositor's shared structures (wps-compositor.c: the core;
 * desktop.c: the link to WPS's desktop process). */
#ifndef WPS_COMP_H
#define WPS_COMP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

#include "ui.h"

enum mode { MODE_PASS, MODE_MOVE, MODE_RESIZE, MODE_BUTTON };
enum kind { KIND_NORMAL, KIND_DESKTOP, KIND_PALETTE };   /* palette: the LaunchPad, above */

struct server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_session *session;      /* on a console (the WPS session): screens and input devices */
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_tree *layer_desktop, *layer_normal, *layer_palette;
    struct wlr_scene_rect *background;   /* under everything: a colour until WPS's desktop is there */
    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_toplevel, new_popup;
    struct wl_list toplevels;   /* front (focused) first */
    struct wlr_xdg_decoration_manager_v1 *deco_mgr;
    struct wl_listener new_decoration;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion, cursor_motion_absolute, cursor_button, cursor_axis, cursor_frame;
    const char *cursor_image;

    struct wlr_seat *seat;
    struct wl_listener new_input, request_cursor, request_set_selection, request_set_primary;
    struct wl_listener request_start_drag, start_drag, drag_destroy;
    struct wlr_scene_tree *layer_drag;   /* a dragged object's icon, above all windows */
    struct wlr_scene_tree *drag_icon;
    struct wl_list keyboards;

    enum mode mode;
    struct toplevel *grabbed;
    double grab_x, grab_y;
    struct wlr_box grab_box;
    uint32_t resize_edges;
    double last_click_ms;
    double press_x, press_y;          /* where a move began */
    struct toplevel *last_click_on;

    struct wlr_keyboard *script_kb;   /* the script's own keyboard state (xkb) */
    struct wlr_xdg_activation_v1 *activation;
    struct wlr_compositor *compositor;
    struct wlr_xwayland *xwayland;    /* X11 programs (lazily started) */
    struct wl_listener new_xsurface, xwayland_ready;
    struct wl_listener request_activate;
    bool focus_window_list;           /* Ctrl+Esc: the Window List takes the keyboard when it shows */
};

struct output {
    struct wl_list link;
    struct server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame, request_state, destroy;
    double last_frame_ms;               /* when the last frame was shown */
    struct wl_event_source *pace;       /* a frame that came too soon waits for it */
};

struct toplevel {
    struct wl_list link;
    struct server *server;
    struct wlr_xdg_toplevel *xdg;     /* a Wayland window, or */
    struct wlr_xwayland_surface *xw;  /* an X11 one (xdg NULL) */
    struct wlr_scene_tree *tree;      /* the whole window, at its position */
    struct wlr_scene_tree *client;    /* the program's surfaces, inside the frame */
    struct wlr_scene_buffer *frame;   /* WPS's frame (server side), or disabled */
    struct wlr_xdg_toplevel_decoration_v1 *deco;
    bool ssd;                         /* our frame */
    bool mapped, minimized, maximized, activated;
    enum kind kind;
    struct wlr_box restore;           /* the frame box before maximizing */
    unsigned id;                      /* for the desktop's Window List */
    char *sent;                       /* the last window line sent for it */
    bool listed;                      /* in the desktop's Window List */
    ui_hit hover, pressed;
    int fw, fh;                       /* the frame drawn, its size */
    struct wl_listener map, unmap, commit, destroy, request_move, request_resize, request_maximize,
        request_minimize, request_fullscreen, set_title, set_app_id;
    struct wl_listener deco_request_mode, deco_destroy;
    struct wl_listener associate, dissociate, request_configure, request_activate, set_decorations;   /* X11 */
};

struct keyboard {
    struct wl_list link;
    struct server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers, key, destroy;
};

/* What a scene node belongs to (node->data). */
struct tag { int type; struct toplevel *t; };   /* type 1: a frame */

extern struct server server;
extern bool comp_debug;
double comp_uptime(void);   /* seconds since the start, for the log */
#define DBG(...) do { if (comp_debug) { fprintf(stderr, "wps-compositor: %.3f ", comp_uptime()); fprintf(stderr, __VA_ARGS__); } } while (0)

/* wps-compositor.c: a window, Wayland or X11 */
const char *view_title(struct toplevel *t);
const char *view_app(struct toplevel *t);
struct wl_client *view_client(struct toplevel *t);
struct wlr_box view_geo(struct toplevel *t);       /* the program's own size (and its shadow's offset) */
void comp_focus(struct toplevel *t);
void comp_minimize(struct toplevel *t, bool on);
void comp_maximize(struct toplevel *t, bool on);
void comp_close(struct toplevel *t);
void comp_redraw_frames(void);   /* the scheme or font changed */
void comp_before_lock(void);     /* nothing has the keyboard or the pointer */
void comp_after_unlock(void);

/* lock.c: the lock screen */
/* idle.c: screens off and the lock after idle time, idle inhibitors, logind */
void idle_init(void);
void idle_fini(void);
bool idle_activity(void);        /* input came; true if it woke the screens (then not passed on) */

bool lock_screen(void);          /* false: it cannot lock (no PAM) */
bool lock_active(void);
void lock_key(uint32_t sym, const char *utf8);
void lock_layout(void);          /* the screens changed */
struct toplevel *comp_toplevel_of(struct wlr_surface *s);

/* Start a program (sh -c COMMAND, or argv) apart from the compositor: a
 * double fork, so it is never the compositor's child (no zombies, and
 * SIGCHLD stays as it is: XWayland, which wlroots starts, needs it).
 * prepare runs in the program's process before it starts (or NULL).
 * Returns false if it could not be started. */
bool comp_spawn(char *const argv[], void (*prepare)(void *), void *arg);

/* desktop.c: WPS's desktop process, started and followed as the GNOME
 * extension does (the same lines on its standard input and output). */
void desktop_start(const char *command);
bool desktop_is(struct wl_client *c);               /* the desktop process's connection */
void desktop_window(struct toplevel *t, const char *change);   /* "add", "change", "remove" */
void desktop_event(struct toplevel *t, const char *name);     /* window.open, .close, ... */
bool desktop_key(const char *what);                 /* "window-list" (Ctrl+Esc) */
void desktop_stop(void);

#endif
