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
#ifndef OSD_ICONS_H
#define OSD_ICONS_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

void icons_init(ui_app *a);

/* The icon at `png` (cached; NULL if it cannot be read). */
cairo_surface_t *icons_get(const char *png);

/* A theme's icon (0.4 step 2): NAME (computer, folder, file-image, ...)
 * drawn at px pixels from the theme's package (by name or id; NULL or "":
 * the desktop's), else WPS (flat)'s; NULL if neither has it. Cached. */
cairo_surface_t *icons_theme(const char *theme, const char *name, int px);
/* The desktop's theme: menus, the LaunchPad and the WarpCenter use it. */
void icons_set_default_theme(const char *theme);
const char *icons_default_theme(void);
/* An object's icon: its own (a PNG, a .wpsicon package) or a theme's name
 * ("theme:NAME"), in `theme` (NULL: the desktop's), at px device pixels
 * (the package's exact size if it has one; wpsicon's rules otherwise). */
cairo_surface_t *icons_object(const char *theme, const char *icon, int px);
/* A theme's icon (NULL theme: the desktop's) drawn in r, centred, pad
 * inside its edges, at the pixels it covers; false if the theme has none. */
bool icons_draw_theme(cairo_t *cr, const char *theme, const char *name, ui_rect r, double pad);
/* An object's icon as everything shows it: its own or a theme's name
 * (icons_object), else its kind's from the theme (a folder, a program, a
 * data file by the type of `name`); NULL: none (the kind drawn). */
cairo_surface_t *icons_item(const char *theme, const char *icon, ui_kind kind, const char *name, int px);
/* The desktop's icon set (the System's): the toolkit's (menus, title bars,
 * every folder without its own) and the LaunchPad's, redrawn. */
void icons_set_default_size(ui_icon_size which);
/* The theme's icon name for a data file's type ("image/png": file-image). */
const char *icons_name_for_type(const char *mime);
/* A theme's icon as a PNG file (for what reads files: the notebook's Icon
 * field), made once in the cache; false if there is none. */
bool icons_theme_png(const char *theme, const char *name, int px, char *out, size_t n);
/* The same for any object's icon: a theme's name, or a .wpsicon package;
 * false for anything else (a PNG is one already). */
bool icons_object_png(const char *icon, int px, char *out, size_t n);

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
