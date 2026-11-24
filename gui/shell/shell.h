/* The WPS GUI's shell (wps-shell): what its files share - the folder
 * windows' state and every function and variable used across them.
 * Split from wps-shell.c (refactor R1); see each file's top for its part. */
#ifndef SHELL_H
#define SHELL_H


#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "layout.h"
#include "wallpaper.h"
#include "events.h"
#include "winlist.h"
#include "appicons.h"
#include "warpcenter.h"
#include "notify.h"
#include "messages.h"
#include "findwin.h"
#include "mime.h"
#include "icons.h"
#include "filepick.h"
#include "apppick.h"
#include "fontpal.h"
#include "colorpal.h"
#include "tooltip.h"
#include "session.h"
#include "iconpick.h"
#include "launchpad.h"
#include "gnome.h"
#include "sound.h"
#include "svc.h"
#include "ui.h"
#include "wps_menu_ids.h"

#define PAGE 256
#define STATUS_H status_h()

enum { P_NONE, P_NAMES, P_DETAILED };

struct row { char *name, *title; ui_kind kind; int64_t size, mtime; bool shadow, in_use, link, tpl; char *target, *icon,
             *icon_open;   /* its own open icon (shown while it is open), or NULL */
             bool open_hatch;   /* hatched while open, over the open icon too */
             float cellh;   /* its icon's cell with the whole title (0: not measured yet) */
             float cellw;   /* ... its width in the free layout (measured with cellh) */
             char *noext;   /* its title without the extension (Show file extensions off), made when shown */ };
struct page { int state; bool requested; struct row rows[PAGE]; };

/* A folder's contents as read from the service, a page at a time. */
/* A content event, kept until the batch it came in is applied. */
struct change { char kind; char *name, *to, *type; uint64_t version; int64_t index; };

struct model {
    struct page **pages;
    size_t npages, total;
    uint64_t version;        /* the service's listing version these rows match (0: unknown) */
    bool complete, growing;
    bool heights_dirty;      /* a title changed: whole-title rows may be taller or shorter */
    int pending;             /* detail pages still awaited before a swap */
};

struct folder {
    struct folder *next;
    long id;                 /* replies find their window by ID: it may have closed */
    ui_surface *win;
    ui_container *cont;
    char path[PATH_MAX];
    char title[256];
    struct model m;          /* on screen */
    struct model *sh;        /* being re-read after a change, swapped in when ready */
    unsigned gen, sgen;      /* replies for an older model are dropped */
    bool reload_again;       /* a change came during the re-read */
    int64_t sub;             /* event subscription */
    bool closing;
    bool desktop;            /* the desktop: frameless, full screen, on the wallpaper; never closes */
    size_t vis_first, vis_last;
    struct tnode *troot;     /* tree view */
    struct tnode **tvis;     /* its visible rows */
    size_t ntvis, captvis;
    unsigned tgen;           /* replies for a freed tree are dropped */
    char pending_open[PATH_MAX];   /* the program asked about, if any */
    char rename_new[NAME_MAX + 64];  /* a new object made here: edit its name once it is listed */
    struct change *changes;          /* content events not yet applied (applied together, next turn) */
    size_t nchanges, capchanges;
    bool changes_posted;
    ui_view pending_view;          /* how to open a folder the service names */
    icon_layout *lay;        /* icon view: where each icon is */
    bool lay_started;        /* its grid width is set */
    double want_scroll;      /* where it was scrolled when last closed (0: the top) */
    double open_w, open_h;   /* its size when opened (kept for a maximized one) */
    bool arranged_by_name;   /* the first, unstored arrangement was done */
    int sort_pending;        /* a Sort waiting for every row's details: its key + 1 */
    char **spot_new;         /* new objects whose spots are to be stored (store_spots) */
    size_t nspot_new;
    char error[256];
    /* its Background page (v0.3-design 3), followed by a subscription to
     * the folder object itself */
    int64_t bg_sub;
    char bg_image[PATH_MAX], bg_color[16], bg_img_path[PATH_MAX];
    char title_color[16];    /* its icons' titles' colour ("": the scheme's) */
    char title_bar_color[16];   /* its active title bar's ("": the scheme's) */
    bool work_area, wa_opened;  /* (0.4 step 9) a work area; its group asked back once */
    int wa_state;               /* its window as the compositor last said: 0 not yet, 1 shown, 2 minimized */
    char wa_group[PATH_MAX];    /* the work area it was opened from ("" none): minimized, restored, closed with it */
    int bg_mode;
    bool bg_gnome;
    bool whole_titles;       /* the Icon view page: titles whole (OS/2's) or on two lines */
    int free_layout;         /* the Icon view page's Layout: 1 free (OS/2's), 0 grid; -1 not read yet */
    bool show_hidden;        /* its View page (all folders' by default: class:WPFolder) */
    bool show_ext;
    bool lock_all;           /* ... Lock positions of all icons */
    char icon_font[128];     /* its own icon font (the Font Palette's), "" the System's */
    char icon_theme[128];    /* its icons' theme (its own, or all folders') */
    int icon_size;           /* its icon set (ui_icon_size; its own, or all folders') */
    char title_font[128];    /* its title bar's, "" all title bars' */
    ui_font font;            /* the one its titles are drawn in */
    cairo_surface_t *bg_img;     /* the picture, decoded */
    /* its details view (v0.3-design 4): the sort (choice 0 none, 1 name,
     * 2 type, 3 size, 4 last write; -1 not read yet) and columns */
    int sort_choice;
    bool sort_desc;
    unsigned cols;
    double col_w[6];
    /* its search field (v0.3-design 6): shown with Ctrl+F, filters its own
     * listing as typed */
    bool find_on, find_focus;
    ui_entry *find_entry;
    char filter[128];
    bool find_x_down;
    int find_place;          /* its View page's Search field: 0 top, 1 bottom, 2 floating */
    char msg_last[256];      /* the desktop's: the message its box last showed */
    bool msg_hidden, msg_sticky;
    int arrange_offered;     /* -1 not known yet; 1: its overlapping icons' Arrange offered once already */
    bool lay_placed_in;      /* its stored positions read (got_layout) */
    bool offer_arrange;      /* its status line offers Arrange now (icons overlapping: positions from 0.2) */   /* the desktop's: its message box gone; staying until clicked (an error) */
    /* benchmark */
    double bench_secs, bench_start, bench_next_jump;
    unsigned bench_seed;
    size_t bench_from;
    size_t frames_seen, verified, verify_failed, verify_diff;
};

struct bg_load { long folder; char path[PATH_MAX]; };

/* Which model and generation a reply belongs to. */
struct pctx { long id; unsigned gen; bool shadow; };
#define NSETUP 11
#define TIMING(f, fmt, ...) do { if ((f)->desktop && getenv("WPS_DEBUG_TIMING")) \
    fprintf(stderr, "timing: %7.0f ms: " fmt "\n", since_start(), ##__VA_ARGS__); } while (0)

/* The re-read model replaces the one on screen, keeping the scroll
 * position, the selection and the cursor (by name). */
/* What the user has in a window (selection, cursor, a name being edited),
 * kept by name across a change of its rows. */
struct keep { char **sel; size_t k; char *cur, *ren; size_t ren_i; };

/* Changes in a batch beyond this are cheaper as one read of the folder. */
#define MAX_FOLLOW 64


/* -------------------------------------------------------------- tree view */

/* The tree view shows folders only, as in OS/2. Each node's subfolders are
 * read when it is first expanded, sorted by name, and kept when it is
 * collapsed. An expanded node is watched for changes. */
struct tnode {
    struct tnode *parent;
    char *name, *path;
    int depth;
    bool expanded, loaded, loading;
    struct tnode **kids;
    size_t nkids;
    char **pending;          /* subfolder names while a read streams in */
    size_t npending, cappending;
    int64_t sub;
};

struct tctx { long folder; unsigned tgen; struct tnode *n; size_t offset; };

struct tsub { long folder; unsigned tgen; struct tnode *n; };

/* A program another user owns: show its command and ask (the service
 * never runs it unconfirmed). */
struct confirm_ctx { long folder; char target[PATH_MAX]; };

/* ------------------------------------------------------------ properties */

/* An object's settings notebook. Replies find it by ID: it may have
 * closed. The title field renames (the service renames files, titles the
 * rest), so the target path can change while it is open. */
struct props {
    struct props *next;
    long id;
    char target[PATH_MAX];
    char title[256];
    int64_t changes;
    ui_notebook *nb;
    size_t want_page;        /* reopened at login: the page it showed */
    char cls[64], cls_title[64];   /* its class (Class... opens all of them) */
    bool makes;                    /* a template making that class (no type menu) */
};

/* An icon made from an image (dropped, or chosen in the picker): the
 * object's icon now. */
struct icon_req { long props; char key[64]; };

/* Menus: the service gives the items; the GUI carries out the well-known
 * ones and sends the rest back. */
/* Where a new or dropped object lands (placed at x, y in icon view). */
struct land { long folder; char dest[PATH_MAX]; int64_t x, y; bool place; };

struct menu_ctx {
    long folder; size_t item; double x, y; uint32_t serial;
    char **tpl, **tpl_title;   /* New > (a background menu): the templates' addresses and titles */
    size_t ntpl;
};

/* ---- Drives (v0.2-design 5): their menu's actions, the passphrase */

struct drive_ctx { long folder; char target[PATH_MAX], action[16]; bool reopen; ui_dialog *dlg; };


/* ---------------------------------------------------------- drag and drop */

#define MIME_WPS "application/x-wps-objects"
#define MIME_URIS "text/uri-list"

/* What this program is dragging now (for refusing a folder dropped into
 * itself before the data is read). */
struct dragging {
    char **paths;
    bool *tpl;               /* templates: dropping one makes a new object */
    size_t n;
    char src_folder[PATH_MAX];
    char *wps, *uris;        /* the data in both formats */
    double *dx, *dy;         /* each icon's offset from the grab point (icon view) */
};

/* Jobs started by drops: progress in the target window, decisions asked. */
struct jobtrack {
    struct jobtrack *next;
    int64_t job;
    long folder;
    char kind[8];
    bool asking;
    char dest[PATH_MAX];   /* where the objects go */
};

struct xfer {
    long folder;
    char kind[8], dest[PATH_MAX], from[PATH_MAX];
    char **names;
    int64_t *x, *y;          /* where each lands (if place) */
    bool place;
    size_t n;
};

enum { DROP_MOVE, DROP_COPY, DROP_SHADOW };

/* Where dropped items land: on the window's own folder's background in icon
 * view, at the drop point (in list coordinates), each keeping its offset in
 * the drag; elsewhere (onto a folder icon, other views) wherever is free. */
struct drop_at { bool place; double lx, ly; };

/* The compositor said "ask" (Alt in GNOME): a menu at the drop point. */
struct ask_ctx { long folder; char dest[PATH_MAX]; char *mime, *data; struct drop_at at; };

struct another {
    long folder;
    char dir[PATH_MAX], cls[32], what[32], name[NAME_MAX + 32], tpl[PATH_MAX];
    ui_dialog *dlg;
};

struct decide_ctx { int64_t job; };

struct del {
    long folder;
    bool permanent;
    char parent[PATH_MAX];   /* the folder the files are in */
    char **names;            /* files and folders there */
    size_t n;
    char **vobjs;            /* central objects ("vobj:ID"): deleted outright */
    size_t nv;
    struct del *next;        /* the same request's files in another folder (a tree's selection) */
};

/* ------------------------------------------------------------------ window */

/* ---- the search field (v0.3-design 6): a row under the title bar, shown
 * with Ctrl+F; what is typed filters the window's own listing (wpsd's:
 * names containing it, any case); Esc or x: all again, the row gone. */

#define FIND_H 30
#define OFFER_TEXT "Some icons overlap (placed by an older WPS):"   /* the Arrange offer's words (a link after them) */

struct rec { char *buf; size_t len, cap; };

/* ---- variables */

extern ui_app *app;
extern struct folder *folders;
extern long next_folder_id;
extern double opt_bench;
extern ui_view opt_view;
extern char folders_font[128];
extern char titles_font[128];
extern char setup_ids[NSETUP][64];
extern char setup_cls[NSETUP][32];
extern ui_dialog *last_dialog;
extern int reloads;
extern struct props *props_open;
extern void (*menu_hook)(struct menu_ctx *m, const ui_menu_item *items, size_t n);
extern char setup_folder[PATH_MAX];
extern struct dragging *cur_drag;
extern struct jobtrack *jobs;
extern ui_dialog *last_dialog;
extern struct another *another_now;
extern bool confirm_delete, confirm_folder_delete;
extern bool hatch_open_all;   /* the System's: every open icon hatched behind too */
/* Whether target's Properties notebook is open (its icon hatched). */
bool props_is_open(const char *target);
extern const ui_container_src folder_src;
extern const ui_surface_ops win_ops;
extern double next_w, next_h;
extern bool restore_windows;
extern char ev_buf[8192];
extern size_t ev_len;

/* ---- functions */

double status_h(void);
struct folder *folder_by_id(long id);
void *as_arg(struct folder *f);
const char *row_shown(struct folder *f, struct row *r);
struct folder *from_arg(void *arg);
void head_clicked(void *ctx, int col);
void cols_resized(void *ctx, const double *w);
void bg_read(struct folder *f);
void folder_font(struct folder *f);
void bg_follow(struct folder *f);
struct page *page_at(struct model *m, size_t p);
void model_free(struct model *m);
const char *model_name(struct model *m, size_t i);
const char *view_name(ui_view v);
void update_title(struct folder *f);
void damage_status(struct folder *f);
void *page_arg(struct folder *f, bool shadow);
double since_start(void);
ssize_t model_find(struct model *m, const char *name);
struct row *row_ptr(struct model *m, size_t i);
void row_stale(struct model *m, size_t i);
void changes_clear(struct folder *f);
void got_page(void *arg, const cv *reply);
void request_more(struct folder *f, bool shadow);
void fetch_visible(struct folder *f, size_t first, size_t last);
void reload(struct folder *f);
void on_event(void *arg, const cv *ev);
void got_sub(void *arg, const cv *reply);
void tree_rebuild(struct folder *f, struct tnode *keep);
void tree_expand(struct folder *f, struct tnode *n, bool expand);
bool tree_event(struct folder *f, int64_t sub);
void tree_start(struct folder *f);
void tree_free(struct folder *f);
bool is_tree(struct folder *f);
void notify_in_use(const char *path);
size_t src_count(void *ctx);
void appicons_ready(void);
void src_item(void *ctx, size_t i, ui_item *out);
void src_visible(void *ctx, size_t first, size_t last);
void src_expand(void *ctx, size_t i, bool expand);
const char *row_name(struct folder *f, size_t i);
void target_of(struct folder *f, size_t i, char *out, size_t n);
void ignore_reply(void *arg, const cv *reply);
void opened(void *arg, const cv *reply);
/* workarea.c: Work Area folders (0.4 step 9) */
void workarea_act(struct folder *f, const char *act);   /* open, close, minimize, restore */
void workarea_settings(struct folder *f, bool on);
void workarea_window(const char *title, bool minimized);
const char *workarea_group_of(struct folder *f);   /* the work area f is (or is in, or was opened from), NULL none */
void workarea_join(struct folder *f, const char *group);
bool wps_command_event(const cv *ev);   /* the wps command's ask, selection, show (0.4.1) */
void workarea_focused(struct folder *f);
struct row *row_at(struct folder *f, size_t i);
void open_item(struct folder *f, size_t i, ui_view view);
void src_activate(void *ctx, size_t i);
const char *lay_name(void *ctx, size_t i);
double lay_h(void *ctx, size_t i);
double lay_w(void *ctx, size_t i);
int cols_now(struct folder *f);
/* Its icon set (its container's). */
const ui_icon_set *folder_icons(struct folder *f);
/* Icon positions are stored in the Small set's units (0.3's cell, 100 by
 * 80) and shown in the folder's set's, so an arrangement keeps its shape
 * when the size changes: these store positions shown in f. */
void folder_place(struct folder *f, const char *dest, const char *const *targets, const int64_t *x, const int64_t *y, size_t n, svc_fn fn, void *arg);
void folder_place_lock(struct folder *f, const char *target, int64_t x, int64_t y, bool lock, svc_fn fn, void *arg);
/* A position shown in f as stored, and back. */
void folder_pos_stored(struct folder *f, int64_t *x, int64_t *y);
void folder_pos_shown(struct folder *f, int64_t *x, int64_t *y);
/* Its icon set changed (got_bg): cells, layout and positions again. */
void folder_icons_changed(struct folder *f);
void layout_follow(struct folder *f, bool full);
void got_layout(void *arg, const cv *reply);
void placed_reply(void *arg, const cv *reply);
void arrange(struct folder *f);
void sort_icons(struct folder *f, int key);
void props_set(void *ctx, const char *key, ui_field_kind kind, const ui_value *v);
bool open_properties(const char *target);
void open_with_chosen(long owner, const char *target, const char *app_id, bool always);
void menu_chosen(void *arg, int64_t id);
void unlock_ask(struct folder *f, const char *target, bool reopen);
void drives_changed_event(void);
bool is_trash_dir(const char *p);
bool is_startup_dir(const char *p);
void startup_at_login(void);
void restore_items(struct folder *f, size_t i);
void lock_items(struct folder *f, size_t i);
void src_menu(void *ctx, size_t i, double x, double y, uint32_t serial);
void src_rename(void *ctx, size_t i, const char *name);
void uri_append(char **buf, size_t *len, size_t *cap, const char *path);
void src_drag(void *ctx, size_t i, uint32_t serial);
void shell_drag_paths(ui_surface *win, uint32_t serial, const char *const *paths, size_t n, ui_kind kind);
uint32_t win_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n, uint32_t src_actions, unsigned mods, const char **mime);
void win_drag_leave(ui_surface *s);
void set_status(long folder, const char *msg);
void raise_in(struct folder *f, const char *name, const char *object, const char *cls);
void report_error(long folder, const char *msg);
ui_dialog *ask(const char *title, const char *msg, const char *const *buttons, size_t n, int def, ui_dialog_fn fn, void *ctx);
void got_sel(void *arg, const cv *reply);
char *uri_path(const char *u);
bool reposition(struct folder *f, double x, double y);
int drop_how(uint32_t action, uint32_t start_action, unsigned mods);
uint32_t win_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len, uint32_t action, uint32_t start_action, unsigned mods);
void rename_new_now(struct folder *f);
void create_another(struct folder *f, size_t i);
void made_new(void *arg, const cv *reply);
void decided(void *arg, int choice);
void load_system_conf(void);
void delete_request(struct folder *f, size_t i, bool permanent);
bool dragged_onto_itself(struct folder *f, size_t item);
void startup_dropped(struct folder *f, const char *mime, const char *data);
void trash_dropped(struct folder *f, const char *mime, const char *data);
void on_job_event(const cv *st);
void find_focus(struct folder *f, bool on);
void win_draw(ui_surface *s, cairo_t *cr, ui_rect clip);
void layout(struct folder *f);
void win_resized(ui_surface *s, double w, double h);
void win_pointer(ui_surface *s, const ui_pointer_event *e);
void set_view(struct folder *f, ui_view v);
void win_key(ui_surface *s, const ui_key_event *e);
void win_focus(ui_surface *s, bool on);
void free_folder(struct folder *f);
void win_close(ui_surface *s);
void selftest_chosen(void *arg, int64_t id);
void win_frame(ui_surface *s, double now);
void report(struct folder *f);
void got_title(void *arg, const cv *reply);
void lp_props(const char *addr);
const char *setup_addr(const char *cls);
void font_apply(const char *what, const char *spec);
void font_dropped_dialogs(const char *spec, unsigned mods);
void font_dropped_wc(const char *spec);
/* The Colour Palette's (colorpal.h): its samples kept; a colour for the
 * scheme in use ("title_active", "bar": a title bar, the WarpCenter or
 * the LaunchPad dropped on). */
void color_apply(const char *what, const char *hex);
void color_dropped_bar(const char *hex);
void templates_dir(char *out, size_t n);
bool open_view(const char *v);
void got_setup(void *arg, const cv *reply);
void gnome_set_wps(const char *cls, const char *key, const char *value);
void gnome_font(void);
bool setup_event(const cv *ev);
void toolkit_action(const char *name, ui_surface *s, ui_rect at);
/* A folder opened as it was last closed (its place, size, scroll; with
 * VIEW_LAST its view too); the window comes once its properties are read
 * (NULL then; the window if it was open already). open_window: at once. */
#define VIEW_LAST ((ui_view)-1)
struct folder *open_folder(const char *path, ui_view view);
struct folder *open_folder_from(struct folder *from, const char *path, ui_view view);
void folder_window_save(struct folder *f);
/* The desktop's status line (0.4 step 5): its message (f->error) in a box
 * at the bottom of its usable area for a few seconds, an error until
 * clicked; desktop_say: a message of its own there (sticky: until clicked). */
void desktop_status(struct folder *f);
void desktop_say(const char *text, bool sticky);
void desktop_messages_enable(bool on);   /* the Messages object's setting */
/* Icons placed (by 0.2, for two-line titles) over each other: Arrange
 * offered once in the status line (nothing moves unasked). */
void arrange_offer_check(struct folder *f);
void folder_scroll_restore(struct folder *f);
void launchpad_open_folder(const char *path);
void desk_backdrop(void *ctx, cairo_t *cr, ui_rect clip);
void desk_wallpaper_changed(void *ctx);
void user_dir(const char *key, const char *fallback, char *out, size_t n);
void windows_at_login(void);
struct folder *open_desktop(const char *path);
struct folder *open_window(const char *path, ui_view view, bool desktop);
int main(int argc, char **argv);

#endif
