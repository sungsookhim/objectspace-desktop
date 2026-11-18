/*
 * wpsd - the per-user WPS object service (M2).
 *
 * Serves clients on $WPS_RUNTIME_DIR/wps.sock (or $WPS_SOCKET): each message
 * is a 4-byte big-endian length and a CBOR map. Requests:
 *   {op:"ping"}
 *   {op:"resolve", target:PATH|"id:ID"}
 *   {op:"call", target, method, args:[...], expect:N}  objects as {obj:PATH|"id:ID"}
 *   {op:"describe", class}
 *   {op:"flush"}       reply once every pending change is written
 *   {op:"sync"}        rescan volumes and synchronise portable ones
 *   {op:"scan"}        run the catch-up scan now
 *   {op:"shadow", target, folder}   create a shadow (managed symlink)
 *   {op:"create", folder, class, title, command}   a new object (file, folder,
 *                      .desktop program or .wpobj abstract object), never overwriting
 *   {op:"stats"}
 *   {op:"shutdown"}
 * Every request carries an "id" that the reply echoes; replies are
 * {id, ok:true, result, ...} or {id, ok:false, error, message}.
 *
 * Object access is two-phase: a worker reads the file and the database,
 * then the loop thread finds or creates the object and calls the method.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "cbor.h"
#include "db.h"
#include "env.h"
#include "objects.h"
#include "shadows.h"
#include "WPShadow.h"
#include "WPProgram.h"
#include "WPFolder.h"
#include "WPDataFile.h"
#include "tracker.h"
#include "volumes.h"
#include "wpo.h"
#include "wpo_loop.h"
#include "wps_classes.h"
#include "WPObject.h"

#define MAX_FRAME (1u << 20)
#define MAX_ARGS 16

static wps_env env;
static wps_db *db;
static wpo_loop *loop;
static char sock_path[PATH_MAX + 32];
static bool quitting;

static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wpsd: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------- clients */

struct client {
    struct client *next, *prev;   /* all open connections, for shutdown */
    int fd;
    int refs;          /* the connection plus requests in flight */
    bool dead;
    unsigned char *in;
    size_t inlen, incap;
    unsigned char *out;
    size_t outlen, outoff;
};

static void client_io(int fd, short rev, void *arg);
static struct client *clients;

static void client_unref(struct client *c)
{
    if (--c->refs) return;
    free(c->in);
    free(c->out);
    free(c);
}

static void client_close(struct client *c)
{
    if (c->dead) return;
    c->dead = true;
    if (c->prev) c->prev->next = c->next;
    else clients = c->next;
    if (c->next) c->next->prev = c->prev;
    wpo_loop_unwatch_fd(loop, c->fd);
    close(c->fd);
    client_unref(c);
}

static void client_flush(struct client *c)
{
    while (c->outoff < c->outlen) {
        ssize_t w = send(c->fd, c->out + c->outoff, c->outlen - c->outoff, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN) break;
            client_close(c);
            return;
        }
        c->outoff += (size_t)w;
    }
    if (c->outoff == c->outlen) c->outoff = c->outlen = 0;
    /* A client that stops reading gets a bounded buffer, then is dropped:
     * nobody else waits for it (design: Responsiveness). */
    if (c->outlen > 16u << 20) { client_close(c); return; }
    wpo_loop_watch_fd(loop, c->fd, POLLIN | (c->outlen ? POLLOUT : 0), client_io, c);
}

static void send_frame(struct client *c, cbuf *b)
{
    if (c->dead || b->oom) return;
    size_t need = c->outlen + 4 + b->n;
    unsigned char *n = realloc(c->out, need);
    if (!n) { client_close(c); return; }
    c->out = n;
    unsigned char *w = c->out + c->outlen;
    w[0] = (unsigned char)(b->n >> 24); w[1] = (unsigned char)(b->n >> 16);
    w[2] = (unsigned char)(b->n >> 8); w[3] = (unsigned char)b->n;
    memcpy(w + 4, b->d, b->n);
    c->outlen = need;
    client_flush(c);
}

static void reply_error(struct client *c, int64_t id, const char *error, const char *message)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 4);
    cb_text(&b, "id"); cb_int(&b, id);
    cb_text(&b, "ok"); cb_bool(&b, false);
    cb_text(&b, "error"); cb_text(&b, error);
    cb_text(&b, "message"); cb_text(&b, message ? message : error);
    send_frame(c, &b);
    cb_free(&b);
}

/* ------------------------------------------------------------- values */

static void put_object_depth(cbuf *b, wpo_object *o, int depth);

static void put_object(cbuf *b, wpo_object *o) { put_object_depth(b, o, 0); }

static void put_object_depth(cbuf *b, wpo_object *o, int depth)
{
    if (!o) { cb_null(b); return; }
    const char *path = wps_object_path(o);
    char *title = wpo_is_a(o, WPObjectClass) ? WPObject_wpQueryTitle(o) : NULL;
    bool shadow = wpo_is_a(o, WPShadowClass);
    sobj *rec = wps_sobj_of(o);
    bool missing = rec && rec->declared_class[0];
    cb_map(b, 6 + (shadow ? 2 : 0) + (missing ? 1 : 0));
    cb_text(b, "path"); path ? cb_text(b, path) : cb_null(b);
    cb_text(b, "id"); cb_text(b, wpo_is_a(o, WPObjectClass) ? WPObject_data(o)->id : "");
    cb_text(b, "class"); cb_text(b, o->cls->name);
    cb_text(b, "title"); title ? cb_text(b, title) : cb_null(b);
    cb_text(b, "changes"); cb_uint(b, wpo_is_a(o, WPObjectClass) ? WPObject_data(o)->changes : 0);
    cb_text(b, "trashed"); cb_bool(b, path && wps_in_trash(path));
    if (missing) { cb_text(b, "class_not_installed"); cb_text(b, rec->declared_class); }
    if (shadow) {
        wpo_object *t = WPShadow_data(o)->target;
        cb_text(b, "target");
        if (t && depth == 0) put_object_depth(b, t, 1);
        else cb_null(b);
        cb_text(b, "available"); cb_bool(b, t != NULL);
    }
    free(title);
}

static void put_value(cbuf *b, const wpo_value *v)
{
    switch (v->type) {
    case 'b': cb_bool(b, v->u.b); break;
    case 'i': cb_int(b, v->u.i); break;
    case 'd': cb_double(b, v->u.d); break;
    case 's': v->u.s ? cb_text(b, v->u.s) : cb_null(b); break;
    case 'o': put_object(b, v->u.o); break;
    case 'p': {
        const wpo_value *pv = wpo_pending_value(v->u.p);
        if (pv) put_value(b, pv);
        else cb_text(b, "<pending>");
        break;
    }
    default: cb_null(b); break;
    }
}

/* ------------------------------------------------------------ requests */

struct req {
    struct client *c;
    int64_t id;
    cv msg;
    const char *op;
    char target[PATH_MAX];
    wps_loaded tgt;
    wps_loaded shadow_target;    /* a shadow's target, when the target is a shadow */
    int nobj;
    char objpath[MAX_ARGS][PATH_MAX];
    wps_loaded *objs;
};

static void req_free(struct req *r)
{
    wps_loaded_free(&r->tgt);
    wps_loaded_free(&r->shadow_target);
    for (int i = 0; r->objs && i < r->nobj; i++) wps_loaded_free(&r->objs[i]);
    free(r->objs);
    cv_free(&r->msg);
    client_unref(r->c);
    free(r);
}

static void prepare_work(void *arg)    /* worker */
{
    struct req *r = arg;
    wps_load(r->target, false, &r->tgt);
    if (!r->tgt.err && r->tgt.shadow_of[0]) {
        char t[WPS_ID_LEN + 4];
        snprintf(t, sizeof t, "id:%s", r->tgt.shadow_of);
        wps_load(t, false, &r->shadow_target);   /* fails if its volume is not mounted */
    }
    for (int i = 0; i < r->nobj; i++) wps_load(r->objpath[i], true, &r->objs[i]);
}

static void reply_ok_object(struct req *r, wpo_object *o, const wpo_value *result)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 4);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    if (result) put_value(&b, result);
    else put_object(&b, o);
    cb_text(&b, "object"); put_object(&b, o);
    send_frame(r->c, &b);
    cb_free(&b);
}

static void do_call(struct req *r, wpo_object *o)
{
    const char *method = cv_text(&r->msg, "method");
    const cv *args = cv_get(&r->msg, "args");
    int64_t expect;
    if (!method) { reply_error(r->c, r->id, "bad request", "call needs a method"); return; }
    /* Optimistic concurrency (design: Concurrency): a write based on an
     * old view of the object is refused with the current values. */
    if (cv_int(&r->msg, "expect", &expect) && (uint64_t)expect != WPObject_data(o)->changes) {
        cbuf b;
        cb_init(&b);
        cb_map(&b, 5);
        cb_text(&b, "id"); cb_int(&b, r->id);
        cb_text(&b, "ok"); cb_bool(&b, false);
        cb_text(&b, "error"); cb_text(&b, "conflict");
        cb_text(&b, "message"); cb_text(&b, "the object changed since you last saw it");
        cb_text(&b, "object"); put_object(&b, o);
        send_frame(r->c, &b);
        cb_free(&b);
        return;
    }
    wpo_value av[MAX_ARGS];
    size_t na = 0;
    int oi = 0;
    for (size_t i = 0; args && args->k == CV_ARRAY && i < args->n && na < MAX_ARGS; i++) {
        const cv *a = &args->items[i];
        switch (a->k) {
        case CV_BOOL: av[na++] = wpo_v_bool(a->b); break;
        case CV_INT: av[na++] = wpo_v_int(a->i); break;
        case CV_FLOAT: av[na++] = wpo_v_double(a->d); break;
        case CV_TEXT: av[na++] = wpo_v_string(a->s); break;
        case CV_MAP: {
            wpo_object *ao = oi < r->nobj ? wps_object_from_loaded(&r->objs[oi]) : NULL;
            if (!ao) {
                reply_error(r->c, r->id, "not found", oi < r->nobj ? strerror(r->objs[oi].err) : "object");
                for (size_t k = 0; k < na; k++) wpo_value_clear(&av[k]);
                return;
            }
            oi++;
            av[na++] = wpo_v_object(ao);
            break;
        }
        default: av[na++] = wpo_v_void(); break;
        }
    }
    wpo_value ret;
    wpo_status st = wpo_send(o, method, av, na, &ret);
    for (size_t k = 0; k < na; k++) wpo_value_clear(&av[k]);
    if (st != WPO_OK) {
        reply_error(r->c, r->id, st == WPO_ENOMETHOD ? "no method" : st == WPO_EARGS ? "bad arguments" : "failed",
                    wpo_strerror(st));
        return;
    }
    reply_ok_object(r, o, &ret);
    wpo_value_clear(&ret);
}

static void prepare_done(void *arg)    /* loop */
{
    struct req *r = arg;
    if (r->c->dead) { req_free(r); return; }
    if (r->tgt.err) {
        reply_error(r->c, r->id, "not found", strerror(r->tgt.err));
        req_free(r);
        return;
    }
    wpo_object *o = wps_object_from_loaded(&r->tgt);
    if (o && wpo_is_a(o, WPShadowClass)) {
        wpo_object *t = r->tgt.shadow_of[0] && !r->shadow_target.err && r->shadow_target.path[0]
                            ? wps_object_from_loaded(&r->shadow_target) : NULL;
        WPShadow_wpSetTarget(o, t);   /* NULL: unavailable (volume not mounted, or gone) */
    }
    if (!o) reply_error(r->c, r->id, "failed", "cannot create the object");
    else if (strcmp(r->op, "resolve") == 0) reply_ok_object(r, o, NULL);
    else do_call(r, o);
    req_free(r);
}

/* ---- describe, stats, flush, sync, shutdown */

static void do_describe(struct client *c, int64_t id, const char *name)
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

struct simple { struct client *c; int64_t id; };

static void reply_simple_ok(struct simple *s, const char *result)
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

static void flushed(void *arg) { reply_simple_ok(arg, "flushed"); }

struct stats_job { struct client *c; int64_t id; int64_t rows, conflicts; };

static void stats_work(void *arg)
{
    struct stats_job *j = arg;
    j->rows = wps_db_count(db, "state", NULL);
    j->conflicts = wps_db_count(db, "conflicts", NULL);
}

static void stats_done(void *arg)
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
        cb_map(&b, 12);
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

/* ---- volumes: scan, attach portable ones, detach vanished ones */

#define MAX_ATTACHED 32
static struct { char uuid[WPS_VOLID_MAX]; bool ro; } attached[MAX_ATTACHED];   /* touched by volume jobs only */
static bool volume_job_running, volume_job_again;

struct vol_job {
    wps_volumes vs;
    struct simple *reply;     /* a "sync" request waiting, or NULL */
    char report[1024];
};

static void volume_work(void *arg)
{
    struct vol_job *j = arg;
    if (wps_volumes_scan(&j->vs, &env) < 0) { snprintf(j->report, sizeof j->report, "cannot read mounts"); return; }
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
        if (!v->portable) continue;
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
            if (s >= 0) { snprintf(attached[s].uuid, sizeof attached[s].uuid, "%s", v->uuid); attached[s].ro = v->ro; }
            off += (size_t)snprintf(j->report + off, sizeof j->report - off,
                                    "%s %s%s: pulled %d, pushed %d, conflicts %d; ", slot >= 0 ? "synced" : "attached",
                                    v->uuid, writable ? "" : " (read-only)", st.pulled, st.pushed, st.conflicts);
        } else {
            off += (size_t)snprintf(j->report + off, sizeof j->report - off, "%s: %s; ", v->uuid, err);
        }
        if (off >= sizeof j->report) off = sizeof j->report - 1;
    }
    if (!off) snprintf(j->report, sizeof j->report, "no portable volumes");
}

static void start_volume_job(struct simple *reply);

static void volume_done(void *arg)
{
    struct vol_job *j = arg;
    wps_volumes_replace(&j->vs);
    log_msg("volumes: %s", j->report);
    if (j->reply) reply_simple_ok(j->reply, j->report);
    free(j);
    volume_job_running = false;
    if (volume_job_again) { volume_job_again = false; start_volume_job(NULL); }
}

static void start_volume_job(struct simple *reply)
{
    if (volume_job_running && !reply) { volume_job_again = true; return; }
    struct vol_job *j = calloc(1, sizeof *j);
    j->reply = reply;
    volume_job_running = true;
    wpo_defer(loop, "volume scan", volume_work, volume_done, j);
}

static void on_mounts(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    /* Re-read to clear the change notification, then rescan on a worker. */
    char buf[4096];
    lseek(fd, 0, SEEK_SET);
    while (read(fd, buf, sizeof buf) > 0) {}
    start_volume_job(NULL);
}

static void scanned(void *arg, const char *report) { reply_simple_ok(arg, report); }

/* ---- create: a new object of a class in a folder, never overwriting */

struct create_req {
    struct client *c;
    int64_t id;
    char folder[PATH_MAX], cls[64], title[256], command[1024];
    char path[PATH_MAX + 64];
    int err;
    wps_loaded ld;
};

static void create_work(void *arg)
{
    struct create_req *r = arg;
    wpo_class *c = wpo_find_class(r->cls);   /* the registry is read-only after start-up */
    for (int n = 1; n < 100; n++) {
        wps_object_filename(r->folder, r->cls, r->title, n, r->path, sizeof r->path);
        int e;
        if (wpo_subclass_of(c, WPFolderClass)) e = mkdir(r->path, 0777) == 0 ? 0 : errno;
        else if (wpo_subclass_of(c, WPDataFileClass)) {
            int fd = open(r->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
            e = fd >= 0 ? 0 : errno;
            if (fd >= 0) close(fd);
        } else {
            e = wps_object_file_create(r->path, r->cls, r->title);
        }
        if (e == 0) { wps_load(r->path, false, &r->ld); return; }
        if (e != EEXIST) { r->err = e; return; }
    }
    r->err = EEXIST;
}

static void create_done(void *arg)
{
    struct create_req *r = arg;
    if (!r->c->dead) {
        wpo_object *o = r->err || r->ld.err ? NULL : wps_object_from_loaded(&r->ld);
        if (!o) {
            reply_error(r->c, r->id, "failed", strerror(r->err ? r->err : r->ld.err));
        } else {
            if (r->title[0]) WPObject_wpSetTitle(o, r->title);
            if (r->command[0] && wpo_is_a(o, WPProgramClass)) WPProgram_wpSetCommand(o, r->command);
            struct req fake = { .c = r->c, .id = r->id };
            reply_ok_object(&fake, o, NULL);
        }
    }
    wps_loaded_free(&r->ld);
    client_unref(r->c);
    free(r);
}

struct shadow_reply { struct simple *s; char link[PATH_MAX]; };

static void shadow_recorded(void *arg)
{
    struct shadow_reply *r = arg;
    reply_simple_ok(r->s, r->link);
    free(r);
}

static void shadow_made(void *arg, const char *link, const char *error)
{
    struct simple *s = arg;
    if (link) {
        /* Reply once the folder's record is written, so the shadow is a
         * shadow for everyone from the moment it is reported. */
        struct shadow_reply *r = calloc(1, sizeof *r);
        r->s = s;
        snprintf(r->link, sizeof r->link, "%s", link);
        wps_objects_flush(shadow_recorded, r);
        return;
    }
    if (!s->c->dead) reply_error(s->c, s->id, "failed", error);
    client_unref(s->c);
    free(s);
}

static void quit_after_flush(void *arg) { (void)arg; quitting = true; wpo_loop_quit(loop); }

static void shutdown_flushed(void *arg)
{
    reply_simple_ok(arg, "shutting down");
    quit_after_flush(NULL);
}

/* ---- dispatch */

static void handle(struct client *c, cv *msg)
{
    int64_t id = 0;
    cv_int(msg, "id", &id);
    const char *op = cv_text(msg, "op");
    if (!op) { reply_error(c, id, "bad request", "missing op"); cv_free(msg); return; }

    if (strcmp(op, "ping") == 0) {
        struct simple *s = calloc(1, sizeof *s);
        s->c = c; s->id = id; c->refs++;
        reply_simple_ok(s, "pong");
    } else if (strcmp(op, "describe") == 0) {
        do_describe(c, id, cv_text(msg, "class"));
    } else if (strcmp(op, "flush") == 0 || strcmp(op, "shutdown") == 0 || strcmp(op, "sync") == 0) {
        struct simple *s = calloc(1, sizeof *s);
        s->c = c; s->id = id; c->refs++;
        if (op[0] == 'f') wps_objects_flush(flushed, s);
        else if (op[1] == 'h') wps_objects_flush(shutdown_flushed, s);
        else start_volume_job(s);
    } else if (strcmp(op, "create") == 0) {
        const char *folder = cv_text(msg, "folder"), *clsname = cv_text(msg, "class");
        const char *title = cv_text(msg, "title"), *command = cv_text(msg, "command");
        wpo_class *cls = clsname ? wpo_find_class(clsname) : NULL;
        if (!folder || !cls || cls->is_meta || !wpo_subclass_of(cls, WPObjectClass) || wpo_subclass_of(cls, WPShadowClass)) {
            reply_error(c, id, "bad request", "create needs a folder and an object class (shadows: use shadow)");
            cv_free(msg);
            return;
        }
        struct create_req *r = calloc(1, sizeof *r);
        r->c = c; c->refs++;
        r->id = id;
        snprintf(r->folder, sizeof r->folder, "%s", folder);
        snprintf(r->cls, sizeof r->cls, "%s", clsname);
        snprintf(r->title, sizeof r->title, "%s", title ? title : "");
        snprintf(r->command, sizeof r->command, "%s", command ? command : "");
        wpo_defer(loop, "create object", create_work, create_done, r);
    } else if (strcmp(op, "shadow") == 0) {
        const char *t = cv_text(msg, "target"), *f = cv_text(msg, "folder");
        if (!t || !f) { reply_error(c, id, "bad request", "shadow needs target and folder"); cv_free(msg); return; }
        struct simple *s = calloc(1, sizeof *s);
        s->c = c; s->id = id; c->refs++;
        /* An ID of an object we hold: use where it is now. */
        sobj *known = strncmp(t, "id:", 3) == 0 ? wps_sobj_by_id(t + 3) : NULL;
        wps_shadow_create(known ? known->path : t, f, shadow_made, s);
    } else if (strcmp(op, "scan") == 0) {
        struct simple *s = calloc(1, sizeof *s);
        s->c = c; s->id = id; c->refs++;
        wps_tracker_scan(scanned, s);
    } else if (strcmp(op, "stats") == 0) {
        struct stats_job *j = calloc(1, sizeof *j);
        j->c = c; j->id = id; c->refs++;
        wpo_defer(loop, "stats", stats_work, stats_done, j);
    } else if (strcmp(op, "resolve") == 0 || strcmp(op, "call") == 0) {
        const char *target = cv_text(msg, "target");
        if (!target) { reply_error(c, id, "bad request", "missing target"); cv_free(msg); return; }
        struct req *r = calloc(1, sizeof *r);
        r->c = c; c->refs++;
        r->id = id;
        r->msg = *msg;     /* takes ownership */
        r->op = cv_text(&r->msg, "op");
        /* An ID of an object we already hold: use where it is now. */
        sobj *known = strncmp(target, "id:", 3) == 0 ? wps_sobj_by_id(target + 3) : NULL;
        snprintf(r->target, sizeof r->target, "%s", known ? known->path : target);
        const cv *args = cv_get(&r->msg, "args");
        for (size_t i = 0; args && args->k == CV_ARRAY && i < args->n; i++) {
            const char *p = cv_text(&args->items[i], "obj");
            if (p && r->nobj < MAX_ARGS) snprintf(r->objpath[r->nobj++], PATH_MAX, "%s", p);
        }
        r->objs = calloc((size_t)r->nobj + 1, sizeof *r->objs);
        wpo_defer(loop, op, prepare_work, prepare_done, r);
        return;
    } else {
        reply_error(c, id, "bad request", "unknown op");
    }
    cv_free(msg);
}

static void client_io(int fd, short rev, void *arg)
{
    struct client *c = arg;
    if (rev & POLLOUT) client_flush(c);
    if (c->dead || !(rev & (POLLIN | POLLHUP | POLLERR))) return;
    for (;;) {
        if (c->inlen == c->incap) {
            size_t ncap = c->incap ? c->incap * 2 : 4096;
            if (ncap > MAX_FRAME + 4) ncap = MAX_FRAME + 4;
            if (ncap == c->incap) { client_close(c); return; }   /* frame too big */
            unsigned char *n = realloc(c->in, ncap);
            if (!n) { client_close(c); return; }
            c->in = n;
            c->incap = ncap;
        }
        ssize_t r = recv(fd, c->in + c->inlen, c->incap - c->inlen, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { client_close(c); return; }
        if (r < 0) break;
        c->inlen += (size_t)r;
    }
    size_t o = 0;
    c->refs++;   /* keep c alive while dispatching */
    while (!c->dead && c->inlen - o >= 4) {
        uint32_t n = (uint32_t)c->in[o] << 24 | (uint32_t)c->in[o + 1] << 16 | (uint32_t)c->in[o + 2] << 8 | c->in[o + 3];
        if (n > MAX_FRAME) { client_close(c); break; }
        if (c->inlen - o - 4 < n) break;
        cv msg;
        if (cv_decode(c->in + o + 4, n, &msg) == 0) handle(c, &msg);
        else reply_error(c, 0, "bad request", "malformed message");
        o += 4 + n;
    }
    if (!c->dead && o) {
        memmove(c->in, c->in + o, c->inlen - o);
        c->inlen -= o;
    }
    client_unref(c);
}

static void on_accept(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0) return;
    struct client *c = calloc(1, sizeof *c);
    c->fd = cfd;
    c->refs = 1;
    c->next = clients;
    if (clients) clients->prev = c;
    clients = c;
    wpo_loop_watch_fd(loop, cfd, POLLIN, client_io, c);
}

static void on_signal(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    struct signalfd_siginfo si;
    ssize_t r = read(fd, &si, sizeof si);
    (void)r;
    log_msg("signal %u: writing pending changes and stopping", si.ssi_signo);
    wps_objects_flush(quit_after_flush, NULL);
}

static void on_watchdog(const char *label, double ms, void *user)
{
    (void)user;
    log_msg("watchdog: \"%s\" held the loop for %.1f ms", label, ms);
}

/* ----------------------------------------------------------------- main */

int main(void)
{
    char err[512];
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (wps_env_load(&env, err, sizeof err) < 0) { log_msg("%s", err); return 1; }

    /* One service per user (design: Concurrency). */
    char lockpath[PATH_MAX + 16];
    snprintf(lockpath, sizeof lockpath, "%s/wpsd.lock", env.state_dir);
    int lockfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
        log_msg("another wpsd is already running for this user (%s)", lockpath);
        return 1;
    }

    char dbpath[PATH_MAX + 16];
    snprintf(dbpath, sizeof dbpath, "%s/objects.db", env.state_dir);
    db = wps_db_open(dbpath, env.writer, err, sizeof err);
    if (!db) { log_msg("%s", err); return 1; }
    if (wpo_init() != WPO_OK || wps_classes_register() != WPO_OK) { log_msg("class registration failed"); return 1; }

    loop = wpo_loop_new(4);
    wpo_loop_set_watchdog(loop, 50.0, on_watchdog, NULL);
    const char *delay = getenv("WPS_SAVE_DELAY_MS");
    wps_objects_init(&env, db, loop, delay ? atoi(delay) : 2000);

    const char *sp = getenv("WPS_SOCKET");
    if (sp && *sp) snprintf(sock_path, sizeof sock_path, "%s", sp);
    else snprintf(sock_path, sizeof sock_path, "%s/wps.sock", env.runtime_dir);
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock_path);
    unlink(sock_path);   /* we hold the lock, so any old socket is stale */
    mode_t old = umask(0077);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 64) < 0) {
        log_msg("%s: %s", sock_path, strerror(errno));
        return 1;
    }
    umask(old);
    wpo_loop_watch_fd(loop, ls, POLLIN, on_accept, NULL);

    int mfd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mfd >= 0) wpo_loop_watch_fd(loop, mfd, POLLPRI, on_mounts, NULL);

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    int sfd = signalfd(-1, &sigs, SFD_CLOEXEC | SFD_NONBLOCK);
    wpo_loop_watch_fd(loop, sfd, POLLIN, on_signal, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Know the volumes before the first request; attach portable ones in
     * the background. */
    wps_volumes vs;
    if (wps_volumes_scan(&vs, &env) == 0) wps_volumes_replace(&vs);
    start_volume_job(NULL);
    wps_shadows_init(loop, db);
    wps_tracker_init(loop, &env, db);
    wps_tracker_stats ts;
    wps_tracker_get_stats(&ts);
    log_msg("tracking changes with %s", ts.mode);
    log_msg("ready: socket %s, identity %s, host %s", sock_path, env.identity, env.host);
    wpo_loop_run(loop);

    wpo_loop_run_until_idle(loop, 10000);
    while (clients) client_close(clients);
    wps_tracker_shutdown();
    unlink(sock_path);
    wps_objects_shutdown();
    wpo_loop_free(loop);
    wps_db_close(db);
    wpo_shutdown();
    log_msg("stopped");
    return quitting ? 0 : 1;
}
