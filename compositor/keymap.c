/*
 * The keyboard layouts: GNOME's input sources (the xkb ones, up to four,
 * as xkb's groups) and its xkb options, read from GNOME's settings and
 * followed while the session runs (`gsettings monitor`): a change in
 * GNOME's Settings, Keyboard applies at once to every keyboard. Super+Space
 * (Shift+Super+Space) goes to the next (previous) input source, as in
 * GNOME: an xkb source is a layout (an xkb group); an IBus one (Hangul,
 * Anthy, ...) is set as IBus's engine (`ibus engine`), and going back to
 * a layout sets IBus's engine for that layout, so keys pass as typed. The
 * WPS session starts IBus (tools/osd-session).
 * Without gsettings (headless tests, OSD_NO_GSETTINGS) XKB_DEFAULT_* or US.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard.h>

#include "comp.h"

static struct {
    char layout[128], variant[128], options[256];
    bool from_gnome;            /* else XKB_DEFAULT_* (or the compiled-in default) */
    int ngroups;
    struct { bool ibus; char name[64]; int group; } srcs[16];   /* every input source, in GNOME's order */
    int nsrc, cur;
    pid_t mon;
    int mon_fd;
    struct wl_event_source *src;
    char line[1024];
    size_t ll;
} km = { .mon = -1, .mon_fd = -1 };

static bool gsettings_get(const char *schema, const char *key, char *out, size_t n)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "gsettings get %s %s 2>/dev/null", schema, key);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    bool ok = fgets(out, (int)n, p) != NULL;
    pclose(p);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok && *out;
}

/* [('xkb', 'us'), ('ibus', 'hangul'), ('xkb', 'de+nodeadkeys')] -> "us,de" and ",nodeadkeys" */
static void read_gnome(void)
{
    char s[1024], o[512];
    km.from_gnome = false;
    if (getenv("OSD_NO_GSETTINGS") || !gsettings_get("org.gnome.desktop.input-sources", "sources", s, sizeof s)) return;
    km.layout[0] = km.variant[0] = 0;
    km.ngroups = 0;
    km.nsrc = 0;
    km.cur = 0;
    /* every source: ('xkb', 'us'), ('ibus', 'hangul') */
    for (const char *p = s; (p = strstr(p, "('")) && km.nsrc < 16;) {
        p += 2;
        bool ibus = !strncmp(p, "ibus'", 5), xkb = !strncmp(p, "xkb'", 4);
        const char *q = strstr(p, ", '");
        if (!q) break;
        q += 3;
        const char *e = strchr(q, '\'');
        if (!e) break;
        if (ibus || xkb) {
            km.srcs[km.nsrc].ibus = ibus;
            snprintf(km.srcs[km.nsrc].name, sizeof km.srcs[km.nsrc].name, "%.*s", (int)(e - q), q);
            km.srcs[km.nsrc].group = -1;
            km.nsrc++;
        }
        p = e;
    }
    for (int i = 0, g = 0; i < km.nsrc && g < 4; i++) if (!km.srcs[i].ibus) km.srcs[i].group = g++;
    for (const char *p = s; (p = strstr(p, "('xkb', '")) && km.ngroups < 4;) {
        p += 9;
        const char *e = strchr(p, '\'');
        if (!e) break;
        char one[96];
        snprintf(one, sizeof one, "%.*s", (int)(e - p), p);
        char *plus = strchr(one, '+');
        if (plus) *plus++ = 0;
        size_t ll = strlen(km.layout), vl = strlen(km.variant);
        snprintf(km.layout + ll, sizeof km.layout - ll, "%s%s", km.ngroups ? "," : "", one);
        snprintf(km.variant + vl, sizeof km.variant - vl, "%s%s", km.ngroups ? "," : "", plus ? plus : "");
        km.ngroups++;
        p = e;
    }
    km.options[0] = 0;
    if (gsettings_get("org.gnome.desktop.input-sources", "xkb-options", o, sizeof o) && strcmp(o, "@as []")) {
        /* ['caps:ctrl_modifier', 'compose:ralt'] -> caps:ctrl_modifier,compose:ralt */
        size_t k = 0;
        for (const char *p = o; *p && k + 1 < sizeof km.options; p++)
            if (*p != '[' && *p != ']' && *p != '\'' && *p != ' ') km.options[k++] = *p;
        km.options[k] = 0;
    }
    km.from_gnome = km.ngroups > 0;
}

struct xkb_keymap *keymap_new(void)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *m = NULL;
    if (km.from_gnome) {
        struct xkb_rule_names names = { .layout = km.layout, .variant = km.variant,
                                        .options = km.options[0] ? km.options : NULL };
        m = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!m) fprintf(stderr, "osd-compositor: keyboard layouts %s (variants %s) cannot be made\n", km.layout, km.variant);
    }
    if (!m) m = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);   /* XKB_DEFAULT_* */
    if (!m) {
        /* a layout that cannot be made (a typo, an unknown variant): US,
         * rather than a keyboard that types nothing at all */
        fprintf(stderr, "osd-compositor: keyboard layout %s cannot be made; US instead\n",
                getenv("XKB_DEFAULT_LAYOUT") ? getenv("XKB_DEFAULT_LAYOUT") : "(default)");
        struct xkb_rule_names us = { .layout = "us" };
        m = xkb_keymap_new_from_names(ctx, &us, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    xkb_context_unref(ctx);
    return m;
}

static void apply_all(void)
{
    struct xkb_keymap *m = keymap_new();
    if (!m) return;
    struct keyboard *k;
    wl_list_for_each(k, &server.keyboards, link) wlr_keyboard_set_keymap(k->wlr_keyboard, m);
    xkb_keymap_unref(m);
    fprintf(stderr, "osd-compositor: %.1f keyboard: %s%s%s%s%s\n", comp_uptime(), km.from_gnome ? km.layout : "(XKB_DEFAULT_*)",
            km.variant[0] && strspn(km.variant, ",") != strlen(km.variant) ? " variants " : "",
            km.variant[0] && strspn(km.variant, ",") != strlen(km.variant) ? km.variant : "",
            km.options[0] ? " options " : "", km.options);
}

static void set_group(xkb_layout_index_t g)
{
    struct keyboard *k;
    wl_list_for_each(k, &server.keyboards, link) {
        struct wlr_keyboard *wk = k->wlr_keyboard;
        if (wk->keymap && g < xkb_keymap_num_layouts(wk->keymap))
            wlr_keyboard_notify_modifiers(wk, wk->modifiers.depressed, wk->modifiers.latched, wk->modifiers.locked, g);
    }
}

/* IBus's engine: an input method, or for a layout the engine that types
 * it as is (xkb:LAYOUT:VARIANT:LANG, as IBus lists it). */
static void ibus_engine(int i)
{
    char cmd[512];
    if (km.srcs[i].ibus) snprintf(cmd, sizeof cmd, "ibus engine '%s'", km.srcs[i].name);
    else {
        char lay[64], *var;
        snprintf(lay, sizeof lay, "%s", km.srcs[i].name);
        var = strchr(lay, '+');
        if (var) *var++ = 0;
        snprintf(cmd, sizeof cmd, "e=$(ibus list-engine | grep -o 'xkb:%s:%s:[a-zA-Z_]*' | head -1); [ -n \"$e\" ] && ibus engine \"$e\"",
                 lay, var ? var : "");
    }
    char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    comp_spawn(argv, NULL, NULL);
}

/* The current input source, to the desktop (the WarpCenter's keyboard
 * part). Without GNOME's sources: the keyboard's xkb group. */
void keymap_report(void)
{
    if (km.nsrc >= 1) {
        desktop_input_source(km.cur, km.nsrc, km.srcs[km.cur].ibus, km.srcs[km.cur].name);
        return;
    }
    struct keyboard *k;
    wl_list_for_each(k, &server.keyboards, link) {
        struct wlr_keyboard *wk = k->wlr_keyboard;
        if (!wk->keymap) continue;
        xkb_layout_index_t n = xkb_keymap_num_layouts(wk->keymap), g = wk->modifiers.group;
        const char *nm = xkb_keymap_layout_get_name(wk->keymap, g < n ? g : 0);
        /* (a long name, "English (US)": the desktop shortens it) */
        desktop_input_source((int)g, (int)n, false, getenv("XKB_DEFAULT_LAYOUT") && n == 1 ? getenv("XKB_DEFAULT_LAYOUT") : nm ? nm : "us");
        return;
    }
    desktop_input_source(0, 1, false, "us");
}

static void use_source(int i)
{
    bool any_ibus = false;
    for (int j = 0; j < km.nsrc; j++) any_ibus |= km.srcs[j].ibus;
    if (!km.srcs[i].ibus) set_group((xkb_layout_index_t)km.srcs[i].group);
    if (any_ibus) ibus_engine(i);
    fprintf(stderr, "osd-compositor: %.1f input source %d of %d: %s%s\n", comp_uptime(), i + 1, km.nsrc,
            km.srcs[i].ibus ? "IBus " : "", km.srcs[i].name);
    keymap_report();
}

void keymap_select(int i)
{
    if (km.nsrc < 2) {   /* (XKB_DEFAULT_* groups) */
        struct keyboard *k;
        wl_list_for_each(k, &server.keyboards, link) {
            struct wlr_keyboard *wk = k->wlr_keyboard;
            if (wk->keymap && i >= 0 && (xkb_layout_index_t)i < xkb_keymap_num_layouts(wk->keymap)) set_group((xkb_layout_index_t)i);
            break;
        }
        keymap_report();
        return;
    }
    if (i < 0 || i >= km.nsrc) return;
    km.cur = i;
    use_source(i);
}

/* The next (or previous) input source, as GNOME's Super+Space. */
void keymap_cycle(int dir)
{
    if (km.nsrc < 2) {   /* (layouts from XKB_DEFAULT_*: their groups) */
        struct keyboard *k;
        wl_list_for_each(k, &server.keyboards, link) {
            struct wlr_keyboard *wk = k->wlr_keyboard;
            xkb_layout_index_t n = wk->keymap ? xkb_keymap_num_layouts(wk->keymap) : 0;
            if (n >= 2) set_group((wk->modifiers.group + (dir < 0 ? n - 1 : 1)) % n);
            break;
        }
        keymap_report();
        return;
    }
    km.cur = (km.cur + km.nsrc + (dir < 0 ? -1 : 1)) % km.nsrc;
    use_source(km.cur);
}

static int on_monitor(int fd, uint32_t mask, void *data)
{
    (void)data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        wl_event_source_remove(km.src);
        km.src = NULL;
        return 0;
    }
    char buf[512];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) return 0;
    bool changed = false;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] != '\n') { if (km.ll + 1 < sizeof km.line) km.line[km.ll++] = buf[i]; continue; }
        km.line[km.ll] = 0;
        km.ll = 0;
        /* "sources: [...]", "xkb-options: [...]" (current, mru-sources: GNOME's own switching) */
        if (!strncmp(km.line, "sources:", 8) || !strncmp(km.line, "xkb-options:", 12)) changed = true;
    }
    if (changed) { read_gnome(); apply_all(); }
    return 0;
}

/* As the session starts, IBus (started with it) on the first source. */
static int first_engine(void *data)
{
    struct wl_event_source **t = data;
    for (int j = 0; j < km.nsrc; j++)
        if (km.srcs[j].ibus) { ibus_engine(km.cur); break; }
    wl_event_source_remove(*t);
    *t = NULL;
    return 0;
}

void keymap_init(void)
{
    read_gnome();
    static struct wl_event_source *t;
    if (getenv("OSD_SESSION") && !getenv("OSD_NO_GSETTINGS")) {
        t = wl_event_loop_add_timer(server.loop, first_engine, &t);
        wl_event_source_timer_update(t, 4000);
    }
    if (km.from_gnome)
        fprintf(stderr, "osd-compositor: keyboard: %s (GNOME's input sources)%s%s\n", km.layout,
                km.options[0] ? " options " : "", km.options);
    if (getenv("OSD_NO_GSETTINGS")) return;
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return;
    pid_t parent = getpid();
    km.mon = fork();
    if (km.mon == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);   /* ends with the compositor */
        if (getppid() != parent) _exit(0);
        dup2(p[1], 1);
        execlp("gsettings", "gsettings", "monitor", "org.gnome.desktop.input-sources", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (km.mon < 0) { close(p[0]); return; }
    km.mon_fd = p[0];
    km.src = wl_event_loop_add_fd(server.loop, km.mon_fd, WL_EVENT_READABLE, on_monitor, NULL);
}

void keymap_fini(void)
{
    if (km.src) wl_event_source_remove(km.src);
    if (km.mon_fd >= 0) close(km.mon_fd);
    if (km.mon > 0) { kill(km.mon, SIGTERM); waitpid(km.mon, NULL, 0); }
}
