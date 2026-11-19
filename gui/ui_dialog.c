/* Message dialogs: a message and a row of buttons (gui-design.md, Widget
 * set 5). Enter chooses the focused button, Escape the last one (by
 * convention the cancelling one), Tab and the arrows move between them. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ui.h"

#define MARGIN 16
#define LINE_H 18
#define BUTTON_H 26
#define WIDTH 440

struct ui_dialog {
    ui_surface *win;
    char title[256];
    char **lines;
    size_t nlines;
    char **buttons;
    size_t nbuttons;
    int focus, pressed;
    ui_dialog_fn fn;
    void *ctx;
    bool done;
};

/* Word-wrap the message to the dialog's width (paragraphs on \n). */
static void wrap(ui_dialog *d, const char *msg, double width)
{
    char *copy = strdup(msg), *save = NULL;
    for (char *para = strtok_r(copy, "\n", &save); para; para = strtok_r(NULL, "\n", &save)) {
        char line[1024] = "";
        char *wsave = NULL;
        for (char *w = strtok_r(para, " ", &wsave); w; w = strtok_r(NULL, " ", &wsave)) {
            char trial[1024];
            snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", w);
            if (line[0] && ui_text_width(UI_FONT_REGULAR, trial) > width) {
                d->lines = realloc(d->lines, (d->nlines + 1) * sizeof *d->lines);
                d->lines[d->nlines++] = strdup(line);
                snprintf(line, sizeof line, "%s", w);
            } else {
                snprintf(line, sizeof line, "%s", trial);
            }
        }
        d->lines = realloc(d->lines, (d->nlines + 1) * sizeof *d->lines);
        d->lines[d->nlines++] = strdup(line);
    }
    free(copy);
}

static double button_w(ui_dialog *d, size_t i) { return ui_text_width(UI_FONT_REGULAR, d->buttons[i]) + 28; }

static ui_rect button_rect(ui_dialog *d, size_t i)
{
    ui_rect c = ui_frame_client(d->win);
    double total = 0;
    for (size_t k = 0; k < d->nbuttons; k++) total += button_w(d, k) + 8;
    double x = c.x + c.w - MARGIN - total + 8;
    for (size_t k = 0; k < i; k++) x += button_w(d, k) + 8;
    return (ui_rect){ x, c.y + c.h - MARGIN - BUTTON_H, button_w(d, i), BUTTON_H };
}

static void dlg_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    ui_dialog *d = ui_surface_data(s);
    const ui_scheme *k = &ui_scheme_now;
    ui_frame_draw(s, cr, clip, d->title);
    ui_rect c = ui_frame_client(s);
    if (!ui_rect_meets(clip, c)) return;
    ui_fill(cr, c, k->face);
    ui_set_color(cr, k->text);
    for (size_t i = 0; i < d->nlines; i++)
        ui_text_draw(cr, UI_FONT_REGULAR, c.x + MARGIN, c.y + MARGIN + (double)(i + 1) * LINE_H - 4, d->lines[i], c.w - 2 * MARGIN);
    for (size_t i = 0; i < d->nbuttons; i++) {
        ui_rect b = button_rect(d, i);
        bool down = d->pressed == (int)i;
        ui_fill(cr, b, k->face);
        ui_bevel(cr, b, !down);
        if (d->focus == (int)i) {   /* the default button: a dark outline, as in OS/2 */
            ui_set_color(cr, k->dark);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, b.x - 1.5, b.y - 1.5, b.w + 3, b.h + 3);
            cairo_stroke(cr);
        }
        ui_set_color(cr, k->text);
        double tw = ui_text_width(UI_FONT_REGULAR, d->buttons[i]);
        ui_text_draw(cr, UI_FONT_REGULAR, b.x + (b.w - tw) / 2 + (down ? 1 : 0),
                     b.y + (b.h + ui_text_ascent()) / 2 - 1 + (down ? 1 : 0), d->buttons[i], 0);
    }
}

static void destroy_later(void *arg)
{
    ui_dialog *d = arg;
    for (size_t i = 0; i < d->nlines; i++) free(d->lines[i]);
    for (size_t i = 0; i < d->nbuttons; i++) free(d->buttons[i]);
    free(d->lines);
    free(d->buttons);
    ui_surface_destroy(d->win);
    free(d);
}

static void finish(ui_dialog *d, int choice)
{
    if (d->done) return;
    d->done = true;
    if (d->fn) d->fn(d->ctx, choice);
    ui_app_post(ui_surface_app(d->win), destroy_later, d);
}

static void dlg_pointer(ui_surface *s, const ui_pointer_event *e)
{
    ui_dialog *d = ui_surface_data(s);
    if (e->kind != UI_PRESS || ui_frame_hit(s, e->x, e->y) != UI_HIT_CLIENT)
        if (ui_frame_pointer(s, e) && e->kind == UI_PRESS) return;
    if (e->kind == UI_PRESS && e->button == ui_mouse_button(UI_ACT_SELECT)) {
        for (size_t i = 0; i < d->nbuttons; i++)
            if (ui_rect_has(button_rect(d, i), e->x, e->y)) {
                d->pressed = d->focus = (int)i;
                ui_surface_damage_all(s);
            }
    } else if (e->kind == UI_RELEASE && d->pressed >= 0) {
        int p = d->pressed;
        d->pressed = -1;
        ui_surface_damage_all(s);
        if (ui_rect_has(button_rect(d, (size_t)p), e->x, e->y)) finish(d, p);
    }
}

static void dlg_key(ui_surface *s, const ui_key_event *e)
{
    ui_dialog *d = ui_surface_data(s);
    int n = (int)d->nbuttons;
    switch (e->sym) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space: finish(d, d->focus); return;
    case XKB_KEY_Escape: finish(d, n - 1); return;
    case XKB_KEY_Tab: case XKB_KEY_Right: d->focus = (d->focus + 1) % n; break;
    case XKB_KEY_ISO_Left_Tab: case XKB_KEY_Left: d->focus = (d->focus + n - 1) % n; break;
    default:
        /* A button's first letter chooses it. */
        if (e->utf8[0] && !e->utf8[1])
            for (int i = 0; i < n; i++)
                if ((d->buttons[i][0] | 0x20) == (e->utf8[0] | 0x20)) { finish(d, i); return; }
        return;
    }
    ui_surface_damage_all(s);
}

static void dlg_close(ui_surface *s) { finish(ui_surface_data(s), (int)((ui_dialog *)ui_surface_data(s))->nbuttons - 1); }
static void dlg_resized(ui_surface *s, double w, double h) { (void)w; (void)h; ui_surface_damage_all(s); }
static void dlg_focus(ui_surface *s, bool on) { (void)on; ui_frame_damage(s); }

static const ui_surface_ops dlg_ops = {
    .draw = dlg_draw, .pointer = dlg_pointer, .key = dlg_key, .close = dlg_close, .resized = dlg_resized, .focus = dlg_focus,
};

ui_dialog *ui_dialog_open(ui_app *a, const char *title, const char *message, const char *const *buttons, size_t n,
                          int default_button, ui_dialog_fn fn, void *ctx)
{
    ui_dialog *d = calloc(1, sizeof *d);
    snprintf(d->title, sizeof d->title, "%s", title);
    d->fn = fn;
    d->ctx = ctx;
    d->pressed = -1;
    d->focus = default_button >= 0 && (size_t)default_button < n ? default_button : 0;
    d->buttons = calloc(n ? n : 1, sizeof *d->buttons);
    d->nbuttons = n;
    for (size_t i = 0; i < n; i++) d->buttons[i] = strdup(buttons[i]);
    wrap(d, message, WIDTH - 2 * MARGIN - 8);
    double h = ui_scheme_now.title_h + 2 * ui_scheme_now.border + 2 * MARGIN + (double)d->nlines * LINE_H + 16 + BUTTON_H + MARGIN;
    d->win = ui_window_new(a, &dlg_ops, d, "org.wps.dialog", WIDTH, h);
    ui_window_set_title(d->win, title);
    return d;
}

ui_surface *ui_dialog_surface(ui_dialog *d) { return d->win; }
