/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * Idle: the screens go off after a while without input, the lock screen
 * follows (GNOME's settings: idle-delay, lock-enabled, lock-delay, given
 * by tools/osd-session as OSD_IDLE_DELAY, OSD_LOCK_ENABLED and
 * OSD_LOCK_DELAY, in seconds; 0 delay: never), and programs may hold the
 * screens on (idle-inhibit: video players, presentations) or hear of
 * idleness (ext-idle-notify). In the Objectspace session logind's requests are
 * followed too: lock the session (loginctl lock-session) and sleep (the
 * screen is locked before the machine sleeps).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#ifdef HAVE_SD_BUS
#include <systemd/sd-bus.h>
#endif

#include "comp.h"

static struct {
    int idle_s, lock_s;            /* screens off after idle_s; locked lock_s after that (-1: never) */
    struct wl_event_source *timer;
    bool off;                      /* the screens are off */
    int inhibitors;
    struct wlr_idle_inhibit_manager_v1 *inhibit;
    struct wlr_idle_notifier_v1 *notifier;
    struct wl_listener new_inhibitor;
    enum { WAIT_IDLE, WAIT_LOCK } waiting;
#ifdef HAVE_SD_BUS
    sd_bus *bus;
    char session[256];             /* our logind session's object path */
    int sleep_fd;                  /* the delay inhibitor: released once locked */
    struct wl_event_source *bus_src;
#endif
} idle = { .sleep_fd = -1 };

static void arm(void)
{
    if (!idle.timer) return;
    int s = idle.inhibitors ? 0 : idle.waiting == WAIT_IDLE ? idle.idle_s : idle.lock_s;
    wl_event_source_timer_update(idle.timer, s > 0 ? s * 1000 : 0);   /* 0: disarmed */
}

static void screens(bool on)
{
    if (idle.off == !on) return;
    idle.off = !on;
    struct output *o;
    wl_list_for_each(o, &server.outputs, link) {
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_enabled(&st, on);
        if (!wlr_output_commit_state(o->wlr_output, &st))
            fprintf(stderr, "osd-compositor: %s: could not turn the screen %s\n", o->wlr_output->name, on ? "on" : "off");
        wlr_output_state_finish(&st);
        if (on) wlr_output_schedule_frame(o->wlr_output);
    }
    fprintf(stderr, "osd-compositor: %.1f screens %s\n", comp_uptime(), on ? "on" : "off (idle)");
}

#ifdef HAVE_SD_BUS
static void idle_hint(bool idle_now)
{
    if (idle.bus && idle.session[0])
        sd_bus_call_method(idle.bus, "org.freedesktop.login1", idle.session, "org.freedesktop.login1.Session",
                           "SetIdleHint", NULL, NULL, "b", idle_now);
}
#else
static void idle_hint(bool idle_now) { (void)idle_now; }
#endif

static int on_timer(void *data)
{
    (void)data;
    if (idle.inhibitors) return 0;
    if (idle.waiting == WAIT_IDLE) {
        screens(false);
        idle_hint(true);
        if (idle.lock_s >= 0 && !lock_active()) {
            if (idle.lock_s == 0) lock_screen();
            else { idle.waiting = WAIT_LOCK; arm(); }
        }
    } else {
        if (!lock_active()) lock_screen();
        idle.waiting = WAIT_IDLE;
    }
    return 0;
}

bool idle_activity(void)
{
    bool was_off = idle.off;
    if (idle.notifier) wlr_idle_notifier_v1_notify_activity(idle.notifier, server.seat);
    if (was_off) { screens(true); idle_hint(false); }
    idle.waiting = WAIT_IDLE;
    arm();
    return was_off;
}

/* ------------------------------------------------------------ inhibitors */

struct inhibitor { struct wl_listener destroy; };

static void inhibitors_changed(int d)
{
    idle.inhibitors += d;
    if (idle.notifier) wlr_idle_notifier_v1_set_inhibited(idle.notifier, idle.inhibitors > 0);
    DBG("idle inhibitors: %d\n", idle.inhibitors);
    idle.waiting = WAIT_IDLE;
    arm();
}

static void on_inhibitor_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct inhibitor *in = wl_container_of(l, in, destroy);
    wl_list_remove(&in->destroy.link);
    free(in);
    inhibitors_changed(-1);
}

static void on_new_inhibitor(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_idle_inhibitor_v1 *w = data;
    struct inhibitor *in = calloc(1, sizeof *in);
    in->destroy.notify = on_inhibitor_destroy;
    wl_signal_add(&w->events.destroy, &in->destroy);
    inhibitors_changed(1);
}

/* ---------------------------------------------------------------- logind */

#ifdef HAVE_SD_BUS
static void take_sleep_delay(void)
{
    if (idle.sleep_fd >= 0) return;
    sd_bus_message *reply = NULL;
    if (sd_bus_call_method(idle.bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                           "Inhibit", NULL, &reply, "ssss", "sleep", "Objectspace", "Lock the screen before sleeping", "delay") >= 0) {
        int fd;
        if (sd_bus_message_read(reply, "h", &fd) >= 0) idle.sleep_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    }
    sd_bus_message_unref(reply);
}

static int release_sleep(void *data)
{
    struct wl_event_source **src = data;
    if (idle.sleep_fd >= 0) { close(idle.sleep_fd); idle.sleep_fd = -1; }
    wl_event_source_remove(*src);
    free(src);
    return 0;
}

static int on_prepare_for_sleep(sd_bus_message *m, void *data, sd_bus_error *err)
{
    (void)data; (void)err;
    int going = 0;
    if (sd_bus_message_read(m, "b", &going) < 0) return 0;
    if (going) {
        fprintf(stderr, "osd-compositor: %.1f going to sleep: locking\n", comp_uptime());
        lock_screen();
        /* a moment for the lock screen to be drawn, then sleep may go on */
        struct wl_event_source **src = malloc(sizeof *src);
        *src = wl_event_loop_add_timer(server.loop, release_sleep, src);
        wl_event_source_timer_update(*src, 300);
    } else {
        fprintf(stderr, "osd-compositor: %.1f awake\n", comp_uptime());
        take_sleep_delay();
        idle_activity();
    }
    return 0;
}

static int on_lock(sd_bus_message *m, void *data, sd_bus_error *err)
{
    (void)m; (void)data; (void)err;
    fprintf(stderr, "osd-compositor: %.1f logind asks to lock\n", comp_uptime());
    lock_screen();
    return 0;
}

static int on_bus(int fd, uint32_t mask, void *data)
{
    (void)fd; (void)mask; (void)data;
    while (sd_bus_process(idle.bus, NULL) > 0) {}
    return 0;
}

static void logind_init(void)
{
    if (sd_bus_open_system(&idle.bus) < 0) { idle.bus = NULL; return; }
    sd_bus_message *reply = NULL;
    const char *path = NULL;
    const char *sid = getenv("XDG_SESSION_ID");
    int r = sid && *sid
        ? sd_bus_call_method(idle.bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                             "GetSession", NULL, &reply, "s", sid)
        : sd_bus_call_method(idle.bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                             "GetSessionByPID", NULL, &reply, "u", (uint32_t)getpid());
    if (r >= 0 && sd_bus_message_read(reply, "o", &path) >= 0 && path)
        snprintf(idle.session, sizeof idle.session, "%s", path);
    sd_bus_message_unref(reply);
    if (idle.session[0])
        sd_bus_match_signal(idle.bus, NULL, "org.freedesktop.login1", idle.session, "org.freedesktop.login1.Session",
                            "Lock", on_lock, NULL);
    sd_bus_match_signal(idle.bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager", "PrepareForSleep", on_prepare_for_sleep, NULL);
    take_sleep_delay();
    idle.bus_src = wl_event_loop_add_fd(server.loop, sd_bus_get_fd(idle.bus), WL_EVENT_READABLE, on_bus, NULL);
    on_bus(0, 0, NULL);
    fprintf(stderr, "osd-compositor: logind: session %s%s\n", idle.session[0] ? idle.session : "(not found)",
            idle.sleep_fd >= 0 ? ", locks before sleep" : "");
}
#endif

/* ------------------------------------------------------------------ init */

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : dflt;
}

void idle_init(void)
{
    idle.idle_s = env_int("OSD_IDLE_DELAY", 300);     /* GNOME's default: 5 minutes */
    bool lock = env_int("OSD_LOCK_ENABLED", 1) != 0;
    idle.lock_s = lock ? env_int("OSD_LOCK_DELAY", 0) : -1;
    idle.inhibit = wlr_idle_inhibit_v1_create(server.display);
    idle.new_inhibitor.notify = on_new_inhibitor;
    wl_signal_add(&idle.inhibit->events.new_inhibitor, &idle.new_inhibitor);
    idle.notifier = wlr_idle_notifier_v1_create(server.display);
    idle.timer = wl_event_loop_add_timer(server.loop, on_timer, NULL);
    arm();
    if (idle.idle_s > 0)
        fprintf(stderr, "osd-compositor: screens off after %d s idle%s\n", idle.idle_s,
                idle.lock_s < 0 ? "" : idle.lock_s == 0 ? ", then locked" : ", then locked after a delay");
#ifdef HAVE_SD_BUS
    if (server.session) logind_init();
#endif
}

void idle_fini(void)
{
    if (idle.timer) { wl_event_source_remove(idle.timer); idle.timer = NULL; }
    wl_list_remove(&idle.new_inhibitor.link);
#ifdef HAVE_SD_BUS
    if (idle.bus_src) wl_event_source_remove(idle.bus_src);
    if (idle.sleep_fd >= 0) close(idle.sleep_fd);
    if (idle.bus) sd_bus_flush_close_unref(idle.bus);
#endif
}
