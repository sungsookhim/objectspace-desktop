/*
 * The Window List and the Minimized Window Viewer (G4; OS/2's window list,
 * Ctrl+Esc or a mouse button on the desktop, and Warp 4's Minimized
 * Window Viewer).
 *
 * In a GNOME session a program cannot see other programs' windows; the
 * wps@wps-linux extension can, and tells the desktop process about every
 * window on its standard input ("W" lines: add, change, remove, with an
 * ID, the app, the title, minimized, focused). Commands go back on the
 * desktop's standard output (activate, minimize, close an ID), so even
 * WPS's own windows are restored through GNOME (no program can
 * unminimize itself). Without the extension the lists say so.
 */
#ifndef WPS_WINLIST_H
#define WPS_WINLIST_H

#include <stdbool.h>

#include "ui.h"

/* cmd_fd: where commands to the extension go, or -1 (no extension). */
void winlist_init(ui_app *a, int cmd_fd);

/* A "W" line from the extension, split at its tabs. */
void winlist_line(char **f, int n);

/* Open (or bring forward) the Window List, or the Minimized Window Viewer. */
void winlist_open(bool minimized_only);

/* Self-tests: how many windows are known (minimized ones), and send a
 * command for the window whose title contains `title`. */
size_t winlist_count(bool minimized_only);
bool winlist_command(const char *cmd, const char *title);
bool winlist_is_open(bool minimized_only);
const char *winlist_title(size_t i, bool minimized_only);
bool winlist_snapshot(bool minimized_only, const char *png);

#endif
