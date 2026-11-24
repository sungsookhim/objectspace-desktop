/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdTrashCan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd_classes.h"
#include "osd_menu_ids.h"

char *OsdTrashCan__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    char *base = OsdTrashCan_parent_osdModifyPopupMenu(self, menu);
    /* not Delete (the trash itself) nor Create another */
    if (base) {
        char *w = base;
        for (char *r = base; *r;) {
            char *nl = strchr(r, '\n');
            size_t len = nl ? (size_t)(nl - r) + 1 : strlen(r);
            long id = strtol(r, NULL, 10);
            if (id != OSD_MI_DELETE && id != OSD_MI_CREATE_ANOTHER) { memmove(w, r, len); w += len; }
            r += len;
        }
        *w = 0;
    }
    char line[64];
    snprintf(line, sizeof line, "%d\tEmpty trash\n", OSD_MI_EMPTY_TRASH);   /* the GUI asks first */
    char *r = osd_concat(base, line);
    free(base);
    return r;
}

char *OsdTrashCan__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Trash Can");
}

void OsdTrashCan__osdUnInitData(osdo_object *self)
{
    free(OsdTrashCan_data(self)->full_icon);
    OsdTrashCan_data(self)->full_icon = NULL;
    OsdTrashCan_parent_osdUnInitData(self);
}

char *OsdTrashCan__osdQueryFullIcon(osdo_object *self)
{
    const char *i = OsdTrashCan_data(self)->full_icon;
    return osd_strdup(i ? i : "");
}

bool OsdTrashCan__osdSetFullIcon(osdo_object *self, const char *path)
{
    if (path && *path && path[0] != '/') return false;
    OsdTrashCanData *d = OsdTrashCan_data(self);
    free(d->full_icon);
    d->full_icon = path && *path ? osd_strdup(path) : NULL;
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdTrashCan__osdSaveState(osdo_object *self)
{
    OsdTrashCan_parent_osdSaveState(self);
    const char *i = OsdTrashCan_data(self)->full_icon;
    if (i) osd_state_put_str(self, "OsdTrashCan", "fullicon", i);
    else osd_state_remove(self, "OsdTrashCan", "fullicon");
    return true;
}

void OsdTrashCan__osdRestoreProperties(osdo_object *self)
{
    OsdTrashCan_parent_osdRestoreProperties(self);
    char *t = NULL;
    free(OsdTrashCan_data(self)->full_icon);
    OsdTrashCan_data(self)->full_icon = osd_state_get_str(self, "OsdTrashCan", "fullicon", &t) ? t : NULL;
}

/* The Icon page: two icons, for empty and for full. */
