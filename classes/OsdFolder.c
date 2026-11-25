/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#include "OsdFolder.h"
#include "osd_themes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OsdFileSystem.h"
#include "osd_classes.h"
#include "osd_startup_hook.h"

bool (*osd_is_desktop)(const char *path);

char *OsdFolder__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    char *base = OsdFolder_parent_osdModifyPopupMenu(self, menu);
    /* "Open" becomes a cascade of the folder's views, as "Open as" in Warp 4. */
    char *r = osd_concat(base, "11\tIcon view\t1\n12\tTree view\t1\n13\tDetails view\t1\n14\tColumn view\t1\n20\tArrange\n73\tCommand prompt here\n");
    free(base);
    return r;
}

int64_t OsdFolder__osdQuerySortOrder(osdo_object *self)
{
    return OsdFolder_data(self)->sort_order;
}

bool OsdFolder__osdSetSortOrder(osdo_object *self, int64_t order)
{
    if (order < 0 || order > 16) return false;
    OsdFolder_data(self)->sort_order = (int)order;
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdFolder__osdSaveState(osdo_object *self)
{
    OsdFolder_parent_osdSaveState(self);
    osd_state_put_int(self, "OsdFolder", "sort", OsdFolder_data(self)->sort_order);
    return true;
}

void OsdFolder__osdRestoreProperties(osdo_object *self)
{
    OsdFolder_parent_osdRestoreProperties(self);
    int64_t v = 0;
    OsdFolder_data(self)->sort_order = osd_state_get_int(self, "OsdFolder", "sort", &v) ? (int)v : 0;
    v = 0;
    OsdFolder_data(self)->local_objects = osd_state_get_int(self, "OsdFolder", "local", &v) ? (int)v : 0;
    /* 0.3's sort and columns, made the properties they are now (0.4): the
     * sort as its choice (it was -1 none, 0 name ... 3 last write), the
     * columns one each (a bitmask) - shared by all folders through Class... */
    static const char *const sorts[] = { "name", "type", "size", "last write" };
    if (osd_state_get_int(self, "OsdFolder", "sort_key", &v)) {
        osd_state_put_str(self, "OsdFolder", "sort_key", v >= 0 && v <= 3 ? sorts[v] : "none");
        OsdObject_osdSaveDeferred(self);
    }
    int64_t cols;
    if (osd_state_get_int(self, "OsdFolder", "cols", &cols)) {
        static const char *const keys[] = { "col_name", "col_type", "col_size", "col_date" };
        for (int i = 0; i < 4; i++) osd_state_put_int(self, "OsdFolder", keys[i], (cols >> i) & 1);
        osd_state_remove(self, "OsdFolder", "cols");
        OsdObject_osdSaveDeferred(self);
    }
}

/* Positions are keyed by the child's object ID, so they survive renames and
 * a hard-linked file can sit at a different place in each folder. */
static bool pos_key(osdo_object *child, char *key, size_t n)
{
    if (!child || !osdo_is_a(child, OsdObjectClass) || !OsdObject_data(child)->id[0]) return false;
    snprintf(key, n, "pos:%s", OsdObject_data(child)->id);
    return true;
}

static bool pos_locked(const char *v) { size_t n = v ? strlen(v) : 0; return n && v[n - 1] == 'L'; }

bool OsdFolder__osdSetIconPos(osdo_object *self, osdo_object *child, int64_t x, int64_t y)
{
    char key[64], val[48], *old = NULL;
    if (!pos_key(child, key, sizeof key)) return false;
    osd_state_get_str(self, "OsdFolder", key, &old);
    snprintf(val, sizeof val, "%lld,%lld%s", (long long)x, (long long)y, pos_locked(old) ? ",L" : "");   /* (a lock stays) */
    free(old);
    osd_state_put_str(self, "OsdFolder", key, val);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

char *OsdFolder__osdQueryIconPos(osdo_object *self, osdo_object *child)
{
    char key[64], *v = NULL;
    if (!pos_key(child, key, sizeof key)) return osd_strdup("");
    return osd_state_get_str(self, "OsdFolder", key, &v) ? v : osd_strdup("");
}

bool OsdFolder__osdSetIconLock(osdo_object *self, osdo_object *child, bool on)
{
    char key[64], *old = NULL, val[48];
    if (!pos_key(child, key, sizeof key)) return false;
    osd_state_get_str(self, "OsdFolder", key, &old);
    bool was = pos_locked(old);
    if (was == on) { free(old); return true; }
    if (on) snprintf(val, sizeof val, "%s%s", old && *old ? old : "", old && *old ? ",L" : "L");
    else {
        size_t n = strlen(old);
        snprintf(val, sizeof val, "%.*s", (int)(n >= 2 && old[n - 2] == ',' ? n - 2 : n - 1), old);
    }
    free(old);
    if (*val) osd_state_put_str(self, "OsdFolder", key, val);
    else osd_state_remove(self, "OsdFolder", key);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdFolder__osdQueryIconLock(osdo_object *self, osdo_object *child)
{
    char key[64], *v = NULL;
    if (!pos_key(child, key, sizeof key)) return false;
    osd_state_get_str(self, "OsdFolder", key, &v);
    bool l = pos_locked(v);
    free(v);
    return l;
}

bool OsdFolder__osdSetIconTitles(osdo_object *self, int64_t choice)
{
    if (choice < 0 || choice > 1) return false;
    osd_state_put_int(self, "OsdFolder", "icon_titles", choice);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

static bool set_font(osdo_object *self, const char *key, const char *font)
{
    if (!font || strlen(font) > 120 || strchr(font, '\n') || strchr(font, '\t')) return false;
    if (*font) {   /* "size.Family" */
        char *end;
        long pt = strtol(font, &end, 10);
        if (end == font || *end != '.' || !end[1] || pt < 4 || pt > 72) return false;
        osd_state_put_str(self, "OsdFolder", key, font);
    } else {
        osd_state_remove(self, "OsdFolder", key);
    }
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdFolder__osdSetIconFont(osdo_object *self, const char *font) { return set_font(self, "icon_font", font); }
bool OsdFolder__osdSetTitleFont(osdo_object *self, const char *font) { return set_font(self, "title_font", font); }

bool OsdFolder__osdSetLockAll(osdo_object *self, bool on)
{
    osd_state_put_int(self, "OsdFolder", "lock_all", on);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

osdo_pending *OsdFolder__osdPopulate(osdo_object *self)
{
    (void)self;
    /* Streaming, paged listing on workers comes in M4. */
    osdo_pending *p = osdo_pending_new();
    osdo_pending_complete(p, osdo_v_int(0));
    return p;
}

/* ---- the details view: sort and columns */

bool OsdFolder__osdSetSortKey(osdo_object *self, int64_t choice)
{
    static const char *const opts[] = { "none", "name", "type", "size", "last write" };
    return choice >= 0 && choice <= 4 && OsdObject_osdSetProperty(self, "sort_key", opts[choice]);
}

bool OsdFolder__osdSetSortDesc(osdo_object *self, bool on)
{
    osd_state_put_int(self, "OsdFolder", "sort_desc", on);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

/* "name=1", "type=0", ...: one column on or off */
bool OsdFolder__osdSetColumn(osdo_object *self, const char *which_on)
{
    /* "name=1": its column property (col_name, ...) */
    const char *eq = which_on ? strchr(which_on, '=') : NULL;
    if (!eq || eq - which_on > 8) return false;
    char key[16];
    snprintf(key, sizeof key, "col_%.*s", (int)(eq - which_on), which_on);
    return OsdObject_osdSetProperty(self, key, eq[1] == '1' ? "1" : "0");
}

bool OsdFolder__osdSetColWidths(osdo_object *self, const char *widths)
{
    if (!widths || strlen(widths) > 120 || strspn(widths, "0123456789.,") != strlen(widths)) return false;
    osd_state_put_str(self, "OsdFolder", "col_widths", widths);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

/* ---- the Background page */

static bool set_bg_str(osdo_object *self, const char *key, const char *v)
{
    osd_state_put_str(self, "OsdFolder", key, v ? v : "");
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdFolder__osdSetBgImage(osdo_object *self, const char *path)
{
    if (path && *path && path[0] != '/') return false;
    return set_bg_str(self, "bg_image", path);
}

bool OsdFolder__osdSetBgMode(osdo_object *self, int64_t mode)
{
    if (mode < 0 || mode > 4) return false;
    osd_state_put_int(self, "OsdFolder", "bg_mode", mode);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

bool OsdFolder__osdSetBgColor(osdo_object *self, const char *color)
{
    if (color && *color && (color[0] != '#' || strlen(color) != 7)) return false;
    return set_bg_str(self, "bg_color", color);
}

bool OsdFolder__osdSetBgGnome(osdo_object *self, bool on)
{
    osd_state_put_int(self, "OsdFolder", "bg_gnome", on);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

/* ---- its pages' properties (OsdFolder.osdi) */

bool OsdFolder__osdQueryIsDesktop(osdo_object *self)
{
    const char *path = osdo_is_a(self, OsdFileSystemClass) ? OsdFileSystem_data(self)->path : NULL;
    return path && osd_is_desktop && osd_is_desktop(path);
}

/* (a work area is a folder with a window of its own: not the desktop) */
bool OsdFolder__osdQueryIsNotDesktop(osdo_object *self) { return !OsdFolder__osdQueryIsDesktop(self); }


char *OsdFolder__osdclsQueryTitle(osdo_object *self)
{
    (void)self;
    return osd_strdup("Folder");
}

bool OsdFolder__osdQueryLocalObjects(osdo_object *self) { return OsdFolder_data(self)->local_objects != 0; }

bool OsdFolder__osdSetLocalObjects(osdo_object *self, bool on)
{
    OsdFolder_data(self)->local_objects = on;
    osd_state_put_int(self, "OsdFolder", "local", on);
    OsdObject_osdSaveDeferred(self);
    OsdObject_osdObjectChanged(self);
    return true;
}

char *OsdFolder__osdQueryIconThemes(osdo_object *self)
{
    (void)self;
    char *t = osd_icon_theme_names();
    return t && *t ? t : (free(t), osd_strdup("Objectspace (flat)"));
}
