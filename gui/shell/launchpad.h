/*
 * The LaunchPad (G4; OS/2 Warp 3/4): a floating palette of buttons.
 *
 * Its buttons are the objects in a folder ($XDG_DATA_HOME/wps/LaunchPad;
 * WPS_LAUNCHPAD_DIR for tests), mostly shadows: drop any object on the
 * palette to add a button, remove one from its menu. Each button has a
 * drawer (the arrow above it): a folder in "<LaunchPad folder> Drawers"
 * named after the button, shown above it; drop on the arrow to put an
 * object in it. On the
 * left, OS/2's own buttons: Lockup, Find, Shut down, Window list (through
 * the GNOME Shell extension). The extension keeps it above other windows,
 * on every workspace, out of Alt+Tab. The grip at the left end moves it.
 *
 * The first time, it gets shadows of the home folder, Templates and System
 * Setup.
 */
#ifndef WPS_LAUNCHPAD_H
#define WPS_LAUNCHPAD_H

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

/* open_folder: the shell opens a folder window for a path. */
void launchpad_open(ui_app *a, void (*open_folder)(const char *path), const char *templates, const char *setup);

/* An event from wpsd: true if it was the LaunchPad's (its folder changed). */
bool launchpad_event(const char *name, int64_t sub);

/* Self-tests. */
size_t launchpad_count(void);
const char *launchpad_title(size_t i);
bool launchpad_press(size_t button);          /* as a click: 0..3 OS/2's, 4.. the objects */
bool launchpad_add(const char *path);         /* as a drop */
bool launchpad_drawer(size_t button, const char *path);   /* as a drop on a drawer arrow */
bool launchpad_snapshot(const char *png);
const char *launchpad_dir(void);

#endif
