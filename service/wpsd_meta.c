/* wpsd: describe, stats, flush, sync, shutdown; volumes (scan, portable ones attached, vanished ones detached). */
#define _GNU_SOURCE
#include "wpsd.h"

/* ---- describe, stats, flush, sync, shutdown */

void do_describe(struct client *c, int64_t id, const char *name)
{
    wpo_class *cls = name ? wpo_find_class(name) : NULL;
    if (!cls) { reply_error(c, id, "not found", "no such class"); return; }
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, 4);
    cb_text(&b, "name"); cb_text(&b, cls->name);
    cb_text(&b, "parent"); cls->parent ? cb_text(&b, cls->parent->name) : cb_null(&b);
    cb_text(&b, "version"); {
        char v[32];
        snprintf(v, sizeof v, "%u.%u", cls->major, cls->minor);
        cb_text(&b, v);
    }
    cb_text(&b, "methods");
    cb_array(&b, cls->nslots);
    for (uint32_t s = 0; s < cls->nslots; s++) {
        cb_map(&b, 3);
        cb_text(&b, "name"); cb_text(&b, cls->minfo[s]->name);
        cb_text(&b, "sig"); cb_text(&b, cls->minfo[s]->sig);
        cb_text(&b, "async"); cb_bool(&b, (cls->minfo[s]->flags & WPO_M_ASYNC) != 0);
    }
    send_frame(c, &b);
    cb_free(&b);
}

void reply_simple_ok(struct simple *s, const char *result)
{
    if (!s->c->dead) {
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, s->id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result"); cb_text(&b, result);
        send_frame(s->c, &b);
        cb_free(&b);
    }
    client_unref(s->c);
    free(s);
}

void flushed(void *arg) { reply_simple_ok(arg, "flushed"); }

void stats_work(void *arg)
{
    struct stats_job *j = arg;
    j->rows = wps_db_count(db, "state", NULL);
    j->conflicts = wps_db_count(db, "conflicts", NULL);
}

void stats_done(void *arg)
{
    struct stats_job *j = arg;
    if (!j->c->dead) {
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, j->id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result");
        wps_tracker_stats ts;
        wps_tracker_get_stats(&ts);
        cb_map(&b, 13);
        cb_text(&b, "plugins"); wps_plugins_status(&b);
        cb_text(&b, "objects"); cb_uint(&b, wps_objects_count());
        cb_text(&b, "tracking"); cb_text(&b, ts.mode ? ts.mode : "none");
        cb_text(&b, "events"); cb_uint(&b, ts.events);
        cb_text(&b, "overflows"); cb_uint(&b, ts.overflows);
        cb_text(&b, "watched"); cb_uint(&b, ts.watched);
        cb_text(&b, "scans"); cb_uint(&b, ts.scans);
        cb_text(&b, "missing"); cb_uint(&b, ts.missing);
        cb_text(&b, "pending"); cb_uint(&b, wps_objects_pending());
        cb_text(&b, "rows"); cb_int(&b, j->rows);
        cb_text(&b, "conflicts"); cb_int(&b, j->conflicts);
        cb_text(&b, "identity"); cb_text(&b, env.identity);
        cb_text(&b, "writer"); cb_text(&b, env.writer);
        send_frame(j->c, &b);
        cb_free(&b);
    }
    client_unref(j->c);
    free(j);
}

static struct { char uuid[WPS_VOLID_MAX]; bool ro; char mnt[PATH_MAX]; } attached[MAX_ATTACHED];   /* under vol_mu */

/* Volumes let go of before an unmount (their database on them closed, or
 * the unmount fails "busy"): not attached again while they stay mounted. */
static char released[8][WPS_VOLID_MAX];

static pthread_mutex_t vol_mu = PTHREAD_MUTEX_INITIALIZER;

static bool is_released(const char *uuid)
{
    for (int i = 0; i < 8; i++) if (released[i][0] && !strcmp(released[i], uuid)) return true;
    return false;
}

static bool volume_job_running, volume_job_again;

static void volume_work(void *arg)
{
    struct vol_job *j = arg;
    if (wps_volumes_scan(&j->vs, &env) < 0) { snprintf(j->report, sizeof j->report, "cannot read mounts"); return; }
    pthread_mutex_lock(&vol_mu);
    /* released ones that are no longer mounted: done with */
    for (int i = 0; i < 8; i++)
        if (released[i][0] && !wps_volumes_by_uuid(&j->vs, released[i])) released[i][0] = 0;
    size_t off = 0;
    /* Detach volumes that are gone. */
    for (int a = 0; a < MAX_ATTACHED; a++) {
        if (!attached[a].uuid[0]) continue;
        const wps_volume *v = wps_volumes_by_uuid(&j->vs, attached[a].uuid);
        if (!v || !v->portable) {
            wps_db_detach_volume(db, attached[a].uuid);
            off += (size_t)snprintf(j->report + off, sizeof j->report - off, "detached %s; ", attached[a].uuid);
            attached[a].uuid[0] = 0;
        }
    }
    /* Attach new portable volumes, and re-attach those whose read-only
     * state changed (a remount): pull, then push what waited. */
    for (size_t i = 0; i < j->vs.n; i++) {
        const wps_volume *v = &j->vs.v[i];
        if (!v->portable || is_released(v->uuid)) continue;
        int slot = -1, free_slot = -1;
        for (int a = 0; a < MAX_ATTACHED; a++) {
            if (strcmp(attached[a].uuid, v->uuid) == 0) slot = a;
            else if (!attached[a].uuid[0] && free_slot < 0) free_slot = a;
        }
        if (slot >= 0 && attached[slot].ro == v->ro && j->reply == NULL) continue;
        char dir[PATH_MAX + 8], file[PATH_MAX + 64], err[256];
        snprintf(dir, sizeof dir, "%s/.wps", v->mnt);
        if (!v->ro && mkdir(dir, 01777) == 0) chmod(dir, 01777);   /* shared by all users, like /tmp */
        snprintf(file, sizeof file, "%s/%s.db", dir, env.identity);
        bool writable = !v->ro && access(dir, W_OK) == 0;
        wps_sync_stats st;
        if (wps_db_attach_volume(db, v->uuid, file, writable, &st, err, sizeof err) == 0) {
            int s = slot >= 0 ? slot : free_slot;
            if (s >= 0) {
                snprintf(attached[s].uuid, sizeof attached[s].uuid, "%s", v->uuid);
                snprintf(attached[s].mnt, sizeof attached[s].mnt, "%s", v->mnt);
                attached[s].ro = v->ro;
            }
            off += (size_t)snprintf(j->report + off, sizeof j->report - off,
                                    "%s %s%s: pulled %d, pushed %d, conflicts %d; ", slot >= 0 ? "synced" : "attached",
                                    v->uuid, writable ? "" : " (read-only)", st.pulled, st.pushed, st.conflicts);
        } else {
            off += (size_t)snprintf(j->report + off, sizeof j->report - off, "%s: %s; ", v->uuid, err);
        }
        if (off >= sizeof j->report) off = sizeof j->report - 1;
    }
    pthread_mutex_unlock(&vol_mu);
    if (!off) snprintf(j->report, sizeof j->report, "no portable volumes");
}

/* Let go of the volume mounted at `mnt` (its database closed) before it
 * is unmounted; worker thread. */
void volume_release(const char *mnt)
{
    pthread_mutex_lock(&vol_mu);
    for (int a = 0; a < MAX_ATTACHED; a++) {
        if (!attached[a].uuid[0] || strcmp(attached[a].mnt, mnt)) continue;
        wps_db_detach_volume(db, attached[a].uuid);
        log_msg("volumes: released %s for unmounting", attached[a].uuid);
        for (int i = 0; i < 8; i++)
            if (!released[i][0]) { snprintf(released[i], sizeof released[i], "%s", attached[a].uuid); break; }
        attached[a].uuid[0] = 0;
    }
    pthread_mutex_unlock(&vol_mu);
}

/* The unmount failed: it may be attached again. */
void volume_unrelease_all(void)
{
    pthread_mutex_lock(&vol_mu);
    for (int i = 0; i < 8; i++) released[i][0] = 0;
    pthread_mutex_unlock(&vol_mu);
}

static void volume_done(void *arg)
{
    struct vol_job *j = arg;
    wps_volumes_replace(&j->vs);
    static char last[sizeof j->report];   /* (logged when it changes: mounts rescan often) */
    if (strcmp(last, j->report) != 0) {
        log_msg("volumes: %s", j->report);
        snprintf(last, sizeof last, "%s", j->report);
    }
    if (strcmp(j->report, "no portable volumes") != 0) on_volumes_changed(j->report);
    if (j->reply) reply_simple_ok(j->reply, j->report);
    free(j);
    volume_job_running = false;
    if (volume_job_again) { volume_job_again = false; start_volume_job(NULL); }
}

void start_volume_job(struct simple *reply)
{
    if (volume_job_running && !reply) { volume_job_again = true; return; }
    struct vol_job *j = calloc(1, sizeof *j);
    j->reply = reply;
    volume_job_running = true;
    wpo_defer(loop, "volume scan", volume_work, volume_done, j);
}

void on_mounts(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    /* Re-read to clear the change notification, then rescan on a worker. */
    char buf[4096];
    lseek(fd, 0, SEEK_SET);
    while (read(fd, buf, sizeof buf) > 0) {}
    start_volume_job(NULL);
}

void scanned(void *arg, const char *report) { reply_simple_ok(arg, report); }
