/* The Colour Palette (colorpal.h). */
#define _GNU_SOURCE
#include "colorpal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "events.h"

#define COLS 6
#define CELL_W 52
#define CELL_H 40
#define M 8
#define BTN_W (110 * ui_dialog_wider())
#define ROW (24 + ui_dialog_grow())
#define STATUS_H (20 + ui_dialog_grow())
#define MAX_SAMPLES 30

static ui_app *app;
static colorpal_apply_fn apply;

static char samples[MAX_SAMPLES][8];
static size_t nsamples;

static struct pal {
    ui_surface *s;
    size_t sel;
    bool closing;
    /* a press on a sample: a drag once it moves, a menu if released (button 2) */
    bool armed, popup_armed;
    size_t press_i;
    double px, py;
    uint32_t serial;
    char drag[8];
    bool edit_down;
} pal;

static bool hex_ok(const char *t)
{
    double rgb[3];
    return t && t[0] == '#' && strlen(t) == 7 && ui_parse_color(t, rgb);
}

/* ---------------------------------------------------------- the samples */

void colorpal_set_samples(const char *list)
{
    nsamples = 0;
    char copy[1024];
    snprintf(copy, sizeof copy, "%s", list ? list : "");
    for (char *q = copy, *t; nsamples < MAX_SAMPLES && (t = strsep(&q, ";"));) {
        while (*t == ' ') t++;
        if (!hex_ok(t)) continue;
        snprintf(samples[nsamples], sizeof samples[0], "%s", t);
        for (char *c = samples[nsamples]; *c; c++) if (*c >= 'A' && *c <= 'F') *c = (char)(*c - 'A' + 'a');
        nsamples++;
    }
    if (pal.sel >= nsamples) pal.sel = 0;
    if (pal.s && !pal.closing) ui_surface_damage_all(pal.s);
}

static void save_samples(void)
{
    char list[1024] = "";
    for (size_t i = 0; i < nsamples; i++)
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ";" : "", samples[i]);
    if (apply) apply("samples", list);
}

static void set_rgb(cairo_t *cr, const char *hex)
{
    double rgb[3] = { 0, 0, 0 };
    ui_parse_color(hex, rgb);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
}

/* ------------------------------------------------------------ small parts */

static void ring(cairo_t *cr, ui_rect r)
{
    ui_set_color(cr, ui_scheme_now.cursor);
    double dash = 1;
    cairo_save(cr);
    cairo_set_dash(cr, &dash, 1, 0);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, r.x + 2.5, r.y + 2.5, r.w - 5, r.h - 5);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void button(cairo_t *cr, ui_rect r, const char *label, bool down, bool focus)
{
    const ui_scheme *k = &ui_scheme_now;
    ui_fill(cr, r, k->face);
    ui_bevel(cr, r, !down);
    ui_set_color(cr, k->text);
    double tw = ui_text_width(UI_FONT_DIALOG, label);
    ui_text_draw(cr, UI_FONT_DIALOG, r.x + (r.w - tw) / 2 + down, r.y + (r.h + ui_font_ascent(UI_FONT_DIALOG)) / 2 - 1 + down, label, r.w - 6);
    if (focus) ring(cr, r);
}

/* ---------------------------------------------------------- hue, saturation, value */

static void hsv_rgb(double h, double s, double v, double rgb[3])
{
    h = fmod(h, 360);
    if (h < 0) h += 360;
    double c = v * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else { r = c; b = x; }
    rgb[0] = r + m; rgb[1] = g + m; rgb[2] = b + m;
}

static void rgb_hsv(const double rgb[3], double *h, double *s, double *v)
{
    double mx = fmax(rgb[0], fmax(rgb[1], rgb[2])), mn = fmin(rgb[0], fmin(rgb[1], rgb[2])), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) return;   /* (a grey: the hue stays as it was) */
    double hh;
    if (mx == rgb[0]) hh = fmod((rgb[1] - rgb[2]) / d, 6);
    else if (mx == rgb[1]) hh = (rgb[2] - rgb[0]) / d + 2;
    else hh = (rgb[0] - rgb[1]) / d + 4;
    *h = hh * 60;
    if (*h < 0) *h += 360;
}

/* ------------------------------------------------------- the Edit Colour dialog */

#define EC_W 430
#define SQ_W 240
#define SQ_H 180
#define BR_W 20
static struct ec {
    ui_surface *s;
    size_t sample;
    char old[8];
    double h, sat, v;
    ui_entry *hex;
    int focus;                 /* 0 the square, 1 brightness, 2 #rrggbb, 3 OK, 4 Cancel */
    int pressed;               /* a button held: 3 or 4 */
    int dragging;              /* 1 in the square, 2 on the brightness, 0 none */
    bool closing;
    cairo_surface_t *sq;       /* the square at full brightness (made once) */
    char shown[8];
} ec;

struct ec_rects { ui_rect sq, br, preview, hex_l, hex, ok, cancel; };
static struct ec_rects ec_rects(void)
{
    ui_rect c = ui_frame_client(ec.s);
    struct ec_rects r;
    r.sq = (ui_rect){ c.x + M, c.y + M, SQ_W, SQ_H };
    r.br = (ui_rect){ r.sq.x + SQ_W + 10, c.y + M, BR_W, SQ_H };
    double rx = r.br.x + BR_W + 14, rw = c.x + c.w - M - rx;
    r.preview = (ui_rect){ rx, c.y + M, rw, 80 };
    r.hex_l = (ui_rect){ rx, c.y + M + 92, rw, ROW };
    r.hex = (ui_rect){ rx, c.y + M + 92 + ROW, rw, ROW };
    r.ok = (ui_rect){ c.x + c.w - M - 2 * 90 - 8, c.y + c.h - M - ROW, 90, ROW };
    r.cancel = (ui_rect){ c.x + c.w - M - 90, c.y + c.h - M - ROW, 90, ROW };
    return r;
}

static void ec_hex_now(char out[8])
{
    double rgb[3];
    hsv_rgb(ec.h, ec.sat, ec.v, rgb);
    snprintf(out, 8, "#%02x%02x%02x", (int)lround(rgb[0] * 255), (int)lround(rgb[1] * 255), (int)lround(rgb[2] * 255));
}

/* The #rrggbb field follows the square and the brightness. */
static void ec_sync_hex(void)
{
    ec_hex_now(ec.shown);
    if (ec.focus != 2 || strcmp(ui_entry_text(ec.hex), ec.shown)) ui_entry_set_text(ec.hex, ec.shown);
}

static void ec_from_hex(const char *t)
{
    double rgb[3];
    char h[8];
    snprintf(h, sizeof h, "%s%s", t[0] == '#' ? "" : "#", t);
    if (!hex_ok(h)) return;
    ui_parse_color(h, rgb);
    rgb_hsv(rgb, &ec.h, &ec.sat, &ec.v);
    snprintf(ec.shown, sizeof ec.shown, "%s", h);
}

static void make_square(void)
{
    if (ec.sq) return;
    ec.sq = cairo_image_surface_create(CAIRO_FORMAT_RGB24, SQ_W, SQ_H);
    unsigned char *d = cairo_image_surface_get_data(ec.sq);
    int st = cairo_image_surface_get_stride(ec.sq);
    cairo_surface_flush(ec.sq);
    for (int y = 0; y < SQ_H; y++)
        for (int x = 0; x < SQ_W; x++) {
            double rgb[3];
            hsv_rgb(360.0 * x / SQ_W, 1 - (double)y / (SQ_H - 1), 1, rgb);
            uint32_t px = (uint32_t)lround(rgb[0] * 255) << 16 | (uint32_t)lround(rgb[1] * 255) << 8 | (uint32_t)lround(rgb[2] * 255);
            memcpy(d + y * st + x * 4, &px, 4);
        }
    cairo_surface_mark_dirty(ec.sq);
}

static void ec_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s;
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(ec.s, cr, clip, "Edit Colour");
    ui_rect c = ui_frame_client(ec.s);
    ui_fill(cr, c, k->face);
    struct ec_rects r = ec_rects();
    /* the hue (across) and saturation (down) square; its mark */
    make_square();
    cairo_save(cr);
    cairo_set_source_surface(cr, ec.sq, r.sq.x, r.sq.y);
    cairo_paint(cr);
    cairo_restore(cr);
    ui_bevel(cr, (ui_rect){ r.sq.x - 2, r.sq.y - 2, r.sq.w + 4, r.sq.h + 4 }, false);
    double mx = r.sq.x + ec.h / 360 * SQ_W, my = r.sq.y + (1 - ec.sat) * (SQ_H - 1);
    cairo_save(cr);
    cairo_rectangle(cr, r.sq.x, r.sq.y, r.sq.w, r.sq.h);
    cairo_clip(cr);
    cairo_set_line_width(cr, 3);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_arc(cr, mx, my, 6, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_arc(cr, mx, my, 6, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_restore(cr);
    if (ec.focus == 0) ring(cr, (ui_rect){ r.sq.x - 5, r.sq.y - 5, r.sq.w + 10, r.sq.h + 10 });
    /* the brightness: this hue and saturation, light at the top */
    double top[3];
    hsv_rgb(ec.h, ec.sat, 1, top);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, r.br.y, 0, r.br.y + r.br.h);
    cairo_pattern_add_color_stop_rgb(g, 0, top[0], top[1], top[2]);
    cairo_pattern_add_color_stop_rgb(g, 1, 0, 0, 0);
    cairo_set_source(cr, g);
    cairo_rectangle(cr, r.br.x, r.br.y, r.br.w, r.br.h);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    ui_bevel(cr, (ui_rect){ r.br.x - 2, r.br.y - 2, r.br.w + 4, r.br.h + 4 }, false);
    double by = r.br.y + (1 - ec.v) * r.br.h;
    ui_set_color(cr, k->text);
    cairo_move_to(cr, r.br.x + r.br.w + 2, by);
    cairo_line_to(cr, r.br.x + r.br.w + 9, by - 5);
    cairo_line_to(cr, r.br.x + r.br.w + 9, by + 5);
    cairo_close_path(cr);
    cairo_fill(cr);
    if (ec.focus == 1) ring(cr, (ui_rect){ r.br.x - 5, r.br.y - 5, r.br.w + 10, r.br.h + 10 });
    /* the new colour over the old one */
    char now[8];
    ec_hex_now(now);
    ui_rect top_h = { r.preview.x, r.preview.y, r.preview.w, r.preview.h / 2 }, bot = { r.preview.x, r.preview.y + r.preview.h / 2, r.preview.w, r.preview.h / 2 };
    set_rgb(cr, now);
    cairo_rectangle(cr, top_h.x, top_h.y, top_h.w, top_h.h);
    cairo_fill(cr);
    set_rgb(cr, ec.old);
    cairo_rectangle(cr, bot.x, bot.y, bot.w, bot.h);
    cairo_fill(cr);
    ui_bevel(cr, r.preview, false);
    ui_set_color(cr, k->text);
    ui_text_draw(cr, UI_FONT_DIALOG, r.hex_l.x, r.hex_l.y + (ROW + ui_font_ascent(UI_FONT_DIALOG)) / 2 - 1, "Colour (#rrggbb)", r.hex_l.w);
    ui_entry_draw(ec.hex, cr, r.hex, ec.focus == 2);
    button(cr, r.ok, "OK", ec.pressed == 3, ec.focus == 3);
    button(cr, r.cancel, "Cancel", ec.pressed == 4, ec.focus == 4);
}

static void ec_set_focus(int f)
{
    ec.focus = f;
    ui_text_input_focus(ec.s, f == 2 ? ec.hex : NULL, ec_rects().hex);
    if (f == 2) ui_entry_select_all(ec.hex);
    ui_surface_damage_all(ec.s);
}

static void ec_close(void);

static void ec_ok(void)
{
    if (!ec.s || ec.closing || ec.sample >= nsamples) return;
    if (ec.focus == 2) ec_from_hex(ui_entry_text(ec.hex));
    ec_hex_now(samples[ec.sample]);
    save_samples();
    if (pal.s && !pal.closing) ui_surface_damage_all(pal.s);
    ec_close();
}

static void ec_track(double x, double y)
{
    struct ec_rects r = ec_rects();
    if (ec.dragging == 1) {
        ec.h = fmin(359.9, fmax(0, (x - r.sq.x) / SQ_W * 360));
        ec.sat = fmin(1, fmax(0, 1 - (y - r.sq.y) / (SQ_H - 1)));
        if (ec.v < 0.05) ec.v = 1;   /* (picked from black: its colour seen) */
    } else if (ec.dragging == 2) {
        ec.v = fmin(1, fmax(0, 1 - (y - r.br.y) / r.br.h));
    }
    ec_sync_hex();
    ui_surface_damage_all(ec.s);
}

static void ec_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    bool used = false;
    if (e->kind != UI_PRESS || h != UI_HIT_CLIENT) used = ui_frame_pointer(s, e);
    struct ec_rects r = ec_rects();
    if (e->kind == UI_PRESS && !used && h == UI_HIT_CLIENT) {
        if (e->button != UI_BTN1) return;
        ui_rect sq = { r.sq.x - 4, r.sq.y - 4, r.sq.w + 8, r.sq.h + 8 }, br = { r.br.x - 4, r.br.y - 4, r.br.w + 14, r.br.h + 8 };
        if (ui_rect_has(sq, e->x, e->y)) { ec.dragging = 1; ec_set_focus(0); ec_track(e->x, e->y); return; }
        if (ui_rect_has(br, e->x, e->y)) { ec.dragging = 2; ec_set_focus(1); ec_track(e->x, e->y); return; }
        if (ui_rect_has(r.hex, e->x, e->y)) { ec_set_focus(2); ui_entry_pointer(ec.hex, e, r.hex); return; }
        if (ui_rect_has(r.ok, e->x, e->y)) { ec.pressed = 3; ec_set_focus(3); return; }
        if (ui_rect_has(r.cancel, e->x, e->y)) { ec.pressed = 4; ec_set_focus(4); return; }
        return;
    }
    if (e->kind == UI_MOTION && ec.dragging) { ec_track(e->x, e->y); return; }
    if (e->kind == UI_RELEASE && ec.dragging) { ec.dragging = 0; return; }
    if (e->kind == UI_RELEASE && ec.pressed) {
        int p = ec.pressed;
        ec.pressed = 0;
        ui_surface_damage_all(s);
        if (p == 3 && ui_rect_has(r.ok, e->x, e->y)) ec_ok();
        else if (p == 4 && ui_rect_has(r.cancel, e->x, e->y)) ec_close();
        return;
    }
    if (ec.focus == 2 && (e->kind == UI_MOTION || e->kind == UI_RELEASE) && ui_entry_pointer(ec.hex, e, r.hex) != UI_ENTRY_NONE)
        ui_surface_damage_all(s);
}

static void ec_key(ui_surface *s, const ui_key_event *e)
{
    if (e->sym == XKB_KEY_Escape) { ec_close(); return; }
    if (e->sym == XKB_KEY_Tab || e->sym == XKB_KEY_ISO_Left_Tab) {
        bool back = e->sym == XKB_KEY_ISO_Left_Tab || (e->mods & UI_MOD_SHIFT);
        if (ec.focus == 2) { ec_from_hex(ui_entry_text(ec.hex)); ec_sync_hex(); }
        ec_set_focus((ec.focus + (back ? 4 : 1)) % 5);
        return;
    }
    bool enter = e->sym == XKB_KEY_Return || e->sym == XKB_KEY_KP_Enter;
    if (enter && ec.focus != 4) { ec_ok(); return; }
    switch (ec.focus) {
    case 0: {
        double step = e->mods & UI_MOD_SHIFT ? 10 : 2;
        if (e->sym == XKB_KEY_Left) ec.h = fmax(0, ec.h - step);
        else if (e->sym == XKB_KEY_Right) ec.h = fmin(359.9, ec.h + step);
        else if (e->sym == XKB_KEY_Up) ec.sat = fmin(1, ec.sat + step / 100);
        else if (e->sym == XKB_KEY_Down) ec.sat = fmax(0, ec.sat - step / 100);
        else return;
        break;
    }
    case 1:
        if (e->sym == XKB_KEY_Up) ec.v = fmin(1, ec.v + 0.02);
        else if (e->sym == XKB_KEY_Down) ec.v = fmax(0, ec.v - 0.02);
        else return;
        break;
    case 2: {
        ui_entry_result res = ui_entry_key(ec.hex, e);
        if (res == UI_ENTRY_CHANGED) ec_from_hex(ui_entry_text(ec.hex));   /* (a whole colour: the square follows) */
        if (res != UI_ENTRY_NONE) ui_surface_damage_all(s);
        return;
    }
    case 3: if (e->sym == XKB_KEY_space) ec_ok(); return;
    default: if (enter || e->sym == XKB_KEY_space) ec_close(); return;
    }
    ec_sync_hex();
    ui_surface_damage_all(s);
}

static void ec_free_later(void *arg)
{
    (void)arg;
    ui_entry_free(ec.hex);
    ui_surface_destroy(ec.s);
    ec.s = NULL;
    ec.hex = NULL;
    ec.closing = false;
}

static void ec_close(void)
{
    if (!ec.s || ec.closing) return;
    ec.closing = true;
    ui_app_post(app, ec_free_later, NULL);
}

static void ec_close_req(ui_surface *s) { (void)s; ec_close(); }
static void ec_resized(ui_surface *s, double w, double h) { (void)w; (void)h; ui_surface_damage_all(s); }
static void ec_focus_ev(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }
static const ui_surface_ops ec_ops = { .draw = ec_draw, .pointer = ec_pointer, .key = ec_key, .resized = ec_resized,
                                       .close = ec_close_req, .focus = ec_focus_ev,
                                       .drag_motion = ui_font_drag_motion, .drop = ui_font_drop };   /* a font: the Dialogs font */

bool colorpal_edit(size_t i)
{
    if (i >= nsamples) return false;
    ec.sample = i;
    snprintf(ec.old, sizeof ec.old, "%s", samples[i]);
    ec.h = 0;
    ec_from_hex(samples[i]);
    if (ec.s && !ec.closing) {
        ec_sync_hex();
        ui_window_activate(ec.s);
        ui_surface_damage_all(ec.s);
        return true;
    }
    ec.hex = ui_entry_new(samples[i]);
    ec.pressed = ec.dragging = 0;
    double fr = ui_scheme_now.title_h + 2 * ui_scheme_now.border;
    ec.s = ui_window_new(app, &ec_ops, NULL, "io.github.sungsookhim.Objectspace.EditColour", EC_W, 3 * M + SQ_H + 10 + ROW + fr);
    ui_window_set_title(ec.s, "Edit Colour");
    ec_set_focus(0);
    ec_sync_hex();
    return true;
}

bool colorpal_edit_is_open(void) { return ec.s && !ec.closing; }

bool colorpal_edit_pick(double x, double y)
{
    if (!colorpal_edit_is_open()) return false;
    struct ec_rects r = ec_rects();
    ec.dragging = 1;
    ec_track(r.sq.x + x * SQ_W, r.sq.y + y * (SQ_H - 1));
    ec.dragging = 0;
    return true;
}

bool colorpal_edit_bright(double v)
{
    if (!colorpal_edit_is_open()) return false;
    struct ec_rects r = ec_rects();
    ec.dragging = 2;
    ec_track(0, r.br.y + (1 - v) * r.br.h);
    ec.dragging = 0;
    return true;
}

const char *colorpal_edit_hex(void) { return colorpal_edit_is_open() ? ui_entry_text(ec.hex) : NULL; }

bool colorpal_edit_type(const char *hex)
{
    if (!colorpal_edit_is_open()) return false;
    ec_set_focus(2);
    ui_entry_set_text(ec.hex, hex);
    ec_from_hex(hex);
    ui_surface_damage_all(ec.s);
    return true;
}

bool colorpal_edit_ok(void)
{
    if (!colorpal_edit_is_open()) return false;
    ec_ok();
    return true;
}

bool colorpal_edit_snapshot(const char *png) { return colorpal_edit_is_open() && ui_surface_snapshot(ec.s, png); }

/* ------------------------------------------------------------ the palette */

static ui_rect cell(size_t i)
{
    ui_rect c = ui_frame_client(pal.s);
    return (ui_rect){ c.x + M + (double)(i % COLS) * CELL_W, c.y + M + (double)(i / COLS) * CELL_H, CELL_W - 6, CELL_H - 6 };
}

static ui_rect edit_btn(void)
{
    ui_rect c = ui_frame_client(pal.s);
    size_t rows = (MAX_SAMPLES + COLS - 1) / COLS;
    return (ui_rect){ c.x + M, c.y + M + (double)rows * CELL_H + 2, BTN_W, ROW };
}

static size_t sample_at(double x, double y)
{
    for (size_t i = 0; i < nsamples; i++) if (ui_rect_has(cell(i), x, y)) return i;
    return SIZE_MAX;
}

static void pal_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s;
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(pal.s, cr, clip, "Colour Palette");
    ui_rect c = ui_frame_client(pal.s);
    ui_fill(cr, c, k->face);
    for (size_t i = 0; i < nsamples; i++) {
        ui_rect r = cell(i);
        if (i == pal.sel) {   /* (chosen: a frame round it) */
            ui_fill(cr, (ui_rect){ r.x - 3, r.y - 3, r.w + 6, r.h + 6 }, k->selection);
        }
        set_rgb(cr, samples[i]);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_fill(cr);
        ui_bevel(cr, r, false);
    }
    button(cr, edit_btn(), "Edit colour...", pal.edit_down, false);
    ui_rect st = { c.x, c.y + c.h - STATUS_H, c.w, STATUS_H };
    ui_bevel(cr, (ui_rect){ st.x + 1, st.y + 1, st.w - 2, st.h - 2 }, false);
    ui_set_color(cr, k->text);
    cairo_save(cr);
    cairo_rectangle(cr, st.x + 2, st.y + 2, st.w - 4, st.h - 4);
    cairo_clip(cr);
    char msg[160];
    if (nsamples) snprintf(msg, sizeof msg, "%s: drag it onto a folder or its title bar (Ctrl: titles; Shift: all)", samples[pal.sel]);
    else snprintf(msg, sizeof msg, "No colours: add them in its Properties");
    ui_text_draw(cr, UI_FONT_DIALOG, st.x + 6, st.y + (STATUS_H + ui_font_ascent(UI_FONT_DIALOG)) / 2 - 1, msg, st.w - 12);
    cairo_restore(cr);
}

static void drag_send(void *ctx, const char *mime, int fd)
{
    (void)ctx; (void)mime;
    ui_app_write_async(app, fd, pal.drag, strlen(pal.drag));
}

static void drag_end(void *ctx, bool dropped, uint32_t action) { (void)ctx; (void)dropped; (void)action; }

static void start_drag(size_t i)
{
    snprintf(pal.drag, sizeof pal.drag, "%s", samples[i]);
    int sc = ui_surface_scale(pal.s);
    double w = 32, h = 24;
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)w * sc, (int)h * sc);
    cairo_surface_set_device_scale(img, sc, sc);
    cairo_t *cr = cairo_create(img);
    set_rgb(cr, samples[i]);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    ui_bevel(cr, (ui_rect){ 0, 0, w, h }, true);
    cairo_destroy(cr);
    const char *mimes[] = { UI_MIME_COLOR };
    ui_drag_start(pal.s, pal.serial, mimes, 1, UI_DND_COPY, img, sc, drag_send, drag_end, NULL);
    cairo_surface_destroy(img);
}

enum { PM_EDIT = 1 };

static void menu_chosen(void *ctx, int64_t id)
{
    size_t i = (size_t)(uintptr_t)ctx;
    if (i < nsamples && id == PM_EDIT) colorpal_edit(i);
}

static void sample_menu(size_t i, double x, double y, uint32_t serial)
{
    ui_menu_item items[] = { { PM_EDIT, "Edit colour...", true, false, NULL, 0 } };
    ui_menu_popup(pal.s, items, sizeof items / sizeof *items, x, y, serial, menu_chosen, (void *)(uintptr_t)i);
}

static void pal_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_hit h = ui_frame_hit(s, e->x, e->y);
    if ((e->kind != UI_PRESS || h != UI_HIT_CLIENT) && ui_frame_pointer(s, e) && e->kind == UI_PRESS) return;
    if (e->kind == UI_PRESS && h == UI_HIT_CLIENT) {
        size_t i = sample_at(e->x, e->y);
        if (i != SIZE_MAX) {
            pal.sel = i;
            ui_surface_damage_all(s);
            if (e->button == UI_BTN1 && e->clicks == 2) { colorpal_edit(i); return; }
            if (ui_mouse_is(UI_ACT_DRAG, e) || ui_mouse_is(UI_ACT_POPUP, e)) {
                pal.armed = ui_mouse_is(UI_ACT_DRAG, e);
                pal.popup_armed = ui_mouse_is(UI_ACT_POPUP, e);
                pal.press_i = i;
                pal.px = e->x;
                pal.py = e->y;
                pal.serial = e->serial;
            }
            return;
        }
        if (e->button == UI_BTN1 && ui_rect_has(edit_btn(), e->x, e->y)) { pal.edit_down = true; ui_surface_damage_all(s); }
        return;
    }
    if (e->kind == UI_MOTION && pal.armed && hypot(e->x - pal.px, e->y - pal.py) > 4) {
        pal.armed = pal.popup_armed = false;
        start_drag(pal.press_i);
        return;
    }
    if (e->kind == UI_RELEASE) {
        if (pal.popup_armed) sample_menu(pal.press_i, e->x, e->y, e->serial);
        pal.armed = pal.popup_armed = false;
        if (pal.edit_down) {
            pal.edit_down = false;
            ui_surface_damage_all(s);
            if (ui_rect_has(edit_btn(), e->x, e->y)) colorpal_edit(pal.sel);
        }
    }
}

static void pal_key(ui_surface *s, const ui_key_event *e)
{
    if (!nsamples) return;
    size_t n = nsamples;
    switch (e->sym) {
    case XKB_KEY_Right: pal.sel = (pal.sel + 1) % n; break;
    case XKB_KEY_Left: pal.sel = (pal.sel + n - 1) % n; break;
    case XKB_KEY_Down: if (pal.sel + COLS < n) pal.sel += COLS; break;
    case XKB_KEY_Up: if (pal.sel >= COLS) pal.sel -= COLS; break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: colorpal_edit(pal.sel); return;
    default: return;
    }
    ui_surface_damage_all(s);
}

static void pal_free_later(void *arg)
{
    (void)arg;
    ui_surface_destroy(pal.s);
    pal.s = NULL;
    pal.closing = false;
}

static void pal_close(ui_surface *s)
{
    (void)s;
    if (!pal.s || pal.closing) return;
    pal.closing = true;
    events_raise("window.close", "Colour Palette", "window", (ui_rect){ 0, 0, 0, 0 });
    ui_app_post(app, pal_free_later, NULL);
}

static void pal_resized(ui_surface *s, double w, double h) { (void)w; (void)h; ui_surface_damage_all(s); }
static void pal_focus(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }
static const ui_surface_ops pal_ops = { .draw = pal_draw, .pointer = pal_pointer, .key = pal_key, .resized = pal_resized,
                                        .close = pal_close, .focus = pal_focus,
                                       .drag_motion = ui_font_drag_motion, .drop = ui_font_drop };   /* a font: the Dialogs font */

void colorpal_init(ui_app *a, colorpal_apply_fn fn)
{
    app = a;
    apply = fn;
    if (!nsamples)
        colorpal_set_samples("#ffffff;#c0c0c0;#808080;#404040;#000000;#008080;#ff0000;#800000;#ffff00;#808000;#00ff00;#008000;"
                             "#00ffff;#0000ff;#000080;#ff00ff;#800080;#ff8000;#804000;#ffc0c0;#c0ffc0;#c0c0ff;#ffffc0;#c0ffff;"
                             "#3a6ea5;#5f9ea0;#cccccc;#e0d8b0;#b0c4de;#2f4f4f");
}

void colorpal_open(void)
{
    if (pal.s && !pal.closing) { ui_window_activate(pal.s); return; }
    double fr = ui_scheme_now.title_h + 2 * ui_scheme_now.border;
    size_t rows = (MAX_SAMPLES + COLS - 1) / COLS;
    double w = 2 * M + COLS * CELL_W + 2 * ui_scheme_now.border + 120;
    double h = 2 * M + (double)rows * CELL_H + ROW + 2 * M + STATUS_H + fr;
    pal.s = ui_window_new(app, &pal_ops, NULL, "io.github.sungsookhim.Objectspace.ColourPalette", w, h);
    ui_window_set_title(pal.s, "Colour Palette");
    events_raise("window.open", "Colour Palette", "window", (ui_rect){ 0, 0, 0, 0 });
}

bool colorpal_is_open(void) { return pal.s && !pal.closing; }
size_t colorpal_count(void) { return nsamples; }
const char *colorpal_sample(size_t i) { return i < nsamples ? samples[i] : NULL; }
bool colorpal_snapshot(const char *png) { return pal.s && ui_surface_snapshot(pal.s, png); }
