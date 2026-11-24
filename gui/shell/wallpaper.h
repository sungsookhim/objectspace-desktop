/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * The desktop's background: GNOME's (org.gnome.desktop.background, with
 * the dark picture when the colour scheme prefers dark), so that it is
 * the same in both (gui-design.md: Settings shared with GNOME).
 *
 * Pictures are decoded by glycin-thumbnailer (GNOME's sandboxed image
 * loaders: JPEG XL, AVIF, WebP, HEIC, ...) in the background, once per
 * picture and size, into a PNG cache in $XDG_CACHE_HOME/objectspace; PNG files
 * are read directly. Until a picture is ready, and without one, the
 * background is GNOME's primary colour. Changes made in GNOME Settings
 * are followed through `gsettings monitor`.
 */
#ifndef OSD_WALLPAPER_H
#define OSD_WALLPAPER_H

#include <cairo.h>

#include "ui.h"

/* `changed` runs when there is something new to draw. */
void wallpaper_start(ui_app *a, void (*changed)(void *ctx), void *ctx);
void wallpaper_stop(void);

/* The desktop's size in logical pixels, and its scale. */
void wallpaper_set_size(double w, double h, int scale);

/* Draw the part `clip` of the background (logical coordinates, the
 * desktop's top left at 0,0). */
void wallpaper_draw(cairo_t *cr, ui_rect clip);

/* The desktop's own background (its Background page, v0.3-design 3)
 * instead of GNOME's: a picture's path ("" none), how it is shown (0
 * centred, 1 tiled, 2 scaled to fill, 3 scaled to fit, 4 stretched) and a
 * colour ("#rrggbb", "" OS/2's teal). on false: GNOME's again. */
void wallpaper_own(bool on, const char *path, int mode, const char *color);

/* The scheme changed: the desktop's colour (no picture) is read again if
 * the scheme's is another. */
void wallpaper_scheme_changed(void);

/* For tests: what is shown now ("color", or the picture's path). */
const char *wallpaper_showing(void);

#endif
