/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Icon layout: spots kept by name, placed icons, area queries. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "layout.h"

static const char **names;
static const char *name_at(void *ctx, size_t i) { (void)ctx; return names[i]; }

static int found(icon_layout *l, double x, double y, double w, double h, size_t want)
{
    size_t out[64], n = layout_items_in(l, x, y, w, h, out, 64);
    for (size_t i = 0; i < n; i++) if (out[i] == want) return 1;
    return 0;
}

TEST(spots_follow_names_not_indices)
{
    const char *a[] = { "a", "b", "c", "d" };
    names = a;
    icon_layout *l = layout_new(100, 70);
    layout_set_cols(l, 2);
    layout_update(l, 4, name_at, NULL, true, NULL);
    double x, y;
    layout_pos(l, 3, &x, &y);
    CHECK(x == 100 && y == 70);                  /* d: second column, second row */
    /* The folder is read again in another order, with b gone and e new. */
    const char *b2[] = { "d", "e", "a", "c" };
    names = b2;
    layout_update(l, 4, name_at, NULL, true, NULL);
    layout_pos(l, 0, &x, &y);
    CHECK(x == 100 && y == 70);                  /* d did not move */
    layout_pos(l, 1, &x, &y);
    CHECK(x == 0 && y == 140);                   /* e: the next free spot; b's stays a gap */
    CHECK(!found(l, 100, 0, 100, 70, 0) && !found(l, 100, 0, 100, 70, 1) && !found(l, 100, 0, 100, 70, 2) &&
          !found(l, 100, 0, 100, 70, 3));        /* b's spot is empty */
    layout_free(l);
}

TEST(placed_icons_keep_their_place_and_block_spots)
{
    const char *a[] = { "x", "y", "z" };
    names = a;
    icon_layout *l = layout_new(100, 70);
    layout_set_cols(l, 3);
    layout_place(l, "y", 0, 0);                  /* y sits where the first spot would be */
    layout_update(l, 3, name_at, NULL, true, NULL);
    double x0, y0, x1, y1, x2, y2;
    layout_pos(l, 0, &x0, &y0);
    layout_pos(l, 1, &x1, &y1);
    layout_pos(l, 2, &x2, &y2);
    CHECK(x1 == 0 && y1 == 0);
    CHECK(!(x0 == 0 && y0 == 0) && !(x2 == 0 && y2 == 0));   /* nobody handed y's spot */
    CHECK(found(l, 10, 10, 1, 1, 1));
    layout_place(l, "z", 1234, 567);             /* moved far away */
    layout_update(l, 3, name_at, NULL, true, NULL);
    double w, h;
    layout_size(l, &w, &h);
    CHECK(w >= 1334 && h >= 637);
    CHECK(found(l, 1240, 570, 10, 10, 2));
    layout_free(l);
}

TEST(a_million_names)
{
    size_t n = 1000000;
    char *buf = malloc(n * 12);
    names = malloc(n * sizeof *names);
    for (size_t i = 0; i < n; i++) { sprintf(buf + i * 12, "f%07zu", i); names[i] = buf + i * 12; }
    icon_layout *l = layout_new(100, 70);
    layout_set_cols(l, 10);
    layout_update(l, n / 2, name_at, NULL, false, NULL);
    layout_update(l, n, name_at, NULL, false, NULL);     /* streamed in two parts */
    double x, y;
    layout_pos(l, 999999, &x, &y);
    CHECK(x == 900 && y == 99999 * 70.0);
    CHECK(found(l, 900, 99999 * 70.0, 50, 10, 999999));
    layout_free(l);
    free(names);
    free(buf);
}

static double heights[8];
static double height_at(void *ctx, size_t i) { (void)ctx; return heights[i]; }

TEST(rows_as_tall_as_their_tallest_title)
{
    const char *a[] = { "a", "b", "c", "d", "e", "f" };
    names = a;
    double hs[] = { 70, 118, 70, 70, 70, 86 };
    memcpy(heights, hs, sizeof hs);
    icon_layout *l = layout_new(100, 70);
    layout_set_height_fn(l, height_at, NULL);
    layout_set_cols(l, 3);
    layout_update(l, 6, name_at, NULL, true, NULL);
    double x, y, w, h;
    layout_pos(l, 3, &x, &y);
    CHECK(x == 0 && y == 118);                   /* row 2 below b's whole title */
    layout_pos(l, 5, &x, &y);
    CHECK(x == 200 && y == 118);
    CHECK(layout_item_h(l, 1) == 118);
    layout_size(l, &w, &h);
    CHECK(h == 118 + 86);                        /* the second row as tall as f */
    CHECK(found(l, 110, 100, 10, 10, 1) && !found(l, 110, 100, 10, 10, 4));   /* b's title, not e */
    CHECK(found(l, 10, 130, 10, 10, 3));
    /* streamed: a later item with a long title moves only the rows below it */
    const char *b[] = { "a", "b", "c", "d", "e", "f", "g" };
    names = b;
    heights[6] = 150;
    layout_update(l, 7, name_at, NULL, false, NULL);
    layout_pos(l, 6, &x, &y);
    CHECK(x == 0 && y == 204);
    layout_size(l, &w, &h);
    CHECK(h == 204 + 150);
    layout_free(l);
}

TEST(locked_icons_stay_when_arranged)
{
    const char *a[] = { "a", "b", "c" };
    names = a;
    icon_layout *l = layout_new(100, 70);
    layout_set_cols(l, 3);
    layout_place(l, "a", 300, 300);
    layout_place(l, "b", 0, 0);
    layout_lock(l, "b", true);
    layout_update(l, 3, name_at, NULL, true, NULL);
    CHECK(layout_is_locked(l, "b") && !layout_is_locked(l, "a"));
    layout_reset(l, 3);                          /* Arrange */
    layout_update(l, 3, name_at, NULL, true, NULL);
    double x, y;
    layout_pos(l, 1, &x, &y);
    CHECK(x == 0 && y == 0);                     /* b stayed */
    layout_pos(l, 0, &x, &y);
    CHECK(x == 100 && y == 0);                   /* a flowed around it */
    layout_pos(l, 2, &x, &y);
    CHECK(x == 200 && y == 0);
    layout_lock(l, "b", false);
    CHECK(!layout_is_locked(l, "b"));
    layout_lock_all(l, true);
    CHECK(layout_is_locked(l, "c"));
    layout_free(l);
}

/* A big folder keeps few pages of names: icons laid out while their
 * names were not known keep their spots through a full update (they were
 * moved after all the others: the top of the folder empty). */
static size_t known_from, known_to;
static char bigname[1000][8];
static const char *paged_name(void *ctx, size_t i) { (void)ctx; return i >= known_from && i < known_to ? bigname[i] : NULL; }

TEST(spots_kept_while_names_are_not_known)
{
    for (size_t i = 0; i < 1000; i++) snprintf(bigname[i], sizeof bigname[i], "n%zu", i);
    icon_layout *l = layout_new(100, 70);
    layout_set_cols(l, 5);
    known_from = 0, known_to = 128;               /* the first page's names */
    layout_update(l, 1000, paged_name, NULL, true, NULL);
    double x, y;
    layout_pos(l, 500, &x, &y);
    CHECK(x == 0 && y == 100 * 70);               /* item 500: row 100 */
    known_from = 384, known_to = 512;             /* another page now */
    layout_update(l, 1000, paged_name, NULL, true, NULL);
    layout_pos(l, 0, &x, &y);
    CHECK(x == 0 && y == 0);                      /* the first did not move */
    layout_pos(l, 500, &x, &y);
    CHECK(x == 0 && y == 100 * 70);               /* nor one named now */
    layout_pos(l, 999, &x, &y);
    CHECK(x == 400 && y == 199 * 70);             /* nor the last */
    CHECK(found(l, 0, 0, 100, 70, 0));            /* the top shows them */
    /* Arrange starts the grid again (none kept) */
    layout_reset(l, 4);
    layout_update(l, 1000, paged_name, NULL, true, NULL);
    layout_pos(l, 5, &x, &y);
    CHECK(x == 100 && y == 70);                   /* 4 columns now */
    layout_free(l);
}

/* The free layout (0.4 step 4): items as wide as their titles, packed to
 * the width, rows as tall as their tallest. */
static const double *widths, *fheights;
static const char **fnames;
static size_t fname_index(const char *n) { size_t i = 0; while (strcmp(fnames[i], n)) i++; return i; }
static double w_at(void *ctx, size_t i) { (void)ctx; return widths[fname_index(names[i])]; }
static double h_at(void *ctx, size_t i) { (void)ctx; return fheights[fname_index(names[i])]; }

static icon_layout *free_layout(double width)
{
    icon_layout *l = layout_new(100, 80);
    layout_set_free(l, true);
    layout_set_width_fn(l, w_at, NULL);
    layout_set_height_fn(l, h_at, NULL);
    layout_set_width(l, width);
    layout_set_cols(l, 3);
    return l;
}

TEST(free_packing_by_width)
{
    static const char *all[] = { "a", "b", "c", "d", "e" };
    static const double w[] = { 50, 150, 80, 200, 60 }, h[] = { 80, 120, 80, 80, 80 };
    fnames = all; widths = w; fheights = h;
    const char *a[] = { "a", "b", "c", "d" };
    names = a;
    icon_layout *l = free_layout(300);
    layout_update(l, 4, name_at, NULL, true, NULL);
    double x, y;
    layout_pos(l, 1, &x, &y);
    CHECK(x == 50 && y == 0);                    /* b after a */
    layout_pos(l, 2, &x, &y);
    CHECK(x == 200 && y == 0);                   /* c: 280 of 300 */
    layout_pos(l, 3, &x, &y);
    CHECK(x == 0 && y == 120);                   /* d wraps; the row as tall as b */
    CHECK(layout_item_w(l, 3) == 200);
    CHECK(found(l, 260, 10, 1, 1, 2) && !found(l, 260, 10, 1, 1, 1));
    double cw, ch;
    layout_size(l, &cw, &ch);
    CHECK(cw == 280 && ch == 200);
    /* one more, added: after d, nothing before it moves */
    const char *b[] = { "a", "b", "c", "d", "e" };
    names = b;
    layout_update(l, 5, name_at, NULL, false, NULL);
    layout_pos(l, 4, &x, &y);
    CHECK(x == 200 && y == 120);
    layout_pos(l, 2, &x, &y);
    CHECK(x == 200 && y == 0);
    /* b gone: its spot keeps its width (c, d, e stay in their places) */
    const char *c[] = { "a", "c", "d", "e" };
    names = c;
    layout_update(l, 4, name_at, NULL, true, NULL);
    layout_pos(l, 1, &x, &y);
    CHECK(x == 200 && y == 0);
    layout_pos(l, 3, &x, &y);
    CHECK(x == 200 && y == 80);                  /* (the row as tall as what is left in it) */
    /* Arrange: packed again, without the gap */
    layout_reset(l, 3);
    layout_update(l, 4, name_at, NULL, true, NULL);
    layout_pos(l, 1, &x, &y);
    CHECK(x == 50 && y == 0);                    /* c after a */
    layout_pos(l, 2, &x, &y);
    CHECK(x == 0 && y == 80);                    /* d: 130 + 200 > 300 */
    layout_free(l);
}

TEST(free_flows_around_placed_icons)
{
    static const char *all[] = { "p", "a", "b" };
    static const double w[] = { 100, 120, 120 }, h[] = { 80, 80, 80 };
    fnames = all; widths = w; fheights = h;
    const char *a[] = { "p", "a", "b" };
    names = a;
    icon_layout *l = free_layout(300);
    layout_place(l, "p", 0, 0);
    layout_update(l, 3, name_at, NULL, true, NULL);
    double x, y;
    layout_pos(l, 1, &x, &y);
    CHECK(x == 100 && y == 0);                   /* a: past p */
    layout_pos(l, 2, &x, &y);
    CHECK(x == 0 && y == 80);                    /* b: 220 + 120 > 300 */
    layout_pos(l, 0, &x, &y);
    CHECK(x == 0 && y == 0);                     /* p stays */
    layout_free(l);
}

int main(void)
{
    RUN(spots_kept_while_names_are_not_known);
    RUN(spots_follow_names_not_indices);
    RUN(placed_icons_keep_their_place_and_block_spots);
    RUN(a_million_names);
    RUN(rows_as_tall_as_their_tallest_title);
    RUN(locked_icons_stay_when_arranged);
    RUN(free_packing_by_width);
    RUN(free_flows_around_placed_icons);
    return CHECK_SUMMARY();
}
