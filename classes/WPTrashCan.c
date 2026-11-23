#include "WPTrashCan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wps_classes.h"
#include "wps_menu_ids.h"

char *WPTrashCan__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    char *base = WPTrashCan_parent_wpModifyPopupMenu(self, menu);
    /* not Delete (the trash itself) nor Create another */
    if (base) {
        char *w = base;
        for (char *r = base; *r;) {
            char *nl = strchr(r, '\n');
            size_t len = nl ? (size_t)(nl - r) + 1 : strlen(r);
            long id = strtol(r, NULL, 10);
            if (id != WPS_MI_DELETE && id != WPS_MI_CREATE_ANOTHER) { memmove(w, r, len); w += len; }
            r += len;
        }
        *w = 0;
    }
    char line[64];
    snprintf(line, sizeof line, "%d\tEmpty trash\n", WPS_MI_EMPTY_TRASH);   /* the GUI asks first */
    char *r = wps_concat(base, line);
    free(base);
    return r;
}

char *WPTrashCan__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Trash Can");
}

void WPTrashCan__wpUnInitData(wpo_object *self)
{
    free(WPTrashCan_data(self)->full_icon);
    WPTrashCan_data(self)->full_icon = NULL;
    WPTrashCan_parent_wpUnInitData(self);
}

char *WPTrashCan__wpQueryFullIcon(wpo_object *self)
{
    const char *i = WPTrashCan_data(self)->full_icon;
    return wps_strdup(i ? i : "");
}

bool WPTrashCan__wpSetFullIcon(wpo_object *self, const char *path)
{
    if (path && *path && path[0] != '/') return false;
    WPTrashCanData *d = WPTrashCan_data(self);
    free(d->full_icon);
    d->full_icon = path && *path ? wps_strdup(path) : NULL;
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPTrashCan__wpSaveState(wpo_object *self)
{
    WPTrashCan_parent_wpSaveState(self);
    const char *i = WPTrashCan_data(self)->full_icon;
    if (i) wps_state_put_str(self, "WPTrashCan", "fullicon", i);
    else wps_state_remove(self, "WPTrashCan", "fullicon");
    return true;
}

void WPTrashCan__wpRestoreProperties(wpo_object *self)
{
    WPTrashCan_parent_wpRestoreProperties(self);
    char *t = NULL;
    free(WPTrashCan_data(self)->full_icon);
    WPTrashCan_data(self)->full_icon = wps_state_get_str(self, "WPTrashCan", "fullicon", &t) ? t : NULL;
}

/* The Icon page: two icons, for empty and for full. */
