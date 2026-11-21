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
#include "svc.h"
#include "winlist.h"

#define PAD 3
#define BTN_W 34
#define KEY_W 40
#define METER_W 9
#define NMETERS 4

enum part { P_NONE = -1, P_DESKTOP, P_WINDOWS, P_METERS, P_KEYBOARD, P_CLOCK, NPARTS };

/* the Desktop menu, read ahead (the menu widget takes whole trees) */
struct mnode {
    char title[256], addr[PATH_MAX];
    bool folder;
    struct mnode *kids;
    size_t nkids;
};

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
    /* clock */
    char clock[16];
    int timer;
    /* meters: 0..1 each */
    double meter[NMETERS];
    unsigned long long cpu_total, cpu_idle, disk, net;
    double disk_peak, net_peak, disk_rate, net_rate, mem_used, mem_total;
    int ticks;
    /* the menu */
    struct mnode root, *ready;     /* ready: the last tree read whole */
    int pending;
    bool want_menu;
    uint32_t menu_serial;
    struct mnode **flat;           /* by menu ID - 1000 */
    size_t nflat;
    /* the calendar or the meters' pop-up */
    ui_surface *pop;
    int pop_kind;                  /* P_CLOCK or P_METERS */
    int cal_month, cal_year;
} wc = { .pressed = P_NONE, .timer = -1 };

/* ------------------------------------------------------------- layout */

static double bar_h(void) { return ceil(ui_scheme_now.font_px * 1.9) + 2 * PAD; }

static double clock_w(void) { return ui_text_width(UI_FONT_REGULAR, "88:88") + 20; }

static ui_rect part_rect(int p)
{
    double h = wc.h - 2 * PAD, x;
    switch (p) {
    case P_DESKTOP: return (ui_rect){ PAD, PAD, BTN_W, h };
    case P_WINDOWS: return (ui_rect){ PAD + BTN_W + 2, PAD, BTN_W, h };
    case P_CLOCK: return (ui_rect){ wc.w - PAD - clock_w(), PAD, clock_w(), h };
    case P_KEYBOARD: x = wc.w - PAD - clock_w() - 4 - KEY_W; return (ui_rect){ x, PAD, KEY_W, h };
    case P_METERS: {
        double mw = NMETERS * (METER_W + 3) + 7;
        x = wc.w - PAD - clock_w() - 4 - KEY_W - 4 - mw;
        return (ui_rect){ x, PAD, mw, h };
    }
    }
    return (ui_rect){ 0, 0, 0, 0 };
}

static int part_at(double x, double y)
{
    for (int p = 0; p < NPARTS; p++) {
        ui_rect r = part_rect(p);
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

static void centred_text(cairo_t *cr, ui_rect r, const char *t, double d)
{
    double tw = ui_text_width(UI_FONT_REGULAR, t);
    ui_set_color(cr, ui_scheme_now.text);
    ui_text_draw(cr, UI_FONT_REGULAR, r.x + (r.w - tw) / 2 + d, r.y + (r.h + ui_text_ascent()) / 2 - 1 + d, t, r.w);
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
        bool down = wc.pressed == p || (p == P_DESKTOP && ui_menu_is_open() && wc.want_menu) ||
                    ((p == P_CLOCK || p == P_METERS) && wc.pop && wc.pop_kind == p);
        double d = down ? 1 : 0;
        if (p == P_DESKTOP || p == P_WINDOWS) {
            ui_bevel(cr, r, !down);
            cairo_save(cr);
            cairo_translate(cr, d, d);
            if (p == P_DESKTOP) glyph_desktop(cr, r);
            else glyph_windows(cr, r);
            cairo_restore(cr);
        } else if (p == P_KEYBOARD) {
            ui_bevel(cr, r, down);
            centred_text(cr, r, wc.key_label[0] ? wc.key_label : "--", d);
        } else if (p == P_CLOCK) {
            ui_bevel(cr, r, false);
            centred_text(cr, r, wc.clock, d);
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
    char t[16];
    strftime(t, sizeof t, "%H:%M", &tm);
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
        double tw = ui_text_width(UI_FONT_REGULAR, wd);
        ui_text_draw(cr, UI_FONT_REGULAR, x0 + d * CAL_CELL + (CAL_CELL - tw) / 2, y0 + CAL_CELL / 2 + 4, wd, CAL_CELL);
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
        double tw = ui_text_width(UI_FONT_REGULAR, t);
        ui_text_draw(cr, UI_FONT_REGULAR, c.x + (c.w - tw) / 2, c.y + (c.h + ui_text_ascent()) / 2 - 1, t, c.w);
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
    for (int i = 0; i < 4; i++) ui_text_draw(cr, UI_FONT_REGULAR, r.x + 12, r.y + 12 + (i + 1) * 20 - 4, lines[i], r.w - 20);
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

#define MAX_NODES 800
#define MAX_DEPTH 3

static void free_tree(struct mnode *n)
{
    for (size_t i = 0; i < n->nkids; i++) free_tree(&n->kids[i]);
    free(n->kids);
    n->kids = NULL;
    n->nkids = 0;
}

static size_t count_nodes(const struct mnode *n)
{
    size_t c = 1;
    for (size_t i = 0; i < n->nkids; i++) c += count_nodes(&n->kids[i]);
    return c;
}

struct fetch { struct mnode *node; int depth; char folder[PATH_MAX]; };

static void read_folder(struct mnode *node, const char *folder, int depth);

static int by_title(const void *a, const void *b)
{
    const struct mnode *x = a, *y = b;
    if (x->folder != y->folder) return x->folder ? -1 : 1;   /* folders first, as OS/2's menus */
    return strcasecmp(x->title, y->title);
}

static void build_done(void);

static void got_folder(void *arg, const cv *reply)
{
    struct fetch *f = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL, *items = res ? cv_get(res, "items") : NULL;
    struct mnode *node = f->node;
    size_t total = count_nodes(&wc.root);
    for (size_t i = 0; items && items->k == CV_ARRAY && i < items->n && total < MAX_NODES; i++, total++) {
        const cv *it = &items->items[i];
        const char *nm = cv_text(it, "name"), *ti = cv_text(it, "title"), *ty = cv_text(it, "type");
        const char *tt = cv_text(it, "target"), *tp = cv_text(it, "target_path");
        if (!nm || nm[0] == '.') continue;
        const cv *sh = cv_get(it, "shadow");
        bool shadow = sh && sh->k == CV_BOOL && sh->b, link = ty && !strcmp(ty, "link");
        node->kids = realloc(node->kids, (node->nkids + 1) * sizeof *node->kids);
        struct mnode *k = &node->kids[node->nkids++];
        memset(k, 0, sizeof *k);
        if (nm[0] == '\x01') snprintf(k->addr, sizeof k->addr, "%s", nm + 1);   /* a central object */
        else snprintf(k->addr, sizeof k->addr, "%s/%s", strcmp(f->folder, "/") ? f->folder : "", nm);
        k->folder = ty && !strcmp(ty, "folder");
        if ((shadow || link) && tt && !strcmp(tt, "folder") && tp && *tp) {   /* opens as the folder it shows */
            k->folder = true;
            snprintf(k->addr, sizeof k->addr, "%s", tp);
        }
        const char *t = ti && *ti ? ti : nm[0] == '\x01' ? "" : nm;
        if (!*t && tp) t = strrchr(tp, '/') ? strrchr(tp, '/') + 1 : tp;
        snprintf(k->title, sizeof k->title, "%s", *t ? t : "?");
    }
    if (node->nkids) qsort(node->kids, node->nkids, sizeof *node->kids, by_title);
    /* (kids are final now: their addresses stay put) */
    for (size_t i = 0; i < node->nkids && f->depth + 1 < MAX_DEPTH; i++)
        if (node->kids[i].folder) read_folder(&node->kids[i], node->kids[i].addr, f->depth + 1);
    free(f);
    if (--wc.pending == 0) build_done();
}

static void read_folder(struct mnode *node, const char *folder, int depth)
{
    struct fetch *f = calloc(1, sizeof *f);
    f->node = node;
    f->depth = depth;
    snprintf(f->folder, sizeof f->folder, "%s", folder);
    wc.pending++;
    svc_list(folder, 0, 300, true, got_folder, f);
}

static struct mnode *tree_copy;

static void build_start(void)
{
    if (wc.pending) return;   /* being read */
    free_tree(&wc.root);
    memset(&wc.root, 0, sizeof wc.root);
    snprintf(wc.root.addr, sizeof wc.root.addr, "%s", wc.desktop);
    wc.root.folder = true;
    read_folder(&wc.root, wc.desktop, 0);
}

static void show_menu(void);

static void build_done(void)
{
    /* the tree read now replaces the shown one */
    if (tree_copy) { free_tree(tree_copy); free(tree_copy); }
    tree_copy = malloc(sizeof *tree_copy);
    *tree_copy = wc.root;   /* (takes the kids) */
    memset(&wc.root, 0, sizeof wc.root);
    wc.ready = tree_copy;
    if (wc.want_menu && !ui_menu_is_open()) show_menu();
    wc.want_menu = wc.want_menu && ui_menu_is_open();
}

enum { M_WINLIST = 1, M_LOCK, M_SHUTDOWN, M_OPEN_DESKTOP, M_FIRST = 1000 };

static ui_menu_item *to_items(struct mnode *n, size_t *count, bool top)
{
    size_t cap = n->nkids + 8, k = 0;
    ui_menu_item *it = calloc(cap, sizeof *it);
    if (!top) {   /* a folder's cascade: the folder itself first */
        wc.flat = realloc(wc.flat, (wc.nflat + 1) * sizeof *wc.flat);
        wc.flat[wc.nflat] = n;
        it[k++] = (ui_menu_item){ M_FIRST + (int64_t)wc.nflat++, strdup("Open"), true, false, NULL, 0 };
        if (n->nkids) it[k++] = (ui_menu_item){ -1, strdup("-"), false, false, NULL, 0 };
    }
    for (size_t i = 0; i < n->nkids; i++) {
        struct mnode *c = &n->kids[i];
        if (c->folder && c->nkids) {
            size_t sub = 0;
            ui_menu_item *si = to_items(c, &sub, false);
            it[k++] = (ui_menu_item){ 0, strdup(c->title), true, false, si, sub };
        } else {
            wc.flat = realloc(wc.flat, (wc.nflat + 1) * sizeof *wc.flat);
            wc.flat[wc.nflat] = c;
            it[k++] = (ui_menu_item){ M_FIRST + (int64_t)wc.nflat++, strdup(c->title), true, false, NULL, 0 };
        }
    }
    if (top) {
        if (k) it[k++] = (ui_menu_item){ -1, strdup("-"), false, false, NULL, 0 };
        it[k++] = (ui_menu_item){ M_WINLIST, strdup("Window list"), true, false, NULL, 0 };
        it[k++] = (ui_menu_item){ M_LOCK, strdup("Lockup now"), true, false, NULL, 0 };
        it[k++] = (ui_menu_item){ M_SHUTDOWN, strdup("Shut down..."), true, false, NULL, 0 };
    }
    *count = k;
    return it;
}

static void opened(void *arg, const cv *reply)
{
    (void)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (res && cv_text(res, "folder")) { wc.open_folder(cv_text(res, "folder")); return; }
    if (res && cv_text(res, "view") && !strcmp(cv_text(res, "view"), "minimized-windows")) { winlist_open(true); return; }
    if (!res && reply && cv_text(reply, "message")) fprintf(stderr, "wps-shell: WarpCenter: %s\n", cv_text(reply, "message"));
}

static const char *const end_how[] = { "poweroff", "reboot", "logout" };

static void end_chosen(void *ctx, int choice)
{
    (void)ctx;
    if (choice < 0 || choice > 2) return;
    events_raise("session.end", end_how[choice], "warpcenter", (ui_rect){ 0, 0, 0, 0 });
    winlist_ext("shutdown", end_how[choice]);
}

static void menu_chosen(void *arg, int64_t id)
{
    (void)arg;
    wc.want_menu = false;
    damage();
    if (id == M_WINLIST) winlist_open(false);
    else if (id == M_LOCK) winlist_ext("lock", NULL);
    else if (id == M_SHUTDOWN) {
        static const char *const buttons[] = { "Shut down", "Restart", "Log off", "Cancel" };
        ui_dialog_open(wc.app, "Shut Down", "End this session: shut the computer down, restart it, or log off?", buttons, 4, 0,
                       end_chosen, NULL);
    } else if (id >= M_FIRST && (size_t)(id - M_FIRST) < wc.nflat) {
        struct mnode *n = wc.flat[id - M_FIRST];
        events_raise("object.open", n->title, "warpcenter", (ui_rect){ 0, 0, 0, 0 });
        if (n->folder && n->addr[0] == '/') wc.open_folder(n->addr);
        else svc_open(n->addr, false, opened, NULL);
    }
}

static void show_menu(void)
{
    if (!wc.ready) return;
    free(wc.flat);
    wc.flat = NULL;
    wc.nflat = 0;
    size_t n = 0;
    ui_menu_item *items = to_items(wc.ready, &n, true);
    ui_rect r = part_rect(P_DESKTOP);
    ui_menu_popup(wc.s, items, n, r.x, wc.top ? r.y + r.h : r.y, wc.menu_serial, menu_chosen, NULL);
    ui_menu_items_free(items, n);
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
    }
}

static void wc_pointer(ui_surface *s, const ui_pointer_event *e)
{
    (void)s;
    if (e->kind == UI_PRESS) {
        int p = part_at(e->x, e->y);
        if (p == P_NONE) return;
        if (p == P_DESKTOP || p == P_CLOCK || p == P_METERS) { press(p, e->serial, 1); return; }   /* (menus and pop-ups open on press) */
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

static const ui_surface_ops wc_ops = { .draw = wc_draw, .pointer = wc_pointer, .resized = wc_resized, .close = wc_close };

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
}

bool warpcenter_is_open(void) { return wc.s != NULL; }

bool warpcenter_press(const char *part)
{
    static const char *const names[NPARTS] = { "desktop", "windows", "meters", "keyboard", "clock" };
    if (!wc.s) return false;
    for (int p = 0; p < NPARTS; p++)
        if (!strcmp(part, names[p])) { press(p, ui_app_input_serial(wc.app), 1); return true; }
    return false;
}

size_t warpcenter_menu_labels(char *out, size_t n)
{
    size_t o = 0, c = 0;
    out[0] = 0;
    for (size_t i = 0; wc.ready && i < wc.ready->nkids; i++, c++)
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s%s", i ? ", " : "", wc.ready->kids[i].title,
                              wc.ready->kids[i].folder && wc.ready->kids[i].nkids ? " >" : "");
    return c;
}
