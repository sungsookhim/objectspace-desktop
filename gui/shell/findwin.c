/* Find (findwin.h). */
#define _GNU_SOURCE
#include "findwin.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "events.h"
#include "svc.h"

#define M 8                /* margin */
#define ROW 24             /* a field's height */
#define GAP 6
#define LABEL_W 60
#define BTN_W 84
#define TYPE_W 120
#define STATUS_H 20
#define LIMIT 2000         /* results at most */

enum { F_NAME, F_FOLDER, F_SUB, F_TYPE, F_FIND, F_RESULTS, F_COUNT };

static const char *const kinds[] = { "any", "folder", "file", "program" };
static const char *const kind_labels[] = { "Any", "Folders", "Files", "Programs" };

struct res {
    char *path;
    const char *base;      /* in path */
    char *dir;             /* where it is, shown (~ for the home folder) */
    ui_kind kind;
    int64_t size, mtime;
};

struct fw {
    struct fw *next;
    int wid;               /* for replies that come after it closed */
    ui_surface *s;
    ui_container *cont;
    ui_entry *name, *folder;
    bool sub;
    int kind;              /* index into kinds */
    int focus;
    bool find_down;        /* the Find button, pressed */
    int dragging;          /* an entry being selected with the pointer: F_NAME, F_FOLDER, or -1 */
    struct res *res;
    size_t nres, capres;
    int64_t find;          /* the running search (0: none) */
    unsigned token;        /* the latest search asked for */
    bool searching, started, truncated;
    uint64_t scanned;
    char error[160];
    bool closing;
};

static ui_app *app;
static void (*open_folder)(const char *path);
static void (*drag_paths)(ui_surface *win, uint32_t serial, const char *const *paths, size_t n, ui_kind kind);
static struct fw *wins, *last;
static int next_wid = 1;

/* ---------------------------------------------------------------- model */

static struct fw *by_wid(int wid)
{
    for (struct fw *w = wins; w; w = w->next)
        if (w->wid == wid && !w->closing) return w;
    return NULL;
}

static void clear(struct fw *w)
{
    for (size_t i = 0; i < w->nres; i++) { free(w->res[i].path); free(w->res[i].dir); }
    w->nres = 0;
    w->truncated = false;
    w->scanned = 0;
    w->error[0] = 0;
}

static void cancel(struct fw *w)
{
    if (w->find) svc_find_cancel(w->find);
    w->find = 0;
    w->searching = false;
}

static void damage_status(struct fw *w)
{
    ui_rect c = ui_frame_client(w->s);
    ui_surface_damage(w->s, (ui_rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H });
}

static void expand_folder(const char *in, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && home) snprintf(out, n, "%s%s", home, in + 1);
    else snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}

struct req { int wid; unsigned token; };

static void got_find(void *arg, const cv *reply)
{
    struct req *r = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    int64_t id = 0;
    bool ok = res && cv_int(res, "find", &id);
    struct fw *w = by_wid(r->wid);
    if (w && r->token == w->token) {
        if (ok) w->find = id;
        else {
            w->searching = false;
            snprintf(w->error, sizeof w->error, "%s", reply ? (cv_text(reply, "message") ? cv_text(reply, "message") : "failed")
                                                             : "no connection to wpsd");
            damage_status(w);
        }
    } else if (ok) {
        svc_find_cancel(id);   /* a search nobody waits for any more */
    }
    free(r);
}

/* Start the search again with what the fields say. live: as the name is
 * typed (an empty name then finds nothing rather than everything). */
static void search(struct fw *w, bool live)
{
    cancel(w);
    clear(w);
    w->token++;
    const char *name = ui_entry_text(w->name);
    char folder[PATH_MAX];
    expand_folder(ui_entry_text(w->folder), folder, sizeof folder);
    w->started = !(live && !*name);
    ui_container_reset(w->cont);
    ui_surface_damage_all(w->s);
    if (!w->started) return;
    if (folder[0] != '/') {
        snprintf(w->error, sizeof w->error, "The folder must be a full path (/... or ~/...)");
        return;
    }
    w->searching = true;
    struct req *r = calloc(1, sizeof *r);
    r->wid = w->wid;
    r->token = w->token;
    events_raise("find.start", name, "find", (ui_rect){ 0, 0, 0, 0 });
    svc_find(folder, name, w->sub, kinds[w->kind], LIMIT, got_find, r);
}

bool findwin_event(const cv *ev)
{
    const char *name = cv_text(ev, "event");
    int64_t sub;
    if (!name || strcmp(name, "found") || !cv_int(ev, "sub", &sub)) return false;
    struct fw *w = wins;
    while (w && (w->closing || w->find != sub)) w = w->next;
    if (!w) return true;   /* a search cancelled since */
    const cv *items = cv_get(ev, "items");
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    size_t first = w->nres;
    for (size_t i = 0; items && items->k == CV_ARRAY && i < items->n; i++) {
        const cv *it = &items->items[i];
        const char *p = cv_text(it, "path"), *t = cv_text(it, "type");
        if (!p || !t) continue;
        if (w->nres == w->capres) {
            w->capres = w->capres ? w->capres * 2 : 64;
            w->res = realloc(w->res, w->capres * sizeof *w->res);
        }
        struct res *r = &w->res[w->nres++];
        memset(r, 0, sizeof *r);
        r->path = strdup(p);
        char *sl = strrchr(r->path, '/');
        r->base = sl ? sl + 1 : r->path;
        size_t dl = sl ? (size_t)(sl - r->path) : 0;
        if (home && hl > 1 && dl >= hl && !strncmp(r->path, home, hl) && (dl == hl || r->path[hl] == '/'))
            (void)asprintf(&r->dir, "~%.*s", (int)(dl - hl), r->path + hl);
        else
            (void)asprintf(&r->dir, "%.*s", (int)(dl ? dl : 1), dl ? r->path : "/");
        size_t bl = strlen(r->base);
        r->kind = !strcmp(t, "folder") ? UI_KIND_FOLDER : !strcmp(t, "link") ? UI_KIND_LINK :
                  w->kind == 3 || (bl > 8 && !strcmp(r->base + bl - 8, ".desktop")) ? UI_KIND_PROGRAM : UI_KIND_FILE;
        cv_int(it, "size", &r->size);
        cv_int(it, "mtime", &r->mtime);
    }
    int64_t sc;
    if (cv_int(ev, "scanned", &sc)) w->scanned = (uint64_t)sc;
    const cv *done = cv_get(ev, "done"), *tr = cv_get(ev, "truncated");
    if (done && done->k == CV_BOOL && done->b) {
        w->searching = false;
        w->find = 0;
        w->truncated = tr && tr->k == CV_BOOL && tr->b;
        const char *err = cv_text(ev, "error");
        if (err) snprintf(w->error, sizeof w->error, "Cannot look in that folder: %s", err);
        events_raise("find.done", ui_entry_text(w->name), "find", (ui_rect){ 0, 0, 0, 0 });
    }
    if (w->nres != first) ui_container_count_changed(w->cont);
    damage_status(w);
    return true;
}

/* ----------------------------------------------------------- the results */

static size_t src_count(void *ctx) { return ((struct fw *)ctx)->nres; }

static void src_item(void *ctx, size_t i, ui_item *out)
{
    struct fw *w = ctx;
    memset(out, 0, sizeof *out);
    if (i >= w->nres) return;
    const struct res *r = &w->res[i];
    out->loaded = out->detailed = true;
    out->kind = r->kind;
    out->title = r->base;
    out->name = r->dir;
    out->size = r->size;
    out->mtime = r->mtime;
    out->last_child = i + 1 == w->nres;
}

static void opened(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && cv_text(res, "folder")) { open_folder(cv_text(res, "folder")); return; }
    const char *msg = reply ? cv_text(reply, "message") : "lost the connection to wpsd";
    if (!res && msg) fprintf(stderr, "wps-shell: Find: %s\n", msg);
}

static void open_result(struct fw *w, size_t i)
{
    if (i >= w->nres) return;
    const struct res *r = &w->res[i];
    events_raise("object.open", r->base, "find", (ui_rect){ 0, 0, 0, 0 });
    if (r->kind == UI_KIND_FOLDER) open_folder(r->path);
    else svc_open(r->path, false, opened, NULL);
}

static void open_where(struct fw *w, size_t i)
{
    if (i >= w->nres) return;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s", (int)(w->res[i].base - 1 - w->res[i].path), w->res[i].path);
    open_folder(dir[0] ? dir : "/");
}

static void src_activate(void *ctx, size_t i) { open_result(ctx, i); }

struct menu_ctx { int wid; char path[PATH_MAX]; };

static void menu_chosen(void *arg, int64_t item)
{
    struct menu_ctx *m = arg;
    struct fw *w = by_wid(m->wid);
    for (size_t i = 0; w && i < w->nres; i++) {
        if (strcmp(w->res[i].path, m->path)) continue;
        if (item == 1) open_result(w, i);
        else if (item == 2) open_where(w, i);
        break;
    }
    free(m);
}

static void src_menu(void *ctx, size_t i, double x, double y, uint32_t serial)
{
    struct fw *w = ctx;
    if (i >= w->nres) return;
    struct menu_ctx *m = calloc(1, sizeof *m);
    m->wid = w->wid;
    snprintf(m->path, sizeof m->path, "%s", w->res[i].path);
    ui_menu_item items[] = {
        { 1, "Open", true, false, NULL, 0 },
        { 2, "Open folder it is in", true, false, NULL, 0 },
    };
    ui_menu_popup(w->s, items, 2, x, y, serial, menu_chosen, m);
}

/* Results are the objects themselves: dragged (the selection if the item
 * is in it, else the item alone) to be moved, copied or shadowed, as from
 * a folder. */
static void src_drag(void *ctx, size_t i, uint32_t serial)
{
    struct fw *w = ctx;
    if (!drag_paths || i >= w->nres) return;
    bool whole = ui_container_is_selected(w->cont, i);
    const char **p = calloc(w->nres, sizeof *p);
    size_t n = 0;
    for (size_t k = whole ? 0 : i; k < (whole ? w->nres : i + 1); k++)
        if (!whole || ui_container_is_selected(w->cont, k)) p[n++] = w->res[k].path;
    drag_paths(w->s, serial, p, n, w->res[i].kind);
    free(p);
}

static const ui_container_src res_src = { src_count, src_item, NULL, src_activate, src_menu, NULL, NULL, src_drag, NULL, NULL, NULL };

/* --------------------------------------------------------------- layout */

struct rects { ui_rect name_l, name, find, folder_l, folder, sub, type_l, type, results, status; };

static struct rects rects(struct fw *w)
{
    ui_rect c = ui_frame_client(w->s);
    struct rects r;
    double x0 = c.x + M, fx = x0 + LABEL_W, fw = c.w - 2 * M - LABEL_W - BTN_W - M;
    double y = c.y + M;
    r.name_l = (ui_rect){ x0, y, LABEL_W, ROW };
    r.name = (ui_rect){ fx, y, fw, ROW };
    r.find = (ui_rect){ fx + fw + M, y, BTN_W, ROW };
    y += ROW + GAP;
    r.folder_l = (ui_rect){ x0, y, LABEL_W, ROW };
    r.folder = (ui_rect){ fx, y, fw, ROW };
    y += ROW + GAP;
    r.sub = (ui_rect){ fx - 4, y, 190, ROW };
    r.type_l = (ui_rect){ fx + 200, y, 44, ROW };
    r.type = (ui_rect){ fx + 244, y, TYPE_W, ROW };
    y += ROW + GAP + 2;
    r.status = (ui_rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    r.results = (ui_rect){ c.x, y, c.w, r.status.y - y };
    return r;
}

static void layout(struct fw *w) { ui_container_layout(w->cont, rects(w).results); }

/* --------------------------------------------------------------- drawing */

static void focus_ring(cairo_t *cr, ui_rect r)
{
    ui_set_color(cr, ui_scheme_now.cursor);
    double dash = 1;
    cairo_save(cr);
    cairo_set_dash(cr, &dash, 1, 0);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void label(cairo_t *cr, ui_rect r, const char *s)
{
    ui_set_color(cr, ui_scheme_now.text);
    ui_text_draw(cr, UI_FONT_REGULAR, r.x, r.y + (r.h + ui_text_ascent()) / 2 - 1, s, r.w);
}

static void button(cairo_t *cr, ui_rect b, const char *s, bool down, bool focused, bool arrow)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_fill(cr, b, k->face);
    ui_bevel(cr, b, !down);
    double tw = ui_text_width(UI_FONT_REGULAR, s), d = down ? 1 : 0;
    ui_set_color(cr, k->text);
    double tx = arrow ? b.x + 8 : b.x + (b.w - tw) / 2;
    ui_text_draw(cr, UI_FONT_REGULAR, tx + d, b.y + (b.h + ui_text_ascent()) / 2 - 1 + d, s, b.w - 20);
    if (arrow) {
        double cx = b.x + b.w - 10, cy = b.y + b.h / 2;
        cairo_move_to(cr, cx - 4, cy - 2);
        cairo_line_to(cr, cx + 4, cy - 2);
        cairo_line_to(cr, cx, cy + 3);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    if (focused) focus_ring(cr, (ui_rect){ b.x + 3, b.y + 3, b.w - 6, b.h - 6 });
}

static void check(cairo_t *cr, ui_rect r, const char *s, bool on, bool focused)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect b = { r.x + 4, r.y + (r.h - 14) / 2, 14, 14 };
    ui_fill(cr, b, k->client_bg);
    ui_bevel(cr, b, false);
    if (on) {
        ui_set_color(cr, k->text);
        cairo_set_line_width(cr, 2);
        cairo_move_to(cr, b.x + 3, b.y + b.h / 2);
        cairo_line_to(cr, b.x + b.w / 2 - 1, b.y + b.h - 4);
        cairo_line_to(cr, b.x + b.w - 3, b.y + 3);
        cairo_stroke(cr);
    }
    ui_rect t = { b.x + b.w + 6, r.y, ui_text_width(UI_FONT_REGULAR, s) + 4, r.h };
    label(cr, (ui_rect){ t.x + 2, t.y, t.w, t.h }, s);
    if (focused) focus_ring(cr, (ui_rect){ t.x, t.y + 3, t.w, t.h - 6 });
}

static void fw_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    struct fw *w = ui_surface_data(s);
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(s, cr, clip, "Find");
    struct rects r = rects(w);
    ui_rect c = ui_frame_client(s);
    ui_rect top = { c.x, c.y, c.w, r.results.y - c.y };
    if (ui_rect_meets(clip, top)) {
        ui_fill(cr, top, k->face);
        label(cr, r.name_l, "Name:");
        ui_entry_draw(w->name, cr, r.name, w->focus == F_NAME);
        button(cr, r.find, "Find", w->find_down, w->focus == F_FIND, false);
        label(cr, r.folder_l, "Folder:");
        ui_entry_draw(w->folder, cr, r.folder, w->focus == F_FOLDER);
        check(cr, r.sub, "Include subfolders", w->sub, w->focus == F_SUB);
        label(cr, r.type_l, "Type:");
        button(cr, r.type, kind_labels[w->kind], false, w->focus == F_TYPE, true);
    }
    ui_container_draw(w->cont, cr, clip);
    ui_rect st = r.status;
    if (!ui_rect_meets(clip, st)) return;
    ui_fill(cr, st, k->face);
    ui_bevel(cr, (ui_rect){ st.x + 1, st.y + 1, st.w - 2, st.h - 2 }, false);
    char msg[256];
    if (w->error[0]) snprintf(msg, sizeof msg, "%s", w->error);
    else if (!w->started) snprintf(msg, sizeof msg, "Type a name, or a pattern such as *.txt");
    else if (w->searching) snprintf(msg, sizeof msg, "Searching... %zu found", w->nres);
    else if (w->truncated) snprintf(msg, sizeof msg, "%zu found; stopped there (the first %d)", w->nres, LIMIT);
    else snprintf(msg, sizeof msg, "%zu found (%llu looked at)", w->nres, (unsigned long long)w->scanned);
    ui_set_color(cr, k->text);
    cairo_save(cr);
    cairo_rectangle(cr, st.x + 2, st.y + 2, st.w - 4, st.h - 4);
    cairo_clip(cr);
    ui_text_draw(cr, UI_FONT_REGULAR, st.x + 6, st.y + (STATUS_H + ui_text_ascent()) / 2 - 1, msg, st.w - 12);
    cairo_restore(cr);
}

/* ---------------------------------------------------------------- input */

static void damage_top(struct fw *w)
{
    struct rects r = rects(w);
    ui_rect c = ui_frame_client(w->s);
    ui_surface_damage(w->s, (ui_rect){ c.x, c.y, c.w, r.results.y - c.y });
}

/* For screen readers: the field with the keyboard. */
static void a11y_field(struct fw *w)
{
    struct rects r = rects(w);
    ui_a11y_info i = { .index = w->focus, .count = F_COUNT };
    char found[64];
    switch (w->focus) {
    case F_NAME: i.role = UI_ROLE_ENTRY; i.name = "Name"; i.text = ui_entry_text(w->name); i.at = r.name;
        i.description = "a part of the name, or a pattern such as *.txt"; break;
    case F_FOLDER: i.role = UI_ROLE_ENTRY; i.name = "Folder"; i.text = ui_entry_text(w->folder); i.at = r.folder; break;
    case F_SUB: i.role = UI_ROLE_CHECK_BOX; i.name = "Include subfolders"; i.checked = w->sub; i.at = r.sub; break;
    case F_TYPE: i.role = UI_ROLE_COMBO_BOX; i.name = "Type"; i.description = kind_labels[w->kind]; i.at = r.type; break;
    case F_FIND: i.role = UI_ROLE_BUTTON; i.name = "Find"; i.at = r.find; break;
    default:
        if (ui_container_cursor(w->cont) != SIZE_MAX) return;   /* the container says which result */
        snprintf(found, sizeof found, "%zu found", w->nres);
        i.role = UI_ROLE_LIST_ITEM; i.name = "Results"; i.description = found; i.at = r.results;
        i.index = 0; i.count = 0;
    }
    ui_a11y_focus(w->s, &i);
}

static void set_focus(struct fw *w, int f)
{
    w->focus = f;
    ui_container_set_focused(w->cont, f == F_RESULTS);
    /* an input method for the two text fields */
    struct rects r = rects(w);
    ui_text_input_focus(w->s, f == F_NAME ? w->name : f == F_FOLDER ? w->folder : NULL, f == F_NAME ? r.name : r.folder);
    damage_top(w);
    a11y_field(w);
}

static void kind_chosen(void *arg, int64_t item)
{
    struct fw *w = by_wid((int)(intptr_t)arg);
    if (!w || item < 0 || item > 3) return;
    w->kind = (int)item;
    damage_top(w);
    a11y_field(w);
    if (*ui_entry_text(w->name)) search(w, false);
}

static void kind_menu(struct fw *w, uint32_t serial)
{
    ui_menu_item items[4];
    for (int i = 0; i < 4; i++) items[i] = (ui_menu_item){ i, (char *)kind_labels[i], true, i == w->kind, NULL, 0 };
    ui_rect t = rects(w).type;
    ui_menu_popup(w->s, items, 4, t.x, t.y + t.h, serial, kind_chosen, (void *)(intptr_t)w->wid);
}

static void toggle_sub(struct fw *w)
{
    w->sub = !w->sub;
    damage_top(w);
    a11y_field(w);
    if (*ui_entry_text(w->name)) search(w, false);
}

static void fw_pointer(ui_surface *s, const ui_pointer_event *e)
{
    struct fw *w = ui_surface_data(s);
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    bool used = false;
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) used = ui_frame_pointer(s, e);
    struct rects r = rects(w);
    if (e->kind == UI_PRESS && !used && h == UI_HIT_CLIENT) {
        if (ui_rect_has(r.results, e->x, e->y)) {
            if (w->focus != F_RESULTS) set_focus(w, F_RESULTS);
            ui_container_pointer(w->cont, e);
            return;
        }
        if (e->button != UI_BTN1) return;
        ui_entry *en = NULL;
        int f = -1;
        if (ui_rect_has(r.name, e->x, e->y)) { en = w->name; f = F_NAME; }
        else if (ui_rect_has(r.folder, e->x, e->y)) { en = w->folder; f = F_FOLDER; }
        if (en) {
            set_focus(w, f);
            w->dragging = f;
            ui_entry_pointer(en, e, f == F_NAME ? r.name : r.folder);
            damage_top(w);
        } else if (ui_rect_has(r.sub, e->x, e->y)) {
            set_focus(w, F_SUB);
            toggle_sub(w);
        } else if (ui_rect_has(r.type, e->x, e->y)) {
            set_focus(w, F_TYPE);
            kind_menu(w, e->serial);
        } else if (ui_rect_has(r.find, e->x, e->y)) {
            set_focus(w, F_FIND);
            w->find_down = true;
            damage_top(w);
        }
        return;
    }
    if ((e->kind == UI_MOTION || e->kind == UI_RELEASE) && w->dragging >= 0) {
        bool nm = w->dragging == F_NAME;
        if (ui_entry_pointer(nm ? w->name : w->folder, e, nm ? r.name : r.folder) != UI_ENTRY_NONE) damage_top(w);
        if (e->kind == UI_RELEASE) w->dragging = -1;
    }
    if (e->kind == UI_RELEASE && w->find_down) {
        w->find_down = false;
        damage_top(w);
        if (ui_rect_has(r.find, e->x, e->y)) search(w, false);
    }
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) ui_container_pointer(w->cont, e);
}

static void fw_close(ui_surface *s);

static void fw_key(ui_surface *s, const ui_key_event *e)
{
    struct fw *w = ui_surface_data(s);
    if (e->sym == XKB_KEY_Tab || e->sym == XKB_KEY_ISO_Left_Tab) {
        bool back = e->sym == XKB_KEY_ISO_Left_Tab || (e->mods & UI_MOD_SHIFT);
        set_focus(w, (w->focus + (back ? F_COUNT - 1 : 1)) % F_COUNT);
        return;
    }
    if (e->sym == XKB_KEY_Escape) {
        /* a search going on: stop it; otherwise close, as OS/2's dialogs */
        if (w->searching) { cancel(w); damage_status(w); }
        else fw_close(s);
        return;
    }
    bool enter = e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter;
    switch (w->focus) {
    case F_NAME: case F_FOLDER: {
        ui_entry_result r = ui_entry_key(w->focus == F_NAME ? w->name : w->folder, e);
        if (r != UI_ENTRY_NONE) { damage_top(w); a11y_field(w); }
        if (r == UI_ENTRY_COMMIT) search(w, false);
        else if (r == UI_ENTRY_CHANGED && w->focus == F_NAME) search(w, true);   /* as you type */
        break;
    }
    case F_SUB:
        if (e->sym == XKB_KEY_space) toggle_sub(w);
        else if (enter) search(w, false);
        break;
    case F_TYPE:
        if (e->sym == XKB_KEY_space || e->sym == XKB_KEY_Down) {
            w->kind = (w->kind + 1) % 4;
            damage_top(w);
            a11y_field(w);
            if (*ui_entry_text(w->name)) search(w, false);
        } else if (enter) {
            search(w, false);
        }
        break;
    case F_FIND:
        if (enter || e->sym == XKB_KEY_space) search(w, false);
        break;
    default:
        ui_container_key(w->cont, e);
    }
}

static void fw_free(void *arg)
{
    struct fw *w = arg;
    for (struct fw **p = &wins; *p; p = &(*p)->next)
        if (*p == w) { *p = w->next; break; }
    if (last == w) last = NULL;
    clear(w);
    free(w->res);
    ui_container_free(w->cont);
    ui_entry_free(w->name);
    ui_entry_free(w->folder);
    ui_surface_destroy(w->s);
    free(w);
}

static void fw_close(ui_surface *s)
{
    struct fw *w = ui_surface_data(s);
    if (w->closing) return;
    cancel(w);
    w->closing = true;
    events_raise("window.close", "Find", "window", (ui_rect){ 0, 0, 0, 0 });
    ui_app_post(app, fw_free, w);
}

static void fw_resized(ui_surface *s, double width, double height)
{
    (void)width; (void)height;
    layout(ui_surface_data(s));
    ui_surface_damage_all(s);
}

static void fw_focus(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }

static const ui_surface_ops fw_ops = { .draw = fw_draw, .pointer = fw_pointer, .key = fw_key, .resized = fw_resized,
                                       .close = fw_close, .focus = fw_focus };

void findwin_init(ui_app *a, void (*of)(const char *path),
                  void (*dr)(ui_surface *win, uint32_t serial, const char *const *paths, size_t n, ui_kind kind))
{
    app = a;
    open_folder = of;
    drag_paths = dr;
}

void findwin_open(const char *folder, const char *name)
{
    struct fw *w = calloc(1, sizeof *w);
    w->wid = next_wid++;
    w->dragging = -1;
    w->sub = true;   /* OS/2's Find looked in subfolders by default */
    const char *home = getenv("HOME");
    char shown[PATH_MAX];
    size_t hl = home ? strlen(home) : 0;
    if (home && hl > 1 && !strncmp(folder, home, hl) && (!folder[hl] || folder[hl] == '/'))
        snprintf(shown, sizeof shown, "~%s", folder + hl);
    else
        snprintf(shown, sizeof shown, "%s", folder);
    w->name = ui_entry_new(name ? name : "");
    w->folder = ui_entry_new(shown);
    w->next = wins;
    wins = last = w;
    w->s = ui_window_new(app, &fw_ops, w, "org.wps.Find", 620, 440);
    ui_window_set_title(w->s, "Find");
    w->cont = ui_container_new(w->s, &res_src, w, UI_VIEW_DETAILS);
    ui_container_set_name_head(w->cont, "Folder");
    set_focus(w, F_NAME);
    layout(w);
    events_raise("window.open", "Find", "window", (ui_rect){ 0, 0, 0, 0 });
    if (name && *name) search(w, false);
}

void findwin_show(const char *folder, bool fresh)
{
    struct fw *w = last ? last : wins;   /* the latest opened, else the latest still open */
    if (!fresh && w) { ui_window_activate(w->s); return; }
    findwin_open(folder, NULL);
}

/* ------------------------------------------------------------ self-tests */

size_t findwin_count(void) { return last ? last->nres : 0; }
const char *findwin_result(size_t i) { return last && i < last->nres ? last->res[i].path : NULL; }
bool findwin_searching(void) { return last && last->searching; }

bool findwin_type(const char *text)
{
    if (!last) return false;
    set_focus(last, F_NAME);
    ui_entry_set_text(last->name, "");
    /* one key at a time, as typed: each starts the search again */
    for (const char *p = text; *p; p++) {
        ui_key_event k = { .sym = (uint32_t)(unsigned char)*p };
        k.utf8[0] = *p;
        fw_key(last->s, &k);
    }
    return true;
}

bool findwin_kind(int kind)
{
    if (!last || kind < 0 || kind > 3) return false;
    last->kind = kind;
    damage_top(last);
    return true;
}

bool findwin_key(uint32_t sym)
{
    if (!last) return false;
    ui_key_event k = { .sym = sym };
    fw_key(last->s, &k);
    return true;
}

bool findwin_snapshot(const char *png) { return last && ui_surface_snapshot(last->s, png); }
