/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Action events and the sound scheme (events.h). */
#define _GNU_SOURCE
#include "events.h"
#include "gnome.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sound.h"

#define MAX_SUBS 64   /* (GUI plug-ins and scripts later; 8 was the sound player and the self-tests) */

static struct { osd_event_fn fn; void *ctx; } subs[MAX_SUBS];
static int nsubs;

/* ----------------------------------------------------------- the scheme */

/* What plays by default: few sounds (gui-design.md), freedesktop names. */
static const char *const defaults[][2] = {
    { "alert.error", "dialog-error" },
    { "alert.warning", "dialog-warning" },
    { "alert.info", "dialog-information" },
    { "job.done", "complete" },
    { "job.failed", "dialog-error" },
    { "object.trash", "trash-empty" },
    { "shell.startup", "service-login" },
};

struct entry { char name[64]; char sound[PATH_MAX]; };
static struct entry *scheme;
static size_t nscheme;
static bool enabled = true;       /* the scheme's own switch */
static bool gnome_sounds = true;  /* GNOME's event-sounds switch */
static float volume = 0.7f;
static char theme[128] = "freedesktop";

static void set(const char *name, const char *sound)
{
    for (size_t i = 0; i < nscheme; i++)
        if (!strcmp(scheme[i].name, name)) { snprintf(scheme[i].sound, sizeof scheme[i].sound, "%s", sound); return; }
    struct entry *n = realloc(scheme, (nscheme + 1) * sizeof *n);
    if (!n) return;
    scheme = n;
    snprintf(scheme[nscheme].name, sizeof scheme[nscheme].name, "%s", name);
    snprintf(scheme[nscheme].sound, sizeof scheme[nscheme].sound, "%s", sound);
    nscheme++;
}

/* GNOME's settings: gnome.c's reader (none with OSD_NO_GSETTINGS: tests) */
static bool gsetting(const char *schema, const char *key, char *out, size_t n)
{
    if (getenv("OSD_NO_GSETTINGS")) return false;
    return gnome_get(schema, key, out, n);
}

static void config_path(char *out, size_t n, const char *file)
{
    const char *cfg = getenv("OSD_CONFIG_DIR"), *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cfg && *cfg) snprintf(out, n, "%s/%s", cfg, file);
    else if (xdg && *xdg) snprintf(out, n, "%s/objectspace/%s", xdg, file);
    else snprintf(out, n, "%s/.config/objectspace/%s", home ? home : "", file);
}

static void load(void)
{
    nscheme = 0;
    for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++) set(defaults[i][0], defaults[i][1]);
    char v[128];
    enabled = true;
    volume = 0.7f;
    gnome_sounds = true;
    if (gsetting("org.gnome.desktop.sound", "event-sounds", v, sizeof v)) gnome_sounds = strcmp(v, "false") != 0;
    if (gsetting("org.gnome.desktop.sound", "theme-name", v, sizeof v) && *v) snprintf(theme, sizeof theme, "%s", v);
    char path[PATH_MAX], line[PATH_MAX + 128];
    config_path(path, sizeof path, "sounds.conf");
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *val = eq + 1;
        while (isspace((unsigned char)*k)) k++;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]);) *--e = 0;
        while (isspace((unsigned char)*val)) val++;
        for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]);) *--e = 0;
        if (!strcmp(k, "volume")) { int x = atoi(val); volume = (float)(x < 0 ? 0 : x > 100 ? 100 : x) / 100.0f; }
        else if (!strcmp(k, "enabled")) enabled = !strcmp(val, "yes") || !strcmp(val, "true") || !strcmp(val, "1");
        else if (strchr(k, '.')) set(k, !strcmp(val, "none") ? "" : val);
        else fprintf(stderr, "osd-shell: %s: unknown setting \"%s\"\n", path, k);
    }
    if (f) fclose(f);
}

/* sounds.conf (before the Sound object): its settings as a setup string,
 * to be moved into the Sound object once ("enabled=1;volume=70;window.open=bell"),
 * or NULL if there is none. */
char *events_conf_setup(void)
{
    char path[PATH_MAX], line[PATH_MAX + 128];
    config_path(path, sizeof path, "sounds.conf");
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *out = malloc(cap);
    out[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *val = eq + 1;
        while (isspace((unsigned char)*k)) k++;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]);) *--e = 0;
        while (isspace((unsigned char)*val)) val++;
        for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]);) *--e = 0;
        if (strcmp(k, "volume") && strcmp(k, "enabled") && !strchr(k, '.')) continue;
        if (!strcmp(val, "none")) val = "";
        if (n + strlen(k) + 2 * strlen(val) + 4 >= cap) break;
        n += (size_t)snprintf(out + n, cap - n, "%s=", k);
        for (const char *c = val; *c; c++) {   /* (";" and "\" escaped) */
            if (*c == ';' || *c == '\\') out[n++] = '\\';
            out[n++] = *c;
        }
        out[n++] = ';';
        out[n] = 0;
    }
    fclose(f);
    return out;
}

/* ... moved: kept as sounds.conf.imported. */
void events_conf_imported(void)
{
    char path[PATH_MAX], to[PATH_MAX + 16];
    config_path(path, sizeof path, "sounds.conf");
    snprintf(to, sizeof to, "%s.imported", path);
    rename(path, to);
}

/* A theme sound by name (sound theme spec: <dir>/sounds/<theme>/stereo/<name>.<ext>), or a path. */
static bool find_sound(const char *name, char *out, size_t n)
{
    if (!*name) return false;
    if (name[0] == '/') { snprintf(out, n, "%s", name); return access(out, R_OK) == 0; }
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME"), *dd = getenv("XDG_DATA_DIRS");
    char dirs[16][PATH_MAX];
    int nd = 0;
    if (dh && *dh) snprintf(dirs[nd++], PATH_MAX, "%s/sounds", dh);
    else if (home) snprintf(dirs[nd++], PATH_MAX, "%s/.local/share/sounds", home);
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", dd && *dd ? dd : "/usr/local/share:/usr/share");
    for (char *s = copy, *t; nd < 16 && (t = strsep(&s, ":"));)
        if (*t) {
            size_t l = strlen(t);
            while (l > 1 && t[l - 1] == '/') t[--l] = 0;   /* "/usr/share/" */
            snprintf(dirs[nd++], PATH_MAX, "%s/sounds", t);
        }
    const char *themes[2] = { theme, "freedesktop" };
    static const char *const exts[] = { "oga", "ogg", "wav", "flac", "opus" };
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < nd; i++)
            for (size_t e = 0; e < sizeof exts / sizeof *exts; e++) {
                snprintf(out, n, "%s/%s/stereo/%s.%s", dirs[i], themes[t], name, exts[e]);
                if (access(out, R_OK) == 0) return true;
            }
    return false;
}

const char *events_sound_for(const char *name)
{
    static char path[PATH_MAX];
    if (!enabled || !gnome_sounds) return NULL;
    for (size_t i = 0; i < nscheme; i++)
        if (!strcmp(scheme[i].name, name)) return find_sound(scheme[i].sound, path, sizeof path) ? path : NULL;
    return NULL;
}

/* An object's own sound for the next event of a name (its on:EVENT
 * action, the service's; "" none): in place of the Sound object's. */
static char once_name[64], once_sound[PATH_MAX];
static bool once_set;

void events_sound_once(const char *name, const char *sound)
{
    snprintf(once_name, sizeof once_name, "%s", name);
    snprintf(once_sound, sizeof once_sound, "%s", sound ? sound : "");
    once_set = sound != NULL;
}

static void play_sound(const osd_event *e, void *ctx)
{
    (void)ctx;
    const char *p;
    static char own[PATH_MAX];
    if (once_set && !strcmp(once_name, e->name)) {
        once_set = false;
        if (!enabled || !gnome_sounds || !once_sound[0] || !find_sound(once_sound, own, sizeof own)) return;
        p = own;
    } else {
        p = events_sound_for(e->name);
    }
    if (p && !sound_play(p, volume) && getenv("OSD_DEBUG_EVENTS"))
        fprintf(stderr, "event: cannot play %s (formats: %s)\n", p, sound_formats());
}

/* ------------------------------------------------------------------ bus */

void events_init(void)
{
    load();
    sound_init();
    events_subscribe(play_sound, NULL);
}

void events_reload(void) { load(); }

void events_setup_begin(void)
{
    nscheme = 0;   /* the Sound object lists every event */
}

void events_setup_set(const char *key, const char *value)
{
    if (!strcmp(key, "enabled")) enabled = !strcmp(value, "1");
    else if (!strcmp(key, "volume")) { int x = atoi(value); volume = (float)(x < 0 ? 0 : x > 100 ? 100 : x) / 100.0f; }
    else if (strchr(key, '.')) set(key, value);
}

void events_shutdown(void)
{
    sound_shutdown();
    free(scheme);
    scheme = NULL;
    nscheme = 0;
    nsubs = 0;
}

void events_subscribe(osd_event_fn fn, void *ctx)
{
    if (nsubs < MAX_SUBS) subs[nsubs++] = (typeof(subs[0])){ fn, ctx };
}

void events_raise(const char *name, const char *object, const char *cls, ui_rect at)
{
    osd_event e = { name, object ? object : "", cls ? cls : "", at };
    if (getenv("OSD_DEBUG_EVENTS"))
        fprintf(stderr, "event: %s %s%s%s at %.0f,%.0f%s%s\n", name, e.cls, *e.cls ? " " : "", e.object, at.x, at.y,
                events_sound_for(name) ? " -> " : "", events_sound_for(name) ? events_sound_for(name) : "");
    for (int i = 0; i < nsubs; i++) subs[i].fn(&e, subs[i].ctx);
}

bool events_play(const char *value)
{
    char path[PATH_MAX];
    if (!value || !find_sound(value, path, sizeof path)) return false;
    return sound_play(path, volume);
}
