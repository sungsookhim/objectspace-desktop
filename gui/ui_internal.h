/* Internal to libwpsui: surface and app structures. */
#ifndef WPS_UI_INTERNAL_H
#define WPS_UI_INTERNAL_H

#include <pthread.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "ui.h"
#include "ui_damage.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define UI_BUFFERS 3

struct ui_buffer {
    struct wl_buffer *wb;
    void *data;
    size_t size;
    int w, h;              /* device pixels */
    bool busy;             /* with the compositor */
    long frame;            /* frame number drawn into it; -1: contents unknown */
};

struct ui_surface {
    ui_app *app;
    const char *client_cursor;   /* the pointer's shape over the client area (NULL: the default) */
    ui_surface *next;
    const ui_surface_ops *ops;
    void *data;
    struct wl_surface *wl;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;
    struct xdg_popup *popup;
    ui_rect pop_anchor;    /* where it was put, for a new size */
    bool pop_above;        /* opens above its anchor (drawers) */
    int pop_side;          /* opens beside it: -1 out to the left, 1 to the right, 0 neither */
    ui_surface *parent;
    double w, h;           /* logical: the window itself */
    double m;              /* a margin around it for its shadow (0: none) */
    double geo_m, geo_w, geo_h;   /* the window geometry last told the compositor */
    char opaque_key[64];   /* the opaque region last told ("": none) */
    int scale;
    bool configured, maximized, activated, destroyed;
    bool frameless;          /* no frame at all (the desktop) */
    bool own_title_font;     /* its own title font (a folder's) */
    ui_font title_font;
    bool own_title_color;    /* its own active title bar colour (a folder's) */
    ui_color title_color;
    bool needs_draw, frame_pending, continuous;
    struct wl_callback *frame_cb;   /* destroyed with the surface */
    struct ui_buffer bufs[UI_BUFFERS];
    ui_damage_frame hist[UI_DAMAGE_HISTORY];
    int nhist;
    ui_damage_frame cur;
    bool cur_any;
    long frame_no;
    char title[256];
    ui_hit hover;          /* frame part under the pointer */
    ui_hit pressed;        /* frame button held down */
    ui_layer *layer;       /* a scroll layer, if any */
    bool layer_dirty;
    bool pointer_inside;
    ui_frame_stat *stats;
    /* accessibility (ui_a11y.c) */
    unsigned a11y_id;
    ui_role a11y_role;
    char *a11y_desc;
    struct ui_a11y_node *a11y_focus;   /* the element with the keyboard, or NULL */
    size_t nstats, capstats;
    double last_draw_t;
};

struct post { struct post *next; void (*fn)(void *); void *arg; };

struct ui_app {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    int comp_version;
    struct wl_shm *shm;
    struct wl_subcompositor *subcomp;
    struct xdg_activation_v1 *activation;
    /* Input methods (ui_textinput.c): text-input-v3. */
    struct zwp_text_input_manager_v3 *ti_manager;
    struct zwp_text_input_v3 *ti;
    ui_surface *ti_entered;          /* the surface the input method is on */
    ui_surface *ti_surface;          /* the entry being typed in, and where */
    struct ui_entry *ti_entry;
    ui_rect ti_rect;
    bool ti_enabled;
    char *ti_preedit, *ti_commit;    /* pending until done */
    int32_t ti_pre_cursor;
    uint32_t ti_del_before, ti_del_after;
    struct wl_data_device_manager *ddm;
    int ddm_version;
    struct wl_data_device *ddev;
    struct offer *drag_offer;       /* a drag over one of our surfaces */
    ui_surface *drag_focus;
    double drag_x, drag_y;
    uint32_t drag_serial;
    const char *drag_mime;
    struct wp_viewporter *viewporter;
    struct xdg_wm_base *wm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *kstate;
    struct wl_cursor_theme *cursor_theme;
    int cursor_scale;
    struct wl_surface *cursor_surface;
    const char *cursor_name;
    ui_surface *surfaces;
    ui_surface *ptr_focus, *key_focus;
    double px, py;
    uint32_t enter_serial, last_serial;
    int last_button;
    int swallow_release;
    double last_click_t, last_click_x, last_click_y;
    int axis_source;
    unsigned mods;
    int repeat_fd;
    int repeat_rate, repeat_delay;
    uint32_t repeat_key;
    int wake_fd;
    pthread_mutex_t post_mu;
    struct post *posts, *posts_tail;
    bool running;
};

void ui_set_cursor(ui_app *a, const char *name);
/* Menus close themselves on presses elsewhere in this program (compositors
 * only dismiss them for clicks on other programs' windows). */
bool ui_menu_press_outside(ui_surface *s);   /* true: a menu closed; the press is used up */
ui_surface *ui_menu_key_surface(void);       /* where keys go while a menu is open */
void ui_menu_close_all(void);
void ui_menu_forget_surface(ui_surface *s);   /* before s is destroyed */
void ui_container_menu_closed(void);          /* a menu closed: its emphasis goes */
/* Draw the layer's changes; -1 if no buffer is free yet. */
double ui_layer_render(ui_layer *l);
void ui_dnd_init(ui_app *a);
void ui_dnd_renegotiate(ui_app *a);
struct pollfd;
size_t ui_watch_fill(struct pollfd *pf, size_t max);
void ui_watch_dispatch(const struct pollfd *pf, size_t n);
/* For ui_surface_verify: paste the slice last shown onto `img` (device
 * pixels), and draw the same window of content afresh onto `cr`. */
bool ui_layer_paste_shown(ui_layer *l, unsigned char *img, int stride);
void ui_layer_draw_fresh(ui_layer *l, cairo_t *cr);

/* ui_textinput.c */
void ui_textinput_bind(ui_app *a, struct wl_registry *r, uint32_t name, uint32_t ver);
void ui_textinput_seat(ui_app *a);
void ui_textinput_surface_gone(ui_surface *s);
void ui_textinput_key(ui_app *a, ui_surface *s, const char *utf8);   /* deliver text as keys */
/* ui_ibus.c: IBus directly, where the session asks for it (GTK_IM_MODULE=ibus) */
void ui_ibus_focus(ui_app *a, bool in);
bool ui_ibus_key(ui_app *a, uint32_t key, uint32_t sym, bool pressed);   /* true: IBus took it */

/* ui_a11y.c */
void ui_a11y_init(ui_app *a);
void ui_a11y_surface_new(ui_surface *s);
void ui_a11y_surface_gone(ui_surface *s);
void ui_a11y_activated(ui_surface *s, bool on);
void ui_a11y_shown(ui_surface *s);

#endif
