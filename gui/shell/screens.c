/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * The Screens object (0.7 step 5, the owner's): GNOME's Settings, Displays
 * needs Mutter's own interface and cannot change the screens in the
 * Objectspace session, so the System Setup folder has its own. Its
 * notebook: an Arrangement page (a picture of the screens, dragged to
 * place them beside each other; the primary chosen) and a page per screen
 * (on or off, resolution, refresh rate, scale).
 *
 * The compositor says what screens there are (the M lines after S: each
 * one's modes, place and scale). A change is written to GNOME's
 * monitors.xml (the configuration for the screens connected now, first;
 * the others, for other sets of screens, kept), which the compositor
 * follows at once, so GNOME and Objectspace keep one setup. As OS/2 did,
 * a change to a mode, a scale or a screen on or off is asked to be kept:
 * unless Keep is chosen in 15 seconds, the file as it was is written back.
 *
 * Inside GNOME (no compositor of ours: no M lines), GNOME's Settings,
 * Displays opens instead.
 */
#define _GNU_SOURCE
#include "shell.h"

#include <math.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

struct mode { int w, h, mhz; };
struct scr {
    char name[64], make[128], model[128], serial[128];
    bool on, primary;
    int w, h, mhz;       /* the mode, in pixels */
    double scale;
    int x, y;            /* its place, in logical pixels */
    struct mode *modes;
    size_t nmodes;
};

static struct scr *live;   /* as the compositor says */
static size_t nlive;
static bool have_live, reading;

static struct scr *cur;    /* as the notebook has them (the next to write) */
static size_t ncur;

static ui_notebook *nb;
static ui_canvas canvas;
static int drag = -1;           /* the screen dragged in the picture */
static double drag_dx, drag_dy; /* (the pointer's offset in it, in the picture's pixels) */
static double pic_k, pic_x0, pic_y0;   /* the picture: logical pixels to its pixels */

static char *before;            /* monitors.xml before the change asked about (NULL: there was none) */
static bool before_had;
static ui_dialog *keep_dlg;
static int ask_fd = -1, ask_left;

static const double scales[] = { 1, 1.25, 1.5, 1.75, 2, 2.5, 3 };
#define NSCALES (sizeof scales / sizeof *scales)

/* ------------------------------------------------------------ the list */

static void free_list(struct scr *s, size_t n)
{
    for (size_t i = 0; i < n; i++) free(s[i].modes);
    free(s);
}

static struct scr *copy_list(const struct scr *s, size_t n)
{
    struct scr *c = calloc(n ? n : 1, sizeof *c);
    for (size_t i = 0; i < n; i++) {
        c[i] = s[i];
        c[i].modes = calloc(s[i].nmodes ? s[i].nmodes : 1, sizeof *c[i].modes);
        memcpy(c[i].modes, s[i].modes, s[i].nmodes * sizeof *s[i].modes);
    }
    return c;
}

static int lw(const struct scr *s) { return (int)lround(s->w / (s->scale > 0 ? s->scale : 1)); }
static int lh(const struct scr *s) { return (int)lround(s->h / (s->scale > 0 ? s->scale : 1)); }

static void rebuild_notebook(void);
void screens_lines_done(void);

/* "M\t-": the list anew; "M\tNAME\tON\tW\tH\tMHZ\tSCALE\tX\tY\tPRIMARY\tMAKE\tMODEL\tSERIAL\tMODES" */
void screens_line(char **f, int k)
{
    if (k >= 2 && !strcmp(f[1], "-")) {
        free_list(live, nlive);
        live = NULL;
        nlive = 0;
        reading = true;
        return;
    }
    if (k >= 2 && !strcmp(f[1], ".")) { screens_lines_done(); return; }
    if (k < 14 || !reading) return;
    live = realloc(live, (nlive + 1) * sizeof *live);
    struct scr *s = &live[nlive++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", f[1]);
    s->on = atoi(f[2]);
    s->w = atoi(f[3]);
    s->h = atoi(f[4]);
    s->mhz = atoi(f[5]);
    s->scale = atof(f[6]) > 0 ? atof(f[6]) : 1;
    s->x = atoi(f[7]);
    s->y = atoi(f[8]);
    s->primary = atoi(f[9]);
    snprintf(s->make, sizeof s->make, "%s", f[10]);
    snprintf(s->model, sizeof s->model, "%s", f[11]);
    snprintf(s->serial, sizeof s->serial, "%s", f[12]);
    for (char *p = f[13], *t; (t = strsep(&p, ","));) {
        struct mode m;
        if (sscanf(t, "%dx%d@%d", &m.w, &m.h, &m.mhz) != 3 || m.w <= 0 || m.h <= 0) continue;
        s->modes = realloc(s->modes, (s->nmodes + 1) * sizeof *s->modes);
        s->modes[s->nmodes++] = m;
    }
    have_live = true;
}

/* The M lines all read ("M\t."): the notebook as they are. */
void screens_lines_done(void)
{
    if (!reading) return;
    reading = false;
    if (!nb) return;
    if (drag >= 0) return;   /* (dragging: as it is) */
    bool same_set = ncur == nlive;
    for (size_t i = 0; same_set && i < nlive; i++) same_set = !strcmp(cur[i].name, live[i].name);
    if (!same_set) {   /* a screen came or went: the pages anew */
        ui_notebook_close(nb);
        return;
    }
    free_list(cur, ncur);
    cur = copy_list(live, nlive);
    ncur = nlive;
    rebuild_notebook();
}

/* ------------------------------------------------------------ arranging */

static bool overlaps(const struct scr *a, const struct scr *b)
{
    return a->x < b->x + lw(b) && b->x < a->x + lw(a) && a->y < b->y + lh(b) && b->y < a->y + lh(a);
}

static bool touches(const struct scr *a, const struct scr *b)
{
    bool side = (a->x + lw(a) == b->x || b->x + lw(b) == a->x) && a->y < b->y + lh(b) && b->y < a->y + lh(a);
    bool top = (a->y + lh(a) == b->y || b->y + lh(b) == a->y) && a->x < b->x + lw(b) && b->x < a->x + lw(a);
    return side || top;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Screen i moved to touch one of the screens in `placed` (beside or
 * above or below, sharing some edge), the nearest place to where it is,
 * overlapping none; edges near each other's aligned. False: no place. */
static bool snap(size_t i, const bool *placed)
{
    struct scr *s = &cur[i];
    int bx = s->x, by = s->y;
    long best = -1;
    int px = bx, py = by;
    for (size_t j = 0; j < ncur; j++) {
        if (j == i || !placed[j] || !cur[j].on) continue;
        const struct scr *o = &cur[j];
        int cand[4][2];
        int yy = clampi(by, o->y - lh(s) + 1, o->y + lh(o) - 1), xx = clampi(bx, o->x - lw(s) + 1, o->x + lw(o) - 1);
        if (abs(yy - o->y) < 48) yy = o->y;   /* (tops aligned when near) */
        else if (abs(yy + lh(s) - o->y - lh(o)) < 48) yy = o->y + lh(o) - lh(s);
        if (abs(xx - o->x) < 48) xx = o->x;
        else if (abs(xx + lw(s) - o->x - lw(o)) < 48) xx = o->x + lw(o) - lw(s);
        cand[0][0] = o->x + lw(o); cand[0][1] = yy;   /* right of it */
        cand[1][0] = o->x - lw(s); cand[1][1] = yy;   /* left */
        cand[2][0] = xx; cand[2][1] = o->y + lh(o);   /* below */
        cand[3][0] = xx; cand[3][1] = o->y - lh(s);   /* above */
        for (int c = 0; c < 4; c++) {
            struct scr t = *s;
            t.x = cand[c][0];
            t.y = cand[c][1];
            bool ok = true;
            for (size_t m = 0; m < ncur && ok; m++)
                if (m != i && placed[m] && cur[m].on && overlaps(&t, &cur[m])) ok = false;
            if (!ok) continue;
            long d = (long)(t.x - bx) * (t.x - bx) + (long)(t.y - by) * (t.y - by);
            if (best < 0 || d < best) { best = d; px = t.x; py = t.y; }
        }
    }
    if (best < 0) return false;
    s->x = px;
    s->y = py;
    return true;
}

/* All the screens on kept together: from the one furthest up and left,
 * each one touching one placed before it (moved to the nearest place
 * that does, when it does not: a screen made bigger, or smaller); the
 * whole at 0,0. `first`: the one placed first (the one dragged), or -1. */
static void tidy(int first)
{
    size_t n = ncur;
    bool *placed = calloc(n ? n : 1, sizeof *placed);
    size_t *order = calloc(n ? n : 1, sizeof *order);
    for (size_t i = 0; i < n; i++) order[i] = i;
    for (size_t a = 0; a < n; a++)   /* (by place: left to right, then down) */
        for (size_t b = a + 1; b < n; b++) {
            const struct scr *p = &cur[order[a]], *q = &cur[order[b]];
            if (q->x < p->x || (q->x == p->x && q->y < p->y)) { size_t t = order[a]; order[a] = order[b]; order[b] = t; }
        }
    bool any = false;
    if (first >= 0 && cur[first].on) { placed[first] = true; any = true; }
    for (size_t a = 0; a < n; a++) {
        size_t i = order[a];
        if (!cur[i].on || placed[i]) continue;
        if (!any) { placed[i] = any = true; continue; }
        bool ok = true, touching = false;
        for (size_t j = 0; j < n; j++) {
            if (!placed[j] || !cur[j].on) continue;
            if (overlaps(&cur[i], &cur[j])) ok = false;
            if (touches(&cur[i], &cur[j])) touching = true;
        }
        if (!ok || !touching) snap(i, placed);
        placed[i] = true;
    }
    int mx = INT32_MAX, my = INT32_MAX;
    for (size_t i = 0; i < n; i++)
        if (cur[i].on) { if (cur[i].x < mx) mx = cur[i].x; if (cur[i].y < my) my = cur[i].y; }
    if (mx != INT32_MAX)
        for (size_t i = 0; i < n; i++) if (cur[i].on) { cur[i].x -= mx; cur[i].y -= my; }
    free(placed);
    free(order);
}

/* ------------------------------------------------------------ monitors.xml */

static void xml_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/monitors.xml", xdg);
    else snprintf(out, n, "%s/.config/monitors.xml", home ? home : "");
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *b = malloc(cap);
    size_t r;
    while ((r = fread(b + len, 1, cap - len - 1, f)) > 0) {
        len += r;
        if (len + 1 >= cap) b = realloc(b, cap *= 2);
    }
    fclose(f);
    b[len] = 0;
    return b;
}

static bool write_all(const char *path, const char *text)
{
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.osd-new", path);
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0700); }
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* The text between <tag> and </tag> in [from, to), into out ("" none). */
static void tag_text(const char *from, const char *to, const char *tag, char *out, size_t n)
{
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    out[0] = 0;
    const char *a = strstr(from, open);
    if (!a || a >= to) return;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b || b > to) return;
    snprintf(out, n, "%.*s", (int)(b - a), a);
}

/* A screen's monitorspec: as monitors.xml names it already (GNOME's own
 * vendor, product and serial, from the screen's EDID), else as the
 * compositor does. */
static void monitorspec(const char *old, const struct scr *s, char *out, size_t n)
{
    char want[160];
    snprintf(want, sizeof want, "<connector>%s</connector>", s->name);
    const char *c = old ? strstr(old, want) : NULL;
    char vendor[128] = "", product[128] = "", serial[128] = "";
    if (c) {
        const char *end = strstr(c, "</monitorspec>");
        if (end) {
            tag_text(c, end, "vendor", vendor, sizeof vendor);
            tag_text(c, end, "product", product, sizeof product);
            tag_text(c, end, "serial", serial, sizeof serial);
        }
    }
    if (!vendor[0]) snprintf(vendor, sizeof vendor, "%s", strcmp(s->make, "-") ? s->make : "unknown");
    if (!product[0]) snprintf(product, sizeof product, "%s", strcmp(s->model, "-") ? s->model : "unknown");
    if (!serial[0]) snprintf(serial, sizeof serial, "%s", strcmp(s->serial, "-") ? s->serial : "unknown");
    snprintf(out, n,
             "<monitorspec>\n          <connector>%s</connector>\n          <vendor>%s</vendor>\n          <product>%s</product>\n"
             "          <serial>%s</serial>\n        </monitorspec>",
             s->name, vendor, product, serial);
}

/* Whether the configuration [conf, end) names exactly the screens there are. */
static bool conf_is_ours(const char *conf, const char *end)
{
    size_t named = 0;
    for (const char *c = strstr(conf, "<connector>"); c && c < end; c = strstr(c + 1, "<connector>")) {
        const char *e = strstr(c, "</connector>");
        if (!e || e > end) break;
        bool found = false;
        for (size_t i = 0; i < ncur; i++)
            if ((size_t)(e - c - 11) == strlen(cur[i].name) && !strncmp(c + 11, cur[i].name, strlen(cur[i].name))) found = true;
        if (!found) return false;
        named++;
    }
    return named == ncur;
}

static void append(char **b, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap *= 2;
        *b = realloc(*b, *cap);
    }
    memcpy(*b + *len, s, n + 1);
    *len += n;
}

/* monitors.xml with the screens as `cur` has them: their configuration
 * first, the others (for other sets of screens) after, as they were. */
static char *make_xml(const char *old)
{
    size_t cap = 8192, len = 0;
    char *b = malloc(cap);
    b[0] = 0;
    append(&b, &len, &cap, "<monitors version=\"2\">\n  <configuration>\n");
    bool fractional = false, logical = old && strstr(old, "<layoutmode>logical</layoutmode>");
    for (size_t i = 0; i < ncur; i++) if (cur[i].on && fabs(cur[i].scale - lround(cur[i].scale)) > 0.001) fractional = true;
    bool scaled = false;
    for (size_t i = 0; i < ncur; i++) if (cur[i].on && fabs(cur[i].scale - 1) > 0.001) scaled = true;
    if (logical || fractional || scaled) append(&b, &len, &cap, "    <layoutmode>logical</layoutmode>\n");   /* (places in logical pixels, as here) */
    char line[2048], spec[1024];
    for (size_t i = 0; i < ncur; i++) {
        const struct scr *s = &cur[i];
        if (!s->on) continue;
        monitorspec(old, s, spec, sizeof spec);
        snprintf(line, sizeof line,
                 "    <logicalmonitor>\n      <x>%d</x>\n      <y>%d</y>\n      <scale>%g</scale>\n%s"
                 "      <monitor>\n        %s\n        <mode>\n          <width>%d</width>\n          <height>%d</height>\n"
                 "          <rate>%.3f</rate>\n        </mode>\n      </monitor>\n    </logicalmonitor>\n",
                 s->x, s->y, s->scale, s->primary ? "      <primary>yes</primary>\n" : "", spec, s->w, s->h, s->mhz / 1000.0);
        append(&b, &len, &cap, line);
    }
    for (size_t i = 0; i < ncur; i++) {
        if (cur[i].on) continue;
        monitorspec(old, &cur[i], spec, sizeof spec);
        snprintf(line, sizeof line, "    <disabled>\n      %s\n    </disabled>\n", spec);
        append(&b, &len, &cap, line);
    }
    append(&b, &len, &cap, "  </configuration>\n");
    for (const char *conf = old ? strstr(old, "<configuration>") : NULL; conf; conf = strstr(conf + 1, "<configuration>")) {
        const char *end = strstr(conf, "</configuration>");
        if (!end) break;
        end += strlen("</configuration>");
        if (conf_is_ours(conf, end)) continue;   /* (ours: the one above) */
        append(&b, &len, &cap, "  ");
        size_t n = (size_t)(end - conf);
        char *part = strndup(conf, n);
        append(&b, &len, &cap, part);
        free(part);
        append(&b, &len, &cap, "\n");
    }
    append(&b, &len, &cap, "</monitors>\n");
    return b;
}

/* ------------------------------------------------------------ keep it? */

static void ask_stop(void)
{
    if (ask_fd >= 0) { ui_app_unwatch_fd(app, ask_fd); close(ask_fd); ask_fd = -1; }
}

static void put_back(void)
{
    char path[4096];
    xml_path(path, sizeof path);
    if (before_had) write_all(path, before);
    else unlink(path);
    free(before);
    before = NULL;
    events_raise("screens.back", "Screens", "screens", (ui_rect){ 0, 0, 0, 0 });
}

static void ask_message(char *out, size_t n)
{
    snprintf(out, n, "Keep these screen settings? Without an answer they go back as they were in %d second%s.", ask_left,
             ask_left == 1 ? "" : "s");
}

static void asked(void *ctx, int choice)
{
    (void)ctx;
    ask_stop();
    keep_dlg = NULL;
    if (choice == 0) {   /* Keep */
        free(before);
        before = NULL;
        events_raise("screens.kept", "Screens", "screens", (ui_rect){ 0, 0, 0, 0 });
    } else put_back();
}

static void on_ask_tick(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t n;
    if (read(fd, &n, sizeof n) != sizeof n) return;
    ask_left -= (int)n;
    if (!keep_dlg) { ask_stop(); return; }
    if (ask_left <= 0) { ui_dialog_choose(keep_dlg, 1); return; }
    char msg[256];
    ask_message(msg, sizeof msg);
    ui_dialog_set_message(keep_dlg, msg);
}

static void ask_keep(void)
{
    if (keep_dlg) {   /* asked already: the time from now */
        const char *e = getenv("OSD_SCREENS_KEEP_SECS");
        ask_left = e && atoi(e) > 0 ? atoi(e) : 15;
        return;
    }
    const char *e = getenv("OSD_SCREENS_KEEP_SECS");   /* (tests: shorter) */
    ask_left = e && atoi(e) > 0 ? atoi(e) : 15;
    char msg[256];
    ask_message(msg, sizeof msg);
    static const char *const b[] = { "Keep", "Go back" };
    keep_dlg = ui_dialog_open(app, "Screens", msg, b, 2, 0, asked, NULL);
    ask_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ask_fd >= 0) {
        struct itimerspec its = { .it_value = { 1, 0 }, .it_interval = { 1, 0 } };
        timerfd_settime(ask_fd, 0, &its, NULL);
        ui_app_watch_fd(app, ask_fd, POLLIN, on_ask_tick, NULL);
    }
}

/* `cur` written to monitors.xml (the compositor follows it); `risky` (a
 * mode, a scale, a screen on or off): asked to be kept. */
static void save(bool risky)
{
    char path[4096];
    xml_path(path, sizeof path);
    char *old = read_all(path);
    char *text = make_xml(old);
    if (risky && !keep_dlg) {   /* (the file before the first change asked about) */
        free(before);
        before = old ? strdup(old) : NULL;
        before_had = old != NULL;
    }
    bool ok = write_all(path, text);
    free(text);
    free(old);
    if (!ok) {
        if (nb) ui_notebook_set_status(nb, "monitors.xml could not be written.");
        return;
    }
    if (nb) ui_notebook_set_status(nb, "");
    events_raise("screens.saved", "Screens", "screens", (ui_rect){ 0, 0, 0, 0 });
    if (risky) ask_keep();
}

/* ------------------------------------------------------------ the picture */

static void picture_fit(ui_rect r)
{
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (size_t i = 0; i < ncur; i++) {
        if (!cur[i].on) continue;
        if (cur[i].x < x0) x0 = cur[i].x;
        if (cur[i].y < y0) y0 = cur[i].y;
        if (cur[i].x + lw(&cur[i]) > x1) x1 = cur[i].x + lw(&cur[i]);
        if (cur[i].y + lh(&cur[i]) > y1) y1 = cur[i].y + lh(&cur[i]);
    }
    if (x0 == INT32_MAX) { pic_k = 0.1; pic_x0 = r.x; pic_y0 = r.y; return; }
    /* (room round them to drag one beside the others: as wide as the widest) */
    double room = 0;
    for (size_t i = 0; i < ncur; i++) if (cur[i].on) room = fmax(room, fmax(lw(&cur[i]), lh(&cur[i])));
    double W = (x1 - x0) + room, H = (y1 - y0) + room;
    pic_k = fmin((r.w - 16) / W, (r.h - 16) / H);
    pic_x0 = r.x + (r.w - (x1 - x0) * pic_k) / 2 - x0 * pic_k;
    pic_y0 = r.y + (r.h - (y1 - y0) * pic_k) / 2 - y0 * pic_k;
}

static ui_rect screen_rect(size_t i)
{
    return (ui_rect){ pic_x0 + cur[i].x * pic_k, pic_y0 + cur[i].y * pic_k, lw(&cur[i]) * pic_k, lh(&cur[i]) * pic_k };
}

static double drag_px, drag_py;   /* (the dragged one's top left, in the picture) */

static void canvas_draw(void *ctx, cairo_t *cr, ui_rect r, bool focused)
{
    (void)ctx; (void)focused;
    const ui_scheme *k = &ui_scheme_now;
    ui_fill(cr, r, k->client_bg);
    ui_bevel(cr, r, false);
    if (drag < 0) picture_fit(r);
    for (size_t i = 0; i < ncur; i++) {
        if (!cur[i].on) continue;
        ui_rect s = screen_rect(i);
        if ((int)i == drag) { s.x = drag_px; s.y = drag_py; }
        ui_rect in = { s.x + 1, s.y + 1, s.w - 2, s.h - 2 };
        ui_fill(cr, in, cur[i].primary ? k->selection : k->face);
        ui_bevel(cr, in, true);
        ui_set_color(cr, cur[i].primary ? k->selection_text : k->text);
        char t[160];
        snprintf(t, sizeof t, "%zu", i + 1);
        double tw = ui_text_width(UI_FONT_DIALOG_BOLD, t);
        ui_text_draw(cr, UI_FONT_DIALOG_BOLD, s.x + (s.w - tw) / 2, s.y + s.h / 2 - 4, t, s.w - 8);
        snprintf(t, sizeof t, "%s", cur[i].name);   /* (the primary: in the selection's colour) */
        tw = fmin(ui_text_width(UI_FONT_DIALOG, t), s.w - 8);
        ui_text_draw(cr, UI_FONT_DIALOG, s.x + (s.w - tw) / 2, s.y + s.h / 2 + 14, t, s.w - 8);
    }
}

static bool canvas_pointer(void *ctx, const ui_pointer_event *e, ui_rect r)
{
    (void)ctx;
    if (e->kind == UI_PRESS) {
        picture_fit(r);
        drag = -1;
        for (size_t i = 0; i < ncur; i++) {
            if (!cur[i].on) continue;
            ui_rect s = screen_rect(i);
            if (ui_rect_has(s, e->x, e->y)) {
                drag = (int)i;
                drag_dx = e->x - s.x;
                drag_dy = e->y - s.y;
                drag_px = s.x;
                drag_py = s.y;
            }
        }
        return drag >= 0;
    }
    if (drag < 0) return false;
    drag_px = e->x - drag_dx;
    drag_py = e->y - drag_dy;
    if (e->kind == UI_MOTION) return true;
    /* let go: there, beside the nearest */
    int was_x = cur[drag].x, was_y = cur[drag].y;
    int *px = calloc(ncur, sizeof *px), *py = calloc(ncur, sizeof *py);
    for (size_t i = 0; i < ncur; i++) { px[i] = cur[i].x; py[i] = cur[i].y; }
    cur[drag].x = (int)lround((drag_px - pic_x0) / pic_k);
    cur[drag].y = (int)lround((drag_py - pic_y0) / pic_k);
    bool *others = calloc(ncur, sizeof *others);
    for (size_t i = 0; i < ncur; i++) others[i] = (int)i != drag && cur[i].on;
    bool alone = true;
    for (size_t i = 0; i < ncur; i++) if (others[i]) alone = false;
    if (!alone && !snap((size_t)drag, others)) { cur[drag].x = was_x; cur[drag].y = was_y; }
    free(others);
    drag = -1;
    tidy(-1);
    bool moved = false;
    for (size_t i = 0; i < ncur; i++) if (cur[i].x != px[i] || cur[i].y != py[i]) moved = true;
    free(px);
    free(py);
    if (moved) save(false);
    return true;
}

static bool canvas_key(void *ctx, const ui_key_event *e)
{
    (void)ctx; (void)e;
    return false;
}

/* ------------------------------------------------------------ the notebook */

/* The sizes a screen has (each once, the largest first), and the rates of one. */
static size_t sizes_of(const struct scr *s, struct mode *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < s->nmodes; i++) {
        bool seen = false;
        for (size_t j = 0; j < n; j++) if (out[j].w == s->modes[i].w && out[j].h == s->modes[i].h) seen = true;
        if (!seen && n < max) out[n++] = s->modes[i];
    }
    for (size_t a = 0; a < n; a++)
        for (size_t b = a + 1; b < n; b++)
            if ((long)out[b].w * out[b].h > (long)out[a].w * out[a].h) { struct mode t = out[a]; out[a] = out[b]; out[b] = t; }
    return n;
}

static size_t rates_of(const struct scr *s, int w, int h, int *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < s->nmodes; i++)
        if (s->modes[i].w == w && s->modes[i].h == h && n < max) {
            bool seen = false;
            for (size_t j = 0; j < n; j++) if (abs(out[j] - s->modes[i].mhz) < 10) seen = true;
            if (!seen) out[n++] = s->modes[i].mhz;
        }
    for (size_t a = 0; a < n; a++)
        for (size_t b = a + 1; b < n; b++) if (out[b] > out[a]) { int t = out[a]; out[a] = out[b]; out[b] = t; }
    return n;
}

#define MAXM 64
struct built {   /* the pages' texts, kept while the notebook has them */
    ui_page_desc *pages;
    ui_field_desc *fields;
    char **strs;
    size_t nstrs;
    const char ***lists;
    size_t nlists;
};
static struct built bt;

static void built_free(void)
{
    for (size_t i = 0; i < bt.nstrs; i++) free(bt.strs[i]);
    for (size_t i = 0; i < bt.nlists; i++) free(bt.lists[i]);
    free(bt.strs);
    free(bt.lists);
    free(bt.pages);
    free(bt.fields);
    memset(&bt, 0, sizeof bt);
}

static const char *keep_str(const char *s)
{
    bt.strs = realloc(bt.strs, (bt.nstrs + 1) * sizeof *bt.strs);
    return bt.strs[bt.nstrs++] = strdup(s);
}

static const char **keep_list(size_t n)
{
    bt.lists = realloc(bt.lists, (bt.nlists + 1) * sizeof *bt.lists);
    return (const char **)(bt.lists[bt.nlists++] = (const char **)calloc(n ? n : 1, sizeof(char *)));
}

static size_t page_count(void) { return 1 + ncur; }

/* The pages from `cur`: Arrangement, then one per screen. */
static void build_pages(void)
{
    built_free();
    size_t np = page_count();
    bt.pages = calloc(np, sizeof *bt.pages);
    bt.fields = calloc(np * 6, sizeof *bt.fields);
    /* Arrangement: the picture, the primary */
    ui_field_desc *f = bt.fields;
    canvas = (ui_canvas){ 260, canvas_draw, canvas_pointer, canvas_key, "the screens' arrangement: drag a screen to move it", NULL };
    f[0] = (ui_field_desc){ .key = "arrangement", .label = "", .kind = UI_FIELD_CANVAS, .canvas = &canvas };
    const char **names = keep_list(ncur);
    size_t on = 0, prim = 0;
    for (size_t i = 0; i < ncur; i++) {
        if (!cur[i].on) continue;
        char t[200];
        snprintf(t, sizeof t, "%zu: %s", i + 1, cur[i].name);
        if (cur[i].primary) prim = on;
        names[on++] = keep_str(t);
    }
    f[1] = (ui_field_desc){ .key = "primary", .label = "Primary screen (TheBar's)", .kind = UI_FIELD_CHOICE,
                            .value = { .number = (int64_t)prim }, .choices = names, .nchoices = on };
    bt.pages[0] = (ui_page_desc){ "Arrangement", f, 2 };
    for (size_t i = 0; i < ncur; i++) {
        const struct scr *s = &cur[i];
        f = bt.fields + (i + 1) * 6;
        char key[96], t[300];
        snprintf(t, sizeof t, "%s%s%s", strcmp(s->make, "-") ? s->make : "", strcmp(s->model, "-") ? " " : "", strcmp(s->model, "-") ? s->model : "");
        f[0] = (ui_field_desc){ .key = keep_str("monitor"), .label = "Monitor", .kind = UI_FIELD_INFO,
                                .value = { .text = (char *)keep_str(t[0] ? t : "(unknown)") } };
        snprintf(key, sizeof key, "on:%zu", i);
        f[1] = (ui_field_desc){ .key = keep_str(key), .label = "On", .kind = UI_FIELD_CHECKBOX, .value = { .checked = s->on } };
        struct mode sz[MAXM];
        size_t nsz = sizes_of(s, sz, MAXM), at = 0;
        const char **sl = keep_list(nsz + 1);
        for (size_t j = 0; j < nsz; j++) {
            snprintf(t, sizeof t, "%d × %d", sz[j].w, sz[j].h);
            sl[j] = keep_str(t);
            if (sz[j].w == s->w && sz[j].h == s->h) at = j;
        }
        if (!nsz) { snprintf(t, sizeof t, "%d × %d", s->w, s->h); sl[nsz++] = keep_str(t); }
        snprintf(key, sizeof key, "size:%zu", i);
        f[2] = (ui_field_desc){ .key = keep_str(key), .label = "Resolution", .kind = UI_FIELD_CHOICE, .value = { .number = (int64_t)at },
                                .choices = sl, .nchoices = nsz };
        int rates[MAXM];
        size_t nr = rates_of(s, s->w, s->h, rates, MAXM), rat = 0;
        const char **rl = keep_list(nr + 1);
        for (size_t j = 0; j < nr; j++) {
            snprintf(t, sizeof t, "%.2f Hz", rates[j] / 1000.0);
            rl[j] = keep_str(t);
            if (abs(rates[j] - s->mhz) < abs(rates[rat] - s->mhz)) rat = j;
        }
        if (!nr) { snprintf(t, sizeof t, "%.2f Hz", s->mhz / 1000.0); rl[nr++] = keep_str(t); }
        snprintf(key, sizeof key, "rate:%zu", i);
        f[3] = (ui_field_desc){ .key = keep_str(key), .label = "Refresh rate", .kind = UI_FIELD_CHOICE, .value = { .number = (int64_t)rat },
                                .choices = rl, .nchoices = nr };
        const char **cl = keep_list(NSCALES);
        size_t sat = 0;
        for (size_t j = 0; j < NSCALES; j++) {
            snprintf(t, sizeof t, "%g %%", scales[j] * 100);
            cl[j] = keep_str(t);
            if (fabs(scales[j] - s->scale) < fabs(scales[sat] - s->scale)) sat = j;
        }
        snprintf(key, sizeof key, "scale:%zu", i);
        f[4] = (ui_field_desc){ .key = keep_str(key), .label = "Scale", .kind = UI_FIELD_CHOICE, .value = { .number = (int64_t)sat },
                                .choices = cl, .nchoices = NSCALES };
        snprintf(t, sizeof t, "%zu: %s", i + 1, s->name);
        bt.pages[i + 1] = (ui_page_desc){ keep_str(t), f, 5 };
    }
}

static void rebuild_notebook(void)
{
    if (!nb) return;
    build_pages();
    ui_notebook_update(nb, bt.pages, page_count());
}

static void set_field(void *ctx, const char *key, ui_field_kind kind, const ui_value *v)
{
    (void)ctx; (void)kind;
    if (!strcmp(key, "primary")) {
        size_t on = 0;
        for (size_t i = 0; i < ncur; i++) {
            if (!cur[i].on) continue;
            cur[i].primary = (int64_t)on++ == v->number;
        }
        save(false);
        rebuild_notebook();
        return;
    }
    const char *colon = strchr(key, ':');
    if (!colon) return;
    size_t i = (size_t)atoi(colon + 1);
    if (i >= ncur) return;
    struct scr *s = &cur[i];
    if (!strncmp(key, "on:", 3)) {
        int on = 0;
        for (size_t j = 0; j < ncur; j++) on += cur[j].on;
        if (!v->checked && on <= 1) {   /* (the last one stays on) */
            ui_notebook_set_status(nb, "One screen at least stays on.");
            rebuild_notebook();
            return;
        }
        s->on = v->checked;
        if (s->on) {   /* back on: at the right of the others */
            int right = 0;
            for (size_t j = 0; j < ncur; j++) if (j != i && cur[j].on && cur[j].x + lw(&cur[j]) > right) right = cur[j].x + lw(&cur[j]);
            s->x = right;
            s->y = 0;
            if (s->w <= 0 && s->nmodes) { s->w = s->modes[0].w; s->h = s->modes[0].h; s->mhz = s->modes[0].mhz; }
        }
        if (!s->on && s->primary) {   /* the primary off: the first on one instead */
            s->primary = false;
            for (size_t j = 0; j < ncur; j++) if (cur[j].on) { cur[j].primary = true; break; }
        }
    } else if (!strncmp(key, "size:", 5)) {
        struct mode sz[MAXM];
        size_t nsz = sizes_of(s, sz, MAXM);
        if (v->number < 0 || (size_t)v->number >= nsz) return;
        s->w = sz[v->number].w;
        s->h = sz[v->number].h;
        int rates[MAXM];
        size_t nr = rates_of(s, s->w, s->h, rates, MAXM), best = 0;
        for (size_t j = 0; j < nr; j++) if (abs(rates[j] - s->mhz) < abs(rates[best] - s->mhz)) best = j;   /* (the rate nearest the one it had) */
        if (nr) s->mhz = rates[best];
    } else if (!strncmp(key, "rate:", 5)) {
        int rates[MAXM];
        size_t nr = rates_of(s, s->w, s->h, rates, MAXM);
        if (v->number < 0 || (size_t)v->number >= nr) return;
        s->mhz = rates[v->number];
    } else if (!strncmp(key, "scale:", 6)) {
        if (v->number < 0 || (size_t)v->number >= NSCALES) return;
        s->scale = scales[v->number];
    } else return;
    tidy(-1);
    save(true);
    rebuild_notebook();
}

static void closed(void *ctx)
{
    (void)ctx;
    nb = NULL;
    drag = -1;
    built_free();
    events_raise("window.close", "Screens", "window", (ui_rect){ 0, 0, 0, 0 });
}

/* GNOME's Settings, Displays (inside GNOME, where it works). */
static void open_gnome_displays(void)
{
    char *argv[] = { "gnome-control-center", "display", NULL };
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0)
        desktop_say("GNOME's Settings could not be started (gnome-control-center).", false);
}

void screens_open(void)
{
    if (nb) { ui_window_activate(ui_notebook_surface(nb)); return; }
    if (!have_live) { open_gnome_displays(); return; }
    free_list(cur, ncur);
    cur = copy_list(live, nlive);
    ncur = nlive;
    build_pages();
    nb = ui_notebook_open(app, "Screens - Properties", bt.pages, page_count(), set_field, closed, NULL);
    events_raise("window.open", "Screens", "window", (ui_rect){ 0, 0, 0, 0 });
}

bool screens_is_open(void) { return nb != NULL; }
bool screens_asking(void) { return keep_dlg != NULL; }
