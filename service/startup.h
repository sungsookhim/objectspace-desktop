/*
 * Startup (v0.2-design 7.1): the XDG autostart folder
 * ($XDG_CONFIG_HOME/autostart), titled Startup, in System.
 *
 * What is dropped there: a program (a desktop entry, or a shadow of one)
 * becomes an autostart entry, a copy of the entry with X-WPS-Startup=true;
 * a script or other executable an entry running it; anything else a
 * shadow, which the WPS desktop opens at login (systemd's autostart
 * generator reads only .desktop files: it ignores them).
 */
#ifndef WPS_STARTUP_H
#define WPS_STARTUP_H

#include <stdbool.h>
#include <stddef.h>

void wps_startup_dir(char *out, size_t n);
bool wps_startup_is_entry(const char *path);   /* a .desktop file directly in it */
bool wps_startup_enabled(const char *path);
int wps_startup_set_enabled(const char *path, bool on);   /* 0 or an errno */

/* An entry for `path` (a regular file: a .desktop program, or an
 * executable): 0 and the entry's path in made; ENOEXEC when it is
 * neither (the caller makes a shadow instead). Never overwrites. */
int wps_startup_add(const char *path, char *made, size_t n);

#endif
