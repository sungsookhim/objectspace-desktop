/* Startup (v0.2-design 7.1): whether a program object is an autostart
 * entry (a .desktop file in ~/.config/autostart) and whether it starts at
 * login (not Hidden, not X-GNOME-Autostart-enabled=false); the service
 * fills these (service/startup.c). NULL: no Startup here. */
#ifndef WPS_STARTUP_HOOK_H
#define WPS_STARTUP_HOOK_H

#include <stdbool.h>

#include "wpo.h"

/* true when o is an entry; *on: it starts at login */
extern bool (*wps_startup_query)(wpo_object *o, bool *on);
extern bool (*wps_startup_set)(wpo_object *o, bool on);

#endif
