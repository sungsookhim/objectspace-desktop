/* wps-shell: Delete and the System's confirmations; the service's jobs (progress, questions, errors). */
#define _GNU_SOURCE
#include "shell.h"

/* ---------------------------------------------------------------- delete */

/* Confirmations, as on OS/2's System object (its Confirmations page, read
 * with the other settings objects); <config>/system.conf, read first, is
 * 0.1's older place for them. Deleting for good always asks. */
bool confirm_delete = true, confirm_folder_delete = true;
bool hatch_open_all;

void load_system_conf(void)
{
    char path[4096];
    const char *cfg = getenv("WPS_CONFIG_DIR"), *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cfg && *cfg) snprintf(path, sizeof path, "%s/system.conf", cfg);
    else if (xdg && *xdg) snprintf(path, sizeof path, "%s/wps/system.conf", xdg);
    else snprintf(path, sizeof path, "%s/.config/wps/system.conf", home ? home : "");
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        char k[64], v[64];
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %63s", k, v) != 2) continue;
        bool on = !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
        if (!strcmp(k, "confirm_delete")) confirm_delete = on;
        else if (!strcmp(k, "confirm_folder_delete")) confirm_folder_delete = on;
        else fprintf(stderr, "wps-shell: %s: unknown setting \"%s\"\n", path, k);
    }
    fclose(fp);
}

static void del_free(struct del *d)
{
    while (d) {
        struct del *nx = d->next;
        for (size_t i = 0; i < d->n; i++) free(d->names[i]);
        for (size_t i = 0; i < d->nv; i++) free(d->vobjs[i]);
        free(d->names);
        free(d->vobjs);
        free(d);
        d = nx;
    }
}

/* The request's part for files in `parent` (one job per folder). */
static struct del *del_for(struct del *d, const char *parent)
{
    for (struct del *e = d;; e = e->next) {
        if (!e->n || !strcmp(e->parent, parent)) {
            if (!e->n) snprintf(e->parent, sizeof e->parent, "%s", parent);
            return e;
        }
        if (!e->next) {
            struct del *nd = calloc(1, sizeof *nd);
            nd->folder = d->folder;
            nd->permanent = d->permanent;
            e->next = nd;
        }
    }
}

static void push(char ***v, size_t *n, const char *s)
{
    *v = realloc(*v, (*n + 1) * sizeof **v);
    (*v)[(*n)++] = strdup(s);
}

static void vdeleted(void *arg, const cv *reply)
{
    long folder = (long)(intptr_t)arg;
    if (!reply || !cv_get(reply, "result")) {
        char m[300];
        snprintf(m, sizeof m, "Cannot delete: %s", reply ? cv_text(reply, "message") : "lost the connection to wpsd");
        set_status(folder, m);
    }
}

static void start_deletion(struct del *d)
{
    for (struct del *e = d; e; e = e->next) {
        /* central objects (shadows ...): into the Trash Can, or for good */
        for (size_t i = 0; i < e->nv; i++)
            (e->permanent ? svc_vdelete : svc_vtrash)(e->vobjs[i], vdeleted, (void *)(intptr_t)e->folder);
        if (e->n) {
            struct xfer *x = calloc(1, sizeof *x);
            x->folder = e->folder;
            snprintf(x->kind, sizeof x->kind, "%s", e->permanent ? "delete" : "trash");
            snprintf(x->from, sizeof x->from, "%s", e->parent);
            x->names = e->names;
            x->n = e->n;
            e->names = NULL;
            e->n = 0;
            svc_select(x->from, (const char *const *)x->names, x->n, got_sel, x);
        }
    }
    del_free(d);
}

static void delete_confirmed(void *arg, int choice)
{
    struct del *d = arg;
    last_dialog = NULL;
    if (choice == 0) start_deletion(d);
    else del_free(d);
}

/* Delete item i (the folder itself for SIZE_MAX): the selection if i is in
 * it, else i alone (OS/2). Into the trash, or for good. */
void delete_request(struct folder *f, size_t i, bool permanent)
{
    struct del *d = calloc(1, sizeof *d);
    d->folder = f->id;
    d->permanent = permanent;
    bool any_folder = false;
    char first[PATH_MAX] = "";
    size_t total = is_tree(f) ? f->ntvis : f->m.total;
    bool whole = i != SIZE_MAX && ui_container_is_selected(f->cont, i);
    /* a tree's selection: folders in other folders; an item whose folder is
     * selected too goes with it */
    char **sel = NULL;
    size_t nsel = 0;
    if (whole && is_tree(f))
        for (size_t k = 0; k < total; k++)
            if (ui_container_is_selected(f->cont, k)) {
                char t[PATH_MAX];
                target_of(f, k, t, sizeof t);
                push(&sel, &nsel, t);
            }
    for (size_t k = whole ? 0 : i; whole ? k < total : k == i; k++) {
        if (whole && !ui_container_is_selected(f->cont, k)) continue;
        char t[PATH_MAX];
        target_of(f, k, t, sizeof t);
        bool inside = false;
        for (size_t q = 0; q < nsel && !inside; q++) {
            size_t l = strlen(sel[q]);
            inside = l > 1 && !strncmp(t, sel[q], l) && t[l] == '/';
        }
        if (inside) continue;
        struct row *rw = row_at(f, k);
        const char *title = rw && rw->title ? rw->title : NULL;
        char p[PATH_MAX];   /* outlives the branches: title may point into it */
        if (!strncmp(t, "vobj:", 5)) {
            push(&d->vobjs, &d->nv, t);
        } else {
            /* the item's own path, not a shadow's or link's target */
            if (is_tree(f) || k == SIZE_MAX || !rw) snprintf(p, sizeof p, "%s", k == SIZE_MAX ? f->path : t);
            else snprintf(p, sizeof p, "%s/%s", strcmp(f->path, "/") ? f->path : "", rw->name);
            char *sl = strrchr(p, '/');
            if (!sl || !sl[1] || !strcmp(p, "/")) continue;
            char par[PATH_MAX];
            snprintf(par, sizeof par, "%.*s", sl == p ? 1 : (int)(sl - p), p);
            struct del *e = del_for(d, par);
            push(&e->names, &e->n, sl + 1);
            if (!title) title = sl + 1;
            any_folder |= k == SIZE_MAX || is_tree(f) || (rw && rw->kind == UI_KIND_FOLDER && !rw->shadow && !rw->link);
        }
        if (!first[0]) snprintf(first, sizeof first, "%s", title ? title : t);
        if (k == SIZE_MAX) break;
    }
    for (size_t q = 0; q < nsel; q++) free(sel[q]);
    free(sel);
    size_t n = 0;
    for (struct del *e = d; e; e = e->next) n += e->n + e->nv;
    if (!n) { del_free(d); return; }
    if (!permanent && !(any_folder ? confirm_folder_delete : confirm_delete)) { start_deletion(d); return; }
    char msg[700], what[400];
    if (n == 1) snprintf(what, sizeof what, "\"%s\"", first);
    else snprintf(what, sizeof what, "these %zu objects", n);
    if (permanent) snprintf(msg, sizeof msg, "Delete %s for good?\nThis cannot be undone.", what);
    else if (!d->n) snprintf(msg, sizeof msg, "Delete %s?", what);   /* shadows only: their originals stay */
    else snprintf(msg, sizeof msg, "Move %s to the trash?%s", what,
                  any_folder ? "\nFolders go with everything in them." : "");
    const char *buttons[] = { "Delete", "Cancel" };
    last_dialog = ask(permanent ? "Delete for good" : "Delete", msg, buttons, 2, 0, delete_confirmed, d);
}

/* Objects dropped on the Trash Can go to the trash (shadows are deleted,
 * as OS/2 did): one job per folder they came from. */
/* Over one of the icons being dragged (the Trash Can dragged a little
 * way): not a drop onto it. */
bool dragged_onto_itself(struct folder *f, size_t item)
{
    if (!cur_drag || item == SIZE_MAX || strcmp(cur_drag->src_folder, f->path)) return false;
    const char *nm = model_name(&f->m, item);
    if (!nm) return false;
    char p[PATH_MAX];
    if (nm[0] == '\x01') snprintf(p, sizeof p, "%s", nm + 1);   /* a central object: vobj:ID */
    else snprintf(p, sizeof p, "%s/%s", f->path, nm);
    for (size_t k = 0; k < cur_drag->n; k++)
        if (!strcmp(cur_drag->paths[k], p)) return true;
    return false;
}

static void startup_added(void *arg, const cv *reply)
{
    long folder = (long)(intptr_t)arg;
    const cv *ok = reply ? cv_get(reply, "ok") : NULL;
    if (ok && ok->k == CV_BOOL && ok->b) {
        set_status(folder, "Added to Startup: it starts (or opens) at login");
        raise_in(folder_by_id(folder), "object.create", NULL, "");
    } else {
        char m[600];
        snprintf(m, sizeof m, "Cannot add to Startup: %s", reply && cv_text(reply, "message") ? cv_text(reply, "message") : "lost the connection to wpsd");
        report_error(folder, m);
    }
}

/* Dropped on Startup: programs and scripts become autostart entries,
 * other objects shadows (wpsd decides); nothing moves. */
void startup_dropped(struct folder *f, const char *mime, const char *data)
{
    char *copy = strdup(data), *save = NULL;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char *tab = !strcmp(mime, MIME_URIS) ? NULL : strchr(line, '\t');
        if (tab) *tab = 0;
        char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
        if (!p) continue;
        svc_startup_add(p, startup_added, (void *)(intptr_t)f->id);
        free(p);
    }
    free(copy);
}

void trash_dropped(struct folder *f, const char *mime, const char *data)
{
    struct del *by[16];
    size_t nb = 0;
    char *copy = strdup(data), *save = NULL;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char *tab = !strcmp(mime, MIME_URIS) ? NULL : strchr(line, '\t');
        if (tab) *tab = 0;
        char *p = !strcmp(mime, MIME_URIS) ? uri_path(line) : strdup(line);
        if (!p) continue;
        if (!strncmp(p, "vobj:", 5)) {
            if (!nb) { by[0] = calloc(1, sizeof **by); by[0]->folder = f->id; nb = 1; }
            push(&by[0]->vobjs, &by[0]->nv, p);
            free(p);
            continue;
        }
        char *sl = strrchr(p, '/');
        if (!sl || sl == p || !sl[1]) { free(p); continue; }
        *sl = 0;
        struct del *d = NULL;
        for (size_t k = 0; k < nb && !d; k++) if (!strcmp(by[k]->parent, p) || (!by[k]->parent[0] && !by[k]->n)) d = by[k];
        if (!d && nb < 16) { d = by[nb++] = calloc(1, sizeof *d); d->folder = f->id; }
        if (d) {
            if (!d->parent[0]) snprintf(d->parent, sizeof d->parent, "%s", p);
            push(&d->names, &d->n, sl + 1);
        }
        free(p);
    }
    free(copy);
    for (size_t k = 0; k < nb; k++) start_deletion(by[k]);
    raise_in(f, "object.drop", "trash", "");
}

/* Folder windows on folders that are gone (deleted, or in the trash) close. */
static void close_gone_windows(void)
{
    for (struct folder *o = folders; o; o = o->next)
        if (!o->closing && (access(o->path, F_OK) != 0 || strstr(o->path, "/Trash/files/") || strstr(o->path, "/.Trash-")))
            win_close(o->win);
}

static void forgood_confirmed(void *arg, int choice)
{
    struct del *d = arg;
    last_dialog = NULL;
    if (choice == 0) { d->permanent = true; start_deletion(d); }
    else del_free(d);
}

/* A trash or delete job's progress; true when it has ended. */
static bool deletion_event(struct jobtrack *t, const cv *st, const char *state, int64_t done, int64_t total)
{
    bool trash = !strcmp(t->kind, "trash");
    char m[400];
    const char *err = cv_text(st, "error"), *cur = cv_text(st, "current");
    if (strcmp(state, "done") && strcmp(state, "failed") && strcmp(state, "cancelled")) {
        snprintf(m, sizeof m, "%s %lld of %lld%s%s", trash ? "Moving to the trash" : "Deleting", (long long)done,
                 (long long)total, cur && *cur ? ": " : "", cur ? cur : "");
        set_status(t->folder, m);
        return false;
    }
    const cv *nt = cv_get(st, "not_trashed"), *miss = cv_get(st, "missing");
    size_t nn = nt && nt->k == CV_ARRAY ? nt->n : 0, nm = miss && miss->k == CV_ARRAY ? miss->n : 0;
    long long ok = (long long)done - (long long)nn - (long long)nm;
    if (!strcmp(state, "done"))
        snprintf(m, sizeof m, "%s %lld object%s%s%s", trash ? "Moved to the trash:" : "Deleted", ok, ok == 1 ? "" : "s",
                 nm ? "; some were gone already" : "", nn ? "; some cannot go to the trash" : "");
    else if (!strcmp(state, "failed")) snprintf(m, sizeof m, "Delete failed: %s", err ? err : "unknown error");
    else snprintf(m, sizeof m, "Delete cancelled after %lld", (long long)done);
    set_status(t->folder, m);
    if (!strcmp(state, "failed")) {
        raise_in(folder_by_id(t->folder), "job.failed", NULL, "");
        messages_post(m, err ? err : "", 2);   /* kept in Messages (a failure only: not every delete) */
    }
    else if (!strcmp(state, "done") && ok > 0) raise_in(folder_by_id(t->folder), trash ? "object.trash" : "object.delete", NULL, "");
    close_gone_windows();
    if (nn) {
        /* No trash takes them (a drive without one, or the trash itself):
         * ask before deleting them for good. */
        struct del *d = calloc(1, sizeof *d);
        d->folder = t->folder;
        const char *from = cv_text(st, "from");
        snprintf(d->parent, sizeof d->parent, "%s", from ? from : "");
        for (size_t i = 0; i < nn; i++)
            if (nt->items[i].k == CV_TEXT) push(&d->names, &d->n, nt->items[i].s);
        char msg[700];
        if (d->n == 1) snprintf(msg, sizeof msg, "\"%s\" cannot go to the trash (it is in the trash already, or its drive has none).\nDelete it for good?", d->names[0]);
        else snprintf(msg, sizeof msg, "%zu objects cannot go to the trash (they are in the trash already, or their drive has none).\nDelete them for good?", d->n);
        const char *buttons[] = { "Delete", "Cancel" };
        last_dialog = ask("Delete for good", msg, buttons, 2, 1, forgood_confirmed, d);
    }
    return true;
}

void on_job_event(const cv *st)
{
    int64_t id, done = 0, total = 0;
    if (!st || !cv_int(st, "job", &id)) return;
    struct jobtrack *t = jobs, **pp = &jobs;
    for (; t && t->job != id; pp = &t->next, t = t->next) {}
    if (!t) return;   /* not one of ours */
    cv_int(st, "items_done", &done);
    cv_int(st, "items_total", &total);
    const char *state = cv_text(st, "state"), *cur = cv_text(st, "current"), *err = cv_text(st, "error");
    bool move = !strcmp(t->kind, "move");
    char m[400];
    if (!state) return;
    if (!strcmp(t->kind, "trash") || !strcmp(t->kind, "delete")) {
        if (deletion_event(t, st, state, done, total)) { *pp = t->next; free(t); }
        return;
    }
    if (!strcmp(state, "done") || !strcmp(state, "failed") || !strcmp(state, "cancelled")) {
        /* The service keeps where the objects were dropped as the job
         * ends: the windows on the destination read the positions again
         * (the new icons may have appeared, in the next free spot, before
         * their positions were kept). */
        for (struct folder *o = folders; o; o = o->next)
            if (!o->closing && t->dest[0] && !strcmp(o->path, t->dest) && !is_tree(o)) svc_layout(o->path, got_layout, as_arg(o));
        const cv *miss = cv_get(st, "missing"), *skip = cv_get(st, "skipped");
        size_t nm = miss && miss->k == CV_ARRAY ? miss->n : 0, ns = skip && skip->k == CV_ARRAY ? skip->n : 0;
        if (!strcmp(state, "done"))
            snprintf(m, sizeof m, "%s %lld object%s%s%s", move ? "Moved" : "Copied", (long long)(done - (int64_t)ns),
                     done - (int64_t)ns == 1 ? "" : "s", ns ? "; some skipped" : "", nm ? "; some were gone already" : "");
        else if (!strcmp(state, "failed")) snprintf(m, sizeof m, "%s failed: %s", move ? "Move" : "Copy", err ? err : "unknown error");
        else snprintf(m, sizeof m, "%s cancelled after %lld", move ? "Move" : "Copy", (long long)done);
        set_status(t->folder, m);
        raise_in(folder_by_id(t->folder), !strcmp(state, "failed") ? "job.failed" : "job.done", NULL, "");
        if (!strcmp(state, "failed")) messages_post(m, t->dest, 2);
        *pp = t->next;
        free(t);
        return;
    }
    if (!strcmp(state, "needs-decision") && !t->asking) {
        const char *clash = cv_text(st, "conflict"), *to = cv_text(st, "to");
        char msg[1200];
        snprintf(msg, sizeof msg, "An object named \"%s\" is already in %s.\nRename the one being %s, or skip it?",
                 clash ? clash : "?", to ? to : "the folder", move ? "moved" : "copied");
        const char *buttons[] = { "Rename", "Skip", "Rename all", "Skip all", "Cancel" };
        struct decide_ctx *d = malloc(sizeof *d);
        d->job = id;
        t->asking = true;
        last_dialog = ask(move ? "Move" : "Copy", msg, buttons, 5, 0, decided, d);
        return;
    }
    snprintf(m, sizeof m, "%s %lld of %lld%s%s", move ? "Moving" : "Copying", (long long)done, (long long)total,
             cur && *cur ? ": " : "", cur ? cur : "");
    set_status(t->folder, m);
}

static void src_item_pos(void *ctx, size_t i, double *x, double *y) { layout_pos(((struct folder *)ctx)->lay, i, x, y); }

static size_t src_items_in(void *ctx, ui_rect r, size_t *out, size_t max)
{
    return layout_items_in(((struct folder *)ctx)->lay, r.x, r.y, r.w, r.h, out, max);
}

static void src_content_size(void *ctx, double *w, double *h) { layout_size(((struct folder *)ctx)->lay, w, h); }

static double src_item_h(void *ctx, size_t i) { return layout_item_h(((struct folder *)ctx)->lay, i); }
static double src_item_w(void *ctx, size_t i) { return layout_item_w(((struct folder *)ctx)->lay, i); }

const ui_container_src folder_src = { src_count, src_item, src_visible, src_activate, src_menu, src_rename, src_expand,
                                             src_drag, src_item_pos, src_items_in, src_content_size, src_item_h, src_item_w };
