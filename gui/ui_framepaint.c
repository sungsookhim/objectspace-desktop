/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* The window frame's geometry and painting, apart from any surface
 * (ui.h: ui_frame_geom): the toolkit's windows draw their own frames with
 * it, and osd-compositor draws other programs' frames with it, so both
 * look the same. Square by default (gui-design.md: Appearance). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

/* Title bars' font (the Font Palette's; the interface font's bold unless set). */
static ui_font title_font = UI_FONT_BOLD;
void ui_frame_set_title_font(ui_font f) { title_font = f; }

static double bw(void) { return ui_scheme_now.border; }

ui_rect ui_frame_geom_client(const ui_frame_geom *s)
{
    const ui_scheme *k = &ui_scheme_now;
    if (s->frameless) return (ui_rect){ 0, 0, s->w, s->h };
    double b = s->maximized ? 0 : bw();
    ui_rect r = { b, b + k->title_h, s->w - 2 * b, s->h - 2 * b - k->title_h };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

static ui_rect title_rect(const ui_frame_geom *s)
{
    double b = s->maximized ? 0 : bw();
    ui_rect r = { b, b, s->w - 2 * b, ui_scheme_now.title_h };
    return r;
}

/* which: 0 system menu (left), 1 minimize, 2 maximize, 3 close (right). */
ui_rect ui_frame_geom_button(const ui_frame_geom *s, int which)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect t = title_rect(s);
    double y = t.y + (t.h - k->button_h) / 2;
    ui_rect r = which == 0 ? (ui_rect){ t.x + 3, y, k->button_h, k->button_h }
                           : (ui_rect){ t.x + t.w - 3 - (4 - which) * (k->button_w + 2), y, k->button_w, k->button_h };
    /* Mirrored layout: the system menu on the right, close first on the left. */
    if (k->title_mirrored) r.x = t.x + t.w - (r.x - t.x) - r.w;
    return r;
}

static bool buttons_shown(const ui_frame_geom *s)
{
    if (!ui_scheme_now.buttons_on_hover) return true;
    return s->pointer_inside && s->hover >= UI_HIT_TITLE && s->hover <= UI_HIT_CLOSE;
}

ui_hit ui_frame_geom_hit(const ui_frame_geom *s, double x, double y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return UI_HIT_NONE;
    if (s->frameless) return UI_HIT_CLIENT;
    double b = s->maximized ? 0 : bw();
    if (x < b || y < b || x >= s->w - b || y >= s->h - b) return UI_HIT_BORDER;
    ui_rect t = title_rect(s);
    if (ui_rect_has(t, x, y)) {
        for (int i = 0; i < 4; i++)
            if (ui_rect_has(ui_frame_geom_button(s, i), x, y)) {
                if (i && !buttons_shown(s) && ui_scheme_now.buttons_on_hover) return UI_HIT_TITLE;
                return (ui_hit)(UI_HIT_SYSMENU + i);
            }
        return UI_HIT_TITLE;
    }
    return UI_HIT_CLIENT;
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void draw_button(cairo_t *cr, const ui_frame_geom *s, int which)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect r = ui_frame_geom_button(s, which);
    bool down = s->pressed == (ui_hit)(UI_HIT_SYSMENU + which) && s->hover == s->pressed;
    ui_fill(cr, r, k->face);
    ui_bevel(cr, r, !down);
    double o = down ? 1 : 0;
    ui_set_color(cr, k->text);
    cairo_set_line_width(cr, 1.5);
    switch (which) {
    case 0:   /* system menu: a small window */
        cairo_rectangle(cr, r.x + 3.5 + o, r.y + 4.5 + o, r.w - 7, r.h - 8);
        cairo_stroke(cr);
        cairo_rectangle(cr, r.x + 3.5 + o, r.y + 4.5 + o, r.w - 7, 2);
        cairo_fill(cr);
        break;
    case 1:   /* minimize: a dot low down */
        cairo_rectangle(cr, r.x + r.w / 2 - 2 + o, r.y + r.h - 6 + o, 4, 3);
        cairo_fill(cr);
        break;
    case 2:   /* maximize or restore */
        if (s->maximized) {
            cairo_rectangle(cr, r.x + 6.5 + o, r.y + 3.5 + o, r.w - 10, r.h - 9);
            cairo_rectangle(cr, r.x + 3.5 + o, r.y + 6.5 + o, r.w - 10, r.h - 9);
        } else {
            cairo_rectangle(cr, r.x + 4.5 + o, r.y + 3.5 + o, r.w - 9, r.h - 7);
        }
        cairo_stroke(cr);
        break;
    case 3:   /* close */
        cairo_move_to(cr, r.x + 5 + o, r.y + 4 + o);
        cairo_line_to(cr, r.x + r.w - 5 + o, r.y + r.h - 4 + o);
        cairo_move_to(cr, r.x + r.w - 5 + o, r.y + 4 + o);
        cairo_line_to(cr, r.x + 5 + o, r.y + r.h - 4 + o);
        cairo_stroke(cr);
        break;
    }
}

void ui_frame_geom_draw(const ui_frame_geom *s, cairo_t *cr, ui_rect clip, const char *title)
{
    const ui_scheme *k = &ui_scheme_now;
    if (s->frameless) return;
    double rad = s->maximized ? 0 : k->corner_radius;
    ui_rect c = ui_frame_geom_client(s);
    /* Only the frame: the client area is the owner's. */
    bool frame_part = clip.x < c.x || clip.y < c.y || clip.x + clip.w > c.x + c.w || clip.y + clip.h > c.y + c.h;
    if (!frame_part) return;
    cairo_save(cr);
    if (rad > 0) {
        /* Rounded corners (a setting, off by default): outside is clear. */
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        rounded(cr, 0, 0, s->w, s->h, rad);
        cairo_clip(cr);
    }
    cairo_rectangle(cr, 0, 0, s->w, s->h);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    ui_set_color(cr, k->face);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    if (!s->maximized) {
        ui_bevel(cr, (ui_rect){ 0, 0, s->w, s->h }, true);
        ui_bevel(cr, (ui_rect){ bw() - 1, bw() - 1, s->w - 2 * bw() + 2, s->h - 2 * bw() + 2 }, false);
    }
    ui_rect t = title_rect(s);
    ui_color tc = !s->activated ? k->title_inactive : s->own_title_color ? s->title_color : k->title_active;
    if (k->translucency == 1) {
        /* Translucent title bar: what is behind the window shows through. */
        tc.a = 0.8;
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        ui_fill(cr, t, tc);
        cairo_restore(cr);
    } else {
        ui_fill(cr, t, tc);
    }
    ui_rect sys = ui_frame_geom_button(s, 0), mn = ui_frame_geom_button(s, 1);
    double left, right;
    if (!k->title_mirrored) {
        left = sys.x + sys.w + 6;
        right = buttons_shown(s) ? mn.x - 6 : t.x + t.w - 6;
    } else {
        left = buttons_shown(s) ? mn.x + mn.w + 6 : t.x + 6;
        right = sys.x - 6;
    }
    ui_set_color(cr, s->activated ? k->title_text : k->title_text_inactive);
    ui_font tf = s->own_title_font ? s->title_font : title_font;
    double tw = ui_text_width(tf, title);
    double tx = left + fmax(0, ((right - left) - tw) / 2);   /* centred, as in Warp 4 */
    ui_text_draw(cr, tf, tx, t.y + (t.h + ui_font_ascent(tf)) / 2 - 1, title, right - tx);
    draw_button(cr, s, 0);
    if (buttons_shown(s))
        for (int i = 1; i < 4; i++) draw_button(cr, s, i);
    if (rad > 0) {
        rounded(cr, 0.5, 0.5, s->w - 1, s->h - 1, rad);
        ui_set_color(cr, k->dark);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

