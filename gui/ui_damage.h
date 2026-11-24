/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Damage planning for buffer reuse (gui-design.md, G0 findings).
 *
 * Each surface keeps the changes of its last few frames: rectangles that
 * must be redrawn, and at most one scroll (an area whose contents moved up
 * by dy device pixels; dy < 0 moves them down). A buffer that was last
 * drawn `age` frames ago is brought up to date by moving its scrolled area
 * once by the net amount, then redrawing the planned rectangles. A buffer
 * too old, or a history with a full redraw or scrolls of different areas,
 * is redrawn whole.
 *
 * All coordinates are in device pixels (integers), so moves are exact.
 */
#ifndef UI_DAMAGE_H
#define UI_DAMAGE_H

#include <stdbool.h>

typedef struct { int x, y, w, h; } ui_irect;

#define UI_DAMAGE_HISTORY 4
#define UI_DAMAGE_RECTS 32

typedef struct {
    bool full;
    bool scrolled;
    ui_irect area;          /* scrolled area */
    int dy;                 /* contents moved up by dy */
    ui_irect rects[UI_DAMAGE_RECTS];
    int nrects;
} ui_damage_frame;

/* Record into the frame being built. */
void ui_damage_add(ui_damage_frame *f, ui_irect r, int surface_w, int surface_h);
void ui_damage_scroll(ui_damage_frame *f, ui_irect area, int dy);

typedef struct {
    bool full;
    bool move;              /* move `area` up by `dy` before drawing */
    ui_irect area;
    int dy;
    ui_irect rects[UI_DAMAGE_RECTS * UI_DAMAGE_HISTORY + 2];
    int nrects;
} ui_damage_plan;

/* frames[0..n-1]: oldest first; the last one is the frame being drawn.
 * `age`: how many of them the buffer has not seen (0 or more than n means
 * the buffer has no usable contents). */
void ui_damage_make_plan(const ui_damage_frame *frames, int n, int age, int surface_w, int surface_h,
                         ui_damage_plan *out);

#endif
