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
 *   {op:"list", target, offset, count, details}   a page of a folder (streamed)
 *   {op:"select", folder, all, except|items}   a selection; returns its ID
 *   {op:"move"|"copy", selection, dest}        a job; returns its ID
 *   {op:"job", job} {op:"jobs"} {op:"job_cancel"|"job_pause"|"job_resume", job}
 *   {op:"job_decide", job, decision:"skip"|"rename", all}
 *   {op:"subscribe", objects:[...], folders:[...], jobs} -> sub; events follow:
 *       {event:"object_changed"|"content_added"|"content_removed"|"object_moved"|"job"|"volumes", sub, ...}
 *   {op:"unsubscribe", sub}
 *   {op:"menu"|"settings", target}            menus and settings pages as data
 *   {op:"menu_select", target, item}  {op:"settings_set", target, values:{key:value}, expect}
 *   {op:"open", target, confirm}   start a program (as this user) or open a file;
 *                      a program owned by another user (not root) needs confirm
 *   {op:"from_template", target, folder, title}
 *   {op:"layout", target:FOLDER} -> {positions:{ID:"x,y"}, items:[{name,id,x,y}]}
 *   {op:"place", folder, items:[{target, x, y}]} -> [ID...]  set positions
 *   {op:"arrange", target:FOLDER}   forget the folder's positions
 *   {op:"rename", target, name}   rename a file or folder in its folder (never
 *                      replacing); its title then follows the name. Without
 *                      permission to rename, the name becomes its title
 *                      (shared or private, as titles are). Others: title.
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
#include <dirent.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <spawn.h>
#include <sys/xattr.h>

#include "cbor.h"
#include "db.h"
#include "env.h"
#include "objects.h"
#include "jobs.h"
#include "listing.h"
#include "plugins.h"
#include "shadows.h"
#include "WPShadow.h"
#include "WPProgram.h"
#include "WPFolder.h"
#include "WPDataFile.h"
#include "wps_state.h"
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

static void subs_drop_client(struct client *c);
static void on_volumes_changed(const char *report);
static char *call_text(wpo_object *o, const char *method, const char *arg);
static const char *apply_settings(wpo_object *o, const cv *values);
static void put_menu(cbuf *b, const char *text, const char *parent);
static void put_settings(cbuf *b, const char *text);
struct req;
static void do_open(struct req *r, wpo_object *o);
static void do_from_template(struct req *r, wpo_object *o);
static void do_rename(struct req *r, wpo_object *o);
static void do_layout(struct req *r, wpo_object *o);
static void do_arrange(struct req *r, wpo_object *o);
static void start_place(struct client *c, int64_t id, const cv *msg);

static void client_close(struct client *c)
{
    if (c->dead) return;
    c->dead = true;
    subs_drop_client(c);
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
    char *remote_text;           /* a plug-in's answer, for menus */
    int nobj;
    char objpath[MAX_ARGS][PATH_MAX];
    wps_loaded *objs;
};

static void req_free(struct req *r)
{
    wps_loaded_free(&r->tgt);
    wps_loaded_free(&r->shadow_target);
    free(r->remote_text);
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

static void req_free(struct req *r);

/* A method implemented by a plug-in: run in its host; reply when it
 * answers, or with the parent's behaviour if it misses its deadline. */
struct remote_ctx { struct req *r; wpo_object *o; };

static void local_call_reply(struct req *r, wpo_object *o);

static void remote_done(void *arg, const cv *result)
{
    struct remote_ctx *x = arg;
    struct req *r = x->r;
    if (!r->c->dead) {
        if (!result) {
            local_call_reply(r, x->o);   /* deadline passed or host failed: parent's behaviour */
        } else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 5);
            cb_text(&b, "id"); cb_int(&b, r->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result");
            cbuf j;
            cb_init(&j);
            switch (result->k) {
            case CV_BOOL: cb_bool(&b, result->b); break;
            case CV_INT: cb_int(&b, result->i); break;
            case CV_FLOAT: cb_double(&b, result->d); break;
            case CV_TEXT: cb_textn(&b, result->s, result->len); break;
            default: cb_null(&b); break;
            }
            cb_free(&j);
            cb_text(&b, "plugin"); cb_bool(&b, true);
            cb_text(&b, "object"); put_object(&b, x->o);
            send_frame(r->c, &b);
            cb_free(&b);
        }
    }
    wpo_unref(x->o);
    free(x);
    req_free(r);
}

/* Returns true if the reply will come later (the request is kept). */
static bool do_call(struct req *r, wpo_object *o)
{
    const char *method = cv_text(&r->msg, "method");
    int64_t ex;
    if (method && wps_plugin_routes(o, method) &&
        !(cv_int(&r->msg, "expect", &ex) && (uint64_t)ex != WPObject_data(o)->changes)) {
        struct remote_ctx *x = calloc(1, sizeof *x);
        x->r = r;
        x->o = wpo_ref(o);
        wps_plugin_call(o, wps_object_path(o), method, cv_get(&r->msg, "args"), 2000, remote_done, x);
        return true;
    }
    local_call_reply(r, o);
    return false;
}

static void local_call_reply(struct req *r, wpo_object *o)
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

static void do_menus(struct req *r, wpo_object *o)
{
    const char *err = NULL;
    int64_t v;
    if (!strcmp(r->op, "menu_select")) {
        wpo_value a, ret;
        a = wpo_v_int(cv_int(&r->msg, "item", &v) ? v : -1);
        bool ok = wpo_send(o, "wpMenuItemSelected", &a, 1, &ret) == WPO_OK && ret.type == 'b' && ret.u.b;
        wpo_value_clear(&ret);
        if (!ok) { reply_error(r->c, r->id, "failed", "the item was not handled"); return; }
    } else if (!strcmp(r->op, "settings_set")) {
        if (cv_int(&r->msg, "expect", &v) && (uint64_t)v != WPObject_data(o)->changes) err = "conflict";
        else err = apply_settings(o, cv_get(&r->msg, "values"));
        if (err) {
            reply_error(r->c, r->id, !strcmp(err, "conflict") ? "conflict" : "bad request", err);
            return;
        }
    }
    cbuf b;
    cb_init(&b);
    cb_map(&b, 4);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    if (!strcmp(r->op, "menu") || !strcmp(r->op, "menu_select")) {
        char *t = r->remote_text ? strdup(r->remote_text) : call_text(o, "wpModifyPopupMenu", "");
        put_menu(&b, t, "");
        free(t);
    } else {
        char *t = call_text(o, "wpAddSettingsPages", "");
        put_settings(&b, t);
        free(t);
    }
    cb_text(&b, "object"); put_object(&b, o);
    send_frame(r->c, &b);
    cb_free(&b);
}

static void menu_remote_done(void *arg, const cv *result)
{
    struct remote_ctx *x = arg;
    struct req *r = x->r;
    if (result && result->k == CV_TEXT) r->remote_text = strdup(result->s);
    /* With no answer in time, the parent class's menu is used. */
    if (!r->c->dead) do_menus(r, x->o);
    wpo_unref(x->o);
    free(x);
    req_free(r);
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
    else if (!strcmp(r->op, "menu") && wps_plugin_routes(o, "wpModifyPopupMenu")) {
        /* A plug-in builds this menu: ask its host, with a deadline. */
        struct remote_ctx *x = calloc(1, sizeof *x);
        x->r = r;
        x->o = wpo_ref(o);
        cv args = { .k = CV_ARRAY, .n = 1, .nitems = 1 };
        cv empty = { .k = CV_TEXT, .s = "", .len = 0 };
        args.items = &empty;
        wps_plugin_call(o, wps_object_path(o), "wpModifyPopupMenu", &args, 2000, menu_remote_done, x);
        return;
    }
    else if (!strcmp(r->op, "menu") || !strcmp(r->op, "settings") || !strcmp(r->op, "settings_set") ||
             !strcmp(r->op, "menu_select")) do_menus(r, o);
    else if (!strcmp(r->op, "open")) do_open(r, o);
    else if (!strcmp(r->op, "from_template")) do_from_template(r, o);
    else if (!strcmp(r->op, "rename")) do_rename(r, o);
    else if (!strcmp(r->op, "layout")) do_layout(r, o);
    else if (!strcmp(r->op, "arrange")) do_arrange(r, o);
    else if (do_call(r, o)) return;   /* the reply comes from the plug-in */
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
    if (strcmp(j->report, "no portable volumes") != 0) on_volumes_changed(j->report);
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

/* ---- subscriptions and events (design: Service API, Events) */

struct sub {
    struct sub *next;
    int64_t id;
    struct client *c;
    char **targets;        /* object IDs, or paths */
    size_t ntargets;
    char **folders;
    size_t nfolders;
    bool jobs;
};

static struct sub *subs;
static int64_t next_sub = 1;

static void subs_drop_client(struct client *c)
{
    for (struct sub **pp = &subs; *pp;) {
        struct sub *x = *pp;
        if (x->c != c) { pp = &x->next; continue; }
        *pp = x->next;
        for (size_t i = 0; i < x->ntargets; i++) free(x->targets[i]);
        for (size_t i = 0; i < x->nfolders; i++) free(x->folders[i]);
        free(x->targets);
        free(x->folders);
        free(x);
    }
}

static bool in_folder(const char *path, const char *folder)
{
    size_t l = strlen(folder);
    if (strncmp(path, folder, l) != 0 || path[l] != '/') return false;
    return strchr(path + l + 1, '/') == NULL;   /* a direct entry */
}

/* Start an event frame: {event, sub, ...rest}; the caller adds `extra`
 * key/value pairs and sends. */
static void event_head(cbuf *b, const char *name, int64_t sub, size_t extra)
{
    cb_init(b);
    cb_map(b, 2 + extra);
    cb_text(b, "event"); cb_text(b, name);
    cb_text(b, "sub"); cb_int(b, sub);
}

static void on_object_changed(wpo_object *o)
{
    sobj *rec = wps_sobj_of(o);
    const char *path = rec ? rec->path : wps_object_path(o);
    const char *id = wpo_is_a(o, WPObjectClass) ? WPObject_data(o)->id : "";
    for (struct sub *x = subs; x; x = x->next) {
        bool hit = false;
        for (size_t i = 0; i < x->ntargets && !hit; i++)
            hit = (id[0] && !strcmp(x->targets[i], id)) || (path && !strcmp(x->targets[i], path));
        for (size_t i = 0; i < x->nfolders && !hit && path; i++) hit = in_folder(path, x->folders[i]);
        if (!hit || x->c->dead) continue;
        cbuf b;
        event_head(&b, "object_changed", x->id, 1);
        cb_text(&b, "object"); put_object(&b, o);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

static void on_fs_event(char kind, const char *from, const char *path)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        for (size_t i = 0; i < x->nfolders; i++) {
            const char *f = x->folders[i];
            bool in_new = path[0] && in_folder(path, f), in_old = from[0] && in_folder(from, f);
            if (!in_new && !in_old) continue;
            cbuf b;
            if (kind == 'm' && in_new && in_old) {
                event_head(&b, "object_moved", x->id, 3);
                cb_text(&b, "folder"); cb_text(&b, f);
                cb_text(&b, "from"); cb_text(&b, strrchr(from, '/') + 1);
                cb_text(&b, "to"); cb_text(&b, strrchr(path, '/') + 1);
            } else {
                bool added = kind == 'c' || (kind == 'm' && in_new);
                const char *p = added ? path : (kind == 'm' ? from : path);
                event_head(&b, added ? "content_added" : "content_removed", x->id, 2);
                cb_text(&b, "folder"); cb_text(&b, f);
                cb_text(&b, "name"); cb_text(&b, strrchr(p, '/') + 1);
            }
            send_frame(x->c, &b);
            cb_free(&b);
        }
    }
}

static void on_job(int64_t jid)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (!x->jobs || x->c->dead) continue;
        cbuf st;
        cb_init(&st);
        if (!wps_job_status(jid, &st)) { cb_free(&st); return; }
        cbuf b;
        event_head(&b, "job", x->id, 1);
        cb_text(&b, "status");
        unsigned char *n = realloc(b.d, b.n + st.n);
        if (n) { b.d = n; memcpy(b.d + b.n, st.d, st.n); b.n += st.n; b.cap = b.n; }
        send_frame(x->c, &b);
        cb_free(&b);
        cb_free(&st);
    }
}

static void on_volumes_changed(const char *report)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        cbuf b;
        event_head(&b, "volumes", x->id, 1);
        cb_text(&b, "report"); cb_text(&b, report);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

static void strlist(const cv *a, char ***out, size_t *n)
{
    *out = NULL;
    *n = 0;
    if (!a || a->k != CV_ARRAY) return;
    *out = calloc(a->n + 1, sizeof **out);
    for (size_t i = 0; i < a->n; i++)
        if (a->items[i].k == CV_TEXT) {
            const char *t = a->items[i].s;
            if (!strncmp(t, "id:", 3)) t += 3;
            (*out)[(*n)++] = strdup(t);
        }
}

/* ---- menus and settings pages as data */

/* Menu text: id TAB label [TAB parent [TAB flags]] per line, as a tree. */
static void put_menu(cbuf *b, const char *text, const char *parent)
{
    size_t n = 0;
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char line[512], *f[4] = { 0 };
        const char *e = strchr(l, '\n');
        snprintf(line, sizeof line, "%.*s", (int)(e ? e - l : (long)strlen(l)), l);
        int k = 0;
        for (char *p = line, *t; k < 4 && (t = strsep(&p, "\t")); k++) f[k] = t;
        if (k >= 2 && !strcmp(f[2] ? f[2] : "", parent)) n++;
    }
    cb_array(b, n);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char line[512], *f[4] = { 0 };
        const char *e = strchr(l, '\n');
        snprintf(line, sizeof line, "%.*s", (int)(e ? e - l : (long)strlen(l)), l);
        int k = 0;
        for (char *p = line, *t; k < 4 && (t = strsep(&p, "\t")); k++) f[k] = t;
        if (k < 2 || strcmp(f[2] ? f[2] : "", parent)) continue;
        const char *flags = f[3] ? f[3] : "";
        cb_map(b, 5);
        cb_text(b, "id"); cb_int(b, atoll(f[0]));
        cb_text(b, "label"); cb_text(b, f[1]);
        cb_text(b, "enabled"); cb_bool(b, !strstr(flags, "disabled"));
        cb_text(b, "checked"); cb_bool(b, strstr(flags, "checked") != NULL);
        cb_text(b, "items"); put_menu(b, text, f[0]);
    }
}

struct field { char key[64], type[16], label[128], value[1024], setter[64], choices[256]; };

static size_t parse_settings(const char *text, char pages[][64], int *page_of, struct field *fields, size_t max, size_t *npages)
{
    size_t nf = 0;
    *npages = 0;
    for (const char *l = text; l && *l && nf < max; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char line[2048], *f[7] = { 0 };
        const char *e = strchr(l, '\n');
        snprintf(line, sizeof line, "%.*s", (int)(e ? e - l : (long)strlen(l)), l);
        int k = 0;
        for (char *p = line, *t; k < 7 && (t = strsep(&p, "\t")); k++) f[k] = t;
        if (k >= 2 && !strcmp(f[0], "page") && *npages < 16) snprintf(pages[(*npages)++], 64, "%s", f[1]);
        else if (k >= 6 && !strcmp(f[0], "field") && *npages) {
            struct field *x = &fields[nf];
            page_of[nf++] = (int)*npages - 1;
            snprintf(x->key, sizeof x->key, "%s", f[1]);
            snprintf(x->type, sizeof x->type, "%s", f[2]);
            snprintf(x->label, sizeof x->label, "%s", f[3]);
            snprintf(x->value, sizeof x->value, "%s", f[4]);
            snprintf(x->setter, sizeof x->setter, "%s", f[5]);
            snprintf(x->choices, sizeof x->choices, "%s", f[6] ? f[6] : "");
        }
    }
    return nf;
}

static void put_settings(cbuf *b, const char *text)
{
    char pages[16][64];
    int page_of[64];
    struct field fields[64];
    size_t np, nf = parse_settings(text, pages, page_of, fields, 64, &np);
    cb_array(b, np);
    for (size_t p = 0; p < np; p++) {
        size_t cnt = 0;
        for (size_t i = 0; i < nf; i++) cnt += page_of[i] == (int)p;
        cb_map(b, 2);
        cb_text(b, "title"); cb_text(b, pages[p]);
        cb_text(b, "fields");
        cb_array(b, cnt);
        for (size_t i = 0; i < nf; i++) {
            if (page_of[i] != (int)p) continue;
            struct field *x = &fields[i];
            cb_map(b, x->choices[0] ? 5 : 4);
            cb_text(b, "key"); cb_text(b, x->key);
            cb_text(b, "type"); cb_text(b, x->type);
            cb_text(b, "label"); cb_text(b, x->label);
            cb_text(b, "value");
            if (!strcmp(x->type, "number") || !strcmp(x->type, "choice")) cb_int(b, atoll(x->value));
            else if (!strcmp(x->type, "checkbox")) cb_bool(b, !strcmp(x->value, "1") || !strcmp(x->value, "true"));
            else cb_text(b, x->value);
            if (x->choices[0]) {
                size_t nc = 1;
                for (const char *c = x->choices; *c; c++) nc += *c == ',';
                cb_text(b, "choices");
                cb_array(b, nc);
                char tmp[256];
                snprintf(tmp, sizeof tmp, "%s", x->choices);
                for (char *q = tmp, *t; (t = strsep(&q, ","));) cb_text(b, t);
            }
        }
    }
}

static char *call_text(wpo_object *o, const char *method, const char *arg)
{
    wpo_value a = wpo_v_string(arg), r;
    char *out = NULL;
    if (wpo_send(o, method, &a, 1, &r) == WPO_OK && r.type == 's') { out = r.u.s; r.u.s = NULL; }
    wpo_value_clear(&a);
    wpo_value_clear(&r);
    return out ? out : strdup("");
}

/* settings_set: apply {key: value} through each field's setter. */
static const char *apply_settings(wpo_object *o, const cv *values)
{
    char *text = call_text(o, "wpAddSettingsPages", "");
    char pages[16][64];
    int page_of[64];
    struct field fields[64];
    size_t np, nf = parse_settings(text, pages, page_of, fields, 64, &np);
    free(text);
    const char *err = NULL;
    for (size_t k = 0; values && values->k == CV_MAP && k < values->n && !err; k++) {
        const cv *key = &values->items[2 * k], *val = &values->items[2 * k + 1];
        struct field *f = NULL;
        for (size_t i = 0; i < nf && !f; i++) if (key->k == CV_TEXT && !strcmp(fields[i].key, key->s)) f = &fields[i];
        if (!f) { err = "no such setting"; break; }
        wpo_value a, r;
        if (val->k == CV_TEXT) a = wpo_v_string(val->s);
        else if (val->k == CV_INT) a = wpo_v_int(val->i);
        else if (val->k == CV_BOOL) a = wpo_v_bool(val->b);
        else { err = "unsupported value"; break; }
        if (wpo_send(o, f->setter, &a, 1, &r) != WPO_OK) err = "the setter refused the value";
        else if (r.type == 'b' && !r.u.b) err = "the setter refused the value";
        wpo_value_clear(&a);
        wpo_value_clear(&r);
    }
    return err;
}

/* ---- selections and jobs */

static void reply_value_cbor(struct client *c, int64_t id, cbuf *val)
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

static void reply_int(struct client *c, int64_t id, int64_t v)
{
    cbuf val;
    cb_init(&val);
    cb_int(&val, v);
    reply_value_cbor(c, id, &val);
    cb_free(&val);
}

static bool handle_jobs(struct client *c, int64_t id, const char *op, cv *msg)
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
        if (jid < 0) reply_error(c, id, "bad request", err[0] ? err : "move/copy needs a selection and a dest");
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

/* ---- list: a page of a folder's entries */

struct list_req { struct client *c; int64_t id; };

static void list_reply(void *arg, const wps_page *pg)
{
    struct list_req *r = arg;
    if (!r->c->dead) {
        if (pg->err) {
            reply_error(r->c, r->id, "failed", strerror(pg->err));
        } else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_int(&b, r->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result");
            cb_map(&b, 4);
            cb_text(&b, "total"); cb_uint(&b, pg->total);
            cb_text(&b, "complete"); cb_bool(&b, pg->complete);
            cb_text(&b, "offset"); cb_uint(&b, pg->offset);
            cb_text(&b, "items");
            cb_array(&b, pg->n);
            for (size_t i = 0; i < pg->n; i++) {
                const wps_entry *e = &pg->items[i];
                const char *t = e->type == DT_DIR ? "folder" : e->type == DT_LNK ? "link" : e->type == DT_REG ? "file" : "other";
                cb_map(&b, e->detailed ? (e->shadow ? 8 : 6) : 2);
                cb_text(&b, "name"); cb_text(&b, e->name);
                cb_text(&b, "type"); cb_text(&b, t);
                if (e->detailed) {
                    cb_text(&b, "size"); cb_int(&b, e->size);
                    cb_text(&b, "mtime"); cb_int(&b, e->mtime);
                    cb_text(&b, "id"); cb_text(&b, e->id);
                    cb_text(&b, "title"); cb_text(&b, e->title);
                    if (e->shadow) {
                        cb_text(&b, "shadow"); cb_bool(&b, true);
                        cb_text(&b, "target"); cb_text(&b, e->target_type == DT_DIR ? "folder" : "file");
                    }
                }
            }
            send_frame(r->c, &b);
            cb_free(&b);
        }
    }
    client_unref(r->c);
    free(r);
}

/* ---- open: start a program (as this user), or open a file */

/* Split a .desktop Exec line into arguments: double quotes and backslash
 * escapes, field codes (%f %u ...) dropped, %% kept as %. */
static int split_exec(const char *exec, char *buf, size_t n, char **argv, int max)
{
    int argc = 0;
    size_t o = 0;
    const char *p = exec;
    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc] = buf + o;
        bool quoted = false, any = false;
        for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++) {
            if (*p == '"') { quoted = !quoted; any = true; continue; }
            if (*p == '\\' && p[1]) { p++; }
            else if (*p == '%' && p[1]) {
                p++;
                if (*p != '%') continue;   /* a field code: no files are passed */
            }
            if (o + 1 < n) buf[o++] = *p;
            any = true;
        }
        if (o + 1 < n) buf[o++] = 0;
        if (any && argv[argc][0]) argc++;
        else if (any && quoted == false && argv[argc][0] == 0) argc++;
    }
    argv[argc] = NULL;
    return argc;
}

static pid_t spawn(char **argv)
{
    posix_spawnattr_t at;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    extern char **environ;
    pid_t pid = -1;
    if (posix_spawnp(&pid, argv[0], &fa, &at, argv, environ) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    return pid;
}

static void do_open(struct req *r, wpo_object *o)
{
    const cv *cf = cv_get(&r->msg, "confirm");
    bool confirmed = cf && cf->k == CV_BOOL && cf->b;
    char buf[4096], *argv[64];
    const char *what;
    if (wpo_is_a(o, WPProgramClass)) {
        const char *cmd = WPProgram_data(o)->command;
        if (!cmd || !*cmd) { reply_error(r->c, r->id, "failed", "the program has no command"); return; }
        /* A program another user (not root) owns asks first: nobody can
         * plant something that runs when another user opens it. */
        uid_t owner = r->tgt.st.st_uid;
        if (owner != geteuid() && owner != 0 && !confirmed) {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_int(&b, r->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result");
            cb_map(&b, 3);
            cb_text(&b, "needs_confirmation"); cb_bool(&b, true);
            cb_text(&b, "command"); cb_text(&b, cmd);
            cb_text(&b, "owner_uid"); cb_int(&b, (int64_t)owner);
            send_frame(r->c, &b);
            cb_free(&b);
            return;
        }
        if (split_exec(cmd, buf, sizeof buf, argv, 64) == 0) { reply_error(r->c, r->id, "failed", "empty command"); return; }
        what = cmd;
    } else if (wpo_is_a(o, WPDataFileClass)) {
        argv[0] = "xdg-open";
        argv[1] = r->tgt.path;
        argv[2] = NULL;
        what = "xdg-open";
    } else {
        reply_error(r->c, r->id, "bad request", "folders and other objects open in the GUI");
        return;
    }
    pid_t pid = spawn(argv);   /* as this user, in this user's session */
    if (pid < 0) { reply_error(r->c, r->id, "failed", strerror(errno)); return; }
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, 2);
    cb_text(&b, "launched"); cb_int(&b, pid);
    cb_text(&b, "command"); cb_text(&b, what);
    send_frame(r->c, &b);
    cb_free(&b);
}

/* ---- from_template: a new object copied from a template */

struct tmpl_job {
    struct client *c;
    int64_t id;
    char src[PATH_MAX], folder[PATH_MAX], title[256], path[PATH_MAX + 300];
    int err;
    wps_loaded ld;
};

static void tmpl_work(void *arg)
{
    struct tmpl_job *j = arg;
    const char *base = strrchr(j->src, '/') ? strrchr(j->src, '/') + 1 : j->src;
    const char *ext = strchr(base, '.') ? strchr(base, '.') : "";
    int in = open(j->src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), out = -1;
    if (in < 0) { j->err = errno; return; }
    for (int k = 1; k < 100 && out < 0; k++) {
        if (k == 1) snprintf(j->path, sizeof j->path, "%s/%.200s%s", j->folder, j->title, ext);
        else snprintf(j->path, sizeof j->path, "%s/%.200s (%d)%s", j->folder, j->title, k, ext);
        out = open(j->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);   /* never overwrite */
        if (out < 0 && errno != EEXIST) break;
    }
    if (out < 0) { j->err = errno; close(in); return; }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { j->err = EIO; break; }
    /* Settings come along; identity and the template mark do not. */
    char names[8192];
    ssize_t nl = flistxattr(in, names, sizeof names);
    for (ssize_t o = 0; nl > 0 && o < nl; o += (ssize_t)strlen(names + o) + 1) {
        const char *nm = names + o;
        if (strncmp(nm, "user.wps.", 9) || !strcmp(nm, WPS_ATTR_ID)) continue;
        unsigned char v[65536];
        ssize_t vl = fgetxattr(in, nm, v, sizeof v);
        if (vl < 0) continue;
        if (!strcmp(nm, "user.wps.WPObject")) {
            wps_props ps;
            if (wps_props_decode(v, (size_t)vl, &ps) == 0) {
                wps_props_remove(&ps, "template");
                ps.counter = 0;
                size_t el;
                unsigned char *enc = wps_props_encode(&ps, &el);
                if (enc) fsetxattr(out, nm, enc, el, 0);
                free(enc);
                wps_props_free(&ps);
            }
        } else {
            fsetxattr(out, nm, v, (size_t)vl, 0);
        }
    }
    close(in);
    close(out);
    if (!j->err) wps_load(j->path, false, &j->ld);
}

static void tmpl_done(void *arg)
{
    struct tmpl_job *j = arg;
    if (!j->c->dead) {
        wpo_object *o = j->err || j->ld.err ? NULL : wps_object_from_loaded(&j->ld);
        if (!o) reply_error(j->c, j->id, "failed", strerror(j->err ? j->err : j->ld.err));
        else {
            WPObject_wpSetTitle(o, j->title);
            struct req fake = { .c = j->c, .id = j->id };
            reply_ok_object(&fake, o, NULL);
        }
    }
    wps_loaded_free(&j->ld);
    client_unref(j->c);
    free(j);
}


/* ---- icon positions: layout, place, arrange (G2: free placement) */

/* {op:"layout", target:FOLDER} -> {positions:{ID:"x,y"}, items:[{name, id, x, y}]}
 * Positions are kept by object ID; the GUI knows items by name, so a
 * worker reads the folder and matches each child's ID (one attribute read
 * per child, and only when the folder has positions at all). */
struct layout_job {
    struct client *c;
    int64_t id;
    char folder[PATH_MAX];
    size_t n;
    char (*ids)[WPS_ID_LEN + 1];
    char **vals;
    char **names;        /* found by the worker, per position */
};

static void layout_work(void *arg)
{
    struct layout_job *j = arg;
    DIR *d = opendir(j->folder);
    struct dirent *de;
    size_t found = 0;
    while (d && found < j->n && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[PATH_MAX + NAME_MAX + 2], id[WPS_ID_LEN + 1];
        snprintf(p, sizeof p, "%s/%s", j->folder, de->d_name);
        if (!wps_attr_get_id(p, id)) continue;
        for (size_t i = 0; i < j->n; i++)
            if (!j->names[i] && !strcmp(j->ids[i], id)) { j->names[i] = strdup(de->d_name); found++; break; }
    }
    if (d) closedir(d);
}

static void layout_done(void *arg)
{
    struct layout_job *j = arg;
    if (!j->c->dead) {
        size_t named = 0;
        for (size_t i = 0; i < j->n; i++) named += j->names[i] != NULL;
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, j->id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result");
        cb_map(&b, 2);
        cb_text(&b, "positions");
        cb_map(&b, j->n);
        for (size_t i = 0; i < j->n; i++) { cb_text(&b, j->ids[i]); cb_text(&b, j->vals[i]); }
        cb_text(&b, "items");
        cb_array(&b, named);
        for (size_t i = 0; i < j->n; i++) {
            if (!j->names[i]) continue;
            long long x = 0, y = 0;
            sscanf(j->vals[i], "%lld,%lld", &x, &y);
            cb_map(&b, 4);
            cb_text(&b, "name"); cb_text(&b, j->names[i]);
            cb_text(&b, "id"); cb_text(&b, j->ids[i]);
            cb_text(&b, "x"); cb_int(&b, x);
            cb_text(&b, "y"); cb_int(&b, y);
        }
        send_frame(j->c, &b);
        cb_free(&b);
    }
    for (size_t i = 0; i < j->n; i++) { free(j->vals[i]); free(j->names[i]); }
    free(j->ids);
    free(j->vals);
    free(j->names);
    client_unref(j->c);
    free(j);
}

static void do_layout(struct req *r, wpo_object *o)
{
    if (!wpo_is_a(o, WPFolderClass)) { reply_error(r->c, r->id, "bad request", "not a folder"); return; }
    wps_state *st = WPObject_data(o)->state;
    wps_cstate *cs = st ? wps_state_class(&st, "WPFolder", false) : NULL;
    struct layout_job *j = calloc(1, sizeof *j);
    j->c = r->c; r->c->refs++;
    j->id = r->id;
    snprintf(j->folder, sizeof j->folder, "%s", r->tgt.path);
    size_t n = 0;
    for (size_t i = 0; cs && i < cs->props.n; i++) n += !strncmp(cs->props.p[i].key, "pos:", 4);
    j->ids = calloc(n ? n : 1, sizeof *j->ids);
    j->vals = calloc(n ? n : 1, sizeof *j->vals);
    j->names = calloc(n ? n : 1, sizeof *j->names);
    for (size_t i = 0; cs && i < cs->props.n; i++) {
        const wps_prop *p = &cs->props.p[i];
        if (strncmp(p->key, "pos:", 4)) continue;
        snprintf(j->ids[j->n], sizeof j->ids[0], "%s", p->key + 4);
        j->vals[j->n++] = strndup((const char *)p->val, p->len);
    }
    if (!j->n) { layout_done(j); return; }   /* nothing placed: no need to read the folder */
    wpo_defer(loop, "layout", layout_work, layout_done, j);
}

/* {op:"arrange", target:FOLDER}: forget the stored positions; the GUI
 * arranges the icons (and may place them again). */
static void do_arrange(struct req *r, wpo_object *o)
{
    if (!wpo_is_a(o, WPFolderClass)) { reply_error(r->c, r->id, "bad request", "not a folder"); return; }
    wps_state *st = WPObject_data(o)->state;
    wps_cstate *cs = st ? wps_state_class(&st, "WPFolder", false) : NULL;
    char **keys = NULL;
    size_t n = 0;
    for (size_t i = 0; cs && i < cs->props.n; i++)
        if (!strncmp(cs->props.p[i].key, "pos:", 4)) {
            keys = realloc(keys, (n + 1) * sizeof *keys);
            keys[n++] = strdup(cs->props.p[i].key);
        }
    for (size_t i = 0; i < n; i++) { wps_state_remove(o, "WPFolder", keys[i]); free(keys[i]); }
    free(keys);
    if (n) { WPObject_wpSaveDeferred(o); WPObject_wpObjectChanged(o); }
    wpo_value v = wpo_v_int((int64_t)n);
    reply_ok_object(r, o, &v);
}

/* {op:"place", folder, items:[{target, x, y}]}: set icon positions. Items
 * get object IDs if they have none (positions are kept by ID). Replies with
 * the IDs, in order. */
struct place_job {
    struct client *c;
    int64_t id;
    char folder[PATH_MAX];
    size_t n;
    char **targets;
    int64_t *x, *y;
    wps_loaded fld, *items;
};

static void place_work(void *arg)
{
    struct place_job *j = arg;
    wps_load(j->folder, false, &j->fld);
    for (size_t i = 0; i < j->n; i++) wps_load(j->targets[i], true, &j->items[i]);
}

static void place_done(void *arg)
{
    struct place_job *j = arg;
    wpo_object *f = j->fld.err ? NULL : wps_object_from_loaded(&j->fld);
    if (!j->c->dead) {
        if (!f || !wpo_is_a(f, WPFolderClass)) reply_error(j->c, j->id, "failed", "the folder cannot be loaded");
        else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_int(&b, j->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result");
            cb_array(&b, j->n);
            for (size_t i = 0; i < j->n; i++) {
                wpo_object *o = j->items[i].err ? NULL : wps_object_from_loaded(&j->items[i]);
                bool ok = o && WPFolder_wpSetIconPos(f, o, j->x[i], j->y[i]);
                if (ok) cb_text(&b, WPObject_data(o)->id);
                else cb_null(&b);
            }
            send_frame(j->c, &b);
            cb_free(&b);
        }
    }
    wps_loaded_free(&j->fld);
    for (size_t i = 0; i < j->n; i++) { wps_loaded_free(&j->items[i]); free(j->targets[i]); }
    free(j->items);
    free(j->targets);
    free(j->x);
    free(j->y);
    client_unref(j->c);
    free(j);
}

static void start_place(struct client *c, int64_t id, const cv *msg)
{
    const char *folder = cv_text(msg, "folder");
    const cv *items = cv_get(msg, "items");
    if (!folder || !items || items->k != CV_ARRAY || items->n > 100000) {
        reply_error(c, id, "bad request", "place needs a folder and up to 100000 items");
        return;
    }
    struct place_job *j = calloc(1, sizeof *j);
    j->c = c; c->refs++;
    j->id = id;
    snprintf(j->folder, sizeof j->folder, "%s", folder);
    j->targets = calloc(items->n ? items->n : 1, sizeof *j->targets);
    j->x = calloc(items->n ? items->n : 1, sizeof *j->x);
    j->y = calloc(items->n ? items->n : 1, sizeof *j->y);
    j->items = calloc(items->n ? items->n : 1, sizeof *j->items);
    for (size_t i = 0; i < items->n; i++) {
        const char *t = cv_text(&items->items[i], "target");
        if (!t) continue;
        j->targets[j->n] = strdup(t);
        cv_int(&items->items[i], "x", &j->x[j->n]);
        cv_int(&items->items[i], "y", &j->y[j->n]);
        j->n++;
    }
    wpo_defer(loop, "place icons", place_work, place_done, j);
}

/* ---- rename: the file or folder itself (decided 2026-11-18, as in OS/2) */

struct rename_job {
    struct client *c;
    int64_t id;
    char path[PATH_MAX], newpath[PATH_MAX + NAME_MAX + 2], name[NAME_MAX + 1];
    int err;
    bool as_title;          /* no permission to rename: keep it as a title */
    wps_loaded ld;
};

static void rename_work(void *arg)
{
    struct rename_job *j = arg;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", j->path);
    char *slash = strrchr(dir, '/');
    if (!slash) { j->err = EINVAL; return; }
    *slash = 0;
    snprintf(j->newpath, sizeof j->newpath, "%s/%s", dir[0] ? dir : "", j->name);
    if (strcmp(j->newpath, j->path) != 0 &&
        syscall(SYS_renameat2, AT_FDCWD, j->path, AT_FDCWD, j->newpath, RENAME_NOREPLACE) != 0) {
        if (errno == EACCES || errno == EPERM || errno == EROFS) j->as_title = true;
        else { j->err = errno; return; }
    }
    wps_load(j->as_title ? j->path : j->newpath, false, &j->ld);
}

static void rename_done(void *arg)
{
    struct rename_job *j = arg;
    if (!j->c->dead) {
        wpo_object *o = j->err || j->ld.err ? NULL : wps_object_from_loaded(&j->ld);
        if (!o && j->err == EEXIST) reply_error(j->c, j->id, "exists", "an object with that name is already there");
        else if (!o) reply_error(j->c, j->id, "failed", strerror(j->err ? j->err : j->ld.err));
        else {
            if (j->as_title) WPObject_wpSetTitle(o, j->name);
            else if (WPObject_data(o)->title) WPObject_wpSetTitle(o, NULL);   /* the title follows the name */
            struct req fake = { .c = j->c, .id = j->id };
            wpo_value v = wpo_v_bool(!j->as_title);   /* result: was the file renamed? */
            reply_ok_object(&fake, o, &v);
        }
    }
    wps_loaded_free(&j->ld);
    client_unref(j->c);
    free(j);
}

static void do_rename(struct req *r, wpo_object *o)
{
    const char *name = cv_text(&r->msg, "name");
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) > NAME_MAX) {
        reply_error(r->c, r->id, "bad request", "a name must be non-empty, without '/', and not . or ..");
        return;
    }
    if (!wpo_is_a(o, WPFileSystemClass)) {   /* programs and abstract objects: the title */
        WPObject_wpSetTitle(o, name);
        reply_ok_object(r, o, NULL);
        return;
    }
    struct rename_job *j = calloc(1, sizeof *j);
    j->c = r->c; r->c->refs++;
    j->id = r->id;
    snprintf(j->path, sizeof j->path, "%s", r->tgt.path);
    snprintf(j->name, sizeof j->name, "%s", name);
    wpo_defer(loop, "rename", rename_work, rename_done, j);
}

static void do_from_template(struct req *r, wpo_object *o)
{
    const char *folder = cv_text(&r->msg, "folder"), *title = cv_text(&r->msg, "title");
    if (!WPObject_data(o)->is_template) { reply_error(r->c, r->id, "bad request", "not a template"); return; }
    if (!folder) { reply_error(r->c, r->id, "bad request", "from_template needs a folder"); return; }
    if (!S_ISREG(r->tgt.st.st_mode)) { reply_error(r->c, r->id, "bad request", "only file templates in this version"); return; }
    char *t = WPObject_wpQueryTitle(o);
    struct tmpl_job *j = calloc(1, sizeof *j);
    j->c = r->c; r->c->refs++;
    j->id = r->id;
    snprintf(j->src, sizeof j->src, "%s", r->tgt.path);
    snprintf(j->folder, sizeof j->folder, "%s", folder);
    snprintf(j->title, sizeof j->title, "%s", title && *title ? title : t ? t : "New");
    free(t);
    wpo_defer(loop, "from template", tmpl_work, tmpl_done, j);
}

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
    } else if (handle_jobs(c, id, op, msg)) {
        /* selections and jobs */
    } else if (strcmp(op, "list") == 0) {
        const char *folder = cv_text(msg, "target");
        int64_t off = 0, cnt = 100;
        cv_int(msg, "offset", &off);
        cv_int(msg, "count", &cnt);
        const cv *det = cv_get(msg, "details");
        if (!folder || off < 0 || cnt < 0 || cnt > 100000) {
            reply_error(c, id, "bad request", "list needs a target folder and a sane range");
            cv_free(msg);
            return;
        }
        struct list_req *r = calloc(1, sizeof *r);
        r->c = c; c->refs++;
        r->id = id;
        wps_listing_request(folder, (size_t)off, (size_t)cnt, det && det->k == CV_BOOL && det->b, list_reply, r);
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
    } else if (strcmp(op, "place") == 0) {
        start_place(c, id, msg);
    } else if (strcmp(op, "subscribe") == 0) {
        struct sub *x = calloc(1, sizeof *x);
        x->id = next_sub++;
        x->c = c;
        strlist(cv_get(msg, "objects"), &x->targets, &x->ntargets);
        strlist(cv_get(msg, "folders"), &x->folders, &x->nfolders);
        const cv *j = cv_get(msg, "jobs");
        x->jobs = j && j->k == CV_BOOL && j->b;
        for (size_t i = 0; i < x->nfolders; i++) wps_tracker_watch(x->folders[i]);   /* in view now */
        x->next = subs;
        subs = x;
        reply_int(c, id, x->id);
    } else if (strcmp(op, "unsubscribe") == 0) {
        int64_t sid = -1;
        cv_int(msg, "sub", &sid);
        bool found = false;
        for (struct sub **pp = &subs; *pp; pp = &(*pp)->next)
            if ((*pp)->id == sid && (*pp)->c == c) {
                struct sub *x = *pp;
                *pp = x->next;
                for (size_t i = 0; i < x->ntargets; i++) free(x->targets[i]);
                for (size_t i = 0; i < x->nfolders; i++) free(x->folders[i]);
                free(x->targets); free(x->folders); free(x);
                found = true;
                break;
            }
        if (found) reply_int(c, id, sid);
        else reply_error(c, id, "not found", "no such subscription");
    } else if (strcmp(op, "resolve") == 0 || strcmp(op, "call") == 0 || strcmp(op, "menu") == 0 ||
               strcmp(op, "menu_select") == 0 || strcmp(op, "settings") == 0 || strcmp(op, "settings_set") == 0 ||
               strcmp(op, "open") == 0 || strcmp(op, "from_template") == 0 || strcmp(op, "rename") == 0 ||
               strcmp(op, "layout") == 0 || strcmp(op, "arrange") == 0) {
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
    wps_mime_init(getenv("WPS_MIME_GLOBS"));
    if (geteuid() == 0) {
        /* Supported, not recommended: root's service could see every
         * user's files. It never follows other users' homes on its own. */
        log_msg("warning: running as root; other users' home folders are not tracked");
        wps_set_root_session(true);
    }

    loop = wpo_loop_new(8);   /* workers wait on the disk; more of them keep it busy */
    wpo_loop_set_watchdog(loop, 50.0, on_watchdog, NULL);
    const char *delay = getenv("WPS_SAVE_DELAY_MS");
    wps_objects_init(&env, db, loop, delay ? atoi(delay) : 2000);

    const char *sp = getenv("WPS_SOCKET");
    if (sp && *sp) snprintf(sock_path, sizeof sock_path, "%s", sp);
    else snprintf(sock_path, sizeof sock_path, "%s/wps.sock", env.runtime_dir);
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    /* A socket address holds 107 bytes; a longer path must not be cut
     * short (the stale-socket cleanup below would miss the real name). */
    if (strlen(sock_path) >= sizeof sa.sun_path) {
        log_msg("%s: socket path longer than %zu bytes; set WPS_RUNTIME_DIR or WPS_SOCKET to a shorter one",
                sock_path, sizeof sa.sun_path - 1);
        return 1;
    }
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
    wps_listing_init(loop);
    wps_jobs_init(loop, db, env.identity);
    wps_plugins_init(loop, &env);
    wps_on_changed = on_object_changed;
    wps_on_fs_event = on_fs_event;
    wps_on_job = on_job;
    wps_tracker_init(loop, &env, db);
    wps_tracker_stats ts;
    wps_tracker_get_stats(&ts);
    log_msg("tracking changes with %s", ts.mode);
    log_msg("ready: socket %s, identity %s, host %s", sock_path, env.identity, env.host);
    wpo_loop_run(loop);

    wpo_loop_run_until_idle(loop, 10000);
    while (clients) client_close(clients);
    /* Running jobs pause and are journaled; they resume at the next start
     * (design: logout). */
    wps_jobs_shutdown(true);
    /* The jobs' last progress events (no subscribers remain to hear them). */
    wpo_loop_run_until_idle(loop, 1000);
    wps_plugins_shutdown();
    wps_tracker_shutdown();
    wps_listing_shutdown();
    unlink(sock_path);
    wps_objects_shutdown();
    wpo_loop_free(loop);
    wps_db_close(db);
    wpo_shutdown();
    wps_plugins_free_defs();
    log_msg("stopped");
    return quitting ? 0 : 1;
}
