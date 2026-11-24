#include "OsdDrives.h"

#include <stdlib.h>
#include <string.h>

#include "osd_classes.h"
#include "osd_menu_ids.h"

/* Menu lines without these IDs (a table "id<TAB>label..." per line). */
char *osd_menu_without(char *menu, const int *ids, int n)
{
    if (!menu) return NULL;
    char *w = menu;
    for (char *r = menu; *r;) {
        char *nl = strchr(r, '\n');
        size_t len = nl ? (size_t)(nl - r) + 1 : strlen(r);
        long id = strtol(r, NULL, 10);
        bool drop = false;
        for (int k = 0; k < n; k++) drop |= id == ids[k];
        if (!drop) { memmove(w, r, len); w += len; }
        r += len;
    }
    *w = 0;
    return menu;
}

char *OsdDrives__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    static const int gone[] = { OSD_MI_DELETE, OSD_MI_CREATE_ANOTHER };   /* kept by wpsd */
    return osd_menu_without(OsdDrives_parent_osdModifyPopupMenu(self, menu), gone, 2);
}

char *OsdDrives__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Drives");
}
