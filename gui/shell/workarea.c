/* wps-shell: Work Area folders (0.4 step 9, v0.4-design 11). A work area
 * folder's window takes what was opened from it along: minimized, its
 * group's windows are minimized (its folders' windows too); restored,
 * restored; closed, its programs are asked to close (what was open is kept,
 * and opened again when the window opens again). wpsd knows the group
 * (its processes and scopes); the compositor finds their windows (the WPS
 * session; inside GNOME, the extension: part 2). */
#define _GNU_SOURCE
#include "shell.h"

struct wa_req {
    char act[16];
    char path[PATH_MAX];
};

/* Is `p` in the folder `in` (not the folder itself)? */
static bool inside(const char *p, const char *in)
{
    size_t l = strlen(in);
    return !strncmp(p, in, l) && p[l] == '/';
}

static void window_title(struct folder *f, char *out, size_t n)
{
    snprintf(out, n, "%s - %s", f->title, view_name(ui_container_view(f->cont)));
}

static void got(void *arg, const cv *reply)
{
    struct wa_req *q = arg;
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    if (!res) {
        fprintf(stderr, "wps-shell: work area %s: %s\n", q->path, reply && cv_text(reply, "message") ? cv_text(reply, "message") : "no reply");
        free(q);
        return;
    }
    /* its programs' windows: "group TAB act TAB pids TAB scopes" for the compositor */
    const cv *pids = cv_get(res, "pids"), *scopes = cv_get(res, "scopes");
    size_t np = pids && pids->k == CV_ARRAY ? pids->n : 0, ns = scopes && scopes->k == CV_ARRAY ? scopes->n : 0;
    if (np || ns) {
        char *line = NULL;
        size_t len = 0;
        FILE *m = open_memstream(&line, &len);
        fprintf(m, "%s\t", !strcmp(q->act, "open") ? "restore" : q->act);
        for (size_t i = 0; i < np; i++) if (pids->items[i].k == CV_INT) fprintf(m, "%s%lld", i ? "," : "", (long long)pids->items[i].i);
        fputc('\t', m);
        for (size_t i = 0; i < ns; i++) if (scopes->items[i].k == CV_TEXT) fprintf(m, "%s%s", i ? "," : "", scopes->items[i].s);
        fclose(m);
        winlist_ext("group", line);
        free(line);
    }
    const cv *re0 = cv_get(res, "reopen");
    fprintf(stderr, "wps-shell: work area %s: %s, %zu programs, %zu to open again\n", q->path, q->act, np,
            re0 && re0->k == CV_ARRAY ? re0->n : 0);
    /* its folders' windows go with it (minimized and restored; closed, they stay) */
    if (!strcmp(q->act, "minimize") || !strcmp(q->act, "restore"))
        for (struct folder *g = folders; g; g = g->next) {
            if (g->closing || g->desktop || !inside(g->path, q->path)) continue;
            char t[400];
            window_title(g, t, sizeof t);
            winlist_command(q->act, t);
        }
    /* opened: what was open when it closed, opened again */
    const cv *re = cv_get(res, "reopen");
    struct folder *wf = NULL;
    for (struct folder *g = folders; g && !wf; g = g->next) if (!g->closing && !strcmp(g->path, q->path)) wf = g;
    for (size_t i = 0; wf && re && re->k == CV_ARRAY && i < re->n; i++)
        if (re->items[i].k == CV_TEXT && re->items[i].s[0] == '/') svc_open(re->items[i].s, false, opened, as_arg(wf));
    free(q);
}

void workarea_act(struct folder *f, const char *act)
{
    if (!f || f->desktop || !f->work_area) return;
    /* another view of it still shown (Icon and Details at once): that one
     * keeps the group, closed or minimized only with the last */
    if (!strcmp(act, "close") || !strcmp(act, "minimize"))
        for (struct folder *g = folders; g; g = g->next)
            if (g != f && !g->closing && !strcmp(g->path, f->path) && g->wa_state != 2) return;
    struct wa_req *q = calloc(1, sizeof *q);
    if (!q) return;
    snprintf(q->act, sizeof q->act, "%s", act);
    snprintf(q->path, sizeof q->path, "%s", f->path);
    svc_work_area(f->path, act, got, q);
}

/* Its settings were read: Work area on, the first time for this window:
 * its group comes back (and what was open, opened again). */
void workarea_settings(struct folder *f, bool on)
{
    bool was = f->work_area;
    f->work_area = on;
    if (on && !was && !f->wa_opened) {
        f->wa_opened = true;
        workarea_act(f, "open");
    }
}

/* A window's state from the compositor (the window list's lines): a work
 * area's own window minimized or restored. */
void workarea_window(const char *title, bool minimized)
{
    for (struct folder *f = folders; f; f = f->next) {
        if (f->closing || f->desktop || !f->work_area) continue;
        char t[400];
        window_title(f, t, sizeof t);
        if (strcmp(t, title)) continue;
        if (f->wa_state && (f->wa_state == 2) != minimized) workarea_act(f, minimized ? "minimize" : "restore");
        f->wa_state = minimized ? 2 : 1;
    }
}
