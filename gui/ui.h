/*
 * libwpsui: the WPS widget set (gui-design.md: Toolkit, Widget set).
 *
 * Our own drawing with Cairo, HarfBuzz and FreeType on Wayland. One thread
 * (the UI thread) owns everything here; other threads hand results over
 * with ui_app_post(). Drawing is by damage: a surface redraws only what
 * changed, and scrolling moves pixels already drawn (ui_damage.h).
 *
 * Coordinates are logical pixels (doubles, but widgets keep them whole so
 * moves are exact at any integer scale).
 */
#ifndef WPS_UI_H
#define WPS_UI_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { double x, y, w, h; } ui_rect;
typedef struct { double r, g, b, a; } ui_color;

static inline bool ui_rect_has(ui_rect r, double x, double y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static inline bool ui_rect_meets(ui_rect a, ui_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* ---------------------------------------------------------------- scheme */

/* Colours, fonts and metrics (an object in the GUI; here a struct). The
 * appearance settings of gui-design.md are fields: corner radius 0, title
 * bar buttons always visible, visible sizing border, no shadows. */
typedef struct {
    const char *name;
    ui_color face, light, shadow, dark, text, disabled_text;
    ui_color title_active, title_inactive, title_text, title_text_inactive;
    ui_color client_bg, alt_row, selection, selection_text, cursor;
    ui_color menu_bg, menu_text, menu_hilite, menu_hilite_text;
    ui_color placeholder;
    ui_color shadow_text;      /* titles of shadows */
    double font_px;
    double border, title_h, button_w, button_h;
    double corner_radius;      /* 0: square (the default) */
    bool buttons_on_hover;     /* false: always visible (the default) */
    int shadows;               /* 0 off (the default), 1 subtle, 2 strong */
    int translucency;          /* 0 off (the default), 1 title bars, 2 menus */
    bool title_mirrored;       /* buttons on the left, system menu on the right */
} ui_scheme;

extern ui_scheme ui_scheme_now;
/* Switch the colours to a scheme ("classic", "dark", "high contrast"),
 * keeping the window options; false if unknown. */
bool ui_scheme_use(const char *name);
/* A window option, as the System object names them: corner_radius,
 * buttons (always, on hover), border (visible, thin, none), shadows (off,
 * subtle, strong), translucency (off, title bars, menus), title_layout
 * (normal, mirrored, compact). False if unknown. */
bool ui_scheme_set_option(const char *key, const char *value);
/* The frame border: "visible" (4 px), "thin" (1 px) or "none". */
void ui_scheme_set_border(const char *mode);
/* "classic" (default), "dark"; WPS_UI_SCHEME, WPS_UI_CORNER_RADIUS and
 * WPS_UI_BUTTONS=hover override until the scheme objects exist (G3). */
void ui_scheme_init(void);
void ui_set_color(cairo_t *cr, ui_color c);

/* ------------------------------------------------------------------ text */

typedef enum { UI_FONT_REGULAR, UI_FONT_BOLD } ui_font;

void ui_text_init(void);
/* The interface font: a fontconfig family and a size in pixels (GNOME's,
 * G3); every string is shaped again. */
void ui_text_set_font(const char *family, double px);
double ui_text_width(ui_font f, const char *s);
/* Draw at a baseline, cut with "..." beyond max_w (0: no limit). Shaped
 * runs are cached, so redrawing the same strings costs little. */
double ui_text_draw(cairo_t *cr, ui_font f, double x, double baseline, const char *s, double max_w);
double ui_text_ascent(void);    /* for centring text in a row */
double ui_text_caret_x(ui_font f, const char *s, size_t at);   /* x before byte `at` */
size_t ui_text_hit(ui_font f, const char *s, double x);        /* byte nearest to x */

/* --------------------------------------------------------------- drawing */

void ui_bevel(cairo_t *cr, ui_rect r, bool raised);
void ui_fill(cairo_t *cr, ui_rect r, ui_color c);

/* ------------------------------------------------------------- app, input */

typedef struct ui_app ui_app;
typedef struct ui_surface ui_surface;

enum { UI_BTN1 = 1, UI_BTN2 = 2, UI_BTN3 = 3 };   /* OS/2 numbering: 2 is the right button */
enum { UI_MOD_SHIFT = 1, UI_MOD_CTRL = 2, UI_MOD_ALT = 4 };
typedef enum { UI_PRESS, UI_RELEASE, UI_MOTION, UI_ENTER, UI_LEAVE, UI_WHEEL } ui_ptr_kind;

typedef struct {
    ui_ptr_kind kind;
    double x, y;              /* surface coordinates */
    int button;               /* UI_BTN1.. for press and release */
    int clicks;               /* 1, or 2 for a double click */
    double wheel;             /* logical pixels, + is down */
    unsigned mods;
    uint32_t serial;
} ui_pointer_event;

typedef struct {
    uint32_t sym;             /* xkb keysym */
    char utf8[8];
    unsigned mods;
    bool repeat;
} ui_key_event;

/* Mouse mappings, as on OS/2's Mouse object (ui_mouse.c; <config>/mouse.conf).
 * Every mouse action in the GUI goes through these. */
typedef enum {
    UI_ACT_SELECT, UI_ACT_OPEN, UI_ACT_POPUP, UI_ACT_EDIT_TITLE, UI_ACT_DRAG, UI_ACT_WINDOW_LIST, UI_ACT_COUNT
} ui_mouse_action;
typedef struct { int button; unsigned mods; int clicks; } ui_chord;   /* clicks: 1, or 2 for a double click */
typedef struct {
    ui_chord act[UI_ACT_COUNT];
    bool left_handed;          /* buttons 1 and 2 swapped */
    int double_click_ms;
    double wheel_speed;        /* lines per wheel step */
} ui_mouse_config;
/* One mouse setting by name (an action's chord, left_handed,
 * double_click_ms, wheel_speed), as from the Mouse object; false when the
 * value cannot be used. */
bool ui_mouse_set(const char *key, const char *value);
extern ui_mouse_config ui_mouse;
void ui_mouse_load(void);
bool ui_mouse_parse_chord(const char *s, ui_chord *out);
bool ui_mouse_is(ui_mouse_action a, const ui_pointer_event *e);
int ui_mouse_button(ui_mouse_action a);

ui_app *ui_app_new(void);
void ui_app_run(ui_app *a);            /* until ui_app_quit or no windows remain */
void ui_app_quit(ui_app *a);
/* File descriptors watched by the main loop (UI thread). */
void ui_app_watch_fd(ui_app *a, int fd, short events, void (*fn)(void *arg, int fd, short revents), void *arg);
void ui_app_unwatch_fd(ui_app *a, int fd);
/* Write without blocking, then close fd (a drag's data to a pipe). */
void ui_app_write_async(ui_app *a, int fd, const char *data, size_t len);

/* Drag and drop (ui_dnd.c), with Wayland's actions. */
enum { UI_DND_COPY = 1, UI_DND_MOVE = 2, UI_DND_ASK = 4 };
typedef void (*ui_drag_send_fn)(void *ctx, const char *mime, int fd);   /* write the data, then close */
typedef void (*ui_drag_end_fn)(void *ctx, bool dropped, uint32_t action);
bool ui_drag_start(ui_surface *origin, uint32_t serial, const char *const *mimes, size_t n, uint32_t actions,
                   cairo_surface_t *icon, int icon_scale, ui_drag_send_fn send, ui_drag_end_fn end, void *ctx);

/* Run fn(arg) on the UI thread, from any thread. */
void ui_app_post(ui_app *a, void (*fn)(void *), void *arg);
int ui_app_scale_override(void);       /* WPS_UI_SCALE, or 0 */
double ui_now_ms(void);
/* The serial of the last input event (for a menu or popup opened by a key). */
uint32_t ui_app_input_serial(ui_app *a);

/* --------------------------------------------------------------- surfaces */

/* What a surface's owner implements. */
typedef struct {
    /* Draw everything that meets `clip` (logical). The cairo context is
     * already clipped to the damaged area. */
    void (*draw)(ui_surface *s, cairo_t *cr, ui_rect clip);
    void (*pointer)(ui_surface *s, const ui_pointer_event *e);
    void (*key)(ui_surface *s, const ui_key_event *e);
    void (*resized)(ui_surface *s, double w, double h);
    void (*focus)(ui_surface *s, bool on);
    /* A popup the compositor closed (click elsewhere). */
    void (*dismissed)(ui_surface *s);
    void (*close)(ui_surface *s);       /* a window's close request */
    /* Drag and drop onto this surface: which action a drop at (x, y) would
     * take (UI_DND_*, 0 to refuse) and which offered type it would read. */
    uint32_t (*drag_motion)(ui_surface *s, double x, double y, const char *const *mimes, size_t n,
                            uint32_t src_actions, unsigned mods, const char **mime);
    void (*drag_leave)(ui_surface *s);
    /* `start_action` is what the compositor chose when the drag began:
     * keys pressed during a drag reach only the compositor (GNOME), so a
     * change of its choice is the only sign of them. */
    /* Returns what it did, for the source: UI_DND_MOVE, UI_DND_COPY (a
     * copy, a shadow, or a choice still to be made in a menu), or 0 for
     * nothing (the drop is cancelled). */
    uint32_t (*drop)(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len,
                     uint32_t action, uint32_t start_action, unsigned mods);
    /* Each frame, before drawing (animations, benchmarks). */
    void (*frame)(ui_surface *s, double now_ms);
} ui_surface_ops;

ui_surface *ui_window_new(ui_app *a, const ui_surface_ops *ops, void *data, const char *app_id, double w, double h);
void ui_window_set_title(ui_surface *s, const char *title);
/* No frame at all, the client area the whole window (the desktop). */
void ui_window_set_frameless(ui_surface *s, bool on);
/* The window sets its own size (the compositor takes it on the next
 * frame); fixed: the user cannot resize it (min = max). */
void ui_window_set_size(ui_surface *s, double w, double h, bool fixed);
void ui_window_set_maximized(ui_surface *s);
void ui_window_move(ui_surface *s, uint32_t serial);
void ui_window_resize(ui_surface *s, uint32_t serial, unsigned edges);   /* xdg resize edges */
void ui_window_minimize(ui_surface *s);
void ui_window_toggle_maximize(ui_surface *s);
bool ui_window_maximized(ui_surface *s);
bool ui_window_active(ui_surface *s);
/* Bring a window forward, in answer to the user's latest click or key
 * (Wayland lets a program do so only then: xdg-activation). */
void ui_window_activate(ui_surface *s);

/* A popup at (x, y) of `parent`, `w` x `h`, grabbing input. */
ui_surface *ui_popup_new(ui_app *a, ui_surface *parent, const ui_surface_ops *ops, void *data,
                         ui_rect anchor, double w, double h, uint32_t serial);

/* The same, opening above the anchor (a LaunchPad drawer). */
ui_surface *ui_popup_new_above(ui_app *a, ui_surface *parent, const ui_surface_ops *ops, void *data,
                               ui_rect anchor, double w, double h, uint32_t serial);
/* A popup's new size, placed again by the compositor. */
void ui_popup_set_size(ui_surface *s, double w, double h);

void ui_surface_destroy(ui_surface *s);
void *ui_surface_data(ui_surface *s);
ui_app *ui_surface_app(ui_surface *s);
double ui_surface_width(ui_surface *s);
double ui_surface_height(ui_surface *s);
int ui_surface_scale(ui_surface *s);

/* Deliver an event as if it came from the seat (self-tests). */
void ui_surface_send_key(ui_surface *s, const ui_key_event *e);

/* Damage: redraw `r` at the next frame. */
void ui_surface_damage(ui_surface *s, ui_rect r);
void ui_surface_damage_all(ui_surface *s);
/* The contents of `area` moved up by dy (logical, whole pixels); the
 * uncovered strip is damaged. */
void ui_surface_scroll(ui_surface *s, ui_rect area, double dy);
/* Keep drawing every frame even without damage (benchmarks). */
void ui_surface_set_continuous(ui_surface *s, bool on);

/* Scroll layers: an area of a surface showing a window of tall content,
 * scrolled without moving pixels (ui_layer.c). Content coordinates: x as
 * on the surface, y from the top of the content. Needs wl_subcompositor
 * and wp_viewporter; ui_layer_new returns NULL without them. */
typedef struct ui_layer ui_layer;
typedef void (*ui_layer_draw_fn)(void *ctx, cairo_t *cr, ui_rect clip);
ui_layer *ui_layer_new(ui_surface *s, ui_layer_draw_fn draw, void *ctx);
void ui_layer_free(ui_layer *l);
void ui_layer_set_area(ui_layer *l, ui_rect area);      /* whole logical pixels */
void ui_layer_set_offset(ui_layer *l, double y);        /* content y at the area's top */
void ui_layer_damage(ui_layer *l, ui_rect content);
void ui_layer_damage_all(ui_layer *l);

/* Check drawing by damage against a full redraw: rebuild what the
 * compositor shows (the last buffer, with any layer's visible slice placed
 * on it) and compare it with the whole window drawn afresh. Returns the
 * number of differing pixels, or SIZE_MAX when there is a change not yet
 * drawn (nothing to compare). With `png_prefix`, mismatches are saved as
 * <prefix>-shown.png and <prefix>-expected.png. */
size_t ui_surface_verify(ui_surface *s, const char *png_prefix);
/* Save the buffer last given to the compositor (with any shadow margin
 * and transparency) as a PNG (self-tests). False if there is none yet. */
bool ui_surface_snapshot(ui_surface *s, const char *png);

/* Per-frame statistics (for benchmarks). */
typedef struct { double draw_ms, move_ms, interval_ms, drawn_fraction; bool full; } ui_frame_stat;
void ui_surface_stats(ui_surface *s, const ui_frame_stat **stats, size_t *n);

/* ------------------------------------------------------------------ frame */

/* The window frame (title bar, buttons, sizing border), drawn by the
 * window's owner inside its draw callback with these helpers. */
typedef enum { UI_HIT_NONE, UI_HIT_CLIENT, UI_HIT_TITLE, UI_HIT_SYSMENU, UI_HIT_MIN, UI_HIT_MAX, UI_HIT_CLOSE, UI_HIT_BORDER } ui_hit;
/* A frame apart from any surface (ui_framepaint.c): what the toolkit's
 * windows use, and what wps-compositor draws around other programs'. */
typedef struct {
    double w, h;               /* the whole window, frame included */
    bool frameless, maximized, activated, pointer_inside;
    ui_hit hover, pressed;     /* the part under the pointer, the button held */
} ui_frame_geom;
ui_rect ui_frame_geom_client(const ui_frame_geom *g);
ui_hit ui_frame_geom_hit(const ui_frame_geom *g, double x, double y);
ui_rect ui_frame_geom_button(const ui_frame_geom *g, int which);   /* 0 system menu, 1 min, 2 max, 3 close */
void ui_frame_geom_draw(const ui_frame_geom *g, cairo_t *cr, ui_rect clip, const char *title);

ui_rect ui_frame_client(ui_surface *s);              /* inside the frame */
void ui_frame_draw(ui_surface *s, cairo_t *cr, ui_rect clip, const char *title);
ui_hit ui_frame_hit(ui_surface *s, double x, double y);
/* Standard handling of frame clicks; returns true if it used the event. */
bool ui_frame_pointer(ui_surface *s, const ui_pointer_event *e);
void ui_frame_damage(ui_surface *s);                 /* title bar, e.g. on focus change */
void ui_frame_system_menu(ui_surface *s, uint32_t serial);

/* ------------------------------------------------------------------ menus */

typedef struct ui_menu_item {
    int64_t id;
    char *label;               /* "-" is a separator */
    bool enabled, checked;
    struct ui_menu_item *items;
    size_t nitems;
} ui_menu_item;

typedef void (*ui_menu_fn)(void *arg, int64_t id);   /* id -1: closed with no choice */

/* Pop up a menu at (x, y) of `parent`. `items` is copied. */
void ui_menu_popup(ui_surface *parent, const ui_menu_item *items, size_t n, double x, double y, uint32_t serial,
                   ui_menu_fn fn, void *arg);
void ui_menu_items_free(ui_menu_item *items, size_t n);
bool ui_menu_is_open(void);

/* ---------------------------------------------------------------- entries */

/* A one-line text entry: draws into a rectangle; the owner routes keys and
 * pointer events to it and redraws on anything but UI_ENTRY_NONE. */
typedef struct ui_entry ui_entry;
typedef enum { UI_ENTRY_NONE, UI_ENTRY_REDRAW, UI_ENTRY_CHANGED, UI_ENTRY_COMMIT, UI_ENTRY_CANCEL } ui_entry_result;
ui_entry *ui_entry_new(const char *text);
void ui_entry_free(ui_entry *e);
const char *ui_entry_text(ui_entry *e);
void ui_entry_set_text(ui_entry *e, const char *text);
void ui_entry_select_all(ui_entry *e);
size_t ui_entry_cursor(ui_entry *e);         /* a byte offset */
/* Text an input method is composing (NULL: none), drawn at the cursor;
 * cursor: bytes into it. */
void ui_entry_set_preedit(ui_entry *e, const char *text, int cursor);
const char *ui_entry_preedit(ui_entry *e);
extern void (*ui_entry_free_hook)(ui_entry *e);   /* the toolkit's, for input methods */
/* Input methods (CJK and others, through the compositor): the entry being
 * typed in on surface s, at r (surface coordinates: the input method's
 * candidate window goes by it), or NULL when none is. An entry's owner
 * calls it when the entry gets or loses the keyboard, and again when it
 * moves; committed text then arrives as key events (sym 0, utf8 the
 * character). */
void ui_text_input_focus(ui_surface *s, ui_entry *e, ui_rect r);
/* ---------------------------------------------------------- accessibility */

/* Screen readers (G4; AT-SPI, ui_a11y.c): each window, and in it the
 * element that has the keyboard, as the owner describes it. The toolkit
 * does this itself for containers (the item with the cursor), menus and
 * dialogs; owners of other controls call ui_a11y_focus as the focus moves
 * among them. */
typedef enum {
    UI_ROLE_NONE, UI_ROLE_LIST_ITEM, UI_ROLE_TREE_ITEM, UI_ROLE_ENTRY, UI_ROLE_BUTTON, UI_ROLE_CHECK_BOX,
    UI_ROLE_COMBO_BOX, UI_ROLE_MENU_ITEM, UI_ROLE_PAGE_TAB, UI_ROLE_SPIN_BUTTON, UI_ROLE_ICON,
    /* windows */
    UI_ROLE_FRAME, UI_ROLE_DIALOG, UI_ROLE_MENU, UI_ROLE_DESKTOP, UI_ROLE_TOOL_BAR,
} ui_role;
typedef struct {
    ui_role role;
    const char *name;          /* what is read first: a title, a label */
    const char *description;   /* then: the kind of object, a hint */
    const char *text;          /* an entry's text (NULL: none) */
    bool checked, selected, expandable, expanded, disabled;
    int index, count;          /* "3 of 10" (count 0: not in a set) */
    ui_rect at;                /* where, in the window */
} ui_a11y_info;
/* The element of window s with the keyboard (NULL: none); copied. */
void ui_a11y_focus(ui_surface *s, const ui_a11y_info *info);
/* A window's role (default: frame) and description (e.g. a dialog's message). */
void ui_a11y_window(ui_surface *s, ui_role role, const char *description);

/* Self-tests: as if the input method had sent these (then done). */
void ui_text_input_inject(ui_app *a, const char *commit, const char *preedit);
void ui_entry_select_stem(ui_entry *e);     /* up to the last dot: a file's name without extension */
void ui_entry_draw(ui_entry *e, cairo_t *cr, ui_rect r, bool focused);
ui_entry_result ui_entry_key(ui_entry *e, const ui_key_event *k);
ui_entry_result ui_entry_pointer(ui_entry *e, const ui_pointer_event *p, ui_rect r);

/* -------------------------------------------------------------- notebooks */

/* Settings notebooks, as in Warp 4 (ui_notebook.c). Built from pages of
 * typed fields; `set` is called as each change is made. */
/* UI_FIELD_ICON: value.text is a PNG's path ("": the kind's icon). Its
 * owner's `set` gets value.text "" for Default, the path of an image
 * dropped on it (to be made into an icon), or NULL when Choose... was
 * pressed; the notebook shows the new icon when ui_notebook_update says. */
typedef enum { UI_FIELD_TEXT, UI_FIELD_CHECKBOX, UI_FIELD_NUMBER, UI_FIELD_CHOICE, UI_FIELD_ICON } ui_field_kind;
typedef struct { char *text; int64_t number; bool checked; } ui_value;   /* choice: number is the index */
typedef struct {
    const char *key, *label;
    ui_field_kind kind;
    ui_value value;
    const char *const *choices;
    size_t nchoices;
} ui_field_desc;
typedef struct { const char *title; const ui_field_desc *fields; size_t nfields; } ui_page_desc;
typedef struct ui_notebook ui_notebook;
typedef void (*ui_notebook_set_fn)(void *ctx, const char *key, ui_field_kind kind, const ui_value *v);
typedef void (*ui_notebook_close_fn)(void *ctx);
ui_notebook *ui_notebook_open(ui_app *a, const char *title, const ui_page_desc *pages, size_t n,
                              ui_notebook_set_fn set, ui_notebook_close_fn closed, void *ctx);
void ui_notebook_update(ui_notebook *nb, const ui_page_desc *pages, size_t n);
void ui_notebook_set_status(ui_notebook *nb, const char *msg);
void ui_notebook_close(ui_notebook *nb);
ui_surface *ui_notebook_surface(ui_notebook *nb);
void ui_notebook_show_page(ui_notebook *nb, size_t page);

typedef enum { UI_KIND_FILE, UI_KIND_FOLDER, UI_KIND_PROGRAM, UI_KIND_LINK, UI_KIND_OTHER } ui_kind;

/* ---------------------------------------------------------- action events */

/* Every user action is a named event (gui-design.md: Every action is an
 * event). The toolkit names what it handles itself (window.minimize,
 * window.maximize, window.restore, menu.open, menu.select); the
 * application names the rest and receives them all through one hook. */
typedef void (*ui_action_fn)(const char *name, ui_surface *s, ui_rect at);
void ui_set_action_hook(ui_action_fn fn);
void ui_action(const char *name, ui_surface *s, ui_rect at);

/* ---------------------------------------------------------------- dialogs */

/* A message and a row of buttons; fn gets the chosen button's index
 * (Escape and the close button choose the last one). */
typedef struct ui_dialog ui_dialog;
typedef void (*ui_dialog_fn)(void *ctx, int choice);
ui_dialog *ui_dialog_open(ui_app *a, const char *title, const char *message, const char *const *buttons, size_t n,
                          int default_button, ui_dialog_fn fn, void *ctx);
ui_surface *ui_dialog_surface(ui_dialog *d);
void ui_dialog_key(ui_dialog *d, const ui_key_event *e);   /* as if typed (self-tests) */

/* An object kind's icon (the GUI's own, until icon objects). */
void ui_draw_kind_icon(cairo_t *cr, ui_kind k, double x, double y, double size);

/* ------------------------------------------------------------- containers */


typedef struct {
    bool loaded;               /* false: draw a placeholder */
    bool detailed;             /* size and date known */
    ui_kind kind;
    const char *title, *name;  /* valid until the next call */
    int64_t size, mtime;
    bool shadow;               /* a shadow of another object: drawn as one */
    bool link;                 /* a plain file-system link: drawn as what it points to, with a link mark */
    bool is_template;          /* a template: drawn on a pad of paper (OS/2) */
    bool in_use;               /* in-use emphasis (OS/2): a folder with a window open (drawn open), or a file open in a program */
    cairo_surface_t *image;    /* its own icon (an app's), drawn instead of the kind's; NULL: the kind's */
    /* Tree view only. */
    int depth;
    bool expandable, expanded;
    bool last_child;           /* no sibling below it (for the joining lines) */
    uint32_t open_levels;      /* bit d: the ancestor at depth d has siblings below */
} ui_item;

/* An object's icon, drawn as a folder draws it (in-use, template, link
 * and shadow marks included), anywhere: the LaunchPad, drawers. */
void ui_draw_item_icon(cairo_t *cr, const ui_item *it, double x, double y, double size);

typedef struct ui_container ui_container;

typedef struct {
    size_t (*count)(void *ctx);
    void (*item)(void *ctx, size_t i, ui_item *out);
    /* Items first..last are on screen (fetch them). */
    void (*visible)(void *ctx, size_t first, size_t last);
    void (*activate)(void *ctx, size_t i);
    /* Button 2 click: on item i, or on the background (i == SIZE_MAX). */
    void (*menu)(void *ctx, size_t i, double x, double y, uint32_t serial);
    /* In-place rename finished with a new name (NULL: not supported). */
    void (*rename)(void *ctx, size_t i, const char *name);
    /* Tree view: expand or collapse item i (expand: true). */
    void (*expand)(void *ctx, size_t i, bool expand);
    /* The drag button was pressed on item i and moved: start a drag. */
    void (*drag)(void *ctx, size_t i, uint32_t serial);
    /* Icon view with icons placed anywhere (as in OS/2); NULL: a grid that
     * flows with the window. Coordinates are the list's: (0, 0) is the
     * top left of the folder's contents. */
    void (*item_pos)(void *ctx, size_t i, double *x, double *y);
    size_t (*items_in)(void *ctx, ui_rect r, size_t *out, size_t max);   /* items whose cells meet r */
    void (*content_size)(void *ctx, double *w, double *h);
} ui_container_src;

#define UI_CELL_W 100            /* an icon's cell in icon view */
#define UI_CELL_H 70

typedef enum { UI_VIEW_ICON, UI_VIEW_DETAILS, UI_VIEW_TREE } ui_view;

ui_container *ui_container_new(ui_surface *s, const ui_container_src *src, void *ctx, ui_view view);
void ui_container_free(ui_container *c);
void ui_container_layout(ui_container *c, ui_rect r);
/* Details view: the title of the second column ("Name"), e.g. "Folder"; a
 * string that lives as long as the container. */
void ui_container_set_name_head(ui_container *c, const char *title);
void ui_container_draw(ui_container *c, cairo_t *cr, ui_rect clip);
bool ui_container_pointer(ui_container *c, const ui_pointer_event *e);
bool ui_container_key(ui_container *c, const ui_key_event *e);
/* Forget what was typed (type-ahead), as a pause does. */
void ui_container_typeahead_reset(ui_container *c);
void ui_container_set_view(ui_container *c, ui_view v);
ui_view ui_container_view(ui_container *c);
/* Whether it has the keyboard (its cursor is drawn); a new one has. */
void ui_container_set_focused(ui_container *c, bool on);
/* The model changed: items first..last (or the count). */
void ui_container_changed(ui_container *c, size_t first, size_t last);
void ui_container_count_changed(ui_container *c);
void ui_container_scroll_by(ui_container *c, double dy);
void ui_container_scroll_to_fraction(ui_container *c, double f);
size_t ui_container_selected_count(ui_container *c);
bool ui_container_is_selected(ui_container *c, size_t i);
void ui_container_select_none(ui_container *c);
/* Draw `fn` behind the items instead of the plain background (content
 * coordinates, as for scroll layers); titles then get a shadow, for
 * reading on a picture (the desktop). NULL: back to the plain one. */
void ui_container_set_backdrop(ui_container *c, ui_layer_draw_fn fn, void *ctx);
/* (Drawing then goes straight to the window, with no scroll layer: a
 * layer covering a whole surface stops its frame callbacks in Mutter.) */
void ui_container_select(ui_container *c, size_t i);
size_t ui_container_cursor(ui_container *c);          /* SIZE_MAX: none */
void ui_container_set_cursor(ui_container *c, size_t i);
/* The whole model was replaced (same scroll position): redraw all. */
void ui_container_reset(ui_container *c);
/* In-place rename of item i (F2 or Alt+click start it too). */
void ui_container_begin_rename(ui_container *c, size_t i);
bool ui_container_renaming(ui_container *c);
/* Drops: the item under (x, y) (SIZE_MAX: the background), and the target
 * emphasis on one item (SIZE_MAX: on the background, i.e. none). */
size_t ui_container_item_at(ui_container *c, double x, double y);
/* Surface point to list coordinates (icon view placement), and where the
 * drag that is starting was grabbed. */
void ui_container_to_list(ui_container *c, double x, double y, double *lx, double *ly);
void ui_container_drag_origin(ui_container *c, double *x, double *y);
/* The free layout changed (positions, arrangement): redraw everything. */
void ui_container_layout_changed(ui_container *c);
double ui_container_list_width(ui_container *c);
void ui_container_set_drop_target(ui_container *c, size_t i);
size_t ui_container_rename_item(ui_container *c);           /* SIZE_MAX: none */
/* Start editing item i's title (as F2 does). */
void ui_container_begin_rename(ui_container *c, size_t i);
/* After ui_container_reset: the item being renamed is now at i, or gone
 * (SIZE_MAX cancels the rename). */
void ui_container_rename_moved(ui_container *c, size_t i);

#endif
