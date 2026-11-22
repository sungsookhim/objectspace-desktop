#define _GNU_SOURCE
#include "svc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct out { struct out *next; cbuf b; };
struct pending { struct pending *next; int64_t id; svc_fn fn; void *arg; };
struct delivery { svc_fn fn; void *arg; cv *reply; };

static ui_app *app;
static int sock = -1;
static pthread_t reader, writer;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t out_cv = PTHREAD_COND_INITIALIZER;
static struct out *outq, *outq_tail;
static struct pending *pend;
static int64_t next_id = 1;
static bool stopping, broken;
static svc_fn event_fn;
static void *event_arg;

void svc_on_event(svc_fn fn, void *arg)
{
    event_fn = fn;
    event_arg = arg;
}

static void deliver(void *arg)   /* UI thread */
{
    struct delivery *d = arg;
    d->fn(d->arg, d->reply);
    if (d->reply) { cv_free(d->reply); free(d->reply); }
    free(d);
}

static void post(svc_fn fn, void *arg, cv *reply)
{
    struct delivery *d = malloc(sizeof *d);
    d->fn = fn;
    d->arg = arg;
    d->reply = reply;
    ui_app_post(app, deliver, d);
}

/* The connection is gone: every waiting request gets NULL. */
static void fail_all(void)
{
    pthread_mutex_lock(&mu);
    broken = true;
    struct pending *p = pend;
    pend = NULL;
    pthread_mutex_unlock(&mu);
    while (p) {
        struct pending *n = p->next;
        post(p->fn, p->arg, NULL);
        free(p);
        p = n;
    }
}

static bool io(int fd, void *buf, size_t n, bool wr)
{
    unsigned char *b = buf;
    while (n) {
        ssize_t r = wr ? send(fd, b, n, MSG_NOSIGNAL) : read(fd, b, n);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r;
        n -= (size_t)r;
    }
    return true;
}

static void *writer_main(void *x)
{
    (void)x;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!outq && !stopping) pthread_cond_wait(&out_cv, &mu);
        if (stopping) break;
        struct out *o = outq;
        outq = o->next;
        if (!outq) outq_tail = NULL;
        pthread_mutex_unlock(&mu);
        unsigned char h[4] = { (unsigned char)(o->b.n >> 24), (unsigned char)(o->b.n >> 16), (unsigned char)(o->b.n >> 8),
                               (unsigned char)o->b.n };
        bool ok = io(sock, h, 4, true) && io(sock, o->b.d, o->b.n, true);
        cb_free(&o->b);
        free(o);
        pthread_mutex_lock(&mu);
        if (!ok) break;
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void *reader_main(void *x)
{
    (void)x;
    for (;;) {
        unsigned char h[4];
        if (!io(sock, h, 4, false)) break;
        size_t n = (size_t)h[0] << 24 | (size_t)h[1] << 16 | (size_t)h[2] << 8 | h[3];
        unsigned char *buf = malloc(n ? n : 1);
        if (!io(sock, buf, n, false)) { free(buf); break; }
        cv *m = calloc(1, sizeof *m);
        int rc = cv_decode(buf, n, m);
        free(buf);
        int64_t id;
        if (!rc && cv_get(m, "event") && event_fn) { post(event_fn, event_arg, m); continue; }
        if (rc || !cv_int(m, "id", &id)) { if (!rc) cv_free(m); free(m); continue; }
        pthread_mutex_lock(&mu);
        struct pending *found = NULL;
        for (struct pending **pp = &pend; *pp; pp = &(*pp)->next)
            if ((*pp)->id == id) { found = *pp; *pp = found->next; break; }
        pthread_mutex_unlock(&mu);
        if (found) { post(found->fn, found->arg, m); free(found); }
        else { cv_free(m); free(m); }
    }
    fail_all();
    return NULL;
}

bool svc_start(ui_app *a, char *err, size_t errlen)
{
    app = a;
    char path[4096];
    const char *s = getenv("WPS_SOCKET"), *rt = getenv("WPS_RUNTIME_DIR"), *xr = getenv("XDG_RUNTIME_DIR");
    if (s && *s) snprintf(path, sizeof path, "%s", s);
    else if (rt && *rt) snprintf(path, sizeof path, "%s/wps.sock", rt);
    else if (xr && *xr) snprintf(path, sizeof path, "%s/wps.sock", xr);
    else snprintf(path, sizeof path, "/tmp/wps-%u/wps.sock", (unsigned)getuid());
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof sa.sun_path) { snprintf(err, errlen, "socket path too long: %s", path); return false; }
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0 || connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
        snprintf(err, errlen, "cannot reach wpsd at %s: %s", path, strerror(errno));
        if (sock >= 0) close(sock);
        sock = -1;
        return false;
    }
    pthread_create(&writer, NULL, writer_main, NULL);
    pthread_create(&reader, NULL, reader_main, NULL);
    return true;
}

void svc_stop(void)
{
    if (sock < 0) return;
    pthread_mutex_lock(&mu);
    stopping = true;
    pthread_cond_signal(&out_cv);
    pthread_mutex_unlock(&mu);
    shutdown(sock, SHUT_RDWR);
    pthread_join(writer, NULL);
    pthread_join(reader, NULL);
    close(sock);
    sock = -1;
    while (outq) { struct out *o = outq; outq = o->next; cb_free(&o->b); free(o); }
}

/* Start a request map with `nfields` fields after id and op. */
static struct out *begin(const char *op, size_t nfields, int64_t *id)
{
    struct out *o = calloc(1, sizeof *o);
    cb_init(&o->b);
    pthread_mutex_lock(&mu);
    *id = next_id++;
    pthread_mutex_unlock(&mu);
    cb_map(&o->b, 2 + nfields);
    cb_text(&o->b, "id"); cb_int(&o->b, *id);
    cb_text(&o->b, "op"); cb_text(&o->b, op);
    return o;
}

static void send_req(struct out *o, int64_t id, svc_fn fn, void *arg)
{
    pthread_mutex_lock(&mu);
    if (broken || sock < 0 || o->b.oom) {
        pthread_mutex_unlock(&mu);
        cb_free(&o->b);
        free(o);
        post(fn, arg, NULL);
        return;
    }
    struct pending *p = malloc(sizeof *p);
    p->id = id;
    p->fn = fn;
    p->arg = arg;
    p->next = pend;
    pend = p;
    if (outq_tail) outq_tail->next = o;
    else outq = o;
    outq_tail = o;
    pthread_cond_signal(&out_cv);
    pthread_mutex_unlock(&mu);
}

void svc_list(const char *folder, size_t offset, size_t count, bool details, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("list", 4, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, folder);
    cb_text(&o->b, "offset"); cb_uint(&o->b, offset);
    cb_text(&o->b, "count"); cb_uint(&o->b, count);
    cb_text(&o->b, "details"); cb_bool(&o->b, details);
    send_req(o, id, fn, arg);
}

void svc_list_v(const char *folder, const char *filter, bool hidden, size_t offset, size_t count, bool details, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("list", 6, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, folder);
    cb_text(&o->b, "offset"); cb_uint(&o->b, offset);
    cb_text(&o->b, "count"); cb_uint(&o->b, count);
    cb_text(&o->b, "details"); cb_bool(&o->b, details);
    cb_text(&o->b, "filter"); cb_text(&o->b, filter ? filter : "");
    cb_text(&o->b, "hidden"); cb_bool(&o->b, hidden);
    send_req(o, id, fn, arg);
}

void svc_resolve(const char *target, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("resolve", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    send_req(o, id, fn, arg);
}

void svc_menu(const char *target, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("menu", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    send_req(o, id, fn, arg);
}

void svc_menu_select(const char *target, int64_t item, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("menu_select", 2, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    cb_text(&o->b, "item"); cb_int(&o->b, item);
    send_req(o, id, fn, arg);
}

void svc_open_files(const char *target, bool confirm, const char *const *files, size_t nfiles, svc_fn fn, void *arg)
{
    int64_t id;
    /* What opens appears on this GUI's display (wpsd may have been
     * started elsewhere, e.g. before the session's compositor). */
    char disp[4096] = "";
    const char *wd = getenv("WAYLAND_DISPLAY"), *rt = getenv("XDG_RUNTIME_DIR");
    if (wd && *wd) snprintf(disp, sizeof disp, "%s%s%s", wd[0] == '/' || !rt ? "" : rt, wd[0] == '/' || !rt ? "" : "/", wd);
    const char *x11 = getenv("DISPLAY");
    bool x = x11 && *x11;
    /* and this session's input-method and desktop settings (wpsd may have
     * started before the session gave them: Hangul was not typed in
     * programs opened from WPS) */
    static const char *const keys[] = { "GTK_IM_MODULE", "QT_IM_MODULE", "XMODIFIERS", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE" };
    char envs[5][300];
    size_t ne = 0;
    for (size_t i = 0; i < 5; i++) {
        const char *v = getenv(keys[i]);
        if (v && *v) snprintf(envs[ne++], sizeof envs[0], "%s=%s", keys[i], v);
    }
    struct out *o = begin("open", 2 + (disp[0] ? 1 : 0) + (x ? 1 : 0) + (ne ? 1 : 0) + (nfiles ? 1 : 0), &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    cb_text(&o->b, "confirm"); cb_bool(&o->b, confirm);
    if (disp[0]) { cb_text(&o->b, "display"); cb_text(&o->b, disp); }
    if (x) { cb_text(&o->b, "x11"); cb_text(&o->b, x11); }
    if (ne) {
        cb_text(&o->b, "env");
        cb_array(&o->b, ne);
        for (size_t i = 0; i < ne; i++) cb_text(&o->b, envs[i]);
    }
    if (nfiles) {   /* objects dropped on a program: it starts with them */
        cb_text(&o->b, "files");
        cb_array(&o->b, nfiles);
        for (size_t i = 0; i < nfiles; i++) cb_text(&o->b, files[i]);
    }
    send_req(o, id, fn, arg);
}

void svc_open(const char *target, bool confirm, svc_fn fn, void *arg) { svc_open_files(target, confirm, NULL, 0, fn, arg); }

void svc_subscribe_folder(const char *folder, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("subscribe", 1, &id);
    cb_text(&o->b, "folders");
    cb_array(&o->b, 1);
    cb_text(&o->b, folder);
    send_req(o, id, fn, arg);
}

void svc_subscribe_objects(const char *const *targets, size_t n, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("subscribe", 1, &id);
    cb_text(&o->b, "objects");
    cb_array(&o->b, n);
    for (size_t i = 0; i < n; i++) cb_text(&o->b, targets[i]);
    send_req(o, id, fn, arg);
}

void svc_first_start(const char *desktop, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("first_start", 1, &id);
    cb_text(&o->b, "desktop"); cb_text(&o->b, desktop);
    send_req(o, id, fn, arg);
}

void svc_drive(const char *target, const char *action, const char *pass, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("drive", pass ? 3 : 2, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    cb_text(&o->b, "action"); cb_text(&o->b, action);
    if (pass) { cb_text(&o->b, "pass"); cb_text(&o->b, pass); }
    send_req(o, id, fn, arg);
}

void svc_restore(const char *target, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("restore", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    send_req(o, id, fn, arg);
}

void svc_empty_trash(svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("empty_trash", 0, &id);
    send_req(o, id, fn, arg);
}

void svc_setup(svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("setup", 0, &id);
    send_req(o, id, fn, arg);
}

static void ignore(void *arg, const cv *reply) { (void)arg; (void)reply; }

void svc_unsubscribe(int64_t sub)
{
    int64_t id;
    struct out *o = begin("unsubscribe", 1, &id);
    cb_text(&o->b, "sub"); cb_int(&o->b, sub);
    send_req(o, id, ignore, NULL);
}

void svc_rename(const char *target, const char *name, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("rename", 2, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    cb_text(&o->b, "name"); cb_text(&o->b, name);
    send_req(o, id, fn, arg);
}

void svc_settings(const char *target, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("settings", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    send_req(o, id, fn, arg);
}

void svc_settings_set(const char *target, const char *key, const cv *value, int64_t expect, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("settings_set", expect >= 0 ? 3 : 2, &id);   /* expect < 0: no check */
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    cb_text(&o->b, "values");
    cb_map(&o->b, 1);
    cb_text(&o->b, key);
    if (value->k == CV_TEXT) cb_text(&o->b, value->s);
    else if (value->k == CV_BOOL) cb_bool(&o->b, value->b);
    else cb_int(&o->b, value->i);
    if (expect >= 0) { cb_text(&o->b, "expect"); cb_int(&o->b, expect); }
    send_req(o, id, fn, arg);
}

void svc_from_template(const char *target, const char *folder, const char *title, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("from_template", title ? 3 : 2, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    cb_text(&o->b, "folder"); cb_text(&o->b, folder);
    if (title) { cb_text(&o->b, "title"); cb_text(&o->b, title); }
    send_req(o, id, fn, arg);
}

void svc_create(const char *folder, const char *cls, const char *title, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("create", 3, &id);
    cb_text(&o->b, "folder"); cb_text(&o->b, folder);
    cb_text(&o->b, "class"); cb_text(&o->b, cls);
    cb_text(&o->b, "title"); cb_text(&o->b, title);
    send_req(o, id, fn, arg);
}

void svc_shadow(const char *target, const char *folder, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("shadow", 2, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    cb_text(&o->b, "folder"); cb_text(&o->b, folder);
    send_req(o, id, fn, arg);
}

void svc_select(const char *folder, const char *const *names, size_t n, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("select", 3, &id);
    cb_text(&o->b, "folder"); cb_text(&o->b, folder);
    cb_text(&o->b, "all"); cb_bool(&o->b, false);
    cb_text(&o->b, "items");
    cb_array(&o->b, n);
    for (size_t i = 0; i < n; i++) cb_text(&o->b, names[i]);
    send_req(o, id, fn, arg);
}

void svc_job_start(const char *kind, int64_t selection, const char *dest, const char *const *names, const int64_t *x,
                   const int64_t *y, size_t n, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin(kind, n ? 3 : 2, &id);
    cb_text(&o->b, "selection"); cb_int(&o->b, selection);
    cb_text(&o->b, "dest"); cb_text(&o->b, dest);
    if (n) {
        cb_text(&o->b, "positions");
        cb_array(&o->b, n);
        for (size_t i = 0; i < n; i++) {
            cb_map(&o->b, 3);
            cb_text(&o->b, "name"); cb_text(&o->b, names[i]);
            cb_text(&o->b, "x"); cb_int(&o->b, x[i]);
            cb_text(&o->b, "y"); cb_int(&o->b, y[i]);
        }
    }
    send_req(o, id, fn, arg);
}

void svc_vtrash(const char *target, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("vtrash", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    send_req(o, id, fn, arg);
}

void svc_vdelete(const char *target, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("vdelete", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    send_req(o, id, fn, arg);
}

void svc_vmove(const char *target, const char *folder, bool copy, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("vmove", 3, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    cb_text(&o->b, "folder"); cb_text(&o->b, folder);
    cb_text(&o->b, "copy"); cb_bool(&o->b, copy);
    send_req(o, id, fn, arg);
}

void svc_job_decide(int64_t job, const char *decision, bool all)
{
    int64_t id;
    struct out *o = begin("job_decide", 3, &id);
    cb_text(&o->b, "job"); cb_int(&o->b, job);
    cb_text(&o->b, "decision"); cb_text(&o->b, decision);
    cb_text(&o->b, "all"); cb_bool(&o->b, all);
    send_req(o, id, ignore, NULL);
}

void svc_job_cancel(int64_t job)
{
    int64_t id;
    struct out *o = begin("job_cancel", 1, &id);
    cb_text(&o->b, "job"); cb_int(&o->b, job);
    send_req(o, id, ignore, NULL);
}

void svc_find(const char *folder, const char *name, bool subfolders, const char *kind, int64_t limit, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("find", 5, &id);
    cb_text(&o->b, "folder"); cb_text(&o->b, folder);
    cb_text(&o->b, "name"); cb_text(&o->b, name);
    cb_text(&o->b, "subfolders"); cb_bool(&o->b, subfolders);
    cb_text(&o->b, "kind"); cb_text(&o->b, kind);
    cb_text(&o->b, "limit"); cb_int(&o->b, limit);
    send_req(o, id, fn, arg);
}

void svc_find_cancel(int64_t find)
{
    int64_t id;
    struct out *o = begin("find_cancel", 1, &id);
    cb_text(&o->b, "find"); cb_int(&o->b, find);
    send_req(o, id, ignore, NULL);
}

void svc_subscribe_jobs(svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("subscribe", 1, &id);
    cb_text(&o->b, "jobs"); cb_bool(&o->b, true);
    send_req(o, id, fn, arg);
}

void svc_layout(const char *folder, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("layout", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, folder);
    send_req(o, id, fn, arg);
}

void svc_arrange(const char *folder, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("arrange", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, folder);
    send_req(o, id, fn, arg);
}

void svc_place(const char *folder, const char *const *targets, const int64_t *x, const int64_t *y, size_t n, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("place", 2, &id);
    cb_text(&o->b, "folder"); cb_text(&o->b, folder);
    cb_text(&o->b, "items");
    cb_array(&o->b, n);
    for (size_t i = 0; i < n; i++) {
        cb_map(&o->b, 3);
        cb_text(&o->b, "target"); cb_text(&o->b, targets[i]);
        cb_text(&o->b, "x"); cb_int(&o->b, x[i]);
        cb_text(&o->b, "y"); cb_int(&o->b, y[i]);
    }
    send_req(o, id, fn, arg);
}

/* ---- messages (wpsd keeps them: messages.h) */

void svc_msg_add(const svc_message *m, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("msg_add", 8, &id);
    cb_text(&o->b, "app"); cb_text(&o->b, m->app ? m->app : "");
    cb_text(&o->b, "icon"); cb_text(&o->b, m->icon ? m->icon : "");
    cb_text(&o->b, "summary"); cb_text(&o->b, m->summary ? m->summary : "");
    cb_text(&o->b, "body"); cb_text(&o->b, m->body ? m->body : "");
    cb_text(&o->b, "urgency"); cb_int(&o->b, m->urgency);
    cb_text(&o->b, "nid"); cb_int(&o->b, m->nid);
    cb_text(&o->b, "sender"); cb_text(&o->b, m->sender ? m->sender : "");
    cb_text(&o->b, "actions"); cb_array(&o->b, m->nactions);
    for (size_t i = 0; i < m->nactions; i++) cb_text(&o->b, m->actions[i]);
    send_req(o, id, fn, arg);
}

void svc_msg_list(svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("msg_list", 0, &id);
    send_req(o, id, fn, arg);
}

/* ids (n of them), or every message when n is 0. */
static void ids_or_all(struct out *o, const int64_t *ids, size_t n)
{
    if (!n) { cb_text(&o->b, "all"); cb_bool(&o->b, true); return; }
    cb_text(&o->b, "ids");
    cb_array(&o->b, n);
    for (size_t i = 0; i < n; i++) cb_int(&o->b, ids[i]);
}

void svc_msg_set(const int64_t *ids, size_t n, int read, int closed, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("msg_set", 1 + (read >= 0) + (closed >= 0), &id);
    ids_or_all(o, ids, n);
    if (read >= 0) { cb_text(&o->b, "read"); cb_bool(&o->b, read); }
    if (closed >= 0) { cb_text(&o->b, "closed"); cb_bool(&o->b, closed); }
    send_req(o, id, fn, arg);
}

void svc_msg_delete(const int64_t *ids, size_t n, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("msg_delete", 1, &id);
    ids_or_all(o, ids, n);
    send_req(o, id, fn, arg);
}

void svc_msg_closed(int64_t nid, const char *sender, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("msg_closed", 2, &id);
    cb_text(&o->b, "nid"); cb_int(&o->b, nid);
    cb_text(&o->b, "sender"); cb_text(&o->b, sender ? sender : "");
    send_req(o, id, fn, arg);
}

void svc_startup_add(const char *target, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("startup_add", 1, &id);
    cb_text(&o->b, "target"); cb_text(&o->b, target);
    send_req(o, id, fn, arg);
}

void svc_restore_defaults(const char *desktop, svc_fn fn, void *arg)
{
    int64_t id;
    struct out *o = begin("restore_defaults", desktop ? 1 : 0, &id);
    if (desktop) { cb_text(&o->b, "desktop"); cb_text(&o->b, desktop); }
    send_req(o, id, fn, arg);
}

bool (*svc_view_fn)(const char *view);

bool svc_reply_view(const cv *reply)
{
    const cv *res = reply ? cv_get(reply, "result") : NULL;
    const char *v = res ? cv_text(res, "view") : NULL;
    return v && svc_view_fn && svc_view_fn(v);
}
