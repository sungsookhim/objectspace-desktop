/* The core WPS classes. */
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

#endif
