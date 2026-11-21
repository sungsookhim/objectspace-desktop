/*
 * Programs (v0.2-design 6): wpsd keeps the Programs folder in step with
 * the installed programs' desktop entries.
 *
 * The Programs folder ($XDG_DATA_HOME/wps/Programs, class
 * WPProgramsFolder) holds category folders (Accessories, Development,
 * Games, Graphics, Internet, Office, Multimedia, System, Settings,
 * Other) with shadows of the entries in $XDG_DATA_HOME/applications and
 * each $XDG_DATA_DIRS/applications (the first by desktop file ID wins);
 * entries that are not applications, NoDisplay, Hidden, not for this
 * desktop (OnlyShowIn/NotShowIn) or whose TryExec is missing are left
 * out. wpsd remembers what it made (<state>/programs-made: desktop file
 * ID, category, entry, shadow), so a shadow the user deleted is not made
 * again and the user's own objects are never touched. The application
 * folders are watched (inotify); changes are gathered for a second.
 */
#ifndef WPS_PROGRAMS_H
#define WPS_PROGRAMS_H

#include <stddef.h>

#include "wpo_loop.h"

/* folder: the Programs folder; state_dir: where programs-made is kept.
 * Nothing is done while the folder does not exist (wps_programs_sync
 * once it does). */
void wps_programs_init(wpo_loop *loop, const char *folder, const char *state_dir);
void wps_programs_sync(void);
void wps_programs_shutdown(void);

/* The category folder for a Categories= value ("AudioVideo;Player;"). */
const char *wps_program_category(const char *categories);

#endif
