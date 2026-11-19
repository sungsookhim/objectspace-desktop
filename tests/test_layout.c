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

int main(void)
{
    RUN(spots_follow_names_not_indices);
    RUN(placed_icons_keep_their_place_and_block_spots);
    RUN(a_million_names);
    return CHECK_SUMMARY();
}
