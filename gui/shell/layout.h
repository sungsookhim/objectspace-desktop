/* Icon view layout with icons placed anywhere (layout.c). */
#ifndef WPS_SHELL_LAYOUT_H
#define WPS_SHELL_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct icon_layout icon_layout;
typedef const char *(*layout_name_fn)(void *ctx, size_t i);   /* NULL if not known yet */

icon_layout *layout_new(double cell_w, double cell_h);
void layout_free(icon_layout *l);
/* Arrange: forget spots and stored positions; a new grid width. */
void layout_reset(icon_layout *l, int cols);
/* The grid width, while nothing has been laid out yet. */
void layout_set_cols(icon_layout *l, int cols);
int layout_cols(icon_layout *l);
void layout_place(icon_layout *l, const char *name, int x, int y);   /* a stored position */
void layout_clear_placed(icon_layout *l);
bool layout_is_placed(icon_layout *l, const char *name);
void layout_rename(icon_layout *l, const char *from, const char *to);   /* keeps its place */
/* Called when a name gets a grid spot for the first time (not when arranging). */
void layout_on_new_spot(icon_layout *l, void (*fn)(void *ctx, const char *name), void *ctx);
/* Lay out items 0..n-1: all of them (full), or only those added since the
 * last call. `order` (full only) gives the order spots are handed out. */
void layout_update(icon_layout *l, size_t n, layout_name_fn name_at, void *ctx, bool full, const size_t *order);
void layout_pos(icon_layout *l, size_t i, double *x, double *y);
size_t layout_items_in(icon_layout *l, double x, double y, double w, double h, size_t *out, size_t max);
void layout_size(icon_layout *l, double *w, double *h);

#endif
