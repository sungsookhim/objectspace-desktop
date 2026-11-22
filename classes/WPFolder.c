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

static bool pos_locked(const char *v) { size_t n = v ? strlen(v) : 0; return n && v[n - 1] == 'L'; }

bool WPFolder__wpSetIconPos(wpo_object *self, wpo_object *child, int64_t x, int64_t y)
{
    char key[64], val[48], *old = NULL;
    if (!pos_key(child, key, sizeof key)) return false;
    wps_state_get_str(self, "WPFolder", key, &old);
    snprintf(val, sizeof val, "%lld,%lld%s", (long long)x, (long long)y, pos_locked(old) ? ",L" : "");   /* (a lock stays) */
    free(old);
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

bool WPFolder__wpSetIconLock(wpo_object *self, wpo_object *child, bool on)
{
    char key[64], *old = NULL, val[48];
    if (!pos_key(child, key, sizeof key)) return false;
    wps_state_get_str(self, "WPFolder", key, &old);
    bool was = pos_locked(old);
    if (was == on) { free(old); return true; }
    if (on) snprintf(val, sizeof val, "%s%s", old && *old ? old : "", old && *old ? ",L" : "L");
    else {
        size_t n = strlen(old);
        snprintf(val, sizeof val, "%.*s", (int)(n >= 2 && old[n - 2] == ',' ? n - 2 : n - 1), old);
    }
    free(old);
    if (*val) wps_state_put_str(self, "WPFolder", key, val);
    else wps_state_remove(self, "WPFolder", key);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpQueryIconLock(wpo_object *self, wpo_object *child)
{
    char key[64], *v = NULL;
    if (!pos_key(child, key, sizeof key)) return false;
    wps_state_get_str(self, "WPFolder", key, &v);
    bool l = pos_locked(v);
    free(v);
    return l;
}

bool WPFolder__wpSetIconTitles(wpo_object *self, int64_t choice)
{
    if (choice < 0 || choice > 1) return false;
    wps_state_put_int(self, "WPFolder", "icon_titles", choice);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpSetIconFont(wpo_object *self, const char *font)
{
    if (!font || strlen(font) > 120 || strchr(font, '\n') || strchr(font, '\t')) return false;
    if (*font) {   /* "size.Family" */
        char *end;
        long pt = strtol(font, &end, 10);
        if (end == font || *end != '.' || !end[1] || pt < 4 || pt > 72) return false;
        wps_state_put_str(self, "WPFolder", "icon_font", font);
    } else {
        wps_state_remove(self, "WPFolder", "icon_font");
    }
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpSetLockAll(wpo_object *self, bool on)
{
    wps_state_put_int(self, "WPFolder", "lock_all", on);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

wpo_pending *WPFolder__wpPopulate(wpo_object *self)
{
    (void)self;
    /* Streaming, paged listing on workers comes in M4. */
    wpo_pending *p = wpo_pending_new();
    wpo_pending_complete(p, wpo_v_int(0));
    return p;
}

/* ---- the details view: sort and columns */

bool WPFolder__wpSetSortKey(wpo_object *self, int64_t choice)
{
    if (choice < 0 || choice > 4) return false;
    wps_state_put_int(self, "WPFolder", "sort_key", choice - 1);   /* (-1: none, the folder's own order) */
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpSetSortDesc(wpo_object *self, bool on)
{
    wps_state_put_int(self, "WPFolder", "sort_desc", on);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

/* "name=1", "type=0", ...: one column on or off */
bool WPFolder__wpSetColumn(wpo_object *self, const char *which_on)
{
    static const char *const names[] = { "name", "type", "size", "date" };
    const char *eq = which_on ? strchr(which_on, '=') : NULL;
    if (!eq) return false;
    int bit = -1;
    for (int i = 0; i < 4; i++) if ((size_t)(eq - which_on) == strlen(names[i]) && !strncmp(which_on, names[i], strlen(names[i]))) bit = i;
    if (bit < 0) return false;
    int64_t cols = 15;
    wps_state_get_int(self, "WPFolder", "cols", &cols);
    cols = eq[1] == '1' ? cols | (1 << bit) : cols & ~(1 << bit);
    wps_state_put_int(self, "WPFolder", "cols", cols);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpSetColWidths(wpo_object *self, const char *widths)
{
    if (!widths || strlen(widths) > 120 || strspn(widths, "0123456789.,") != strlen(widths)) return false;
    wps_state_put_str(self, "WPFolder", "col_widths", widths);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
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
    int64_t key = -1, desc = 0, cols = 15, titles = 0, lock_all = 0;
    char *cw = NULL;
    wps_state_get_int(self, "WPFolder", "icon_titles", &titles);
    wps_state_get_int(self, "WPFolder", "lock_all", &lock_all);
    char *ifont = NULL;
    wps_state_get_str(self, "WPFolder", "icon_font", &ifont);
    wps_state_get_str(self, "WPFolder", "col_widths", &cw);
    wps_state_get_int(self, "WPFolder", "sort_key", &key);
    wps_state_get_int(self, "WPFolder", "sort_desc", &desc);
    wps_state_get_int(self, "WPFolder", "cols", &cols);
    char line2[1400];
    snprintf(line2, sizeof line2, "page\tView\nfield\tsort_key\tchoice\tSort by\t%lld\twpSetSortKey\tnone,name,type,size,last write\n"
             "field\tsort_desc\tcheckbox\tDescending\t%d\twpSetSortDesc\n"
             "field\tlocal\tcheckbox\tKeep objects as files here\t%d\twpSetLocalObjects\n"
             "page\tIcon view\nfield\ticon_titles\tchoice\tTitles\t%lld\twpSetIconTitles\twhole,two lines\n"
             "field\tlock_all\tcheckbox\tLock positions of all icons\t%d\twpSetLockAll\n"
             "field\ticon_font\ttext\tIcon font (e.g. 10.Sans; empty: the System's)\t%s\twpSetIconFont\n"
             "page\tDetails view\nfield\tcol_name\tcheckbox\tReal name\t%d\t@col:name\n"
             "field\tcol_type\tcheckbox\tType\t%d\t@col:type\n"
             "field\tcol_size\tcheckbox\tSize\t%d\t@col:size\n"
             "field\tcol_date\tcheckbox\tLast write\t%d\t@col:date\n"
             "field\tcol_widths\thidden\tWidths\t%s\twpSetColWidths\n",
             (long long)key + 1, desc != 0, WPFolder_data(self)->local_objects, (long long)titles, lock_all != 0, ifont ? ifont : "",
             (int)(cols & 1) != 0, (int)(cols & 2) != 0, (int)(cols & 4) != 0, (int)(cols & 8) != 0, cw ? cw : "");
    free(cw);
    free(ifont);
    (void)line;
    char *r = wps_concat(base, line2);
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
