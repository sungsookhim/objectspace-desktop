/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Thumbnails (0.6 step 5; v0.6-design 4): pictures of files, for the
 * Preview setting and the Gallery view.
 *
 * Where they are kept (the owner's: the shared ways first):
 *   1. the user's shared cache, ~/.cache/thumbnails/{normal,large,x-large}/
 *      MD5(URI).png (the freedesktop.org Thumbnail Managing Standard),
 *      read and written, valid while its Thumb::MTime is the file's;
 *   2. a folder's shared repository, .sh_thumbnails/{normal,large}/
 *      MD5(file name).png (the same standard's, for removable and
 *      read-only media), read;
 *   3. a folder's .osd-thumbnails file, for the exceptions neither can
 *      hold: lines "NAME<TAB>PICTURE" (PICTURE a path, relative to the
 *      folder or absolute), read.
 * Made by the system's thumbnailers (/usr/share/thumbnailers and the
 * user's: their MimeType and Exec), two at a time, each stopped after 10
 * seconds; a file that fails is not tried again until it changes.
 */
#ifndef OSD_THUMBS_H
#define OSD_THUMBS_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

/* changed: a thumbnail asked for is ready (draw again). */
void thumbs_init(ui_app *a, void (*changed)(void));
/* File `path`'s thumbnail of about px pixels (its longer side), if it is
 * ready (owned by the cache: valid until the next call that makes room);
 * else NULL, and it is asked for (changed() when it comes). */
cairo_surface_t *thumbs_get(const char *path, int64_t mtime, int px);
/* Whether files of this name could have one (a thumbnailer takes their type). */
bool thumbs_possible(const char *path);

#endif
