/* Startup (v0.2-design 7.1): whether a program object is an autostart
 * entry (a .desktop file in ~/.config/autostart) and whether it starts at
 * login (not Hidden, not X-GNOME-Autostart-enabled=false); the service
 * fills these (service/startup.c). NULL: no Startup here. */
#ifndef OSD_STARTUP_HOOK_H
#define OSD_STARTUP_HOOK_H

#include <stdbool.h>

#include "osdo.h"

/* true when o is an entry; *on: it starts at login */
extern bool (*osd_startup_query)(osdo_object *o, bool *on);
extern bool (*osd_startup_set)(osdo_object *o, bool on);

/* The desktop's folder (its Background page has Use GNOME's background);
 * the service fills it. NULL: none known. */
extern bool (*osd_is_desktop)(const char *path);

/* A data file that is a program (ELF, or a script with #!): its Program
 * page (0.4 step 6); the service fills it. NULL: none are. */
extern bool (*osd_is_program)(const char *path);

#endif
