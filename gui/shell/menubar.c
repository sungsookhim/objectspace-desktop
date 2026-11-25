/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * A folder window's menu bar (0.6 step 3; v0.6-design 2; Warp 4's folders
 * had one): Folder, Edit, View, Selected, Help across the top of the
 * window. On or off per folder (its View page's Menu bar, or all folders
 * through Class...; System's Folders page: all folders), off by default.
 * An Alt tap or F10 puts the keyboard on it (shown for a moment when it is
 * off): Left and Right move along its titles, Down, Enter or a title's
 * first letter opens one; in an open menu, Left and Right at its top
 * open the neighbouring menu; Escape, or Alt or F10 again, leave it.
 * Folder and Selected are the
 * folder's and the selection's own pop-up menus (menus.c); Edit, View and
 * Help are its own, their actions those of the keys and pop-up menus.
 */
#define _GNU_SOURCE
#include "shell.h"

#include <math.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define NMENUS 5
static const char *const titles[NMENUS] = { "Folder", "Edit", "View", "Selected", "Help" };

double menubar_h(void) { return ceil(ui_font_line_h(UI_FONT_REGULAR)) + 8; }

bool menubar_shown(const struct folder *f) { return f && !f->desktop && !f->col_owner && (f->menubar_on || f->menubar_temp); }

ui_rect menubar_rect(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    return (ui_rect){ c.x, c.y, c.w, menubar_h() };
}

/* Menu i's title on the bar. */
static ui_rect title_rect(struct folder *f, int i)
{
    ui_rect c = ui_frame_client(f->win);
    double x = c.x + 4;
    for (int k = 0; k < i; k++) x += ui_text_width(UI_FONT_REGULAR, titles[k]) + 16;
    return (ui_rect){ x, c.y + 2, ui_text_width(UI_FONT_REGULAR, titles[i]) + 16, menubar_h() - 4 };
}

static bool any_selected(struct folder *f) { return ui_container_selected_count(f->cont) > 0; }

void menubar_draw(struct folder *f, cairo_t *cr)
{
    if (!menubar_shown(f)) return;
    const ui_scheme *k = &ui_scheme_now;
    ui_rect c = ui_frame_client(f->win), bar = { c.x, c.y, c.w, menubar_h() };
    ui_fill(cr, bar, k->face);
    ui_fill(cr, (ui_rect){ bar.x, bar.y + bar.h - 1, bar.w, 1 }, k->shadow);
    for (int i = 0; i < NMENUS; i++) {
        ui_rect r = title_rect(f, i);
        bool open = f->menubar_open == i || (f->menubar_open < 0 && f->menubar_kbd == i);
        if (open) ui_fill(cr, r, k->selection);
        bool enabled = i != 3 || any_selected(f);
        ui_set_color(cr, open ? k->selection_text : enabled ? k->text : k->shadow);
        ui_text_draw(cr, UI_FONT_REGULAR, r.x + 8, r.y + (r.h + ui_text_ascent()) / 2 - 1, titles[i], r.w - 8);
    }
}

static void bar_damage(struct folder *f)
{
    ui_rect c = ui_frame_client(f->win);
    ui_surface_damage(f->win, (ui_rect){ c.x, c.y, c.w, menubar_h() });
}

/* The bar shown for a moment goes again (a menu closed, Escape). */
static void temp_off(struct folder *f)
{
    f->menubar_open = -1;
    f->menubar_kbd = -1;
    if (f->menubar_temp) {
        f->menubar_temp = false;
        layout(f);
        ui_surface_damage_all(f->win);
    } else bar_damage(f);
}

/* ---- Edit, View, Help: their items (ids above the pop-up menus' own) */

enum { MB_SELECT_ALL = 9201, MB_DESELECT, MB_RENAME, MB_DELETE, MB_REFRESH, MB_SEARCH, MB_MENUBAR, MB_KEYS, MB_ABOUT };

struct mb_ctx { long folder; struct menu_ctx *m; };

static void key_to(struct folder *f, uint32_t sym, unsigned mods)
{
    ui_key_event e = { .sym = sym, .mods = mods };
    win_key(f->win, &e);
}

static void about(struct folder *f)
{
    (void)f;
    static const char *const b[] = { "OK" };
    ui_dialog_open(app, "About Objectspace",
                   "Objectspace " OSD_VERSION "\n\nAn object desktop for Linux, inspired by the Workplace Shell of OS/2 "
                   "(OS/2 and Workplace Shell are IBM's trademarks; Objectspace is not affiliated with IBM).\n\n"
                   "MIT License. https://github.com/sungsookhim/objectspace-desktop", b, 1, 0, NULL, NULL);
}

static void keys_help(struct folder *f)
{
    (void)f;
    static const char *const b[] = { "OK" };
    ui_dialog_open(app, "Keys in a Folder",
                   "Enter: open   Delete: delete (Shift: for good)   F2: rename\n"
                   "Ctrl+A: select all   Ctrl+1 to 5: icon, tree, details, column, gallery view\n"
                   "Ctrl+F: search this folder   Ctrl+Shift+F: Find\n"
                   "Alt or F10: the menu bar   Ctrl+W: close   Typing: jump to a name", b, 1, 0, NULL, NULL);
}

static void own_chosen(void *arg, int64_t id)
{
    struct mb_ctx *x = arg;
    struct folder *f = folder_by_id(x->folder);
    if (!f) { if (x->m) menu_chosen(x->m, -1); free(x); return; }
    struct menu_ctx *m = x->m;
    free(x);
    bool own = id >= MB_SELECT_ALL && id <= MB_ABOUT;
    if (m) menu_chosen(m, own ? -1 : id);   /* the pop-up menu's own (views, sort, arrange): menus.c acts; it frees m (and closes) */
    else menubar_closed(f);
    if (!own) return;
    switch (id) {
    case MB_SELECT_ALL: key_to(f, XKB_KEY_a, UI_MOD_CTRL); break;
    case MB_DESELECT: ui_container_select_none(f->cont); damage_status(f); break;
    case MB_RENAME: key_to(f, XKB_KEY_F2, 0); break;
    case MB_DELETE: key_to(f, XKB_KEY_Delete, 0); break;
    case MB_REFRESH: reload(f); break;
    case MB_SEARCH: key_to(f, XKB_KEY_f, UI_MOD_CTRL); break;
    case MB_MENUBAR: {   /* kept as the folder's own setting (its View page) */
        cv v = { .k = CV_BOOL, .b = !f->menubar_on };
        svc_settings_set(f->path, "menu_bar", &v, -1, ignore_reply, NULL);
        f->menubar_on = !f->menubar_on;
        layout(f);
        ui_surface_damage_all(f->win);
        break;
    }
    case MB_KEYS: keys_help(f); break;
    case MB_ABOUT: about(f); break;
    }
}

static void open_own(struct folder *f, int i, double x, double y)
{
    struct mb_ctx *c = calloc(1, sizeof *c);
    c->folder = f->id;
    ui_menu_item items[16];
    size_t n = 0;
    bool sel = any_selected(f);
    if (i == 1) {
        items[n++] = (ui_menu_item){ MB_SELECT_ALL, "Select all\tCtrl+A", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ MB_DESELECT, "Deselect all", sel, false, NULL, 0 };
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ MB_RENAME, "Rename\tF2", sel || ui_container_cursor(f->cont) != SIZE_MAX, false, NULL, 0 };
        items[n++] = (ui_menu_item){ MB_DELETE, "Delete...\tDel", sel, false, NULL, 0 };
    } else if (i == 2) {
        /* the views and the pop-up menu's sort and arrange (menus.c acts on them) */
        c->m = calloc(1, sizeof *c->m);
        c->m->folder = f->id;
        c->m->item = SIZE_MAX;
        c->m->x = x;
        c->m->y = y;
        ui_view v = ui_container_view(f->cont);
        bool icons = v == UI_VIEW_ICON;
        static ui_menu_item sorts[4];
        sorts[0] = (ui_menu_item){ OSD_MI_SORT_NAME, "By name", icons, false, NULL, 0 };
        sorts[1] = (ui_menu_item){ OSD_MI_SORT_TYPE, "By type", icons, false, NULL, 0 };
        sorts[2] = (ui_menu_item){ OSD_MI_SORT_SIZE, "By size", icons, false, NULL, 0 };
        sorts[3] = (ui_menu_item){ OSD_MI_SORT_DATE, "By last write", icons, false, NULL, 0 };
        items[n++] = (ui_menu_item){ OSD_MI_VIEW_ICON, "Icon view\tCtrl+1", true, v == UI_VIEW_ICON && !f->gallery, NULL, 0 };
        items[n++] = (ui_menu_item){ OSD_MI_VIEW_TREE, "Tree view\tCtrl+2", true, v == UI_VIEW_TREE, NULL, 0 };
        items[n++] = (ui_menu_item){ OSD_MI_VIEW_DETAILS, "Details view\tCtrl+3", true, v == UI_VIEW_DETAILS, NULL, 0 };
        items[n++] = (ui_menu_item){ OSD_MI_VIEW_COLUMN, "Column view\tCtrl+4", true, v == UI_VIEW_LIST, NULL, 0 };
        items[n++] = (ui_menu_item){ OSD_MI_VIEW_GALLERY, "Gallery view\tCtrl+5", true, f->gallery, NULL, 0 };
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ OSD_MI_SORT, "Sort", icons, false, sorts, 4 };
        items[n++] = (ui_menu_item){ OSD_MI_ARRANGE, "Arrange", icons, false, NULL, 0 };
        items[n++] = (ui_menu_item){ MB_REFRESH, "Refresh", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ MB_SEARCH, "Search field\tCtrl+F", !is_tree(f), f->find_on, NULL, 0 };
        items[n++] = (ui_menu_item){ MB_MENUBAR, "Menu bar", true, f->menubar_on, NULL, 0 };
    } else {
        items[n++] = (ui_menu_item){ MB_KEYS, "Keys in a folder", true, false, NULL, 0 };
        items[n++] = (ui_menu_item){ -1, "-", false, false, NULL, 0 };
        items[n++] = (ui_menu_item){ MB_ABOUT, "About Objectspace", true, false, NULL, 0 };
    }
    ui_menu_next_sideways(menubar_side, (void *)(intptr_t)f->id, menubar_rect(f));
    ui_menu_popup(f->win, items, n, x, y, 0, own_chosen, c);
}

/* Menu i, under its title: Folder and Selected are the pop-up menus. */
void menubar_open(struct folder *f, int i)
{
    if (!menubar_shown(f) || i < 0 || i >= NMENUS) return;
    if (i == 3 && !any_selected(f)) return;
    ui_rect r = title_rect(f, i);
    f->menubar_open = f->menubar_last = i;
    if (f->menubar_kbd >= 0) f->menubar_kbd = i;
    bar_damage(f);
    tooltip_hide();
    double x = r.x, y = r.y + r.h + 2;
    if (i == 0) src_menu_bar(f, SIZE_MAX, x, y);
    else if (i == 3) {
        size_t total = is_tree(f) ? f->ntvis : f->m.total, at = SIZE_MAX;
        for (size_t k = 0; k < total && at == SIZE_MAX; k++) if (ui_container_is_selected(f->cont, k)) at = k;
        src_menu_bar(f, at, x, y);
    } else open_own(f, i, x, y);
}

/* The title `dir` along from i (Selected passed over with nothing selected). */
static int along(struct folder *f, int i, int dir)
{
    for (int k = 0; k < NMENUS; k++) {
        i = (i + dir + NMENUS) % NMENUS;
        if (i != 3 || any_selected(f)) return i;
    }
    return 0;
}

/* Shown again for a moment if a menu's closing took it away (switching menus). */
static void show_for_keys(struct folder *f, int at)
{
    if (!f->menubar_on && !f->menubar_temp) {
        f->menubar_temp = true;
        layout(f);
        ui_surface_damage_all(f->win);
    }
    f->menubar_kbd = at;
    bar_damage(f);
}

void menubar_side(void *ctx, int dir)
{
    struct folder *f = folder_by_id((long)(intptr_t)ctx);
    if (!f || f->menubar_last < 0) return;
    int next = along(f, f->menubar_last, dir);
    show_for_keys(f, next);
    menubar_open(f, next);
}

/* A press on the bar: a menu's title opens it. True when the bar took it. */
bool menubar_pointer(struct folder *f, const ui_pointer_event *e)
{
    if (!menubar_shown(f)) return false;
    ui_rect c = ui_frame_client(f->win);
    if (e->y < c.y || e->y >= c.y + menubar_h() || e->x < c.x || e->x >= c.x + c.w) {
        if (e->kind == UI_PRESS && f->menubar_temp && !ui_menu_is_open()) temp_off(f);   /* (a click elsewhere: it goes) */
        return false;
    }
    if (e->kind == UI_MOTION && f->menubar_open >= 0 && ui_menu_is_open()) {   /* moved along the bar with a menu open: that one's */
        for (int i = 0; i < NMENUS; i++)
            if (i != f->menubar_open && ui_rect_has(title_rect(f, i), e->x, e->y) && (i != 3 || any_selected(f))) { menubar_open(f, i); break; }
        return true;
    }
    if (e->kind != UI_PRESS) return true;
    for (int i = 0; i < NMENUS; i++)
        if (ui_rect_has(title_rect(f, i), e->x, e->y)) {
            if (i == f->menubar_open && ui_menu_is_open()) return true;   /* its menu, open already: it stays */
            menubar_open(f, i);
            return true;
        }
    if (ui_menu_is_open()) ui_menu_close_all();   /* the bar, between titles: the menu closes (the bar with it, if shown for a moment) */
    return true;
}

/* Alt tapped or F10: the keyboard on the bar (shown for a moment when it
 * is off), on its first title, no menu open yet; again: away. With the
 * keyboard on it: Left, Right; Down, Enter, Space or a title's first
 * letter opens one; Escape leaves; any other key leaves and goes on to
 * the folder. */
bool menubar_key(struct folder *f, const ui_key_event *e)
{
    if (!f || f->desktop) return false;
    bool toggle = e->alt_tap || (e->sym == XKB_KEY_F10 && !(e->mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SHIFT)));
    if (f->menubar_kbd < 0) {
        if (!toggle) return false;
        show_for_keys(f, 0);
        return true;
    }
    if (toggle || e->sym == XKB_KEY_Escape) { temp_off(f); return true; }
    switch (e->sym) {
    case XKB_KEY_Left: f->menubar_kbd = along(f, f->menubar_kbd, -1); bar_damage(f); return true;
    case XKB_KEY_Right: case XKB_KEY_Tab: f->menubar_kbd = along(f, f->menubar_kbd, 1); bar_damage(f); return true;
    case XKB_KEY_Down: case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space: menubar_open(f, f->menubar_kbd); return true;
    default: break;
    }
    if (e->utf8[0] && !e->utf8[1] && !(e->mods & (UI_MOD_CTRL | UI_MOD_ALT)))
        for (int i = 0; i < NMENUS; i++)
            if ((titles[i][0] | 0x20) == (e->utf8[0] | 0x20) && (i != 3 || any_selected(f))) { menubar_open(f, i); return true; }
    if (e->sym == XKB_KEY_Shift_L || e->sym == XKB_KEY_Shift_R || e->sym == XKB_KEY_Control_L || e->sym == XKB_KEY_Control_R ||
        e->sym == XKB_KEY_Alt_L || e->sym == XKB_KEY_Alt_R)
        return true;   /* (a modifier on its own: stays) */
    temp_off(f);   /* any other key: away, and on to the folder */
    return false;
}

/* A menu opened from the bar closed (menus.c, after a choice or none). */
void menubar_closed(struct folder *f)
{
    if (!f || f->menubar_open < 0) return;
    if (ui_menu_is_open()) return;   /* (another of the bar's menus opened meanwhile: the bar stays) */
    temp_off(f);
}
