/*
 * Input methods through IBus directly (G5), as GTK's IBus module does:
 * where the session asks programs to use IBus (GTK_IM_MODULE=ibus, the
 * Objectspace session) and the compositor has no input method of its own for
 * text-input-v3, Objectspace's entries talk to IBus's daemon over its own D-Bus
 * connection: the keys typed in an entry go to an IBus input context
 * first; what IBus commits, and the text it is composing (Hangul,
 * Anthy, ...), reach the entry as text-input-v3's would
 * (ui_text_input_inject). IBus's engine is the global one, which the
 * compositor's Super+Space sets.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <xkbcommon/xkbcommon.h>

#include "ui_internal.h"

#define CAP_PREEDIT 1u
#define CAP_FOCUS 8u
#define M_SHIFT (1u << 0)
#define M_LOCK (1u << 1)
#define M_CTRL (1u << 2)
#define M_ALT (1u << 3)
#define M_SUPER (1u << 26)
#define M_RELEASE (1u << 30)

static struct {
    bool wanted, tried;
    sd_bus *bus;
    char ctx[256];          /* the input context's object path */
    ui_app *app;
    bool focused;
    int fd;
    bool debug;
} ib = { .fd = -1 };

static void disconnect(void)
{
    if (ib.fd >= 0 && ib.app) ui_app_unwatch_fd(ib.app, ib.fd);
    ib.fd = -1;
    if (ib.bus) sd_bus_flush_close_unref(ib.bus);
    ib.bus = NULL;
    ib.ctx[0] = 0;
    ib.focused = false;
}

/* IBus's address: IBUS_ADDRESS, else the file the daemon wrote for this
 * display (~/.config/ibus/bus/MACHINE-unix-DISPLAY). */
static bool address(char *out, size_t n)
{
    const char *env = getenv("IBUS_ADDRESS");
    if (env && *env) { snprintf(out, n, "%s", env); return true; }
    char mid[64] = "";
    FILE *f = fopen("/etc/machine-id", "r");
    if (!f) f = fopen("/var/lib/dbus/machine-id", "r");
    if (f) { if (!fgets(mid, sizeof mid, f)) mid[0] = 0; fclose(f); }
    mid[strcspn(mid, "\n")] = 0;
    if (!mid[0]) return false;
    const char *wd = getenv("WAYLAND_DISPLAY"), *xd = getenv("DISPLAY");
    char disp[128];
    if (wd && *wd) snprintf(disp, sizeof disp, "%s", wd);
    else if (xd && *xd) {   /* ":0.0" -> "0" (IBus's own naming: host-display) */
        const char *c = strchr(xd, ':');
        snprintf(disp, sizeof disp, "%.*s", c ? (int)strcspn(c + 1, ".") : 0, c ? c + 1 : "");
    } else return false;
    const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char path[4096];
    if (cfg && *cfg) snprintf(path, sizeof path, "%s/ibus/bus/%s-unix-%s", cfg, mid, disp);
    else snprintf(path, sizeof path, "%s/.config/ibus/bus/%s-unix-%s", home ? home : "", mid, disp);
    f = fopen(path, "r");
    if (!f) return false;
    char line[1024];
    bool ok = false;
    while (!ok && fgets(line, sizeof line, f))
        if (!strncmp(line, "IBUS_ADDRESS=", 13)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(out, n, "%s", line + 13);
            ok = true;
        }
    fclose(f);
    return ok;
}

/* The text in an IBusText variant: (sa{sv}sv) - name, attachments, text, attributes. */
static char *read_text(sd_bus_message *m)
{
    const char *name = NULL, *text = NULL;
    if (sd_bus_message_enter_container(m, 'v', "(sa{sv}sv)") < 0) return NULL;
    if (sd_bus_message_enter_container(m, 'r', "sa{sv}sv") < 0) return NULL;
    if (sd_bus_message_read(m, "s", &name) < 0 || sd_bus_message_skip(m, "a{sv}") < 0 || sd_bus_message_read(m, "s", &text) < 0)
        return NULL;
    char *r = text ? strdup(text) : NULL;
    sd_bus_message_skip(m, "v");
    sd_bus_message_exit_container(m);
    sd_bus_message_exit_container(m);
    return r;
}

static ui_app *app_for_text(void) { return ib.app && ib.app->ti_entry && ib.app->ti_surface ? ib.app : NULL; }

static int on_commit(sd_bus_message *m, void *data, sd_bus_error *err)
{
    (void)data; (void)err;
    char *t = read_text(m);
    if (ib.debug) fprintf(stderr, "osd-ui: IBus commits '%s'\n", t ? t : "");
    ui_app *a = app_for_text();
    if (a && t) ui_text_input_inject(a, t, NULL);
    free(t);
    return 0;
}

static int on_preedit(sd_bus_message *m, void *data, sd_bus_error *err)
{
    (void)data; (void)err;
    char *t = read_text(m);
    uint32_t cursor = 0;
    int visible = 1;
    sd_bus_message_read(m, "ub", &cursor, &visible);
    if (ib.debug) fprintf(stderr, "osd-ui: IBus composes '%s'%s\n", t ? t : "", visible ? "" : " (hidden)");
    ui_app *a = app_for_text();
    if (a) ui_text_input_inject(a, NULL, visible && t && *t ? t : NULL);
    free(t);
    return 0;
}

static int on_hide_preedit(sd_bus_message *m, void *data, sd_bus_error *err)
{
    (void)m; (void)data; (void)err;
    ui_app *a = app_for_text();
    if (a) ui_text_input_inject(a, NULL, NULL);
    return 0;
}

/* A key IBus gives back as it is (e.g. after a syllable ends). */
static int on_forward(sd_bus_message *m, void *data, sd_bus_error *err)
{
    (void)data; (void)err;
    uint32_t keyval = 0, keycode = 0, state = 0;
    if (sd_bus_message_read(m, "uuu", &keyval, &keycode, &state) < 0 || (state & M_RELEASE)) return 0;
    ui_app *a = app_for_text();
    if (!a || !a->ti_surface->ops->key) return 0;
    ui_key_event e = { .sym = keyval };
    if (state & M_SHIFT) e.mods |= UI_MOD_SHIFT;
    if (state & M_CTRL) e.mods |= UI_MOD_CTRL;
    if (state & M_ALT) e.mods |= UI_MOD_ALT;
    xkb_keysym_to_utf8(keyval, e.utf8, sizeof e.utf8);
    a->ti_surface->ops->key(a->ti_surface, &e);
    return 0;
}

static void on_bus_fd(void *arg, int fd, short rev)
{
    (void)arg; (void)fd;
    if (!ib.bus) return;
    if (rev & (POLLHUP | POLLERR)) { disconnect(); return; }
    int r;
    while ((r = sd_bus_process(ib.bus, NULL)) > 0) {}
    if (r < 0) disconnect();   /* IBus went away: connect again when next needed */
}

static bool connect_ibus(ui_app *a)
{
    if (ib.bus) return true;
    char addr[1024];
    if (!address(addr, sizeof addr)) return false;
    if (sd_bus_new(&ib.bus) < 0) return false;
    sd_bus_set_address(ib.bus, addr);
    sd_bus_set_bus_client(ib.bus, true);
    if (sd_bus_start(ib.bus) < 0) { sd_bus_unref(ib.bus); ib.bus = NULL; return false; }
    sd_bus_set_method_call_timeout(ib.bus, 500000);   /* a stuck IBus does not freeze the window for long */
    sd_bus_message *reply = NULL;
    const char *path = NULL;
    if (sd_bus_call_method(ib.bus, "org.freedesktop.IBus", "/org/freedesktop/IBus", "org.freedesktop.IBus",
                           "CreateInputContext", NULL, &reply, "s", "osd") < 0 ||
        sd_bus_message_read(reply, "o", &path) < 0 || !path) {
        sd_bus_message_unref(reply);
        disconnect();
        return false;
    }
    snprintf(ib.ctx, sizeof ib.ctx, "%s", path);
    sd_bus_message_unref(reply);
    static const char *const ifc = "org.freedesktop.IBus.InputContext";
    sd_bus_match_signal(ib.bus, NULL, NULL, ib.ctx, ifc, "CommitText", on_commit, NULL);
    sd_bus_match_signal(ib.bus, NULL, NULL, ib.ctx, ifc, "UpdatePreeditText", on_preedit, NULL);
    sd_bus_match_signal(ib.bus, NULL, NULL, ib.ctx, ifc, "HidePreeditText", on_hide_preedit, NULL);
    sd_bus_match_signal(ib.bus, NULL, NULL, ib.ctx, ifc, "ForwardKeyEvent", on_forward, NULL);
    sd_bus_call_method(ib.bus, "org.freedesktop.IBus", ib.ctx, ifc, "SetCapabilities", NULL, NULL, "u", CAP_PREEDIT | CAP_FOCUS);
    ib.app = a;
    ib.fd = sd_bus_get_fd(ib.bus);
    ui_app_watch_fd(a, ib.fd, POLLIN, on_bus_fd, NULL);
    if (ib.debug) fprintf(stderr, "osd-ui: IBus: input context %s\n", ib.ctx);
    return true;
}

static bool wanted(void)
{
    if (!ib.tried) {
        ib.tried = true;
        const char *m = getenv("GTK_IM_MODULE");
        ib.wanted = (m && !strcmp(m, "ibus")) && !getenv("OSD_UI_NO_IM");
        ib.debug = getenv("OSD_DEBUG_IM") != NULL;
    }
    return ib.wanted;
}

void ui_ibus_focus(ui_app *a, bool in)
{
    if (!wanted() || in == ib.focused) return;
    if (in && !connect_ibus(a)) return;
    if (!ib.bus) return;
    ib.focused = in;
    if (!in) sd_bus_call_method(ib.bus, "org.freedesktop.IBus", ib.ctx, "org.freedesktop.IBus.InputContext", "Reset", NULL, NULL, "");
    if (sd_bus_call_method(ib.bus, "org.freedesktop.IBus", ib.ctx, "org.freedesktop.IBus.InputContext", in ? "FocusIn" : "FocusOut",
                           NULL, NULL, "") < 0)
        disconnect();
}

/* A key typed in an entry: IBus first. True when IBus took it. */
bool ui_ibus_key(ui_app *a, uint32_t key, uint32_t sym, bool pressed)
{
    if (!wanted() || !a->ti_entry || !a->ti_surface || a->key_focus != a->ti_surface) return false;
    if (!ib.focused) ui_ibus_focus(a, true);
    if (!ib.bus || !ib.focused) return false;
    uint32_t state = pressed ? 0 : M_RELEASE;
    if (a->kstate) {
        if (xkb_state_mod_name_is_active(a->kstate, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) state |= M_SHIFT;
        if (xkb_state_mod_name_is_active(a->kstate, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) > 0) state |= M_LOCK;
        if (xkb_state_mod_name_is_active(a->kstate, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) state |= M_CTRL;
        if (xkb_state_mod_name_is_active(a->kstate, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) state |= M_ALT;
        if (xkb_state_mod_name_is_active(a->kstate, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) state |= M_SUPER;
    }
    sd_bus_message *reply = NULL;
    int handled = 0;
    int r = sd_bus_call_method(ib.bus, "org.freedesktop.IBus", ib.ctx, "org.freedesktop.IBus.InputContext", "ProcessKeyEvent",
                               NULL, &reply, "uuu", sym, key, state);
    if (r < 0) { disconnect(); return false; }
    sd_bus_message_read(reply, "b", &handled);
    sd_bus_message_unref(reply);
    while (sd_bus_process(ib.bus, NULL) > 0) {}   /* its commit and preedit, before the next key */
    if (ib.debug && pressed) fprintf(stderr, "osd-ui: IBus %s key %#x\n", handled ? "took" : "passed", sym);
    return handled != 0;
}
