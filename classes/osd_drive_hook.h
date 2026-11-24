/* A drive's state, for OsdDrive's menu and notebook (v0.2-design 5): the
 * service fills it from udisks2 (service/udisks.c); NULL: no drives. */
#ifndef OSD_DRIVE_HOOK_H
#define OSD_DRIVE_HOOK_H

#include <limits.h>
#include <stdbool.h>

typedef struct {
    bool present, mounted, ejectable, can_poweroff, encrypted, unlocked, root;
    char device[64], fstype[32], mount[PATH_MAX], size[32], free[32], kind[12];
} osd_drive_state;

/* details: also its free space (reads the file system: the notebook only) */
extern bool (*osd_drive_query)(const char *uuid, osd_drive_state *out, bool details);

#endif
