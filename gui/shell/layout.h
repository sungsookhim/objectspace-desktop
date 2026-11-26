/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Icon view layout with icons placed anywhere (layout.c). */
#ifndef OSD_SHELL_LAYOUT_H
#define OSD_SHELL_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct icon_layout icon_layout;
typedef const char *(*layout_name_fn)(void *ctx, size_t i);   /* NULL if not known yet */
typedef double (*layout_height_fn)(void *ctx, size_t i);      /* item i's cell height */
typedef double (*layout_width_fn)(void *ctx, size_t i);       /* item i's cell width (free layout) */

icon_layout *layout_new(double cell_w, double cell_h);
void layout_free(icon_layout *l);
/* Items' heights (whole titles: rows as tall as their tallest); without
 * one, every item is as tall as a cell. */
void layout_set_height_fn(icon_layout *l, layout_height_fn fn, void *ctx);
/* The free layout (OS/2's): items as wide as the width function says,
 * packed into rows to the width given (layout_set_width, taken when the
 * columns are: Arrange, a new cell); off: the grid. A change takes a full
 * update (and, to look as it should, an Arrange). */
void layout_set_free(icon_layout *l, bool on);
bool layout_is_free(icon_layout *l);
void layout_set_width_fn(icon_layout *l, layout_width_fn fn, void *ctx);
void layout_set_width(icon_layout *l, double w);
/* Arrange: forget spots and stored positions, but locked ones; a new grid width. */
void layout_reset(icon_layout *l, int cols);
/* The grid width, while nothing has been laid out yet. */
void layout_set_cols(icon_layout *l, int cols);
/* The icon set's cell and the columns that fit now: the spots keep their
 * order, flowed again; placed positions are the caller's to set again. */
void layout_set_cell(icon_layout *l, double cell_w, double cell_h, int cols);
int layout_cols(icon_layout *l);
/* Where the layout's 0,0 is in the window (the desktop: its primary
 * screen's corner); positions in and out are the window's. */
void layout_set_origin(icon_layout *l, double x, double y);
void layout_place(icon_layout *l, const char *name, int x, int y);   /* a stored position */
void layout_clear_placed(icon_layout *l);
bool layout_is_placed(icon_layout *l, const char *name);
/* Locked icons keep their places when arranged (all of them: lock_all).
 * Locks are cleared with the stored positions (layout_clear_placed). */
void layout_lock(icon_layout *l, const char *name, bool on);
bool layout_is_locked(icon_layout *l, const char *name);
void layout_lock_all(icon_layout *l, bool on);
bool layout_all_locked(icon_layout *l);
void layout_rename(icon_layout *l, const char *from, const char *to);   /* keeps its place */
/* Called when a name gets a grid spot for the first time (not when arranging). */
void layout_on_new_spot(icon_layout *l, void (*fn)(void *ctx, const char *name), void *ctx);
/* Lay out items 0..n-1: all of them (full), or only those added since the
 * last call. `order` (full only) gives the order spots are handed out. */
void layout_update(icon_layout *l, size_t n, layout_name_fn name_at, void *ctx, bool full, const size_t *order);
void layout_pos(icon_layout *l, size_t i, double *x, double *y);
double layout_item_h(icon_layout *l, size_t i);
double layout_item_w(icon_layout *l, size_t i);
size_t layout_items_in(icon_layout *l, double x, double y, double w, double h, size_t *out, size_t max);
void layout_size(icon_layout *l, double *w, double *h);

#endif
