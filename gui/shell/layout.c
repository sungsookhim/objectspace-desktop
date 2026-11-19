/*
 * Icon view layout with icons placed anywhere, as in OS/2 (G2).
 *
 * Icons the user placed keep their stored positions (by name here; the
 * service stores them by object ID). Every other icon gets a grid spot
 * the first time its name is seen, and keeps it for the session: a new
 * file takes the next free spot, a deleted one leaves a gap, and resizing
 * the window moves nothing. Arrange starts the grid again.
 *
 * Finding the icons in an area is grid arithmetic for the unplaced ones
 * and a scan of the placed ones (few, unless arranged and stored).
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

struct icon_layout {
    int cols;
    double cw, ch;
    struct map slots;        /* name -> grid spot (this session) */
    struct map placed;       /* name -> (x << 32 | y) stored positions */
    uint8_t *spot_taken;     /* spots given to a name, or under a placed icon */
    size_t nspots;
    size_t next_spot;
    float *x, *y;
    int64_t *spot_of;        /* per item: its spot, or -1 if placed */
    size_t n, cap;
    size_t *spot_item;       /* spot -> item, SIZE_MAX if none now */
    size_t *placed_items;
    size_t nplaced, capplaced;
    double max_x, max_y;
};

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
    free(l->spot_taken);
    free(l->x);
    free(l->y);
    free(l->spot_of);
    free(l->spot_item);
    free(l->placed_items);
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
    l->nspots = m;
}

void layout_reset(icon_layout *l, int cols)
{
    map_free(&l->slots);
    map_free(&l->placed);
    l->cols = cols < 1 ? 1 : cols;
    if (l->spot_taken) memset(l->spot_taken, 0, l->nspots);
    l->next_spot = 0;
}

void layout_set_cols(icon_layout *l, int cols)
{
    if (l->slots.n == 0 && l->placed.n == 0) l->cols = cols < 1 ? 1 : cols;
}

void layout_place(icon_layout *l, const char *name, int x, int y)
{
    int64_t *v = map_slot(&l->placed, name, true);
    *v = ((int64_t)x << 32) | (uint32_t)y;
}

void layout_clear_placed(icon_layout *l) { map_free(&l->placed); }

bool layout_is_placed(icon_layout *l, const char *name) { return map_slot(&l->placed, name, false) != NULL; }

static void grow_items(icon_layout *l, size_t n)
{
    if (n <= l->cap) return;
    size_t c = l->cap ? l->cap : 256;
    while (c < n) c *= 2;
    l->x = realloc(l->x, c * sizeof *l->x);
    l->y = realloc(l->y, c * sizeof *l->y);
    l->spot_of = realloc(l->spot_of, c * sizeof *l->spot_of);
    l->cap = c;
}

/* Spots under placed icons are not handed out (they would overlap). */
static void block_under_placed(icon_layout *l)
{
    for (size_t i = 0; i < l->placed.cap; i++) {
        if (!l->placed.keys[i]) continue;
        int64_t v = l->placed.vals[i];
        double px = (double)(int32_t)(v >> 32), py = (double)(int32_t)(uint32_t)v;
        long c0 = (long)(px / l->cw), c1 = (long)((px + l->cw - 1) / l->cw);
        long r0 = (long)(py / l->ch), r1 = (long)((py + l->ch - 1) / l->ch);
        for (long r = r0 < 0 ? 0 : r0; r <= r1; r++)
            for (long c = c0 < 0 ? 0 : c0; c <= c1 && c < l->cols; c++) {
                size_t s = (size_t)r * (size_t)l->cols + (size_t)c;
                ensure_spots(l, s + 1);
                l->spot_taken[s] = 1;
            }
    }
}

static void lay_one(icon_layout *l, size_t i, const char *name)
{
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
    } else {
        int64_t *sv = name ? map_slot(&l->slots, name, true) : NULL;
        int64_t spot = sv ? *sv : -1;
        if (spot < 0) {
            for (;; l->next_spot++) {
                ensure_spots(l, l->next_spot + 1);
                if (!l->spot_taken[l->next_spot]) break;
            }
            spot = (int64_t)l->next_spot++;
            l->spot_taken[spot] = 1;
            if (sv) *sv = spot;
        }
        ensure_spots(l, (size_t)spot + 1);
        l->spot_item[spot] = i;
        l->spot_of[i] = spot;
        l->x[i] = (float)((double)(spot % l->cols) * l->cw);
        l->y[i] = (float)((double)(spot / l->cols) * l->ch);
    }
    if (l->x[i] + l->cw > l->max_x) l->max_x = l->x[i] + l->cw;
    if (l->y[i] + l->ch > l->max_y) l->max_y = l->y[i] + l->ch;
}

void layout_update(icon_layout *l, size_t n, layout_name_fn name_at, void *ctx, bool full, const size_t *order)
{
    size_t from = full ? 0 : l->n;
    if (full) {
        for (size_t s = 0; s < l->nspots; s++) l->spot_item[s] = SIZE_MAX;
        l->nplaced = 0;
        l->max_x = l->max_y = 0;
        block_under_placed(l);
    }
    grow_items(l, n);
    for (size_t k = from; k < n; k++) {
        size_t i = order ? order[k] : k;
        lay_one(l, i, name_at(ctx, i));
    }
    l->n = n;
}

void layout_pos(icon_layout *l, size_t i, double *x, double *y)
{
    if (i >= l->n) { *x = *y = 0; return; }
    *x = l->x[i];
    *y = l->y[i];
}

size_t layout_items_in(icon_layout *l, double rx, double ry, double rw, double rh, size_t *out, size_t max)
{
    size_t k = 0;
    /* Cells are [c*w, (c+1)*w): those that overlap [rx, rx+rw), not just touch it. */
    if (rw <= 0 || rh <= 0) return 0;
    long c0 = (long)floor(rx / l->cw), c1 = (long)floor((rx + rw) / l->cw - 1e-9);
    long r0 = (long)floor(ry / l->ch), r1 = (long)floor((ry + rh) / l->ch - 1e-9);
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 >= l->cols) c1 = l->cols - 1;
    for (long r = r0; r <= r1 && k < max; r++)
        for (long c = c0; c <= c1 && k < max; c++) {
            size_t s = (size_t)r * (size_t)l->cols + (size_t)c;
            if (s >= l->nspots) { r = r1 + 1; break; }
            size_t i = l->spot_item[s];
            if (i != SIZE_MAX && i < l->n && l->spot_of[i] == (int64_t)s) out[k++] = i;
        }
    for (size_t p = 0; p < l->nplaced && k < max; p++) {
        size_t i = l->placed_items[p];
        if (l->x[i] < rx + rw && l->x[i] + l->cw > rx && l->y[i] < ry + rh && l->y[i] + l->ch > ry) out[k++] = i;
    }
    return k;
}

void layout_size(icon_layout *l, double *w, double *h)
{
    double gw = (double)l->cols * l->cw;
    *w = l->max_x > gw ? l->max_x : gw;
    *h = l->max_y;
}
