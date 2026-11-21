/* A drive's state, for WPDrive's menu and notebook (v0.2-design 5): the
 * service fills it from udisks2 (service/udisks.c); NULL: no drives. */
#ifndef WPS_DRIVE_HOOK_H
#define WPS_DRIVE_HOOK_H

#include <limits.h>
#include <stdbool.h>

typedef struct {
    bool present, mounted, ejectable, can_poweroff, encrypted, unlocked, root;
    char device[64], fstype[32], mount[PATH_MAX], size[32], free[32], kind[12];
} wps_drive_state;

extern bool (*wps_drive_query)(const char *uuid, wps_drive_state *out);

#endif
