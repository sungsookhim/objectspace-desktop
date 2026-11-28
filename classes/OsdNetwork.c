/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdNetwork.h"
#include "OsdShare.h"

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

/* Open, Properties; Disconnect while it is connected. */
char *OsdShare__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    static const int gone[] = { OSD_MI_DELETE, OSD_MI_CREATE_ANOTHER, OSD_MI_MOUNT, OSD_MI_UNMOUNT, OSD_MI_EJECT,
                                OSD_MI_POWER_OFF, OSD_MI_LOCK, OSD_MI_UNLOCK };
    char *base = osd_menu_without(OsdShare_parent_osdModifyPopupMenu(self, menu), gone, 8);
    osd_drive_state st;
    memset(&st, 0, sizeof st);
    const char *u = OsdDrive_data(self)->uuid;
    if (!u || !osd_drive_query || !osd_drive_query(u, &st, false) || !st.mounted) return base;
    char add[64];
    snprintf(add, sizeof add, "%d\tDisconnect\n", OSD_MI_DISCONNECT);
    char *r = osd_concat(base, add);
    free(base);
    return r;
}

char *OsdShare__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Network share");
}
