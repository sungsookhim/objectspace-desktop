/* The WarpCenter (see warpcenter.h). */
#define _GNU_SOURCE
#include "warpcenter.h"

#include <limits.h>
#include <poll.h>
#include <strings.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "events.h"
#include "icons.h"
#include "tray.h"
#include "svc.h"
#include "findwin.h"
#include "launchpad.h"
#include "session.h"
#include "winlist.h"
#include "objmenu.h"
#include "tooltip.h"

#define PAD 3
#define BTN_W 34
#define KEY_W 40
#define SYS_W 28           /* Lockup, Find, Log off, Shut down */
#define METER_W 9
#define NMETERS 4

/* (P_LOCK..P_SHUTDOWN: OS/2's buttons after the window list, the owner's) */
enum part { P_NONE = -1, P_DESKTOP, P_WINDOWS, P_METERS, P_MESSAGES, P_KEYBOARD, P_CLOCK, P_TRAY,
            P_LOCK, P_FIND, P_LOGOFF, P_SHUTDOWN, NPARTS };

/* Its font (the Font Palette's; the interface font unless set). */
static ui_font wc_font = UI_FONT_REGULAR, wc_bold = UI_FONT_BOLD;

static struct {
    ui_app *app;
    ui_surface *s;
    double w, h;
    bool top;
    char desktop[PATH_MAX];
    void (*open_folder)(const char *path);
    int pressed;
    /* keyboard */
    char key_label[16];
    int key_index, key_count;
    /* clock: 0 24 hours, 1 12 hours, 2 off */
    char clock[64];
    int clock_mode;
    bool seconds, date;
    int timer;
    /* the WarpCenter object's parts (meters, messages, keyboard) */
    bool shown[NPARTS];
    /* messages */
    size_t unread;
    void (*open_messages)(void);
    /* meters: 0..1 each */
    double meter[NMETERS];
    unsigned long long cpu_total, cpu_idle, disk, net;
    double disk_peak, net_peak, disk_rate, net_rate, mem_used, mem_total;
    int ticks;
    /* the menu */
    objmenu *reading, *ready;      /* ready: the last tree read whole (objmenu.h) */
    bool want_menu;
    uint32_t menu_serial;
    /* the calendar or the meters' pop-up */
    ui_surface *pop;
    int pop_kind;                  /* P_CLOCK or P_METERS */
    int cal_month, cal_year;
} wc = { .pressed = P_NONE, .timer = -1, .shown = { true, true, true, true, true, true, true, true, true, true, true } };

/* ------------------------------------------------------------- layout */

static double bar_h(void) { return ceil(ui_scheme_now.font_px * 1.9) + 2 * PAD; }

/* (the clock's text with its digits as 8s: it does not jitter as they change) */
static double clock_w(void)
{
    char t[64];
    snprintf(t, sizeof t, "%s", wc.clock);
    for (char *q = t; *q; q++) if (*q >= '0' && *q <= '9') *q = '8';
    return ui_text_width(wc_font, t) + 20;
}

/* The Desktop button: its picture and the desktop's name, as OS/2's
 * WarpCenter's first button. */
static const char *desk_label(void) { return "Desktop"; }
static double desk_w(void) { return wc.h - 2 * PAD + ui_text_width(wc_bold, desk_label()) + 14; }

static double msg_w(void)
{
    char t[16];
    snprintf(t, sizeof t, "%zu", wc.unread);
    return wc.h - 2 * PAD + 6 + (wc.unread ? ui_text_width(wc_bold, t) + 4 : 0);
}

static bool part_shown(int p)
{
    if (p == P_TRAY) return wc.shown[p] && tray_count() > 0;
    return p == P_CLOCK ? wc.clock_mode != 2 : wc.shown[p];
}

static double tray_cell(void) { return wc.h - 2 * PAD; }

/* From the right: the clock, the keyboard, messages, the meters, the
 * tray (other programs' icons, when there are any); parts the WarpCenter
 * object turned off take no room. */
/* Log off: a door, and an arrow leaving it (centred on 0,0; 30 across). */
static void glyph_logoff(cairo_t *cr)
{
    cairo_set_line_width(cr, 2);
    cairo_set_source_rgb(cr, 0.55, 0.35, 0.15);
    cairo_rectangle(cr, -11, -12, 12, 24);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.95, 0.85, 0.3);
    cairo_arc(cr, -2, 1, 1.5, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.1, 0.45, 0.1);
    cairo_set_line_width(cr, 3);
    cairo_move_to(cr, 1, 0); cairo_line_to(cr, 12, 0);
    cairo_stroke(cr);
    cairo_move_to(cr, 13, 0); cairo_line_to(cr, 7, -6); cairo_line_to(cr, 7, 6);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static ui_rect part_rect(int p)
{
    double h = wc.h - 2 * PAD;
    switch (p) {
    case P_DESKTOP: return (ui_rect){ PAD, PAD, desk_w(), h };
    case P_WINDOWS: return (ui_rect){ PAD + desk_w() + 2, PAD, BTN_W, h };
    case P_LOCK: case P_FIND: case P_LOGOFF: case P_SHUTDOWN:
        if (!wc.shown[p]) return (ui_rect){ 0, 0, 0, 0 };
        return (ui_rect){ PAD + desk_w() + 2 + BTN_W + 6 + (p - P_LOCK) * (SYS_W + 2), PAD, SYS_W, h };
    default: break;
    }
    static const int order[] = { P_CLOCK, P_KEYBOARD, P_MESSAGES, P_METERS, P_TRAY };
    double x = wc.w - PAD;
    for (size_t i = 0; i < sizeof order / sizeof *order; i++) {
        int q = order[i];
        if (!part_shown(q)) { if (q == p) return (ui_rect){ 0, 0, 0, 0 }; continue; }
        double w = q == P_CLOCK ? clock_w() : q == P_KEYBOARD ? KEY_W : q == P_MESSAGES ? msg_w()
                 : q == P_TRAY ? (double)tray_count() * tray_cell() + 4 : NMETERS * (METER_W + 3) + 7;
        x -= w;
        if (q == p) return (ui_rect){ x, PAD, w, h };
        x -= 4;
    }
    return (ui_rect){ 0, 0, 0, 0 };
}

static int part_at(double x, double y)
{
    for (int p = 0; p < NPARTS; p++) {
        ui_rect r = part_rect(p);
        if (r.w <= 0) continue;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return p;
    }
    return P_NONE;
}

/* ------------------------------------------------------------- drawing */

static void glyph_desktop(cairo_t *cr, ui_rect r)
{
    /* a little monitor with a folder on it: the desktop */
    double cx = r.x + r.w / 2, cy = r.y + r.h / 2, s = fmin(r.w, r.h) / 26.0;
    cairo_set_line_width(cr, 1.2);
    ui_set_color(cr, ui_scheme_now.text);
    cairo_rectangle(cr, cx - 9 * s + 0.5, cy - 7 * s + 0.5, 18 * s, 12 * s);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 4 * s, cy + 8 * s);
    cairo_line_to(cr, cx + 4 * s, cy + 8 * s);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0.94, 0.78, 0.35);
    cairo_rectangle(cr, cx - 5 * s, cy - 3 * s, 10 * s, 6 * s);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.24, 0.43, 0.75);
    cairo_rectangle(cr, cx - 8 * s, cy - 6 * s, 16 * s, 2 * s);
    cairo_fill(cr);
}

static void glyph_windows(cairo_t *cr, ui_rect r)
{
    /* two windows, one over the other: the Window List */
    double cx = r.x + r.w / 2, cy = r.y + r.h / 2, s = fmin(r.w, r.h) / 26.0;
    for (int k = 0; k < 2; k++) {
        double x = cx - 8 * s + k * 5 * s, y = cy - 7 * s + k * 5 * s;
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, x, y, 12 * s, 9 * s);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.5);
        cairo_rectangle(cr, x, y, 12 * s, 2.5 * s);
        cairo_fill(cr);
        ui_set_color(cr, ui_scheme_now.text);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x + 0.5, y + 0.5, 12 * s, 9 * s);
        cairo_stroke(cr);
    }
}

static void glyph_envelope(cairo_t *cr, ui_rect r, bool unread)
{
    double s = fmin(r.w, r.h) / 26.0, w = 18 * s, h = 12 * s;
    double x = round(r.x + (r.w - w) / 2) + 0.5, y = round(r.y + (r.h - h) / 2) + 0.5;
    if (unread) cairo_set_source_rgb(cr, 1.0, 0.95, 0.6);
    else cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill_preserve(cr);
    ui_set_color(cr, ui_scheme_now.text);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + w / 2, y + h * 0.6);
    cairo_line_to(cr, x + w, y);
    cairo_stroke(cr);
}

static void centred_text(cairo_t *cr, ui_rect r, const char *t, double d)
{
    double tw = ui_text_width(wc_font, t);
    ui_set_color(cr, ui_scheme_now.text);
    ui_text_draw(cr, wc_font, r.x + (r.w - tw) / 2 + d, r.y + (r.h + ui_font_ascent(wc_font)) / 2 - 1 + d, t, r.w);
}

static void wc_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s; (void)clip;
    const ui_scheme *k = &ui_scheme_now;
    ui_rect all = { 0, 0, wc.w, wc.h };
    ui_fill(cr, all, k->face);
    /* a raised bar; the edge towards the screen's middle lit */
    ui_fill(cr, (ui_rect){ 0, wc.top ? wc.h - 1 : 0, wc.w, 1 }, wc.top ? k->shadow : k->light);
    for (int p = 0; p < NPARTS; p++) {
        ui_rect r = part_rect(p);
        if (r.w <= 0) continue;
        bool down = wc.pressed == p || (p == P_DESKTOP && ui_menu_is_open() && wc.want_menu) ||
                    ((p == P_CLOCK || p == P_METERS) && wc.pop && wc.pop_kind == p);
        double d = down ? 1 : 0;
        if (p == P_DESKTOP || p == P_WINDOWS) {
            ui_bevel(cr, r, !down);
            cairo_save(cr);
            cairo_translate(cr, d, d);
            if (p == P_DESKTOP) {
                glyph_desktop(cr, (ui_rect){ r.x + 2, r.y, r.h, r.h });
                ui_set_color(cr, ui_scheme_now.text);
                ui_text_draw(cr, wc_bold, r.x + r.h + 4, r.y + (r.h + ui_font_ascent(wc_font)) / 2 - 1, desk_label(), r.w - r.h - 6);
            } else glyph_windows(cr, r);
            cairo_restore(cr);
        } else if (p >= P_LOCK && p <= P_SHUTDOWN) {
            ui_bevel(cr, r, !down);
            cairo_save(cr);
            cairo_translate(cr, r.x + r.w / 2 + d, r.y + r.h / 2 + d);
            double sc = fmin(r.w, r.h) / 30;
            cairo_scale(cr, sc, sc);
            if (p == P_LOGOFF) { if (!icons_draw_theme(cr, NULL, "sys-logoff", (ui_rect){ -15, -15, 30, 30 }, 3.6)) glyph_logoff(cr); }
            else launchpad_glyph(cr, p == P_LOCK ? 0 : p == P_FIND ? 1 : 2, (ui_rect){ -15, -15, 30, 30 });
            cairo_restore(cr);
        } else if (p == P_KEYBOARD) {
            ui_bevel(cr, r, down);
            centred_text(cr, r, wc.key_label[0] ? wc.key_label : "--", d);
        } else if (p == P_MESSAGES) {
            ui_bevel(cr, r, down);
            glyph_envelope(cr, (ui_rect){ r.x + d + 2, r.y + d, r.h, r.h }, wc.unread > 0);
            if (wc.unread) {
                char t[16];
                snprintf(t, sizeof t, "%zu", wc.unread);
                ui_set_color(cr, ui_scheme_now.text);
                ui_text_draw(cr, wc_bold, r.x + r.h + 2 + d, r.y + (r.h + ui_font_ascent(wc_font)) / 2 - 1 + d, t, r.w - r.h);
            }
        } else if (p == P_CLOCK) {
            ui_bevel(cr, r, false);
            centred_text(cr, r, wc.clock, d);
        } else if (p == P_TRAY) {
            ui_bevel(cr, r, false);
            double c = tray_cell(), sz = fmin(22, c - 4);
            for (size_t i = 0, n = tray_count(); i < n; i++) {
                cairo_surface_t *im = tray_icon(i);
                double x = r.x + 2 + (double)i * c + (c - sz) / 2, y = r.y + (r.h - sz) / 2;
                if (!im) {   /* (not read yet, or none): a dot */
                    ui_set_color(cr, k->shadow);
                    cairo_arc(cr, x + sz / 2, y + sz / 2, 3, 0, 2 * M_PI);
                    cairo_fill(cr);
                    continue;
                }
                int iw = cairo_image_surface_get_width(im), ih = cairo_image_surface_get_height(im);
                if (iw <= 0 || ih <= 0) continue;
                cairo_save(cr);
                cairo_translate(cr, x, y);
                cairo_scale(cr, sz / iw, sz / ih);
                cairo_set_source_surface(cr, im, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
                cairo_paint(cr);
                cairo_restore(cr);
            }
        } else if (p == P_METERS) {
            ui_bevel(cr, r, false);
            static const double col[NMETERS][3] = { { 0.2, 0.7, 0.3 }, { 0.25, 0.45, 0.85 }, { 0.85, 0.55, 0.15 }, { 0.6, 0.35, 0.8 } };
            for (int m = 0; m < NMETERS; m++) {
                ui_rect b = { r.x + 4 + m * (METER_W + 3), r.y + 4, METER_W, r.h - 8 };
                ui_fill(cr, b, k->client_bg);
                ui_bevel(cr, b, false);
                double v = fmin(1, fmax(0, wc.meter[m]));
                double hh = (b.h - 2) * v;
                cairo_set_source_rgb(cr, col[m][0], col[m][1], col[m][2]);
                cairo_rectangle(cr, b.x + 1, b.y + b.h - 1 - hh, b.w - 2, hh);
                cairo_fill(cr);
            }
        }
    }
}

static void damage(void) { if (wc.s) ui_surface_damage_all(wc.s); }

/* ------------------------------------------------------------ keyboard */

void warpcenter_source_label(bool ibus, const char *name, char *out, size_t n)
{
    static const char *const ime[][2] = {
        { "hangul", "한" }, { "anthy", "あ" }, { "mozc-jp", "あ" }, { "kkc", "あ" }, { "pinyin", "拼" },
        { "libpinyin", "拼" }, { "rime", "中" }, { "chewing", "注" }, { "m17n:", "M" },
    };
    if (ibus) {
        for (size_t i = 0; i < sizeof ime / sizeof ime[0]; i++)
            if (!strncmp(name, ime[i][0], strlen(ime[i][0]))) { snprintf(out, n, "%s", ime[i][1]); return; }
    } else {
        static const char *const lang[][2] = {
            { "us", "EN" }, { "gb", "EN" }, { "kr", "KO" }, { "jp", "JA" }, { "cn", "ZH" }, { "se", "SV" },
            { "dk", "DA" }, { "cz", "CS" }, { "gr", "EL" }, { "ua", "UK" }, { "English", "EN" }, { "Korean", "KO" },
        };
        char lay[64];
        snprintf(lay, sizeof lay, "%s", name);
        lay[strcspn(lay, "+ (")] = 0;
        for (size_t i = 0; i < sizeof lang / sizeof lang[0]; i++)
            if (!strcmp(lay, lang[i][0])) { snprintf(out, n, "%s", lang[i][1]); return; }
    }
    char t[3] = { 0 };
    for (int i = 0; i < 2 && name[i]; i++) t[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
    snprintf(out, n, "%s", t);
}

void warpcenter_input_source(int index, int count, bool ibus, const char *name)
{
    wc.key_index = index;
    wc.key_count = count;
    warpcenter_source_label(ibus, name, wc.key_label, sizeof wc.key_label);
    damage();
}

const char *warpcenter_keyboard_label(void) { return wc.key_label; }

/* ------------------------------------------------------- clock, meters */

static bool read_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char t[64] = "", hm[32] = "", f[16];
    if (wc.date) strftime(t, sizeof t, "%b %e  ", &tm);
    snprintf(f, sizeof f, "%s%s%s", wc.clock_mode == 1 ? "%I:%M" : "%H:%M", wc.seconds ? ":%S" : "", wc.clock_mode == 1 ? " %p" : "");
    strftime(hm, sizeof hm, f, &tm);
    /* 12 hours: "9:05 PM", not "09:05 PM" */
    strncat(t, wc.clock_mode == 1 && hm[0] == '0' ? hm + 1 : hm, sizeof t - strlen(t) - 1);
    if (!strcmp(t, wc.clock)) return false;
    snprintf(wc.clock, sizeof wc.clock, "%s", t);
    return true;
}

static void read_meters(void)
{
    char line[512];
    FILE *f = fopen("/proc/stat", "r");
    if (f && fgets(line, sizeof line, f)) {
        unsigned long long v[10] = { 0 }, total = 0;
        sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        for (int i = 0; i < 8; i++) total += v[i];
        unsigned long long idle = v[3] + v[4];
        if (wc.cpu_total && total > wc.cpu_total) wc.meter[0] = 1.0 - (double)(idle - wc.cpu_idle) / (double)(total - wc.cpu_total);
        wc.cpu_total = total;
        wc.cpu_idle = idle;
    }
    if (f) fclose(f);
    unsigned long long mt = 0, ma = 0;
    f = fopen("/proc/meminfo", "r");
    while (f && fgets(line, sizeof line, f)) {
        sscanf(line, "MemTotal: %llu", &mt);
        sscanf(line, "MemAvailable: %llu", &ma);
    }
    if (f) fclose(f);
    if (mt) { wc.meter[1] = (double)(mt - ma) / (double)mt; wc.mem_used = (double)(mt - ma) * 1024; wc.mem_total = (double)mt * 1024; }
    /* disk: sectors read and written by whole disks (not partitions) */
    unsigned long long sectors = 0;
    f = fopen("/proc/diskstats", "r");
    while (f && fgets(line, sizeof line, f)) {
        char dev[64];
        unsigned long long rd = 0, wr = 0;
        if (sscanf(line, "%*u %*u %63s %*u %*u %llu %*u %*u %*u %llu", dev, &rd, &wr) != 3) continue;
        if (!strncmp(dev, "loop", 4) || !strncmp(dev, "ram", 3) || !strncmp(dev, "zram", 4) || !strncmp(dev, "dm-", 3)) continue;
        char sys[128];
        snprintf(sys, sizeof sys, "/sys/block/%s", dev);
        if (access(sys, F_OK) != 0) continue;   /* a partition */
        sectors += rd + wr;
    }
    if (f) fclose(f);
    if (wc.disk && sectors >= wc.disk) {
        wc.disk_rate = (double)(sectors - wc.disk) * 512 / 2.0;   /* bytes a second (2 s apart) */
        if (wc.disk_rate > wc.disk_peak) wc.disk_peak = wc.disk_rate;
        wc.meter[2] = wc.disk_peak > 0 ? wc.disk_rate / wc.disk_peak : 0;
    }
    wc.disk = sectors;
    unsigned long long bytes = 0;
    f = fopen("/proc/net/dev", "r");
    while (f && fgets(line, sizeof line, f)) {
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        char *nm = line;
        while (*nm == ' ') nm++;
        if (!strcmp(nm, "lo")) continue;
        unsigned long long rx = 0, tx = 0;
        if (sscanf(c + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx) == 2) bytes += rx + tx;
    }
    if (f) fclose(f);
    if (wc.net && bytes >= wc.net) {
        wc.net_rate = (double)(bytes - wc.net) / 2.0;
        if (wc.net_rate > wc.net_peak) wc.net_peak = wc.net_rate;
        wc.meter[3] = wc.net_peak > 0 ? wc.net_rate / wc.net_peak : 0;
    }
    wc.net = bytes;
    /* peaks fade, so a burst long ago does not flatten today's bars */
    wc.disk_peak *= 0.98;
    wc.net_peak *= 0.98;
}

static void pop_draw_meters(cairo_t *cr, ui_rect r);
static void pop_draw_calendar(cairo_t *cr, ui_rect r);

static void on_tick(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t n;
    if (read(fd, &n, sizeof n) < 0) {}
    bool ch = read_clock();
    if (++wc.ticks % 2 == 0) { read_meters(); ch = true; }
    if (ch) damage();
    if (wc.pop && wc.pop_kind == P_METERS) ui_surface_damage_all(wc.pop);
}

/* ------------------------------------------------------------ pop-ups */

#define CAL_CELL 28

static void pop_free(void)
{
    if (wc.pop) ui_surface_destroy(wc.pop);
    wc.pop = NULL;
    damage();
}

static void pop_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)clip;
    ui_rect r = { 0, 0, ui_surface_width(s), ui_surface_height(s) };
    ui_fill(cr, r, ui_scheme_now.face);
    ui_bevel(cr, r, true);
    if (wc.pop_kind == P_CLOCK) pop_draw_calendar(cr, r);
    else pop_draw_meters(cr, r);
}

static void pop_pointer(ui_surface *s, const ui_pointer_event *e)
{
    (void)s;
    if (e->kind != UI_PRESS || wc.pop_kind != P_CLOCK) return;
    /* the calendar's arrows: the month before, after */
    if (e->y < CAL_CELL + 6) {
        if (e->x < CAL_CELL + 8) { if (--wc.cal_month < 0) { wc.cal_month = 11; wc.cal_year--; } }
        else if (e->x > ui_surface_width(wc.pop) - CAL_CELL - 8) { if (++wc.cal_month > 11) { wc.cal_month = 0; wc.cal_year++; } }
        ui_surface_damage_all(wc.pop);
    }
}

static void pop_dismissed(ui_surface *s) { (void)s; ui_app_post(wc.app, (void (*)(void *))pop_free, NULL); }

static const ui_surface_ops pop_ops = { .draw = pop_draw, .pointer = pop_pointer, .dismissed = pop_dismissed };

static void pop_draw_calendar(cairo_t *cr, ui_rect r)
{
    const ui_scheme *k = &ui_scheme_now;
    struct tm first = { .tm_mday = 1, .tm_mon = wc.cal_month, .tm_year = wc.cal_year - 1900, .tm_hour = 12 };
    mktime(&first);
    char head[64];
    strftime(head, sizeof head, "%B %Y", &first);
    centred_text(cr, (ui_rect){ r.x, r.y + 4, r.w, CAL_CELL }, head, 0);
    centred_text(cr, (ui_rect){ r.x + 6, r.y + 4, CAL_CELL, CAL_CELL }, "◀", 0);
    centred_text(cr, (ui_rect){ r.x + r.w - CAL_CELL - 6, r.y + 4, CAL_CELL, CAL_CELL }, "▶", 0);
    double x0 = r.x + 8, y0 = r.y + 4 + CAL_CELL;
    for (int d = 0; d < 7; d++) {   /* the weekdays' initials, Sunday first */
        struct tm t = { .tm_wday = d };
        char wd[16];
        strftime(wd, sizeof wd, "%a", &t);
        /* two letters (characters, not bytes): "Su", "Mo"; "일", "월" */
        int chars = 0;
        for (char *q = wd; *q; q++)
            if (((unsigned char)*q & 0xc0) != 0x80 && ++chars > 2) { *q = 0; break; }
        ui_set_color(cr, k->disabled_text);
        double tw = ui_text_width(wc_font, wd);
        ui_text_draw(cr, wc_font, x0 + d * CAL_CELL + (CAL_CELL - tw) / 2, y0 + CAL_CELL / 2 + 4, wd, CAL_CELL);
    }
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    int days = 31;
    while (days > 28) {
        struct tm t = first;
        t.tm_mday = days;
        mktime(&t);
        if (t.tm_mon == wc.cal_month) break;
        days--;
    }
    for (int day = 1; day <= days; day++) {
        int cell = first.tm_wday + day - 1;
        ui_rect c = { x0 + (cell % 7) * CAL_CELL, y0 + CAL_CELL * (1 + cell / 7), CAL_CELL, CAL_CELL };
        bool is_today = day == today.tm_mday && wc.cal_month == today.tm_mon && wc.cal_year == today.tm_year + 1900;
        if (is_today) ui_fill(cr, (ui_rect){ c.x + 2, c.y + 2, c.w - 4, c.h - 4 }, k->selection);
        char t[4];
        snprintf(t, sizeof t, "%d", day);
        ui_set_color(cr, is_today ? k->selection_text : k->text);
        double tw = ui_text_width(wc_font, t);
        ui_text_draw(cr, wc_font, c.x + (c.w - tw) / 2, c.y + (c.h + ui_font_ascent(wc_font)) / 2 - 1, t, c.w);
    }
}

static void human(double v, char *out, size_t n, const char *per)
{
    const char *u[] = { "B", "kB", "MB", "GB", "TB" };
    int i = 0;
    while (v >= 1000 && i < 4) { v /= 1000; i++; }
    snprintf(out, n, i && v < 10 ? "%.1f %s%s" : "%.0f %s%s", v, u[i], per);
}

static void pop_draw_meters(cairo_t *cr, ui_rect r)
{
    char lines[4][96], a[32], b[32];
    snprintf(lines[0], sizeof lines[0], "Processor: %.0f%%", wc.meter[0] * 100);
    human(wc.mem_used, a, sizeof a, "");
    human(wc.mem_total, b, sizeof b, "");
    snprintf(lines[1], sizeof lines[1], "Memory: %s of %s", a, b);
    human(wc.disk_rate, a, sizeof a, "/s");
    snprintf(lines[2], sizeof lines[2], "Disks: %s", a);
    human(wc.net_rate, a, sizeof a, "/s");
    snprintf(lines[3], sizeof lines[3], "Network: %s", a);
    ui_set_color(cr, ui_scheme_now.text);
    for (int i = 0; i < 4; i++) ui_text_draw(cr, wc_font, r.x + 12, r.y + 12 + (i + 1) * 20 - 4, lines[i], r.w - 20);
}

static void open_pop(int kind, uint32_t serial)
{
    if (wc.pop) { int was = wc.pop_kind; pop_free(); if (was == kind) return; }
    wc.pop_kind = kind;
    ui_rect a = part_rect(kind);
    double w, h;
    if (kind == P_CLOCK) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        wc.cal_month = tm.tm_mon;
        wc.cal_year = tm.tm_year + 1900;
        w = 7 * CAL_CELL + 16;
        h = 8 * CAL_CELL + 12;
    } else {
        w = 260;
        h = 4 * 20 + 24;
    }
    if (a.x + w > wc.w) a.x = wc.w - w;
    wc.pop = wc.top ? ui_popup_new(wc.app, wc.s, &pop_ops, NULL, a, w, h, serial)
                    : ui_popup_new_above(wc.app, wc.s, &pop_ops, NULL, a, w, h, serial);
    damage();
}

/* ---------------------------------------------------- the Desktop menu */

static void show_menu(void);

static void menu_read(objmenu *m, void *ctx)
{
    (void)ctx;
    /* the tree read now replaces the shown one */
    if (wc.ready) objmenu_free(wc.ready);
    wc.ready = m;
    wc.reading = NULL;
    if (wc.want_menu && !ui_menu_is_open()) show_menu();
    wc.want_menu = wc.want_menu && ui_menu_is_open();
}

static void build_start(void)
{
    if (wc.reading) return;   /* being read */
    wc.reading = objmenu_read(wc.desktop, 3, menu_read, NULL);
}

enum { M_WINLIST = 1, M_LOCK, M_SHUTDOWN, M_OPEN_DESKTOP, M_LOGOFF };

static void opened(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && cv_text(res, "folder")) { wc.open_folder(cv_text(res, "folder")); return; }
    if (svc_reply_view(reply)) return;
    if (!res && reply && cv_text(reply, "message")) fprintf(stderr, "wps-shell: WarpCenter: %s\n", cv_text(reply, "message"));
}

static void menu_chosen(void *arg, int64_t id)
{
    (void)arg;
    wc.want_menu = false;
    damage();
    char addr[PATH_MAX], title[256];
    bool folder;
    if (id == M_WINLIST) winlist_open(false);
    else if (id == M_LOCK) session_lock("warpcenter");
    else if (id == M_LOGOFF) session_ask_logoff(wc.app, "warpcenter");
    else if (id == M_SHUTDOWN) session_ask_end(wc.app, "warpcenter");
    else if (objmenu_choice(wc.ready, id, addr, sizeof addr, &folder, title, sizeof title)) {
        events_raise("object.open", title, "warpcenter", (ui_rect){ 0, 0, 0, 0 });
        if (folder) wc.open_folder(addr);
        else svc_open(addr, false, opened, NULL);
    }
}

static void show_menu(void)
{
    if (!wc.ready) return;
    size_t k = 0;
    ui_menu_item *it = objmenu_items(wc.ready, &k, false, 6);
    if (k) it[k++] = (ui_menu_item){ -1, strdup("-"), false, false, NULL, 0 };
    it[k++] = (ui_menu_item){ M_WINLIST, strdup("Window list"), true, false, NULL, 0 };
    it[k++] = (ui_menu_item){ M_LOCK, strdup("Lockup now"), true, false, NULL, 0 };
    it[k++] = (ui_menu_item){ M_LOGOFF, strdup("Log off..."), true, false, NULL, 0 };
    it[k++] = (ui_menu_item){ M_SHUTDOWN, strdup("Shut down..."), true, false, NULL, 0 };
    ui_rect r = part_rect(P_DESKTOP);
    ui_menu_popup_icons(wc.s, it, k, r.x, wc.top ? r.y + r.h : r.y, wc.menu_serial, menu_chosen, wc.ready, objmenu_icon);
    ui_menu_items_free(it, k);
    damage();
}

/* -------------------------------------------------------------- input */

static void press(int p, uint32_t serial, int button)
{
    switch (p) {
    case P_DESKTOP:
        /* read now (a few local requests), shown when read: the desktop
         * may have changed since */
        wc.want_menu = true;
        wc.menu_serial = serial;
        build_start();
        break;
    case P_WINDOWS: winlist_open(false); break;
    case P_KEYBOARD: winlist_ext("keyboard", button == 2 ? "prev" : "next"); break;
    case P_CLOCK: open_pop(P_CLOCK, serial); break;
    case P_METERS: open_pop(P_METERS, serial); break;
    case P_MESSAGES: if (wc.open_messages) wc.open_messages(); break;
    case P_LOCK: session_lock("warpcenter"); break;
    case P_FIND: {   /* WPS's own Find, from the home folder (Shift held: another) */
        const char *home = getenv("HOME");
        findwin_show(home && *home ? home : "/", false);
        break;
    }
    case P_LOGOFF: session_ask_logoff(wc.app, "warpcenter"); break;
    case P_SHUTDOWN: session_ask_end(wc.app, "warpcenter"); break;
    }
}

/* A press in any of the desktop's other windows (the desktop, folders)
 * closes the calendar or the meters' figures. */
static void any_press(ui_surface *s)
{
    if (wc.pop && s != wc.pop && s != wc.s) ui_app_post(wc.app, (void (*)(void *))pop_free, NULL);
}

/* A part's tooltip (v0.3-design 11): what it is, or its value now. */
static void part_tip(int p, double x, char *out, size_t n)
{
    out[0] = 0;
    switch (p) {
    case P_DESKTOP: snprintf(out, n, "The desktop's objects; Lockup, Log off, Shut down"); break;
    case P_WINDOWS: snprintf(out, n, "Window list"); break;
    case P_METERS:
        snprintf(out, n, "Processor %.0f%%, memory %.0f%%, disk %.0f%%, network %.0f%%", wc.meter[0] * 100, wc.meter[1] * 100,
                 wc.meter[2] * 100, wc.meter[3] * 100);
        break;
    case P_MESSAGES: snprintf(out, n, wc.unread ? "Messages: %zu unread" : "Messages", wc.unread); break;
    case P_KEYBOARD: snprintf(out, n, "Input source: %s (click: the next)", wc.key_label); break;
    case P_CLOCK: {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(out, n, "%A, %e %B %Y", &tm);
        break;
    }
    case P_TRAY: {
        ui_rect r = part_rect(P_TRAY);
        size_t i = (size_t)fmax(0, (x - r.x - 2) / tray_cell());
        const char *t = tray_title(i);
        snprintf(out, n, "%s", t ? t : "");
        break;
    }
    case P_LOCK: snprintf(out, n, "Lockup now"); break;
    case P_FIND: snprintf(out, n, "Find"); break;
    case P_LOGOFF: snprintf(out, n, "Log off"); break;
    case P_SHUTDOWN: snprintf(out, n, "Shut down"); break;
    default: break;
    }
}

static void wc_pointer(ui_surface *s, const ui_pointer_event *e)
{
    if (e->kind == UI_MOTION || e->kind == UI_ENTER) {
        int p = part_at(e->x, e->y);
        char tip[256];
        part_tip(p, e->x, tip, sizeof tip);
        ui_rect r = part_rect(p);
        if (p == P_TRAY) {   /* each tray icon its own */
            double c = tray_cell();
            r.x += 2 + floor((e->x - r.x - 2) / c) * c;
            r.w = c;
        }
        if (p == P_NONE || wc.pop) tooltip_hide();
        else tooltip_hover(s, r, tip, !wc.top);
    } else if (e->kind == UI_LEAVE || e->kind == UI_PRESS) {
        tooltip_hide();
    }
    if (e->kind == UI_PRESS) {
        int p = part_at(e->x, e->y);
        if (wc.pop && p != wc.pop_kind) pop_free();   /* another part, or the bar: it closes */
        if (p == P_NONE) return;
        if (p == P_DESKTOP || p == P_CLOCK || p == P_METERS) { press(p, e->serial, 1); return; }   /* (menus and pop-ups open on press) */
        if (p == P_TRAY) {   /* (its menus too) */
            ui_rect r = part_rect(P_TRAY);
            double c = tray_cell();
            size_t i = (size_t)fmax(0, (e->x - r.x - 2) / c);
            ui_rect at = { r.x + 2 + (double)i * c, wc.top ? r.y + r.h : r.y, 1, 1 };
            tray_press(i, e->button, wc.s, at, e->serial);
            return;
        }
        wc.pressed = p;
        damage();
    } else if (e->kind == UI_RELEASE && wc.pressed != P_NONE) {
        int p = wc.pressed;
        wc.pressed = P_NONE;
        damage();
        if (part_at(e->x, e->y) == p) press(p, e->serial, e->button == ui_mouse_button(UI_ACT_POPUP) ? 2 : 1);
    }
}

static void wc_resized(ui_surface *s, double w, double h)
{
    (void)s;
    wc.w = w;
    wc.h = h;
    damage();
}

static void wc_close(ui_surface *s) { (void)s; }   /* (it stays) */

/* A font from the Font Palette dropped on it: its own. */
static void (*on_font)(const char *spec);
void warpcenter_on_font(void (*fn)(const char *spec)) { on_font = fn; }

static uint32_t wc_drag_motion(ui_surface *s, double x, double y, const char *const *mimes, size_t n, uint32_t src_actions,
                               unsigned mods, const char **mime)
{
    (void)s; (void)x; (void)y; (void)src_actions; (void)mods;
    for (size_t i = 0; i < n; i++) if (!strcmp(mimes[i], "application/x-wps-font")) { *mime = mimes[i]; return UI_DND_COPY; }
    return 0;
}

static uint32_t wc_drop(ui_surface *s, double x, double y, const char *mime, const char *data, size_t len, uint32_t action,
                        uint32_t start_action, unsigned mods)
{
    (void)s; (void)x; (void)y; (void)len; (void)action; (void)start_action; (void)mods;
    if (strcmp(mime, "application/x-wps-font") || !on_font) return 0;
    char spec[128];
    snprintf(spec, sizeof spec, "%.*s", (int)strcspn(data, "\r\n"), data);
    if (!ui_font_valid(spec)) return 0;
    on_font(spec);
    return UI_DND_COPY;
}

static const ui_surface_ops wc_ops = { .draw = wc_draw, .pointer = wc_pointer, .resized = wc_resized, .close = wc_close,
                                       .drag_motion = wc_drag_motion, .drop = wc_drop };

void warpcenter_open(ui_app *a, const char *desktop, bool top, void (*open_folder)(const char *path))
{
    if (wc.s) return;
    wc.app = a;
    wc.top = top;
    wc.open_folder = open_folder;
    snprintf(wc.desktop, sizeof wc.desktop, "%s", desktop);
    read_clock();
    read_meters();
    wc.w = 1280;
    wc.h = bar_h();
    winlist_ext("panel", top ? "top" : "bottom");
    ui_press_hook = any_press;
    wc.s = ui_window_new(a, &wc_ops, NULL, "org.wps.WarpCenter", wc.w, wc.h);
    ui_window_set_frameless(wc.s, true);
    ui_window_set_title(wc.s, "WarpCenter");
    wc.timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (wc.timer >= 0) {
        struct itimerspec it = { { 1, 0 }, { 1, 0 } };
        timerfd_settime(wc.timer, 0, &it, NULL);
        ui_app_watch_fd(a, wc.timer, POLLIN, on_tick, NULL);
    }
    winlist_ext("keyboard", "report");
    build_start();
    tray_init(a, damage);   /* (the session's tray: only with the bar) */
}

void warpcenter_damage(void) { damage(); }

void warpcenter_set_font(ui_font f)
{
    wc_font = f;
    wc_bold = f == UI_FONT_REGULAR ? UI_FONT_BOLD : f;   /* (one of the palette's: as it is) */
    damage();
}

bool warpcenter_is_open(void) { return wc.s != NULL; }

bool warpcenter_press(const char *part)
{
    static const char *const names[NPARTS] = { "desktop", "windows", "meters", "messages", "keyboard", "clock", "tray",
                                               "lock", "find", "logoff", "shutdown" };
    if (!wc.s) return false;
    for (int p = 0; p < NPARTS; p++)
        if (!strcmp(part, names[p])) { press(p, ui_app_input_serial(wc.app), 1); return true; }
    return false;
}

size_t warpcenter_menu_labels(char *out, size_t n) { return objmenu_labels(wc.ready, out, n); }

/* ----------------------------------------------- the WarpCenter object */

void warpcenter_setting(const char *key, const char *val)
{
    bool on = !strcmp(val, "1");
    if (!strcmp(key, "edge")) {
        bool top = !strcmp(val, "top");
        if (top != wc.top) {
            wc.top = top;
            if (wc.pop) pop_free();
            if (wc.s) winlist_ext("panel", top ? "top" : "bottom");
        }
    } else if (!strcmp(key, "meters")) wc.shown[P_METERS] = on;
    else if (!strcmp(key, "tray")) wc.shown[P_TRAY] = on;
    else if (!strcmp(key, "keyboard")) wc.shown[P_KEYBOARD] = on;
    else if (!strcmp(key, "messages")) wc.shown[P_MESSAGES] = on;
    else if (!strcmp(key, "system_buttons")) for (int q = P_LOCK; q <= P_SHUTDOWN; q++) wc.shown[q] = on;
    else if (!strcmp(key, "clock")) wc.clock_mode = !strcmp(val, "12 hours") ? 1 : !strcmp(val, "off") ? 2 : 0;
    else if (!strcmp(key, "seconds")) wc.seconds = on;
    else if (!strcmp(key, "date")) wc.date = on;
    else if (!strcmp(key, "font")) warpcenter_set_font(ui_font_named(val));
    else return;
    if (wc.pop && !part_shown(wc.pop_kind)) pop_free();
    wc.clock[0] = 0;
    read_clock();
    damage();
}

bool warpcenter_top(void) { return wc.top; }

void warpcenter_messages(size_t unread, void (*open_messages)(void))
{
    wc.open_messages = open_messages;
    if (wc.unread == unread) return;
    wc.unread = unread;
    damage();
}

bool warpcenter_part_rect(const char *part, double *x, double *y, double *w, double *h)
{
    static const char *const names[NPARTS] = { "desktop", "windows", "meters", "messages", "keyboard", "clock", "tray",
                                               "lock", "find", "logoff", "shutdown" };
    for (int p = 0; p < NPARTS; p++) {
        if (strcmp(part, names[p])) continue;
        ui_rect r = part_rect(p);
        *x = r.x; *y = r.y; *w = r.w; *h = r.h;
        return wc.s && r.w > 0;
    }
    return false;
}

const char *warpcenter_clock_text(void) { return wc.clock; }
