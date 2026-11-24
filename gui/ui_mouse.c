/*
 * Mouse mappings, as on OS/2's Mouse object (Mappings and Setup pages):
 * what each action is bound to, handedness, double-click time and wheel
 * speed. Read from <config>/mouse.conf; the Mouse settings object with
 * its notebook comes with the other settings objects (G3).
 *
 *   # action   = [modifiers+]buttonN [double]
 *   select      = button1
 *   open        = button1 double
 *   popup_menu  = button2
 *   edit_title  = alt+button1
 *   drag        = button2
 *   window_list = button3
 *   copy        = ctrl          # keys held at a drop
 *   shadow      = ctrl+shift
 *   drop_menu   = alt
 *   left_handed = no
 *   double_click_ms = 400
 *   wheel_speed = 3
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

ui_mouse_config ui_mouse = {
    .act = {
        [UI_ACT_SELECT] = { UI_BTN1, 0, 1 },
        [UI_ACT_OPEN] = { UI_BTN1, 0, 2 },
        [UI_ACT_POPUP] = { UI_BTN2, 0, 1 },
        [UI_ACT_EDIT_TITLE] = { UI_BTN1, UI_MOD_ALT, 1 },
        [UI_ACT_DRAG] = { UI_BTN2, 0, 1 },
        [UI_ACT_WINDOW_LIST] = { UI_BTN3, 0, 1 },
    },
    .copy_mods = UI_MOD_CTRL,
    .shadow_mods = UI_MOD_CTRL | UI_MOD_SHIFT,
    .ask_mods = UI_MOD_ALT,
    .left_handed = false,
    .double_click_ms = 400,
    .wheel_speed = 3,
};

static const char *names[UI_ACT_COUNT] = {
    [UI_ACT_SELECT] = "select", [UI_ACT_OPEN] = "open", [UI_ACT_POPUP] = "popup_menu",
    [UI_ACT_EDIT_TITLE] = "edit_title", [UI_ACT_DRAG] = "drag", [UI_ACT_WINDOW_LIST] = "window_list",
};

/* "ctrl+shift+button2 double" */
bool ui_mouse_parse_chord(const char *s, ui_chord *out)
{
    ui_chord c = { 0, 0, 1 };
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    char *dbl = strstr(buf, "double");
    if (dbl) { c.clicks = 2; *dbl = 0; }
    for (char *save = NULL, *t = strtok_r(buf, "+ \t", &save); t; t = strtok_r(NULL, "+ \t", &save)) {
        if (!strcmp(t, "alt")) c.mods |= UI_MOD_ALT;
        else if (!strcmp(t, "ctrl") || !strcmp(t, "control")) c.mods |= UI_MOD_CTRL;
        else if (!strcmp(t, "shift")) c.mods |= UI_MOD_SHIFT;
        else if (!strncmp(t, "button", 6) && t[6] >= '1' && t[6] <= '3' && !t[7]) c.button = t[6] - '0';
        else return false;
    }
    if (!c.button) return false;
    *out = c;
    return true;
}

/* "ctrl+shift" -> its modifiers; false for anything else (or none). */
static bool parse_mods(const char *s, unsigned *out)
{
    unsigned m = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *save = NULL, *t = strtok_r(buf, "+ \t", &save); t; t = strtok_r(NULL, "+ \t", &save)) {
        if (!strcmp(t, "alt")) m |= UI_MOD_ALT;
        else if (!strcmp(t, "ctrl") || !strcmp(t, "control")) m |= UI_MOD_CTRL;
        else if (!strcmp(t, "shift")) m |= UI_MOD_SHIFT;
        else return false;
    }
    if (!m) return false;
    *out = m;
    return true;
}

int ui_mouse_drop_how(unsigned mods)
{
    mods &= UI_MOD_CTRL | UI_MOD_SHIFT | UI_MOD_ALT;
    if (mods && mods == ui_mouse.shadow_mods) return 2;
    if (mods && mods == ui_mouse.copy_mods) return 1;
    if (mods && mods == ui_mouse.ask_mods) return -1;
    return 0;
}

static bool yes(const char *v) { return !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1"); }

void ui_mouse_load(void)
{
    char path[4096];
    const char *cfg = getenv("OSD_CONFIG_DIR"), *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cfg && *cfg) snprintf(path, sizeof path, "%s/mouse.conf", cfg);
    else if (xdg && *xdg) snprintf(path, sizeof path, "%s/objectspace/mouse.conf", xdg);
    else snprintf(path, sizeof path, "%s/.config/objectspace/mouse.conf", home ? home : "");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (isspace((unsigned char)*k)) k++;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]);) *--e = 0;
        while (isspace((unsigned char)*v)) v++;
        for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]);) *--e = 0;
        if (!ui_mouse_set(k, v)) fprintf(stderr, "osd-ui: %s:%d: cannot use \"%s = %s\"\n", path, n, k, v);
    }
    fclose(f);
}

bool ui_mouse_set(const char *k, const char *v)
{
    for (int a = 0; a < UI_ACT_COUNT; a++)
        if (!strcmp(k, names[a])) return ui_mouse_parse_chord(v, &ui_mouse.act[a]);
    if (!strcmp(k, "copy")) return parse_mods(v, &ui_mouse.copy_mods);
    if (!strcmp(k, "shadow")) return parse_mods(v, &ui_mouse.shadow_mods);
    if (!strcmp(k, "drop_menu")) return parse_mods(v, &ui_mouse.ask_mods);
    if (!strcmp(k, "left_handed")) ui_mouse.left_handed = yes(v);
    else if (!strcmp(k, "double_click_ms")) { int ms = atoi(v); if (ms < 100 || ms > 2000) return false; ui_mouse.double_click_ms = ms; }
    else if (!strcmp(k, "wheel_speed")) { double w = atof(v); if (w <= 0 || w > 20) return false; ui_mouse.wheel_speed = w; }
    else return false;
    return true;
}

/* Does this press (or release) make the action? The chord's modifiers
 * must be held; others are allowed (Ctrl and Shift change a selection). */
bool ui_mouse_is(ui_mouse_action a, const ui_pointer_event *e)
{
    const ui_chord *c = &ui_mouse.act[a];
    if (e->button != c->button || (e->mods & c->mods) != c->mods) return false;
    if (c->clicks == 2 && e->kind == UI_PRESS && e->clicks < 2) return false;
    /* An action without Alt is not made while Alt is held, when another
     * action on the same button uses Alt (edit title vs. select). */
    if (!(c->mods & UI_MOD_ALT) && (e->mods & UI_MOD_ALT))
        for (int k = 0; k < UI_ACT_COUNT; k++)
            if (k != (int)a && ui_mouse.act[k].button == c->button && (ui_mouse.act[k].mods & UI_MOD_ALT)) return false;
    return true;
}

int ui_mouse_button(ui_mouse_action a) { return ui_mouse.act[a].button; }
