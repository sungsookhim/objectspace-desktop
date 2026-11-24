/* wpsd: events' actions (object-properties-design.md 2.4). An object's
 * on:EVENT property, resolved through the chain like any other (its own,
 * its type's, its class's, ...), holds actions separated by "|":
 *   none              silence (the Sound object's sound not played)
 *   sound:FILE        a sound (a theme's name or a file), played by the GUI
 *   run:COMMAND       a command (sh -c), as the user's program, in its own
 *                     scope, with OSD_OBJECT, OSD_PATH, OSD_EVENT, OSD_CLASS
 *   call:METHOD ARGS  a method of the object, its arguments separated by spaces
 *   set:SETUP         a setup string applied to the object
 * Run as the event happens, by the service (the owner's: run: is the user's
 * own command, not sandboxed). */
#define _GNU_SOURCE
#include "wpsd.h"

/* An action that could not be done: told in Messages (and the log). */
static void problem(const char *event, const char *path, const char *action, const char *why)
{
    char body[PATH_MAX + 512];
    snprintf(body, sizeof body, "%s (%s): %s\n%s", path && *path ? path : "an object", event, action, why);
    log_msg("action: %s", body);
    cv items[8] = {
        { .k = CV_TEXT, .s = "app", .len = 3 }, { .k = CV_TEXT, .s = "WPS", .len = 3 },
        { .k = CV_TEXT, .s = "summary", .len = 7 }, { .k = CV_TEXT, .s = "An action was not done", .len = 22 },
        { .k = CV_TEXT, .s = "body", .len = 4 }, { .k = CV_TEXT, .s = body, .len = strlen(body) },
        { .k = CV_TEXT, .s = "urgency", .len = 7 }, { .k = CV_INT, .i = 1 },
    };
    cv m = { .k = CV_MAP, .items = items, .n = 4, .nitems = 8 };
    messages_broadcast(osd_messages_add(&m));
}

static void run_command(osdo_object *o, const char *path, const char *event, const char *cmd, const char *display,
                        const char *x11, const char *const *envs, size_t nenvs)
{
    char e_obj[64], e_path[PATH_MAX + 16], e_event[96], e_cls[96], cwd[PATH_MAX];
    snprintf(e_obj, sizeof e_obj, "OSD_OBJECT=%s", osdo_is_a(o, OsdObjectClass) ? OsdObject_data(o)->id : "");
    snprintf(e_path, sizeof e_path, "OSD_PATH=%s", path ? path : "");
    snprintf(e_event, sizeof e_event, "OSD_EVENT=%s", event);
    snprintf(e_cls, sizeof e_cls, "OSD_CLASS=%s", o->cls->name);
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
    if (osd_launch(argv, NULL, cwd, display, x11, env, n) < 0) {
        char a[600];
        snprintf(a, sizeof a, "run:%s", cmd);
        problem(event, path, a, strerror(errno));
    }
}

static void call_method(osdo_object *o, const char *path, const char *event, const char *spec)
{
    char buf[1024], *args[8];
    snprintf(buf, sizeof buf, "%s", spec);
    size_t n = 0;
    char *method = strtok(buf, " ");
    for (char *a; n < 8 && (a = strtok(NULL, " "));) args[n++] = a;
    const osdo_method_def *m = method ? osdo_method_info(o->cls, method) : NULL;
    char a0[600];
    snprintf(a0, sizeof a0, "call:%s", spec);
    if (!m || strlen(m->sig) != n + 3) { problem(event, path, a0, "no such method, or another number of arguments"); return; }
    osdo_value a[8], r = osdo_v_void();
    for (size_t i = 0; i < n; i++) {
        char t = m->sig[2 + i];
        a[i] = t == 'b' ? osdo_v_bool(!strcmp(args[i], "1") || !strcmp(args[i], "true")) :
               t == 'i' ? osdo_v_int(strtoll(args[i], NULL, 10)) :
               t == 'd' ? osdo_v_double(strtod(args[i], NULL)) : osdo_v_string(args[i]);
    }
    if (osdo_send(o, method, a, n, &r) != OSDO_OK || (r.type == 'b' && !r.u.b)) problem(event, path, a0, "refused");
    for (size_t i = 0; i < n; i++) osdo_value_clear(&a[i]);
    osdo_value_clear(&r);
}

/* A client's desktop session (its display and settings, from its last
 * open): for the actions of what it does next (trash, delete, create). */
void client_note_session(struct client *c, const char *display, const char *x11, const char *const *envs, size_t nenvs)
{
    snprintf(c->display, sizeof c->display, "%s", display ? display : "");
    snprintf(c->x11, sizeof c->x11, "%s", x11 ? x11 : "");
    c->nenvs = 0;
    for (size_t i = 0; i < nenvs && c->nenvs < 8; i++) snprintf(c->envs[c->nenvs++], sizeof c->envs[0], "%s", envs[i]);
}

char *fire_event_for(struct client *c, osdo_object *o, const char *path, const char *event)
{
    const char *e[8];
    size_t n = 0;
    for (size_t i = 0; c && i < c->nenvs; i++) e[n++] = c->envs[i];
    return fire_event(o, path, event, c && c->display[0] ? c->display : NULL, c && c->x11[0] ? c->x11 : NULL, e, n);
}

char *fire_event(osdo_object *o, const char *path, const char *event, const char *display, const char *x11,
                 const char *const *envs, size_t nenvs)
{
    if (!o || !osdo_is_a(o, OsdObjectClass)) return NULL;
    char prop[128];
    snprintf(prop, sizeof prop, "on:%s", event);
    char *v = OsdObject_osdQueryProperty(o, prop);
    if (!v || !*v) { free(v); return NULL; }
    char *sound = NULL;
    for (char *q = v, *a; (a = strsep(&q, "|"));) {
        while (*a == ' ') a++;
        size_t l = strlen(a);
        while (l && a[l - 1] == ' ') a[--l] = 0;
        if (!strcmp(a, "none")) { free(sound); sound = strdup(""); }
        else if (!strncmp(a, "sound:", 6)) { free(sound); sound = strdup(a + 6); }
        else if (!strncmp(a, "run:", 4)) run_command(o, path, event, a + 4, display, x11, envs, nenvs);
        else if (!strncmp(a, "call:", 5)) call_method(o, path, event, a + 5);
        else if (!strncmp(a, "set:", 4)) {
            char err[256];
            if (!apply_setup(o, a + 4, err, sizeof err)) problem(event, path, a, err);
        }
    }
    free(v);
    return sound;
}
