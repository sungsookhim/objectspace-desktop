/* wps-shell: The icon and details views' source (the container's items) and the icons' layout and arrangement. */
#define _GNU_SOURCE
#include "shell.h"

static void new_spot(void *ctx, const char *name);
static void spots_later(void);
static void arrange_by(struct folder *f, int key);

/* ----------------------------------------------------------- container src */

/* A folder's window opened or closed: its icon, and any shadow of it, may
 * be in any window; they redraw (this happens only on opening or closing). */
void notify_in_use(const char *path)
{
    (void)path;
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing) ui_container_layout_changed(o->cont);
}

size_t src_count(void *ctx)
{
    struct folder *f = ctx;
    return is_tree(f) ? f->ntvis : f->m.total;
}

/* A program's icon was made (an SVG, by glycin): every window draws again. */
void appicons_ready(void)
{
    warpcenter_damage();   /* (the tray's icons) */
    apppick_icons_changed();
    for (struct folder *f = folders; f; f = f->next)
        if (!f->closing && f->cont) ui_container_changed(f->cont, 0, SIZE_MAX);
}

void src_item(void *ctx, size_t i, ui_item *out)
{
    struct folder *f = ctx;
    memset(out, 0, sizeof *out);
    if (is_tree(f)) {
        if (i >= f->ntvis) return;
        struct tnode *n = f->tvis[i];
        out->loaded = true;
        out->kind = UI_KIND_FOLDER;
        out->name = out->title = n->name;
        out->depth = n->depth;
        out->expanded = n->expanded;
        out->expandable = !n->loaded || n->nkids > 0;   /* unknown until read */
        for (struct folder *o = folders; o && !out->in_use; o = o->next)
            out->in_use = o != f && !o->closing && !strcmp(o->path, n->path);
        out->last_child = !n->parent || n->parent->kids[n->parent->nkids - 1] == n;
        for (struct tnode *a = n->parent; a && a->parent; a = a->parent)
            if (a->parent->kids[a->parent->nkids - 1] != a) out->open_levels |= 1u << a->depth;
        return;
    }
    struct page *pg = i / PAGE < f->m.npages ? f->m.pages[i / PAGE] : NULL;
    if (!pg || pg->state == P_NONE || !pg->rows[i % PAGE].name) return;
    struct row *r = &pg->rows[i % PAGE];
    out->loaded = true;
    out->detailed = pg->state == P_DETAILED;
    out->kind = r->kind;
    out->name = r->name;
    out->title = row_shown(f, r);
    out->size = r->size;
    out->mtime = r->mtime;
    out->shadow = r->shadow;
    out->in_use = r->in_use;
    out->link = r->link;
    out->is_template = r->tpl;
    /* its icon at the pixels it is drawn with: the view's size in the
     * folder's set (the details' small, a tree's middle) */
    const ui_icon_set *set = folder_icons(f);
    ui_view v = ui_container_view(f->cont);
    int px = (int)((v == UI_VIEW_ICON ? set->large : v == UI_VIEW_TREE ? set->middle : set->small) * ui_surface_scale(f->win) + 0.5);
    if (r->kind == UI_KIND_FOLDER) {
        /* The folder, or a shadow of it, looks open while its window is (OS/2). */
        char p[PATH_MAX];
        if (r->shadow || r->link) snprintf(p, sizeof p, "%s", r->target ? r->target : "");
        else snprintf(p, sizeof p, "%s/%s", strcmp(f->path, "/") ? f->path : "", r->name);
        for (struct folder *o = folders; o && !out->in_use; o = o->next)
            out->in_use = !o->closing && !strcmp(o->path, p);
    }
    if (out->in_use) {
        /* open: its open icon (its own, else the theme's NAME-open for its
         * icon, a folder's folder-open), drawn without the in-use hatching;
         * none: the normal icon, hatched */
        char on[160] = "";
        if (r->icon_open) out->image = icons_object(f->icon_theme, r->icon_open, px);
        else if (r->icon && !strncmp(r->icon, "theme:", 6)) snprintf(on, sizeof on, "%s-open", r->icon);
        else if (!r->icon && r->kind == UI_KIND_FOLDER) snprintf(on, sizeof on, "theme:folder-open");
        if (!out->image && on[0]) out->image = icons_object(f->icon_theme, on, px);
        out->open_icon = out->image != NULL;
        out->hatch_over = r->open_hatch;   /* (hatched over it too, if it asks) */
    }
    if (!out->image) out->image = icons_object(f->icon_theme, r->icon, px);   /* its own icon, or WPS's (a theme's) */
    if (!out->image && r->kind == UI_KIND_PROGRAM) {
        /* a program (or its shadow): its desktop entry's icon */
        char p[PATH_MAX];
        if (r->shadow || r->link) snprintf(p, sizeof p, "%s", r->target ? r->target : "");
        else snprintf(p, sizeof p, "%s/%s", strcmp(f->path, "/") ? f->path : "", r->name);
        size_t l = strlen(p);
        if (l > 8 && !strcmp(p + l - 8, ".desktop")) out->image = appicon_for(p);
    }
    if (!out->image)   /* its kind's, from the folder's theme (a data file's by its type) */
        out->image = icons_item(f->icon_theme, NULL, r->kind, (r->shadow || r->link) && r->target ? r->target : r->name, px);
}

void src_visible(void *ctx, size_t first, size_t last)
{
    struct folder *f = ctx;
    if (!is_tree(f)) fetch_visible(f, first, last);
}

void src_expand(void *ctx, size_t i, bool expand)
{
    struct folder *f = ctx;
    if (is_tree(f) && i < f->ntvis) tree_expand(f, f->tvis[i], expand);
}

const char *row_name(struct folder *f, size_t i) { return model_name(&f->m, i); }

void target_of(struct folder *f, size_t i, char *out, size_t n)
{
    const char *cn = is_tree(f) || i == SIZE_MAX ? NULL : model_name(&f->m, i);
    if (cn && cn[0] == '\x01') { snprintf(out, n, "%s", cn + 1); return; }   /* a central object: vobj:<id> */
    if (is_tree(f)) {
        snprintf(out, n, "%s", i < f->ntvis ? f->tvis[i]->path : f->path);
        return;
    }
    const char *nm = i == SIZE_MAX ? NULL : row_name(f, i);
    if (nm) snprintf(out, n, "%s/%s", strcmp(f->path, "/") ? f->path : "", nm);
    else snprintf(out, n, "%s", f->path);
}

void ignore_reply(void *arg, const cv *reply) { (void)arg; (void)reply; }

static void confirmed(void *arg, int choice)
{
    struct confirm_ctx *c = arg;
    struct folder *f = folder_by_id(c->folder);
    if (choice == 0 && f) svc_open(c->target, true, opened, as_arg(f));
    free(c);
}

static void confirm_program(struct folder *f, const cv *res)
{
    const char *cmd = cv_text(res, "command");
    int64_t uid = -1;
    cv_int(res, "owner_uid", &uid);
    struct confirm_ctx *c = calloc(1, sizeof *c);
    c->folder = f->id;
    snprintf(c->target, sizeof c->target, "%s", f->pending_open);
    char msg[1400];
    snprintf(msg, sizeof msg, "This program belongs to another user (user ID %lld). It would run, as you:\n%s\nRun it?",
             (long long)uid, cmd ? cmd : "?");
    const char *buttons[] = { "Run", "Cancel" };
    ask("Run a program", msg, buttons, 2, 1, confirmed, c);
}

void opened(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *msg = reply ? cv_text(reply, "message") : "lost the connection to wpsd";
    if (res && cv_get(res, "needs_confirmation")) {
        confirm_program(f, res);
        return;
    }
    if (svc_reply_view(reply)) return;
    if (res && cv_text(res, "folder")) {   /* a shadow of a folder, or a link to one */
        open_folder(cv_text(res, "folder"), f->pending_view);
        f->error[0] = 0;
        damage_status(f);
        return;
    }
    else if (!res && msg && strstr(msg, "opens in the GUI") && f->pending_open[0]) {
        /* An abstract object with no view of its own (a built-in
         * template): its settings notebook. */
        open_properties(f->pending_open);
        f->error[0] = 0;
    } else if (!res && reply && cv_text(reply, "error") && !strcmp(cv_text(reply, "error"), "locked") && f->pending_open[0]) {
        unlock_ask(f, f->pending_open, true);   /* a locked drive: its passphrase, then open it */
        f->error[0] = 0;
    } else if (!res && msg) {
        snprintf(f->error, sizeof f->error, "%s", msg);
        raise_in(f, "alert.error", f->pending_open, "");
    } else {
        f->error[0] = 0;
        if (res && cv_get(res, "launched")) {
            const char *own = cv_text(res, "sound");   /* its own (on:object.open), if it has one */
            if (own) events_sound_once("object.open", own);
            raise_in(f, "object.open", f->pending_open, "file");
        }
    }
    damage_status(f);
}

struct row *row_at(struct folder *f, size_t i)
{
    if (is_tree(f) || i == SIZE_MAX || i / PAGE >= f->m.npages || !f->m.pages[i / PAGE]) return NULL;
    return &f->m.pages[i / PAGE]->rows[i % PAGE];
}

void open_item(struct folder *f, size_t i, ui_view view)
{
    struct row *rw = row_at(f, i);
    if (rw && (rw->shadow || rw->link)) {
        /* A shadow opens its original, a link what it points to; the
         * service finds them (a shadow by ID) and says if it is a folder. */
        char t[PATH_MAX];
        target_of(f, i, t, sizeof t);
        snprintf(f->pending_open, sizeof f->pending_open, "%s", t);
        f->pending_view = view;
        svc_open(t, false, opened, as_arg(f));
        return;
    }
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    struct page *pg = is_tree(f) || i == SIZE_MAX || i / PAGE >= f->m.npages ? NULL : f->m.pages[i / PAGE];
    bool is_folder = i == SIZE_MAX || is_tree(f) || (pg && pg->rows[i % PAGE].kind == UI_KIND_FOLDER);
    if (is_folder) open_folder(t, view);
    else {
        snprintf(f->pending_open, sizeof f->pending_open, "%s", t);
        svc_open(t, false, opened, as_arg(f));
    }
}

void src_activate(void *ctx, size_t i) { open_item(ctx, i, UI_VIEW_ICON); }

/* ---------------------------------------------------------- icon layout */

const char *lay_name(void *ctx, size_t i) { return model_name(&((struct folder *)ctx)->m, i); }

/* Item i's cell height: with its whole title (OS/2's layout), measured
 * once per title. */
double lay_h(void *ctx, size_t i)
{
    struct folder *f = ctx;
    const ui_icon_set *set = folder_icons(f);
    if (!f->whole_titles) return set->cell_h;
    struct row *r = row_at(f, i);
    if (!r || !r->name) return set->cell_h;
    if (r->cellh <= 0) r->cellh = (float)ui_icon_cell_height_set(f->font, set, row_shown(f, r));
    return r->cellh;
}

int cols_now(struct folder *f)
{
    int k = (int)(ui_container_list_width(f->cont) / folder_icons(f)->cell_w);
    return k < 1 ? 1 : k;
}

const ui_icon_set *folder_icons(struct folder *f) { return f->cont ? ui_container_icon_set(f->cont) : ui_icons(); }

#define POS_W 100.0   /* (the Small set's cell: the stored units) */
#define POS_H 80.0

void folder_pos_stored(struct folder *f, int64_t *x, int64_t *y)
{
    const ui_icon_set *set = folder_icons(f);
    *x = llround((double)*x * POS_W / set->cell_w);
    *y = llround((double)*y * POS_H / set->cell_h);
}

void folder_place(struct folder *f, const char *dest, const char *const *targets, const int64_t *x, const int64_t *y, size_t n, svc_fn fn, void *arg)
{
    const ui_icon_set *set = folder_icons(f);
    int64_t *sx = calloc(n ? n : 1, sizeof *sx), *sy = calloc(n ? n : 1, sizeof *sy);
    for (size_t i = 0; i < n; i++) {
        sx[i] = llround((double)x[i] * POS_W / set->cell_w);
        sy[i] = llround((double)y[i] * POS_H / set->cell_h);
    }
    svc_place(dest, targets, sx, sy, n, fn, arg);
    free(sx);
    free(sy);
}

void folder_place_lock(struct folder *f, const char *target, int64_t x, int64_t y, bool lock, svc_fn fn, void *arg)
{
    const ui_icon_set *set = folder_icons(f);
    svc_place_lock(f->path, target, llround((double)x * POS_W / set->cell_w), llround((double)y * POS_H / set->cell_h), lock, fn, arg);
}

void folder_pos_shown(struct folder *f, int64_t *x, int64_t *y)
{
    const ui_icon_set *set = folder_icons(f);
    *x = llround((double)*x * set->cell_w / POS_W);
    *y = llround((double)*y * set->cell_h / POS_H);
    if (f->desktop) {   /* (it does not scroll: kept on it, at a larger set too) */
        ui_rect c = ui_frame_client(f->win);
        if (c.w > set->cell_w && *x > c.w - set->cell_w - 8) *x = (int64_t)(c.w - set->cell_w - 8);
        if (c.h > set->cell_h && *y > c.h - set->cell_h - 8) *y = (int64_t)(c.h - set->cell_h - 8);
    }
}

void folder_icons_changed(struct folder *f)
{
    const ui_icon_set *set = folder_icons(f);
    for (size_t p = 0; p < f->m.npages; p++)
        for (size_t k = 0; f->m.pages[p] && k < PAGE; k++) f->m.pages[p]->rows[k].cellh = 0;
    bool wide = ui_container_list_width(f->cont) >= set->cell_w;   /* (not laid out yet: its columns as they are) */
    layout_set_cell(f->lay, set->cell_w, set->cell_h, wide ? cols_now(f) : layout_cols(f->lay));
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
    ui_container_changed(f->cont, 0, SIZE_MAX);
    if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
    if (!is_tree(f)) svc_layout(f->path, got_layout, as_arg(f));   /* stored positions in the new size */
}

/* Names in name order (for Arrange). */
static struct folder *sort_folder;

static int cmp_index_name(const void *a, const void *b)
{
    const char *x = model_name(&sort_folder->m, *(const size_t *)a), *y = model_name(&sort_folder->m, *(const size_t *)b);
    return strcoll(x ? x : "", y ? y : "");
}

/* Sort: by type (the extension; folders first), size, last write - each
 * then by name. */
static int sort_key;

static int cmp_index_key(const void *a, const void *b)
{
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    struct row *x = row_at(sort_folder, i), *y = row_at(sort_folder, j);
    const char *nx = model_name(&sort_folder->m, i), *ny = model_name(&sort_folder->m, j);
    nx = nx ? nx : "";
    ny = ny ? ny : "";
    int r = 0;
    if (x && y) {
        if (sort_key == 1) {
            bool fx = x->kind == UI_KIND_FOLDER, fy = y->kind == UI_KIND_FOLDER;
            if (fx != fy) r = fx ? -1 : 1;
            else {
                const char *ex = strrchr(nx, '.'), *ey = strrchr(ny, '.');
                r = strcasecmp(ex && ex != nx ? ex : "", ey && ey != ny ? ey : "");
            }
        } else if (sort_key == 2) r = x->size < y->size ? -1 : x->size > y->size;
        else if (sort_key == 3) r = x->mtime > y->mtime ? -1 : x->mtime < y->mtime;   /* newest first */
    }
    return r ? r : strcoll(nx, ny);
}

static size_t *name_order(struct folder *f)
{
    size_t n = f->m.total, *o = malloc((n ? n : 1) * sizeof *o);
    for (size_t i = 0; i < n; i++) o[i] = i;
    sort_folder = f;
    qsort(o, n, sizeof *o, cmp_index_name);
    return o;
}

/* New rows from the service (or a re-read): extend or redo the layout. */
void layout_follow(struct folder *f, bool full)
{
    if (!f->lay_started) { layout_set_cols(f->lay, cols_now(f)); f->lay_started = true; }
    /* A small folder with no stored positions opens arranged by name (not
     * in the order the directory happens to list), once it is all read. */
    if (f->m.complete && !f->arranged_by_name && f->m.total <= 100000) {
        f->arranged_by_name = true;
        layout_reset(f->lay, cols_now(f));
        size_t *o = name_order(f);
        layout_update(f->lay, f->m.total, lay_name, f, true, o);
        free(o);
        svc_layout(f->path, got_layout, as_arg(f));   /* stored positions win over it */
        layout_on_new_spot(f->lay, new_spot, f);   /* from now on, new ones' spots are kept */
    } else {
        layout_update(f->lay, f->m.total, lay_name, f, full, NULL);
    }
    if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
}

/* New objects' spots, stored a moment after they came (see layout_follow). */
static int spot_timer = -1;

static void store_spots(struct folder *f)
{
    /* every icon not placed yet, with its spot now (the new one with the
     * others: stored and unstored mixed overlapped at the next login, the
     * unstored ones arranged by name over the stored) */
    for (size_t j = 0; j < f->nspot_new; j++) free(f->spot_new[j]);
    free(f->spot_new);
    f->spot_new = NULL;
    f->nspot_new = 0;
    size_t n = f->m.total;
    if (!f->m.complete || !n || n > 2000) return;
    char **t = calloc(n, sizeof *t);
    int64_t *x = calloc(n, sizeof *x), *y = calloc(n, sizeof *y);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *nm = model_name(&f->m, i);
        if (!nm || layout_is_placed(f->lay, nm)) continue;
        double px, py;
        layout_pos(f->lay, i, &px, &py);
        if (nm[0] == '\x01') t[k] = strdup(nm + 1);   /* a central object: its address */
        else if (asprintf(&t[k], "%s/%s", strcmp(f->path, "/") ? f->path : "", nm) < 0) continue;
        x[k] = (int64_t)px;
        y[k] = (int64_t)py;
        layout_place(f->lay, nm, (int)px, (int)py);
        k++;
    }
    if (k) folder_place(f, f->path, (const char *const *)t, x, y, k, placed_reply, as_arg(f));
    for (size_t i = 0; i < k; i++) free(t[i]);
    free(t);
    free(x);
    free(y);
}

static void on_spot_timer(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t n;
    if (read(fd, &n, sizeof n) < 0) { /* nothing */ }
    for (struct folder *f = folders; f; f = f->next)
        if (!f->closing && f->nspot_new) store_spots(f);
}

/* An object that comes after the first arrangement (made, copied, moved
 * in) takes the next free spot; that spot is stored a moment later (a
 * drop's own position, stored as its job ends, comes first), so it is
 * there at the next login - unstored, the folder arranged by name then
 * put it elsewhere, and the order changed. */
static void new_spot(void *ctx, const char *name)
{
    struct folder *f = ctx;
    if (!f->arranged_by_name || f->nspot_new >= 64) return;
    if (f->filter[0]) return;   /* (flowed while filtered: not spots to keep) */
    f->spot_new = realloc(f->spot_new, (f->nspot_new + 1) * sizeof *f->spot_new);
    f->spot_new[f->nspot_new++] = strdup(name);
    spots_later();
}

static void spots_later(void)
{
    if (spot_timer < 0) {
        spot_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (spot_timer < 0) return;
        ui_app_watch_fd(app, spot_timer, POLLIN, on_spot_timer, NULL);
    }
    struct itimerspec its = { .it_value = { 2, 0 } };   /* (each new one starts the 2 s again) */
    timerfd_settime(spot_timer, 0, &its, NULL);
}

void got_layout(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    if (!f || !items || items->k != CV_ARRAY) return;
    if (f->filter[0]) return;   /* filtered: the matches flow, stored positions unused */
    /* where the unplaced locked ones are now (locked in the notebook) */
    size_t nl = 0;
    char **lname = NULL;
    for (size_t i = 0; i < items->n; i++) {
        const cv *lk = cv_get(&items->items[i], "locked");
        const char *nm = cv_text(&items->items[i], "name");
        if (nm && lk && lk->k == CV_BOOL && lk->b && !cv_get(&items->items[i], "x")) {
            lname = realloc(lname, (nl + 1) * sizeof *lname);
            lname[nl++] = strdup(nm);
        }
    }
    double *lx = calloc(nl ? nl : 1, sizeof *lx), *ly = calloc(nl ? nl : 1, sizeof *ly);
    for (size_t k = 0; k < nl; k++)
        for (size_t i = 0; i < f->m.total; i++) {
            const char *nm = model_name(&f->m, i);
            if (nm && !strcmp(nm, lname[k])) { layout_pos(f->lay, i, &lx[k], &ly[k]); break; }
        }
    layout_clear_placed(f->lay);
    for (size_t i = 0; i < items->n; i++) {
        const char *nm = cv_text(&items->items[i], "name");
        const cv *lk = cv_get(&items->items[i], "locked");
        int64_t x = 0, y = 0;
        bool has = cv_int(&items->items[i], "x", &x);
        cv_int(&items->items[i], "y", &y);
        folder_pos_shown(f, &x, &y);
        if (nm && has) layout_place(f->lay, nm, (int)x, (int)y);
        if (nm && lk && lk->k == CV_BOOL && lk->b) layout_lock(f->lay, nm, true);
    }
    for (size_t k = 0; k < nl; k++) {   /* their places now, stored (still locked) */
        char addr[PATH_MAX];
        if (lname[k][0] == '\x01') snprintf(addr, sizeof addr, "%s", lname[k] + 1);
        else snprintf(addr, sizeof addr, "%s/%s", strcmp(f->path, "/") ? f->path : "", lname[k]);
        layout_place(f->lay, lname[k], (int)lx[k], (int)ly[k]);
        folder_place_lock(f, addr, (int64_t)lx[k], (int64_t)ly[k], true, ignore_reply, NULL);
        free(lname[k]);
    }
    free(lname);
    free(lx);
    free(ly);
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
    if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
}

void placed_reply(void *arg, const cv *reply)
{
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b)) {
        char m[300];
        snprintf(m, sizeof m, "Positions not kept: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        struct folder *f = from_arg(arg);
        if (f) { snprintf(f->error, sizeof f->error, "%s", m); damage_status(f); }
        return;
    }
    /* Stored: read the positions again, so an earlier answer (asked when the
     * new item appeared, before this one was kept) does not have the last word. */
    struct folder *f = from_arg(arg);
    if (f) svc_layout(f->path, got_layout, as_arg(f));
}

/* Arrange (folder menu): a grid that fits the window, in name order;
 * stored for folders up to 2000 objects (larger ones are arranged for
 * this session). */

void arrange(struct folder *f) { arrange_by(f, 0); }

/* Sort (icon view, as OS/2): the icons arranged in an order, once. Size
 * and date need every row's details: they are read first. */
void sort_icons(struct folder *f, int key)
{
    if (key >= 2) {
        bool all = true;
        for (size_t p = 0; p * PAGE < f->m.total; p++) {
            struct page *pg = page_at(&f->m, p);
            if (pg->state == P_DETAILED) continue;
            all = false;
            if (pg->requested) continue;
            pg->requested = true;
            svc_list_v(f->path, f->filter, f->show_hidden, p * PAGE, PAGE, true, got_page, page_arg(f, false));
        }
        if (!all || !f->m.complete) {
            f->sort_pending = key + 1;
            snprintf(f->error, sizeof f->error, "Sorting...");
            damage_status(f);
            return;
        }
    }
    f->sort_pending = 0;
    arrange_by(f, key);
}

static void arrange_by(struct folder *f, int key)
{
    if (f->lock_all) {   /* (nothing would move) */
        snprintf(f->error, sizeof f->error, "Positions are locked (the folder's Icon view page)");
        damage_status(f);
        return;
    }
    svc_arrange(f->path, placed_reply, as_arg(f));
    layout_reset(f->lay, cols_now(f));
    size_t *o = name_order(f);
    if (key) {
        sort_key = key;
        sort_folder = f;
        qsort(o, f->m.total, sizeof *o, cmp_index_key);
    }
    layout_update(f->lay, f->m.total, lay_name, f, true, o);
    free(o);
    f->arranged_by_name = true;
    size_t n = f->m.total;
    if (n && n <= 2000) {
        char **t = calloc(n, sizeof *t);
        int64_t *x = calloc(n, sizeof *x), *y = calloc(n, sizeof *y);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            const char *nm = model_name(&f->m, i);
            if (!nm) continue;
            double px, py;
            layout_pos(f->lay, i, &px, &py);
            t[k] = malloc(strlen(f->path) + strlen(nm) + 2);
            sprintf(t[k], "%s/%s", strcmp(f->path, "/") ? f->path : "", nm);
            x[k] = (int64_t)px;
            y[k] = (int64_t)py;
            layout_place(f->lay, nm, (int)px, (int)py);
            k++;
        }
        folder_place(f, f->path, (const char *const *)t, x, y, k, placed_reply, as_arg(f));
        for (size_t i = 0; i < k; i++) free(t[i]);
        free(t);
        free(x);
        free(y);
    }
    ui_container_layout_changed(f->cont);
    static const char *const by[] = { "Arranged", "Sorted by type", "Sorted by size", "Sorted by last write" };
    snprintf(f->error, sizeof f->error, "%s", by[key >= 0 && key < 4 ? key : 0]);
    damage_status(f);
}
