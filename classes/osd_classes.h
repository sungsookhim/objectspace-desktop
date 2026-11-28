/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* The core Objectspace classes. */
#ifndef OSD_CLASSES_H
#define OSD_CLASSES_H

#include "osdo.h"

/* Register OsdObject and its six subclasses, parents first. */
osdo_status osd_classes_register(void);

/* Helpers shared by the class implementations. */
char *osd_strdup(const char *s);
char *osd_concat(const char *a, const char *b);   /* a + b, newly allocated */

/* File types from shared-mime-info (call osd_mime_init once at start-up). */
void osd_mime_init(const char *globs2);
const char *osd_mime_for_name(const char *name);
/* A file's type (0.8): the one set on it (its user.mime_type attribute,
 * freedesktop.org's, as OS/2's .TYPE; on a file system without attributes
 * Objectspace's database through the hooks below), else its name's, else
 * its contents'; NULL: none. The returned string lasts until the next call
 * on this thread. `source`, if given: "set", "name" or "contents". */
const char *osd_file_mime(const char *path, const char **source);
/* The type set on it, into out; NULL: none set. */
const char *osd_file_mime_set(const char *path, char *out, size_t n);
/* Set its type (NULL or "": take the set one off); false if it could not
 * be kept. */
bool osd_file_mime_store(const char *path, const char *mime);
/* Whether s is a type's name ("image/png"). */
bool osd_mime_valid(const char *s);
/* (osdd's: the type kept in the database for a file whose file system
 * takes no attributes; get: into out, NULL none; set: NULL to remove) */
extern const char *(*osd_mime_overlay_get)(const char *path, char *out, size_t n);
extern bool (*osd_mime_overlay_set)(const char *path, const char *mime);

#endif
