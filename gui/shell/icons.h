/*
 * Objects' own icons (G4): loading them, and making them from any image.
 *
 * An object's icon is a PNG in WPS's icons folder ($XDG_DATA_HOME/wps/
 * icons), named after the image it was made from and when, so a new icon
 * never has the old one's name (nothing cached goes stale). Any image
 * GNOME's glycin reads (PNG, JPEG, SVG, ICO, WebP, JPEG XL, ...) is
 * made into a 128-pixel PNG there by glycin-thumbnailer, off the UI
 * thread; the source can then go.
 */
#ifndef WPS_ICONS_H
#define WPS_ICONS_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

void icons_init(ui_app *a);

/* The icon at `png` (cached; NULL if it cannot be read). */
cairo_surface_t *icons_get(const char *png);

/* Make an icon from the image at `src`; done(ctx, path of the PNG) on the
 * UI thread, or done(ctx, NULL) when it cannot be read. */
void icons_import(const char *src, void (*done)(void *ctx, const char *png), void *ctx);
/* Why the last import failed (for its notebook's status line). */
const char *icons_import_error(void);

/* A PNG to show `src` (any image) at about `size` pixels: true with its
 * path in out when there is one now (a PNG itself, or made before);
 * otherwise false, and it is being made: done(ctx, path or NULL) later. */
bool icons_preview(const char *src, int size, char *out, size_t n, void (*done)(void *ctx, const char *png), void *ctx);

/* WPS's icon library folder (made if needed): images there are offered
 * by the icon picker. */
const char *icons_library_dir(void);

#endif
