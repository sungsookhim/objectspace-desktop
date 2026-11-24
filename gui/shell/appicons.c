/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Programs' own icons (see appicons.h). */
#define _GNU_SOURCE
#include "appicons.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "icons.h"

struct entry { char *desktop; char png[PATH_MAX]; bool asked; };
static struct entry *cache;
static size_t ncache;
static void (*on_changed)(void);

void appicons_init(void (*changed)(void)) { on_changed = changed; }

static bool exists(const char *p) { return access(p, R_OK) == 0; }

static bool icon_name(const char *desktop, char *out, size_t n)
{
    FILE *f = fopen(desktop, "r");
    if (!f) return false;
    char line[2048];
    bool in = false, found = false;
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in = !strcmp(line, "[Desktop Entry]"); continue; }
        if (in && !strncmp(line, "Icon=", 5) && line[5]) { snprintf(out, n, "%s", line + 5); found = true; }
    }
    fclose(f);
    return found;
}

/* A PNG (best) or an SVG for the icon called `name`. */
static bool theme_file(const char *name, char *png, size_t n, char *svg, size_t m)
{
    *png = *svg = 0;
    if (name[0] == '/') {
        size_t l = strlen(name);
        if (l > 4 && !strcasecmp(name + l - 4, ".png")) snprintf(png, n, "%s", name);
        else snprintf(svg, m, "%s", name);
        return exists(name);
    }
    static const char *const sizes[] = { "64x64", "48x48", "128x128", "96x96", "256x256", "32x32", "24x24", "22x22" };
    static const char *const themes[] = { "hicolor", "Adwaita", "AdwaitaLegacy" };
    const char *x = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char user[PATH_MAX], fp[PATH_MAX];
    if (x && *x) snprintf(user, sizeof user, "%s/icons", x);
    else snprintf(user, sizeof user, "%s/.local/share/icons", home ? home : "/nonexistent");
    snprintf(fp, sizeof fp, "%s/.local/share/flatpak/exports/share/icons", home ? home : "/nonexistent");
    const char *roots[] = { user, "/usr/share/icons", "/usr/local/share/icons", "/var/lib/flatpak/exports/share/icons", fp };
    char p[PATH_MAX];
    /* (programs' icons are "apps"; notifications and tray icons name
     * "status" ones too, some only in AdwaitaLegacy's "legacy") */
    static const char *const cats[] = { "apps", "status", "devices", "categories", "legacy" };
    for (size_t c = 0; c < sizeof cats / sizeof *cats; c++)
    for (size_t t = 0; t < 3; t++)
        for (size_t r = 0; r < 5; r++) {
            for (size_t z = 0; z < sizeof sizes / sizeof *sizes; z++) {
                snprintf(p, sizeof p, "%s/%s/%s/%s/%s.png", roots[r], themes[t], sizes[z], cats[c], name);
                if (exists(p)) { snprintf(png, n, "%s", p); return true; }
            }
            snprintf(p, sizeof p, "%s/%s/scalable/%s/%s.svg", roots[r], themes[t], cats[c], name);
            if (!*svg && exists(p)) snprintf(svg, m, "%s", p);
        }
    if (*svg) return true;
    /* symbolic ones last (Adwaita's status icons, many tray icons'):
     * NAME-symbolic, or NAME if it is one */
    for (size_t c = 0; c < sizeof cats / sizeof *cats; c++)
        for (size_t r = 0; r < 5; r++) {
            snprintf(p, sizeof p, "%s/Adwaita/symbolic/%s/%s%s.svg", roots[r], cats[c], name,
                     strstr(name, "-symbolic") ? "" : "-symbolic");
            if (exists(p)) { snprintf(svg, m, "%s", p); return true; }
        }
    snprintf(p, sizeof p, "/usr/share/pixmaps/%s.png", name);
    if (exists(p)) { snprintf(png, n, "%s", p); return true; }
    snprintf(p, sizeof p, "/usr/share/pixmaps/%s.svg", name);
    if (exists(p)) { snprintf(svg, m, "%s", p); return true; }
    return false;
}

static void made(void *ctx, const char *png)
{
    size_t i = (size_t)(uintptr_t)ctx;
    if (i < ncache && png) snprintf(cache[i].png, sizeof cache[i].png, "%s", png);
    if (on_changed) on_changed();
}

/* By a desktop entry's path, or ("\x01" first) by an icon's name or path. */
static cairo_surface_t *lookup(const char *key)
{
    for (size_t i = 0; i < ncache; i++)
        if (!strcmp(cache[i].desktop, key)) return cache[i].png[0] ? icons_get(cache[i].png) : NULL;
    cache = realloc(cache, (ncache + 1) * sizeof *cache);
    size_t i = ncache++;
    struct entry *e = &cache[i];
    memset(e, 0, sizeof *e);
    e->desktop = strdup(key);
    char name[512], png[PATH_MAX], svg[PATH_MAX];
    if (key[0] == '\x01') snprintf(name, sizeof name, "%s", key + 1);
    else if (!icon_name(key, name, sizeof name)) return NULL;
    if (!theme_file(name, png, sizeof png, svg, sizeof svg)) return NULL;
    if (png[0]) { snprintf(e->png, sizeof e->png, "%s", png); return icons_get(e->png); }
    char out[PATH_MAX];
    if (icons_preview(svg, 64, out, sizeof out, made, (void *)(uintptr_t)i)) {
        snprintf(cache[i].png, sizeof cache[i].png, "%s", out);
        return icons_get(cache[i].png);
    }
    return NULL;   /* being made */
}

cairo_surface_t *appicon_for(const char *desktop)
{
    if (!desktop || !*desktop) return NULL;
    return lookup(desktop);
}

cairo_surface_t *appicon_named(const char *name)
{
    if (!name || !*name) return NULL;
    if (!strncmp(name, "file://", 7)) name += 7;
    char key[PATH_MAX + 2];
    snprintf(key, sizeof key, "\x01%s", name);
    return lookup(key);
}
