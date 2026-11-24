/* The window frame: sizing border, title bar, system menu and window
 * buttons, square by default (gui-design.md: Appearance). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

static double bw(void) { return ui_scheme_now.border; }

static ui_frame_geom geom(ui_surface *s)
{
    return (ui_frame_geom){ .w = s->w, .h = s->h, .frameless = s->frameless, .maximized = s->maximized,
                            .activated = s->activated, .pointer_inside = s->pointer_inside, .hover = s->hover,
                            .pressed = s->pressed, .own_title_font = s->own_title_font, .title_font = s->title_font,
                            .own_title_color = s->own_title_color, .title_color = s->title_color };
}

void ui_window_set_title_font(ui_surface *s, bool own, ui_font f)
{
    if (s->own_title_font == own && (!own || s->title_font == f)) return;
    s->own_title_font = own;
    s->title_font = f;
    ui_frame_damage(s);
}

void ui_window_set_title_color(ui_surface *s, const ui_color *c)
{
    bool own = c != NULL;
    if (s->own_title_color == own && (!own || !memcmp(c, &s->title_color, sizeof *c))) return;
    s->own_title_color = own;
    if (own) s->title_color = *c;
    ui_frame_damage(s);
}

ui_rect ui_frame_client(ui_surface *s) { ui_frame_geom g = geom(s); return ui_frame_geom_client(&g); }
ui_hit ui_frame_hit(ui_surface *s, double x, double y) { ui_frame_geom g = geom(s); return ui_frame_geom_hit(&g, x, y); }
static ui_rect title_rect(ui_surface *s)
{
    double b = s->maximized ? 0 : bw();
    return (ui_rect){ b, b, s->w - 2 * b, ui_scheme_now.title_h };
}
static ui_rect button_rect(ui_surface *s, int which) { ui_frame_geom g = geom(s); return ui_frame_geom_button(&g, which); }
void ui_frame_draw(ui_surface *s, cairo_t *cr, ui_rect clip, const char *title)
{
    ui_frame_geom g = geom(s);
    ui_frame_geom_draw(&g, cr, clip, title);
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

static void system_menu_open(ui_surface *s, uint32_t serial);
#define system_menu system_menu_open

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

void ui_frame_system_menu(ui_surface *s, uint32_t serial) { system_menu_open(s, serial); }

static void system_menu_open(ui_surface *s, uint32_t serial)
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
        ui_set_cursor(s->app, h == UI_HIT_BORDER ? edge_cursor(edges_at(s, e->x, e->y)) : h == UI_HIT_CLIENT && s->client_cursor ? s->client_cursor : "default");
        return h != UI_HIT_CLIENT;
    }
    if (e->kind == UI_PRESS && e->button == ui_mouse_button(UI_ACT_SELECT)) {
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
    /* The system menu icon opens its menu with either button (OS/2); the
     * rest of the title bar has no button 2 menu. */
    if (e->kind == UI_PRESS && e->button == ui_mouse_button(UI_ACT_POPUP) && h == UI_HIT_SYSMENU) {
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
