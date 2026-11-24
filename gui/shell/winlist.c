/* The Window List and the Minimized Window Viewer (winlist.h). */
#define _GNU_SOURCE
#include "winlist.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "events.h"

void workarea_window(const char *title, bool minimized);   /* (workarea.c) */

#define STATUS_H (20 + ui_dialog_grow())

struct win {
    char id[32];
    char app[128];
    char title[256];
    bool minimized, focused, maximized;
    int x, y, w, h;            /* where (the WPS session's compositor says; else -1) */
    char icon_path[256];       /* the app's icon, a PNG the extension made */
    cairo_surface_t *icon;
};

static ui_app *app;
static int cmd_fd = -1;
static struct win *wins;
static size_t nwins, capwins;

/* One of the two list windows. */
struct listwin {
    bool minimized_only;
    ui_surface *s;
    ui_container *cont;
    size_t *rows;            /* indexes into wins, for what it shows */
    size_t nrows;
    bool closing;
};
static struct listwin lists[2];   /* [0] the Window List, [1] the Minimized Window Viewer */

static void refresh(struct listwin *l);

/* ---------------------------------------------------------------- model */

static struct win *find(const char *id)
{
    for (size_t i = 0; i < nwins; i++) if (!strcmp(wins[i].id, id)) return &wins[i];
    return NULL;
}

void winlist_line(char **f, int n)
{
    if (n < 7 || strcmp(f[0], "W")) return;
    struct win *w = find(f[2]);
    if (!strcmp(f[1], "remove")) {
        if (w) {
            if (w->icon) cairo_surface_destroy(w->icon);
            size_t i = (size_t)(w - wins);
            memmove(w, w + 1, (nwins - i - 1) * sizeof *w);
            nwins--;
        }
    } else {
        if (!w) {
            if (nwins == capwins) {
                capwins = capwins ? capwins * 2 : 32;
                wins = realloc(wins, capwins * sizeof *wins);
            }
            w = &wins[nwins++];
            memset(w, 0, sizeof *w);
            snprintf(w->id, sizeof w->id, "%s", f[2]);
        }
        snprintf(w->app, sizeof w->app, "%s", f[3]);
        snprintf(w->title, sizeof w->title, "%s", f[4][0] ? f[4] : f[3]);
        w->minimized = !strcmp(f[5], "1");
        w->focused = !strcmp(f[6], "1");
        w->maximized = n > 8 && !strcmp(f[8], "1");
        w->x = n > 12 ? atoi(f[9]) : -1;
        w->y = n > 12 ? atoi(f[10]) : -1;
        w->w = n > 12 ? atoi(f[11]) : -1;
        w->h = n > 12 ? atoi(f[12]) : -1;
        const char *ip = n > 7 ? f[7] : "";
        if (strcmp(ip, w->icon_path)) {
            if (w->icon) cairo_surface_destroy(w->icon);
            w->icon = NULL;
            snprintf(w->icon_path, sizeof w->icon_path, "%s", ip);
            if (*ip) {
                w->icon = cairo_image_surface_create_from_png(ip);
                if (cairo_surface_status(w->icon) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(w->icon); w->icon = NULL; }
            }
        }
    }
    for (int k = 0; k < 2; k++) if (lists[k].s && !lists[k].closing) refresh(&lists[k]);
    if (w && strcmp(f[1], "remove")) workarea_window(w->title, w->minimized);   /* a work area's window: its group follows */
}

static void command(const char *cmd, const char *id)
{
    if (cmd_fd < 0) return;
    /* One line on a pipe kept open (not ui_app_write_async, which closes
     * its pipe after one transfer); a pipe the extension does not read any
     * more drops it rather than block. Lines can be long (a scheme's
     * definition): written whole (a short buffer once wrote past its end). */
    char *line = NULL;
    int n = asprintf(&line, "%s\t%s\n", cmd, id);
    if (n < 0) return;
    if (write(cmd_fd, line, (size_t)n) != n) fprintf(stderr, "wps-shell: the window command was not sent\n");
    free(line);
}

bool winlist_ext(const char *cmd, const char *arg)
{
    if (cmd_fd < 0) return false;
    command(cmd, arg ? arg : "");
    return true;
}

size_t winlist_count(bool minimized_only)
{
    size_t n = 0;
    for (size_t i = 0; i < nwins; i++) n += !minimized_only || wins[i].minimized;
    return n;
}

bool winlist_command(const char *cmd, const char *title)
{
    for (size_t i = 0; i < nwins; i++)
        if (strstr(wins[i].title, title)) { command(cmd, wins[i].id); return true; }
    return false;
}

bool winlist_state(const char *title, bool *minimized, bool *maximized)
{
    for (size_t i = 0; i < nwins; i++)
        if (strstr(wins[i].title, title)) { *minimized = wins[i].minimized; *maximized = wins[i].maximized; return true; }
    return false;
}

bool winlist_where(const char *app, const char *title, int *x, int *y, bool *maximized)
{
    for (size_t i = 0; i < nwins; i++)
        if (!strcmp(wins[i].app, app) && !strcmp(wins[i].title, title) && wins[i].x >= -100000 && wins[i].w > 0) {
            *x = wins[i].x;
            *y = wins[i].y;
            *maximized = wins[i].maximized;
            return true;
        }
    return false;
}

bool winlist_is_open(bool minimized_only) { return lists[minimized_only].s && !lists[minimized_only].closing; }

const char *winlist_title(size_t i, bool minimized_only)
{
    struct listwin *l = &lists[minimized_only];
    return l->s && i < l->nrows ? wins[l->rows[i]].title : NULL;
}

void winlist_init(ui_app *a, int fd)
{
    app = a;
    cmd_fd = fd;
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* -------------------------------------------------------------- windows */

static void refresh(struct listwin *l)
{
    free(l->rows);
    l->rows = calloc(nwins ? nwins : 1, sizeof *l->rows);
    l->nrows = 0;
    for (size_t i = 0; i < nwins; i++)
        if (!l->minimized_only || wins[i].minimized) l->rows[l->nrows++] = i;
    ui_container_reset(l->cont);
    ui_container_layout_changed(l->cont);
    ui_surface_damage_all(l->s);
}

static size_t src_count(void *ctx) { return ((struct listwin *)ctx)->nrows; }

static void src_item(void *ctx, size_t i, ui_item *out)
{
    struct listwin *l = ctx;
    memset(out, 0, sizeof *out);
    if (i >= l->nrows) return;
    const struct win *w = &wins[l->rows[i]];
    out->loaded = out->detailed = true;
    /* the app's own icon; WPS's folder windows as folders */
    out->kind = !strcmp(w->app, "org.wps.shell") || !strcmp(w->app, "org.wps.MinimizedViewer") ? UI_KIND_FOLDER : UI_KIND_PROGRAM;
    out->image = w->icon;
    out->name = w->app;
    out->title = w->title;
    out->in_use = w->focused;   /* the active window, emphasized */
    out->shadow = w->minimized && !l->minimized_only;   /* minimized: drawn dimmer in the full list */
    out->last_child = i + 1 == l->nrows;
}

/* Double-click: show it (restored, raised, focused), as OS/2's list did. */
static void src_activate(void *ctx, size_t i)
{
    struct listwin *l = ctx;
    if (i < l->nrows) {
        command("activate", wins[l->rows[i]].id);
        events_raise("window.restore", wins[l->rows[i]].app, "window", (ui_rect){ 0, 0, 0, 0 });
    }
}

struct menu_ctx { struct listwin *l; char id[32]; };

static void menu_chosen(void *arg, int64_t item)
{
    struct menu_ctx *m = arg;
    static const char *const cmds[] = { NULL, "activate", "restore", "minimize", "maximize", "close", "terminate" };
    if (item >= 1 && item <= 6) command(cmds[item], m->id);
    free(m);
}

/* Its menu: Show (forward), Restore, Minimize, Maximize, Close (asks the
 * program), Terminate (ends it, unasked: for one that does not answer). */
static void src_menu(void *ctx, size_t i, double x, double y, uint32_t serial)
{
    struct listwin *l = ctx;
    if (i >= l->nrows) return;
    struct menu_ctx *m = calloc(1, sizeof *m);
    m->l = l;
    snprintf(m->id, sizeof m->id, "%s", wins[l->rows[i]].id);
    const struct win *w = &wins[l->rows[i]];
    ui_menu_item items[] = {
        { 1, "Show", true, false, NULL, 0 },
        { 0, "-", false, false, NULL, 0 },
        { 2, "Restore", w->minimized || w->maximized, false, NULL, 0 },
        { 3, "Minimize", !w->minimized, false, NULL, 0 },
        { 4, "Maximize", !w->maximized || w->minimized, false, NULL, 0 },
        { 0, "-", false, false, NULL, 0 },
        { 5, "Close", true, false, NULL, 0 },
        { 6, "Terminate", true, false, NULL, 0 },
    };
    ui_menu_popup(l->s, items, sizeof items / sizeof *items, x, y, serial, menu_chosen, m);
}

static const ui_container_src list_src = { src_count, src_item, NULL, src_activate, src_menu, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static void lw_layout(struct listwin *l)
{
    ui_rect c = ui_frame_client(l->s);
    c.h -= STATUS_H;
    ui_container_layout(l->cont, c);
}

static void lw_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    struct listwin *l = ui_surface_data(s);
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(s, cr, clip, l->minimized_only ? "Minimized Window Viewer" : "Window List");
    ui_container_draw(l->cont, cr, clip);
    ui_rect c = ui_frame_client(s);
    ui_rect st = { c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    if (!ui_rect_meets(clip, st)) return;
    ui_fill(cr, st, k->face);
    ui_bevel(cr, (ui_rect){ st.x + 1, st.y + 1, st.w - 2, st.h - 2 }, false);
    char msg[160];
    if (cmd_fd < 0) snprintf(msg, sizeof msg, "Needs the WPS GNOME Shell extension (tools/install-extension.sh)");
    else snprintf(msg, sizeof msg, "%zu %swindow%s", l->nrows, l->minimized_only ? "minimized " : "", l->nrows == 1 ? "" : "s");
    ui_set_color(cr, k->text);
    cairo_save(cr);
    cairo_rectangle(cr, st.x + 2, st.y + 2, st.w - 4, st.h - 4);
    cairo_clip(cr);
    ui_text_draw(cr, UI_FONT_DIALOG, st.x + 6, st.y + (STATUS_H + ui_font_ascent(UI_FONT_DIALOG)) / 2 - 1, msg, st.w - 12);
    cairo_restore(cr);
}

static void lw_pointer(ui_surface *s, const ui_pointer_event *e)
{
    struct listwin *l = ui_surface_data(s);
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    bool used = false;
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) used = ui_frame_pointer(s, e);
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) ui_container_pointer(l->cont, e);
}

static void lw_key(ui_surface *s, const ui_key_event *e)
{
    struct listwin *l = ui_surface_data(s);
    if (e->sym == XKB_KEY_Escape) { ui_window_minimize(s); return; }   /* out of the way, as Ctrl+Esc's list */
    ui_container_key(l->cont, e);
}

static void lw_free(void *arg)
{
    struct listwin *l = arg;
    ui_container_free(l->cont);
    ui_surface_destroy(l->s);
    free(l->rows);
    memset(l, 0, sizeof *l);
}

static void lw_close(ui_surface *s)
{
    struct listwin *l = ui_surface_data(s);
    if (l->closing) return;
    l->closing = true;
    events_raise("window.close", "", "window", (ui_rect){ 0, 0, 0, 0 });
    ui_app_post(app, lw_free, l);   /* not inside its own event */
}

static void lw_resized(ui_surface *s, double w, double h)
{
    (void)w; (void)h;
    lw_layout(ui_surface_data(s));
    ui_surface_damage_all(s);
}

static void lw_focus(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }

static const ui_surface_ops lw_ops = { .draw = lw_draw, .pointer = lw_pointer, .key = lw_key, .resized = lw_resized,
                                       .close = lw_close, .focus = lw_focus,
                                       .drag_motion = ui_font_drag_motion, .drop = ui_font_drop };   /* a font: the Dialogs font */

void winlist_open(bool minimized_only)
{
    struct listwin *l = &lists[minimized_only];
    if (l->s && !l->closing) {
        /* already open: forward (a minimized list window, restored through GNOME) */
        ui_window_activate(l->s);
        return;
    }
    memset(l, 0, sizeof *l);
    l->minimized_only = minimized_only;
    /* The Window List's app ID keeps it out of the list it shows (the
     * compositor and the extension skip it); the viewer is an ordinary
     * window there, as in OS/2. */
    l->s = ui_window_new(app, &lw_ops, l, minimized_only ? "org.wps.MinimizedViewer" : "org.wps.WindowList", minimized_only ? 520 : 420, minimized_only ? 300 : 360);
    ui_window_set_title(l->s, minimized_only ? "Minimized Window Viewer" : "Window List");
    l->cont = ui_container_new(l->s, &list_src, l, minimized_only ? UI_VIEW_ICON : UI_VIEW_TREE);
    ui_container_set_font(l->cont, UI_FONT_DIALOG);   /* (a dialog's list) */
    lw_layout(l);
    refresh(l);
    events_raise("window.open", "", "window", (ui_rect){ 0, 0, 0, 0 });
}

bool winlist_snapshot(bool minimized_only, const char *png)
{
    struct listwin *l = &lists[minimized_only];
    return l->s && !l->closing && ui_surface_snapshot(l->s, png);
}
