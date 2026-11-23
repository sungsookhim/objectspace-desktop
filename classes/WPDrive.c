#include "WPDrive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wps_classes.h"
#include "wps_menu_ids.h"

bool (*wps_drive_query)(const char *uuid, wps_drive_state *out, bool details);
char *wps_menu_without(char *menu, const int *ids, int n);

void WPDrive__wpUnInitData(wpo_object *self)
{
    free(WPDrive_data(self)->uuid);
    WPDrive_data(self)->uuid = NULL;
    WPDrive_parent_wpUnInitData(self);
}

char *WPDrive__wpQueryUUID(wpo_object *self)
{
    const char *u = WPDrive_data(self)->uuid;
    return wps_strdup(u ? u : "");
}

bool WPDrive__wpSetUUID(wpo_object *self, const char *uuid)
{
    WPDriveData *d = WPDrive_data(self);
    free(d->uuid);
    d->uuid = uuid && *uuid ? wps_strdup(uuid) : NULL;
    WPObject_wpSaveDeferred(self);
    return true;
}

bool WPDrive__wpSaveState(wpo_object *self)
{
    WPDrive_parent_wpSaveState(self);
    const char *u = WPDrive_data(self)->uuid;
    if (u) wps_state_put_str(self, "WPDrive", "uuid", u);
    return true;
}

void WPDrive__wpRestoreProperties(wpo_object *self)
{
    WPDrive_parent_wpRestoreProperties(self);
    char *t = NULL;
    free(WPDrive_data(self)->uuid);
    WPDrive_data(self)->uuid = wps_state_get_str(self, "WPDrive", "uuid", &t) ? t : NULL;
}

static bool state_of(wpo_object *self, wps_drive_state *st, bool details)
{
    memset(st, 0, sizeof *st);
    const char *u = WPDrive_data(self)->uuid;
    return u && wps_drive_query && wps_drive_query(u, st, details);
}

/* Open, Properties; then what the drive can do now. */
char *WPDrive__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    static const int gone[] = { WPS_MI_DELETE, WPS_MI_CREATE_ANOTHER };
    char *base = wps_menu_without(WPDrive_parent_wpModifyPopupMenu(self, menu), gone, 2);
    wps_drive_state st;
    if (!state_of(self, &st, false) || st.root) return base;
    char add[256] = "";
    size_t o = 0;
#define ADD(id, label) o += (size_t)snprintf(add + o, sizeof add - o, "%d\t%s\n", id, label)
    if (st.encrypted && !st.unlocked) ADD(WPS_MI_UNLOCK, "Unlock...");
    else if (st.mounted) ADD(WPS_MI_UNMOUNT, "Unmount");
    else ADD(WPS_MI_MOUNT, "Mount");
    if (st.encrypted && st.unlocked) ADD(WPS_MI_LOCK, "Lock");
    if (st.ejectable) ADD(WPS_MI_EJECT, "Eject");
    if (st.can_poweroff) ADD(WPS_MI_POWER_OFF, "Safely remove");
#undef ADD
    char *r = wps_concat(base, add);
    free(base);
    return r;
}

/* The Drive page, read only. */
/* Its Drive page (WPDrive.wpi): the device as udisks has it now. */
char *WPDrive__wpQueryDriveInfo(wpo_object *self, const char *which)
{
    wps_drive_state st;
    bool known = state_of(self, &st, true);
    const char *v = "";
    if (!strcmp(which, "device")) v = known ? st.device : "(not present)";
    else if (!known) v = "";
    else if (!strcmp(which, "fstype")) v = st.fstype;
    else if (!strcmp(which, "size")) v = st.size;
    else if (!strcmp(which, "free")) v = st.free;
    else if (!strcmp(which, "mount")) v = st.mounted ? st.mount : (st.encrypted && !st.unlocked ? "(locked)" : "(not mounted)");
    return wps_strdup(v);
}

char *WPDrive__wpQueryDevice(wpo_object *self) { return WPDrive_wpQueryDriveInfo(self, "device"); }
char *WPDrive__wpQueryFsType(wpo_object *self) { return WPDrive_wpQueryDriveInfo(self, "fstype"); }
char *WPDrive__wpQuerySize(wpo_object *self) { return WPDrive_wpQueryDriveInfo(self, "size"); }
char *WPDrive__wpQueryFree(wpo_object *self) { return WPDrive_wpQueryDriveInfo(self, "free"); }
char *WPDrive__wpQueryMount(wpo_object *self) { return WPDrive_wpQueryDriveInfo(self, "mount"); }

char *WPDrive__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Drive");
}
