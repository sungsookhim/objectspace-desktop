#include "WPFolder.h"
#include "wps_themes.h"

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
    /* 0.3's sort and columns, made the properties they are now (0.4): the
     * sort as its choice (it was -1 none, 0 name ... 3 last write), the
     * columns one each (a bitmask) - shared by all folders through Class... */
    static const char *const sorts[] = { "name", "type", "size", "last write" };
    if (wps_state_get_int(self, "WPFolder", "sort_key", &v)) {
        wps_state_put_str(self, "WPFolder", "sort_key", v >= 0 && v <= 3 ? sorts[v] : "none");
        WPObject_wpSaveDeferred(self);
    }
    int64_t cols;
    if (wps_state_get_int(self, "WPFolder", "cols", &cols)) {
        static const char *const keys[] = { "col_name", "col_type", "col_size", "col_date" };
        for (int i = 0; i < 4; i++) wps_state_put_int(self, "WPFolder", keys[i], (cols >> i) & 1);
        wps_state_remove(self, "WPFolder", "cols");
        WPObject_wpSaveDeferred(self);
    }
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

static bool set_font(wpo_object *self, const char *key, const char *font)
{
    if (!font || strlen(font) > 120 || strchr(font, '\n') || strchr(font, '\t')) return false;
    if (*font) {   /* "size.Family" */
        char *end;
        long pt = strtol(font, &end, 10);
        if (end == font || *end != '.' || !end[1] || pt < 4 || pt > 72) return false;
        wps_state_put_str(self, "WPFolder", key, font);
    } else {
        wps_state_remove(self, "WPFolder", key);
    }
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

bool WPFolder__wpSetIconFont(wpo_object *self, const char *font) { return set_font(self, "icon_font", font); }
bool WPFolder__wpSetTitleFont(wpo_object *self, const char *font) { return set_font(self, "title_font", font); }

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
    static const char *const opts[] = { "none", "name", "type", "size", "last write" };
    return choice >= 0 && choice <= 4 && WPObject_wpSetProperty(self, "sort_key", opts[choice]);
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
    /* "name=1": its column property (col_name, ...) */
    const char *eq = which_on ? strchr(which_on, '=') : NULL;
    if (!eq || eq - which_on > 8) return false;
    char key[16];
    snprintf(key, sizeof key, "col_%.*s", (int)(eq - which_on), which_on);
    return WPObject_wpSetProperty(self, key, eq[1] == '1' ? "1" : "0");
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

/* ---- its pages' properties (WPFolder.wpi) */

bool WPFolder__wpQueryIsDesktop(wpo_object *self)
{
    const char *path = wpo_is_a(self, WPFileSystemClass) ? WPFileSystem_data(self)->path : NULL;
    return path && wps_is_desktop && wps_is_desktop(path);
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

char *WPFolder__wpQueryIconThemes(wpo_object *self)
{
    (void)self;
    char *t = wps_icon_theme_names();
    return t && *t ? t : (free(t), wps_strdup("WPS (flat)"));
}
