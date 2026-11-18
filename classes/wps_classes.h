/* The core WPS classes. */
#ifndef WPS_CLASSES_H
#define WPS_CLASSES_H

#include "wpo.h"

/* Register WPObject and its six subclasses, parents first. */
wpo_status wps_classes_register(void);

/* Helpers shared by the class implementations. */
char *wps_strdup(const char *s);
char *wps_concat(const char *a, const char *b);   /* a + b, newly allocated */

#endif
