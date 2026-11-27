/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Settings objects: System, Sound, Mouse, Scheme Palette (OsdSetup.osdi). */
#include "OsdSetup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OsdMouse.h"
#include "OsdSchemePalette.h"
#include "OsdFontPalette.h"
#include "OsdColorPalette.h"
#include "OsdScreens.h"
#include "OsdSound.h"
#include "OsdSystem.h"
#include "OsdLauncher.h"
#include "OsdTheBar.h"
#include "OsdMessages.h"
#include "osd_classes.h"
#include "osd_state.h"
#include "osd_props.h"
#include "osd_themes.h"

/* Each subclass declares its settings as properties in its .osdi (kept as
 * text in the OsdSetup state; object-properties-design.md 2.1): its pages
 * come from them, and the GUI reads them all here. */

/* "key\tvalue" lines of the settings this object's class declares (not
 * the title and icon every object has). */
char *OsdSetup__osdQuerySettings(osdo_object *self)
{
    char *out = osd_strdup("");
    for (size_t i = 0; i < self->cls->nprops; i++) {
        const char *k = self->cls->props[i].name;
        char *v = osd_prop_get(self, k);
        size_t n = strlen(k) + strlen(v ? v : "") + 3;
        char *line = malloc(n);
        snprintf(line, n, "%s\t%s\n", k, v ? v : "");
        char *r = osd_concat(out, line);
        free(out);
        free(line);
        free(v);
        out = r;
    }
    return out;
}

bool OsdSetup__osdSetSetting(osdo_object *self, const char *key, const char *value)
{
    return osd_prop_set(self, key, value);
}

/* ---- the System object's Folders and Fonts fields: all folders' settings
 * (class:OsdFolder; object-properties-design.md 2.2). A value the System
 * object kept itself (0.3) moves there the first time it is read. */

static char *folders_get(osdo_object *self, const char *mine, const char *prop, const char *def)
{
    char *v = osd_scope_get ? osd_scope_get("class:OsdFolder", "OsdFolder", prop) : NULL;
    if (v) return v;
    char *old = NULL;
    if (osd_state_get_str(self, "OsdSetup", mine, &old) && old) {
        if (osd_scope_set && osd_scope_set("class:OsdFolder", "OsdFolder", prop, old)) {
            osd_state_remove(self, "OsdSetup", mine);
            OsdObject_osdSaveDeferred(self);
        }
        return old;
    }
    free(old);
    return osd_strdup(def);
}

static bool folders_set(osdo_object *self, const char *prop, const char *v)
{
    if (!osd_scope_set || !osd_scope_set("class:OsdFolder", "OsdFolder", prop, v)) return false;
    OsdObject_osdObjectChanged(self);   /* (its notebook) */
    return true;
}

char *OsdSystem__osdQueryFoldersHidden(osdo_object *self) { return folders_get(self, "show_hidden", "show_hidden", "1"); }
bool OsdSystem__osdSetFoldersHidden(osdo_object *self, const char *on) { return folders_set(self, "show_hidden", on); }
char *OsdSystem__osdQueryFoldersExt(osdo_object *self) { return folders_get(self, "show_ext", "show_ext", "1"); }
bool OsdSystem__osdSetFoldersExt(osdo_object *self, const char *on) { return folders_set(self, "show_ext", on); }
char *OsdSystem__osdQueryFoldersMenuBar(osdo_object *self) { return folders_get(self, "menu_bar", "menu_bar", "0"); }
bool OsdSystem__osdSetFoldersMenuBar(osdo_object *self, const char *on) { return folders_set(self, "menu_bar", on); }
char *OsdSystem__osdQueryFoldersTheme(osdo_object *self) { return folders_get(self, "icon_theme", "icon_theme", "Objectspace (flat)"); }
bool OsdSystem__osdSetFoldersTheme(osdo_object *self, const char *theme) { return folders_set(self, "icon_theme", theme); }
char *OsdSystem__osdQueryFoldersIconSet(osdo_object *self) { return folders_get(self, "icon_set", "icon_set", "Default"); }
bool OsdSystem__osdSetFoldersIconSet(osdo_object *self, const char *set) { return folders_set(self, "icon_set", set); }
char *OsdSystem__osdQueryIconThemes(osdo_object *self)
{
    (void)self;
    char *t = osd_icon_theme_names();
    return t && *t ? t : (free(t), osd_strdup("Objectspace (flat)"));
}

char *OsdSystem__osdQueryFoldersFont(osdo_object *self) { return folders_get(self, "font_folders", "icon_font", ""); }
bool OsdSystem__osdSetFoldersFont(osdo_object *self, const char *font) { return folders_set(self, "icon_font", font); }

/* The Scheme Palette's choices: the schemes there are (the service's list),
 * else the three Objectspace makes. */
char *OsdSchemePalette__osdQuerySchemes(osdo_object *self)
{
    (void)self;
    char *t = osd_scheme_titles ? osd_scheme_titles() : NULL;
    return t && *t ? t : (free(t), osd_strdup("classic,dark,high contrast"));
}

/* Settings objects open as their notebook; they are not copied or made. */
char *OsdSetup__osdModifyPopupMenu(osdo_object *self, const char *menu)
{
    /* the Launcher, TheBar and Messages also open (the palette,
     * the bar, the list); the others are only their settings */
    bool opens = osdo_is_a(self, OsdLauncherClass) || osdo_is_a(self, OsdTheBarClass) || osdo_is_a(self, OsdMessagesClass) ||
                 osdo_is_a(self, OsdFontPaletteClass) || osdo_is_a(self, OsdColorPaletteClass) || osdo_is_a(self, OsdScreensClass);
    return osd_concat(menu, opens ? "1\tOpen\n2\tProperties\n" : "2\tProperties\n");
}

char *OsdSystem__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("System"); }
char *OsdSound__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("Sound"); }
char *OsdMouse__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("Mouse"); }
char *OsdSchemePalette__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("Scheme Palette"); }
char *OsdFontPalette__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("Font Palette"); }
char *OsdColorPalette__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("Colour Palette"); }
char *OsdScreens__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("Screens"); }
char *OsdLauncher__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("Launcher"); }
char *OsdTheBar__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("TheBar"); }
char *OsdMessages__osdclsQueryTitle(osdo_object *self) { (void)self; return osd_strdup("Messages"); }
