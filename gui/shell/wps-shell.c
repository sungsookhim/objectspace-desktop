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
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "svc.h"
#include "ui.h"
#include "wps_menu_ids.h"

#define PAGE 256
#define STATUS_H 20

enum { P_NONE, P_NAMES, P_DETAILED };

struct row { char *name, *title; ui_kind kind; int64_t size, mtime; };
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

static void update_title(struct folder *f)
{
    char t[400];
    snprintf(t, sizeof t, "%s - %s", f->title, ui_container_view(f->cont) == UI_VIEW_ICON ? "Icon View" : "Details View");
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
    model_free(old);
    f->m = *nm;
    free(nm);
    f->sh = NULL;
    f->gen++;
    ui_container_select_none(f->cont);
    ui_container_reset(f->cont);
    if (k || curname)
        for (size_t i = 0; i < f->m.total; i++) {
            const char *n = model_name(&f->m, i);
            if (!n) continue;
            for (size_t j = 0; j < k; j++)
                if (sel[j] && !strcmp(sel[j], n)) { ui_container_select(f->cont, i); free(sel[j]); sel[j] = NULL; }
            if (curname && !strcmp(curname, n)) { ui_container_set_cursor(f->cont, i); free(curname); curname = NULL; }
        }
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
        const cv *e = reply ? cv_get(reply, "error") : NULL;
        const char *msg = e ? cv_text(e, "message") : NULL;
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
    for (struct folder *f = folders; f; f = f->next)
        if (f->sub == sub && !f->closing &&
            (!strcmp(name, "content_added") || !strcmp(name, "content_removed") || !strcmp(name, "object_moved") ||
             !strcmp(name, "object_changed")))
            reload(f);
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

/* ----------------------------------------------------------- container src */

static size_t src_count(void *ctx) { return ((struct folder *)ctx)->m.total; }

static void src_item(void *ctx, size_t i, ui_item *out)
{
    struct folder *f = ctx;
    memset(out, 0, sizeof *out);
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
}

static void src_visible(void *ctx, size_t first, size_t last) { fetch_visible(ctx, first, last); }

static const char *row_name(struct folder *f, size_t i) { return model_name(&f->m, i); }

static void target_of(struct folder *f, size_t i, char *out, size_t n)
{
    const char *nm = i == SIZE_MAX ? NULL : row_name(f, i);
    if (nm) snprintf(out, n, "%s/%s", strcmp(f->path, "/") ? f->path : "", nm);
    else snprintf(out, n, "%s", f->path);
}

static void opened(void *arg, const cv *reply)
{
    struct folder *f = from_arg(arg);
    if (!f) return;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const cv *e = reply ? cv_get(reply, "error") : NULL;
    if (res && cv_get(res, "needs_confirmation"))
        snprintf(f->error, sizeof f->error, "This program belongs to another user; confirming comes with dialogs (G2)");
    else if (e)
        snprintf(f->error, sizeof f->error, "%s", cv_text(e, "message") ? cv_text(e, "message") : "cannot open");
    else
        f->error[0] = 0;
    damage_status(f);
}

static void open_item(struct folder *f, size_t i, ui_view view)
{
    char t[PATH_MAX];
    target_of(f, i, t, sizeof t);
    struct page *pg = i == SIZE_MAX || i / PAGE >= f->m.npages ? NULL : f->m.pages[i / PAGE];
    bool is_folder = i == SIZE_MAX || (pg && pg->rows[i % PAGE].kind == UI_KIND_FOLDER);
    if (is_folder) open_folder(t, view);
    else svc_open(t, false, opened, as_arg(f));
}

static void src_activate(void *ctx, size_t i) { open_item(ctx, i, UI_VIEW_ICON); }

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
            snprintf(f->error, sizeof f->error, "The settings notebook comes in G2");
            damage_status(f);
            break;
        case WPS_MI_ARRANGE: break;   /* icon view is arranged in G1; free placement in G2 */
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
    ui_menu_popup(f->win, items, n, m->x, m->y, m->serial, menu_chosen, m);
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

static const ui_container_src folder_src = { src_count, src_item, src_visible, src_activate, src_menu };

/* ------------------------------------------------------------------ window */

static void win_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    struct folder *f = ui_surface_data(s);
    const ui_scheme *k = &ui_scheme_now;
    char t[400];
    snprintf(t, sizeof t, "%s - %s", f->title, ui_container_view(f->cont) == UI_VIEW_ICON ? "Icon View" : "Details View");
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

static void win_key(ui_surface *s, const ui_key_event *e)
{
    struct folder *f = ui_surface_data(s);
    size_t before = ui_container_selected_count(f->cont);
    if ((e->mods & UI_MOD_CTRL) && (e->sym == XKB_KEY_w || e->sym == XKB_KEY_W)) { win_close(s); return; }
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_1) { ui_container_set_view(f->cont, UI_VIEW_ICON); update_title(f); return; }
    if ((e->mods & UI_MOD_CTRL) && e->sym == XKB_KEY_2) { ui_container_set_view(f->cont, UI_VIEW_DETAILS); update_title(f); return; }
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
    f->cont = ui_container_new(f->win, &folder_src, f, view);
    f->next = folders;
    folders = f;
    update_title(f);
    layout(f);
    svc_resolve(f->path, got_title, as_arg(f));
    svc_subscribe_folder(f->path, got_sub, as_arg(f));
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
    for (int k = i; k < argc; k++) {
        struct folder *f = open_folder(argv[k], opt_view);
        if (k == i && getenv("WPS_SHELL_SELFTEST")) ui_surface_set_continuous(f->win, true);
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
