/*
 * wps-shell: the WPS GUI (G1: folder windows). One process draws every
 * WPS window (gui-design.md: Processes); the object service is wpsd.
 *
 * usage: wps-shell [--details] [--bench SECONDS] FOLDER...
 *   --bench: scroll the first window by itself, print frame statistics as
 *   JSON, and exit (WPS_UI_SCALE and WPS_SHELL_SIZE=WxH help measure).
 */
#define _GNU_SOURCE
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "layout.h"
#include "svc.h"
#include "ui.h"
#include "wps_menu_ids.h"

#define PAGE 256
#define STATUS_H 20

enum { P_NONE, P_NAMES, P_DETAILED };

struct row { char *name, *title; ui_kind kind; int64_t size, mtime; bool shadow; };
struct page { int state; bool requested; struct row rows[PAGE]; };

/* A folder's contents as read from the service, a page at a time. */
struct model {
    struct page **pages;
    size_t npages, total;
    bool complete, growing;
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
    size_t vis_first, vis_last;
    struct tnode *troot;     /* tree view */
    struct tnode **tvis;     /* its visible rows */
    size_t ntvis, captvis;
    unsigned tgen;           /* replies for a freed tree are dropped */
    char pending_open[PATH_MAX];   /* the program asked about, if any */
    icon_layout *lay;        /* icon view: where each icon is */
    bool lay_started;        /* its grid width is set */
    bool arranged_by_name;   /* the first, unstored arrangement was done */
    char error[256];
    /* benchmark */
    double bench_secs, bench_start, bench_next_jump;
    unsigned bench_seed;
    size_t bench_from;
    size_t frames_seen, verified, verify_failed, verify_diff;
};

static ui_app *app;
static struct folder *folders;
static long next_folder_id = 1;
static double opt_bench;
static ui_view opt_view = UI_VIEW_ICON;

static struct folder *folder_by_id(long id)
{
    for (struct folder *f = folders; f; f = f->next)
        if (f->id == id) return f;
    return NULL;
}

static void *as_arg(struct folder *f) { return (void *)(intptr_t)f->id; }
static struct folder *from_arg(void *arg) { return folder_by_id((long)(intptr_t)arg); }

static struct folder *open_folder(const char *path, ui_view view);
static bool tree_event(struct folder *f, int64_t sub);
static void on_job_event(const cv *st);
static const char *lay_name(void *ctx, size_t i);
static void got_layout(void *arg, const cv *reply);
static void layout_follow(struct folder *f, bool full);
static void set_view(struct folder *f, ui_view v);

/* ------------------------------------------------------------------ model */

static struct page *page_at(struct model *m, size_t p)
{
    if (p >= m->npages) {
        size_t n = m->npages ? m->npages : 64;
        while (n <= p) n *= 2;
        m->pages = realloc(m->pages, n * sizeof *m->pages);
        memset(m->pages + m->npages, 0, (n - m->npages) * sizeof *m->pages);
        m->npages = n;
    }
    if (!m->pages[p]) m->pages[p] = calloc(1, sizeof **m->pages);
    return m->pages[p];
}

static void model_free(struct model *m)
{
    for (size_t p = 0; p < m->npages; p++) {
        if (!m->pages[p]) continue;
        for (size_t r = 0; r < PAGE; r++) { free(m->pages[p]->rows[r].name); free(m->pages[p]->rows[r].title); }
        free(m->pages[p]);
    }
    free(m->pages);
    memset(m, 0, sizeof *m);
}

static const char *model_name(struct model *m, size_t i)
{
    struct page *pg = i / PAGE < m->npages ? m->pages[i / PAGE] : NULL;
    return pg && pg->rows[i % PAGE].name ? pg->rows[i % PAGE].name : NULL;
}

static ui_kind kind_of(const char *type, const char *name)
{
    if (type && !strcmp(type, "folder")) return UI_KIND_FOLDER;
    if (type && !strcmp(type, "link")) return UI_KIND_LINK;
    size_t l = name ? strlen(name) : 0;
    if (l > 8 && !strcmp(name + l - 8, ".desktop")) return UI_KIND_PROGRAM;
    return UI_KIND_FILE;
}

static const char *view_name(ui_view v) { return v == UI_VIEW_ICON ? "Icon View" : v == UI_VIEW_TREE ? "Tree View" : "Details View"; }

static void update_title(struct folder *f)
{
    char t[400];
    snprintf(t, sizeof t, "%s - %s", f->title, view_name(ui_container_view(f->cont)));
    ui_window_set_title(f->win, t);
}

static void damage_status(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    ui_surface_damage(f->win, (ui_rect){ c.x, c.y + c.h - STATUS_H, c.w, STATUS_H });
}

/* Which model and generation a reply belongs to. */
struct pctx { long id; unsigned gen; bool shadow; };

static void *page_arg(struct folder *f, bool shadow)
{
    struct pctx *pc = malloc(sizeof *pc);
    pc->id = f->id;
    pc->gen = shadow ? f->sgen : f->gen;
    pc->shadow = shadow;
    return pc;
}

static void request_more(struct folder *f, bool shadow);
static void fetch_visible(struct folder *f, size_t first, size_t last);
static void reload(struct folder *f);
static void got_page(void *arg, const cv *reply);

/* Store a reply's rows; returns whether they had details. */
static bool store(struct model *m, const cv *res, size_t *off_out, size_t *n_out)
{
    int64_t off = 0, tot = 0;
    cv_int(res, "offset", &off);
    cv_int(res, "total", &tot);
    const cv *cmp = cv_get(res, "complete"), *items = cv_get(res, "items");
    size_t n = items && items->k == CV_ARRAY ? items->n : 0;
    bool details = false;
    for (size_t i = 0; i < n; i++) {
        const cv *it = &items->items[i];
        size_t r = (size_t)off + i;
        struct row *dst = &page_at(m, r / PAGE)->rows[r % PAGE];
        const char *nm = cv_text(it, "name"), *ti = cv_text(it, "title"), *ty = cv_text(it, "type");
        free(dst->name);
        free(dst->title);
        dst->name = strdup(nm ? nm : "?");
        dst->title = ti && *ti ? strdup(ti) : NULL;
        dst->kind = kind_of(ty, nm);
        if (cv_get(it, "mtime")) {
            details = true;
            cv_int(it, "size", &dst->size);
            cv_int(it, "mtime", &dst->mtime);
            const cv *sh = cv_get(it, "shadow");
            dst->shadow = sh && sh->k == CV_BOOL && sh->b;
            const char *tt = cv_text(it, "target");
            if (dst->shadow) dst->kind = tt && !strcmp(tt, "folder") ? UI_KIND_FOLDER : kind_of("file", nm);
        }
    }
    for (size_t p = (size_t)off / PAGE; n && p <= ((size_t)off + n - 1) / PAGE; p++) {
        struct page *pg = page_at(m, p);
        if (details) { pg->state = P_DETAILED; pg->requested = false; }
        else if (pg->state == P_NONE) pg->state = P_NAMES;
    }
    if ((size_t)tot > m->total) m->total = (size_t)tot;
    m->complete = cmp && cmp->k == CV_BOOL && cmp->b;
    if (!details) m->growing = false;
    *off_out = (size_t)off;
    *n_out = n;
    return details;
}

/* The re-read model replaces the one on screen, keeping the scroll
 * position, the selection and the cursor (by name). */
static void swap(struct folder *f)
{
    struct model *old = &f->m, *nm = f->sh;
    size_t nsel = ui_container_selected_count(f->cont), cur = ui_container_cursor(f->cont);
    char **sel = NULL, *curname = NULL;
    size_t k = 0;
    if (nsel && nsel <= 256) {
        sel = calloc(nsel, sizeof *sel);
        for (size_t i = 0; i < old->total && k < nsel; i++)
            if (ui_container_is_selected(f->cont, i) && model_name(old, i)) sel[k++] = strdup(model_name(old, i));
    }
    if (cur != SIZE_MAX && model_name(old, cur)) curname = strdup(model_name(old, cur));
    size_t ren = ui_container_rename_item(f->cont);
    char *renname = ren != SIZE_MAX && model_name(old, ren) ? strdup(model_name(old, ren)) : NULL;
    size_t ren_new = SIZE_MAX;
    model_free(old);
    f->m = *nm;
    free(nm);
    f->sh = NULL;
    f->gen++;
    ui_container_select_none(f->cont);
    ui_container_reset(f->cont);
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);   /* spots stay with names */
    svc_layout(f->path, got_layout, as_arg(f));                     /* positions may have changed too */
    if (k || curname || renname)
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (!n) continue;
            for (size_t j = 0; j < k; j++)
                if (sel[j] && !strcmp(sel[j], n)) { ui_container_select(f->cont, i); free(sel[j]); sel[j] = NULL; }
            if (curname && !strcmp(curname, n)) { ui_container_set_cursor(f->cont, i); free(curname); curname = NULL; }
            if (renname && !strcmp(renname, n)) ren_new = i;
        }
    else if (renname)
        for (size_t i = 0; i < f->m.total && ren_new == SIZE_MAX; i++)
            if (model_name(&f->m, i) && !strcmp(renname, model_name(&f->m, i))) ren_new = i;
    if (ren != SIZE_MAX) ui_container_rename_moved(f->cont, ren_new);   /* gone: cancelled */
    free(renname);
    for (size_t j = 0; j < k; j++) free(sel[j]);
    free(sel);
    free(curname);
    damage_status(f);
    if (f->reload_again) {
        f->reload_again = false;
        reload(f);
    }
}

static void got_page(void *arg, const cv *reply)
{
    struct pctx *pc = arg;
    struct folder *f = folder_by_id(pc->id);
    bool shadow = pc->shadow;
    unsigned gen = pc->gen;
    free(pc);
    if (!f) return;
    struct model *m = shadow ? f->sh : &f->m;
    if (!m || gen != (shadow ? f->sgen : f->gen)) return;   /* for a model replaced since */
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res) {
        const char *msg = reply ? cv_text(reply, "message") : NULL;   /* {ok:false, error, message} */
        snprintf(f->error, sizeof f->error, "%s", msg ? msg : reply ? "the folder cannot be read" : "lost the connection to wpsd");
        m->growing = false;
        if (shadow) { model_free(f->sh); free(f->sh); f->sh = NULL; }
        damage_status(f);
        return;
    }
    size_t off, n;
    bool was_complete = m->complete;
    bool details = store(m, res, &off, &n);
    if (shadow) {
        if (details) { if (--m->pending == 0) swap(f); return; }
        if (!m->complete) { request_more(f, true); return; }
        /* All names are in: fetch details for what is on screen, then swap. */
        size_t p0 = f->vis_first / PAGE, p1 = f->vis_last / PAGE + 1;
        for (size_t p = p0; p <= p1 && p * PAGE < m->total; p++) {
            m->pending++;
            page_at(m, p)->requested = true;
            svc_list(f->path, p * PAGE, PAGE, true, got_page, page_arg(f, true));
        }
        if (!m->pending) swap(f);
        return;
    }
    f->error[0] = 0;
    ui_container_count_changed(f->cont);
    if (n) ui_container_changed(f->cont, off, off + n - 1);
    if (!details || m->complete != was_complete) layout_follow(f, false);
    if (!m->complete) request_more(f, false);
    if (m->complete != was_complete || !details) damage_status(f);
}

/* Names stream in; the folder's size grows as the service reads it. */
static void request_more(struct folder *f, bool shadow)
{
    struct model *m = shadow ? f->sh : &f->m;
    if (!m || m->growing || m->complete) return;
    m->growing = true;
    svc_list(f->path, m->total, m->total ? 4096 : 128, false, got_page, page_arg(f, shadow));
}

/* Details (size, date, title) for the pages on screen. */
static void fetch_visible(struct folder *f, size_t first, size_t last)
{
    f->vis_first = first;
    f->vis_last = last;
    size_t p0 = first / PAGE, p1 = last / PAGE + 1;
    for (size_t p = p0; p <= p1 && p * PAGE < f->m.total; p++) {
        struct page *pg = page_at(&f->m, p);
        if (pg->state == P_DETAILED || pg->requested) continue;
        pg->requested = true;
        svc_list(f->path, p * PAGE, PAGE, true, got_page, page_arg(f, false));
    }
}

/* The folder changed: read it again in the background. Changes during the
 * re-read start one more when it is done, so bursts cost two reads. */
static void reload(struct folder *f)
{
    if (f->sh) { f->reload_again = true; return; }
    f->sh = calloc(1, sizeof *f->sh);
    f->sgen++;
    request_more(f, true);
}

static void on_event(void *arg, const cv *ev)
{
    (void)arg;
    int64_t sub;
    const char *name = ev ? cv_text(ev, "event") : NULL;
    if (!name || !cv_int(ev, "sub", &sub)) return;
    if (!strcmp(name, "job")) { on_job_event(cv_get(ev, "status")); return; }
    for (struct folder *f = folders; f; f = f->next) {
        if (!f->closing && f->troot && tree_event(f, sub)) continue;
        if (f->sub == sub && !f->closing &&
            (!strcmp(name, "content_added") || !strcmp(name, "content_removed") || !strcmp(name, "object_moved") ||
             !strcmp(name, "object_changed")))
            reload(f);
    }
}

static void got_sub(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    int64_t sub;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!f || !res || res->k != CV_INT) return;
    sub = res->i;
    if (f->closing) { svc_unsubscribe(sub); return; }
    f->sub = sub;
}


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

static void tnode_free(struct tnode *n)
{
    if (!n) return;
    if (n->sub) svc_unsubscribe(n->sub);
    for (size_t i = 0; i < n->nkids; i++) tnode_free(n->kids[i]);
    for (size_t i = 0; i < n->npending; i++) free(n->pending[i]);
    free(n->pending);
    free(n->kids);
    free(n->name);
    free(n->path);
    free(n);
}

static struct tnode *tnode_new(struct tnode *parent, const char *name, const char *path)
{
    struct tnode *n = calloc(1, sizeof *n);
    n->parent = parent;
    n->name = strdup(name);
    n->path = strdup(path);
    n->depth = parent ? parent->depth + 1 : 0;
    return n;
}

static void tree_flatten(struct folder *f, struct tnode *n)
{
    if (f->ntvis == f->captvis) {
        f->captvis = f->captvis ? f->captvis * 2 : 64;
        f->tvis = realloc(f->tvis, f->captvis * sizeof *f->tvis);
    }
    f->tvis[f->ntvis++] = n;
    if (n->expanded)
        for (size_t i = 0; i < n->nkids; i++) tree_flatten(f, n->kids[i]);
}

/* Rebuild the visible rows, keeping the cursor on the same node. */
static void tree_rebuild(struct folder *f, struct tnode *keep)
{
    f->ntvis = 0;
    if (f->troot) tree_flatten(f, f->troot);
    if (ui_container_view(f->cont) != UI_VIEW_TREE) return;
    ui_container_select_none(f->cont);
    ui_container_reset(f->cont);
    for (size_t i = 0; keep && i < f->ntvis; i++)
        if (f->tvis[i] == keep) { ui_container_set_cursor(f->cont, i); ui_container_select(f->cont, i); }
    damage_status(f);
}

static int cmp_names(const void *a, const void *b) { return strcoll(*(char *const *)a, *(char *const *)b); }

struct tctx { long folder; unsigned tgen; struct tnode *n; size_t offset; };

static void tree_load(struct folder *f, struct tnode *n, size_t offset);

static void got_kids(void *arg, const cv *reply)
{
    struct tctx *t = arg;
    struct folder *f = folder_by_id(t->folder);
    struct tnode *n = t->n;
    size_t offset = t->offset;
    bool valid = f && t->tgen == f->tgen;
    free(t);
    if (!valid) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const cv *items = res ? cv_get(res, "items") : NULL, *cmp = res ? cv_get(res, "complete") : NULL;
    size_t cnt = items && items->k == CV_ARRAY ? items->n : 0;
    for (size_t i = 0; i < cnt; i++) {
        const char *ty = cv_text(&items->items[i], "type"), *nm = cv_text(&items->items[i], "name");
        if (!ty || strcmp(ty, "folder") || !nm) continue;
        if (n->npending == n->cappending) {
            n->cappending = n->cappending ? n->cappending * 2 : 16;
            n->pending = realloc(n->pending, n->cappending * sizeof *n->pending);
        }
        n->pending[n->npending++] = strdup(nm);
    }
    if (res && !(cmp && cmp->k == CV_BOOL && cmp->b)) { tree_load(f, n, offset + cnt); return; }
    /* All read: the new subfolders, keeping nodes (and their expansion)
     * that are still there. */
    qsort(n->pending, n->npending, sizeof *n->pending, cmp_names);
    struct tnode **kids = calloc(n->npending ? n->npending : 1, sizeof *kids);
    for (size_t i = 0; i < n->npending; i++) {
        struct tnode *old = NULL;
        for (size_t k = 0; k < n->nkids && !old; k++)
            if (n->kids[k] && !strcmp(n->kids[k]->name, n->pending[i])) { old = n->kids[k]; n->kids[k] = NULL; }
        if (!old) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", strcmp(n->path, "/") ? n->path : "", n->pending[i]);
            old = tnode_new(n, n->pending[i], p);
        }
        kids[i] = old;
        free(n->pending[i]);
    }
    for (size_t k = 0; k < n->nkids; k++) tnode_free(n->kids[k]);   /* gone */
    free(n->kids);
    n->kids = kids;
    n->nkids = n->npending;
    n->npending = 0;
    n->loaded = true;
    n->loading = false;
    if (!reply) snprintf(f->error, sizeof f->error, "lost the connection to wpsd");
    struct tnode *cur = NULL;
    size_t ci = ui_container_cursor(f->cont);
    if (ci != SIZE_MAX && ci < f->ntvis) cur = f->tvis[ci];
    tree_rebuild(f, cur);
}

static void tree_load(struct folder *f, struct tnode *n, size_t offset)
{
    struct tctx *t = malloc(sizeof *t);
    t->folder = f->id;
    t->tgen = f->tgen;
    t->n = n;
    t->offset = offset;
    n->loading = true;
    svc_list(n->path, offset, 4096, false, got_kids, t);
}

struct tsub { long folder; unsigned tgen; struct tnode *n; };

static void got_tree_sub(void *arg, const cv *reply)
{
    struct tsub *t = arg;
    struct folder *f = folder_by_id(t->folder);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (f && t->tgen == f->tgen && res && res->k == CV_INT) {
        if (t->n->expanded && !t->n->sub) t->n->sub = res->i;
        else svc_unsubscribe(res->i);   /* collapsed meanwhile */
    }
    free(t);
}

static void tree_expand(struct folder *f, struct tnode *n, bool expand)
{
    if (expand == n->expanded) return;
    n->expanded = expand;
    if (expand) {
        if (!n->loaded && !n->loading) tree_load(f, n, 0);
        if (!n->sub && n != f->troot) {   /* the root is the window's own subscription */
            struct tsub *t = malloc(sizeof *t);
            t->folder = f->id;
            t->tgen = f->tgen;
            t->n = n;
            svc_subscribe_folder(n->path, got_tree_sub, t);
        }
    } else if (n->sub) {
        svc_unsubscribe(n->sub);
        n->sub = 0;
    }
    tree_rebuild(f, n);
}

/* A watched folder changed: read its subfolders again. */
static bool tree_event(struct folder *f, int64_t sub)
{
    struct tnode *stack[256];
    int sp = 0;
    if (f->troot) stack[sp++] = f->troot;
    while (sp) {
        struct tnode *n = stack[--sp];
        if (n->sub == sub || (n == f->troot && sub == f->sub)) {
            if (n->loaded && !n->loading) { n->npending = 0; tree_load(f, n, 0); }
            if (n != f->troot) return true;
        }
        for (size_t i = 0; i < n->nkids && sp < 256; i++)
            if (n->kids[i]->expanded) stack[sp++] = n->kids[i];
    }
    return false;
}

static void tree_start(struct folder *f)
{
    if (f->troot) return;
    f->tgen++;
    const char *base = strrchr(f->path, '/');
    f->troot = tnode_new(NULL, base && base[1] ? base + 1 : f->path, f->path);
    tree_expand(f, f->troot, true);
}

static void tree_free(struct folder *f)
{
    tnode_free(f->troot);
    f->troot = NULL;
    free(f->tvis);
    f->tvis = NULL;
    f->ntvis = f->captvis = 0;
    f->tgen++;
}

static bool is_tree(struct folder *f) { return ui_container_view(f->cont) == UI_VIEW_TREE; }

/* ----------------------------------------------------------- container src */
static void src_item(void *ctx, size_t i, ui_item *out);
static void target_of(struct folder *f, size_t i, char *out, size_t n);

static size_t src_count(void *ctx)
{
    struct folder *f = ctx;
    return is_tree(f) ? f->ntvis : f->m.total;
}

static void src_item(void *ctx, size_t i, ui_item *out)
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
    out->title = r->title ? r->title : r->name;
    out->size = r->size;
    out->mtime = r->mtime;
    out->shadow = r->shadow;
}

static void src_visible(void *ctx, size_t first, size_t last)
{
    struct folder *f = ctx;
    if (!is_tree(f)) fetch_visible(f, first, last);
}

static void src_expand(void *ctx, size_t i, bool expand)
{
    struct folder *f = ctx;
    if (is_tree(f) && i < f->ntvis) tree_expand(f, f->tvis[i], expand);
}

static const char *row_name(struct folder *f, size_t i) { return model_name(&f->m, i); }

static void target_of(struct folder *f, size_t i, char *out, size_t n)
{
    if (is_tree(f)) {
        snprintf(out, n, "%s", i < f->ntvis ? f->tvis[i]->path : f->path);
        return;
    }
    const char *nm = i == SIZE_MAX ? NULL : row_name(f, i);
    if (nm) snprintf(out, n, "%s/%s", strcmp(f->path, "/") ? f->path : "", nm);
    else snprintf(out, n, "%s", f->path);
}

static void ignore_reply(void *arg, const cv *reply) { (void)arg; (void)reply; }
static void opened(void *arg, const cv *reply);

/* A program another user owns: show its command and ask (the service
 * never runs it unconfirmed). */
struct confirm_ctx { long folder; char target[PATH_MAX]; };

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
    ui_dialog_open(app, "Run a program", msg, buttons, 2, 1, confirmed, c);
}

static void opened(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *msg = reply ? cv_text(reply, "message") : "lost the connection to wpsd";
    if (res && cv_get(res, "needs_confirmation")) {
        confirm_program(f, res);
        return;
    }
    else if (!res && msg)
        snprintf(f->error, sizeof f->error, "%s", msg);
    else
        f->error[0] = 0;
    damage_status(f);
}

static void open_item(struct folder *f, size_t i, ui_view view)
{
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

static void src_activate(void *ctx, size_t i) { open_item(ctx, i, UI_VIEW_ICON); }



/* ---------------------------------------------------------- icon layout */

static const char *lay_name(void *ctx, size_t i) { return model_name(&((struct folder *)ctx)->m, i); }

static int cols_now(struct folder *f)
{
    int k = (int)(ui_container_list_width(f->cont) / UI_CELL_W);
    return k < 1 ? 1 : k;
}

/* Names in name order (for Arrange). */
static struct folder *sort_folder;
static int cmp_index_name(const void *a, const void *b)
{
    const char *x = model_name(&sort_folder->m, *(const size_t *)a), *y = model_name(&sort_folder->m, *(const size_t *)b);
    return strcoll(x ? x : "", y ? y : "");
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
static void layout_follow(struct folder *f, bool full)
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
    } else {
        layout_update(f->lay, f->m.total, lay_name, f, full, NULL);
    }
    if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
}

static void got_layout(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    if (!f || !items || items->k != CV_ARRAY) return;
    layout_clear_placed(f->lay);
    for (size_t i = 0; i < items->n; i++) {
        const char *nm = cv_text(&items->items[i], "name");
        int64_t x = 0, y = 0;
        cv_int(&items->items[i], "x", &x);
        cv_int(&items->items[i], "y", &y);
        if (nm) layout_place(f->lay, nm, (int)x, (int)y);
    }
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
    if (ui_container_view(f->cont) == UI_VIEW_ICON) ui_container_layout_changed(f->cont);
}

static void placed_reply(void *arg, const cv *reply)
{
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b)) {
        char m[300];
        snprintf(m, sizeof m, "Positions not kept: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        struct folder *f = from_arg(arg);
        if (f) { snprintf(f->error, sizeof f->error, "%s", m); damage_status(f); }
    }
}

/* Arrange (folder menu): a grid that fits the window, in name order;
 * stored for folders up to 2000 objects (larger ones are arranged for
 * this session). */
static void arrange(struct folder *f)
{
    svc_arrange(f->path, placed_reply, as_arg(f));
    layout_reset(f->lay, cols_now(f));
    size_t *o = name_order(f);
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
        svc_place(f->path, (const char *const *)t, x, y, k, placed_reply, as_arg(f));
        for (size_t i = 0; i < k; i++) free(t[i]);
        free(t);
        free(x);
        free(y);
    }
    ui_container_layout_changed(f->cont);
    snprintf(f->error, sizeof f->error, "Arranged");
    damage_status(f);
}

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
};

static struct props *props_open;
static long next_props_id = 1;

static struct props *props_by_id(long id)
{
    for (struct props *p = props_open; p; p = p->next)
        if (p->id == id) return p;
    return NULL;
}

/* The service's pages, as notebook pages (strings point into `res`). */
static ui_page_desc *pages_from(const cv *res, size_t *n)
{
    *n = res && res->k == CV_ARRAY ? res->n : 0;
    ui_page_desc *pages = calloc(*n ? *n : 1, sizeof *pages);
    for (size_t i = 0; i < *n; i++) {
        const cv *pg = &res->items[i], *fs = cv_get(pg, "fields");
        pages[i].title = cv_text(pg, "title");
        size_t nf = fs && fs->k == CV_ARRAY ? fs->n : 0;
        ui_field_desc *fields = calloc(nf ? nf : 1, sizeof *fields);
        for (size_t k = 0; k < nf; k++) {
            const cv *fd = &fs->items[k], *v = cv_get(fd, "value"), *ch = cv_get(fd, "choices");
            const char *type = cv_text(fd, "type");
            ui_field_desc *d = &fields[k];
            d->key = cv_text(fd, "key");
            d->label = cv_text(fd, "label");
            d->kind = !type || !strcmp(type, "text") ? UI_FIELD_TEXT : !strcmp(type, "checkbox") ? UI_FIELD_CHECKBOX
                      : !strcmp(type, "number") ? UI_FIELD_NUMBER : UI_FIELD_CHOICE;
            if (v && v->k == CV_TEXT) d->value.text = (char *)v->s;
            else if (v && v->k == CV_INT) d->value.number = v->i;
            else if (v && v->k == CV_BOOL) d->value.checked = v->b;
            if (ch && ch->k == CV_ARRAY) {
                const char **c = calloc(ch->n ? ch->n : 1, sizeof *c);
                for (size_t j = 0; j < ch->n; j++) c[j] = ch->items[j].k == CV_TEXT ? ch->items[j].s : "";
                d->choices = c;
                d->nchoices = ch->n;
            }
        }
        pages[i].fields = fields;
        pages[i].nfields = nf;
    }
    return pages;
}

static void pages_free(ui_page_desc *pages, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < pages[i].nfields; k++) free((void *)pages[i].fields[k].choices);
        free((void *)pages[i].fields);
    }
    free(pages);
}

static void note_object(struct props *p, const cv *reply)
{
    const cv *o = reply ? cv_get(reply, "object") : NULL;
    int64_t ch;
    if (o && cv_int(o, "changes", &ch)) p->changes = ch;
    const char *path = o ? cv_text(o, "path") : NULL;
    if (path && *path) snprintf(p->target, sizeof p->target, "%s", path);   /* renamed */
}

static void props_set(void *ctx, const char *key, ui_field_kind kind, const ui_value *v);

static void props_refreshed(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!p || !res || !p->nb) return;
    note_object(p, reply);
    size_t n;
    ui_page_desc *pages = pages_from(res, &n);
    ui_notebook_update(p->nb, pages, n);
    pages_free(pages, n);
}

static void props_refresh(struct props *p) { svc_settings(p->target, props_refreshed, (void *)(intptr_t)p->id); }

static void props_applied(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    if (!p || !p->nb) return;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        note_object(p, reply);
        const cv *res = cv_get(reply, "result");
        if (res && res->k == CV_BOOL && !res->b)   /* a rename without permission */
            ui_notebook_set_status(p->nb, "No permission to rename the file: the new name is kept as its title");
        else ui_notebook_set_status(p->nb, "");
        props_refresh(p);
        return;
    }
    const char *code = reply ? cv_text(reply, "error") : NULL, *msg = reply ? cv_text(reply, "message") : NULL;
    char m[300];
    if (code && !strcmp(code, "conflict")) snprintf(m, sizeof m, "Changed elsewhere meanwhile: these are the current values");
    else snprintf(m, sizeof m, "Not changed: %s", msg ? msg : reply ? "refused" : "lost the connection to wpsd");
    ui_notebook_set_status(p->nb, m);
    props_refresh(p);
}

static void props_set(void *ctx, const char *key, ui_field_kind kind, const ui_value *v)
{
    struct props *p = ctx;
    void *arg = (void *)(intptr_t)p->id;
    if (!strcmp(key, "title") && kind == UI_FIELD_TEXT) {
        /* Decided: a file's title is its name; the service renames it (or,
         * without permission, keeps it as a title). */
        svc_rename(p->target, v->text ? v->text : "", props_applied, arg);
        return;
    }
    cv val = { 0 };
    if (kind == UI_FIELD_TEXT) { val.k = CV_TEXT; val.s = v->text ? v->text : ""; val.len = strlen(val.s); }
    else if (kind == UI_FIELD_CHECKBOX) { val.k = CV_BOOL; val.b = v->checked; }
    else { val.k = CV_INT; val.i = v->number; }
    svc_settings_set(p->target, key, &val, p->changes, props_applied, arg);
}

static void props_closed(void *ctx)
{
    struct props *p = ctx;
    for (struct props **pp = &props_open; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    free(p);
}

static void got_settings(void *arg, const cv *reply)
{
    struct props *p = props_by_id((long)(intptr_t)arg);
    if (!p) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res) {
        fprintf(stderr, "wps-shell: no settings for %s: %s\n", p->target, reply ? cv_text(reply, "message") : "no service");
        props_closed(p);
        return;
    }
    note_object(p, reply);
    const cv *o = cv_get(reply, "object");
    const char *t = o ? cv_text(o, "title") : NULL;
    snprintf(p->title, sizeof p->title, "%s - Properties", t && *t ? t : p->target);
    size_t n;
    ui_page_desc *pages = pages_from(res, &n);
    p->nb = ui_notebook_open(app, p->title, pages, n, props_set, props_closed, p);
    pages_free(pages, n);
}

/* Open an object's notebook, or leave the open one (Wayland lets no program
 * raise its own window on its own). */
static bool open_properties(const char *target)
{
    for (struct props *p = props_open; p; p = p->next)
        if (!strcmp(p->target, target)) return false;
    struct props *p = calloc(1, sizeof *p);
    p->id = next_props_id++;
    snprintf(p->target, sizeof p->target, "%s", target);
    p->next = props_open;
    props_open = p;
    svc_settings(target, got_settings, (void *)(intptr_t)p->id);
    return true;
}

/* Menus: the service gives the items; the GUI carries out the well-known
 * ones and sends the rest back. */
struct menu_ctx { long folder; size_t item; double x, y; uint32_t serial; };

static void convert(const cv *arr, ui_menu_item **out, size_t *n)
{
    *n = arr && arr->k == CV_ARRAY ? arr->n : 0;
    *out = calloc(*n ? *n : 1, sizeof **out);
    for (size_t i = 0; i < *n; i++) {
        const cv *it = &arr->items[i];
        int64_t id = 0;
        cv_int(it, "id", &id);
        const cv *en = cv_get(it, "enabled"), *ck = cv_get(it, "checked");
        (*out)[i].id = id;
        (*out)[i].label = (char *)(cv_text(it, "label") ? cv_text(it, "label") : "");
        (*out)[i].enabled = !en || (en->k == CV_BOOL && en->b);
        (*out)[i].checked = ck && ck->k == CV_BOOL && ck->b;
        convert(cv_get(it, "items"), &(*out)[i].items, &(*out)[i].nitems);
    }
}

static void free_converted(ui_menu_item *m, size_t n)
{
    for (size_t i = 0; i < n; i++) free_converted(m[i].items, m[i].nitems);
    free(m);
}

static void selected_done(void *arg, const cv *reply) { opened(arg, reply); }

static void menu_chosen(void *arg, int64_t id)
{
    struct menu_ctx *m = arg;
    struct folder *f = folder_by_id(m->folder);
    if (f && id >= 0) {
        char t[PATH_MAX];
        target_of(f, m->item, t, sizeof t);
        switch (id) {
        case WPS_MI_OPEN: open_item(f, m->item, UI_VIEW_ICON); break;
        case WPS_MI_OPEN_ICON: open_item(f, m->item, UI_VIEW_ICON); break;
        case WPS_MI_OPEN_DETAILS: open_item(f, m->item, UI_VIEW_DETAILS); break;
        case WPS_MI_PROPERTIES:
            if (!open_properties(t)) {
                snprintf(f->error, sizeof f->error, "Its settings notebook is already open");
                damage_status(f);
            }
            break;
        case WPS_MI_OPEN_WITH:
            snprintf(f->error, sizeof f->error, "Open With comes in G2");
            damage_status(f);
            break;
        case WPS_MI_VIEW_ICON: set_view(f, UI_VIEW_ICON); break;
        case WPS_MI_VIEW_DETAILS: set_view(f, UI_VIEW_DETAILS); break;
        case WPS_MI_VIEW_TREE: set_view(f, UI_VIEW_TREE); break;
        case WPS_MI_OPEN_TREE: open_item(f, m->item, UI_VIEW_TREE); break;
        case WPS_MI_ARRANGE: arrange(f); break;
        default: svc_menu_select(t, id, selected_done, as_arg(f)); break;
        }
    }
    free(m);
}

static void got_menu(void *arg, const cv *reply)
{
    struct menu_ctx *m = arg;
    struct folder *f = folder_by_id(m->folder);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!f || !res) { free(m); return; }
    ui_menu_item *items;
    size_t n;
    convert(res, &items, &n);
    if (m->item == SIZE_MAX) {
        /* The folder window's own menu: a View cascade after Open, to switch
         * this window's view (the Open cascade opens new windows). */
        ui_view v = ui_container_view(f->cont);
        ui_menu_item *views = calloc(3, sizeof *views);
        views[0] = (ui_menu_item){ WPS_MI_VIEW_ICON, "Icon view\tCtrl+1", true, v == UI_VIEW_ICON, NULL, 0 };
        views[1] = (ui_menu_item){ WPS_MI_VIEW_TREE, "Tree view\tCtrl+2", true, v == UI_VIEW_TREE, NULL, 0 };
        views[2] = (ui_menu_item){ WPS_MI_VIEW_DETAILS, "Details view\tCtrl+3", true, v == UI_VIEW_DETAILS, NULL, 0 };
        items = realloc(items, (n + 1) * sizeof *items);
        size_t at = n ? 1 : 0;
        memmove(items + at + 1, items + at, (n - at) * sizeof *items);
        items[at] = (ui_menu_item){ WPS_MI_VIEW, "View", true, false, views, 3 };
        n++;
    }
    /* No grab (serial 0): the button was released before the service
     * answered, and GNOME refuses a grab then, dismissing the menu at once.
     * The shell closes its own menus on clicks elsewhere. */
    ui_menu_popup(f->win, items, n, m->x, m->y, 0, menu_chosen, m);
    free_converted(items, n);
}

static void src_menu(void *ctx, size_t i, double x, double y, uint32_t serial)
{
    struct folder *f = ctx;
    struct menu_ctx *m = calloc(1, sizeof *m);
    m->folder = f->id;
    m->item = i;
    m->x = x;
    m->y = y;
    m->serial = serial;
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    svc_menu(t, got_menu, m);
}

static void renamed(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        const cv *res = cv_get(reply, "result");
        if (res && res->k == CV_BOOL && !res->b)
            snprintf(f->error, sizeof f->error, "No permission to rename the file: the new name is kept as its title");
        else f->error[0] = 0;
    } else {
        const char *msg = reply ? cv_text(reply, "message") : "lost the connection to wpsd";
        snprintf(f->error, sizeof f->error, "Cannot rename: %s", msg ? msg : "refused");
    }
    damage_status(f);
    reload(f);   /* events follow too; this covers a title-only change */
}

static void src_rename(void *ctx, size_t i, const char *name)
{
    struct folder *f = ctx;
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    const char *old = row_name(f, i);
    if (old && !strcmp(old, name)) return;
    svc_rename(t, name, renamed, as_arg(f));
}


/* ---------------------------------------------------------- drag and drop */

#define MIME_WPS "application/x-wps-objects"
#define MIME_URIS "text/uri-list"

/* What this program is dragging now (for refusing a folder dropped into
 * itself before the data is read). */
struct dragging {
    char **paths;
    size_t n;
    char *wps, *uris;        /* the data in both formats */
    double *dx, *dy;         /* each icon's offset from the grab point (icon view) */
};
static struct dragging *cur_drag;

static void dragging_free(struct dragging *d)
{
    if (!d) return;
    for (size_t i = 0; i < d->n; i++) free(d->paths[i]);
    free(d->paths);
    free(d->dx);
    free(d->dy);
    free(d->wps);
    free(d->uris);
    free(d);
}

/* Append text with %-escapes for a file: URI. */
static void uri_append(char **buf, size_t *len, size_t *cap, const char *path)
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

static void src_drag(void *ctx, size_t i, uint32_t serial)
{
    struct folder *f = ctx;
    struct dragging *d = calloc(1, sizeof *d);
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
            d->dx = realloc(d->dx, cap * sizeof *d->dx);
            d->dy = realloc(d->dy, cap * sizeof *d->dy);
        }
        double gx, gy, sx, sy, ix = 0, iy = 0;
        ui_container_drag_origin(f->cont, &sx, &sy);
        ui_container_to_list(f->cont, sx, sy, &gx, &gy);
        if (!is_tree(f)) layout_pos(f->lay, k, &ix, &iy);
        d->dx[d->n] = ix - gx;
        d->dy[d->n] = iy - gy;
        d->paths[d->n++] = strdup(t);
    }
    size_t wl = 0, wc = 0, ul = 0, uc = 0;
    for (size_t k = 0; k < d->n; k++) {
        size_t need = wl + strlen(d->paths[k]) + 2;
        if (need > wc) { while (wc < need) wc = wc ? wc * 2 : 1024; d->wps = realloc(d->wps, wc); }
        wl += (size_t)sprintf(d->wps + wl, "%s\n", d->paths[k]);
        uri_append(&d->uris, &ul, &uc, d->paths[k]);
    }
    if (!d->wps || !d->uris) { dragging_free(d); return; }
    /* The drag image: the item's icon, and how many go. */
    int sc = ui_surface_scale(f->win);
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48 * sc, 40 * sc);
    cairo_surface_set_device_scale(img, sc, sc);
    cairo_t *cr = cairo_create(img);
    ui_item it = { 0 };
    src_item(f, i, &it);
    ui_draw_kind_icon(cr, it.loaded ? it.kind : UI_KIND_FILE, 0, 4, 32);
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
    if (!ui_drag_start(f->win, serial, mimes, 2, UI_DND_COPY | UI_DND_MOVE | UI_DND_ASK, img, sc, drag_send, drag_end, d)) {
        cur_drag = NULL;
        dragging_free(d);
    }
    cairo_surface_destroy(img);
}

/* Where a drop at (x, y) goes: a folder under the pointer, else the
 * window's folder. */
static void drop_dest(struct folder *f, double x, double y, char *out, size_t n, size_t *item)
{
    size_t i = ui_container_item_at(f->cont, x, y);
    ui_item it = { 0 };
    if (i != SIZE_MAX) src_item(f, i, &it);
    if (i != SIZE_MAX && it.loaded && it.kind == UI_KIND_FOLDER) {
        target_of(f, i, out, n);
        *item = i;
    } else {
        snprintf(out, n, "%s", f->path);
        *item = SIZE_MAX;
    }
}

static bool shadow_mods(unsigned mods) { return (mods & UI_MOD_CTRL) && (mods & UI_MOD_SHIFT); }

static bool under(const char *path, const char *dir)
{
    size_t l = strlen(dir);
    return !strncmp(path, dir, l) && (path[l] == '/' || path[l] == 0);
}

static uint32_t win_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n,
                                uint32_t src_actions, unsigned mods, const char **mime)
{
    struct folder *f = ui_surface_data(s);
    *mime = NULL;
    for (size_t i = 0; i < n && !*mime; i++) if (!strcmp(mimes[i], MIME_WPS)) *mime = MIME_WPS;
    for (size_t i = 0; i < n && !*mime; i++) if (!strcmp(mimes[i], MIME_URIS)) *mime = MIME_URIS;
    if (!*mime) return 0;
    char dest[PATH_MAX];
    size_t item;
    drop_dest(f, x, y, dest, sizeof dest, &item);
    /* As in OS/2, from the keys held: none moves, Ctrl copies, Ctrl+Shift
     * makes a shadow; Alt asks. GNOME's own choice is not used: for a
     * button 2 drag it says "ask", and now and then "copy", with no key held. */
    uint32_t act = (mods & UI_MOD_ALT) ? UI_DND_ASK : (mods & UI_MOD_CTRL) ? UI_DND_COPY : UI_DND_MOVE;
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
    }
    ui_container_set_drop_target(f->cont, act ? item : SIZE_MAX);
    return act;
}

static void win_drag_leave(ui_surface *s)
{
    struct folder *f = ui_surface_data(s);
    ui_container_set_drop_target(f->cont, SIZE_MAX);
}

/* Jobs started by drops: progress in the target window, decisions asked. */
struct jobtrack {
    struct jobtrack *next;
    int64_t job;
    long folder;
    char kind[8];
    bool asking;
};
static struct jobtrack *jobs;
static ui_dialog *last_dialog;   /* for self-tests */

struct xfer {
    long folder;
    char kind[8], dest[PATH_MAX], from[PATH_MAX];
    char **names;
    size_t n;
};

static void xfer_free(struct xfer *x)
{
    for (size_t i = 0; i < x->n; i++) free(x->names[i]);
    free(x->names);
    free(x);
}

static void set_status(long folder, const char *msg)
{
    struct folder *f = folder_by_id(folder);
    if (!f) return;
    snprintf(f->error, sizeof f->error, "%s", msg);
    damage_status(f);
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
        t->next = jobs;
        jobs = t;
    } else {
        char m[300];
        snprintf(m, sizeof m, "Cannot %s: %s", x->kind, reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(x->folder, m);
    }
    xfer_free(x);
}

static void got_sel(void *arg, const cv *reply)
{
    struct xfer *x = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res || res->k != CV_INT) { set_status(x->folder, "Cannot select the dragged objects"); xfer_free(x); return; }
    svc_job_start(x->kind, res->i, x->dest, got_job, x);
}

static void shadowed(void *arg, const cv *reply)
{
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (!(ok && ok->k == CV_BOOL && ok->b)) {
        char m[300];
        snprintf(m, sizeof m, "Cannot make a shadow: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status((long)(intptr_t)arg, m);
    }
}

/* "file:///a%20b" -> "/a b"; NULL if not a local file. */
static char *uri_path(const char *u)
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

enum { DROP_MOVE, DROP_COPY, DROP_SHADOW };

/* Icons dragged within their own folder, in icon view: they go where they
 * were dropped, each keeping its offset from the grab point, and the
 * places are stored. Returns false if the drag came from elsewhere. */
static bool reposition(struct folder *f, double x, double y)
{
    struct dragging *d = cur_drag;
    for (size_t i = 0; i < d->n; i++) {
        const char *sl = strrchr(d->paths[i], '/');
        size_t pl = sl ? (size_t)(sl - d->paths[i]) : 0;
        if (pl != strlen(f->path) || strncmp(d->paths[i], f->path, pl)) return false;
    }
    double lx, ly;
    ui_container_to_list(f->cont, x, y, &lx, &ly);
    int64_t *px = calloc(d->n, sizeof *px), *py = calloc(d->n, sizeof *py);
    for (size_t i = 0; i < d->n; i++) {
        double nx = lx + d->dx[i], ny = ly + d->dy[i];
        px[i] = nx < 0 ? 0 : (int64_t)nx;
        py[i] = ny < 0 ? 0 : (int64_t)ny;
        layout_place(f->lay, strrchr(d->paths[i], '/') + 1, (int)px[i], (int)py[i]);
    }
    svc_place(f->path, (const char *const *)d->paths, px, py, d->n, placed_reply, as_arg(f));
    free(px);
    free(py);
    layout_update(f->lay, f->m.total, lay_name, f, true, NULL);
    ui_container_layout_changed(f->cont);
    return true;
}

static void do_drop(struct folder *f, const char *dest, const char *mime, const char *data, int how);

/* The compositor said "ask" (Alt in GNOME): a menu at the drop point. */
struct ask_ctx { long folder; char dest[PATH_MAX]; char *mime, *data; };

static void ask_chosen(void *arg, int64_t id)
{
    struct ask_ctx *a = arg;
    struct folder *f = folder_by_id(a->folder);
    if (f && id >= DROP_MOVE && id <= DROP_SHADOW) do_drop(f, a->dest, a->mime, a->data, (int)id);
    free(a->mime);
    free(a->data);
    free(a);
}

static void win_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len, uint32_t action,
                     unsigned mods)
{
    (void)len;
    struct folder *f = ui_surface_data(s);
    ui_container_set_drop_target(f->cont, SIZE_MAX);
    char dest[PATH_MAX];
    size_t item;
    drop_dest(f, x, y, dest, sizeof dest, &item);
    (void)action;   /* see win_drag_motion: the keys decide */
    int how = shadow_mods(mods) ? DROP_SHADOW : (mods & UI_MOD_CTRL) ? DROP_COPY : DROP_MOVE;
    if (how == DROP_MOVE && cur_drag && !strcmp(dest, f->path) && ui_container_view(f->cont) == UI_VIEW_ICON &&
        reposition(f, x, y))
        return;
    if ((mods & UI_MOD_ALT) && !shadow_mods(mods)) {
        struct ask_ctx *a = calloc(1, sizeof *a);
        a->folder = f->id;
        snprintf(a->dest, sizeof a->dest, "%s", dest);
        a->mime = strdup(mime);
        a->data = strdup(data);
        ui_menu_item items[] = {
            { DROP_MOVE, "Move here", true, false, NULL, 0 },
            { DROP_COPY, "Copy here", true, false, NULL, 0 },
            { DROP_SHADOW, "Create shadow here", true, false, NULL, 0 },
            { -1, "-", false, false, NULL, 0 },
            { 99, "Cancel", true, false, NULL, 0 },
        };
        ui_menu_popup(s, items, 5, x, y, 0, ask_chosen, a);
        return;
    }
    if (getenv("WPS_DEBUG_DND"))
        fprintf(stderr, "dnd: dropping as %s\n", how == DROP_SHADOW ? "shadow" : how == DROP_COPY ? "copy" : "move");
    do_drop(f, dest, mime, data, how);
}

static void do_drop(struct folder *f, const char *dest, const char *mime, const char *data, int how)
{
    bool shadow = how == DROP_SHADOW;
    const char *kind = how == DROP_COPY ? "copy" : "move";
    /* Paths, grouped by the folder they come from (a job per folder). */
    char *copy = strdup(data), *save = NULL;
    struct xfer *groups[64];
    size_t ng = 0, total = 0;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
        if (!p) continue;
        total++;
        if (shadow) { svc_shadow(p, dest, shadowed, as_arg(f)); free(p); continue; }
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

struct decide_ctx { int64_t job; };

static void decided(void *arg, int choice)
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

static void on_job_event(const cv *st)
{
    int64_t id, done = 0, total = 0;
    if (!st || !cv_int(st, "job", &id)) return;
    struct jobtrack *t = jobs, **pp = &jobs;
    for (; t && t->job != id; pp = &t->next, t = t->next) {}
    if (!t) return;   /* not one of ours */
    cv_int(st, "items_done", &done);
    cv_int(st, "items_total", &total);
    const char *state = cv_text(st, "state"), *cur = cv_text(st, "current"), *err = cv_text(st, "error");
    bool move = !strcmp(t->kind, "move");
    char m[400];
    if (!state) return;
    if (!strcmp(state, "done") || !strcmp(state, "failed") || !strcmp(state, "cancelled")) {
        const cv *miss = cv_get(st, "missing"), *skip = cv_get(st, "skipped");
        size_t nm = miss && miss->k == CV_ARRAY ? miss->n : 0, ns = skip && skip->k == CV_ARRAY ? skip->n : 0;
        if (!strcmp(state, "done"))
            snprintf(m, sizeof m, "%s %lld object%s%s%s", move ? "Moved" : "Copied", (long long)(done - (int64_t)ns),
                     done - (int64_t)ns == 1 ? "" : "s", ns ? "; some skipped" : "", nm ? "; some were gone already" : "");
        else if (!strcmp(state, "failed")) snprintf(m, sizeof m, "%s failed: %s", move ? "Move" : "Copy", err ? err : "unknown error");
        else snprintf(m, sizeof m, "%s cancelled after %lld", move ? "Move" : "Copy", (long long)done);
        set_status(t->folder, m);
        *pp = t->next;
        free(t);
        return;
    }
    if (!strcmp(state, "needs-decision") && !t->asking) {
        const char *clash = cv_text(st, "conflict"), *to = cv_text(st, "to");
        char msg[1200];
        snprintf(msg, sizeof msg, "An object named \"%s\" is already in %s.\nRename the one being %s, or skip it?",
                 clash ? clash : "?", to ? to : "the folder", move ? "moved" : "copied");
        const char *buttons[] = { "Rename", "Skip", "Rename all", "Skip all", "Cancel" };
        struct decide_ctx *d = malloc(sizeof *d);
        d->job = id;
        t->asking = true;
        last_dialog = ui_dialog_open(app, move ? "Move" : "Copy", msg, buttons, 5, 0, decided, d);
        return;
    }
    snprintf(m, sizeof m, "%s %lld of %lld%s%s", move ? "Moving" : "Copying", (long long)done, (long long)total,
             cur && *cur ? ": " : "", cur ? cur : "");
    set_status(t->folder, m);
}

static void src_item_pos(void *ctx, size_t i, double *x, double *y) { layout_pos(((struct folder *)ctx)->lay, i, x, y); }

static size_t src_items_in(void *ctx, ui_rect r, size_t *out, size_t max)
{
    return layout_items_in(((struct folder *)ctx)->lay, r.x, r.y, r.w, r.h, out, max);
}

static void src_content_size(void *ctx, double *w, double *h) { layout_size(((struct folder *)ctx)->lay, w, h); }

static const ui_container_src folder_src = { src_count, src_item, src_visible, src_activate, src_menu, src_rename, src_expand,
                                             src_drag, src_item_pos, src_items_in, src_content_size };

/* ------------------------------------------------------------------ window */

static void win_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    struct folder *f = ui_surface_data(s);
    const ui_scheme *k = &ui_scheme_now;
    char t[400];
    snprintf(t, sizeof t, "%s - %s", f->title, view_name(ui_container_view(f->cont)));
    ui_frame_draw(s, cr, clip, t);
    ui_container_draw(f->cont, cr, clip);
    ui_rect c = ui_frame_client(s);
    ui_rect st = { c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    if (!ui_rect_meets(clip, st)) return;
    ui_fill(cr, st, k->face);
    ui_bevel(cr, (ui_rect){ st.x + 1, st.y + 1, st.w - 2, st.h - 2 }, false);
    char msg[512];
    size_t nsel = ui_container_selected_count(f->cont);
    if (f->error[0]) snprintf(msg, sizeof msg, "%s", f->error);
    else if (is_tree(f)) snprintf(msg, sizeof msg, "%zu folders shown", f->ntvis);
    else if (nsel) snprintf(msg, sizeof msg, "%zu objects, %zu selected%s", f->m.total, nsel, f->m.complete ? "" : " (reading...)");
    else snprintf(msg, sizeof msg, "%zu objects%s", f->m.total, f->m.complete ? "" : " (reading...)");
    ui_set_color(cr, k->text);
    ui_text_draw(cr, UI_FONT_REGULAR, st.x + 6, st.y + (STATUS_H + ui_text_ascent()) / 2 - 1, msg, st.w - 12);
}

static void layout(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    c.h -= STATUS_H;
    ui_container_layout(f->cont, c);
}

static void win_resized(ui_surface *s, double w, double h)
{
    (void)w; (void)h;
    struct folder *f = ui_surface_data(s);
    layout(f);
    ui_surface_damage_all(s);
}

static void win_pointer(ui_surface *s, const ui_pointer_event *e)
{
    struct folder *f = ui_surface_data(s);
    size_t before = ui_container_selected_count(f->cont);
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    bool used = false;
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) used = ui_frame_pointer(s, e);
    if (!used || e->kind == UI_RELEASE || e->kind == UI_MOTION) ui_container_pointer(f->cont, e);
    if (ui_container_selected_count(f->cont) != before) damage_status(f);
}

static void win_close(ui_surface *s);

static void set_view(struct folder *f, ui_view v)
{
    if (v == UI_VIEW_TREE) tree_start(f);
    ui_container_set_view(f->cont, v);
    if (v == UI_VIEW_TREE) tree_rebuild(f, f->troot);
    update_title(f);
    damage_status(f);
}

static void win_key(ui_surface *s, const ui_key_event *e)
{
    struct folder *f = ui_surface_data(s);
    size_t before = ui_container_selected_count(f->cont);
    if ((e->mods & UI_MOD_CTRL) && (e->sym == XKB_KEY_w || e->sym == XKB_KEY_W)) { win_close(s); return; }
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_1) { set_view(f, UI_VIEW_ICON); return; }
    /* In the order the View menu lists them (OS/2's): icon, tree, details. */
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_2) { set_view(f, UI_VIEW_TREE); return; }
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_3) { set_view(f, UI_VIEW_DETAILS); return; }
    ui_container_key(f->cont, e);
    if (ui_container_selected_count(f->cont) != before) damage_status(f);
}

static void win_focus(ui_surface *s, bool on)
{
    (void)on;
    ui_frame_damage(s);
}

static void free_folder(struct folder *f)
{
    for (struct folder **pp = &folders; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    if (f->sub) svc_unsubscribe(f->sub);
    tree_free(f);
    layout_free(f->lay);
    model_free(&f->m);
    if (f->sh) { model_free(f->sh); free(f->sh); }
    ui_container_free(f->cont);
    ui_surface_destroy(f->win);
    free(f);
}

static void report(struct folder *f);

/* Closing happens on the next loop turn: the request comes from inside
 * this window's own event handling (its close button), which must not
 * find the window freed under it. */
static void close_later(void *arg)
{
    struct folder *f = from_arg(arg);
    if (f) free_folder(f);
}

static void win_close(ui_surface *s)
{
    struct folder *f = ui_surface_data(s);
    if (f->closing) return;
    f->closing = true;
    if (f->bench_secs > 0) report(f);
    ui_app_post(app, close_later, as_arg(f));
}

/* Benchmark: a fling, then jumps as if the scroll bar thumb were dragged. */
/* WPS_SHELL_SELFTEST=menus: open, re-open and toggle menus in quick
 * succession (the orders that broke Wayland's popup rules), then quit. */
static void selftest_chosen(void *arg, int64_t id) { (void)arg; (void)id; }

static void selftest(ui_surface *s, double now)
{
    static double t0;
    static int step;
    if (!t0) t0 = now;
    double t = now - t0;
    ui_menu_item items[] = { { 1, "One", true, false, NULL, 0 }, { 2, "Two", true, false, NULL, 0 } };
    if (step == 0 && t > 200) { ui_frame_system_menu(s, 0); ui_frame_system_menu(s, 0); step++; }
    else if (step == 1 && t > 500) { ui_frame_system_menu(s, 0); step++; }           /* after a dismissal */
    else if (step == 2 && t > 520) { ui_frame_system_menu(s, 0); step++; }           /* click again: closes */
    else if (step == 3 && t > 800) { ui_menu_popup(s, items, 2, 100, 120, 0, selftest_chosen, NULL); step++; }
    else if (step == 4 && t > 820) { ui_menu_popup(s, items, 2, 300, 200, 0, selftest_chosen, NULL); ui_frame_system_menu(s, 0); step++; }
    else if (step == 5 && t > 1000) {
        /* The menu opened last is open; a press on the window must close it. */
        ui_menu_item one[] = { { 1, "One", true, false, NULL, 0 } };
        ui_menu_popup(s, one, 1, 50, 50, 0, selftest_chosen, NULL);
        bool open_before = ui_menu_is_open();
        extern bool ui_menu_press_outside(ui_surface *);   /* what a press on the window does */
        bool used = ui_menu_press_outside(s);
        fprintf(stderr, "selftest: press outside: open before %d, used %d, open after %d\n", open_before, used, ui_menu_is_open());
        step++;
    }
    else if (step == 6 && t > 1500) { fprintf(stderr, "selftest: done\n"); ui_app_quit(app); step++; }
}

/* WPS_SHELL_SELFTEST=live: files created and deleted in the folder appear
 * and disappear. =close: closing one of two windows (as its close button
 * does) leaves the other and the process running. */
static void selftest_live(struct folder *f, double now)
{
    static double t0;
    static int step;
    static size_t base;
    char p[PATH_MAX + 32];
    if (!t0) { if (!f->m.complete) return; t0 = now; base = f->m.total; }
    double t = now - t0;
    if (step == 0) {
        for (int i = 0; i < 3; i++) {
            snprintf(p, sizeof p, "%s/selftest-%d.txt", f->path, i);
            FILE *x = fopen(p, "w");
            if (x) fclose(x);
        }
        step++;
    } else if (step == 1 && t > 1500) {
        fprintf(stderr, "selftest: after creating 3 files: %zu objects (was %zu)\n", f->m.total, base);
        for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/selftest-%d.txt", f->path, i); unlink(p); }
        step++;
    } else if (step == 2 && t > 3000) {
        fprintf(stderr, "selftest: after deleting them: %zu objects\n", f->m.total);
        ui_app_quit(app);
        step++;
    }
}

static void selftest_close(ui_surface *s, double now)
{
    static double t0;
    static int step;
    if (!t0) t0 = now;
    if (step == 0 && now - t0 > 300) {
        int n = 0;
        for (struct folder *x = folders; x; x = x->next) n++;
        fprintf(stderr, "selftest: %d windows; closing one\n", n);
        win_close(s);
        step++;
    }
}

static void selftest_after_close(void *arg)
{
    (void)arg;
    int n = 0;
    for (struct folder *x = folders; x; x = x->next) n++;
    fprintf(stderr, "selftest: %d window(s) left, process running\n", n);
    ui_app_quit(app);
}

static void win_frame(ui_surface *s, double now)
{
    struct folder *f = ui_surface_data(s);
    const char *st = getenv("WPS_SHELL_SELFTEST");
    if (st && !strcmp(st, "ctxmenu")) {
        /* A right click on the background, as GNOME delivers it (with a
         * real-looking serial); the menu must still be open afterwards. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        ui_rect c = ui_frame_client(s);
        ui_pointer_event e = { .x = c.x + c.w - 60, .y = c.y + c.h - 60, .button = ui_mouse_button(UI_ACT_POPUP), .clicks = 1, .serial = 4242 };
        if (step == 0) { e.kind = UI_PRESS; win_pointer(s, &e); e.kind = UI_RELEASE; win_pointer(s, &e); step++; }
        else if (step == 1 && now - t0 > 700) { fprintf(stderr, "selftest: right click on background: menu open %d\n", ui_menu_is_open()); step++; }
        else if (step == 2) {
            /* and on an item */
            e.x = c.x + 40; e.y = c.y + 30;
            e.kind = UI_PRESS; win_pointer(s, &e); e.kind = UI_RELEASE; win_pointer(s, &e); step++; t0 = now;
        }
        else if (step == 3 && now - t0 > 700) {
            fprintf(stderr, "selftest: right click on an item: menu open %d\n", ui_menu_is_open());
            /* Choose View > Details view as the background menu would. */
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = SIZE_MAX;
            ui_view before = ui_container_view(f->cont);
            menu_chosen(mc, WPS_MI_VIEW_DETAILS);
            fprintf(stderr, "selftest: View > Details view: %s -> %s\n", before == UI_VIEW_ICON ? "icon" : "details",
                    ui_container_view(f->cont) == UI_VIEW_ICON ? "icon" : "details");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "rename")) {
        /* F2 on old-name.txt, type "renamed", Enter: the file is renamed and
         * the window shows the new name. */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        if (step == 0) {
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "old-name.txt")) i = k;
            if (i == SIZE_MAX) { fprintf(stderr, "selftest: old-name.txt not found\n"); ui_app_quit(app); return; }
            ui_container_set_cursor(f->cont, i);
            ui_key_event k = { .sym = XKB_KEY_F2 };
            win_key(s, &k);
            fprintf(stderr, "selftest: F2 starts a rename: %d\n", ui_container_renaming(f->cont));
            for (const char *p = "renamed"; *p; p++) {
                ui_key_event c = { .sym = (uint32_t)*p };
                c.utf8[0] = *p;
                win_key(s, &c);
            }
            ui_key_event ret = { .sym = XKB_KEY_Return };
            win_key(s, &ret);
            step++;
        } else if (step == 1 && now - t0 > 1500) {
            char p[PATH_MAX + 32];
            snprintf(p, sizeof p, "%s/renamed.txt", f->path);
            bool on_disk = access(p, F_OK) == 0;
            bool shown = false;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "renamed.txt")) shown = true;
            fprintf(stderr, "selftest: renamed.txt on disk %d, in the window %d, status '%s'\n", on_disk, shown, f->error);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "props")) {
        /* Properties on old-name.txt, all by keyboard: Tab to Title, type,
         * Enter (renames the file); Tab to Template, Space; Escape closes. */
        static double t0;
        static int step;
        static ui_surface *nbs;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        if (step == 0) {
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < f->m.total; k++)
                if (model_name(&f->m, k) && !strcmp(model_name(&f->m, k), "old-name.txt")) i = k;
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = i;
            menu_chosen(mc, WPS_MI_PROPERTIES);
            step++;
        } else if (step == 1 && props_open && props_open->nb) {
            nbs = ui_notebook_surface(props_open->nb);
            fprintf(stderr, "selftest: notebook open: %s\n", props_open->title);
            ui_key_event tab = { .sym = XKB_KEY_Tab }, ret = { .sym = XKB_KEY_Return };
            ui_surface_send_key(nbs, &tab);
            for (const char *p = "titled.txt"; *p; p++) {
                ui_key_event c = { .sym = (uint32_t)*p };
                c.utf8[0] = *p;
                ui_surface_send_key(nbs, &c);
            }
            ui_surface_send_key(nbs, &ret);
            step++;
            t0 = now;
        } else if (step == 2 && now - t0 > 800) {
            ui_key_event tab = { .sym = XKB_KEY_Tab }, sp = { .sym = XKB_KEY_space, .utf8 = " " };
            ui_surface_send_key(nbs, &tab);
            ui_surface_send_key(nbs, &sp);
            step++;
            t0 = now;
        } else if (step == 3 && now - t0 > 800) {
            fprintf(stderr, "selftest: notebook now for %s\n", props_open ? props_open->target : "(closed)");
            ui_key_event esc = { .sym = XKB_KEY_Escape };
            ui_surface_send_key(nbs, &esc);
            step++;
            t0 = now;
        } else if (step == 4 && now - t0 > 500) {
            fprintf(stderr, "selftest: after Escape the notebook is %s\n", props_open ? "open" : "closed");
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "tree")) {
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        ui_key_event right = { .sym = XKB_KEY_Right }, left = { .sym = XKB_KEY_Left }, down = { .sym = XKB_KEY_Down };
        char rows[512] = "";
        for (size_t i = 0; i < f->ntvis && strlen(rows) < 400; i++) {
            ui_item it;
            src_item(f, i, &it);
            snprintf(rows + strlen(rows), sizeof rows - strlen(rows), "%s%*s%s%s", i ? " | " : "", it.depth, "", it.name,
                     it.expandable ? (it.expanded ? "[-]" : "[+]") : "");
        }
        if (step == 0) { ui_key_event k2 = { .sym = XKB_KEY_2, .mods = UI_MOD_CTRL }; win_key(s, &k2); step++; }
        else if (step == 1 && t > 600) {
            fprintf(stderr, "selftest: tree: %s\n", rows);
            ui_container_set_cursor(f->cont, 0);
            win_key(s, &down);    /* to a */
            win_key(s, &right);   /* expand a */
            step++;
        } else if (step == 2 && t > 1200) {
            fprintf(stderr, "selftest: a expanded: %s\n", rows);
            for (size_t i = 0; i < f->ntvis; i++)
                if (!strcmp(f->tvis[i]->name, "b")) ui_container_set_cursor(f->cont, i);
            win_key(s, &right);   /* expand b: it has no subfolders */
            char p[PATH_MAX + 16];
            snprintf(p, sizeof p, "%s/a/a3", f->path);
            mkdir(p, 0755);        /* a new subfolder appears in the expanded a */
            step++;
        } else if (step == 3 && t > 2400) {
            fprintf(stderr, "selftest: b read, a3 made: %s\n", rows);
            for (size_t i = 0; i < f->ntvis; i++)
                if (!strcmp(f->tvis[i]->name, "a")) ui_container_set_cursor(f->cont, i);
            win_key(s, &left);     /* collapse a */
            step++;
        } else if (step == 4 && t > 3000) {
            fprintf(stderr, "selftest: a collapsed: %s\n", rows);
            step++;
        } else if (step == 5 && t > 3300) {
            size_t d = ui_surface_verify(s, NULL);
            if (d == SIZE_MAX) return;   /* a change still being drawn: next frame */
            fprintf(stderr, "selftest: tree pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "dnd") && f == folders) {   /* the last window opened: <D>/sub */
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char D[PATH_MAX];
        snprintf(D, sizeof D, "%s", f->path);
        char *sl = strrchr(D, '/');
        *sl = 0;
        char data[PATH_MAX * 2];
        if (step == 0) {
            /* What a drag from the first window offers. */
            struct folder *src = f->next;
            size_t i = SIZE_MAX;
            for (size_t k = 0; k < src->m.total; k++)
                if (model_name(&src->m, k) && !strcmp(model_name(&src->m, k), "with space.txt")) i = k;
            src_drag(src, i, 0);
            if (cur_drag)   /* first line of each (the test prints, it does not change them) */
                fprintf(stderr, "selftest: drag data: wps \"%.*s\" uris \"%.*s\"\n", (int)strcspn(cur_drag->wps, "\n"),
                        cur_drag->wps, (int)strcspn(cur_drag->uris, "\r"), cur_drag->uris);
            /* A folder dropped into itself is refused. */
            struct dragging fake = { 0 };
            char *paths[1] = { f->path };
            fake.paths = paths;
            fake.n = 1;
            struct dragging *was = cur_drag;
            cur_drag = &fake;
            const char *mime, *mimes[] = { MIME_WPS };
            uint32_t act = win_drag_motion(s, 20, 60, mimes, 1, UI_DND_COPY | UI_DND_MOVE, 0, &mime);
            cur_drag = was;
            fprintf(stderr, "selftest: sub dropped into itself: action %u (0 = refused)\n", act);
            snprintf(data, sizeof data, "%s/a.txt\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_MOVE, 0);                      /* move */
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_COPY, UI_MOD_CTRL);            /* copy */
            snprintf(data, sizeof data, "%s/sub2\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_COPY, UI_MOD_CTRL | UI_MOD_SHIFT);   /* shadow */
            data[0] = 0;
            char *u = NULL;
            size_t ul = 0, uc = 0;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/with space.txt", D);
            uri_append(&u, &ul, &uc, p);
            win_drop(s, 20, 60, MIME_URIS, u, ul, UI_DND_MOVE, 0);                                 /* from another app */
            free(u);
            step++;
        } else if (step == 1 && t > 1500) {
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_COPY, UI_MOD_CTRL);            /* b.txt is there now */
            step++;
        } else if (step == 2 && last_dialog) {
            fprintf(stderr, "selftest: a name clash asks: dialog open\n");
            ui_key_event r = { .sym = XKB_KEY_r, .utf8 = "r" };
            ui_surface_send_key(ui_dialog_surface(last_dialog), &r);   /* Rename */
            last_dialog = NULL;
            step++;
            t0 = now;
        } else if (step == 3 && t > 1500) {
            fprintf(stderr, "selftest: status: %s\n", f->error);
            snprintf(data, sizeof data, "%s/b.txt\n", D);
            win_drop(s, 20, 60, MIME_WPS, data, strlen(data), UI_DND_COPY, UI_MOD_CTRL);            /* and again */
            step++;
            t0 = now;
        } else if (step == 4 && last_dialog) {
            ui_key_event r = { .sym = XKB_KEY_r, .utf8 = "r" };
            ui_surface_send_key(ui_dialog_surface(last_dialog), &r);
            last_dialog = NULL;
            step++;
            t0 = now;
        } else if (step == 5 && (last_dialog || t > 2000)) {
            fprintf(stderr, "selftest: a second Rename asked again: %s\n", last_dialog ? "yes" : "no");
            /* What the window shows, with no other change to prompt a refresh. */
            char shown[1000] = "";
            for (size_t i = 0; i < f->m.total; i++) {
                struct page *pg = f->m.pages[i / PAGE];
                struct row *r = &pg->rows[i % PAGE];
                snprintf(shown + strlen(shown), sizeof shown - strlen(shown), " %s%s", r->name,
                         r->shadow ? (r->kind == UI_KIND_FOLDER ? "(shadow of a folder)" : "(shadow)") : "");
            }
            fprintf(stderr, "selftest: the window shows:%s\n", shown);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "dnd")) return;
    if (st && !strcmp(st, "place")) {
        static double t0;
        static int step;
        if (!t0) { if (!f->m.complete) return; t0 = now; }
        double t = now - t0;
        char line[600] = "";
        for (size_t i = 0; i < f->m.total; i++) {
            double x, y;
            layout_pos(f->lay, i, &x, &y);
            snprintf(line + strlen(line), sizeof line - strlen(line), " %s@%.0f,%.0f", model_name(&f->m, i), x, y);
        }
        if (step == 0 && t > 500) {
            fprintf(stderr, "selftest: opened:%s\n", line);
            size_t bi = SIZE_MAX;
            for (size_t i = 0; i < f->m.total; i++) if (!strcmp(model_name(&f->m, i), "b.txt")) bi = i;
            /* Press the drag button on b's icon, as a real drag begins. */
            double bx, by;
            layout_pos(f->lay, bi, &bx, &by);
            ui_rect c = ui_frame_client(s);
            double sx = c.x + 4 + bx + 50, sy = c.y + 4 + by + 20;   /* on the icon */
            ui_pointer_event pr = { .kind = UI_PRESS, .x = sx, .y = sy, .button = ui_mouse_button(UI_ACT_DRAG), .clicks = 1 };
            win_pointer(s, &pr);
            src_drag(f, bi, 0);
            char data[PATH_MAX];
            snprintf(data, sizeof data, "%s/b.txt\n", f->path);
            win_drop(s, sx + 150, sy + 200, MIME_WPS, data, strlen(data), UI_DND_MOVE, 0);
            ui_pointer_event rl = { .kind = UI_RELEASE, .x = sx + 150, .y = sy + 200, .button = ui_mouse_button(UI_ACT_DRAG) };
            win_pointer(s, &rl);
            step++;
        } else if (step == 1 && t > 1500) {
            fprintf(stderr, "selftest: after moving b:%s\n", line);
            struct menu_ctx *mc = calloc(1, sizeof *mc);
            mc->folder = f->id;
            mc->item = SIZE_MAX;
            menu_chosen(mc, WPS_MI_ARRANGE);
            step++;
        } else if (step == 2 && t > 2500) {
            fprintf(stderr, "selftest: after Arrange:%s\n", line);
            step++;
        } else if (step == 3 && t > 2800) {
            size_t d = ui_surface_verify(s, NULL);
            if (d == SIZE_MAX) return;
            fprintf(stderr, "selftest: pixels against a fresh redraw: %zu differ\n", d);
            ui_app_quit(app);
            step++;
        }
        return;
    }
    if (st && !strcmp(st, "live")) { selftest_live(f, now); return; }
    if (st && !strcmp(st, "close")) {
        if (f == folders) {   /* the first window closes itself */
            selftest_close(s, now);
            if (f->closing) ui_app_post(app, selftest_after_close, NULL);
        }
        return;
    }
    if (st) { selftest(s, now); return; }
    if (f->bench_secs <= 0) return;
    if (f->bench_start == 0) {
        if (!f->m.total) return;
        size_t n;
        const ui_frame_stat *st;
        ui_surface_stats(s, &st, &n);
        f->bench_start = now;
        f->bench_next_jump = now + 3000;
        f->bench_from = n;
    }
    /* Every 30 frames, check the picture against a full redraw. */
    if (getenv("WPS_SHELL_VERIFY") && ++f->frames_seen % 30 == 0) {
        size_t d = ui_surface_verify(s, f->verify_failed ? NULL : getenv("WPS_SHELL_VERIFY"));
        if (d != SIZE_MAX) {
            f->verified++;
            if (d) { f->verify_failed++; f->verify_diff += d; }
        }
    }
    if (now - f->bench_start > f->bench_secs * 1000) {
        report(f);
        f->bench_secs = 0;
        ui_surface_set_continuous(s, false);
        ui_app_quit(app);
        return;
    }
    if (now < f->bench_next_jump) ui_container_scroll_by(f->cont, 60);
    else {
        ui_container_scroll_to_fraction(f->cont, (double)(rand_r(&f->bench_seed) % 1000000) / 1e6);
        f->bench_next_jump = now + 500;
    }
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void report(struct folder *f)
{
    const ui_frame_stat *st;
    size_t n;
    ui_surface_stats(f->win, &st, &n);
    size_t from = f->bench_from + 1, m = n > from ? n - from : 0;
    double *d = malloc((m ? m : 1) * sizeof *d), *iv = malloc((m ? m : 1) * sizeof *iv), frac = 0;
    double *mv = malloc((m ? m : 1) * sizeof *mv);
    size_t fulls = 0;
    for (size_t i = 0; i < m; i++) {
        d[i] = st[from + i].draw_ms;
        mv[i] = st[from + i].move_ms;
        iv[i] = st[from + i].interval_ms;
        frac += st[from + i].drawn_fraction;
        fulls += st[from + i].full;
    }
    qsort(d, m, sizeof *d, cmp_d);
    qsort(iv, m, sizeof *iv, cmp_d);
    qsort(mv, m, sizeof *mv, cmp_d);
    double secs = (ui_now_ms() - f->bench_start) / 1000.0;
    size_t late = 0;
    double med = m ? iv[m / 2] : 0;
    for (size_t i = 0; i < m; i++) late += iv[i] > med * 1.5;
#define P(v, p) (m ? (v)[(size_t)((p) / 100.0 * (double)(m - 1) + 0.5)] : 0)
    printf("{\"folder\":\"%s\",\"total\":%zu,\"view\":\"%s\",\"scale\":%d,\"size\":[%.0f,%.0f],\"frames\":%zu,"
           "\"fps\":%.1f,\"draw_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},"
           "\"interval_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},\"late_frames\":%zu,"
           "\"full_redraws\":%zu,\"drawn_fraction_mean\":%.3f,\"move_ms\":{\"p50\":%.2f,\"p99\":%.2f},"
           "\"verified\":%zu,\"verify_failed\":%zu,\"verify_diff_px\":%zu}\n",
           f->path, f->m.total, ui_container_view(f->cont) == UI_VIEW_ICON ? "icon" : "details", ui_surface_scale(f->win),
           ui_surface_width(f->win), ui_surface_height(f->win), m, secs > 0 ? (double)m / secs : 0, P(d, 50), P(d, 99),
           m ? d[m - 1] : 0, P(iv, 50), P(iv, 99), m ? iv[m - 1] : 0, late, fulls, m ? frac / (double)m : 0, P(mv, 50), P(mv, 99), f->verified, f->verify_failed, f->verify_diff);
    fflush(stdout);
    free(d);
    free(iv);
    free(mv);
}

static const ui_surface_ops win_ops = {
    .draw = win_draw, .pointer = win_pointer, .key = win_key, .resized = win_resized,
    .drag_motion = win_drag_motion, .drag_leave = win_drag_leave, .drop = win_drop,
    .focus = win_focus, .close = win_close, .frame = win_frame,
};

static void got_title(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *t = res ? cv_text(res, "title") : NULL;
    if (!f || !t || !*t) return;
    snprintf(f->title, sizeof f->title, "%s", t);
    update_title(f);
}

static struct folder *open_folder(const char *path, ui_view view)
{
    struct folder *f = calloc(1, sizeof *f);
    f->id = next_folder_id++;
    char real[PATH_MAX];
    snprintf(f->path, sizeof f->path, "%s", realpath(path, real) ? real : path);
    const char *base = strrchr(f->path, '/');
    snprintf(f->title, sizeof f->title, "%s", base && base[1] ? base + 1 : f->path);
    double w = view == UI_VIEW_ICON ? 640 : 760, h = 480;
    const char *sz = getenv("WPS_SHELL_SIZE");
    if (sz) sscanf(sz, "%lfx%lf", &w, &h);
    f->win = ui_window_new(app, &win_ops, f, "org.wps.shell", w, h);
    f->lay = layout_new(UI_CELL_W, UI_CELL_H);
    f->cont = ui_container_new(f->win, &folder_src, f, view);
    if (view == UI_VIEW_TREE) tree_start(f);
    f->next = folders;
    folders = f;
    update_title(f);
    layout(f);
    svc_resolve(f->path, got_title, as_arg(f));
    svc_subscribe_folder(f->path, got_sub, as_arg(f));
    svc_layout(f->path, got_layout, as_arg(f));
    request_more(f, false);
    return f;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--details")) opt_view = UI_VIEW_DETAILS;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) opt_bench = atof(argv[++i]);
        else { fprintf(stderr, "usage: wps-shell [--details] [--bench SECONDS] FOLDER...\n"); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "usage: wps-shell [--details] [--bench SECONDS] FOLDER...\n"); return 2; }
    app = ui_app_new();
    if (!app) return 1;
    char err[512];
    if (!svc_start(app, err, sizeof err)) { fprintf(stderr, "wps-shell: %s\n", err); return 1; }
    svc_on_event(on_event, NULL);
    svc_subscribe_jobs(ignore_reply, NULL);
    for (int k = i; k < argc; k++) {
        struct folder *f = open_folder(argv[k], opt_view);
        if (getenv("WPS_SHELL_SELFTEST")) ui_surface_set_continuous(f->win, true);   /* every window ticks */
        if (k == i && opt_bench > 0) {
            f->bench_secs = opt_bench;
            f->bench_seed = 12345;
            ui_surface_set_continuous(f->win, true);
        }
    }
    ui_app_run(app);
    while (folders) free_folder(folders);
    svc_stop();
    return 0;
}
