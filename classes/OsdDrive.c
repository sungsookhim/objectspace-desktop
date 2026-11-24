#include "OsdDrive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_classes.h"
#include "osd_menu_ids.h"

bool (*osd_drive_query)(const char *uuid, osd_drive_state *out, bool details);
char *osd_menu_without(char *menu, const int *ids, int n);

void OsdDrive__osdUnInitData(osdo_object *self)
{
    free(OsdDrive_data(self)->uuid);
    OsdDrive_data(self)->uuid = NULL;
    OsdDrive_parent_osdUnInitData(self);
}

char *OsdDrive__osdQueryUUID(osdo_object *self)
{
    const char *u = OsdDrive_data(self)->uuid;
    return osd_strdup(u ? u : "");
}

bool OsdDrive__osdSetUUID(osdo_object *self, const char *uuid)
{
    OsdDriveData *d = OsdDrive_data(self);
    free(d->uuid);
    d->uuid = uuid && *uuid ? osd_strdup(uuid) : NULL;
    OsdObject_osdSaveDeferred(self);
    return true;
}

bool OsdDrive__osdSaveState(osdo_object *self)
{
    OsdDrive_parent_osdSaveState(self);
    const char *u = OsdDrive_data(self)->uuid;
    if (u) osd_state_put_str(self, "OsdDrive", "uuid", u);
    return true;
}

void OsdDrive__osdRestoreProperties(osdo_object *self)
{
    OsdDrive_parent_osdRestoreProperties(self);
    char *t = NULL;
    free(OsdDrive_data(self)->uuid);
    OsdDrive_data(self)->uuid = osd_state_get_str(self, "OsdDrive", "uuid", &t) ? t : NULL;
}

static bool state_of(osdo_object *self, osd_drive_state *st, bool details)
{
    memset(st, 0, sizeof *st);
    const char *u = OsdDrive_data(self)->uuid;
    return u && osd_drive_query && osd_drive_query(u, st, details);
}

/* Open, Properties; then what the drive can do now. */
char *OsdDrive__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    static const int gone[] = { OSD_MI_DELETE, OSD_MI_CREATE_ANOTHER };
    char *base = osd_menu_without(OsdDrive_parent_osdModifyPopupMenu(self, menu), gone, 2);
    osd_drive_state st;
    if (!state_of(self, &st, false) || st.root) return base;
    char add[256] = "";
    size_t o = 0;
#define ADD(id, label) o += (size_t)snprintf(add + o, sizeof add - o, "%d\t%s\n", id, label)
    if (st.encrypted && !st.unlocked) ADD(OSD_MI_UNLOCK, "Unlock...");
    else if (st.mounted) ADD(OSD_MI_UNMOUNT, "Unmount");
    else ADD(OSD_MI_MOUNT, "Mount");
    if (st.encrypted && st.unlocked) ADD(OSD_MI_LOCK, "Lock");
    if (st.ejectable) ADD(OSD_MI_EJECT, "Eject");
    if (st.can_poweroff) ADD(OSD_MI_POWER_OFF, "Safely remove");
#undef ADD
    char *r = osd_concat(base, add);
    free(base);
    return r;
}

/* The Drive page, read only. */
/* Its Drive page (OsdDrive.osdi): the device as udisks has it now. */
char *OsdDrive__osdQueryDriveInfo(osdo_object *self, const char *which)
{
    osd_drive_state st;
    bool known = state_of(self, &st, true);
    const char *v = "";
    if (!strcmp(which, "device")) v = known ? st.device : "(not present)";
    else if (!known) v = "";
    else if (!strcmp(which, "fstype")) v = st.fstype;
    else if (!strcmp(which, "size")) v = st.size;
    else if (!strcmp(which, "free")) v = st.free;
    else if (!strcmp(which, "mount")) v = st.mounted ? st.mount : (st.encrypted && !st.unlocked ? "(locked)" : "(not mounted)");
    return osd_strdup(v);
}

char *OsdDrive__osdQueryDevice(osdo_object *self) { return OsdDrive_osdQueryDriveInfo(self, "device"); }
char *OsdDrive__osdQueryFsType(osdo_object *self) { return OsdDrive_osdQueryDriveInfo(self, "fstype"); }
char *OsdDrive__osdQuerySize(osdo_object *self) { return OsdDrive_osdQueryDriveInfo(self, "size"); }
char *OsdDrive__osdQueryFree(osdo_object *self) { return OsdDrive_osdQueryDriveInfo(self, "free"); }
char *OsdDrive__osdQueryMount(osdo_object *self) { return OsdDrive_osdQueryDriveInfo(self, "mount"); }

char *OsdDrive__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Drive");
}
