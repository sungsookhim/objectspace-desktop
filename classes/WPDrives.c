#include "WPDrives.h"

#include <stdlib.h>
#include <string.h>

#include "wps_classes.h"
#include "wps_menu_ids.h"

/* Menu lines without these IDs (a table "id<TAB>label..." per line). */
char *wps_menu_without(char *menu, const int *ids, int n)
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

char *WPDrives__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    static const int gone[] = { WPS_MI_DELETE, WPS_MI_CREATE_ANOTHER };   /* kept by wpsd */
    return wps_menu_without(WPDrives_parent_wpModifyPopupMenu(self, menu), gone, 2);
}

char *WPDrives__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Drives");
}
