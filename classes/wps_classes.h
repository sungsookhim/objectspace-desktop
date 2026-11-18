/* The core WPS classes. */
#ifndef WPS_CLASSES_H
#define WPS_CLASSES_H

#include "wpo.h"

/* Register WPObject and its six subclasses, parents first. */
wpo_status wps_classes_register(void);

/* Helpers shared by the class implementations. */
char *wps_strdup(const char *s);
char *wps_concat(const char *a, const char *b);   /* a + b, newly allocated */

/* File types from shared-mime-info (call wps_mime_init once at start-up). */
void wps_mime_init(const char *globs2);
const char *wps_mime_for_name(const char *name);

#endif
