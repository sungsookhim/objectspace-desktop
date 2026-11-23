/* Settings objects: System, Sound, Mouse, Scheme Palette (WPSetup.wpi). */
#include "WPSetup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WPMouse.h"
#include "WPSchemePalette.h"
#include "WPFontPalette.h"
#include "WPSound.h"
#include "WPSystem.h"
#include "WPLaunchPad.h"
#include "WPWarpCenter.h"
#include "WPMessages.h"
#include "wps_classes.h"
#include "wps_state.h"
#include "wps_props.h"
#include "wps_themes.h"

/* Each subclass declares its settings as properties in its .wpi (kept as
 * text in the WPSetup state; object-properties-design.md 2.1): its pages
 * come from them, and the GUI reads them all here. */

/* "key\tvalue" lines of the settings this object's class declares (not
 * the title and icon every object has). */
char *WPSetup__wpQuerySettings(wpo_object *self)
{
    char *out = wps_strdup("");
    for (size_t i = 0; i < self->cls->nprops; i++) {
        const char *k = self->cls->props[i].name;
        char *v = wps_prop_get(self, k);
        size_t n = strlen(k) + strlen(v ? v : "") + 3;
        char *line = malloc(n);
        snprintf(line, n, "%s\t%s\n", k, v ? v : "");
        char *r = wps_concat(out, line);
        free(out);
        free(line);
        free(v);
        out = r;
    }
    return out;
}

bool WPSetup__wpSetSetting(wpo_object *self, const char *key, const char *value)
{
    return wps_prop_set(self, key, value);
}

/* ---- the System object's Folders and Fonts fields: all folders' settings
 * (class:WPFolder; object-properties-design.md 2.2). A value the System
 * object kept itself (0.3) moves there the first time it is read. */

static char *folders_get(wpo_object *self, const char *mine, const char *prop, const char *def)
{
    char *v = wps_scope_get ? wps_scope_get("class:WPFolder", "WPFolder", prop) : NULL;
    if (v) return v;
    char *old = NULL;
    if (wps_state_get_str(self, "WPSetup", mine, &old) && old) {
        if (wps_scope_set && wps_scope_set("class:WPFolder", "WPFolder", prop, old)) {
            wps_state_remove(self, "WPSetup", mine);
            WPObject_wpSaveDeferred(self);
        }
        return old;
    }
    free(old);
    return wps_strdup(def);
}

static bool folders_set(wpo_object *self, const char *prop, const char *v)
{
    if (!wps_scope_set || !wps_scope_set("class:WPFolder", "WPFolder", prop, v)) return false;
    WPObject_wpObjectChanged(self);   /* (its notebook) */
    return true;
}

char *WPSystem__wpQueryFoldersHidden(wpo_object *self) { return folders_get(self, "show_hidden", "show_hidden", "1"); }
bool WPSystem__wpSetFoldersHidden(wpo_object *self, const char *on) { return folders_set(self, "show_hidden", on); }
char *WPSystem__wpQueryFoldersExt(wpo_object *self) { return folders_get(self, "show_ext", "show_ext", "1"); }
bool WPSystem__wpSetFoldersExt(wpo_object *self, const char *on) { return folders_set(self, "show_ext", on); }
char *WPSystem__wpQueryFoldersTheme(wpo_object *self) { return folders_get(self, "icon_theme", "icon_theme", "WPS (flat)"); }
bool WPSystem__wpSetFoldersTheme(wpo_object *self, const char *theme) { return folders_set(self, "icon_theme", theme); }
char *WPSystem__wpQueryFoldersIconSet(wpo_object *self) { return folders_get(self, "icon_set", "icon_set", "Default"); }
bool WPSystem__wpSetFoldersIconSet(wpo_object *self, const char *set) { return folders_set(self, "icon_set", set); }
char *WPSystem__wpQueryIconThemes(wpo_object *self)
{
    (void)self;
    char *t = wps_icon_theme_names();
    return t && *t ? t : (free(t), wps_strdup("WPS (flat)"));
}

char *WPSystem__wpQueryFoldersFont(wpo_object *self) { return folders_get(self, "font_folders", "icon_font", ""); }
bool WPSystem__wpSetFoldersFont(wpo_object *self, const char *font) { return folders_set(self, "icon_font", font); }

/* The Scheme Palette's choices: the schemes there are (the service's list),
 * else the three WPS makes. */
char *WPSchemePalette__wpQuerySchemes(wpo_object *self)
{
    (void)self;
    char *t = wps_scheme_titles ? wps_scheme_titles() : NULL;
    return t && *t ? t : (free(t), wps_strdup("classic,dark,high contrast"));
}

/* Settings objects open as their notebook; they are not copied or made. */
char *WPSetup__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    /* the LaunchPad, the WarpCenter and Messages also open (the palette,
     * the bar, the list); the others are only their settings */
    bool opens = wpo_is_a(self, WPLaunchPadClass) || wpo_is_a(self, WPWarpCenterClass) || wpo_is_a(self, WPMessagesClass) ||
                 wpo_is_a(self, WPFontPaletteClass);
    return wps_concat(menu, opens ? "1\tOpen\n2\tProperties\n" : "2\tProperties\n");
}

char *WPSystem__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("System"); }
char *WPSound__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Sound"); }
char *WPMouse__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Mouse"); }
char *WPSchemePalette__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Scheme Palette"); }
char *WPFontPalette__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Font Palette"); }
char *WPLaunchPad__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("LaunchPad"); }
char *WPWarpCenter__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("WarpCenter"); }
char *WPMessages__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Messages"); }
