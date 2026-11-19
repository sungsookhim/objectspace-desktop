/* Action events and the sound scheme (events.h). */
#define _GNU_SOURCE
#include "events.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sound.h"

#define MAX_SUBS 8

static struct { wps_event_fn fn; void *ctx; } subs[MAX_SUBS];
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

/* `gsettings get`, unquoted. */
static bool gsetting(const char *schema, const char *key, char *out, size_t n)
{
    if (getenv("WPS_NO_GSETTINGS")) return false;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "gsettings get %s %s 2>/dev/null", schema, key);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char buf[512] = "";
    bool ok = fgets(buf, sizeof buf, p) != NULL;
    pclose(p);
    if (!ok) return false;
    buf[strcspn(buf, "\n")] = 0;
    char *s = buf;
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '\'' && s[l - 1] == '\'') { s[l - 1] = 0; s++; }
    snprintf(out, n, "%s", s);
    return true;
}

static void config_path(char *out, size_t n, const char *file)
{
    const char *cfg = getenv("WPS_CONFIG_DIR"), *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cfg && *cfg) snprintf(out, n, "%s/%s", cfg, file);
    else if (xdg && *xdg) snprintf(out, n, "%s/wps/%s", xdg, file);
    else snprintf(out, n, "%s/.config/wps/%s", home ? home : "", file);
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
        else fprintf(stderr, "wps-shell: %s: unknown setting \"%s\"\n", path, k);
    }
    if (f) fclose(f);
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

static void play_sound(const wps_event *e, void *ctx)
{
    (void)ctx;
    const char *p = events_sound_for(e->name);
    if (p && !sound_play(p, volume) && getenv("WPS_DEBUG_EVENTS"))
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

void events_subscribe(wps_event_fn fn, void *ctx)
{
    if (nsubs < MAX_SUBS) subs[nsubs++] = (typeof(subs[0])){ fn, ctx };
}

void events_raise(const char *name, const char *object, const char *cls, ui_rect at)
{
    wps_event e = { name, object ? object : "", cls ? cls : "", at };
    if (getenv("WPS_DEBUG_EVENTS"))
        fprintf(stderr, "event: %s %s%s%s at %.0f,%.0f%s%s\n", name, e.cls, *e.cls ? " " : "", e.object, at.x, at.y,
                events_sound_for(name) ? " -> " : "", events_sound_for(name) ? events_sound_for(name) : "");
    for (int i = 0; i < nsubs; i++) subs[i].fn(&e, subs[i].ctx);
}
