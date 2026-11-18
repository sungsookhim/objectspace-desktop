/* The window frame: sizing border, title bar, system menu and window
 * buttons, square by default (gui-design.md: Appearance). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

static double bw(void) { return ui_scheme_now.border; }

ui_rect ui_frame_client(ui_surface *s)
{
    const ui_scheme *k = &ui_scheme_now;
    double b = s->maximized ? 0 : bw();
    ui_rect r = { b, b + k->title_h, s->w - 2 * b, s->h - 2 * b - k->title_h };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

static ui_rect title_rect(ui_surface *s)
{
    double b = s->maximized ? 0 : bw();
    ui_rect r = { b, b, s->w - 2 * b, ui_scheme_now.title_h };
    return r;
}

/* which: 0 system menu (left), 1 minimize, 2 maximize, 3 close (right). */
static ui_rect button_rect(ui_surface *s, int which)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect t = title_rect(s);
    double y = t.y + (t.h - k->button_h) / 2;
    if (which == 0) return (ui_rect){ t.x + 3, y, k->button_h, k->button_h };
    return (ui_rect){ t.x + t.w - 3 - (4 - which) * (k->button_w + 2), y, k->button_w, k->button_h };
}

static bool buttons_shown(ui_surface *s)
{
    if (!ui_scheme_now.buttons_on_hover) return true;
    return s->pointer_inside && s->hover >= UI_HIT_TITLE && s->hover <= UI_HIT_CLOSE;
}

ui_hit ui_frame_hit(ui_surface *s, double x, double y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return UI_HIT_NONE;
    double b = s->maximized ? 0 : bw();
    if (x < b || y < b || x >= s->w - b || y >= s->h - b) return UI_HIT_BORDER;
    ui_rect t = title_rect(s);
    if (ui_rect_has(t, x, y)) {
        for (int i = 0; i < 4; i++)
            if (ui_rect_has(button_rect(s, i), x, y)) {
                if (i && !buttons_shown(s) && ui_scheme_now.buttons_on_hover) return UI_HIT_TITLE;
                return (ui_hit)(UI_HIT_SYSMENU + i);
            }
        return UI_HIT_TITLE;
    }
    return UI_HIT_CLIENT;
}

static unsigned edges_at(ui_surface *s, double x, double y)
{
    double c = bw() * 4;   /* corners are easier to grab */
    unsigned e = 0;
    if (y < c) e |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (y >= s->h - c) e |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    if (x < c) e |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (x >= s->w - c) e |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    return e;
}

static const char *edge_cursor(unsigned e)
{
    switch (e) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP: return "n-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM: return "s-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT: return "w-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT: return "e-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT: return "nw-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT: return "ne-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT: return "sw-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT: return "se-resize";
    default: return "default";
    }
}

void ui_frame_damage(ui_surface *s)
{
    ui_rect t = title_rect(s);
    ui_surface_damage(s, t);
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

static void draw_button(cairo_t *cr, ui_surface *s, int which)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_rect r = button_rect(s, which);
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

void ui_frame_draw(ui_surface *s, cairo_t *cr, ui_rect clip, const char *title)
{
    const ui_scheme *k = &ui_scheme_now;
    double rad = s->maximized ? 0 : k->corner_radius;
    ui_rect c = ui_frame_client(s);
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
    ui_fill(cr, t, s->activated ? k->title_active : k->title_inactive);
    double left = t.x + 3 + k->button_h + 6;
    double right = buttons_shown(s) ? button_rect(s, 1).x - 6 : t.x + t.w - 6;
    ui_set_color(cr, s->activated ? k->title_text : k->title_text_inactive);
    double tw = ui_text_width(UI_FONT_BOLD, title);
    double tx = left + fmax(0, ((right - left) - tw) / 2);   /* centred, as in Warp 4 */
    ui_text_draw(cr, UI_FONT_BOLD, tx, t.y + (t.h + ui_text_ascent()) / 2 - 1, title, right - tx);
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

static void system_menu_chosen(void *arg, int64_t id)
{
    ui_surface *s = arg;
    switch (id) {
    case 1: ui_window_toggle_maximize(s); break;      /* Restore / Maximize */
    case 3: ui_window_minimize(s); break;
    case 9: if (s->ops->close) s->ops->close(s); break;
    default: break;
    }
}

static void system_menu(ui_surface *s, uint32_t serial)
{
    ui_menu_item items[] = {
        { 1, s->maximized ? "Restore" : "Maximize", true, false, NULL, 0 },
        { 3, "Minimize", true, false, NULL, 0 },
        { 0, "-", false, false, NULL, 0 },
        { 9, "Close", true, false, NULL, 0 },
    };
    ui_rect b = button_rect(s, 0);
    ui_menu_popup(s, items, sizeof items / sizeof items[0], b.x, b.y + b.h, serial, system_menu_chosen, s);
}

bool ui_frame_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    if (e->kind == UI_LEAVE) {
        s->pointer_inside = false;
        if (s->hover != UI_HIT_NONE) { s->hover = UI_HIT_NONE; ui_frame_damage(s); }
        return false;
    }
    s->pointer_inside = true;
    if (e->kind == UI_MOTION || e->kind == UI_ENTER) {
        bool title_before = s->hover >= UI_HIT_TITLE && s->hover <= UI_HIT_CLOSE;
        bool title_now = h >= UI_HIT_TITLE && h <= UI_HIT_CLOSE;
        if (h != s->hover) {
            if (s->pressed || (ui_scheme_now.buttons_on_hover && title_before != title_now)) {
                s->hover = h;
                ui_frame_damage(s);
            }
            s->hover = h;
        }
        ui_set_cursor(s->app, h == UI_HIT_BORDER ? edge_cursor(edges_at(s, e->x, e->y)) : "default");
        return h != UI_HIT_CLIENT;
    }
    if (e->kind == UI_PRESS && e->button == UI_BTN1) {
        switch (h) {
        case UI_HIT_TITLE:
            if (e->clicks == 2) ui_window_toggle_maximize(s);
            else ui_window_move(s, e->serial);
            return true;
        case UI_HIT_BORDER:
            ui_window_resize(s, e->serial, edges_at(s, e->x, e->y));
            return true;
        case UI_HIT_SYSMENU:
            if (e->clicks == 2) { if (s->ops->close) s->ops->close(s); }
            else system_menu(s, e->serial);
            return true;
        case UI_HIT_MIN: case UI_HIT_MAX: case UI_HIT_CLOSE:
            s->pressed = h;
            s->hover = h;
            ui_frame_damage(s);
            return true;
        default:
            return false;
        }
    }
    if (e->kind == UI_PRESS && e->button == UI_BTN2 && (h == UI_HIT_TITLE || h == UI_HIT_SYSMENU)) {
        system_menu(s, e->serial);
        return true;
    }
    if (e->kind == UI_RELEASE && s->pressed) {
        ui_hit p = s->pressed;
        s->pressed = UI_HIT_NONE;
        ui_frame_damage(s);
        if (p == h) {
            if (p == UI_HIT_MIN) ui_window_minimize(s);
            else if (p == UI_HIT_MAX) ui_window_toggle_maximize(s);
            else if (p == UI_HIT_CLOSE && s->ops->close) s->ops->close(s);
        }
        return true;
    }
    return h != UI_HIT_CLIENT && h != UI_HIT_NONE;
}
