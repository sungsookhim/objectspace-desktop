/* Tooltips (tooltip.h). */
#define _GNU_SOURCE
#include "tooltip.h"

#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

static ui_app *app;
static int timer = -1;

static struct {
    ui_surface *parent, *s;    /* s: shown */
    ui_rect anchor;
    char text[256];
    bool above;
} tt;

static void tt_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s; (void)clip;
    const ui_scheme *k = &ui_scheme_now;
    double w = ceil(ui_text_width(UI_FONT_REGULAR, tt.text)) + 12, h = ceil(ui_scheme_now.font_px * 1.5) + 4;
    /* OS/2 Warp 4's: a pale yellow box with a thin border */
    ui_set_color(cr, k->tooltip_bg);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    ui_set_color(cr, k->shadow);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
    cairo_stroke(cr);
    ui_set_color(cr, k->tooltip_text);
    ui_text_draw(cr, UI_FONT_REGULAR, 6, (h + ui_text_ascent()) / 2 - 1, tt.text, 0);
}

static const ui_surface_ops tt_ops = { .draw = tt_draw };

static void disarm(void)
{
    if (timer < 0) return;
    struct itimerspec its = { 0 };
    timerfd_settime(timer, 0, &its, NULL);
}

void tooltip_hide(void)
{
    disarm();
    if (tt.s) ui_surface_destroy(tt.s);
    tt.s = NULL;
    tt.parent = NULL;
    tt.text[0] = 0;
}

static void show(void)
{
    if (!tt.parent || !tt.text[0] || tt.s) return;
    double w = ceil(ui_text_width(UI_FONT_REGULAR, tt.text)) + 12, h = ceil(ui_scheme_now.font_px * 1.5) + 4;
    /* (no grab: serial 0) */
    tt.s = tt.above ? ui_popup_new_above(app, tt.parent, &tt_ops, NULL, tt.anchor, w, h, 0)
                    : ui_popup_new(app, tt.parent, &tt_ops, NULL, tt.anchor, w, h, 0);
}

static void on_timer(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t n;
    if (read(fd, &n, sizeof n) < 0) { /* nothing */ }
    show();
}

void tooltip_init(ui_app *a)
{
    app = a;
    timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer >= 0) ui_app_watch_fd(a, timer, POLLIN, on_timer, NULL);
}

void tooltip_hover(ui_surface *parent, ui_rect anchor, const char *text, bool above)
{
    if (!text || !*text) { tooltip_hide(); return; }
    bool same = parent == tt.parent && !strcmp(text, tt.text) && anchor.x == tt.anchor.x && anchor.y == tt.anchor.y &&
                anchor.w == tt.anchor.w && anchor.h == tt.anchor.h;
    if (same) return;
    tooltip_hide();
    tt.parent = parent;
    tt.anchor = anchor;
    tt.above = above;
    snprintf(tt.text, sizeof tt.text, "%s", text);
    if (timer < 0) return;
    struct itimerspec its = { .it_value = { 0, 600 * 1000000L } };
    timerfd_settime(timer, 0, &its, NULL);
}

const char *tooltip_shown(void) { return tt.s ? tt.text : NULL; }
