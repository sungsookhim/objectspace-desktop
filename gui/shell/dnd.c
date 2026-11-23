/* wps-shell: Drag and drop: between windows, the desktop and other programs; the drop menu; transfers. */
#define _GNU_SOURCE
#include "shell.h"

static void another_ask(struct another *a);
static void do_drop(struct folder *f, const char *dest, const char *mime, const char *data, int how, struct drop_at at);

struct dragging *cur_drag;

static void dragging_free(struct dragging *d)
{
    if (!d) return;
    for (size_t i = 0; i < d->n; i++) free(d->paths[i]);
    free(d->paths);
    free(d->tpl);
    free(d->dx);
    free(d->dy);
    free(d->wps);
    free(d->uris);
    free(d);
}

/* Append text with %-escapes for a file: URI. */
void uri_append(char **buf, size_t *len, size_t *cap, const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t need = *len + strlen(path) * 3 + 16;
    if (need > *cap) { while (*cap < need) *cap = *cap ? *cap * 2 : 1024; *buf = realloc(*buf, *cap); }
    *len += (size_t)sprintf(*buf + *len, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("/-._~", *p))
            (*buf)[(*len)++] = (char)*p;
        else { (*buf)[(*len)++] = '%'; (*buf)[(*len)++] = hex[*p >> 4]; (*buf)[(*len)++] = hex[*p & 15]; }
    }
    (*buf)[(*len)++] = '\r';
    (*buf)[(*len)++] = '\n';
    (*buf)[*len] = 0;
}

static void drag_send(void *ctx, const char *mime, int fd)
{
    struct dragging *d = ctx;
    const char *data = !strcmp(mime, MIME_WPS) ? d->wps : d->uris;
    ui_app_write_async(app, fd, data, strlen(data));
}

static void drag_end(void *ctx, bool dropped, uint32_t action)
{
    (void)dropped; (void)action;
    if (cur_drag == ctx) cur_drag = NULL;
    dragging_free(ctx);
}

/* Start dragging what d holds (its paths and both formats ready). */
static void start_dragging(struct dragging *d, ui_surface *win, uint32_t serial, ui_kind kind)
{
    if (!d->wps || !d->uris) { dragging_free(d); return; }
    /* The drag image: the item's icon, and how many go. */
    int sc = ui_surface_scale(win);
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48 * sc, 40 * sc);
    cairo_surface_set_device_scale(img, sc, sc);
    cairo_t *cr = cairo_create(img);
    ui_draw_kind_icon(cr, kind, 0, 4, 32);
    if (d->n > 1) {
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%zu", d->n);
        double w = ui_text_width(UI_FONT_BOLD, cnt) + 8;
        ui_fill(cr, (ui_rect){ 46 - w, 22, w, 16 }, ui_scheme_now.selection);
        ui_set_color(cr, ui_scheme_now.selection_text);
        ui_text_draw(cr, UI_FONT_BOLD, 50 - w, 35, cnt, 0);
    }
    cairo_destroy(cr);
    const char *mimes[] = { MIME_WPS, MIME_URIS };
    cur_drag = d;
    if (!ui_drag_start(win, serial, mimes, 2, UI_DND_COPY | UI_DND_MOVE | UI_DND_ASK, img, sc, drag_send, drag_end, d)) {
        cur_drag = NULL;
        dragging_free(d);
    }
    cairo_surface_destroy(img);
}

void src_drag(void *ctx, size_t i, uint32_t serial)
{
    struct folder *f = ctx;
    struct dragging *d = calloc(1, sizeof *d);
    raise_in(f, "object.drag", NULL, "");
    /* The selection if the item is in it, else the item alone (OS/2). */
    size_t total = is_tree(f) ? f->ntvis : f->m.total;
    bool whole = ui_container_is_selected(f->cont, i);
    size_t cap = 0;
    for (size_t k = whole ? 0 : i; k < (whole ? total : i + 1); k++) {
        if (whole && !ui_container_is_selected(f->cont, k)) continue;
        char t[PATH_MAX];
        target_of(f, k, t, sizeof t);
        if (d->n == cap) {
            cap = cap ? cap * 2 : 16;
            d->paths = realloc(d->paths, cap * sizeof *d->paths);
            d->tpl = realloc(d->tpl, cap * sizeof *d->tpl);
            d->dx = realloc(d->dx, cap * sizeof *d->dx);
            d->dy = realloc(d->dy, cap * sizeof *d->dy);
        }
        double gx, gy, sx, sy, ix = 0, iy = 0;
        ui_container_drag_origin(f->cont, &sx, &sy);
        ui_container_to_list(f->cont, sx, sy, &gx, &gy);
        if (!is_tree(f)) layout_pos(f->lay, k, &ix, &iy);
        d->dx[d->n] = ix - gx;
        d->dy[d->n] = iy - gy;
        struct row *rw = row_at(f, k);
        d->tpl[d->n] = rw && rw->tpl;
        d->paths[d->n++] = strdup(t);
    }
    size_t wl = 0, wc = 0, ul = 0, uc = 0;
    for (size_t k = 0; k < d->n; k++) {
        size_t need = wl + strlen(d->paths[k]) + 12;
        if (need > wc) { while (wc < need) wc = wc ? wc * 2 : 1024; d->wps = realloc(d->wps, wc); }
        /* "<path>\ttemplate" for a template (WPS's own type only) */
        wl += (size_t)sprintf(d->wps + wl, "%s%s\n", d->paths[k], d->tpl[k] ? "\ttemplate" : "");
        if (strncmp(d->paths[k], "vobj:", 5)) uri_append(&d->uris, &ul, &uc, d->paths[k]);   /* central: no file to offer */
    }
    if (!d->uris) d->uris = strdup("");
    snprintf(d->src_folder, sizeof d->src_folder, "%s", f->path);
    ui_item it = { 0 };
    src_item(f, i, &it);
    start_dragging(d, f->win, serial, it.loaded ? it.kind : UI_KIND_FILE);
}

/* Find's results dragged (paths from anywhere): moved, copied or made
 * shadows of wherever they are dropped, as from a folder. */
void shell_drag_paths(ui_surface *win, uint32_t serial, const char *const *paths, size_t n, ui_kind kind)
{
    if (!n) return;
    struct dragging *d = calloc(1, sizeof *d);
    d->paths = calloc(n, sizeof *d->paths);
    d->tpl = calloc(n, sizeof *d->tpl);
    d->dx = calloc(n, sizeof *d->dx);
    d->dy = calloc(n, sizeof *d->dy);
    size_t wl = 0, wc = 0, ul = 0, uc = 0;
    for (size_t k = 0; k < n; k++) {
        d->paths[d->n] = strdup(paths[k]);
        d->dx[d->n] = d->dy[d->n] = 16.0 * (double)k;   /* a small step each, where they land */
        d->n++;
        size_t need = wl + strlen(paths[k]) + 12;
        if (need > wc) { while (wc < need) wc = wc ? wc * 2 : 1024; d->wps = realloc(d->wps, wc); }
        wl += (size_t)sprintf(d->wps + wl, "%s\n", paths[k]);
        if (strncmp(paths[k], "vobj:", 5)) uri_append(&d->uris, &ul, &uc, paths[k]);
    }
    if (!d->uris) d->uris = strdup("");
    if (!d->wps) { dragging_free(d); return; }
    start_dragging(d, win, serial, kind);   /* (no source folder: they come from many) */
}

/* Where a drop at (x, y) goes: a folder under the pointer, else the
 * window's folder. */
static void drop_dest(struct folder *f, double x, double y, char *out, size_t n, size_t *item)
{
    size_t i = ui_container_item_at(f->cont, x, y);
    ui_item it = { 0 };
    if (i != SIZE_MAX) src_item(f, i, &it);
    /* onto its own icon (a short drag): the folder it is in, not what it
     * is (a folder's shadow would otherwise go into its own original) */
    if (i != SIZE_MAX && dragged_onto_itself(f, i)) i = SIZE_MAX;
    if (i != SIZE_MAX && it.loaded && it.kind == UI_KIND_FOLDER) {
        struct row *rw = row_at(f, i);
        if (rw && rw->shadow && rw->target) snprintf(out, n, "%s", rw->target);   /* into the original */
        else target_of(f, i, out, n);
        *item = i;
    } else {
        snprintf(out, n, "%s", f->path);
        *item = SIZE_MAX;
    }
}

static bool shadow_mods(unsigned mods) { return ui_mouse_drop_how(mods) == 2; }

/* Dragged out of a folder this user cannot change (/usr/share/applications,
 * another user's): a move there could only copy and then fail to remove
 * the originals, so a plain drag elsewhere copies, as GNOME's Files does. */
static bool drag_copies_by_default(const char *dest)
{
    if (!cur_drag || !strcmp(cur_drag->src_folder, dest)) return false;
    for (size_t i = 0; i < cur_drag->n; i++) {
        if (!strncmp(cur_drag->paths[i], "vobj:", 5)) continue;
        /* each item's own folder (Find's results come from many) */
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", cur_drag->paths[i]);
        char *sl = strrchr(parent, '/');
        if (!sl) continue;
        if (sl == parent) sl[1] = 0; else *sl = 0;
        if (strcmp(parent, dest) && access(parent, W_OK) != 0) return true;
    }
    return false;
}

static bool under(const char *path, const char *dir)
{
    size_t l = strlen(dir);
    return !strncmp(path, dir, l) && (path[l] == '/' || path[l] == 0);
}

/* A program's icon under (x, y) (a program object, a .desktop file, an
 * executable, or a shadow of one): objects dropped on it start it with
 * them, as in OS/2. */
/* (1: it takes them; -1: a program that does not open what is dragged -
 * the drop is refused, nothing moves; 0: no program there) */
static int program_at(struct folder *f, double x, double y, char *out, size_t n, size_t *item)
{
    size_t i = ui_container_item_at(f->cont, x, y);
    if (i == SIZE_MAX || is_tree(f)) return 0;
    ui_item it = { 0 };
    src_item(f, i, &it);
    if (!it.loaded || it.kind != UI_KIND_PROGRAM) return 0;
    target_of(f, i, out, n);
    *item = i;
    /* not onto itself, dragged */
    for (size_t k = 0; cur_drag && k < cur_drag->n; k++) if (!strcmp(cur_drag->paths[k], out)) return 0;
    /* what it opens (its desktop entry's types; a shadow's original's) */
    if (cur_drag) {
        struct row *rw = row_at(f, i);
        const char *prog = rw && rw->shadow && rw->target && rw->target[0] == '/' ? rw->target : out;
        const char **files = calloc(cur_drag->n ? cur_drag->n : 1, sizeof *files);
        size_t nf = 0;
        for (size_t k = 0; k < cur_drag->n; k++) if (cur_drag->paths[k][0] == '/') files[nf++] = cur_drag->paths[k];
        bool ok = nf && program_opens(prog, files, nf);
        free(files);
        if (!ok) return -1;
    }
    return 1;
}

uint32_t win_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n,
                                uint32_t src_actions, unsigned mods, const char **mime)
{
    struct folder *f = ui_surface_data(s);
    *mime = NULL;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(mimes[i], MIME_FONT)) { *mime = MIME_FONT; return UI_DND_COPY; }   /* a font: the folder's, or the title bars' */
    for (size_t i = 0; i < n && !*mime; i++) if (!strcmp(mimes[i], MIME_WPS)) *mime = MIME_WPS;
    for (size_t i = 0; i < n && !*mime; i++) if (!strcmp(mimes[i], MIME_URIS)) *mime = MIME_URIS;
    if (!*mime) return 0;
    char dest[PATH_MAX];
    size_t item;
    int pa = program_at(f, x, y, dest, sizeof dest, &item);
    if (pa < 0) { ui_container_set_drop_target(f->cont, SIZE_MAX); return 0; }   /* it does not open these */
    if (pa > 0) {
        /* (a copy, never a move: the objects stay where they are) */
        ui_container_set_drop_target(f->cont, src_actions & UI_DND_COPY ? item : SIZE_MAX);
        return src_actions & UI_DND_COPY ? UI_DND_COPY : 0;
    }
    drop_dest(f, x, y, dest, sizeof dest, &item);
    if (is_trash_dir(dest) && !(cur_drag && !strcmp(cur_drag->src_folder, dest)) && !dragged_onto_itself(f, item)) {
        /* onto the Trash Can: into the trash (a copy for the source: it
         * removes nothing itself) */
        ui_container_set_drop_target(f->cont, item);
        return src_actions & UI_DND_COPY ? UI_DND_COPY : src_actions & UI_DND_MOVE ? UI_DND_MOVE : 0;
    }
    if (is_startup_dir(dest) && !(cur_drag && !strcmp(cur_drag->src_folder, dest)) && !dragged_onto_itself(f, item)) {
        /* onto Startup: an entry or a shadow is made there (a copy for the source) */
        ui_container_set_drop_target(f->cont, item);
        return src_actions & UI_DND_COPY ? UI_DND_COPY : src_actions & UI_DND_MOVE ? UI_DND_MOVE : 0;
    }
    /* As in OS/2, from the keys held (the Mouse object's mappings; by
     * default none moves, Ctrl copies, Ctrl+Shift makes a shadow, Alt
     * asks). GNOME's own choice is not used: for a button 2 drag it says
     * "ask", and now and then "copy", with no key held. A shadow is
     * answered as a copy (nothing is removed from the source). */
    int how = ui_mouse_drop_how(mods);
    if (how == 0 && drag_copies_by_default(dest)) how = 1;
    uint32_t act = how < 0 ? UI_DND_ASK : how ? UI_DND_COPY : UI_DND_MOVE;
    if (!(act & src_actions)) act = src_actions & UI_DND_COPY ? UI_DND_COPY : src_actions & UI_DND_MOVE ? UI_DND_MOVE : 0;
    if (cur_drag) {
        for (size_t i = 0; i < cur_drag->n; i++) {
            if (under(dest, cur_drag->paths[i])) act = 0;   /* a folder into itself */
            char parent[PATH_MAX];
            snprintf(parent, sizeof parent, "%s", cur_drag->paths[i]);
            char *sl = strrchr(parent, '/');
            if (sl) *sl = 0;
            if (act == UI_DND_MOVE && !shadow_mods(mods) && !strcmp(parent, dest) && cur_drag->n == 1 &&
                ui_container_view(f->cont) != UI_VIEW_ICON)
                act = 0;   /* already there (in icon view: moving the icon) */
        }
        /* only locked icons, over their own folder: they would not move
         * (the no-drop pointer says so); onto other objects they go */
        bool all_locked = act == UI_DND_MOVE && !shadow_mods(mods) && !is_tree(f) && cur_drag->n &&
                          ui_container_view(f->cont) == UI_VIEW_ICON && !strcmp(cur_drag->src_folder, f->path) &&
                          !strcmp(dest, f->path);
        for (size_t i = 0; all_locked && i < cur_drag->n; i++) {
            char nm[NAME_MAX + 8];
            const char *p = cur_drag->paths[i];
            if (!strncmp(p, "vobj:", 5)) snprintf(nm, sizeof nm, "\x01%s", p);
            else snprintf(nm, sizeof nm, "%s", strrchr(p, '/') ? strrchr(p, '/') + 1 : p);
            all_locked = layout_is_locked(f->lay, nm);
        }
        if (all_locked) act = 0;
    }
    ui_container_set_drop_target(f->cont, act ? item : SIZE_MAX);
    return act;
}

void win_drag_leave(ui_surface *s)
{
    struct folder *f = ui_surface_data(s);
    ui_container_set_drop_target(f->cont, SIZE_MAX);
}

struct jobtrack *jobs;

ui_dialog *last_dialog;   /* for self-tests */

static void xfer_free(struct xfer *x)
{
    for (size_t i = 0; i < x->n; i++) free(x->names[i]);
    free(x->names);
    free(x->x);
    free(x->y);
    free(x);
}

void set_status(long folder, const char *msg)
{
    struct folder *f = folder_by_id(folder);
    if (!f) return;
    snprintf(f->error, sizeof f->error, "%s", msg);
    damage_status(f);
}

/* An action event about a folder window (or an object in it). */
void raise_in(struct folder *f, const char *name, const char *object, const char *cls)
{
    ui_rect at = f ? ui_frame_client(f->win) : (ui_rect){ 0, 0, 0, 0 };
    events_raise(name, object ? object : f ? f->path : "", cls, at);
}

/* Something failed: the status line says what, and alert.error sounds. */
void report_error(long folder, const char *msg)
{
    set_status(folder, msg);
    struct folder *f = folder_by_id(folder);
    raise_in(f, "alert.error", NULL, "");
    if (f && f->desktop) messages_post(msg, "", 2);   /* (the desktop has no status line: Messages) */
}

/* A question to the user: alert.warning sounds, as OS/2's warning did. */
ui_dialog *ask(const char *title, const char *msg, const char *const *buttons, size_t n, int def, ui_dialog_fn fn,
                      void *ctx)
{
    events_raise("alert.warning", "", "", (ui_rect){ 0, 0, 0, 0 });
    return ui_dialog_open(app, title, msg, buttons, n, def, fn, ctx);
}

static void got_job(void *arg, const cv *reply)
{
    struct xfer *x = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && res->k == CV_INT) {
        struct jobtrack *t = calloc(1, sizeof *t);
        t->job = res->i;
        t->folder = x->folder;
        snprintf(t->kind, sizeof t->kind, "%s", x->kind);
        snprintf(t->dest, sizeof t->dest, "%s", x->dest);
        t->next = jobs;
        jobs = t;
    } else {
        char m[300];
        snprintf(m, sizeof m, "Cannot %s: %s", x->kind, reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        report_error(x->folder, m);
    }
    xfer_free(x);
}

void got_sel(void *arg, const cv *reply)
{
    struct xfer *x = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res || res->k != CV_INT) { set_status(x->folder, "Cannot select the dragged objects"); xfer_free(x); return; }
    svc_job_start(x->kind, res->i, x->dest, (const char *const *)x->names, x->x, x->y, x->place ? x->n : 0, got_job, x);
}

/* "file:///a%20b" -> "/a b"; NULL if not a local file. */
char *uri_path(const char *u)
{
    if (strncmp(u, "file://", 7)) return NULL;
    u += 7;
    if (!strncmp(u, "localhost", 9)) u += 9;
    if (*u != '/') return NULL;
    char *out = malloc(strlen(u) + 1), *o = out;
    for (; *u; u++) {
        if (*u == '%' && u[1] && u[2]) {
            char h[3] = { u[1], u[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            u += 2;
        } else *o++ = *u;
    }
    *o = 0;
    return out;
}

/* Icons dragged within their own folder, in icon view: they go where they
 * were dropped, each keeping its offset from the grab point, and the
 * places are stored. Returns false if the drag came from elsewhere. */
bool reposition(struct folder *f, double x, double y)
{
    struct dragging *d = cur_drag;
    for (size_t i = 0; i < d->n; i++) {
        if (!strncmp(d->paths[i], "vobj:", 5)) {
            if (strcmp(d->src_folder, f->path)) return false;
            continue;
        }
        const char *sl = strrchr(d->paths[i], '/');
        size_t pl = sl ? (size_t)(sl - d->paths[i]) : 0;
        if (pl != strlen(f->path) || strncmp(d->paths[i], f->path, pl)) return false;
    }
    double lx, ly;
    ui_container_to_list(f->cont, x, y, &lx, &ly);
    int64_t *px = calloc(d->n, sizeof *px), *py = calloc(d->n, sizeof *py);
    const char **moved = calloc(d->n ? d->n : 1, sizeof *moved);
    size_t k = 0;
    for (size_t i = 0; i < d->n; i++) {
        char nm[NAME_MAX + 8];
        if (!strncmp(d->paths[i], "vobj:", 5)) snprintf(nm, sizeof nm, "\x01%s", d->paths[i]);
        else snprintf(nm, sizeof nm, "%s", strrchr(d->paths[i], '/') + 1);
        if (layout_is_locked(f->lay, nm)) continue;   /* locked: stays (it can still go to other folders) */
        double nx = lx + d->dx[i], ny = ly + d->dy[i];
        px[k] = nx < 0 ? 0 : (int64_t)nx;
        py[k] = ny < 0 ? 0 : (int64_t)ny;
        moved[k] = d->paths[i];
        layout_place(f->lay, nm, (int)px[k], (int)py[k]);
        k++;
    }
    if (k < d->n) {
        snprintf(f->error, sizeof f->error, "%s", k ? "Locked icons stay where they are" : "The position is locked");
        damage_status(f);
    }
    if (k) svc_place(f->path, moved, px, py, k, placed_reply, as_arg(f));
    free(moved);
    free(px);
    free(py);
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
    ui_container_layout_changed(f->cont);
    return true;
}

static bool drop_moves_templates;   /* Shift held: templates are moved, not made from */

static void ask_chosen(void *arg, int64_t id)
{
    struct ask_ctx *a = arg;
    struct folder *f = folder_by_id(a->folder);
    if (f && id >= DROP_MOVE && id <= DROP_SHADOW) do_drop(f, a->dest, a->mime, a->data, (int)id, a->at);
    free(a->mime);
    free(a->data);
    free(a);
}

/* How a drop is meant, as the Mouse object maps the keys (OS/2's: none moves, Ctrl copies, Ctrl+Shift
  * makes a shadow, Alt asks). Keys held when the drag began reach us; keys
 * pressed during it reach only GNOME, which then changes its choice: to
 * copy for Ctrl, to move for Shift or Ctrl+Shift (it cannot tell them
 * apart, so a change to move means a shadow). */
int drop_how(uint32_t action, uint32_t start_action, unsigned mods)
{
    int how = ui_mouse_drop_how(mods);   /* the keys held, as mapped */
    if (how) return how;
    if (start_action && action != start_action) {
        if (action == UI_DND_COPY) return 1;
        if (action == UI_DND_MOVE) return 2;
        if (action == UI_DND_ASK) return -1;
    }
    return 0;
}

uint32_t win_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len, uint32_t action,
                     uint32_t start_action, unsigned mods)
{
    (void)len;
    struct folder *f = ui_surface_data(s);
    ui_container_set_drop_target(f->cont, SIZE_MAX);
    if (!strcmp(mime, MIME_FONT)) {
        /* the Font Palette's (v0.3-design 10), as on OS/2 a folder's own:
         * onto its title bar, its title bar's (with Shift, every title
         * bar's); else its icons' (with Shift, every folder's) */
        char spec[128];
        snprintf(spec, sizeof spec, "%.*s", (int)strcspn(data, "\r\n"), data);
        if (!ui_font_valid(spec)) return 0;
        bool title = !f->desktop && ui_frame_hit(s, x, y) == UI_HIT_TITLE;
        if (title && (mods & UI_MOD_SHIFT)) font_apply("titles", spec);
        else if (title) {
            cv v = { .k = CV_TEXT, .s = spec, .len = strlen(spec) };
            svc_settings_set(f->path, "title_font", &v, -1, ignore_reply, NULL);
        }
        else if (mods & UI_MOD_SHIFT) font_apply("folders", spec);
        else {
            cv v = { .k = CV_TEXT, .s = spec, .len = strlen(spec) };
            svc_settings_set(f->path, "icon_font", &v, -1, ignore_reply, NULL);   /* (back through its settings) */
        }
        return UI_DND_COPY;
    }
    char dest[PATH_MAX];
    size_t item;
    char prog[PATH_MAX];
    int pa = program_at(f, x, y, prog, sizeof prog, &item);
    if (pa < 0) return 0;   /* refused: nothing moves, the drag ends where it began */
    if (pa > 0) {
        /* dropped on a program: it starts with them */
        const char *files[32];
        size_t nf = 0;
        char *copy = strdup(data), *save = NULL;
        for (char *line = strtok_r(copy, "\r\n", &save); line && nf < 32; line = strtok_r(NULL, "\r\n", &save)) {
            if (!*line || *line == '#') continue;
            char *tab = !strcmp(mime, MIME_URIS) ? NULL : strchr(line, '\t');
            if (tab) *tab = 0;
            char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
            if (p && p[0] == '/') files[nf++] = p;
            else free(p);
        }
        free(copy);
        /* (from another program: what it opens is checked only now) */
        struct row *prw = row_at(f, item);
        const char *pp = prw && prw->shadow && prw->target && prw->target[0] == '/' ? prw->target : prog;
        if (nf && !program_opens(pp, files, nf)) {
            for (size_t k = 0; k < nf; k++) free((char *)files[k]);
            snprintf(f->error, sizeof f->error, "That program does not open this kind of file");
            damage_status(f);
            return 0;
        }
        if (nf) {
            snprintf(f->pending_open, sizeof f->pending_open, "%s", prog);
            raise_in(f, "object.drop", prog, "program");
            svc_open_files(prog, false, files, nf, opened, as_arg(f));
        } else {
            snprintf(f->error, sizeof f->error, "Only files can be given to a program");
            damage_status(f);
        }
        for (size_t k = 0; k < nf; k++) free((char *)files[k]);
        return UI_DND_COPY;
    }
    drop_dest(f, x, y, dest, sizeof dest, &item);
    if (is_trash_dir(dest) && !(cur_drag && !strcmp(cur_drag->src_folder, dest)) && !dragged_onto_itself(f, item)) {
        trash_dropped(f, mime, data);
        return UI_DND_COPY;
    }
    if (is_startup_dir(dest) && !(cur_drag && !strcmp(cur_drag->src_folder, dest)) && !dragged_onto_itself(f, item)) {
        startup_dropped(f, mime, data);
        return UI_DND_COPY;
    }
    int w = drop_how(action, start_action, mods);
    if (w == 0 && drag_copies_by_default(dest)) w = 1;
    struct drop_at at = { item == SIZE_MAX && !strcmp(dest, f->path) && ui_container_view(f->cont) == UI_VIEW_ICON, 0, 0 };
    if (at.place) ui_container_to_list(f->cont, x, y, &at.lx, &at.ly);
    int how = w == 2 ? DROP_SHADOW : w == 1 ? DROP_COPY : DROP_MOVE;
    /* Templates (OS/2): a drag makes a new object from them, even within
     * their own folder; Shift+drag moves the template itself */
    bool tpl_move = (mods & UI_MOD_SHIFT) && !(mods & UI_MOD_CTRL);
    bool has_tpl = false;
    for (size_t k = 0; cur_drag && cur_drag->tpl && k < cur_drag->n; k++) has_tpl |= cur_drag->tpl[k];
    if (w == 0 && how == DROP_MOVE && cur_drag && (tpl_move || !has_tpl) && !strcmp(dest, f->path) &&
        ui_container_view(f->cont) == UI_VIEW_ICON && reposition(f, x, y))
        return UI_DND_MOVE;
    if (w < 0) {
        struct ask_ctx *a = calloc(1, sizeof *a);
        a->folder = f->id;
        snprintf(a->dest, sizeof a->dest, "%s", dest);
        a->mime = strdup(mime);
        a->data = strdup(data);
        a->at = at;
        ui_menu_item items[] = {
            { DROP_MOVE, "Move here", true, false, NULL, 0 },
            { DROP_COPY, "Copy here", true, false, NULL, 0 },
            { DROP_SHADOW, "Create shadow here", true, false, NULL, 0 },
            { -1, "-", false, false, NULL, 0 },
            { 99, "Cancel", true, false, NULL, 0 },
        };
        ui_menu_popup(s, items, 5, x, y, 0, ask_chosen, a);
        return UI_DND_COPY;   /* decided later in the menu: the source must not remove anything */
    }
    if (getenv("WPS_DEBUG_DND"))
        fprintf(stderr, "dnd: dropping as %s\n", how == DROP_SHADOW ? "shadow" : how == DROP_COPY ? "copy" : "move");
    raise_in(f, "object.drop", dest, how == DROP_SHADOW ? "shadow" : how == DROP_COPY ? "copy" : "move");
    drop_moves_templates = tpl_move && how == DROP_MOVE;
    do_drop(f, dest, mime, data, how, at);
    drop_moves_templates = false;
    return how == DROP_MOVE ? UI_DND_MOVE : UI_DND_COPY;
}

/* A shadow or central object made or moved by a drop: place it where it
 * was dropped once the service gives its address. */

/* The new object made here, once listed: selected, its name being edited. */
void rename_new_now(struct folder *f)
{
    if (!f->rename_new[0]) return;
    for (size_t i = 0; i < f->m.total; i++)
        if (model_name(&f->m, i) && !strcmp(model_name(&f->m, i), f->rename_new)) {
            f->rename_new[0] = 0;
            /* Typing goes to this window (the drop came from another). */
            ui_window_activate(f->win);
            /* The object just asked for takes over from a name still open. */
            if (ui_container_rename_item(f->cont) != SIZE_MAX) ui_container_rename_moved(f->cont, SIZE_MAX);
            ui_container_select_none(f->cont);
            ui_container_select(f->cont, i);
            ui_container_set_cursor(f->cont, i);
            ui_container_begin_rename(f->cont, i);
            return;
        }
}

/* Create another (OS/2): a new object of the same kind as item i (the
 * folder itself for SIZE_MAX), or made from it if it is a template; a
 * dialog asks its name and folder (v0.3-design 9). */

struct another *another_now;   /* (one at a time) */

static void another_folder(long owner, const char *key, const char *path)
{
    (void)owner; (void)key;
    if (!another_now) return;
    snprintf(another_now->dir, sizeof another_now->dir, "%s", path);
    another_ask(another_now);
}

static void another_chosen(void *ctx, int choice)
{
    struct another *a = ctx;
    if (a->dlg) {
        const char *t = ui_dialog_text(a->dlg);
        if (t && *t) snprintf(a->name, sizeof a->name, "%s", t);
    }
    a->dlg = NULL;
    if (choice == 1) {   /* Folder...: chosen, then asked again */
        filepick_open("Create Another", "folder", a->dir, another_folder, 0, "");
        return;
    }
    if (choice == 0 && !strchr(a->name, '/')) {
        struct land *l = calloc(1, sizeof *l);
        l->folder = a->folder;
        snprintf(l->dest, sizeof l->dest, "%s", a->dir);
        if (a->tpl[0]) svc_from_template(a->tpl, a->dir, a->name, made_new, l);
        else svc_create(a->dir, a->cls, a->name, made_new, l);
        struct folder *f = folder_by_id(a->folder);
        if (f && strcmp(a->dir, f->path)) {
            char m[PATH_MAX + 64];
            snprintf(m, sizeof m, "Made in %s", a->dir);
            set_status(f->id, m);
        }
    } else if (choice == 0) {
        struct folder *f = folder_by_id(a->folder);
        if (f) set_status(f->id, "Not made: a name cannot have '/' in it");
    }
    if (another_now == a) another_now = NULL;
    free(a);
}

static void another_ask(struct another *a)
{
    char msg[PATH_MAX + 128];
    snprintf(msg, sizeof msg, "A new %s in the folder %s. Its name:", a->what, a->dir);
    static const char *const b[] = { "Create", "Folder...", "Cancel" };
    a->dlg = ui_dialog_open_entry(app, "Create Another", msg, false, b, 3, 0, another_chosen, a);
    ui_dialog_set_text(a->dlg, a->name);
}

void create_another(struct folder *f, size_t i)
{
    struct row *rw = row_at(f, i);
    struct another *a = calloc(1, sizeof *a);
    a->folder = f->id;
    if (i == SIZE_MAX || is_tree(f)) {
        char t[PATH_MAX];
        target_of(f, i, t, sizeof t);
        char *sl = strrchr(t, '/');
        if (!sl || sl == t) { free(a); return; }
        *sl = 0;
        snprintf(a->dir, sizeof a->dir, "%s", t);
        snprintf(a->cls, sizeof a->cls, "WPFolder");
        snprintf(a->what, sizeof a->what, "folder");
        snprintf(a->name, sizeof a->name, "New Folder");
    } else {
        if (!rw || rw->shadow || rw->link || (rw->name[0] == '\x01' && !rw->tpl)) {
            set_status(f->id, "Create another works on files, folders, programs and templates");
            free(a);
            return;
        }
        snprintf(a->dir, sizeof a->dir, "%s", f->path);
        size_t nl = strlen(rw->name);
        if (rw->tpl) {
            target_of(f, i, a->tpl, sizeof a->tpl);   /* made from the template */
            snprintf(a->what, sizeof a->what, "object from the template");
            snprintf(a->name, sizeof a->name, "%s", rw->title ? rw->title : rw->name);
        } else if (rw->kind == UI_KIND_FOLDER) {
            snprintf(a->cls, sizeof a->cls, "WPFolder");
            snprintf(a->what, sizeof a->what, "folder");
            snprintf(a->name, sizeof a->name, "New Folder");
        } else if (nl > 8 && !strcmp(rw->name + nl - 8, ".desktop")) {
            snprintf(a->cls, sizeof a->cls, "WPProgram");
            snprintf(a->what, sizeof a->what, "program");
            snprintf(a->name, sizeof a->name, "New Program");
        } else {
            /* a data file of the same type: the same extension */
            const char *dot = strrchr(rw->name, '.');
            snprintf(a->cls, sizeof a->cls, "WPDataFile");
            snprintf(a->what, sizeof a->what, "data file");
            snprintf(a->name, sizeof a->name, "New Data File%s", dot && dot != rw->name ? dot : "");
        }
    }
    if (another_now && !another_now->dlg) free(another_now);   /* (an earlier one left at its folder picker) */
    another_now = a;
    another_ask(a);
}

/* A new object was made (from a template, or Create another): place it
 * where it was dropped, and edit its name once the window lists it. */
void made_new(void *arg, const cv *reply)
{
    struct land *l = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *addr = res ? cv_text(res, "address") : NULL, *path = res ? cv_text(res, "path") : NULL;
    if (!addr || !*addr) addr = path;
    if (!res || !addr || !*addr) {
        char m[300];
        snprintf(m, sizeof m, "Not made: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(l->folder, m);
        free(l);
        return;
    }
    char nm[NAME_MAX + 64];
    if (!strncmp(addr, "vobj:", 5)) snprintf(nm, sizeof nm, "\x01%s", addr);
    else snprintf(nm, sizeof nm, "%s", strrchr(addr, '/') ? strrchr(addr, '/') + 1 : addr);
    raise_in(folder_by_id(l->folder), "object.create", addr, "");
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing && !strcmp(o->path, l->dest) && !is_tree(o)) {
            snprintf(o->rename_new, sizeof o->rename_new, "%s", nm);
            rename_new_now(o);   /* already listed (the folder's change came first) */
        }
    struct folder *f = folder_by_id(l->folder);
    if (l->place && f) {
        const char *t[1] = { addr };
        svc_place(l->dest, t, &l->x, &l->y, 1, placed_reply, as_arg(f));
        layout_place(f->lay, nm, (int)l->x, (int)l->y);   /* before the listing catches up */
    }
    free(l);
}

static void landed(void *arg, const cv *reply)
{
    struct land *l = arg;
    struct folder *f = folder_by_id(l->folder);
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    const char *addr = reply ? cv_text(reply, "result") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b) || !addr) {
        char m[300];
        snprintf(m, sizeof m, "Not done: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(l->folder, m);
    } else if (l->place && f) {
        const char *t[1] = { addr };
        svc_place(l->dest, t, &l->x, &l->y, 1, placed_reply, as_arg(f));
        char nm[NAME_MAX + 8];
        if (!strncmp(addr, "vobj:", 5)) snprintf(nm, sizeof nm, "\x01%s", addr);
        else snprintf(nm, sizeof nm, "%s", strrchr(addr, '/') ? strrchr(addr, '/') + 1 : addr);
        layout_place(f->lay, nm, (int)l->x, (int)l->y);   /* before the listing catches up */
    }
    free(l);
}

static void do_drop(struct folder *f, const char *dest, const char *mime, const char *data, int how, struct drop_at at)
{
    size_t k = 0;   /* the item's place in the drag */
    bool shadow = how == DROP_SHADOW;
    const char *kind = how == DROP_COPY ? "copy" : "move";
    /* Paths, grouped by the folder they come from (a job per folder). */
    char *copy = strdup(data), *save = NULL;
    struct xfer *groups[64];
    size_t ng = 0, total = 0;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        bool tpl = false;
        char *tab = !strcmp(mime, MIME_URIS) ? NULL : strchr(line, '\t');
        if (tab) { tpl = !strcmp(tab + 1, "template"); *tab = 0; }
        char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
        if (!p) continue;
        total++;
        /* Where this item lands: its offset in our own drag, or a small
         * step per item for drags from other programs. */
        double ox = 16.0 * (double)k, oy = 16.0 * (double)k;
        if (cur_drag && k < cur_drag->n && !strcmp(cur_drag->paths[k], p)) { ox = cur_drag->dx[k]; oy = cur_drag->dy[k]; }
        int64_t px = at.lx + ox < 0 ? 0 : (int64_t)(at.lx + ox), py = at.ly + oy < 0 ? 0 : (int64_t)(at.ly + oy);
        k++;
        if (tpl && !shadow && !drop_moves_templates) {
            /* A template dropped (moved or copied): a new object from it,
             * where it was dropped, with its name ready to edit (OS/2). */
            struct land *l = calloc(1, sizeof *l);
            l->folder = f->id;
            snprintf(l->dest, sizeof l->dest, "%s", dest);
            l->x = px;
            l->y = py;
            l->place = at.place;
            svc_from_template(p, dest, NULL, made_new, l);
            free(p);
            continue;
        }
        if (shadow || !strncmp(p, "vobj:", 5)) {
            struct land *l = calloc(1, sizeof *l);
            l->folder = f->id;
            snprintf(l->dest, sizeof l->dest, "%s", dest);
            l->x = px;
            l->y = py;
            l->place = at.place;
            if (shadow) svc_shadow(p, dest, landed, l);
            else if (!strcmp(kind, "move") && cur_drag && !strcmp(cur_drag->src_folder, dest)) free(l);   /* already there */
            else svc_vmove(p, dest, !strcmp(kind, "copy"), landed, l);
            free(p);
            continue;
        }
        char *sl = strrchr(p, '/');
        if (!sl || under(dest, p)) { free(p); continue; }   /* never into itself */
        *sl = 0;
        const char *from = p[0] ? p : "/", *name = sl + 1;
        if (!strcmp(kind, "move") && !strcmp(from, dest)) { free(p); continue; }   /* already there */
        struct xfer *g = NULL;
        for (size_t k = 0; k < ng && !g; k++) if (!strcmp(groups[k]->from, from)) g = groups[k];
        if (!g && ng < 64) {
            g = calloc(1, sizeof *g);
            g->folder = f->id;
            snprintf(g->kind, sizeof g->kind, "%s", kind);
            snprintf(g->dest, sizeof g->dest, "%s", dest);
            snprintf(g->from, sizeof g->from, "%s", from);
            groups[ng++] = g;
        }
        if (g) {
            g->names = realloc(g->names, (g->n + 1) * sizeof *g->names);
            g->x = realloc(g->x, (g->n + 1) * sizeof *g->x);
            g->y = realloc(g->y, (g->n + 1) * sizeof *g->y);
            g->x[g->n] = px;
            g->y[g->n] = py;
            g->place = at.place;
            g->names[g->n++] = strdup(name);
        }
        free(p);
    }
    free(copy);
    for (size_t k = 0; k < ng; k++) svc_select(groups[k]->from, (const char *const *)groups[k]->names, groups[k]->n, got_sel, groups[k]);
    char m[300];
    snprintf(m, sizeof m, "%s %zu object%s...", shadow ? "Making shadows of" : !strcmp(kind, "copy") ? "Copying" : "Moving",
             total, total == 1 ? "" : "s");
    set_status(f->id, m);
}

void decided(void *arg, int choice)
{
    struct decide_ctx *d = arg;
    for (struct jobtrack *t = jobs; t; t = t->next)
        if (t->job == d->job) t->asking = false;
    switch (choice) {
    case 0: svc_job_decide(d->job, "rename", false); break;
    case 1: svc_job_decide(d->job, "skip", false); break;
    case 2: svc_job_decide(d->job, "rename", true); break;
    case 3: svc_job_decide(d->job, "skip", true); break;
    default: svc_job_cancel(d->job); break;
    }
    free(d);
}
