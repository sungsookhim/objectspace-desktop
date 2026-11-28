/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdNetwork.h"
#include "OsdShare.h"
#include "OsdNetConnect.h"
#include "osd_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_classes.h"
#include "osd_menu_ids.h"

char *osd_menu_without(char *menu, const int *ids, int n);

char *OsdNetwork__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    static const int gone[] = { OSD_MI_DELETE, OSD_MI_CREATE_ANOTHER };   /* kept by osdd */
    return osd_menu_without(OsdNetwork_parent_osdModifyPopupMenu(self, menu), gone, 2);
}

char *OsdNetwork__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Network");
}

/* ---- a share */

char *OsdShare__osdQueryAddress(osdo_object *self)
{
    const char *u = OsdDrive_data(self)->uuid;
    return osd_strdup(u && !strncmp(u, "net:", 4) ? u + 4 : "");
}

/* Open, Properties; Disconnect while it is connected, Connect while not
 * (a connection of the user's); Forget (a connection of the user's). */
char *OsdShare__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    static const int gone[] = { OSD_MI_DELETE, OSD_MI_CREATE_ANOTHER, OSD_MI_MOUNT, OSD_MI_UNMOUNT, OSD_MI_EJECT,
                                OSD_MI_POWER_OFF, OSD_MI_LOCK, OSD_MI_UNLOCK };
    char *base = osd_menu_without(OsdShare_parent_osdModifyPopupMenu(self, menu), gone, 8);
    osd_drive_state st;
    memset(&st, 0, sizeof st);
    const char *u = OsdDrive_data(self)->uuid;
    bool on = u && osd_drive_query && osd_drive_query(u, &st, false) && st.mounted;
    char *conn = NULL;
    bool mine = osd_state_get_str(self, "OsdShare", "connect", &conn) && conn && conn[0];
    free(conn);
    char add[160] = "";
    size_t o = 0;
    if (on) o += (size_t)snprintf(add + o, sizeof add - o, "%d\tDisconnect\n", OSD_MI_DISCONNECT);
    else if (mine) o += (size_t)snprintf(add + o, sizeof add - o, "%d\tConnect\n", OSD_MI_CONNECT);
    if (mine) o += (size_t)snprintf(add + o, sizeof add - o, "%d\tForget\n", OSD_MI_FORGET);
    char *r = osd_concat(base, add);
    free(base);
    return r;
}

/* ---- Connect to a share... (opens as the desktop's dialog) */

char *OsdNetConnect__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Connect to a share");
}

char *OsdShare__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Network share");
}
