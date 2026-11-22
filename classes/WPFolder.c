#include "WPFolder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WPFileSystem.h"
#include "wps_classes.h"
#include "wps_startup_hook.h"

bool (*wps_is_desktop)(const char *path);

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
    v = 0;
    WPFolder_data(self)->local_objects = wps_state_get_int(self, "WPFolder", "local", &v) ? (int)v : 0;
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

/* ---- the Background page */

static bool set_bg_str(wpo_object *self, const char *key, const char *v)
{
    wps_state_put_str(self, "WPFolder", key, v ? v : "");
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpSetBgImage(wpo_object *self, const char *path)
{
    if (path && *path && path[0] != '/') return false;
    return set_bg_str(self, "bg_image", path);
}

bool WPFolder__wpSetBgMode(wpo_object *self, int64_t mode)
{
    if (mode < 0 || mode > 4) return false;
    wps_state_put_int(self, "WPFolder", "bg_mode", mode);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpSetBgColor(wpo_object *self, const char *color)
{
    if (color && *color && (color[0] != '#' || strlen(color) != 7)) return false;
    return set_bg_str(self, "bg_color", color);
}

bool WPFolder__wpSetBgGnome(wpo_object *self, bool on)
{
    wps_state_put_int(self, "WPFolder", "bg_gnome", on);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

char *WPFolder__wpAddSettingsPages(wpo_object *self, const char *pages)
{
    char *base = WPFolder_parent_wpAddSettingsPages(self, pages), line[256];
    snprintf(line, sizeof line, "page\tView\nfield\tsort\tchoice\tSort by\t%d\twpSetSortOrder\tname,type,size,date\n"
             "field\tlocal\tcheckbox\tKeep objects as files here\t%d\twpSetLocalObjects\n",
             WPFolder_data(self)->sort_order, WPFolder_data(self)->local_objects);
    char *r = wps_concat(base, line);
    free(base);
    /* Background (OS/2's page): the desktop's has Use GNOME's background */
    char *img = NULL, *col = NULL;
    int64_t mode = 2, gnome = 1;
    wps_state_get_str(self, "WPFolder", "bg_image", &img);
    wps_state_get_str(self, "WPFolder", "bg_color", &col);
    wps_state_get_int(self, "WPFolder", "bg_mode", &mode);
    wps_state_get_int(self, "WPFolder", "bg_gnome", &gnome);
    const char *path = wpo_is_a(self, WPFileSystemClass) ? WPFileSystem_data(self)->path : NULL;
    bool desk = path && wps_is_desktop && wps_is_desktop(path);
    size_t n = (img ? strlen(img) : 0) + 512;
    char *bg = malloc(n);
    snprintf(bg, n, "page\tBackground\n%s"
             "field\tbg_image\tfile\tImage\t%s\twpSetBgImage\timage\n"
             "field\tbg_mode\tchoice\tShown\t%lld\twpSetBgMode\tcentred,tiled,scaled to fill,scaled to fit,stretched\n"
             "field\tbg_color\tcolor\tColour\t%s\twpSetBgColor\n",
             desk ? (gnome ? "field\tbg_gnome\tcheckbox\tGNOME's background\t1\twpSetBgGnome\n"
                           : "field\tbg_gnome\tcheckbox\tGNOME's background\t0\twpSetBgGnome\n") : "",
             img ? img : "", (long long)mode, col ? col : "");
    free(img);
    free(col);
    char *r2 = wps_concat(r, bg);
    free(r);
    free(bg);
    return r2;
}

char *WPFolder__wpclsQueryTitle(wpo_object *self)
{
    (void)self;
    return wps_strdup("Folder");
}

bool WPFolder__wpQueryLocalObjects(wpo_object *self) { return WPFolder_data(self)->local_objects != 0; }

bool WPFolder__wpSetLocalObjects(wpo_object *self, bool on)
{
    WPFolder_data(self)->local_objects = on;
    wps_state_put_int(self, "WPFolder", "local", on);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}
