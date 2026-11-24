/* Icon themes (0.4 step 2; osd_themes.c), for the service and the window. */
#ifndef OSD_THEMES_H
#define OSD_THEMES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { char id[64], name[128], dir[PATH_MAX]; } osd_icon_theme;

/* The themes there are: WPS's own first (flat, outline, classic), then the
 * others by name; the user's before the system's of one id. */
size_t osd_icon_themes(osd_icon_theme *out, size_t max);
/* A theme's folder, by its name ("WPS (flat)") or id ("wps-flat"). */
bool osd_icon_theme_dir(const char *theme, char *out, size_t n);
/* Their names, "WPS (flat),WPS (outline),Classic,..." (a choice's options). Caller frees. */
char *osd_icon_theme_names(void);

/* The theme's icon name for a data file's MIME type ("file-image", ...;
 * "file-data" for anything else). */
const char *osd_icon_name_for_type(const char *mime);
/* An object class's theme icon name (the System Setup objects', WPS's
 * own abstract ones), NULL for none. */
const char *osd_icon_name_for_class(const char *cls);

#endif
