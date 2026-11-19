/*
 * The desktop's background: GNOME's (org.gnome.desktop.background, with
 * the dark picture when the colour scheme prefers dark), so that it is
 * the same in both (gui-design.md: Settings shared with GNOME).
 *
 * Pictures are decoded by glycin-thumbnailer (GNOME's sandboxed image
 * loaders: JPEG XL, AVIF, WebP, HEIC, ...) in the background, once per
 * picture and size, into a PNG cache in $XDG_CACHE_HOME/wps; PNG files
 * are read directly. Until a picture is ready, and without one, the
 * background is GNOME's primary colour. Changes made in GNOME Settings
 * are followed through `gsettings monitor`.
 */
#ifndef WPS_WALLPAPER_H
#define WPS_WALLPAPER_H

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

/* For tests: what is shown now ("color", or the picture's path). */
const char *wallpaper_showing(void);

#endif
