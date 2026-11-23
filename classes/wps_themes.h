/* Icon themes (0.4 step 2; wps_themes.c), for the service and the window. */
#ifndef WPS_THEMES_H
#define WPS_THEMES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { char id[64], name[128], dir[PATH_MAX]; } wps_icon_theme;

/* The themes there are: WPS's own first (flat, outline, classic), then the
 * others by name; the user's before the system's of one id. */
size_t wps_icon_themes(wps_icon_theme *out, size_t max);
/* A theme's folder, by its name ("WPS (flat)") or id ("wps-flat"). */
bool wps_icon_theme_dir(const char *theme, char *out, size_t n);
/* Their names, "WPS (flat),WPS (outline),Classic,..." (a choice's options). Caller frees. */
char *wps_icon_theme_names(void);

#endif
