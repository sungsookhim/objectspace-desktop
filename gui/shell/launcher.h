/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * The Launcher (G4; OS/2 Warp 3/4): a floating palette of buttons.
 *
 * Its buttons are the objects in a folder ($XDG_DATA_HOME/objectspace/Launcher;
 * OSD_LAUNCHER_DIR for tests), mostly shadows: drop any object on the
 * palette to add a button, remove one from its menu. Each button has a
 * drawer (the arrow above it): a folder in "<Launcher folder> Drawers"
 * named after the button, shown above it; drop on the arrow to put an
 * object in it. On the
 * left, OS/2's own buttons: Lockup, Find, Shut down, Window list (through
 * the GNOME Shell extension). The extension keeps it above other windows,
 * on every workspace, out of Alt+Tab. The grip at the left end moves it.
 *
 * The first time, it gets shadows of the home folder, Templates and System
 * Setup.
 */
#ifndef OSD_LAUNCHER_H
#define OSD_LAUNCHER_H

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

/* open_folder: the shell opens a folder window for a path. */
void launcher_open(ui_app *a, void (*open_folder)(const char *path), const char *templates, const char *setup);

/* Whether it is open; bring it forward (opening its object again). */
bool launcher_is_open(void);
/* The icon set changed: the buttons' sizes again. */
void launcher_icons_changed(void);
/* Closed (its menu's Close); launcher_open opens it again. */
void launcher_close(void);
void launcher_raise(void);

/* An event from osdd: true if it was the Launcher's (its folder changed). */
bool launcher_event(const char *name, int64_t sub);

/* Self-tests. */
size_t launcher_count(void);
const char *launcher_title(size_t i);
bool launcher_press(size_t button);          /* as a click: 0..3 OS/2's, 4.. the objects */
bool launcher_add(const char *path);         /* as a drop */
bool launcher_drawer(size_t button, const char *path);   /* as a drop on a drawer arrow */
bool launcher_snapshot(const char *png);
const char *launcher_dir(void);

/* The system buttons' pictures (0 Lockup, 1 Find, 2 Shut down, 3 Window list). */
void launcher_glyph(cairo_t *cr, int i, ui_rect r);

/* The Launcher object's settings (OS/2's, v0.3-design 11): orientation
 * (horizontal, vertical), small_icons, text (titles on the buttons),
 * float_top, close_drawer (after an object in it is opened). */
void launcher_setting(const char *key, const char *val);
/* Properties on an object button's menu: the shell opens its notebook. */
void launcher_set_props(void (*fn)(const char *addr));
void launcher_on_color(void (*fn)(const char *hex));   /* a colour dropped on it */

/* Self-tests. */
bool launcher_vertical(void);
double launcher_width(void);
double launcher_height(void);
bool launcher_hover(size_t button);          /* the pointer rests on it */
bool launcher_drawer_open(size_t i);
size_t launcher_drawer_count(void);

#endif
