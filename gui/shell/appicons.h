/*
 * Programs' own icons (v0.2 step 5): a desktop entry's Icon=, found in the
 * icon themes (the user's, hicolor, Adwaita; Flatpak's exports) or
 * /usr/share/pixmaps, or a full path. SVGs are made into PNGs by glycin
 * (icons_preview), off the UI thread; changed() runs when one is ready.
 */
#ifndef OSD_APPICONS_H
#define OSD_APPICONS_H

#include <cairo.h>

void appicons_init(void (*changed)(void));
/* The icon of the program whose desktop entry is at `desktop` (NULL: none,
 * or not ready yet). Cached by path. */
cairo_surface_t *appicon_for(const char *desktop);
/* An icon by its theme name ("dialog-information"), or a file's path or
 * file:// URI (a notification's). */
cairo_surface_t *appicon_named(const char *name);

#endif
