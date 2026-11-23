/*
 * Icon view layout with icons placed anywhere, as in OS/2 (G2).
 *
 * Icons the user placed keep their stored positions (by name here; the
 * service stores them by object ID). Every other icon gets a grid spot
 * the first time its name is seen, and keeps it for the session: a new
 * file takes the next free spot, a deleted one leaves a gap, and resizing
 * the window moves nothing. Arrange starts the grid again.
 *
 * Rows are as tall as their tallest title (OS/2's layout, v0.3-design 5):
 * each item has a height (its cell with the whole title, from the
 * window), and a grid row's is the largest of its items'. Spots are handed
 * out in order, so the rows above a new spot are final when it gets one.
 * Locked icons (v0.3-design 5) are placed icons that Arrange leaves.
 *
 * Finding the icons in an area is grid arithmetic for the unplaced ones
 * (a binary search for the rows) and a scan of the placed ones (few,
 * unless arranged and stored).
 *
 * Free layout (OS/2's, v0.4-design 4): each item is as wide as its title
 * needs (a width function); the spots, in the same order, are packed left
 * to right to a width and wrap into rows as tall as their tallest, clear
 * of placed icons. A spot whose icon left (or was placed) keeps the width
 * it had, so the others stay where they were. The flow is worked out again
 * from the row of the first spot that changed.
 */
#define _GNU_SOURCE
#include "layout.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* name -> value, open addressing, owned keys */
struct map { char **keys; int64_t *vals; size_t cap, n; };

static uint64_t hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

static void map_free(struct map *m)
{
    for (size_t i = 0; i < m->cap; i++) free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

static int64_t *map_slot(struct map *m, const char *k, bool create)
{
    if (create && (m->n + 1) * 2 > m->cap) {
        struct map nm = { calloc(m->cap ? m->cap * 2 : 64, sizeof(char *)), calloc(m->cap ? m->cap * 2 : 64, sizeof(int64_t)),
                          m->cap ? m->cap * 2 : 64, 0 };
        for (size_t i = 0; i < m->cap; i++)
            if (m->keys[i]) {
                size_t j = hash(m->keys[i]) & (nm.cap - 1);
                while (nm.keys[j]) j = (j + 1) & (nm.cap - 1);
                nm.keys[j] = m->keys[i];
                nm.vals[j] = m->vals[i];
                nm.n++;
            }
        free(m->keys);
        free(m->vals);
        *m = nm;
    }
    if (!m->cap) return NULL;
    size_t j = hash(k) & (m->cap - 1);
    while (m->keys[j]) {
        if (!strcmp(m->keys[j], k)) return &m->vals[j];
        j = (j + 1) & (m->cap - 1);
    }
    if (!create) return NULL;
    m->keys[j] = strdup(k);
    m->n++;
    m->vals[j] = -1;
    return &m->vals[j];
}

#define SPOT_NAMELESS 2   /* spot_taken: given while its name was not known */

struct icon_layout {
    int cols;
    double cw, ch;
    struct map slots;        /* name -> grid spot (this session) */
    struct map placed;       /* name -> (x << 32 | y) stored positions */
    struct map locked;       /* names whose places are locked */
    bool lock_all;
    uint8_t *spot_taken;     /* spots given to a name, or under a placed icon */
    size_t nspots;
    size_t next_spot;
    float *x, *y, *h, *w;
    int64_t *spot_of;        /* per item: its spot, or -1 if placed */
    size_t n, cap;
    size_t *spot_item;       /* spot -> item, SIZE_MAX if none now */
    size_t *placed_items;
    size_t nplaced, capplaced;
    float *rowh;             /* per grid row: its height (its tallest item's) */
    double *rowy;            /* rowy[r]: row r's top, valid for r <= ry_ok */
    size_t nrows, caprows, ry_ok;
    double max_x, max_y;
    layout_height_fn hfn;
    void *hctx;
    /* free layout */
    bool free;
    layout_width_fn wfn;
    void *wctx;
    double avail_w, flow_w;  /* the width given (the window's), the one flowed to */
    float *spot_w, *spot_x;  /* per spot: its width, its x */
    uint32_t *spot_row;      /* per spot: its row */
    size_t *rowstart;        /* per row: its first spot */
    size_t spot_end;         /* spots past the last one given */
    size_t flowed_end;       /* spot_end when last flowed */
    void (*on_new)(void *ctx, const char *name);   /* a name given a spot for the first time */
    void *on_new_ctx;
};

void layout_on_new_spot(icon_layout *l, void (*fn)(void *ctx, const char *name), void *ctx)
{
    l->on_new = fn;
    l->on_new_ctx = ctx;
}

void layout_set_height_fn(icon_layout *l, layout_height_fn fn, void *ctx)
{
    l->hfn = fn;
    l->hctx = ctx;
}

void layout_set_width_fn(icon_layout *l, layout_width_fn fn, void *ctx)
{
    l->wfn = fn;
    l->wctx = ctx;
}

void layout_set_free(icon_layout *l, bool on) { l->free = on; }
bool layout_is_free(icon_layout *l) { return l->free; }
void layout_set_width(icon_layout *l, double w) { l->avail_w = w; }

/* The width flowed to: the one given when the columns were (a window's
 * resize moves nothing, as in the grid). */
static void take_width(icon_layout *l)
{
    l->flow_w = l->avail_w > 0 ? l->avail_w : (double)l->cols * l->cw;
}

icon_layout *layout_new(double cell_w, double cell_h)
{
    icon_layout *l = calloc(1, sizeof *l);
    l->cw = cell_w;
    l->ch = cell_h;
    l->cols = 1;
    return l;
}

void layout_free(icon_layout *l)
{
    if (!l) return;
    map_free(&l->slots);
    map_free(&l->placed);
    map_free(&l->locked);
    free(l->spot_taken);
    free(l->x);
    free(l->y);
    free(l->h);
    free(l->spot_of);
    free(l->spot_item);
    free(l->placed_items);
    free(l->rowh);
    free(l->rowy);
    free(l->w);
    free(l->spot_w);
    free(l->spot_x);
    free(l->spot_row);
    free(l->rowstart);
    free(l);
}

int layout_cols(icon_layout *l) { return l->cols; }

static void ensure_spots(icon_layout *l, size_t n)
{
    if (n <= l->nspots) return;
    size_t m = l->nspots ? l->nspots : 256;
    while (m < n) m *= 2;
    l->spot_taken = realloc(l->spot_taken, m);
    memset(l->spot_taken + l->nspots, 0, m - l->nspots);
    l->spot_item = realloc(l->spot_item, m * sizeof *l->spot_item);
    for (size_t i = l->nspots; i < m; i++) l->spot_item[i] = SIZE_MAX;
    l->spot_w = realloc(l->spot_w, m * sizeof *l->spot_w);
    l->spot_x = realloc(l->spot_x, m * sizeof *l->spot_x);
    l->spot_row = realloc(l->spot_row, m * sizeof *l->spot_row);
    for (size_t i = l->nspots; i < m; i++) { l->spot_w[i] = (float)l->cw; l->spot_x[i] = 0; l->spot_row[i] = 0; }
    l->nspots = m;
}

static void ensure_rows(icon_layout *l, size_t n)
{
    if (n > l->caprows) {
        size_t c = l->caprows ? l->caprows : 64;
        while (c < n) c *= 2;
        l->rowh = realloc(l->rowh, c * sizeof *l->rowh);
        l->rowy = realloc(l->rowy, (c + 1) * sizeof *l->rowy);
        l->rowstart = realloc(l->rowstart, (c + 1) * sizeof *l->rowstart);
        l->caprows = c;
    }
    for (size_t r = l->nrows; r < n; r++) l->rowh[r] = (float)l->ch;
    if (n > l->nrows) l->nrows = n;
}

/* Row r's top (the rows above it as tall as they are now). */
static double row_y(icon_layout *l, size_t r)
{
    if (r > l->nrows) ensure_rows(l, r);
    if (!l->nrows) return 0;
    l->rowy[0] = 0;
    for (; l->ry_ok < r; l->ry_ok++) l->rowy[l->ry_ok + 1] = l->rowy[l->ry_ok] + l->rowh[l->ry_ok];
    return l->rowy[r];
}

/* Row r is taller now: the rows below it move down. */
static void row_grows(icon_layout *l, size_t r, double h)
{
    ensure_rows(l, r + 1);
    if (h <= l->rowh[r]) return;
    l->rowh[r] = (float)h;
    if (l->ry_ok > r) l->ry_ok = r;
}

static void rows_reset(icon_layout *l)
{
    for (size_t r = 0; r < l->nrows; r++) l->rowh[r] = (float)l->ch;
    l->ry_ok = 0;
}

void layout_reset(icon_layout *l, int cols)
{
    /* Arrange: only locked icons keep their places */
    struct map keep = { 0 };
    for (size_t i = 0; i < l->placed.cap; i++)
        if (l->placed.keys[i] && (l->lock_all || map_slot(&l->locked, l->placed.keys[i], false)))
            *map_slot(&keep, l->placed.keys[i], true) = l->placed.vals[i];
    map_free(&l->slots);
    map_free(&l->placed);
    l->placed = keep;
    l->cols = cols < 1 ? 1 : cols;
    take_width(l);
    if (l->spot_taken) memset(l->spot_taken, 0, l->nspots);
    for (size_t i = 0; i < l->n; i++) l->spot_of[i] = -1;   /* (none kept: the grid starts again) */
    l->next_spot = 0;
    l->spot_end = l->flowed_end = 0;
    rows_reset(l);
}

void layout_set_cell(icon_layout *l, double cell_w, double cell_h, int cols)
{
    l->cw = cell_w;
    l->ch = cell_h;
    l->cols = cols < 1 ? 1 : cols;   /* (the spots in their order, flowed again) */
    take_width(l);
    for (size_t s = 0; s < l->nspots; s++) l->spot_w[s] = (float)cell_w;   /* (gaps: a cell, as new) */
    rows_reset(l);
}

void layout_set_cols(icon_layout *l, int cols)
{
    if (l->slots.n == 0 && l->placed.n == 0) { l->cols = cols < 1 ? 1 : cols; take_width(l); }
}

void layout_place(icon_layout *l, const char *name, int x, int y)
{
    int64_t *v = map_slot(&l->placed, name, true);
    *v = ((int64_t)x << 32) | (uint32_t)y;
}

void layout_clear_placed(icon_layout *l)
{
    map_free(&l->placed);
    map_free(&l->locked);
}

bool layout_is_placed(icon_layout *l, const char *name) { return map_slot(&l->placed, name, false) != NULL; }

void layout_lock(icon_layout *l, const char *name, bool on)
{
    if (on) *map_slot(&l->locked, name, true) = 1;
    else {
        int64_t *v = map_slot(&l->locked, name, false);
        if (v) *v = 0;   /* (kept as a key: open addressing has no removal) */
    }
}

bool layout_is_locked(icon_layout *l, const char *name)
{
    if (!name) return false;
    if (l->lock_all) return true;
    int64_t *v = map_slot(&l->locked, name, false);
    return v && *v == 1;
}

void layout_lock_all(icon_layout *l, bool on) { l->lock_all = on; }
bool layout_all_locked(icon_layout *l) { return l->lock_all; }

/* Renamed: the icon keeps its place (its grid spot or position) under its
 * new name, as on OS/2, instead of going to the next free spot. */
void layout_rename(icon_layout *l, const char *from, const char *to)
{
    int64_t *pv = map_slot(&l->placed, from, false);
    if (pv) { int64_t v = *pv; *map_slot(&l->placed, to, true) = v; }
    int64_t *sv = map_slot(&l->slots, from, false);
    if (sv && *sv >= 0) { int64_t v = *sv; *map_slot(&l->slots, to, true) = v; }
    int64_t *lv = map_slot(&l->locked, from, false);
    if (lv && *lv == 1) *map_slot(&l->locked, to, true) = 1;
}

static void grow_items(icon_layout *l, size_t n)
{
    if (n <= l->cap) return;
    size_t c = l->cap ? l->cap : 256;
    while (c < n) c *= 2;
    l->x = realloc(l->x, c * sizeof *l->x);
    l->y = realloc(l->y, c * sizeof *l->y);
    l->h = realloc(l->h, c * sizeof *l->h);
    l->w = realloc(l->w, c * sizeof *l->w);
    l->spot_of = realloc(l->spot_of, c * sizeof *l->spot_of);
    l->cap = c;
}

static double item_h(icon_layout *l, size_t i)
{
    double h = l->hfn ? l->hfn(l->hctx, i) : l->ch;
    return h > l->ch ? h : l->ch;
}

static double item_w(icon_layout *l, size_t i)
{
    if (!l->free || !l->wfn) return l->cw;
    double w = l->wfn(l->wctx, i);
    return w > 8 ? w : l->cw;
}

/* Whether a cell at (x, y), h tall, would cover a placed icon. */
static bool over_placed(icon_layout *l, double x, double y, double h)
{
    for (size_t p = 0; p < l->nplaced; p++) {
        size_t i = l->placed_items[p];
        if (l->x[i] < x + l->cw && l->x[i] + l->w[i] > x && l->y[i] < y + h && l->y[i] + l->h[i] > y) return true;
    }
    return false;
}

/* The right edge of a placed icon a cell at (x, y), w by h, would cover; 0: none. */
static double placed_edge(icon_layout *l, double x, double y, double w, double h)
{
    double e = 0;
    for (size_t p = 0; p < l->nplaced; p++) {
        size_t i = l->placed_items[p];
        if (l->x[i] < x + w && l->x[i] + l->w[i] > x && l->y[i] < y + h && l->y[i] + l->h[i] > y && l->x[i] + l->w[i] > e)
            e = l->x[i] + l->w[i];
    }
    return e;
}

/* The free layout's flow, from spot s0 (the first of its row) on: each
 * given spot at the next x, wrapping at the width, rows as tall as their
 * tallest; spots with an icon clear of placed ones. */
static void flow(icon_layout *l, size_t s0, size_t n)
{
    size_t r = 0;
    double y = 0;
    if (s0 > 0 && l->nrows) {   /* from the row it was in; a spot past the flowed ones: the last row */
        r = s0 < l->flowed_end ? l->spot_row[s0] : l->nrows - 1;
        if (r >= l->nrows) r = l->nrows - 1;
        s0 = l->rowstart[r];
        y = l->rowy[r];
    } else {
        s0 = 0;
    }
    double x = 0, rh = 0;
    bool open = false;   /* row r has spots */
    double fw = l->flow_w > 0 ? l->flow_w : (double)l->cols * l->cw;
    for (size_t s = s0; s < l->spot_end; s++) {
        size_t i = l->spot_item[s];
        bool has = i != SIZE_MAX && i < n && l->spot_of[i] == (int64_t)s;
        if (!has && !l->spot_taken[s]) continue;   /* (never given) */
        if (has) l->spot_w[s] = l->w[i];
        double ws = l->spot_w[s], hs = has ? l->h[i] : l->ch;
        for (;;) {
            if (x > 0 && x + ws > fw) {   /* the next row */
                ensure_rows(l, r + 1);
                l->rowh[r] = (float)(rh > 0 ? rh : l->ch);
                y += l->rowh[r];
                r++;
                x = 0;
                rh = 0;
                open = false;
            }
            double e = has && l->nplaced ? placed_edge(l, x, y, ws, hs) : 0;
            if (e <= x) break;
            x = e;   /* past the placed icon, and again */
        }
        ensure_rows(l, r + 1);
        if (!open) { l->rowstart[r] = s; l->rowy[r] = y; open = true; }
        l->spot_x[s] = (float)x;
        l->spot_row[s] = (uint32_t)r;
        if (hs > rh) rh = hs;
        if (has) { l->x[i] = (float)x; l->y[i] = (float)y; }
        x += ws;
    }
    ensure_rows(l, r + 1);
    if (open) { l->rowh[r] = (float)(rh > 0 ? rh : l->ch); r++; }
    l->nrows = r;
    l->rowy[r] = y + (open ? l->rowh[r - 1] : 0);
    l->rowstart[r] = l->spot_end;
    l->ry_ok = r;
    l->flowed_end = l->spot_end;
}

static void give_spot(icon_layout *l, size_t i, int64_t spot)
{
    ensure_spots(l, (size_t)spot + 1);
    l->spot_item[spot] = i;
    l->spot_of[i] = spot;
    if ((size_t)spot + 1 > l->spot_end) l->spot_end = (size_t)spot + 1;
    if (!l->free) row_grows(l, (size_t)spot / (size_t)l->cols, l->h[i]);
}

void layout_update(icon_layout *l, size_t n, layout_name_fn name_at, void *ctx, bool full, const size_t *order)
{
    size_t from = full ? 0 : l->n;
    size_t first_row = SIZE_MAX;   /* the rows from here on moved (not full) */
    size_t first_spot = SIZE_MAX;  /* (free: the first spot given) */
    if (full) {
        for (size_t s = 0; s < l->nspots; s++) l->spot_item[s] = SIZE_MAX;
        l->nplaced = 0;
        rows_reset(l);
    }
    grow_items(l, n);
    size_t *pending = NULL, npending = 0;
    if (n > from) pending = malloc((n - from) * sizeof *pending);
    /* placed icons, and those with a spot already */
    for (size_t k = from; k < n; k++) {
        size_t i = order ? order[k] : k;
        const char *name = name_at(ctx, i);
        l->w[i] = (float)item_w(l, i);
        l->h[i] = (float)item_h(l, i);
        int64_t *pv = name ? map_slot(&l->placed, name, false) : NULL;
        if (pv) {
            l->x[i] = (float)(int32_t)(*pv >> 32);
            l->y[i] = (float)(int32_t)(uint32_t)*pv;
            l->spot_of[i] = -1;
            if (l->nplaced == l->capplaced) {
                l->capplaced = l->capplaced ? l->capplaced * 2 : 64;
                l->placed_items = realloc(l->placed_items, l->capplaced * sizeof *l->placed_items);
            }
            l->placed_items[l->nplaced++] = i;
            continue;
        }
        int64_t *sv = name ? map_slot(&l->slots, name, false) : NULL;
        int64_t had = full && i < l->n ? l->spot_of[i] : -1;
        if (sv && *sv >= 0) {
            give_spot(l, i, *sv);
            if ((size_t)*sv < first_spot) first_spot = (size_t)*sv;
            size_t r = (size_t)*sv / (size_t)l->cols;
            if (r < first_row) first_row = r;
        } else if (had >= 0 && (size_t)had < l->nspots && l->spot_item[had] == SIZE_MAX &&
                   (!name || l->spot_taken[had] == SPOT_NAMELESS)) {
            /* A spot given while its name was not known (a big folder keeps
             * few pages of names): it keeps it, and the name gets it now.
             * (It was given the next spot after all the others: the top of
             * a big folder opened empty.) */
            if (name) { *map_slot(&l->slots, name, true) = had; l->spot_taken[had] = 1; }
            give_spot(l, i, had);
            if ((size_t)had < first_spot) first_spot = (size_t)had;
            size_t r = (size_t)had / (size_t)l->cols;
            if (r < first_row) first_row = r;
        } else {
            pending[npending++] = i;
        }
    }
    /* new ones: the next free spots, in order, clear of placed icons */
    for (size_t k = 0; k < npending; k++) {
        size_t i = pending[k];
        const char *name = name_at(ctx, i);
        for (;; l->next_spot++) {
            ensure_spots(l, l->next_spot + 1);
            if (l->spot_taken[l->next_spot]) continue;
            size_t r = l->next_spot / (size_t)l->cols, c = l->next_spot % (size_t)l->cols;
            if (l->free || !l->nplaced || !over_placed(l, (double)c * l->cw, row_y(l, r), l->h[i])) break;   /* (free: the flow clears them) */
            l->spot_taken[l->next_spot] = 1;   /* under a placed icon */
        }
        int64_t spot = (int64_t)l->next_spot++;
        l->spot_taken[spot] = name ? 1 : SPOT_NAMELESS;
        if (name) *map_slot(&l->slots, name, true) = spot;
        if (l->on_new && name) l->on_new(l->on_new_ctx, name);
        give_spot(l, i, spot);
        if ((size_t)spot < first_spot) first_spot = (size_t)spot;
        size_t r = (size_t)spot / (size_t)l->cols;
        if (r < first_row) first_row = r;
    }
    free(pending);
    l->n = n;
    if (l->free) {
        if (full) flow(l, 0, n);
        else if (first_spot != SIZE_MAX) flow(l, first_spot, n);
        l->max_x = l->max_y = 0;
        for (size_t r = l->nrows; r > 0; r--) {   /* (the last row with an icon) */
            bool used = false;
            for (size_t s = l->rowstart[r - 1]; s < l->rowstart[r] && !used; s++) {
                size_t i = l->spot_item[s];
                used = i != SIZE_MAX && i < n && l->spot_of[i] == (int64_t)s;
            }
            if (used) { l->max_y = l->rowy[r - 1] + l->rowh[r - 1]; break; }
        }
        for (size_t s = 0; s < l->spot_end; s++) {
            size_t i = l->spot_item[s];
            if (i != SIZE_MAX && i < n && l->spot_of[i] == (int64_t)s && l->spot_x[s] + l->w[i] > l->max_x) l->max_x = l->spot_x[s] + l->w[i];
        }
        for (size_t p = 0; p < l->nplaced; p++) {
            size_t i = l->placed_items[p];
            if (l->x[i] + l->w[i] > l->max_x) l->max_x = l->x[i] + l->w[i];
            if (l->y[i] + l->h[i] > l->max_y) l->max_y = l->y[i] + l->h[i];
        }
        return;
    }
    /* where the spots are now: rows as tall as their tallest */
    row_y(l, l->nrows);
    size_t s0 = full || first_row == SIZE_MAX ? 0 : first_row * (size_t)l->cols;
    if (!full && first_row == SIZE_MAX) s0 = l->nspots;
    for (size_t s = s0; s < l->nspots; s++) {
        size_t i = l->spot_item[s];
        if (i == SIZE_MAX || i >= n || l->spot_of[i] != (int64_t)s) continue;
        l->x[i] = (float)((double)(s % (size_t)l->cols) * l->cw);
        l->y[i] = (float)l->rowy[s / (size_t)l->cols];
    }
    /* the content's size */
    l->max_x = l->max_y = 0;
    for (size_t r = l->nrows; r > 0; r--) {
        bool used = false;
        for (size_t c = 0; c < (size_t)l->cols && !used; c++) {
            size_t s = (r - 1) * (size_t)l->cols + c;
            used = s < l->nspots && l->spot_item[s] != SIZE_MAX && l->spot_item[s] < n;
        }
        if (used) { l->max_y = l->rowy[r - 1] + l->rowh[r - 1]; break; }
    }
    if (l->max_y > 0) l->max_x = (double)l->cols * l->cw;
    for (size_t p = 0; p < l->nplaced; p++) {
        size_t i = l->placed_items[p];
        if (l->x[i] + l->w[i] > l->max_x) l->max_x = l->x[i] + l->w[i];
        if (l->y[i] + l->h[i] > l->max_y) l->max_y = l->y[i] + l->h[i];
    }
}

void layout_pos(icon_layout *l, size_t i, double *x, double *y)
{
    if (i >= l->n) { *x = *y = 0; return; }
    *x = l->x[i];
    *y = l->y[i];
}

double layout_item_h(icon_layout *l, size_t i) { return i < l->n ? l->h[i] : l->ch; }
double layout_item_w(icon_layout *l, size_t i) { return i < l->n ? l->w[i] : l->cw; }

/* The first row whose bottom is below y. */
static size_t row_at_y(icon_layout *l, double y)
{
    size_t lo = 0, hi = l->nrows;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (l->rowy[m] + l->rowh[m] <= y) lo = m + 1;
        else hi = m;
    }
    return lo;
}

size_t layout_items_in(icon_layout *l, double rx, double ry, double rw, double rh, size_t *out, size_t max)
{
    size_t k = 0;
    /* Cells are [c*w, (c+1)*w): those that overlap [rx, rx+rw), not just touch it. */
    if (rw <= 0 || rh <= 0) return 0;
    if (l->free) {
        for (size_t r = row_at_y(l, ry); r < l->nrows && l->rowy[r] < ry + rh && k < max; r++)
            for (size_t s = l->rowstart[r]; s < l->rowstart[r + 1] && k < max; s++) {
                size_t i = l->spot_item[s];
                if (i == SIZE_MAX || i >= l->n || l->spot_of[i] != (int64_t)s) continue;
                if (l->x[i] < rx + rw && l->x[i] + l->w[i] > rx && l->y[i] < ry + rh && l->y[i] + l->h[i] > ry) out[k++] = i;
            }
        for (size_t p = 0; p < l->nplaced && k < max; p++) {
            size_t i = l->placed_items[p];
            if (l->x[i] < rx + rw && l->x[i] + l->w[i] > rx && l->y[i] < ry + rh && l->y[i] + l->h[i] > ry) out[k++] = i;
        }
        return k;
    }
    long c0 = (long)floor(rx / l->cw), c1 = (long)floor((rx + rw) / l->cw - 1e-9);
    if (c0 < 0) c0 = 0;
    if (c1 >= l->cols) c1 = l->cols - 1;
    row_y(l, l->nrows);
    for (size_t r = row_at_y(l, ry); r < l->nrows && l->rowy[r] < ry + rh && k < max; r++)
        for (long c = c0; c <= c1 && k < max; c++) {
            size_t s = r * (size_t)l->cols + (size_t)c;
            if (s >= l->nspots) { r = l->nrows; break; }
            size_t i = l->spot_item[s];
            if (i != SIZE_MAX && i < l->n && l->spot_of[i] == (int64_t)s) out[k++] = i;
        }
    for (size_t p = 0; p < l->nplaced && k < max; p++) {
        size_t i = l->placed_items[p];
        if (l->x[i] < rx + rw && l->x[i] + l->w[i] > rx && l->y[i] < ry + rh && l->y[i] + l->h[i] > ry) out[k++] = i;
    }
    return k;
}

void layout_size(icon_layout *l, double *w, double *h)
{
    double gw = l->free ? 0 : (double)l->cols * l->cw;
    *w = l->max_x > gw ? l->max_x : gw;
    *h = l->max_y;
}
