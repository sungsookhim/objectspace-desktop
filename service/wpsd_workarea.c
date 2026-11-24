/* wpsd: Work Area folders (0.4 step 9, v0.4-design 11).
 *
 * A folder whose File page has Work area on groups what is opened from it
 * (from it, a folder inside it, or a shadow in either): wpsd records the
 * program it starts as the group's (launch.c follows its children and its
 * scope). The desktop asks for a group's processes when the folder's
 * window is minimized, restored or closed, and the compositor acts on
 * their windows. What was open when the window closed, or at logout, is
 * kept on the folder (`work_area_open`, the paths separated by the record
 * separator, 0x1e: a property takes no line breaks), and opened again with it.
 */
#define _GNU_SOURCE
#include "wpsd.h"

#include <time.h>

/* What was opened from a work area: the object opened (a shadow's own
 * path: it is in the folder), and what shows it in use while it runs. */
struct wa_entry {
    struct wa_entry *next;
    char *group, *opened, *tracked;
    double ended;      /* when it stopped being in use (ms), 0 while it is */
};
static struct wa_entry *entries;

#define RECENT_MS 10000   /* ended this recently at logout: still kept (programs end with the session) */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static bool on(const char *v) { return v && (!strcmp(v, "1") || !strcmp(v, "true")); }

bool work_area_of(const char *path, char *out, size_t n)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path ? path : "");
    for (char *sl = strrchr(p, '/'); sl && sl > p; sl = strrchr(p, '/')) {
        *sl = 0;   /* its folder, then each above it (not "/") */
        osd_loaded ld;
        osd_load(p, false, &ld);
        osdo_object *o = ld.err ? NULL : osd_object_from_loaded(&ld);   /* (borrowed) */
        osd_loaded_free(&ld);
        if (!o || !osdo_is_a(o, OsdFolderClass)) continue;
        char *v = OsdObject_osdQueryProperty(o, "work_area");
        bool yes = on(v);
        free(v);
        if (yes) { snprintf(out, n, "%s", p); return true; }
    }
    return false;
}

void work_area_opened(const char *group, const char *opened, const char *tracked, pid_t pid)
{
    if (pid > 0) osd_launch_set_group(pid, group);
    if (!tracked || !*tracked) return;
    for (struct wa_entry *e = entries; e; e = e->next)
        if (!strcmp(e->group, group) && !strcmp(e->opened, opened)) { e->ended = 0; return; }
    struct wa_entry *e = calloc(1, sizeof *e);
    if (!e) return;
    e->group = strdup(group);
    e->opened = strdup(opened);
    e->tracked = strdup(tracked);
    e->next = entries;
    entries = e;
}

void work_area_in_use(const char *path, bool in_use)
{
    double t = now_ms();
    for (struct wa_entry *e = entries; e; e = e->next)
        if (!strcmp(e->tracked, path)) e->ended = in_use ? 0 : t;
}

#define SEP '\x1e'

/* The group's open objects ("path<SEP>path"), those ended in the last
 * `recent` ms too. */
static char *open_list(const char *group, double recent)
{
    size_t cap = 256, len = 0;
    char *s = malloc(cap);
    if (!s) return NULL;
    s[0] = 0;
    double t = now_ms();
    for (struct wa_entry *e = entries; e; e = e->next) {
        if (strcmp(e->group, group) || (e->ended && t - e->ended > recent)) continue;
        size_t l = strlen(e->opened);
        if (len + l + 2 > cap) {
            cap = (len + l + 2) * 2;
            char *n = realloc(s, cap);
            if (!n) break;
            s = n;
        }
        if (len) s[len++] = SEP;
        memcpy(s + len, e->opened, l + 1);
        len += l;
    }
    return s;
}

static void keep_open_list(const char *group, double recent)
{
    osd_loaded ld;
    osd_load(group, false, &ld);
    osdo_object *o = ld.err ? NULL : osd_object_from_loaded(&ld);
    osd_loaded_free(&ld);
    char *list = open_list(group, recent);
    bool ok = o && list && OsdObject_osdSetProperty(o, "work_area_open", list);
    log_msg("work area %s: kept %s (%s)", group, list ? list : "", ok ? "ok" : "refused");
    free(list);
}

static void forget_group(const char *name)
{
    char *group = strdup(name);   /* (name may be one of the entries' own) */
    if (!group) return;
    for (struct wa_entry **pp = &entries; *pp;) {
        struct wa_entry *e = *pp;
        if (!strcmp(e->group, group)) {
            *pp = e->next;
            free(e->group); free(e->opened); free(e->tracked); free(e);
        } else {
            pp = &e->next;
        }
    }
    free(group);
}

void work_area_save_all(void)
{
    /* each group once: what is open, and what ended with the session */
    for (struct wa_entry *e = entries; e; e = e->next) {
        bool first = true;
        for (struct wa_entry *x = entries; x != e && first; x = x->next) first = strcmp(x->group, e->group) != 0;
        if (first) keep_open_list(e->group, RECENT_MS);
    }
    while (entries) forget_group(entries->group);
}

/* ---- {op:"work_area", target:FOLDER, act:"open"|"close"|"minimize"|"restore"} */

struct pids { cbuf *b; size_t n; bool count_only; char (*units)[96]; size_t nunits; };

static void count_pid(pid_t pid, const char *unit, const char *path, void *arg)
{
    (void)path;
    struct pids *x = arg;
    if (!x->count_only) cb_int(x->b, pid);
    x->n++;
    if (unit && *unit) {
        bool dup = false;
        for (size_t i = 0; i < x->nunits && !dup; i++) dup = !strcmp(x->units[i], unit);
        if (!dup && x->nunits < 64) snprintf(x->units[x->nunits++], sizeof x->units[0], "%s", unit);
    }
}

/* Replies {pids:[...], scopes:[...]} for the compositor to find the
 * group's windows; on open, `reopen`: what was open when it closed (the
 * desktop opens them again), on close what is open is kept for that. Its
 * events: work_area.open, work_area.close (actions on them). */
void do_work_area(struct req *r, osdo_object *o)
{
    const char *act = cv_text(&r->msg, "act");
    const char *group = r->tgt.path;
    if (!act || !group[0] || !osdo_is_a(o, OsdFolderClass)) { reply_error(r->c, r->id, "bad request", "a folder and act are needed"); return; }
    bool opening = !strcmp(act, "open"), closing = !strcmp(act, "close");
    if (!opening && !closing && strcmp(act, "minimize") && strcmp(act, "restore")) {
        reply_error(r->c, r->id, "bad request", "act: open, close, minimize or restore");
        return;
    }
    char *reopen = NULL;
    if (opening) {
        reopen = OsdObject_osdQueryProperty(o, "work_area_open");
        if (reopen && *reopen) OsdObject_osdSetProperty(o, "work_area_open", "");   /* (opened once) */
    }
    if (closing) {
        /* its folders' windows (in it, or opened from it: shadows of
         * folders, links to them), as the desktop says, kept too */
        const cv *fl = cv_get(&r->msg, "folders");
        for (size_t i = 0; fl && fl->k == CV_ARRAY && i < fl->n; i++)
            if (fl->items[i].k == CV_TEXT && fl->items[i].s[0] == '/') work_area_opened(group, fl->items[i].s, fl->items[i].s, 0);
        keep_open_list(group, 0);   /* what is open now: opened again with it */
        forget_group(group);
    }
    char *sound = NULL;
    if (opening || closing) sound = fire_event_for(r->c, o, group, opening ? "work_area.open" : "work_area.close");
    free(sound);   /* (the window's own sounds play) */
    /* the group's processes, and their scopes */
    struct pids cnt = { .count_only = true, .units = calloc(64, 96) };
    osd_launch_group_each(group, count_pid, &cnt);
    size_t nre = 0;
    for (const char *p = reopen; p && *p; p = strchr(p, SEP) ? strchr(p, SEP) + 1 : "") nre++;
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, 3);
    cb_text(&b, "pids");
    cb_array(&b, cnt.n);
    struct pids put = { .b = &b, .units = calloc(64, 96) };
    osd_launch_group_each(group, count_pid, &put);
    cb_text(&b, "scopes");
    cb_array(&b, cnt.nunits);
    for (size_t i = 0; i < cnt.nunits; i++) cb_text(&b, cnt.units[i]);
    cb_text(&b, "reopen");
    cb_array(&b, nre);
    for (const char *p = reopen; p && *p;) {
        const char *nl = strchr(p, SEP);
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        cb_textn(&b, p, l);
        p = nl ? nl + 1 : p + l;
    }
    send_frame(r->c, &b);
    cb_free(&b);
    free(cnt.units);
    free(put.units);
    free(reopen);
}
