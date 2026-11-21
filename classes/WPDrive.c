#include "WPDrive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wps_classes.h"
#include "wps_menu_ids.h"

bool (*wps_drive_query)(const char *uuid, wps_drive_state *out);
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

static bool state_of(wpo_object *self, wps_drive_state *st)
{
    memset(st, 0, sizeof *st);
    const char *u = WPDrive_data(self)->uuid;
    return u && wps_drive_query && wps_drive_query(u, st);
}

/* Open, Properties; then what the drive can do now. */
char *WPDrive__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    static const int gone[] = { WPS_MI_DELETE, WPS_MI_CREATE_ANOTHER };
    char *base = wps_menu_without(WPDrive_parent_wpModifyPopupMenu(self, menu), gone, 2);
    wps_drive_state st;
    if (!state_of(self, &st) || st.root) return base;
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
char *WPDrive__wpAddSettingsPages(wpo_object *self, const char *pages)
{
    char *base = WPDrive_parent_wpAddSettingsPages(self, pages);
    wps_drive_state st;
    bool known = state_of(self, &st);
    const char *u = WPDrive_data(self)->uuid;
    char page[PATH_MAX + 600];
    snprintf(page, sizeof page,
             "page\tDrive\n"
             "field\tdevice\tinfo\tDevice\t%s\t\n"
             "field\tfstype\tinfo\tFile system\t%s\t\n"
             "field\tsize\tinfo\tSize\t%s\t\n"
             "field\tfree\tinfo\tFree\t%s\t\n"
             "field\tmount\tinfo\tMounted at\t%s\t\n"
             "field\tuuid\tinfo\tUUID\t%s\t\n",
             known ? st.device : "(not present)", known ? st.fstype : "", known ? st.size : "", known ? st.free : "",
             known ? (st.mounted ? st.mount : (st.encrypted && !st.unlocked ? "(locked)" : "(not mounted)")) : "",
             u ? u : "");
    char *r = wps_concat(page, base ? base : "");
    free(base);
    return r;
}

char *WPDrive__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Drive");
}
