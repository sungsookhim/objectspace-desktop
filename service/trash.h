/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * The trash, as the freedesktop.org Trash specification has it, so that
 * other desktops and file managers see what Objectspace put there and can restore
 * it (and the other way round).
 *
 * A file on the home filesystem goes to $XDG_DATA_HOME/Trash; one on
 * another filesystem to $topdir/.Trash/$uid (when the administrator made a
 * sticky, non-link .Trash there), else to $topdir/.Trash-$uid. Each item
 * gets an info file (original path, deletion time), written first and
 * without replacing; the item itself is moved by one rename without
 * replacing, so its object ID, settings and shadows go with it.
 */
#ifndef OSD_TRASH_H
#define OSD_TRASH_H

#include <stddef.h>

/* Any thread: move `path` to the trash; its new path into `out`. 0, or a
 * negative errno value: -ENOENT gone already, -EXDEV no trash can be used
 * on its filesystem (read-only, no permission, or already in a trash). */
int osd_trash(const char *path, char *out, size_t n);

/* Any thread: an item inside a trash's files folder that is being deleted
 * for good: remove its info file. */
void osd_trash_forget_info(const char *path);

/* 0.2, the Trash Can: the home trash ($XDG_DATA_HOME/Trash); whether a
 * folder is a trash's files folder; an item's original path and deletion
 * date (from its info file); putting it back where it was (made again if
 * gone, "name (2)" if taken). 0, or a negative errno value. */
#include <stdbool.h>
void osd_trash_home(char *out, size_t n);
/* The home trash's files/ and info/ made (at osdd's start): a Trash Can
 * window opened before the first delete follows a folder that is there. */
bool osd_trash_home_ready(void);
bool osd_is_trash_files_dir(const char *folder);
int osd_trash_info(const char *item, char *orig, size_t n, char *when, size_t wn);
int osd_trash_restore(const char *item, char *out, size_t n);

#endif
