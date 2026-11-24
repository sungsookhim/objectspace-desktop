/* Menus' looks and geometry, shared by the toolkit's pop-up menus
 * (ui_menu.c) and the compositor's system menu: one menu, drawn one way,
 * in the menu font. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define SEP_H 7
#define PAD 3

/* All menus' font (the Font Palette's; the interface font unless set). */
static ui_font menu_font = UI_FONT_REGULAR;
void ui_menu_set_font(ui_font f) { menu_font = f; }
ui_font ui_menu_font(void) { return menu_font; }

static bool is_sep(const ui_menu_item *it) { return it->label && !strcmp(it->label, "-"); }

double ui_menu_look_item_h(const ui_menu_look *l)
{
    double ic = l->icon_small + 6;   /* (room for the icon set's small icon) */
    if (ui_font_is_interface(menu_font)) return fmax(22, ic);   /* (as always) */
    double h = ceil(ui_font_line_h(menu_font)) + 6;
    return fmax(fmax(h, 22), ic);
}

double ui_menu_look_item_y(const ui_menu_look *l, size_t i)
{
    double y = PAD, ih = ui_menu_look_item_h(l);
    for (size_t k = 0; k < i && k < l->n; k++) y += is_sep(&l->items[k]) ? SEP_H : ih;
    return y;
}

int ui_menu_look_item_at(const ui_menu_look *l, double w, double x, double y)
{
    if (x < 0 || x >= w) return -1;
    double top = PAD, ih = ui_menu_look_item_h(l);
    for (size_t i = 0; i < l->n; i++) {
        double h = is_sep(&l->items[i]) ? SEP_H : ih;
        if (y >= top && y < top + h) return is_sep(&l->items[i]) ? -1 : (int)i;
        top += h;
    }
    return -1;
}

void ui_menu_look_size(const ui_menu_look *l, double *w, double *h)
{
    double mw = 120;
    for (size_t i = 0; i < l->n; i++)
        if (!is_sep(&l->items[i])) {
            char label[256];
            snprintf(label, sizeof label, "%s", l->items[i].label);
            char *key = strchr(label, '\t');
            if (key) *key++ = 0;
            double tw = ui_text_width(menu_font, label) + 22 + 30 + (key ? ui_text_width(menu_font, key) + 24 : 0) + l->room;
            if (tw > mw) mw = tw;
        }
    *w = mw;
    *h = ui_menu_look_item_y(l, l->n) + PAD;
}

void ui_menu_look_draw(cairo_t *cr, const ui_menu_look *l, double w, double h, int hot_i)
{
    const ui_scheme *k = &ui_scheme_now;
    double ih = ui_menu_look_item_h(l);
    if (k->translucency == 2) {
        /* Translucent menus: what is behind them shows through. */
        ui_color bg = k->menu_bg;
        bg.a = 0.88;
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        ui_fill(cr, (ui_rect){ 0, 0, w, h }, bg);
        cairo_restore(cr);
    } else {
        ui_fill(cr, (ui_rect){ 0, 0, w, h }, k->menu_bg);
    }
    ui_bevel(cr, (ui_rect){ 0, 0, w, h }, true);
    for (size_t i = 0; i < l->n; i++) {
        const ui_menu_item *it = &l->items[i];
        double y = ui_menu_look_item_y(l, i);
        if (is_sep(it)) {
            ui_set_color(cr, k->shadow);
            cairo_rectangle(cr, 4, y + 3, w - 8, 1);
            cairo_fill(cr);
            ui_set_color(cr, k->light);
            cairo_rectangle(cr, 4, y + 4, w - 8, 1);
            cairo_fill(cr);
            continue;
        }
        bool hot = (int)i == hot_i && it->enabled;
        if (hot) ui_fill(cr, (ui_rect){ 2, y, w - 4, ih }, k->menu_hilite);
        ui_color tc = !it->enabled ? k->disabled_text : hot ? k->menu_hilite_text : k->menu_text;
        ui_set_color(cr, tc);
        if (it->checked) {
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, 6, y + 11);
            cairo_line_to(cr, 9, y + 15);
            cairo_line_to(cr, 15, y + 6);
            cairo_stroke(cr);
        }
        if (l->room && l->icon) {
            double is = l->icon_small;
            if (l->icon(l->arg, it, cr, 20, y + floor((ih - is) / 2), is)) ui_set_color(cr, tc);
        }
        char label[256];
        snprintf(label, sizeof label, "%s", it->label);
        char *key = strchr(label, '\t');   /* "Text\tShortcut": shortcut right-aligned */
        if (key) *key++ = 0;
        double base = y + (ih + ui_font_ascent(menu_font)) / 2 - 1;
        ui_text_draw(cr, menu_font, 22 + l->room, base, label, w - 44 - l->room);
        if (key) ui_text_draw(cr, menu_font, w - 22 - ui_text_width(menu_font, key), base, key, 0);
        if (it->nitems) {
            cairo_move_to(cr, w - 12, y + 7);
            cairo_line_to(cr, w - 8, y + 11);
            cairo_line_to(cr, w - 12, y + 15);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
}
