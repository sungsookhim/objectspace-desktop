/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Object menus (TheBar's, v0.3-design 11): a folder's objects as
 * a menu with their icons, its folders as cascades (each with Open
 * first), read ahead a few levels deep (the menu widget takes whole
 * trees). Used by TheBar's Desktop menu and the Launcher's
 * drawers.
 */
#ifndef OSD_OBJMENU_H
#define OSD_OBJMENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

#define OBJMENU_FIRST 1000   /* the objects' menu IDs; below: the caller's */

typedef struct objmenu objmenu;

/* Read `folder` and its folders, `depth` levels; ready(m, ctx) when read
 * whole (on the UI thread). */
objmenu *objmenu_read(const char *folder, int depth, void (*ready)(objmenu *m, void *ctx), void *ctx);
/* (safe while it is being read: freed when the last answer comes) */
void objmenu_free(objmenu *m);

/* Its items (strdup'ed labels; ui_menu_items_free), with room for `extra`
 * more at the end; `open_first`: the folder itself first ("Open"). */
ui_menu_item *objmenu_items(objmenu *m, size_t *n, bool open_first, size_t extra);
/* For ui_menu_popup_icons (arg: the objmenu). */
bool objmenu_icon(void *m, int64_t id, void *item);
/* What item `id` is: its address, whether it opens as a folder (a path),
 * its title. False if it is not one of the objects. */
bool objmenu_choice(objmenu *m, int64_t id, char *addr, size_t n, bool *folder, char *title, size_t tn);
size_t objmenu_count(objmenu *m);   /* the top folder's objects */
/* Their titles (" >": a cascade), for self-tests; the count. */
size_t objmenu_labels(objmenu *m, char *out, size_t n);

#endif
