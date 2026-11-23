/* wpsd: Selections and jobs. */
#define _GNU_SOURCE
#include "wpsd.h"

/* ---- selections and jobs */

void reply_value_cbor(struct client *c, int64_t id, cbuf *val)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    if (val->n) {
        unsigned char *n = realloc(b.d, b.n + val->n);
        if (n) { b.d = n; memcpy(b.d + b.n, val->d, val->n); b.n += val->n; b.cap = b.n; }
    } else {
        cb_null(&b);
    }
    send_frame(c, &b);
    cb_free(&b);
}

void reply_int(struct client *c, int64_t id, int64_t v)
{
    cbuf val;
    cb_init(&val);
    cb_int(&val, v);
    reply_value_cbor(c, id, &val);
    cb_free(&val);
}

/* The last selections' named items (not "all"), for the actions of their
 * objects before they go to the Trash Can or are deleted (on:object.trash,
 * on:object.delete; object-properties-design.md 2.4). */
#define SEL_KEPT 8
#define SEL_NAMES 64
static struct { int64_t sid; char folder[PATH_MAX]; char *names[SEL_NAMES]; size_t n; } kept[SEL_KEPT];
static size_t kept_next;

static void keep_selection(int64_t sid, const char *folder, const char *const *names, size_t n)
{
    if (n > SEL_NAMES) return;   /* (many: no actions, as for "all") */
    typeof(kept[0]) *k = &kept[kept_next++ % SEL_KEPT];
    for (size_t i = 0; i < k->n; i++) free(k->names[i]);
    k->sid = sid;
    snprintf(k->folder, sizeof k->folder, "%s", folder);
    k->n = 0;
    for (size_t i = 0; i < n; i++) k->names[k->n++] = strdup(names[i]);
}

static void fire_selection(struct client *c, int64_t sid, const char *event)
{
    for (size_t s = 0; s < SEL_KEPT; s++) {
        if (kept[s].sid != sid || !kept[s].n) continue;
        for (size_t i = 0; i < kept[s].n; i++) {
            char path[PATH_MAX * 2];
            snprintf(path, sizeof path, "%s/%s", kept[s].folder, kept[s].names[i]);
            wps_loaded ld;
            wps_load(path, false, &ld);
            wpo_object *o = ld.err ? NULL : wps_object_from_loaded(&ld);   /* (borrowed) */
            wps_loaded_free(&ld);
            if (o) free(fire_event_for(c, o, path, event));   /* (the job has its own sound) */
        }
        return;
    }
}

bool no_delete_of(const char *target, char *why, size_t n)
{
    wps_loaded ld;
    wps_load(target, false, &ld);
    wpo_object *o = ld.err ? NULL : wps_object_from_loaded(&ld);   /* (borrowed) */
    char orig[WPS_ID_LEN + 8] = "";
    if (!ld.err && ld.shadow_of[0]) snprintf(orig, sizeof orig, "id:%s", ld.shadow_of);   /* (its original, by ID) */
    wps_loaded_free(&ld);
    if (!o || !wpo_is_a(o, WPObjectClass)) return false;
    char *v = WPObject_wpQueryProperty(o, "no_delete");
    bool on = v && (!strcmp(v, "1") || !strcmp(v, "true"));
    free(v);
    if (on) {
        const char *t = WPObject_data(o)->title, *b = strrchr(target, '/');
        if ((!t || !*t) && orig[0]) {   /* a shadow: its original's title */
            wps_load(orig, false, &ld);
            wpo_object *so = ld.err ? NULL : wps_object_from_loaded(&ld);
            wps_loaded_free(&ld);
            if (so && wpo_is_a(so, WPObjectClass)) t = WPObject_data(so)->title;
        }
        snprintf(why, n, "%s cannot be deleted: its General page's Prevent delete is on", t && *t ? t : b ? b + 1 : target);
    }
    return on;
}

struct nd { char *why; size_t n; bool hit; };

static bool nd_one(const char *path, void *u)
{
    struct nd *x = u;
    x->hit = no_delete_of(path, x->why, x->n);
    return !x->hit;   /* (the first one stops the walk) */
}

bool no_delete_in(int64_t selection, char *why, size_t n)
{
    struct nd x = { why, n, false };
    wps_selection_each(selection, nd_one, &x);
    return x.hit;
}

bool handle_jobs(struct client *c, int64_t id, const char *op, cv *msg)
{
    int64_t job = -1;
    cv_int(msg, "job", &job);
    if (!strcmp(op, "select")) {
        const char *folder = cv_text(msg, "folder");
        const cv *items = cv_get(msg, "items"), *all = cv_get(msg, "all"), *except = cv_get(msg, "except");
        bool is_all = all && all->k == CV_BOOL && all->b;
        const cv *list = is_all ? except : items;
        size_t n = list && list->k == CV_ARRAY ? list->n : 0;
        const char **names = calloc(n + 1, sizeof *names);
        size_t k = 0;
        for (size_t i = 0; i < n; i++)
            if (list->items[i].k == CV_TEXT) names[k++] = list->items[i].s;
        int64_t sid = folder ? wps_select(folder, is_all, names, k) : -1;
        if (sid >= 0 && !is_all) keep_selection(sid, folder, names, k);
        free(names);
        if (sid < 0) reply_error(c, id, "bad request", "select needs a folder, and all or items");
        else reply_int(c, id, sid);
    } else if (!strcmp(op, "move") || !strcmp(op, "copy")) {
        int64_t sel = -1;
        const char *dest = cv_text(msg, "dest");
        char err[128] = "", why[600];
        /* (a move into the Trash Can is a trash: refused for what may not be deleted) */
        if (cv_int(msg, "selection", &sel) && dest && !strcmp(op, "move") && wps_is_trash_files_dir(dest) &&
            no_delete_in(sel, why, sizeof why)) {
            reply_error(c, id, "protected", why);
            return true;
        }
        int64_t jid = cv_int(msg, "selection", &sel) && dest ? wps_job_start(op, sel, dest, err, sizeof err) : -1;
        const cv *pos = cv_get(msg, "positions");   /* [{name, x, y}]: where dropped items go */
        if (jid >= 0 && pos && pos->k == CV_ARRAY && pos->n) {
            const char **names = calloc(pos->n, sizeof *names);
            int64_t *x = calloc(pos->n, sizeof *x), *y = calloc(pos->n, sizeof *y);
            size_t k = 0;
            for (size_t i = 0; i < pos->n; i++) {
                const char *nm = cv_text(&pos->items[i], "name");
                if (!nm) continue;
                names[k] = nm;
                cv_int(&pos->items[i], "x", &x[k]);
                cv_int(&pos->items[i], "y", &y[k]);
                k++;
            }
            wps_job_set_positions(jid, names, x, y, k);
            free(names);
            free(x);
            free(y);
        }
        if (jid < 0) reply_error(c, id, "bad request", err[0] ? err : "move/copy needs a selection and a dest");
        else reply_int(c, id, jid);
    } else if (!strcmp(op, "trash") || !strcmp(op, "delete")) {
        /* trash: into the trash of each item's filesystem; delete: for good */
        int64_t sel = -1;
        char err[128] = "", why[600];
        if (cv_int(msg, "selection", &sel) && no_delete_in(sel, why, sizeof why)) {   /* (all refused: nothing goes) */
            reply_error(c, id, "protected", why);
            return true;
        }
        if (sel >= 0) fire_selection(c, sel, !strcmp(op, "trash") ? "object.trash" : "object.delete");
        int64_t jid = sel >= 0 ? wps_job_start(op, sel, "", err, sizeof err) : -1;
        if (jid < 0) reply_error(c, id, "bad request", err[0] ? err : "trash/delete needs a selection");
        else reply_int(c, id, jid);
    } else if (!strcmp(op, "job") || !strcmp(op, "jobs")) {
        cbuf val;
        cb_init(&val);
        if (wps_job_status(op[3] == 's' ? -1 : job, &val)) reply_value_cbor(c, id, &val);
        else reply_error(c, id, "not found", "no such job");
        cb_free(&val);
    } else if (!strcmp(op, "job_cancel") || !strcmp(op, "job_pause") || !strcmp(op, "job_resume") ||
               !strcmp(op, "job_decide")) {
        bool ok;
        if (op[4] == 'c') ok = wps_job_cancel(job);
        else if (op[4] == 'p') ok = wps_job_pause(job);
        else if (op[4] == 'r') ok = wps_job_resume(job);
        else {
            const cv *all = cv_get(msg, "all");
            const char *d = cv_text(msg, "decision");
            ok = d && wps_job_decide(job, d, all && all->k == CV_BOOL && all->b);
        }
        if (ok) reply_int(c, id, job);
        else reply_error(c, id, "bad request", "no such job, or not in a state for that");
    } else {
        return false;
    }
    return true;
}
