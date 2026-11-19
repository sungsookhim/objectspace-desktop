#include "WPFolder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wps_classes.h"

char *WPFolder__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    char *base = WPFolder_parent_wpModifyPopupMenu(self, menu);
    /* "Open" becomes a cascade of the folder's views, as "Open as" in Warp 4. */
    char *r = wps_concat(base, "11\tIcon view\t1\n12\tTree view\t1\n13\tDetails view\t1\n20\tArrange\n");
    free(base);
    return r;
}

int64_t WPFolder__wpQuerySortOrder(wpo_object *self)
{
    return WPFolder_data(self)->sort_order;
}

bool WPFolder__wpSetSortOrder(wpo_object *self, int64_t order)
{
    if (order < 0 || order > 16) return false;
    WPFolder_data(self)->sort_order = (int)order;
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpSaveState(wpo_object *self)
{
    WPFolder_parent_wpSaveState(self);
    wps_state_put_int(self, "WPFolder", "sort", WPFolder_data(self)->sort_order);
    return true;
}

void WPFolder__wpRestoreProperties(wpo_object *self)
{
    WPFolder_parent_wpRestoreProperties(self);
    int64_t v = 0;
    WPFolder_data(self)->sort_order = wps_state_get_int(self, "WPFolder", "sort", &v) ? (int)v : 0;
}

/* Positions are keyed by the child's object ID, so they survive renames and
 * a hard-linked file can sit at a different place in each folder. */
static bool pos_key(wpo_object *child, char *key, size_t n)
{
    if (!child || !wpo_is_a(child, WPObjectClass) || !WPObject_data(child)->id[0]) return false;
    snprintf(key, n, "pos:%s", WPObject_data(child)->id);
    return true;
}

bool WPFolder__wpSetIconPos(wpo_object *self, wpo_object *child, int64_t x, int64_t y)
{
    char key[64], val[48];
    if (!pos_key(child, key, sizeof key)) return false;
    snprintf(val, sizeof val, "%lld,%lld", (long long)x, (long long)y);
    wps_state_put_str(self, "WPFolder", key, val);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

char *WPFolder__wpQueryIconPos(wpo_object *self, wpo_object *child)
{
    char key[64], *v = NULL;
    if (!pos_key(child, key, sizeof key)) return wps_strdup("");
    return wps_state_get_str(self, "WPFolder", key, &v) ? v : wps_strdup("");
}

wpo_pending *WPFolder__wpPopulate(wpo_object *self)
{
    (void)self;
    /* Streaming, paged listing on workers comes in M4. */
    wpo_pending *p = wpo_pending_new();
    wpo_pending_complete(p, wpo_v_int(0));
    return p;
}

char *WPFolder__wpAddSettingsPages(wpo_object *self, const char *pages)
{
    char *base = WPFolder_parent_wpAddSettingsPages(self, pages), line[256];
    snprintf(line, sizeof line, "page\tView\nfield\tsort\tchoice\tSort by\t%d\twpSetSortOrder\tname,type,size,date\n",
             WPFolder_data(self)->sort_order);
    char *r = wps_concat(base, line);
    free(base);
    return r;
}

char *WPFolder__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Folder");
}
