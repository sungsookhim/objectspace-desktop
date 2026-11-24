/* osd-shell: Work Area folders (0.4 step 9, v0.4-design 11). A work area
 * folder's window takes what was opened from it along: minimized, its
 * group's windows are minimized (its folders' windows too); restored,
 * restored; closed, its programs are asked to close (what was open is kept,
 * and opened again when the window opens again). osdd knows the group
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

/* Does folder window g go with the work area at `path`: opened from it (a
 * shadow of a folder, a link to one, the folders in it), or in it? */
static bool member(struct folder *g, const char *path)
{
    return !g->closing && !g->desktop && strcmp(g->path, path) && (!strcmp(g->wa_group, path) || inside(g->path, path));
}

const char *workarea_group_of(struct folder *f)
{
    if (!f || f->desktop) return NULL;
    if (f->work_area) return f->path;
    if (f->wa_group[0]) return f->wa_group;
    for (struct folder *w = folders; w; w = w->next)   /* (a folder in an open work area) */
        if (!w->closing && !w->desktop && w->work_area && inside(f->path, w->path)) return w->path;
    return NULL;
}

void workarea_join(struct folder *f, const char *group)
{
    if (f && group && *group && !f->wa_group[0] && strcmp(f->path, group)) snprintf(f->wa_group, sizeof f->wa_group, "%s", group);
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
        fprintf(stderr, "osd-shell: work area %s: %s\n", q->path, reply && cv_text(reply, "message") ? cv_text(reply, "message") : "no reply");
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
    fprintf(stderr, "osd-shell: work area %s: %s, %zu programs, %zu to open again\n", q->path, q->act, np,
            re0 && re0->k == CV_ARRAY ? re0->n : 0);
    /* its folders' windows go with it: those in it, and those opened from
     * it (shadows of folders, links to them) - closed too (kept, as its
     * programs: osdd had their paths with the request) */
    if (!strcmp(q->act, "minimize") || !strcmp(q->act, "restore"))
        for (struct folder *g = folders; g; g = g->next) {
            if (!member(g, q->path)) continue;
            char t[400];
            window_title(g, t, sizeof t);
            winlist_command(q->act, t);
        }
    if (!strcmp(q->act, "close"))
        for (struct folder *g = folders; g; g = g->next)
            if (member(g, q->path)) win_close(g->win);
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
    /* closing: its folders' windows open now, kept with its programs */
    const char *fl[64];
    size_t nf = 0;
    for (struct folder *g = folders; g && nf < 64 && !strcmp(act, "close"); g = g->next)
        if (member(g, f->path)) fl[nf++] = g->path;
    svc_work_area(f->path, act, fl, nf, got, q);
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

/* ------------------------------------------------ the wps command (0.4.1)
 *
 * `wps ask`, `wps selection` and `wps open` on a folder reach the desktop
 * through osdd ("ask", "selection", "show" events); the answer goes back
 * as svc_answer. Only the process with the desktop answers. */

static long active_folder;   /* the folder window that had the keyboard last */

void workarea_focused(struct folder *f) { if (f && !f->desktop) active_folder = f->id; }

static bool has_desktop(void)
{
    for (struct folder *f = folders; f; f = f->next) if (f->desktop && !f->closing) return true;
    return false;
}

struct osd_ask { int64_t ask; char **labels; size_t n; };

static void asked(void *ctx, int choice)
{
    struct osd_ask *a = ctx;
    const char *v = choice >= 0 && (size_t)choice < a->n ? a->labels[choice] : "";
    svc_answer(a->ask, v, NULL, 0);
    for (size_t i = 0; i < a->n; i++) free(a->labels[i]);
    free(a->labels);
    free(a);
}

bool osd_command_event(const cv *ev)
{
    const char *name = cv_text(ev, "event");
    if (!name || (strcmp(name, "ask") && strcmp(name, "selection") && strcmp(name, "show"))) return false;
    if (!has_desktop()) return true;   /* (a folder window's process: the desktop answers) */
    int64_t ask = 0;
    cv_int(ev, "ask", &ask);
    if (!strcmp(name, "show")) {
        const char *p = cv_text(ev, "text");
        if (p && *p) open_folder(p, VIEW_LAST);
        return true;
    }
    if (!strcmp(name, "ask")) {
        const cv *bs = cv_get(ev, "buttons");
        struct osd_ask *a = calloc(1, sizeof *a);
        size_t n = bs && bs->k == CV_ARRAY && bs->n ? bs->n : 1;
        if (n > 6) n = 6;
        a->ask = ask;
        a->labels = calloc(n, sizeof *a->labels);
        a->n = n;
        for (size_t i = 0; i < n; i++)
            a->labels[i] = strdup(bs && bs->k == CV_ARRAY && i < bs->n && bs->items[i].k == CV_TEXT ? bs->items[i].s : "OK");
        ui_dialog_open(app, "WPS", cv_text(ev, "text") ? cv_text(ev, "text") : "", (const char *const *)a->labels, n, 0, asked, a);
        return true;
    }
    /* the selection: the active folder window's (else the desktop's) selected objects */
    struct folder *f = folder_by_id(active_folder);
    if (!f || f->closing) for (struct folder *d = folders; d && !f; d = d->next) if (d->desktop && !d->closing) f = d;
    const char **paths = NULL;
    char (*buf)[PATH_MAX] = NULL;
    size_t np = 0;
    if (f && !is_tree(f) && f->cont) {
        size_t want = ui_container_selected_count(f->cont);
        paths = calloc(want ? want : 1, sizeof *paths);
        buf = calloc(want ? want : 1, sizeof *buf);
        for (size_t i = 0; i < f->m.total && np < want; i++)
            if (ui_container_is_selected(f->cont, i)) {
                target_of(f, i, buf[np], sizeof buf[np]);
                paths[np] = buf[np];
                np++;
            }
    }
    svc_answer(ask, NULL, paths, np);
    free(paths);
    free(buf);
    return true;
}
