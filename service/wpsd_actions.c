/* wpsd: events' actions (object-properties-design.md 2.4). An object's
 * on:EVENT property, resolved through the chain like any other (its own,
 * its type's, its class's, ...), holds actions separated by "|":
 *   none              silence (the Sound object's sound not played)
 *   sound:FILE        a sound (a theme's name or a file), played by the GUI
 *   run:COMMAND       a command (sh -c), as the user's program, in its own
 *                     scope, with WPS_OBJECT, WPS_PATH, WPS_EVENT, WPS_CLASS
 *   call:METHOD ARGS  a method of the object, its arguments separated by spaces
 *   set:SETUP         a setup string applied to the object
 * Run as the event happens, by the service (the owner's: run: is the user's
 * own command, not sandboxed). */
#define _GNU_SOURCE
#include "wpsd.h"

static void run_command(wpo_object *o, const char *path, const char *event, const char *cmd, const char *display,
                        const char *x11, const char *const *envs, size_t nenvs)
{
    char e_obj[64], e_path[PATH_MAX + 16], e_event[96], e_cls[96], cwd[PATH_MAX];
    snprintf(e_obj, sizeof e_obj, "WPS_OBJECT=%s", wpo_is_a(o, WPObjectClass) ? WPObject_data(o)->id : "");
    snprintf(e_path, sizeof e_path, "WPS_PATH=%s", path ? path : "");
    snprintf(e_event, sizeof e_event, "WPS_EVENT=%s", event);
    snprintf(e_cls, sizeof e_cls, "WPS_CLASS=%s", o->cls->name);
    const char *env[16];
    size_t n = 0;
    for (size_t i = 0; i < nenvs && n < 12; i++) env[n++] = envs[i];
    env[n++] = e_obj;
    env[n++] = e_path;
    env[n++] = e_event;
    env[n++] = e_cls;
    /* in the object's folder (a folder: itself) */
    const char *home = getenv("HOME");
    snprintf(cwd, sizeof cwd, "%s", path && *path ? path : home ? home : "/");
    struct stat st;
    if (stat(cwd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char *sl = strrchr(cwd, '/');
        if (sl && sl != cwd) *sl = 0;
        else snprintf(cwd, sizeof cwd, "%s", home ? home : "/");
    }
    char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
    if (wps_launch(argv, NULL, cwd, display, x11, env, n) < 0)
        log_msg("%s on %s: run:%s: %s", event, path ? path : o->cls->name, cmd, strerror(errno));
}

static void call_method(wpo_object *o, const char *event, const char *spec)
{
    char buf[1024], *args[8];
    snprintf(buf, sizeof buf, "%s", spec);
    size_t n = 0;
    char *method = strtok(buf, " ");
    for (char *a; n < 8 && (a = strtok(NULL, " "));) args[n++] = a;
    const wpo_method_def *m = method ? wpo_method_info(o->cls, method) : NULL;
    if (!m || strlen(m->sig) != n + 3) { log_msg("%s: call:%s: no such method, or not %zu arguments", event, spec, n); return; }
    wpo_value a[8], r = wpo_v_void();
    for (size_t i = 0; i < n; i++) {
        char t = m->sig[2 + i];
        a[i] = t == 'b' ? wpo_v_bool(!strcmp(args[i], "1") || !strcmp(args[i], "true")) :
               t == 'i' ? wpo_v_int(strtoll(args[i], NULL, 10)) :
               t == 'd' ? wpo_v_double(strtod(args[i], NULL)) : wpo_v_string(args[i]);
    }
    if (wpo_send(o, method, a, n, &r) != WPO_OK) log_msg("%s: call:%s: refused", event, spec);
    for (size_t i = 0; i < n; i++) wpo_value_clear(&a[i]);
    wpo_value_clear(&r);
}

char *fire_event(wpo_object *o, const char *path, const char *event, const char *display, const char *x11,
                 const char *const *envs, size_t nenvs)
{
    if (!o || !wpo_is_a(o, WPObjectClass)) return NULL;
    char prop[128];
    snprintf(prop, sizeof prop, "on:%s", event);
    char *v = WPObject_wpQueryProperty(o, prop);
    if (!v || !*v) { free(v); return NULL; }
    char *sound = NULL;
    for (char *q = v, *a; (a = strsep(&q, "|"));) {
        while (*a == ' ') a++;
        size_t l = strlen(a);
        while (l && a[l - 1] == ' ') a[--l] = 0;
        if (!strcmp(a, "none")) { free(sound); sound = strdup(""); }
        else if (!strncmp(a, "sound:", 6)) { free(sound); sound = strdup(a + 6); }
        else if (!strncmp(a, "run:", 4)) run_command(o, path, event, a + 4, display, x11, envs, nenvs);
        else if (!strncmp(a, "call:", 5)) call_method(o, event, a + 5);
        else if (!strncmp(a, "set:", 4)) {
            char err[256];
            if (!apply_setup(o, a + 4, err, sizeof err)) log_msg("%s: set:%s: %s", event, a + 4, err);
        }
    }
    free(v);
    return sound;
}
