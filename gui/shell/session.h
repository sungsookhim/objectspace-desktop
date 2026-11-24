/*
 * Ending and locking the session, for the WarpCenter, the LaunchPad and the
 * desktop's menu (v0.3-design 2): through the compositor's link in the WPS
 * session, the GNOME Shell extension inside GNOME (winlist_ext).
 */
#ifndef OSD_SESSION_H
#define OSD_SESSION_H

#include <stdbool.h>

#include "ui.h"

/* `from` names the caller in the action event (warpcenter, launchpad, desktop). */
bool session_lock(const char *from);
/* Shut down, Restart, Log off or Cancel, asked first (OS/2 asked). */
void session_ask_end(ui_app *a, const char *from);
/* Log off or Cancel. */
void session_ask_logoff(ui_app *a, const char *from);

#endif
