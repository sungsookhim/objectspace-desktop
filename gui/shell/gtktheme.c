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
        "spinbutton:not(.vertical) selection, .view text selection:focus {\n"
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

/* Started inside GNOME: GNOME's own GTK 3 theme and pointers back if
 * ours are set. */
void gtktheme_gnome_back(void)
{
    if (off() || getenv("OSD_SESSION")) return;
    if (gnome_ours("gtk-theme", NULL, "Adwaita")) fprintf(stderr, "osd-shell: GNOME's GTK 3 theme back\n");
    if (gnome_ours("cursor-theme", NULL, "Adwaita")) fprintf(stderr, "osd-shell: GNOME's pointers back\n");
}
