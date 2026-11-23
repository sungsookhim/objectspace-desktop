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
        free(names);
        if (sid < 0) reply_error(c, id, "bad request", "select needs a folder, and all or items");
        else reply_int(c, id, sid);
    } else if (!strcmp(op, "move") || !strcmp(op, "copy")) {
        int64_t sel = -1;
        const char *dest = cv_text(msg, "dest");
        char err[128] = "";
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
        char err[128] = "";
        int64_t jid = cv_int(msg, "selection", &sel) ? wps_job_start(op, sel, "", err, sizeof err) : -1;
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
