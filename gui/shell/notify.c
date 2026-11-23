/* The notification service (notify.h). */
#define _GNU_SOURCE
#include "notify.h"

#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/timerfd.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "appicons.h"
#include "events.h"
#include "svc.h"

#define NPATH "/org/freedesktop/Notifications"
#define NIFACE "org.freedesktop.Notifications"
#define MAX_ACTIONS 16          /* key, label pairs: 8 buttons */
#define MAX_NOTES 200           /* active (not closed) notifications */
#define MAX_SHOWN 4             /* pop-ups at once */
#define POP_W 360
#define PAD 8
#define ICON 32
#define BTN_H 24
#define BODY_LINES 4

struct note {
    uint32_t nid;
    char sender[96];            /* "" for WPS's own */
    char app[128], icon[1024], summary[256], body[2048];
    char *actions[MAX_ACTIONS];
    size_t nact;
    int urgency;
    bool transient, resident;
    double deadline;            /* when its pop-up goes (0: when closed) */
    double shown_at;            /* 0: no pop-up */
    int64_t msg;                /* its message in wpsd, once known */
};

static ui_app *app;
static sd_bus *bus;
static struct note *notes;
static size_t nnotes;
static uint32_t next_nid = 1;
static bool dnd;
static int seconds = 6;
static int tick_fd = -1;

static ui_surface *pop;
static double pop_h;
static int pop_hot_note = -1, pop_hot_part = -9;

static void layout_pop(void);
static void bus_process(void *arg);

/* ------------------------------------------------------------- helpers */

void notify_strip_markup(const char *in, char *out, size_t n)
{
    size_t o = 0;
    if (!n) return;
    for (const char *p = in; *p && o + 1 < n;) {
        if (*p == '<') {   /* a tag: <br> is a line, the rest goes */
            const char *e = strchr(p, '>');
            if (!e) break;
            if (!strncasecmp(p, "<br", 3) && o + 1 < n) out[o++] = '\n';
            p = e + 1;
            continue;
        }
        if (*p == '&') {
            static const char *const ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
                                                  { "&apos;", "'" }, { "&#39;", "'" }, { "&nbsp;", " " } };
            bool done = false;
            for (size_t i = 0; i < sizeof ent / sizeof *ent && !done; i++)
                if (!strncmp(p, ent[i][0], strlen(ent[i][0]))) {
                    out[o++] = ent[i][1][0];
                    p += strlen(ent[i][0]);
                    done = true;
                }
            if (done) continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

static struct note *find_note(uint32_t nid)
{
    for (size_t i = 0; i < nnotes; i++) if (notes[i].nid == nid) return &notes[i];
    return NULL;
}

static void note_free(struct note *n)
{
    for (size_t i = 0; i < n->nact; i++) free(n->actions[i]);
    n->nact = 0;
}

static void drop_note(struct note *n)
{
    note_free(n);
    size_t i = (size_t)(n - notes);
    memmove(n, n + 1, (nnotes - i - 1) * sizeof *n);
    nnotes--;
    layout_pop();
}

static void flush_later(void)
{
    if (!bus) return;
    sd_bus_flush(bus);
    ui_app_post(app, bus_process, NULL);   /* (what arrived while flushing) */
}

static void emit_closed(const struct note *n, uint32_t reason)
{
    if (!bus || !n->sender[0]) return;
    sd_bus_message *s = NULL;
    if (sd_bus_message_new_signal(bus, &s, NPATH, NIFACE, "NotificationClosed") >= 0 &&
        sd_bus_message_set_destination(s, n->sender) >= 0 && sd_bus_message_append(s, "uu", n->nid, reason) >= 0)
        sd_bus_send(bus, s, NULL);
    sd_bus_message_unref(s);
    flush_later();
}

static void emit_action(const struct note *n, const char *key)
{
    if (!bus || !n->sender[0]) return;
    sd_bus_message *s = NULL;
    if (sd_bus_message_new_signal(bus, &s, NPATH, NIFACE, "ActionInvoked") >= 0 &&
        sd_bus_message_set_destination(s, n->sender) >= 0 && sd_bus_message_append(s, "us", n->nid, key) >= 0)
        sd_bus_send(bus, s, NULL);
    sd_bus_message_unref(s);
    flush_later();
}

static void ignore(void *arg, const cv *reply) { (void)arg; (void)reply; }

/* Closed (by the user, its program, or it left the bus): gone from the
 * active ones, its message's actions with it. */
static void close_note(struct note *n, uint32_t reason)
{
    emit_closed(n, reason);
    if (n->sender[0]) svc_msg_closed(n->nid, n->sender, ignore, NULL);
    drop_note(n);
}

/* ------------------------------------------------------------- pop-ups */

static size_t shown_list(struct note **out)
{
    size_t k = 0;
    for (size_t i = nnotes; i-- > 0 && k < MAX_SHOWN;)   /* newest first */
        if (notes[i].shown_at > 0) out[k++] = &notes[i];
    return k;
}

static double text_x(void) { return PAD + ICON + PAD; }
static double text_w(void) { return POP_W - text_x() - PAD - 16; }
static double line_h(void) { return ceil(ui_scheme_now.font_px * 1.35); }

/* The body in lines of at most w (at spaces where it can, else between
 * characters: Korean, Chinese and Japanese text has few spaces). */
static size_t wrap(const char *text, double w, char lines[][512], size_t max)
{
    size_t nl = 0;
    const char *p = text;
    while (*p && nl < max) {
        char buf[512];
        size_t b = 0, last_space = 0;
        const char *at_space = NULL;
        bool broke = false;
        while (*p && *p != '\n' && b < sizeof buf - 8) {
            size_t cl = 1;
            while (cl < 4 && (p[cl] & 0xc0) == 0x80) cl++;
            memcpy(buf + b, p, cl);
            buf[b + cl] = 0;
            if (b && ui_text_width(UI_FONT_REGULAR, buf) > w) { broke = true; break; }
            if (*p == ' ') { last_space = b; at_space = p; }
            b += cl;
            p += cl;
        }
        buf[b] = 0;
        if (broke && at_space && last_space) { buf[last_space] = 0; p = at_space + 1; }
        snprintf(lines[nl++], 512, "%s", buf);
        if (*p == '\n') p++;
    }
    if (*p && nl == max) {   /* more than fits: "..." */
        size_t l = strlen(lines[max - 1]);
        snprintf(lines[max - 1] + (l > 500 ? 500 : l), 12, "...");
    }
    return nl;
}

/* The buttons: every action but "default" (a click on the body). */
static size_t buttons(const struct note *n, const char **keys, const char **labels)
{
    size_t k = 0;
    for (size_t i = 0; i + 1 < n->nact && k < 4; i += 2) {
        if (!strcmp(n->actions[i], "default")) continue;
        keys[k] = n->actions[i];
        labels[k] = n->actions[i + 1][0] ? n->actions[i + 1] : n->actions[i];
        k++;
    }
    return k;
}

static double note_h(const struct note *n)
{
    char lines[BODY_LINES][512];
    size_t nl = n->body[0] ? wrap(n->body, text_w(), lines, BODY_LINES) : 0;
    double h = PAD + line_h() * (1 + nl) + PAD;
    if (h < PAD * 2 + ICON) h = PAD * 2 + ICON;
    const char *k[4], *l[4];
    if (buttons(n, k, l)) h += BTN_H + PAD;
    return h;
}

static ui_rect button_rect(const struct note *n, double y0, double h, size_t i, size_t count)
{
    (void)n;
    double bw = (POP_W - text_x() - PAD - (count - 1) * 6) / (double)count;
    if (bw > 110) bw = 110;
    return (ui_rect){ text_x() + i * (bw + 6), y0 + h - PAD - BTN_H, bw, BTN_H };
}

static ui_rect close_rect(double y0) { return (ui_rect){ POP_W - PAD - 14, y0 + PAD, 14, 14 }; }

static void pop_draw(ui_surface *s, cairo_t *cr, ui_rect clip)
{
    (void)s; (void)clip;
    const ui_scheme *k = &ui_scheme_now;
    struct note *sh[MAX_SHOWN];
    size_t ns = shown_list(sh);
    ui_fill(cr, (ui_rect){ 0, 0, POP_W, pop_h }, k->face);
    double y = 0;
    for (size_t j = 0; j < ns; j++) {
        const struct note *n = sh[j];
        double h = note_h(n);
        ui_rect r = { 0, y, POP_W, h };
        ui_fill(cr, r, n->urgency == 2 ? k->urgent_bg : k->face);
        ui_bevel(cr, r, true);
        cairo_surface_t *ic = appicon_named(n->icon);
        if (!ic && n->app[0]) ic = appicon_named(n->app);
        if (ic) {
            double iw = cairo_image_surface_get_width(ic), sc = ICON / (iw > 0 ? iw : ICON);
            cairo_save(cr);
            cairo_translate(cr, PAD, y + PAD);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, ic, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        } else {
            ui_draw_kind_icon(cr, UI_KIND_PROGRAM, PAD, y + PAD, ICON);
        }
        double bl = y + PAD + ui_text_ascent();
        ui_set_color(cr, k->text);
        ui_text_draw(cr, UI_FONT_BOLD, text_x(), bl, n->summary[0] ? n->summary : n->app, text_w());
        char lines[BODY_LINES][512];
        size_t nl = n->body[0] ? wrap(n->body, text_w(), lines, BODY_LINES) : 0;
        for (size_t i = 0; i < nl; i++) ui_text_draw(cr, UI_FONT_REGULAR, text_x(), bl + line_h() * (i + 1), lines[i], text_w() + 16);
        /* the close box */
        ui_rect c = close_rect(y);
        bool hot = pop_hot_note == (int)j && pop_hot_part == -2;
        cairo_set_line_width(cr, 1.5);
        ui_set_color(cr, hot ? k->text : k->disabled_text);
        cairo_move_to(cr, c.x + 3, c.y + 3); cairo_line_to(cr, c.x + c.w - 3, c.y + c.h - 3);
        cairo_move_to(cr, c.x + c.w - 3, c.y + 3); cairo_line_to(cr, c.x + 3, c.y + c.h - 3);
        cairo_stroke(cr);
        const char *keys[4], *labels[4];
        size_t nb = buttons(n, keys, labels);
        for (size_t i = 0; i < nb; i++) {
            ui_rect b = button_rect(n, y, h, i, nb);
            bool down = pop_hot_note == (int)j && pop_hot_part == (int)i;
            ui_fill(cr, b, k->face);
            ui_bevel(cr, b, !down);
            double tw = ui_text_width(UI_FONT_REGULAR, labels[i]);
            if (tw > b.w - 8) tw = b.w - 8;
            ui_set_color(cr, k->text);
            ui_text_draw(cr, UI_FONT_REGULAR, b.x + (b.w - tw) / 2 + down, b.y + (b.h + ui_text_ascent()) / 2 - 1 + down, labels[i], b.w - 8);
        }
        y += h;
    }
}

/* Which note and part (an action button's index, -1 the body, -2 the close box) is at y. */
static int hit(double x, double y, int *part)
{
    struct note *sh[MAX_SHOWN];
    size_t ns = shown_list(sh);
    double y0 = 0;
    for (size_t j = 0; j < ns; j++) {
        double h = note_h(sh[j]);
        if (y >= y0 && y < y0 + h) {
            if (ui_rect_has(close_rect(y0), x, y)) { *part = -2; return (int)j; }
            const char *k[4], *l[4];
            size_t nb = buttons(sh[j], k, l);
            for (size_t i = 0; i < nb; i++)
                if (ui_rect_has(button_rect(sh[j], y0, h, i, nb), x, y)) { *part = (int)i; return (int)j; }
            *part = -1;
            return (int)j;
        }
        y0 += h;
    }
    return -1;
}

static void activate(struct note *n, int part)
{
    if (part == -2) {   /* dismissed: the pop-up goes, and the notification with it */
        if (n->sender[0]) close_note(n, 2);
        else drop_note(n);
        return;
    }
    const char *keys[4], *labels[4];
    size_t nb = buttons(n, keys, labels);
    const char *key = NULL;
    if (part >= 0 && (size_t)part < nb) key = keys[part];
    else if (part == -1) {
        for (size_t i = 0; i + 1 < n->nact && !key; i += 2) if (!strcmp(n->actions[i], "default")) key = "default";
    }
    if (n->msg) svc_msg_set(&n->msg, 1, 1, -1, ignore, NULL);   /* seen */
    if (!key) { n->shown_at = 0; layout_pop(); return; }   /* (no default action: out of the way) */
    events_raise("object.open", n->app, "notification", (ui_rect){ 0, 0, 0, 0 });
    emit_action(n, key);
    if (n->resident) { n->shown_at = 0; layout_pop(); }
    else close_note(n, 2);
}

static void pop_pointer(ui_surface *s, const ui_pointer_event *e)
{
    (void)s;
    int part = -9, j = hit(e->x, e->y, &part);
    if (e->kind == UI_MOTION || e->kind == UI_ENTER) {
        int hp = part == -1 ? -9 : part;
        if (j != pop_hot_note || hp != pop_hot_part) {
            pop_hot_note = j;
            pop_hot_part = hp;
            ui_surface_damage_all(pop);
        }
        return;
    }
    if (e->kind == UI_LEAVE) { pop_hot_note = -1; pop_hot_part = -9; ui_surface_damage_all(pop); return; }
    if (e->kind != UI_RELEASE || j < 0) return;
    struct note *sh[MAX_SHOWN];
    size_t ns = shown_list(sh);
    if ((size_t)j < ns) activate(sh[j], part);
}

static void pop_close(ui_surface *s) { (void)s; }

static const ui_surface_ops pop_ops = { .draw = pop_draw, .pointer = pop_pointer, .close = pop_close };

static void pop_destroy(void *arg)
{
    (void)arg;
    struct note *sh[MAX_SHOWN];
    if (pop && !shown_list(sh)) { ui_surface_destroy(pop); pop = NULL; }
}

/* The pop-up window again: its size from what it shows (none: it goes). */
static void layout_pop(void)
{
    struct note *sh[MAX_SHOWN];
    size_t ns = shown_list(sh);
    if (!ns) { if (pop) ui_app_post(app, pop_destroy, NULL); return; }
    double h = 0;
    for (size_t j = 0; j < ns; j++) h += note_h(sh[j]);
    pop_h = h;
    if (!pop) {
        pop = ui_window_new(app, &pop_ops, NULL, "org.wps.Notice", POP_W, h);
        ui_window_set_frameless(pop, true);
        ui_window_set_title(pop, "Messages");
    }
    ui_window_set_size(pop, POP_W, h, true);
    ui_surface_damage_all(pop);
}

static void on_tick(void *arg, int fd, short rev)
{
    (void)arg; (void)rev;
    uint64_t x;
    if (read(fd, &x, sizeof x) < 0) {}
    double now = ui_now_ms();
    bool ch = false;
    for (size_t i = 0; i < nnotes; i++)
        if (notes[i].shown_at > 0 && notes[i].deadline > 0 && now >= notes[i].deadline) {
            notes[i].shown_at = 0;   /* (it stays in Messages, its actions too: "persistence") */
            ch = true;
        }
    if (ch) layout_pop();
}

static void show(struct note *n, int32_t timeout_ms)
{
    if (dnd) return;
    double now = ui_now_ms();
    n->shown_at = now;
    int ms = timeout_ms > 0 ? timeout_ms : seconds * 1000;
    n->deadline = n->urgency == 2 || timeout_ms == 0 ? 0 : now + ms;
    layout_pop();
}

/* ------------------------------------------------------------ the bus */

static void got_msg(void *arg, const cv *reply)
{
    uint32_t nid = (uint32_t)(uintptr_t)arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    struct note *n = find_note(nid);
    if (n && res && res->k == CV_INT) n->msg = res->i;
}

static void keep(struct note *n)
{
    if (n->transient) return;
    svc_message m = { n->app, n->icon, n->summary, n->body, n->sender, n->urgency, n->nid,
                      (const char *const *)n->actions, n->nact };
    svc_msg_add(&m, got_msg, (void *)(uintptr_t)n->nid);
}

static int read_variant_hint(sd_bus_message *m, const char *key, struct note *n)
{
    char type;
    const char *contents;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r < 0) return r;
    if (!strcmp(key, "urgency") && contents && strchr("yui", contents[0]) && !contents[1]) {
        r = sd_bus_message_enter_container(m, 'v', contents);
        if (r < 0) return r;
        if (contents[0] == 'y') { uint8_t u; r = sd_bus_message_read(m, "y", &u); n->urgency = u; }
        else { uint32_t u; r = sd_bus_message_read_basic(m, contents[0], &u); n->urgency = (int)u; }
        if (n->urgency < 0 || n->urgency > 2) n->urgency = 1;
        return r < 0 ? r : sd_bus_message_exit_container(m);
    }
    if ((!strcmp(key, "transient") || !strcmp(key, "resident")) && contents && !strcmp(contents, "b")) {
        int b = 0;
        r = sd_bus_message_enter_container(m, 'v', "b");
        if (r >= 0) r = sd_bus_message_read(m, "b", &b);
        if (key[0] == 't') n->transient = b; else n->resident = b;
        return r < 0 ? r : sd_bus_message_exit_container(m);
    }
    if ((!strcmp(key, "image-path") || !strcmp(key, "image_path")) && contents && !strcmp(contents, "s")) {
        const char *s = NULL;
        r = sd_bus_message_enter_container(m, 'v', "s");
        if (r >= 0) r = sd_bus_message_read(m, "s", &s);
        if (r >= 0 && s && *s) snprintf(n->icon, sizeof n->icon, "%s", s);
        return r < 0 ? r : sd_bus_message_exit_container(m);
    }
    if (!strcmp(key, "desktop-entry") && contents && !strcmp(contents, "s") && !n->icon[0]) {
        const char *s = NULL;
        r = sd_bus_message_enter_container(m, 'v', "s");
        if (r >= 0) r = sd_bus_message_read(m, "s", &s);
        if (r >= 0 && s && *s) snprintf(n->icon, sizeof n->icon, "%s", s);   /* (its icon is usually its name) */
        return r < 0 ? r : sd_bus_message_exit_container(m);
    }
    return sd_bus_message_skip(m, "v");
}

static int m_notify(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *a = NULL, *icon = NULL, *summary = NULL, *body = NULL;
    uint32_t rep = 0;
    int32_t timeout = -1;
    struct note n = { .urgency = 1 };
    int r = sd_bus_message_read(m, "susss", &a, &rep, &icon, &summary, &body);
    if (r < 0) return r;
    snprintf(n.app, sizeof n.app, "%s", a ? a : "");
    snprintf(n.icon, sizeof n.icon, "%s", icon ? icon : "");
    notify_strip_markup(summary ? summary : "", n.summary, sizeof n.summary);
    notify_strip_markup(body ? body : "", n.body, sizeof n.body);
    r = sd_bus_message_enter_container(m, 'a', "s");
    if (r < 0) return r;
    const char *s;
    while ((r = sd_bus_message_read(m, "s", &s)) > 0)
        if (n.nact < MAX_ACTIONS) n.actions[n.nact++] = strdup(s);
    if (r < 0) goto fail;
    n.nact &= ~(size_t)1;
    if ((r = sd_bus_message_exit_container(m)) < 0) goto fail;
    if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0) goto fail;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *key;
        if ((r = sd_bus_message_read(m, "s", &key)) < 0) goto fail;
        if ((r = read_variant_hint(m, key, &n)) < 0) goto fail;
        if ((r = sd_bus_message_exit_container(m)) < 0) goto fail;
    }
    if (r < 0) goto fail;
    if ((r = sd_bus_message_exit_container(m)) < 0) goto fail;
    if ((r = sd_bus_message_read(m, "i", &timeout)) < 0) goto fail;
    const char *sender = sd_bus_message_get_sender(m);
    snprintf(n.sender, sizeof n.sender, "%s", sender ? sender : "");
    struct note *old = rep ? find_note(rep) : NULL;
    if (old && strcmp(old->sender, n.sender)) old = NULL;   /* (only its own) */
    if (old) {   /* replaces it: the same ID, the newest text (one message) */
        n.nid = old->nid;
        if (old->msg) svc_msg_delete(&old->msg, 1, ignore, NULL);
        note_free(old);
        *old = n;
    } else {
        if (nnotes >= MAX_NOTES) close_note(&notes[0], 1);
        n.nid = next_nid++;
        if (!next_nid) next_nid = 1;
        notes = realloc(notes, (nnotes + 1) * sizeof *notes);
        notes[nnotes++] = n;
        old = &notes[nnotes - 1];
    }
    keep(old);
    events_raise(old->urgency == 2 ? "alert.warning" : "alert.info", old->app, "notification", (ui_rect){ 0, 0, 0, 0 });
    show(old, timeout);
    return sd_bus_reply_method_return(m, "u", old->nid);
fail:
    for (size_t i = 0; i < n.nact; i++) free(n.actions[i]);
    return r;
}

static int m_close(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    uint32_t nid;
    int r = sd_bus_message_read(m, "u", &nid);
    if (r < 0) return r;
    struct note *n = find_note(nid);
    if (n) close_note(n, 3);
    return sd_bus_reply_method_return(m, "");
}

static int m_caps(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    sd_bus_message *rep = NULL;
    int r = sd_bus_message_new_method_return(m, &rep);
    if (r >= 0) r = sd_bus_message_append(rep, "as", 5, "body", "body-markup", "actions", "persistence", "icon-static");
    if (r >= 0) r = sd_bus_send(NULL, rep, NULL);
    sd_bus_message_unref(rep);
    return r;
}

static int m_info(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    return sd_bus_reply_method_return(m, "ssss", "Workplace Shell", "wps-linux", WPS_VERSION, "1.2");
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", m_notify, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CloseNotification", "u", "", m_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetCapabilities", "", "as", m_caps, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetServerInformation", "", "ssss", m_info, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
    SD_BUS_SIGNAL("ActionInvoked", "us", 0),
    SD_BUS_VTABLE_END
};

/* A program left the bus: its notifications are closed (their actions
 * could reach no one). */
static int on_owner_changed(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *name, *old, *now;
    if (sd_bus_message_read(m, "sss", &name, &old, &now) < 0 || name[0] != ':' || (now && *now)) return 0;
    for (size_t i = nnotes; i-- > 0;)
        if (!strcmp(notes[i].sender, name)) {
            svc_msg_closed(notes[i].nid, notes[i].sender, ignore, NULL);
            drop_note(&notes[i]);
        }
    return 0;
}

static void bus_process(void *arg)
{
    (void)arg;
    if (!bus) return;
    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0) {}
    if (r < 0) {
        fprintf(stderr, "wps-shell: notifications: the bus went away (%s)\n", strerror(-r));
        ui_app_unwatch_fd(app, sd_bus_get_fd(bus));
        bus = sd_bus_flush_close_unref(bus);
    }
}

static void on_bus(void *arg, int fd, short rev) { (void)fd; (void)rev; bus_process(arg); }

bool notify_init(ui_app *a)
{
    app = a;
    const char *e = getenv("WPS_NOTIFY");
    if (e ? strcmp(e, "1") : !getenv("WPS_SESSION")) return false;
    int r = sd_bus_open_user(&bus);
    if (r < 0) { fprintf(stderr, "wps-shell: notifications: no session bus (%s)\n", strerror(-r)); bus = NULL; return false; }
    if ((r = sd_bus_add_object_vtable(bus, NULL, NPATH, NIFACE, vtable, NULL)) < 0 ||
        (r = sd_bus_request_name(bus, NIFACE, 0)) < 0) {
        fprintf(stderr, "wps-shell: notifications: cannot serve %s (%s): another notification service?\n", NIFACE, strerror(-r));
        bus = sd_bus_flush_close_unref(bus);
        return false;
    }
    sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", on_owner_changed, NULL);
    ui_app_watch_fd(app, sd_bus_get_fd(bus), POLLIN, on_bus, NULL);
    tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tick_fd >= 0) {
        struct itimerspec it = { { 0, 250000000 }, { 0, 250000000 } };
        timerfd_settime(tick_fd, 0, &it, NULL);
        ui_app_watch_fd(app, tick_fd, POLLIN, on_tick, NULL);
    }
    bus_process(NULL);
    fprintf(stderr, "wps-shell: notifications: serving %s\n", NIFACE);
    return true;
}

bool notify_running(void) { return bus != NULL; }

void notify_set_dnd(bool on)
{
    dnd = on;
    if (!on) return;
    for (size_t i = 0; i < nnotes; i++) notes[i].shown_at = 0;
    layout_pop();
}

void notify_set_seconds(int s) { seconds = s > 0 && s < 3600 ? s : 6; }

void notify_local(const char *summary, const char *body, int urgency)
{
    if (!bus || dnd) return;
    if (nnotes >= MAX_NOTES) drop_note(&notes[0]);
    struct note n = { .urgency = urgency };
    n.nid = next_nid++;
    snprintf(n.app, sizeof n.app, "Workplace Shell");
    snprintf(n.icon, sizeof n.icon, "%s", urgency == 2 ? "dialog-error" : "dialog-information");
    snprintf(n.summary, sizeof n.summary, "%s", summary ? summary : "");
    snprintf(n.body, sizeof n.body, "%s", body ? body : "");
    notes = realloc(notes, (nnotes + 1) * sizeof *notes);
    notes[nnotes++] = n;
    notes[nnotes - 1].transient = true;
    show(&notes[nnotes - 1], -1);
    notes[nnotes - 1].deadline = ui_now_ms() + seconds * 1000;   /* (WPS's own errors fade too) */
}

bool notify_can_invoke(int64_t nid, const char *sender)
{
    struct note *n = nid > 0 && nid <= UINT32_MAX ? find_note((uint32_t)nid) : NULL;
    return n && n->sender[0] && !strcmp(n->sender, sender ? sender : "");
}

bool notify_invoke(int64_t nid, const char *sender, const char *key)
{
    if (!notify_can_invoke(nid, sender)) return false;
    struct note *n = find_note((uint32_t)nid);
    emit_action(n, key);
    if (n->resident) { n->shown_at = 0; layout_pop(); }
    else close_note(n, 2);
    return true;
}

size_t notify_shown(void)
{
    struct note *sh[MAX_SHOWN];
    return shown_list(sh);
}

const char *notify_shown_summary(size_t i)
{
    struct note *sh[MAX_SHOWN];
    size_t ns = shown_list(sh);
    return i < ns ? sh[i]->summary : NULL;
}

bool notify_click(size_t i, int action)
{
    struct note *sh[MAX_SHOWN];
    size_t ns = shown_list(sh);
    if (i >= ns) return false;
    activate(sh[i], action);
    return true;
}

bool notify_snapshot(const char *png) { return pop && ui_surface_snapshot(pop, png); }
