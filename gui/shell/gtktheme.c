/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * A GTK 3 theme from the scheme (0.7 step 7; v0.6-design 8): GTK 3
 * programs in the Objectspace session drawn in the scheme's colours, with
 * OS/2's bevels (square buttons raised by a light and a dark edge, fields
 * sunken), not GNOME's Adwaita.
 *
 * ~/.local/share/themes/Objectspace-<scheme>/gtk-3.0/gtk.css: Adwaita (or
 * Adwaita dark, for a dark scheme) underneath, so every widget has a look;
 * over it the scheme's colours for windows, text, fields, lists,
 * selections, buttons, title bars, menus, tooltips, tabs and scroll bars.
 * Made again when the scheme changes. Chosen for the session's programs
 * through GNOME's gtk-theme setting, which GTK 3 programs follow (at
 * once, running ones too) and libadwaita's ignore (GTK_THEME, in the
 * environment, would be seen by libadwaita's too, which then lose their
 * own look). In the Objectspace session the desktop sets it at login and
 * when the scheme changes, keeping the theme GNOME had; started inside
 * GNOME, it puts that one back if one of ours is set, so GNOME's session
 * keeps its own.
 * OSD_GTK_THEME=0: none.
 */
#define _GNU_SOURCE
#include "shell.h"

#include <ctype.h>
#include <math.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

static void hex(char *out, size_t n, ui_color c)
{
    snprintf(out, n, "#%02x%02x%02x", (int)lround(c.r * 255), (int)lround(c.g * 255), (int)lround(c.b * 255));
}

/* "high contrast" -> "Objectspace-high-contrast" */
void gtktheme_name(const char *scheme, char *out, size_t n)
{
    char slug[96];
    size_t k = 0;
    for (const char *p = scheme ? scheme : "classic"; *p && k + 1 < sizeof slug; p++)
        slug[k++] = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '-';
    slug[k] = 0;
    snprintf(out, n, "Objectspace-%s", slug[0] ? slug : "classic");
}

static bool mkdirs(const char *path)
{
    char p[4096];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    return mkdir(p, 0755) == 0 || errno == EEXIST;
}

static bool write_file(const char *path, const char *text)
{
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* The theme's stylesheet for the scheme now. Caller frees. */
char *gtktheme_css(void)
{
    const ui_scheme *k = &ui_scheme_now;
    char face[8], light[8], shadow[8], dark[8], text[8], dis[8], base[8], sel[8], seltext[8];
    char title[8], titletext[8], titlein[8], titletextin[8], menu[8], menutext[8], hil[8], hiltext[8], tip[8], tiptext[8];
    hex(face, sizeof face, k->face); hex(light, sizeof light, k->light); hex(shadow, sizeof shadow, k->shadow);
    hex(dark, sizeof dark, k->dark); hex(text, sizeof text, k->text); hex(dis, sizeof dis, k->disabled_text);
    hex(base, sizeof base, k->client_bg); hex(sel, sizeof sel, k->selection); hex(seltext, sizeof seltext, k->selection_text);
    hex(title, sizeof title, k->title_active); hex(titletext, sizeof titletext, k->title_text);
    hex(titlein, sizeof titlein, k->title_inactive); hex(titletextin, sizeof titletextin, k->title_text_inactive);
    hex(menu, sizeof menu, k->menu_bg); hex(menutext, sizeof menutext, k->menu_text);
    hex(hil, sizeof hil, k->menu_hilite); hex(hiltext, sizeof hiltext, k->menu_hilite_text);
    hex(tip, sizeof tip, k->tooltip_bg); hex(tiptext, sizeof tiptext, k->tooltip_text);
    bool is_dark = 0.2126 * k->client_bg.r + 0.7152 * k->client_bg.g + 0.0722 * k->client_bg.b < 0.4;
    char *css = NULL;
    if (asprintf(&css,
        "/* Made by Objectspace from the scheme \"%s\" (made again when it changes: edits are lost). */\n"
        "@import url(\"resource:///org/gtk/libgtk/theme/Adwaita/gtk-contained%s.css\");\n"
        "\n"
        "@define-color theme_bg_color %s;\n@define-color theme_fg_color %s;\n@define-color theme_base_color %s;\n"
        "@define-color theme_text_color %s;\n@define-color theme_selected_bg_color %s;\n@define-color theme_selected_fg_color %s;\n"
        "@define-color insensitive_fg_color %s;\n@define-color borders %s;\n"
        "@define-color bg_color %s;\n@define-color fg_color %s;\n@define-color base_color %s;\n@define-color text_color %s;\n"
        "@define-color selected_bg_color %s;\n@define-color selected_fg_color %s;\n"
        "\n"
        "/* windows, dialogs */\n"
        "window, .background, dialog, messagedialog .dialog-vbox, box.dialog-action-box { background-color: %s; color: %s; }\n"
        "label { color: inherit; }\n"
        "*:disabled, label:disabled { color: %s; }\n"
        "\n"
        "/* fields and lists: the window's background, sunken */\n"
        ".view, textview text, iconview, treeview.view, list, row, entry, spinbutton:not(.vertical) { background-color: %s; color: %s; }\n"
        "entry, spinbutton:not(.vertical), textview, scrolledwindow.frame, frame > border, .frame {\n"
        "  border-radius: 0; box-shadow: none; border-style: solid; border-width: 2px; border-color: %s %s %s %s; }\n"
        "entry:focus-within, entry:focus, spinbutton:focus-within:not(.vertical) {\n"
        "  box-shadow: none; border-color: %s %s %s %s; outline: 1px dotted %s; outline-offset: -3px; }\n"
        "\n"
        "/* selections */\n"
        "*:selected, .view:selected, treeview.view:selected, row:selected, iconview:selected, selection, flowboxchild:selected,\n"
        "entry selection, entry selection:focus, textview text selection, textview text selection:focus, label selection,\n"
        "spinbutton:not(.vertical) selection, .view text selection:focus,\n"
        "*:selected:focus, .view:selected:focus, treeview.view:selected:focus, row:selected:focus, iconview:selected:focus,\n"
        "placessidebar row:selected, placessidebar row:selected:focus {\n"
        "  background-color: %s; color: %s; }\n"
        "\n"
        "/* buttons: raised, square (OS/2) */\n"
        "button, button.flat:hover, combobox button, spinbutton button, modelbutton.flat:hover {\n"
        "  background-image: none; background-color: %s; color: %s; text-shadow: none; -gtk-icon-shadow: none;\n"
        "  border-radius: 0; box-shadow: none; border-style: solid; border-width: 2px; border-color: %s %s %s %s; }\n"
        "button:hover { background-image: none; background-color: %s; }\n"
        "button:active, button:checked { background-image: none; background-color: %s; border-color: %s %s %s %s; }\n"
        "button:disabled { background-color: %s; color: %s; }\n"
        "button.flat { border-color: transparent; background-color: transparent; }\n"
        "button:focus { outline: 1px dotted %s; outline-offset: -4px; }\n"
        "\n"
        "/* title bars drawn by the program (header bars) */\n"
        "headerbar, .titlebar, headerbar.titlebar {\n"
        "  background-image: none; background-color: %s; color: %s; border-radius: 0; box-shadow: none; border-color: %s; }\n"
        "headerbar .title, headerbar .subtitle, .titlebar .title { color: %s; }\n"
        "headerbar:backdrop, .titlebar:backdrop { background-color: %s; color: %s; }\n"
        "headerbar:backdrop .title { color: %s; }\n"
        "\n"
        "/* menus */\n"
        "menubar, menu, .menu, .context-menu, popover.menu, popover.background { background-color: %s; color: %s; border-radius: 0; }\n"
        "menuitem, menubar > menuitem { color: %s; }\n"
        "menuitem:hover, menubar > menuitem:hover, popover.menu modelbutton:hover { background-color: %s; color: %s; }\n"
        "menuitem:hover label, menubar > menuitem:hover label { color: %s; }\n"
        "\n"
        "/* tooltips */\n"
        "tooltip, tooltip.background { background-color: %s; color: %s; border-radius: 0; box-shadow: none; text-shadow: none; }\n"
        "tooltip * { color: %s; }\n"
        "\n"
        "/* notebooks' tabs */\n"
        "notebook > header { background-color: %s; }\n"
        "notebook > header > tabs > tab { border-radius: 0; color: %s; }\n"
        "notebook > header.top > tabs > tab:checked, notebook > header.bottom > tabs > tab:checked,\n"
        "notebook > header.left > tabs > tab:checked, notebook > header.right > tabs > tab:checked,\n"
        "notebook > header > tabs > tab:checked { background-color: %s; color: %s; box-shadow: none; border-color: %s %s transparent %s; }\n"
        "notebook > header.top > tabs > tab:hover, notebook > header > tabs > tab:hover { box-shadow: none; }\n"
        "notebook > stack:not(:only-child) { background-color: %s; }\n"
        "\n"
        "/* scroll bars, progress */\n"
        "scrollbar { background-color: %s; }\n"
        "scrollbar slider { border-radius: 0; background-color: %s; border: 1px solid %s; }\n"
        "progressbar progress { background-image: none; background-color: %s; border-radius: 0; }\n"
        "progressbar trough { border-radius: 0; }\n",
        k->name ? k->name : "", is_dark ? "-dark" : "",
        face, text, base, text, sel, seltext, dis, shadow, face, text, base, text, sel, seltext,
        face, text,
        dis,
        base, text,
        shadow, light, light, shadow,
        shadow, light, light, shadow, text,
        sel, seltext,
        face, text, light, dark, dark, light,
        face,
        face, dark, light, light, dark,
        face, dis,
        text,
        title, titletext, dark,
        titletext,
        titlein, titletextin,
        titletextin,
        menu, menutext,
        menutext,
        hil, hiltext,
        hiltext,
        tip, tiptext,
        tiptext,
        face,
        text,
        face, text, light, dark, light,
        face,
        face,
        shadow, dark,
        sel) < 0)
        return NULL;
    return css;
}

static void gsettings_set(const char *key, const char *value)
{
    char arg[200];
    snprintf(arg, sizeof arg, "'%s'", value);
    char *argv[] = { "gsettings", "set", "org.gnome.desktop.interface", (char *)key, arg, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0) waitpid(pid, NULL, 0);
}

/* Where GNOME's own value of a key (gtk-theme, cursor-theme) is kept
 * while ours is set. */
static void kept_path(const char *key, char *out, size_t n)
{
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (dh && *dh) snprintf(out, n, "%s/themes/.objectspace-gnome-%s", dh, key);
    else snprintf(out, n, "%s/.local/share/themes/.objectspace-gnome-%s", home ? home : "", key);
}

static bool off(void)
{
    const char *e = getenv("OSD_GTK_THEME");
    return (e && !strcmp(e, "0")) || getenv("OSD_NO_GSETTINGS");
}

/* GNOME's key set to one of ours (a name starting "Objectspace"), GNOME's
 * own kept the first time; NULL: GNOME's own back. True: changed. */
static bool gnome_ours(const char *key, const char *ours, const char *dflt)
{
    char now[200] = "", kp[4200];
    gnome_get("org.gnome.desktop.interface", key, now, sizeof now);
    kept_path(key, kp, sizeof kp);
    if (!ours) {
        if (strncmp(now, "Objectspace", 11)) return false;
        char was[200] = "";
        FILE *f = fopen(kp, "r");
        if (f) { if (fgets(was, sizeof was, f)) was[strcspn(was, "\n")] = 0; fclose(f); }
        gsettings_set(key, was[0] ? was : dflt);
        return true;
    }
    if (!strcmp(now, ours)) return false;
    if (strncmp(now, "Objectspace", 11)) {   /* GNOME's own: kept, to be put back */
        char dir[4200];
        snprintf(dir, sizeof dir, "%s", kp);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; mkdirs(dir); }
        write_file(kp, now[0] ? now : dflt);
    }
    gsettings_set(key, ours);
    return true;
}

/* The theme for the scheme now written and, in the Objectspace session,
 * set as GNOME's gtk-theme (GNOME's own kept). The desktop's. */
void gtktheme_update(void)
{
    if (off() || !getenv("OSD_SESSION")) return;
    char name[160], dir[4096], path[4200];
    gtktheme_name(ui_scheme_now.name, name, sizeof name);
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (dh && *dh) snprintf(dir, sizeof dir, "%s/themes/%s", dh, name);
    else snprintf(dir, sizeof dir, "%s/.local/share/themes/%s", home ? home : "", name);
    char gtk3[4100];
    snprintf(gtk3, sizeof gtk3, "%s/gtk-3.0", dir);
    if (!mkdirs(gtk3)) return;
    char *css = gtktheme_css();
    if (!css) return;
    snprintf(path, sizeof path, "%s/gtk.css", gtk3);
    bool ok = write_file(path, css);
    free(css);
    char index[600];
    snprintf(index, sizeof index,
             "[Desktop Entry]\nType=X-GNOME-Metatheme\nName=%s\nComment=Made by Objectspace from its scheme \"%s\"\n"
             "\n[X-GNOME-Metatheme]\nGtkTheme=%s\n", name, ui_scheme_now.name ? ui_scheme_now.name : "", name);
    snprintf(path, sizeof path, "%s/index.theme", dir);
    write_file(path, index);
    if (!ok) return;
    if (gnome_ours("gtk-theme", name, "Adwaita")) fprintf(stderr, "osd-shell: GTK 3 programs: the theme %s\n", name);
}

/* The System's pointers (0.7 step 8): the compositor told; in the
 * Objectspace session GNOME's cursor-theme too (GTK 3 programs'), GNOME's
 * own kept, and back when the system's are chosen. */
void cursors_set(bool objectspace)
{
    static int was = -1;   /* (the settings are read again on every change: only a change acts) */
    if (was == (int)objectspace) return;
    was = objectspace;
    winlist_ext("cursor-theme", objectspace ? "Objectspace" : "");
    if (off() || !getenv("OSD_SESSION")) return;
    if (gnome_ours("cursor-theme", objectspace ? "Objectspace" : NULL, "Adwaita"))
        fprintf(stderr, "osd-shell: GTK 3 programs' pointers: %s\n", objectspace ? "Objectspace" : "GNOME's own");
}

/* The System's "Other programs' icons" (0.7 step 9): in the Objectspace
 * session GNOME's icon-theme set to Objectspace's (its folders, places,
 * drives and file types; the rest Adwaita's), GNOME's own kept, and back
 * when the system's are chosen. GTK 3 and GTK 4 programs follow at once. */
void othericons_set(bool objectspace)
{
    static int was = -1;
    if (was == (int)objectspace) return;
    was = objectspace;
    if (off() || !getenv("OSD_SESSION")) return;
    if (gnome_ours("icon-theme", objectspace ? "Objectspace" : NULL, "Adwaita"))
        fprintf(stderr, "osd-shell: other programs' icons: %s\n", objectspace ? "Objectspace" : "GNOME's own");
}

/* Started inside GNOME: GNOME's own GTK 3 theme, pointers and icons back
 * if ours are set. */
void gtktheme_gnome_back(void)
{
    if (off() || getenv("OSD_SESSION")) return;
    if (gnome_ours("gtk-theme", NULL, "Adwaita")) fprintf(stderr, "osd-shell: GNOME's GTK 3 theme back\n");
    if (gnome_ours("cursor-theme", NULL, "Adwaita")) fprintf(stderr, "osd-shell: GNOME's pointers back\n");
    if (gnome_ours("icon-theme", NULL, "Adwaita")) fprintf(stderr, "osd-shell: GNOME's icons back\n");
}

/* ------------------------------------------------------------ Qt (0.7 step 9) */

/* "#rrggbb" of a mix of two colours */
static void mixhex(char *out, size_t n, ui_color a, ui_color b)
{
    hex(out, n, (ui_color){ (a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2, 1 });
}

/* qt5ct's and qt6ct's colour scheme for the scheme now: QPalette's roles
 * in its order (WindowText, Button, Light, Midlight, Dark, Mid, Text,
 * BrightText, ButtonText, Base, Window, Shadow, Highlight,
 * HighlightedText, Link, LinkVisited, AlternateBase, NoRole, ToolTipBase,
 * ToolTipText, PlaceholderText, and Qt 6's Accent), active, disabled and
 * inactive. Caller frees. */
char *qtpalette_conf(bool qt6)
{
    const ui_scheme *k = &ui_scheme_now;
    char c[22][10];
    hex(c[0], 10, k->text); hex(c[1], 10, k->face); hex(c[2], 10, k->light); mixhex(c[3], 10, k->face, k->light);
    hex(c[4], 10, k->shadow); mixhex(c[5], 10, k->face, k->shadow); hex(c[6], 10, k->text);
    snprintf(c[7], 10, "#ffffff"); hex(c[8], 10, k->text); hex(c[9], 10, k->client_bg); hex(c[10], 10, k->face);
    hex(c[11], 10, k->dark); hex(c[12], 10, k->selection); hex(c[13], 10, k->selection_text);
    hex(c[14], 10, k->selection); mixhex(c[15], 10, k->selection, k->text); hex(c[16], 10, k->alt_row);
    snprintf(c[17], 10, "#000000"); hex(c[18], 10, k->tooltip_bg); hex(c[19], 10, k->tooltip_text);
    hex(c[20], 10, k->disabled_text); hex(c[21], 10, k->selection);
    int n = qt6 ? 22 : 21;
    char act[400] = "", dis[400] = "";
    for (int i = 0; i < n; i++) {
        bool off_text = i == 0 || i == 6 || i == 8;   /* (disabled: the texts dimmed) */
        char one[16];
        snprintf(one, sizeof one, "%s#ff%s", i ? ", " : "", c[i] + 1);
        strncat(act, one, sizeof act - strlen(act) - 1);
        snprintf(one, sizeof one, "%s#ff%s", i ? ", " : "", (off_text ? c[20] : c[i]) + 1);
        strncat(dis, one, sizeof dis - strlen(dis) - 1);
    }
    char *out = NULL;
    if (asprintf(&out, "# Made by Objectspace from the scheme \"%s\" (made again when it changes).\n[ColorScheme]\n"
                       "active_colors=%s\ndisabled_colors=%s\ninactive_colors=%s\n", k->name ? k->name : "", act, dis, act) < 0)
        return NULL;
    return out;
}

static void config_dir(char *out, size_t n)
{
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xc && *xc) snprintf(out, n, "%s", xc);
    else snprintf(out, n, "%s/.config", home ? home : "");
}

/* tool's (qt5ct, qt6ct) settings: its colour scheme ours (with Fusion, the
 * style a palette shows in), unless the user set one of their own */
static void qt_conf_point(const char *tool, const char *scheme_path)
{
    char cd[4096], path[4300];
    config_dir(cd, sizeof cd);
    snprintf(path, sizeof path, "%s/%s/%s.conf", cd, tool, tool);
    FILE *f = fopen(path, "r");
    char *text = NULL;
    size_t len = 0;
    if (f) {
        char line[1024];
        bool ours = false;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "color_scheme_path=", 18)) ours = strstr(line, "/Objectspace-") != NULL;
            size_t l = strlen(line);
            text = realloc(text, len + l + 1);
            memcpy(text + len, line, l + 1);
            len += l;
        }
        fclose(f);
        bool has = text && strstr(text, "color_scheme_path=");
        if (has && !ours) { free(text); return; }   /* (the user's own) */
        if (has) {   /* ours: the path now */
            char *out = NULL;
            char *at = strstr(text, "color_scheme_path=");
            char *eol = strchr(at, '\n');
            if (asprintf(&out, "%.*scolor_scheme_path=%s\n%s", (int)(at - text), text, scheme_path, eol ? eol + 1 : "") >= 0) write_file(path, out);
            free(out);
            free(text);
            return;
        }
        free(text);   /* (settings without a scheme: written anew below, ours) */
    }
    char dir[4300], *out = NULL;
    snprintf(dir, sizeof dir, "%s/%s", cd, tool);
    mkdirs(dir);
    if (asprintf(&out, "[Appearance]\ncolor_scheme_path=%s\ncustom_palette=true\nstyle=Fusion\n", scheme_path) >= 0) write_file(path, out);
    free(out);
}

static bool qt_tool_installed(const char *qtver, const char *tool)
{
    char p[256];
    static const char *const libs[] = { "/usr/lib64", "/usr/lib", "/usr/lib/x86_64-linux-gnu" };
    for (size_t i = 0; i < sizeof libs / sizeof *libs; i++) {
        snprintf(p, sizeof p, "%s/%s/plugins/platformthemes/lib%s.so", libs[i], qtver, tool);
        if (access(p, F_OK) == 0) return true;
    }
    return false;
}

/* Qt programs in the scheme's colours: qt5ct's and qt6ct's colour scheme
 * for it written; in the Objectspace session their settings pointed at
 * it, and QT_QPA_PLATFORMTHEME for the programs started from here when
 * one of them is installed (qt6ct, else qt5ct). The desktop's. */
void qtpalette_update(void)
{
    const char *e = getenv("OSD_QT_PALETTE");
    if ((e && !strcmp(e, "0")) || !getenv("OSD_SESSION")) return;
    char name[160], cd[4096];
    gtktheme_name(ui_scheme_now.name, name, sizeof name);
    config_dir(cd, sizeof cd);
    for (int v = 5; v <= 6; v++) {
        char tool[8], dir[4300], path[4500];
        snprintf(tool, sizeof tool, "qt%dct", v);
        snprintf(dir, sizeof dir, "%s/%s/colors", cd, tool);
        if (!mkdirs(dir)) continue;
        snprintf(path, sizeof path, "%s/%s.conf", dir, name);
        char *conf = qtpalette_conf(v == 6);
        if (conf && write_file(path, conf)) qt_conf_point(tool, path);
        free(conf);
    }
    const char *want = qt_tool_installed("qt6", "qt6ct") ? "qt6ct" : qt_tool_installed("qt5", "qt5ct") ? "qt5ct" : NULL;
    const char *was = getenv("QT_QPA_PLATFORMTHEME");
    if (want && (!was || strcmp(was, want))) {
        setenv("QT_QPA_PLATFORMTHEME", want, 1);
        fprintf(stderr, "osd-shell: Qt programs from here: %s (the scheme's colours)\n", want);
    }
}
