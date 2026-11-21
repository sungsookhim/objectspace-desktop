/* Settings objects: System, Sound, Mouse, Scheme Palette (WPSetup.wpi). */
#include "WPSetup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WPMouse.h"
#include "WPSchemePalette.h"
#include "WPSound.h"
#include "WPSystem.h"
#include "WPLaunchPad.h"
#include "WPWarpCenter.h"
#include "WPMessages.h"
#include "wps_classes.h"
#include "wps_state.h"

/* One setting: the page it is on, its key, kind, label, default value,
 * and for a choice its options (the stored value is the option's text). */
struct setting { const char *page, *key, *kind, *label, *def, *options; };

/* The defaults are the GUI's own (gui-design.md): the classic look, OS/2's
 * mouse, few sounds. */
static const struct setting system_settings[] = {
    { "Window", "corner_radius", "choice", "Window corner radius", "0", "0,2,4,8,12,16" },
    { "Window", "buttons", "choice", "Title-bar buttons", "always", "always,on hover" },
    { "Window", "border", "choice", "Frame border", "visible", "visible,thin,none" },
    { "Window", "shadows", "choice", "Window shadows", "off", "off,subtle,strong" },
    { "Window", "translucency", "choice", "Translucency", "off", "off,title bars,menus" },
    { "Window", "title_layout", "choice", "Title-bar layout", "normal", "normal,mirrored,compact" },
    { "Confirmations", "confirm_delete", "checkbox", "Confirm on delete", "1", NULL },
    { "Confirmations", "confirm_folder_delete", "checkbox", "Confirm on folder delete", "1", NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

/* Sounds by name in the sound theme (or a file path); empty: none. */
static const struct setting sound_settings[] = {
    { "Setup", "enabled", "checkbox", "Play sounds for actions", "1", NULL },
    { "Setup", "volume", "text", "Volume (0-100)", "70", NULL },
    { "Events", "shell.startup", "text", "Startup", "service-login", NULL },
    { "Events", "shell.shutdown", "text", "Shutdown", "", NULL },
    { "Events", "window.open", "text", "Open window", "", NULL },
    { "Events", "window.close", "text", "Close window", "", NULL },
    { "Events", "window.minimize", "text", "Minimize window", "", NULL },
    { "Events", "window.maximize", "text", "Maximize window", "", NULL },
    { "Events", "window.restore", "text", "Restore window", "", NULL },
    { "Events", "menu.open", "text", "Open menu", "", NULL },
    { "Events", "menu.select", "text", "Select menu item", "", NULL },
    { "Events", "object.open", "text", "Open object", "", NULL },
    { "Events", "object.drag", "text", "Begin drag", "", NULL },
    { "Events", "object.drop", "text", "Drop", "", NULL },
    { "Events", "object.create", "text", "Create object", "", NULL },
    { "Events", "object.trash", "text", "Move to the trash", "trash-empty", NULL },
    { "Events", "object.delete", "text", "Delete", "", NULL },
    { "Events", "job.done", "text", "Job finished", "complete", NULL },
    { "Events", "job.failed", "text", "Job failed", "dialog-error", NULL },
    { "Events", "alert.info", "text", "Information", "dialog-information", NULL },
    { "Events", "alert.warning", "text", "Warning", "dialog-warning", NULL },
    { "Events", "alert.error", "text", "Error", "dialog-error", NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

static const struct setting mouse_settings[] = {
    { "Mappings", "select", "text", "Select", "button1", NULL },
    { "Mappings", "open", "text", "Open", "button1 double", NULL },
    { "Mappings", "popup_menu", "text", "Pop-up menu", "button2", NULL },
    { "Mappings", "edit_title", "text", "Edit title", "alt+button1", NULL },
    { "Mappings", "drag", "text", "Drag", "button2", NULL },
    { "Mappings", "window_list", "text", "Window list", "button3", NULL },
    { "Mappings", "copy", "text", "Copy (keys held at the drop)", "ctrl", NULL },
    { "Mappings", "shadow", "text", "Create shadow (keys held at the drop)", "ctrl+shift", NULL },
    { "Mappings", "drop_menu", "text", "Choose at the drop (keys held)", "alt", NULL },
    { "Setup", "left_handed", "checkbox", "Left-handed (buttons 1 and 2 swapped)", "0", NULL },
    { "Setup", "double_click_ms", "text", "Double-click time (ms)", "400", NULL },
    { "Setup", "wheel_speed", "text", "Wheel speed (lines)", "3", NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

static const struct setting scheme_settings[] = {
    { "Scheme", "scheme", "choice", "Scheme", "classic", "classic,dark,high contrast" },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

/* "automatic": the session's own choice (v0.2-design 10: in the WPS
 * session the WarpCenter and not the LaunchPad; inside GNOME the
 * LaunchPad). */
static const struct setting launchpad_settings[] = {
    { "LaunchPad", "show_at_start", "choice", "Show at start", "automatic", "automatic,yes,no" },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

static const struct setting warpcenter_settings[] = {
    { "WarpCenter", "show_at_start", "choice", "Show at start (WPS session)", "automatic", "automatic,yes,no" },
    { "WarpCenter", "edge", "choice", "Screen edge", "bottom", "bottom,top" },
    { "Parts", "meters", "checkbox", "Meters (processor, memory, disk, network)", "1", NULL },
    { "Parts", "keyboard", "checkbox", "Keyboard (input source)", "1", NULL },
    { "Parts", "messages", "checkbox", "Messages", "1", NULL },
    { "Clock", "clock", "choice", "Clock", "24 hours", "24 hours,12 hours,off" },
    { "Clock", "seconds", "checkbox", "Seconds", "0", NULL },
    { "Clock", "date", "checkbox", "Date", "0", NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

static const struct setting messages_settings[] = {
    { "Messages", "dnd", "checkbox", "Do not disturb (messages are kept, not shown)", "0", NULL },
    { "Messages", "popup_seconds", "text", "Pop-ups stay (seconds)", "6", NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

static const struct setting *table(wpo_object *self)
{
    const char *n = self->cls->name;
    if (!strcmp(n, "WPSystem")) return system_settings;
    if (!strcmp(n, "WPSound")) return sound_settings;
    if (!strcmp(n, "WPMouse")) return mouse_settings;
    if (!strcmp(n, "WPSchemePalette")) return scheme_settings;
    if (!strcmp(n, "WPLaunchPad")) return launchpad_settings;
    if (!strcmp(n, "WPWarpCenter")) return warpcenter_settings;
    if (!strcmp(n, "WPMessages")) return messages_settings;
    return NULL;
}

/* The value now: the user's, else the default. */
static char *value_of(wpo_object *self, const struct setting *s)
{
    char *v = NULL;
    if (wps_state_get_str(self, "WPSetup", s->key, &v) && v) return v;
    return wps_strdup(s->def);
}

/* A choice's option number in its list, or -1. */
static int option_index(const char *options, const char *v)
{
    int i = 0;
    for (const char *p = options; p && *p; i++) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (strlen(v) == l && !strncmp(p, v, l)) return i;
        p = e ? e + 1 : NULL;
    }
    return -1;
}

static char *option_at(const char *options, long k)
{
    long i = 0;
    for (const char *p = options; p && *p; i++) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (i == k) {
            char *r = malloc(l + 1);
            if (r) { memcpy(r, p, l); r[l] = 0; }
            return r;
        }
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

char *WPSetup__wpQuerySettings(wpo_object *self)
{
    const struct setting *t = table(self);
    char *out = wps_strdup("");
    for (size_t i = 0; t && t[i].key; i++) {
        char *v = value_of(self, &t[i]);
        size_t n = strlen(t[i].key) + strlen(v ? v : "") + 3;
        char *line = malloc(n);
        snprintf(line, n, "%s\t%s\n", t[i].key, v ? v : "");
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
    const struct setting *t = table(self);
    const struct setting *s = NULL;
    for (size_t i = 0; t && t[i].key && !s; i++) if (!strcmp(t[i].key, key)) s = &t[i];
    if (!s || !value) return false;
    char *v = NULL;
    if (!strcmp(s->kind, "choice")) {
        /* the notebook sends the option's number; the option's text is kept */
        char *end;
        long k = strtol(value, &end, 10);
        v = *value && !*end ? option_at(s->options, k) : option_index(s->options, value) >= 0 ? wps_strdup(value) : NULL;
        if (!v) return false;
    } else if (!strcmp(s->kind, "checkbox")) {
        v = wps_strdup(!strcmp(value, "1") || !strcmp(value, "true") ? "1" : "0");
    } else {
        if (strchr(value, '\n') || strchr(value, '\t')) return false;
        v = wps_strdup(value);
    }
    wps_state_put_str(self, "WPSetup", key, v);
    free(v);
    WPObject_wpSaveDeferred(self);
    WPObject_wpObjectChanged(self);
    return true;
}

/* Pages from the table; every field set through wpSetSetting ("@"). */
char *WPSetup__wpAddSettingsPages(wpo_object *self, const char *pages)
{
    char *out = WPSetup_parent_wpAddSettingsPages(self, pages);
    const struct setting *t = table(self);
    const char *page = NULL;
    for (size_t i = 0; t && t[i].key; i++) {
        char *v = value_of(self, &t[i]);
        size_t n = strlen(t[i].label) + strlen(v ? v : "") + strlen(t[i].options ? t[i].options : "") + 256;
        char *line = malloc(n);
        size_t o = 0;
        if (!page || strcmp(page, t[i].page)) o += (size_t)snprintf(line + o, n - o, "page\t%s\n", t[i].page);
        page = t[i].page;
        if (!strcmp(t[i].kind, "choice"))
            snprintf(line + o, n - o, "field\t%s\tchoice\t%s\t%d\t@\t%s\n", t[i].key, t[i].label,
                     option_index(t[i].options, v ? v : ""), t[i].options);
        else if (!strcmp(t[i].kind, "checkbox"))
            snprintf(line + o, n - o, "field\t%s\tcheckbox\t%s\t%d\t@\n", t[i].key, t[i].label, v && !strcmp(v, "1"));
        else
            snprintf(line + o, n - o, "field\t%s\ttext\t%s\t%s\t@\n", t[i].key, t[i].label, v ? v : "");
        char *r = wps_concat(out, line);
        free(out);
        free(line);
        free(v);
        out = r;
    }
    return out;
}

/* Settings objects open as their notebook; they are not copied or made. */
char *WPSetup__wpModifyPopupMenu(wpo_object *self, const char *menu)
{
    (void)self;
    return wps_concat(menu, "2\tProperties\n");
}

char *WPSystem__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("System"); }
char *WPSound__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Sound"); }
char *WPMouse__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Mouse"); }
char *WPSchemePalette__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Scheme Palette"); }
char *WPLaunchPad__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("LaunchPad"); }
char *WPWarpCenter__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("WarpCenter"); }
char *WPMessages__wpclsQueryTitle(wpo_object *self) { (void)self; return wps_strdup("Messages"); }
