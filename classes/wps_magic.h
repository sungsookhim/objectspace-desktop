/* A file's type from its contents (wps_magic.c), for names that say nothing. */
#ifndef WPS_MAGIC_H
#define WPS_MAGIC_H

#include <stddef.h>

/* shared-mime-info's magic rules against these bytes: the type of the
 * highest priority that matches, or NULL. (WPS_MIME_MAGIC: another rules
 * file, for tests.) */
const char *wps_magic_type(const unsigned char *buf, size_t n);
/* A regular file's type from its first 4 KB: the magic rules', else
 * text/plain for an empty file or text; NULL if none (or not readable). */
const char *wps_mime_sniff(const char *path);

#endif
