/*
 * The icon picker (G4, the Icon page's Choose...): icons from the system's
 * icon themes (/usr/share/icons and ~/.local/share/icons: Adwaita, hicolor,
 * the apps' own), or from Objectspace's icon library folder (any images put
 * there), in an icon view with a search field. A double click (or Enter)
 * picks one: chosen(owner, key, the image's path), and the picker closes.
 */
#ifndef OSD_ICONPICK_H
#define OSD_ICONPICK_H

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

typedef void (*iconpick_fn)(long owner, const char *key, const char *image);

void iconpick_init(ui_app *a, void (*open_folder)(const char *path));
/* `for_title`: what it is for (in its title). */
void iconpick_open(const char *for_title, iconpick_fn chosen, long owner, const char *key);

/* Self-tests (the open picker). */
bool iconpick_ready(void);                 /* the icons are read */
size_t iconpick_count(void);               /* shown (after the search) */
const char *iconpick_name(size_t i);
bool iconpick_search(const char *text);
bool iconpick_source(bool library);
bool iconpick_choose(size_t i);
bool iconpick_snapshot(const char *png);
ui_surface *iconpick_surface(void);        /* (self-tests) */
bool iconpick_scroll(double fraction);     /* as the scroll bar's thumb dragged there */

#endif
