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
