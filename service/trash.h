/*
 * The trash, as the freedesktop.org Trash specification has it, so that
 * other desktops and file managers see what WPS put there and can restore
 * it (and the other way round).
 *
 * A file on the home filesystem goes to $XDG_DATA_HOME/Trash; one on
 * another filesystem to $topdir/.Trash/$uid (when the administrator made a
 * sticky, non-link .Trash there), else to $topdir/.Trash-$uid. Each item
 * gets an info file (original path, deletion time), written first and
 * without replacing; the item itself is moved by one rename without
 * replacing, so its object ID, settings and shadows go with it.
 */
#ifndef WPS_TRASH_H
#define WPS_TRASH_H

#include <stddef.h>

/* Any thread: move `path` to the trash; its new path into `out`. 0, or a
 * negative errno value: -ENOENT gone already, -EXDEV no trash can be used
 * on its filesystem (read-only, no permission, or already in a trash). */
int wps_trash(const char *path, char *out, size_t n);

/* Any thread: an item inside a trash's files folder that is being deleted
 * for good: remove its info file. */
void wps_trash_forget_info(const char *path);

#endif
