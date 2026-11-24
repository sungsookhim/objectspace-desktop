/* Icon themes (osd_themes.h): folders of .wpsicon packages with a
 * theme.ini (Name=), in the user's $XDG_DATA_HOME/wps/icon-themes, the
 * system's (beside OSD_ICONDIR: /usr/share/wps/icon-themes), and a build
 * tree's build/icon-themes (a program run from it). The user's first. */
#define _GNU_SOURCE
#include "osd_themes.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef OSD_ICONDIR
#define OSD_ICONDIR "/usr/share/wps/icons"
#endif

static size_t roots(char out[][PATH_MAX], size_t max)
{
    size_t n = 0;
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out[n++], PATH_MAX, "%s/wps/icon-themes", xdg);
    else if (home && *home) snprintf(out[n++], PATH_MAX, "%s/.local/share/wps/icon-themes", home);
    const char *sys = getenv("OSD_ICON_THEMES");   /* (tests, and a checkout: one more folder) */
    if (sys && *sys && n < max) snprintf(out[n++], PATH_MAX, "%s", sys);
    char icondir[PATH_MAX];
    snprintf(icondir, sizeof icondir, "%s", OSD_ICONDIR);
    char *sl = strrchr(icondir, '/');
    if (sl && n < max) { *sl = 0; snprintf(out[n++], PATH_MAX, "%s/icon-themes", icondir); }
    /* a program run from a build tree (build/wpsd, build/wps-shell) */
    char exe[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (l > 0 && n < max) {
        exe[l] = 0;
        char *s = strrchr(exe, '/');
        if (s) { *s = 0; snprintf(out[n++], PATH_MAX, "%s/icon-themes", exe); }
    }
    return n;
}

size_t osd_icon_themes(osd_icon_theme *out, size_t max)
{
    char r[6][PATH_MAX];
    size_t nr = roots(r, 6), n = 0;
    for (size_t i = 0; i < nr && n < max; i++) {
        DIR *d = opendir(r[i]);
        struct dirent *de;
        while (d && n < max && (de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char ini[PATH_MAX * 2], line[256], name[128] = "";
            snprintf(ini, sizeof ini, "%s/%s/theme.ini", r[i], de->d_name);
            FILE *f = fopen(ini, "r");
            if (!f) continue;
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "Name=", 5)) { line[strcspn(line, "\r\n")] = 0; snprintf(name, sizeof name, "%s", line + 5); }
            fclose(f);
            if (!name[0]) snprintf(name, sizeof name, "%s", de->d_name);
            bool seen = false;   /* (a theme of one id: the first folder's, the user's) */
            for (size_t k = 0; k < n && !seen; k++) seen = !strcmp(out[k].id, de->d_name);
            if (seen) continue;
            for (char *c = name; *c; c++) if (*c == ',') *c = ' ';   /* (a choice's options are comma separated) */
            snprintf(out[n].id, sizeof out[n].id, "%s", de->d_name);
            snprintf(out[n].name, sizeof out[n].name, "%s", name);
            snprintf(out[n].dir, sizeof out[n].dir, "%s/%s", r[i], de->d_name);
            n++;
        }
        if (d) closedir(d);
    }
    /* WPS's own first, flat then outline then classic; then the others by name */
    static const char *const order[] = { "wps-flat", "wps-outline", "classic" };
    size_t k = 0;
    for (size_t o = 0; o < 3; o++)
        for (size_t i = k; i < n; i++)
            if (!strcmp(out[i].id, order[o])) { osd_icon_theme t = out[k]; out[k] = out[i]; out[i] = t; k++; break; }
    for (size_t i = k; i + 1 < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (strcasecmp(out[j].name, out[i].name) < 0) { osd_icon_theme t = out[i]; out[i] = out[j]; out[j] = t; }
    return n;
}

bool osd_icon_theme_dir(const char *theme, char *out, size_t n)
{
    osd_icon_theme t[32];
    size_t nt = osd_icon_themes(t, 32);
    for (size_t i = 0; theme && *theme && i < nt; i++)
        if (!strcasecmp(t[i].name, theme) || !strcmp(t[i].id, theme)) { snprintf(out, n, "%s", t[i].dir); return true; }
    return false;
}

char *osd_icon_theme_names(void)
{
    osd_icon_theme t[32];
    size_t nt = osd_icon_themes(t, 32), len = 0;
    char *out = malloc(32 * 130 + 1);
    if (!out) return NULL;
    out[0] = 0;
    for (size_t i = 0; i < nt; i++) len += (size_t)sprintf(out + len, "%s%s", len ? "," : "", t[i].name);
    return out;
}

/* (the GUI's icons_name_for_type; the service's for an Icon page) */
const char *osd_icon_name_for_type(const char *m)
{
    if (!m || !*m) return "file-data";
    static const struct { const char *prefix, *icon; } map[] = {
        { "image/", "file-image" }, { "audio/", "file-audio" }, { "video/", "file-video" }, { "font/", "file-font" },
        { "application/x-font", "file-font" }, { "application/vnd.ms-fontobject", "file-font" },
        { "text/x-shellscript", "file-script" }, { "application/x-shellscript", "file-script" },
        { "application/x-perl", "file-script" }, { "text/x-python", "file-code" }, { "text/x-", "file-code" },
        { "application/javascript", "file-code" }, { "application/json", "file-code" }, { "application/xml", "file-code" },
        { "text/csv", "file-spreadsheet" }, { "text/", "file-text" },
        { "application/pdf", "file-document" }, { "application/msword", "file-document" }, { "application/rtf", "file-document" },
        { "application/vnd.oasis.opendocument.text", "file-document" },
        { "application/vnd.openxmlformats-officedocument.wordprocessingml", "file-document" },
        { "application/vnd.oasis.opendocument.spreadsheet", "file-spreadsheet" }, { "application/vnd.ms-excel", "file-spreadsheet" },
        { "application/vnd.openxmlformats-officedocument.spreadsheetml", "file-spreadsheet" },
        { "application/vnd.oasis.opendocument.presentation", "file-presentation" }, { "application/vnd.ms-powerpoint", "file-presentation" },
        { "application/vnd.openxmlformats-officedocument.presentationml", "file-presentation" },
        { "application/x-iso9660-image", "file-disc-image" }, { "application/x-cd-image", "file-disc-image" },
        { "application/x-raw-disk-image", "file-disc-image" },
        { "application/x-rpm", "file-package" }, { "application/vnd.debian.binary-package", "file-package" },
        { "application/vnd.flatpak", "file-package" }, { "application/vnd.appimage", "file-package" },
        { "application/zip", "file-archive" }, { "application/x-tar", "file-archive" }, { "application/gzip", "file-archive" },
        { "application/x-compressed-tar", "file-archive" }, { "application/x-7z", "file-archive" },
        { "application/vnd.rar", "file-archive" }, { "application/x-rar", "file-archive" }, { "application/x-xz", "file-archive" },
        { "application/x-bzip", "file-archive" }, { "application/zstd", "file-archive" },
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strncmp(m, map[i].prefix, strlen(map[i].prefix))) return map[i].icon;
    return "file-data";
}

const char *osd_icon_name_for_class(const char *cls)
{
    static const char *const map[][2] = {
        { "OsdSystem", "system" }, { "OsdSound", "sound" }, { "OsdMouse", "mouse" },
        { "OsdSchemePalette", "scheme-palette" }, { "OsdScheme", "scheme-palette" }, { "OsdFontPalette", "file-font" }, { "OsdColorPalette", "scheme-palette" },
        { "OsdLaunchPad", "launchpad" }, { "OsdWarpCenter", "warpcenter" }, { "OsdMessages", "messages" },
        { "OsdMinWinViewer", "minwin-viewer" }, { "OsdRestoreDesktop", "restore-desktop" },
    };
    for (size_t i = 0; cls && i < sizeof map / sizeof *map; i++) if (!strcmp(cls, map[i][0])) return map[i][1];
    return NULL;
}
