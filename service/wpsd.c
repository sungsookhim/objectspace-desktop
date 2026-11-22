/*
 * wpsd - the per-user WPS object service (M2).
 *
 * Serves clients on $WPS_RUNTIME_DIR/wps.sock (or $WPS_SOCKET): each message
 * is a 4-byte big-endian length and a CBOR map. Requests:
 *   {op:"first_start", desktop}         the desktop's first-start steps not done yet (versioned); the
 *                                       version it was at
 *   {op:"restore_defaults", desktop?}   the default objects back where they belong (moved back,
 *                                       out of the trash, or made again) -> {restored:[titles]}
 *   {event:"drives_changed", sub}       drives came, went, mounted or unmounted
 *   {op:"drive", uuid|target, action, pass}   mount|unmount|eject|poweroff|unlock|lock -> its mount point
 *   {op:"drives"}                       the volumes udisks2 (or mountinfo) shows -> [{uuid, title, kind, mount, ...}]
 *   {op:"restore", target}              an item in a trash: back where it was; its new path
 *   {op:"empty_trash"}                  the home trash emptied (a delete job); the job's ID
 *   {op:"startup_add", target}         into Startup: an autostart entry for a program or
 *                                       executable, else a shadow; what was made
 *   {op:"msg_add", app, icon, summary, body, urgency, nid, sender, actions}   a message kept (messages.h); its ID
 *   {op:"msg_list"} -> {messages:[...newest first], unread}
 *   {op:"msg_set", ids|all, read, closed}  {op:"msg_delete", ids|all}  {op:"msg_closed", nid, sender}
 *   {event:"messages_changed", sub, unread, total, added}   to each subscribed client once
 *   {op:"ping"}                         "pong", with version (WPS_VERSION) and protocol (a number)
 *   {op:"resolve", target:PATH|"id:ID"}
 *   {op:"call", target, method, args:[...], expect:N}  objects as {obj:PATH|"id:ID"}
 *   {op:"describe", class}
 *   {op:"flush"}       reply once every pending change is written
 *   {op:"sync"}        rescan volumes and synchronise portable ones
 *   {op:"scan"}        run the catch-up scan now
 *   {op:"shadow", target, folder}   create a shadow (managed symlink)
 *   {op:"list", target, offset, count, details}   a page of a folder (streamed); with
 *                      details, an object with its own icon has "icon" (a PNG's path)
 *   {op:"select", folder, all, except|items}   a selection; returns its ID
 *   {op:"move"|"copy", selection, dest}        a job; returns its ID
 *   {op:"job", job} {op:"jobs"} {op:"job_cancel"|"job_pause"|"job_resume", job}
 *   {op:"job_decide", job, decision:"skip"|"rename", all}
 *   {op:"subscribe", objects:[...], folders:[...], jobs} -> sub; events follow:
 *       {event:"object_changed"|"content_added"|"content_removed"|"object_moved"|"job"|"volumes", sub, ...}
 *       {event:"layout_changed", sub, folder}  icon positions in a watched folder were stored
 *                                              (items a copy or move job dropped there)
 *   {op:"unsubscribe", sub}
 *   {op:"menu"|"settings", target}            menus and settings pages as data
 *   {op:"menu_select", target, item}  {op:"settings_set", target, values:{key:value}, expect}
 *   {op:"trash", selection} / {op:"delete", selection}
 *                      a job: into the freedesktop trash (items no trash can
 *                      take are listed as not_trashed), or deleted for good
 *   {op:"vdelete", target:"vobj:ID"}   delete a central object
 *   {op:"open", target, confirm}   start a program (as this user) or open a file;
 *                      a program owned by another user (not root) needs confirm
 *   {op:"from_template", target, folder, title}
 *   {op:"layout", target:FOLDER} -> {positions:{ID:"x,y"}, items:[{name,id,x,y}]}
 *   {op:"place", folder, items:[{target, x, y, lock?}]} -> [ID...]  set positions (and locks)
 *   {op:"arrange", target:FOLDER}   forget the folder's positions
 *   {op:"rename", target, name}   rename a file or folder in its folder (never
 *                      replacing); its title then follows the name. Without
 *                      permission to rename, the name becomes its title
 *                      (shared or private, as titles are). Others: title.
 *   {op:"create", folder, class, title, command}   a new object (file, folder,
 *                      .desktop program or .wpobj abstract object), never overwriting
 *   {op:"find", folder, name, subfolders, kind:"any"|"folder"|"file"|"program", limit}
 *       -> {find:ID}; then "found" events (sub: the ID) with items [{path, type, size, mtime}],
 *       done, scanned, truncated
 *   {op:"find_cancel", find:ID}
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
#include <pthread.h>
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
#include <sys/statvfs.h>
#include <sys/un.h>
#include <dirent.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <ctype.h>
#include <spawn.h>
#include <sys/xattr.h>

#include "cbor.h"
#include "db.h"
#include "env.h"
#include "objects.h"
#include "jobs.h"
#include "trash.h"
#include "udisks.h"
#include "programs.h"
#include "launch.h"
#include "wps_menu_ids.h"
#include "listing.h"
#include "find.h"
#include "plugins.h"
#include "shadows.h"
#include "WPShadow.h"
#include "WPProgram.h"
#include "WPFolder.h"
#include "WPTrashCan.h"
#include "WPDrive.h"
#include "WPMinWinViewer.h"
#include "WPRestoreDesktop.h"
#include "WPMessages.h"
#include "WPLaunchPad.h"
#include "WPWarpCenter.h"
#include "messages.h"
#include "startup.h"
#include "wps_startup_hook.h"
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
static char *settings_text(wpo_object *o);
static void put_menu(cbuf *b, const char *text, const char *parent);
static void put_settings(cbuf *b, const char *text, const char *icon_shown);
struct req;
static void do_open(struct req *r, wpo_object *o);
struct drive_req { struct client *c; int64_t id; bool open; char uuid[80], action[16], *pass; };
static void drive_done(void *arg, int err, const char *msg, const char *mount);
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
    sobj *crec = wps_sobj_of(o);
    bool central = crec && crec->central;
    if (!o) { cb_null(b); return; }
    const char *path = wps_object_path(o);
    char *title = wpo_is_a(o, WPObjectClass) ? WPObject_wpQueryTitle(o) : NULL;
    bool shadow = wpo_is_a(o, WPShadowClass);
    sobj *rec = wps_sobj_of(o);
    bool missing = rec && rec->declared_class[0];
    cb_map(b, 7 + (shadow ? 2 : 0) + (missing ? 1 : 0));
    cb_text(b, "path"); path ? cb_text(b, path) : cb_null(b);
    cb_text(b, "address");
    if (central) {
        char a[WPS_ID_LEN + 8];
        snprintf(a, sizeof a, "vobj:%s", WPObject_data(o)->id);
        cb_text(b, a);
    } else {
        path ? cb_text(b, path) : cb_null(b);
    }
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
        /* Not where it was last seen (moved while this user was away, say):
         * the shadow file's hint, if the object there is the right one;
         * and remember where it is now. */
        char tid[WPS_ID_LEN + 1], hint[PATH_MAX];
        if (r->shadow_target.err && r->tgt.path[0] && wps_shadow_read(r->tgt.path, tid, hint, sizeof hint) && hint[0] &&
            !strcmp(tid, r->tgt.shadow_of)) {
            wps_loaded alt;
            wps_load(hint, false, &alt);
            if (!alt.err && !strcmp(alt.id, r->tgt.shadow_of)) {
                wps_loaded_free(&r->shadow_target);
                r->shadow_target = alt;
                wps_db_set_location(db, alt.id, alt.vol, alt.rel);
            } else {
                wps_loaded_free(&alt);
            }
        }
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
    wpo_value av[MAX_ARGS] = { 0 };
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

/* Open With (v0.3-design 7): a data file's Open is a cascade of the
 * programs for its type, the default checked, then Other program...;
 * the GUI opens item WPS_MI_OPEN_APP + k with "app_index" k. */
static char *open_with_items(wpo_object *o, char *menu)
{
    const char *p = wpo_is_a(o, WPDataFileClass) ? WPFileSystem_data(o)->path : NULL;
    struct stat st;
    if (!p || stat(p, &st) != 0 || !S_ISREG(st.st_mode) || wps_is_program_file(p)) return menu;
    wps_app *a = malloc(WPS_MI_OPEN_APPS * sizeof *a);
    int n = a ? wps_apps_for_file(p, NULL, 0, a, WPS_MI_OPEN_APPS) : 0;
    size_t len = strlen(menu), cap = len + (size_t)n * 200 + 64;
    char *r = realloc(menu, cap);
    if (!r) { free(a); return menu; }
    for (int i = 0; i < n; i++) {
        char name[128];
        snprintf(name, sizeof name, "%s", a[i].name);
        for (char *q = name; *q; q++) if (*q == '\t' || *q == '\n') *q = ' ';
        len += (size_t)snprintf(r + len, cap - len, "%d\t%s\t1\t%s\n", WPS_MI_OPEN_APP + i, name, a[i].is_default ? "checked" : "");
    }
    snprintf(r + len, cap - len, "%d\tOther program...\t1\n", WPS_MI_OPEN_OTHER);
    free(a);
    return r;
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
        if (!r->remote_text) t = open_with_items(o, t);
        put_menu(&b, t, "");
        free(t);
    } else {
        char *t = settings_text(o);
        char shown[PATH_MAX];   /* what its Icon page shows without its own */
        const char *p = wpo_is_a(o, WPFileSystemClass) ? WPFileSystem_data(o)->path : NULL;
        wps_listing_shown_icon(p, wpo_is_a(o, WPFolderClass), wpo_is_a(o, WPProgramClass), shown, sizeof shown);
        put_settings(&b, t, shown);
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

struct vol_job {
    wps_volumes vs;
    struct simple *reply;     /* a "sync" request waiting, or NULL */
    char report[1024];
};

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
static void volume_release(const char *mnt)
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
static void volume_unrelease_all(void)
{
    pthread_mutex_lock(&vol_mu);
    for (int i = 0; i < 8; i++) released[i][0] = 0;
    pthread_mutex_unlock(&vol_mu);
}

static void start_volume_job(struct simple *reply);

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

/* The Trash Can went empty or full, or its icons changed: every window
 * draws it (and its shadows) again. */
static int trash_was_full = -1;

static void trash_broadcast(bool full)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead || (!x->nfolders && !x->ntargets)) continue;
        cbuf b;
        event_head(&b, "trash_changed", x->id, 1);
        cb_text(&b, "full"); cb_bool(&b, full);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

static void trash_check(const char *path)
{
    char files[PATH_MAX];
    wps_trash_home(files, sizeof files);
    strncat(files, "/files", sizeof files - strlen(files) - 1);
    size_t l = strlen(files);
    if (!path || strncmp(path, files, l) || path[l] != '/' || strchr(path + l + 1, '/')) return;
    bool full = wps_trash_full(files);
    if (trash_was_full == (int)full) return;
    trash_was_full = full;
    trash_broadcast(full);
}

/* The messages changed: every client with a subscription (each once). */
static void messages_broadcast(int64_t added)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        bool sent = false;
        for (struct sub *y = subs; y != x && !sent; y = y->next) sent = y->c == x->c;
        if (sent) continue;
        cbuf b;
        event_head(&b, "messages_changed", x->id, 3);
        cb_text(&b, "unread"); cb_uint(&b, wps_messages_unread());
        cb_text(&b, "total"); cb_uint(&b, wps_messages_count());
        cb_text(&b, "added"); cb_int(&b, added);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

struct shadow_ids { char ids[32][WPS_ID_LEN + 1]; int n; };

static void collect_shadow(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct shadow_ids *sh = user;
    if (sh->n < 32) snprintf(sh->ids[sh->n++], sizeof sh->ids[0], "%s", id);
}

static void on_object_changed(wpo_object *o)
{
    if (wpo_is_a(o, WPTrashCanClass)) trash_broadcast(trash_was_full == 1);   /* (its icons) */
    sobj *rec = wps_sobj_of(o);
    const char *path = rec ? rec->path : wps_object_path(o);
    const char *id = wpo_is_a(o, WPObjectClass) ? WPObject_data(o)->id : "";
    for (struct sub *x = subs; x; x = x->next) {
        bool hit = false;
        for (size_t i = 0; i < x->ntargets && !hit; i++)
            hit = (id[0] && !strcmp(x->targets[i], id)) || (path && !strcmp(x->targets[i], path));
        for (size_t i = 0; i < x->nfolders && !hit && path; i++) hit = in_folder(path, x->folders[i]);
        /* a central object (no file): the folders listing it, e.g. System
         * Setup's objects renamed - their windows showed the old title */
        if (!hit && rec && rec->central && id[0]) {
            char nm[WPS_ID_LEN + 16];
            snprintf(nm, sizeof nm, "\x01vobj:%s", id);
            for (size_t i = 0; i < x->nfolders && !hit; i++) hit = wps_listing_has(x->folders[i], nm);
        }
        if (!hit || x->c->dead) continue;
        cbuf b;
        event_head(&b, "object_changed", x->id, 1);
        cb_text(&b, "object"); put_object(&b, o);
        send_frame(x->c, &b);
        cb_free(&b);
    }
    /* its shadows show its title and icon: the folders listing them read
     * their rows again (System's own icon, chosen in its notebook, did
     * not reach the desktop's shadow of it) */
    if (!id[0]) return;
    struct shadow_ids sh = { 0 };
    wps_db_vobjs_shadowing(db, id, collect_shadow, &sh);
    for (int k = 0; k < sh.n; k++) {
        char nm[WPS_ID_LEN + 16], addr[WPS_ID_LEN + 8];
        snprintf(nm, sizeof nm, "\x01vobj:%s", sh.ids[k]);
        snprintf(addr, sizeof addr, "vobj:%s", sh.ids[k]);
        for (struct sub *x = subs; x; x = x->next) {
            bool hit = false;
            for (size_t i = 0; i < x->nfolders && !hit; i++) hit = wps_listing_has(x->folders[i], nm);
            if (!hit || x->c->dead) continue;
            cbuf b;
            event_head(&b, "object_changed", x->id, 1);
            cb_text(&b, "object");
            cb_map(&b, 1);
            cb_text(&b, "address"); cb_text(&b, addr);
            send_frame(x->c, &b);
            cb_free(&b);
        }
    }
}

static void on_fs_event(char kind, const char *from, const char *path);

/* Icon positions were stored in a folder without a client asking (a job's
 * dropped items, placed after the job reported it was done): windows on
 * it read them again, else they keep the spot they gave the new icon. */
static void on_layout_changed(const char *folder)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        bool hit = false;
        for (size_t i = 0; i < x->nfolders && !hit; i++) hit = !strcmp(x->folders[i], folder);
        if (!hit) continue;
        cbuf b;
        event_head(&b, "layout_changed", x->id, 1);
        cb_text(&b, "folder"); cb_text(&b, folder);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

/* A central object appeared in or left a folder: like a file doing so. */
static void on_central_change(const char *folder, const char *id, char kind)
{
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/\x01vobj:%s", folder, id);
    wps_listing_apply(folder, kind == 'c' ? 'c' : 'd', p + strlen(folder) + 1, NULL, DT_UNKNOWN);
    on_fs_event(kind, "", p);
}

static bool wps_folder(const char *path);

static void on_fs_event(char kind, const char *from, const char *path)
{
    /* One of WPS's own folders renamed or moved outside WPS (a shell, a
     * file manager): put back, as wpsd finds them by their paths (else the
     * desktop's shadows would follow the folder and wpsd would make a new,
     * empty one). The move back comes as its own event. */
    if (kind == 'm' && from[0] && path[0] && wps_folder(from) && access(from, F_OK) != 0) {
        if (rename(path, from) == 0) { log_msg("%s is WPS's own folder: moved back from %s", from, path); return; }
        log_msg("%s is WPS's own folder, moved to %s: cannot move it back (%s)", from, path, strerror(errno));
    }
    if (kind == 'm') wps_launch_moved(from, path);
    trash_check(path);
    if (kind == 'm') trash_check(from);
    /* A folder moved or renamed: windows open on it (or inside it) follow,
     * wherever they are, so every subscriber hears it. */
    struct stat dst;
    if (kind == 'm' && from[0] && path[0] && lstat(path, &dst) == 0 && S_ISDIR(dst.st_mode))
        for (struct sub *x = subs; x; x = x->next) {
            if (x->c->dead || (!x->nfolders && !x->ntargets)) continue;
            cbuf b;
            event_head(&b, "folder_moved", x->id, 2);
            cb_text(&b, "from"); cb_text(&b, from);
            cb_text(&b, "to"); cb_text(&b, path);
            send_frame(x->c, &b);
            cb_free(&b);
        }
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        for (size_t i = 0; i < x->nfolders; i++) {
            const char *f = x->folders[i];
            bool in_new = path[0] && in_folder(path, f), in_old = from[0] && in_folder(from, f);
            /* lock files are not listed (only their in-use emphasis shows) */
            if (in_new && wps_is_lock_file(strrchr(path, '/') + 1)) in_new = false;
            if (in_old && wps_is_lock_file(strrchr(from, '/') + 1)) in_old = false;
            if (!in_new && !in_old) continue;
            cbuf b;
            /* version: the folder's listing after this change (0: read it again) */
            uint64_t ver = wps_listing_version(f);
            if (kind == 'm' && in_new && in_old) {
                event_head(&b, "object_moved", x->id, 5);
                cb_text(&b, "folder"); cb_text(&b, f);
                cb_text(&b, "from"); cb_text(&b, strrchr(from, '/') + 1);
                cb_text(&b, "to"); cb_text(&b, strrchr(path, '/') + 1);
                cb_text(&b, "version"); cb_uint(&b, ver);
                cb_text(&b, "index"); cb_int(&b, wps_listing_last_index(f));   /* where it is, by position */
            } else {
                bool added = kind == 'c' || (kind == 'm' && in_new);
                const char *p = added ? path : (kind == 'm' ? from : path);
                const char *nm = strrchr(p, '/') + 1;
                event_head(&b, added ? "content_added" : "content_removed", x->id, 5);
                cb_text(&b, "folder"); cb_text(&b, f);
                cb_text(&b, "name"); cb_text(&b, nm);
                cb_text(&b, "version"); cb_uint(&b, ver);
                cb_text(&b, "index"); cb_int(&b, wps_listing_last_index(f));   /* where, by position (added: at the end) */
                struct stat st;   /* added: its type, as a listing gives it */
                cb_text(&b, "type");
                cb_text(&b, !added ? "" : nm[0] == '\x01' ? "object" : lstat(p, &st) != 0 ? "other" : S_ISDIR(st.st_mode) ? "folder" :
                            S_ISLNK(st.st_mode) ? "link" : S_ISREG(st.st_mode) ? "file" : "other");
            }
            send_frame(x->c, &b);
            cb_free(&b);
        }
    }
}

/* Programs WPS started opened or closed `path`: every window may show it
 * (directly, or as a shadow), so every subscription hears it. */
static void on_in_use(const char *path, bool in_use)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead || (!x->nfolders && !x->ntargets)) continue;
        cbuf b;
        event_head(&b, "in_use", x->id, 2);
        cb_text(&b, "path"); cb_text(&b, path);
        cb_text(&b, "in_use"); cb_bool(&b, in_use);
        send_frame(x->c, &b);
        cb_free(&b);
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
            else if (!strncmp(t, "vobj:", 5)) t += 5;   /* a central object: matched by its ID too */
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

static void put_settings(cbuf *b, const char *text, const char *icon_shown)
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
            if (!strcmp(x->type, "icon") && !x->choices[0] && icon_shown && *icon_shown)   /* (no commas in it: one choice) */
                snprintf(x->choices, sizeof x->choices, "%s", icon_shown);
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

/* The folder an object's icon sits in (loaded: a window lists it), and
 * its path; NULL if not known. */
static wpo_object *home_folder_of(wpo_object *o, char *fpath, size_t n)
{
    sobj *s = wps_sobj_of(o), *fs = NULL;
    if (!s) return NULL;
    if (s->central) fs = s->folder_id[0] ? wps_sobj_by_id(s->folder_id) : NULL;
    else {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s", s->path);
        char *sl = strrchr(p, '/');
        if (!sl || !sl[1]) return NULL;
        if (sl == p) sl[1] = 0;
        else *sl = 0;
        fs = wps_sobj_by_path(p);
    }
    if (!fs || !fs->o || !wpo_is_a(fs->o, WPFolderClass)) return NULL;
    snprintf(fpath, n, "%s", fs->path);
    return fs->o;
}

/* Its pages, and on the General page Lock position (v0.3-design 5: kept
 * by the folder it sits in, with its position). */
static char *settings_text(wpo_object *o)
{
    char *t = call_text(o, "wpAddSettingsPages", ""), fpath[PATH_MAX];
    wpo_object *f = home_folder_of(o, fpath, sizeof fpath);
    char *g = strstr(t, "page\tGeneral\n");
    if (!f || !g || (g != t && g[-1] != '\n')) return t;
    char *next = strstr(g + 1, "\npage\t");
    size_t at = next ? (size_t)(next - t) + 1 : strlen(t);
    if (!next && at && t[at - 1] != '\n') return t;
    char line[160];
    snprintf(line, sizeof line, "field\tlock_pos\tcheckbox\tLock position\t%d\t@lock\n", WPFolder_wpQueryIconLock(f, o) ? 1 : 0);
    size_t tl = strlen(t), ll = strlen(line);
    char *r = malloc(tl + ll + 1);
    memcpy(r, t, at);
    memcpy(r + at, line, ll);
    memcpy(r + at + ll, t + at, tl - at + 1);
    free(t);
    return r;
}

/* settings_set: apply {key: value} through each field's setter. */
static const char *apply_settings(wpo_object *o, const cv *values)
{
    char *text = settings_text(o);
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
        if (!strcmp(f->setter, "@lock")) {
            char fpath[PATH_MAX];
            wpo_object *fo = home_folder_of(o, fpath, sizeof fpath);
            bool on = val->k == CV_BOOL ? val->b : val->k == CV_INT ? val->i != 0 : false;
            if (!fo || !WPFolder_wpSetIconLock(fo, o, on)) { err = "the setter refused the value"; break; }
            on_layout_changed(fpath);   /* its windows read the positions again */
            WPObject_wpObjectChanged(o);  /* (its notebook shows it) */
            continue;
        }
        if (!strncmp(f->setter, "@col:", 5)) {
            /* a details view's column on or off: wpSetColumn("name=1") */
            char arg[64];
            bool on = val->k == CV_BOOL ? val->b : val->k == CV_INT ? val->i != 0 : false;
            snprintf(arg, sizeof arg, "%s=%d", f->setter + 5, on);
            wpo_value av = wpo_v_string(arg);
            if (wpo_send(o, "wpSetColumn", &av, 1, &r) != WPO_OK || (r.type == 'b' && !r.u.b)) err = "the setter refused the value";
            wpo_value_clear(&av);
            wpo_value_clear(&r);
            continue;
        }
        if (!strcmp(f->setter, "@")) {
            /* A settings object's field: wpSetSetting(key, value as text). */
            char num[32];
            const char *txt = val->k == CV_TEXT ? val->s : NULL;
            if (val->k == CV_INT) { snprintf(num, sizeof num, "%lld", (long long)val->i); txt = num; }
            else if (val->k == CV_BOOL) txt = val->b ? "1" : "0";
            if (!txt) { err = "unsupported value"; break; }
            wpo_value args[2] = { wpo_v_string(f->key), wpo_v_string(txt) };
            if (wpo_send(o, "wpSetSetting", args, 2, &r) != WPO_OK || (r.type == 'b' && !r.u.b)) err = "the setter refused the value";
            wpo_value_clear(&args[0]);
            wpo_value_clear(&args[1]);
            wpo_value_clear(&r);
            continue;
        }
        if (val->k == CV_TEXT) a = wpo_v_string(val->s);
        else if (val->k == CV_INT) a = wpo_v_int(val->i);
        else if (val->k == CV_BOOL) a = wpo_v_bool(val->b);
        else { err = "unsupported value"; break; }
        if (wpo_send(o, f->setter, &a, 1, &r) != WPO_OK) err = "the setter refused the value";
        else if (r.type == 'b' && !r.u.b) err = "the setter refused the value";
        wpo_value_clear(&a);
        wpo_value_clear(&r);
        /* a folder's sort: its listing is read again in the new order */
        if (!err && (!strcmp(f->key, "sort_key") || !strcmp(f->key, "sort_desc")) && wpo_is_a(o, WPFileSystemClass))
            wps_listing_invalidate(WPFileSystem_data(o)->path);
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

/* ---- list: a page of a folder's entries */

struct list_req { struct client *c; int64_t id; char folder[PATH_MAX]; };

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
            cb_map(&b, 5);
            cb_text(&b, "version"); cb_uint(&b, pg->version);
            cb_text(&b, "total"); cb_uint(&b, pg->total);
            cb_text(&b, "complete"); cb_bool(&b, pg->complete);
            cb_text(&b, "offset"); cb_uint(&b, pg->offset);
            cb_text(&b, "items");
            cb_array(&b, pg->n);
            for (size_t i = 0; i < pg->n; i++) {
                const wps_entry *e = &pg->items[i];
                const char *t = e->name[0] == '\x01' ? "object" : e->type == DT_DIR ? "folder" : e->type == DT_LNK ? "link" :
                                e->type == DT_REG ? "file" : "other";
                bool iu = false;
                if (e->detailed) {
                    /* In use: a lock file says so, or a program WPS started has it open. */
                    char fp[PATH_MAX + NAME_MAX + 2];
                    snprintf(fp, sizeof fp, "%s/%s", strcmp(r->folder, "/") ? r->folder : "", e->name);
                    iu = e->locked || (e->name[0] != '\x01' && wps_launch_in_use(fp)) ||
                         ((e->shadow || e->type == DT_LNK) && e->target_path[0] && wps_launch_in_use(e->target_path));
                }
                bool link = e->detailed && !e->shadow && e->type == DT_LNK;
                bool tpl = e->detailed && e->is_template, makes = e->detailed && e->makes[0], icon = e->detailed && e->icon[0];
                cb_map(&b, e->detailed ? (e->shadow ? 9 : link ? 8 : 6) + iu + tpl + makes + icon : 2);
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
                        cb_text(&b, "target_path"); cb_text(&b, e->target_path);
                    }
                    if (link) {   /* "folder", "file", or "missing" for a dangling link */
                        cb_text(&b, "target");
                        cb_text(&b, e->target_type == DT_DIR ? "folder" : e->target_type == DT_REG ? "file" : "missing");
                        cb_text(&b, "target_path"); cb_text(&b, e->target_path);
                    }
                    if (iu) { cb_text(&b, "in_use"); cb_bool(&b, true); }
                    if (tpl) { cb_text(&b, "template"); cb_bool(&b, true); }
                    if (makes) { cb_text(&b, "makes"); cb_text(&b, e->makes); }   /* a built-in template */
                    if (icon) { cb_text(&b, "icon"); cb_text(&b, e->icon); }       /* its own icon (a PNG) */
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

/* An object the desktop shows itself (a view of its own, e.g. the
 * Minimized Window Viewer's): {view: name}. */
static void reply_view(struct req *r, const char *view)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, 1);
    cb_text(&b, "view"); cb_text(&b, view);
    send_frame(r->c, &b);
    cb_free(&b);
}

static void reply_folder(struct req *r, const char *path)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, 3);
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    cb_map(&b, 1);
    cb_text(&b, "folder"); cb_text(&b, path);   /* the GUI opens a folder window */
    send_frame(r->c, &b);
    cb_free(&b);
}

/* A program another user (not root) owns asks first: nobody can plant
 * something that runs when another user opens it. */
static void ask_first(struct req *r, const char *cmd, uid_t owner)
{
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
}

static void do_open(struct req *r, wpo_object *o)
{
    const cv *cf = cv_get(&r->msg, "confirm");
    bool confirmed = cf && cf->k == CV_BOOL && cf->b;
    char buf[8192], *argv[64], cwd[PATH_MAX] = "";
    /* objects dropped on a program: it starts with them (files: absolute
     * paths, at most 32) */
    const char *files[32];
    size_t nfiles = 0;
    const cv *fl = cv_get(&r->msg, "files");
    for (size_t i = 0; fl && fl->k == CV_ARRAY && i < fl->n && nfiles < 32; i++)
        if (fl->items[i].k == CV_TEXT && fl->items[i].s[0] == '/' && strlen(fl->items[i].s) < PATH_MAX) files[nfiles++] = fl->items[i].s;
    const char *what;
    const char *tracked;   /* what shows as in use while it runs */
    const char *path = r->tgt.path;
    const struct stat *st = &r->tgt.st;
    wpo_object *orig = NULL;
    /* A shadow opens its original, wherever that is now. */
    if (wpo_is_a(o, WPShadowClass)) {
        orig = WPShadow_wpQueryTarget(o);
        if (!orig || r->shadow_target.err || !r->shadow_target.path[0]) {
            wpo_unref(orig);
            reply_error(r->c, r->id, "unavailable", "the original is not available (on a volume that is not mounted, or gone)");
            return;
        }
        o = orig;
        path = r->shadow_target.path;
        st = &r->shadow_target.st;
    }
    if (wpo_is_a(o, WPFolderClass)) { reply_folder(r, path); wpo_unref(orig); return; }
    /* A plain file-system link opens what it points to, as that would. */
    struct stat ts;
    char real[PATH_MAX];
    bool data = wpo_is_a(o, WPDataFileClass);
    if (S_ISLNK(st->st_mode) && stat(path, &ts) == 0 && realpath(path, real)) {
        if (S_ISDIR(ts.st_mode)) { reply_folder(r, real); wpo_unref(orig); return; }
        path = real;
        st = &ts;
        data = true;
    }
    if (!data && wpo_is_a(o, WPProgramClass)) {
        const char *cmd = WPProgram_data(o)->command;
        if (!cmd || !*cmd) { reply_error(r->c, r->id, "failed", "the program has no command"); wpo_unref(orig); return; }
        if (st->st_uid != geteuid() && st->st_uid != 0 && !confirmed) { ask_first(r, cmd, st->st_uid); wpo_unref(orig); return; }
        int argc = wps_split_exec_files(cmd, nfiles ? files : NULL, nfiles, buf, sizeof buf, argv, 64);
        if (argc == 0) { reply_error(r->c, r->id, "failed", "empty command"); wpo_unref(orig); return; }
        size_t pl = strlen(path);
        if (pl > 8 && !strcmp(path + pl - 8, ".desktop") && wps_desktop_wants_terminal(path) &&
            !wps_terminal_wrap(argv, argc, 64)) {
            reply_error(r->c, r->id, "failed", "it runs in a terminal, and no terminal program was found (set WPS_TERMINAL)");
            wpo_unref(orig);
            return;
        }
        what = cmd;
        tracked = path;
    } else if (data) {
        tracked = path;
        if (S_ISREG(st->st_mode) && wps_is_program_file(path)) {
            /* An executable runs (as a program file does in OS/2), in its
             * own folder; one another user owns asks first. */
            if (st->st_uid != geteuid() && st->st_uid != 0 && !confirmed) { ask_first(r, path, st->st_uid); wpo_unref(orig); return; }
            argv[0] = (char *)path;
            int na = 1;
            for (size_t k = 0; k < nfiles && na < 60; k++) argv[na++] = (char *)files[k];   /* dropped on it */
            argv[na] = NULL;
            what = path;
            if (wps_program_needs_terminal(path) && !wps_terminal_wrap(argv, na, 64)) {
                reply_error(r->c, r->id, "failed", "a text-mode program, and no terminal program was found (set WPS_TERMINAL)");
                wpo_unref(orig);
                return;
            }
            snprintf(cwd, sizeof cwd, "%s", path);
            *strrchr(cwd, '/') = 0;
            if (!cwd[0]) strcpy(cwd, "/");
        } else if (cv_text(&r->msg, "app") || cv_get(&r->msg, "app_index")) {
            /* Open With: the program chosen (by its ID, or its place in the
             * Open cascade), made the type's default if asked */
            const char *app = cv_text(&r->msg, "app");
            int64_t k = -1;
            wps_app *a = NULL;
            if (!app && cv_int(&r->msg, "app_index", &k) && k >= 0 && k < WPS_MI_OPEN_APPS) {
                a = malloc(WPS_MI_OPEN_APPS * sizeof *a);
                int n = a ? wps_apps_for_file(path, NULL, 0, a, WPS_MI_OPEN_APPS) : 0;
                if (k < n) app = a[k].id;
            }
            const cv *al = cv_get(&r->msg, "always");
            bool ok = app != NULL;
            if (ok && al && al->k == CV_BOOL && al->b && !wps_app_set_default(path, app)) {
                reply_error(r->c, r->id, "failed", "the default program for the type could not be stored (mimeapps.list)");
                free(a);
                wpo_unref(orig);
                return;
            }
            if (!ok || wps_app_argv(app, path, buf, sizeof buf, argv, 64) <= 0) {
                reply_error(r->c, r->id, "failed", "that program cannot be started (not installed, or it runs in a terminal and there is none)");
                free(a);
                wpo_unref(orig);
                return;
            }
            free(a);
            what = argv[0];
        } else if (wps_handler_argv(path, buf, sizeof buf, argv, 64) > 0) {
            /* Its type's default application, started directly so that its
             * process stands for the open file (xdg-open hands over and exits). */
            what = argv[0];
        } else {
            argv[0] = "xdg-open";
            argv[1] = (char *)path;
            argv[2] = NULL;
            what = "xdg-open";
        }
    } else if (wpo_is_a(o, WPLaunchPadClass) || wpo_is_a(o, WPWarpCenterClass)) {
        /* opened as the palette or the bar: the desktop shows it, or
         * brings it forward (never two) */
        reply_view(r, wpo_is_a(o, WPLaunchPadClass) ? "launchpad" : "warpcenter");
        wpo_unref(orig);
        return;
    } else if (wpo_is_a(o, WPMessagesClass)) {
        reply_view(r, "messages");
        wpo_unref(orig);
        return;
    } else if (wpo_is_a(o, WPRestoreDesktopClass)) {
        reply_view(r, "restore-defaults");   /* the desktop asks, then sends restore_defaults */
        wpo_unref(orig);
        return;
    } else if (wpo_is_a(o, WPMinWinViewerClass)) {
        reply_view(r, "minimized-windows");
        wpo_unref(orig);
        return;
    } else if (wpo_is_a(o, WPDriveClass)) {
        /* a drive: its top folder, mounted first if need be */
        const char *u = WPDrive_data(o)->uuid;
        wps_drive_info d;
        if (!u || !wps_udisks_find(u, &d)) reply_error(r->c, r->id, "not found", "the drive is not there (plugged out?)");
        else if (d.mount[0]) reply_folder(r, d.mount);
        else if (d.encrypted && !d.unlocked) reply_error(r->c, r->id, "locked", "the volume is locked: unlock it first");
        else {
            struct drive_req *q = calloc(1, sizeof *q);
            q->c = r->c; r->c->refs++;
            q->id = r->id;
            q->open = true;
            wps_udisks_call(u, "mount", NULL, drive_done, q);
        }
        wpo_unref(orig);
        return;
    } else {
        reply_error(r->c, r->id, "bad request", "this kind of object opens in the GUI");
        wpo_unref(orig);
        return;
    }
    /* On the asking GUI's display: a socket path that is this user's. */
    const char *disp = cv_text(&r->msg, "display");
    struct stat ds;
    if (disp && (disp[0] != '/' || stat(disp, &ds) != 0 || !S_ISSOCK(ds.st_mode) || ds.st_uid != geteuid())) disp = NULL;
    /* and its X display (":N" or ":N.S", nothing else) */
    const char *x11 = cv_text(&r->msg, "x11");
    if (x11) {
        const char *q = x11;
        bool ok = *q++ == ':' && isdigit((unsigned char)*q);
        while (ok && isdigit((unsigned char)*q)) q++;
        if (ok && *q == '.') { q++; ok = isdigit((unsigned char)*q); while (ok && isdigit((unsigned char)*q)) q++; }
        if (!ok || *q || strlen(x11) > 16) x11 = NULL;
    }
    /* and the asking session's input-method and desktop settings: these
     * names only, short printable values */
    const char *envs[8];
    size_t nenv = 0;
    const cv *ev = cv_get(&r->msg, "env");
    static const char *const allowed[] = { "GTK_IM_MODULE=", "QT_IM_MODULE=", "XMODIFIERS=", "XDG_CURRENT_DESKTOP=", "XDG_SESSION_TYPE=" };
    for (size_t i = 0; ev && ev->k == CV_ARRAY && i < ev->n && nenv < 8; i++) {
        const cv *e = &ev->items[i];
        if (e->k != CV_TEXT || strlen(e->s) > 280) continue;
        bool ok = false;
        for (size_t k = 0; k < 5 && !ok; k++) ok = !strncmp(e->s, allowed[k], strlen(allowed[k]));
        for (const char *p = e->s; ok && *p; p++) ok = (unsigned char)*p >= 0x20 && *p != 0x7f;
        if (ok) envs[nenv++] = e->s;
    }
    pid_t pid = wps_launch(argv, tracked, cwd[0] ? cwd : NULL, disp, x11, envs, nenv);   /* as this user, in this user's session */
    if (pid < 0) { reply_error(r->c, r->id, "failed", strerror(errno)); wpo_unref(orig); return; }
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
    wpo_unref(orig);   /* after the reply: `what` may be the original's command */
}

/* ---- from_template: a new object copied from a template */

struct tmpl_job {
    struct client *c;
    int64_t id;
    char src[PATH_MAX], folder[PATH_MAX], title[256], path[PATH_MAX + 300];
    int err;
    wps_loaded ld;
};

/* Copy the user.wps.* attributes of one open file to another, without
 * the object ID (a new object gets its own) and, at the top, without the
 * template mark. */
static void tmpl_attrs(int in, int out, bool top)
{
    char names[8192];
    ssize_t nl = flistxattr(in, names, sizeof names);
    for (ssize_t o = 0; nl > 0 && o < nl; o += (ssize_t)strlen(names + o) + 1) {
        const char *nm = names + o;
        if (strncmp(nm, "user.wps.", 9) || !strcmp(nm, WPS_ATTR_ID)) continue;
        unsigned char v[65536];
        ssize_t vl = fgetxattr(in, nm, v, sizeof v);
        if (vl < 0) continue;
        if (top && !strcmp(nm, "user.wps.WPObject")) {
            wps_props ps;
            if (wps_props_decode(v, (size_t)vl, &ps) == 0) {
                wps_props_remove(&ps, "template");
                wps_props_remove(&ps, "title");   /* the copy is titled by its own name */
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
}

/* A folder template's contents, copied (links as links; nothing else). */
static int tmpl_copy_tree(const char *src, const char *dst, int depth)
{
    if (depth > 64) return ELOOP;
    DIR *d = opendir(src);
    if (!d) return errno;
    struct dirent *de;
    int err = 0;
    while (!err && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char s[PATH_MAX], t[PATH_MAX];
        snprintf(s, sizeof s, "%s/%s", src, de->d_name);
        snprintf(t, sizeof t, "%s/%s", dst, de->d_name);
        struct stat st;
        if (lstat(s, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(t, st.st_mode & 07777) != 0) { err = errno; break; }
            int a = open(s, O_RDONLY | O_DIRECTORY | O_CLOEXEC), b = open(t, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (a >= 0 && b >= 0) tmpl_attrs(a, b, false);
            if (a >= 0) close(a);
            if (b >= 0) close(b);
            err = tmpl_copy_tree(s, t, depth + 1);
        } else if (S_ISLNK(st.st_mode)) {
            char l[PATH_MAX];
            ssize_t n = readlink(s, l, sizeof l - 1);
            if (n >= 0) { l[n] = 0; if (symlink(l, t) != 0) err = errno; }
        } else if (S_ISREG(st.st_mode)) {
            int in = open(s, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            int out = in < 0 ? -1 : open(t, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
            if (in < 0 || out < 0) err = errno;
            char buf[65536];
            ssize_t n;
            while (!err && (n = read(in, buf, sizeof buf)) > 0)
                if (write(out, buf, (size_t)n) != n) err = EIO;
            if (!err) tmpl_attrs(in, out, false);
            if (in >= 0) close(in);
            if (out >= 0) close(out);
        }
    }
    closedir(d);
    return err;
}

static void tmpl_work(void *arg)
{
    struct tmpl_job *j = arg;
    struct stat sst;
    if (lstat(j->src, &sst) != 0) { j->err = errno; return; }
    if (S_ISDIR(sst.st_mode)) {
        /* A folder template: a new folder with a copy of what it holds. */
        int e = EEXIST;
        for (int k = 1; k < 100 && e == EEXIST; k++) {
            if (k == 1) snprintf(j->path, sizeof j->path, "%s/%.200s", j->folder, j->title);
            else snprintf(j->path, sizeof j->path, "%s/%.200s (%d)", j->folder, j->title, k);
            e = mkdir(j->path, 0777) == 0 ? 0 : errno;   /* never into an existing one */
        }
        if (e) { j->err = e; return; }
        int a = open(j->src, O_RDONLY | O_DIRECTORY | O_CLOEXEC), b = open(j->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (a >= 0 && b >= 0) tmpl_attrs(a, b, true);
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        j->err = tmpl_copy_tree(j->src, j->path, 0);
        if (!j->err) wps_load(j->path, false, &j->ld);
        return;
    }
    const char *base = strrchr(j->src, '/') ? strrchr(j->src, '/') + 1 : j->src;
    const char *ext = strchr(base, '.') ? strchr(base, '.') : "";
    /* A title that already ends in the extension (a file named as it is
     * titled) does not get it twice. */
    size_t tl = strlen(j->title), el = strlen(ext);
    if (el && tl > el && !strcmp(j->title + tl - el, ext)) j->title[tl - el] = 0;
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
    tmpl_attrs(in, out, true);
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
            /* Titled by its name, like any file (see create_done). */
            if (WPObject_data(o)->title) WPObject_wpSetTitle(o, NULL);
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
    /* Central objects of this folder are named "\x01vobj:<id>". */
    char fid[WPS_ID_LEN + 1];
    if (found < j->n && wps_attr_get_id(j->folder, fid)) {
        for (size_t i = 0; i < j->n; i++) {
            if (j->names[i]) continue;
            char loc[WPS_ID_LEN + 8];
            snprintf(loc, sizeof loc, "vobj:%s", j->ids[i]);
            wps_loaded ld;
            wps_load(loc, false, &ld);
            if (!ld.err && !strcmp(ld.folder_id, fid)) {
                char nm[WPS_ID_LEN + 8];
                snprintf(nm, sizeof nm, "\x01vobj:%s", j->ids[i]);
                j->names[i] = strdup(nm);
            }
            wps_loaded_free(&ld);
        }
    }
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
            bool has = sscanf(j->vals[i], "%lld,%lld", &x, &y) == 2;
            size_t vl = strlen(j->vals[i]);
            bool locked = vl && j->vals[i][vl - 1] == 'L';   /* "x,y,L"; "L": locked, no position yet */
            cb_map(&b, 2 + (has ? 2 : 0) + (locked ? 1 : 0));
            cb_text(&b, "name"); cb_text(&b, j->names[i]);
            cb_text(&b, "id"); cb_text(&b, j->ids[i]);
            if (has) { cb_text(&b, "x"); cb_int(&b, x); cb_text(&b, "y"); cb_int(&b, y); }
            if (locked) { cb_text(&b, "locked"); cb_bool(&b, true); }
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
        if (!strncmp(cs->props.p[i].key, "pos:", 4)
            && !(cs->props.p[i].len && ((const char *)cs->props.p[i].val)[cs->props.p[i].len - 1] == 'L')) {   /* (locked: kept) */
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
    int8_t *lock;        /* per item: -1 as it was, 0 unlock, 1 lock */
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
    if (!j->c) {   /* placing items a job dropped: nobody waits for a reply */
        for (size_t i = 0; f && wpo_is_a(f, WPFolderClass) && i < j->n; i++) {
            wpo_object *o = j->items[i].err ? NULL : wps_object_from_loaded(&j->items[i]);
            if (o) WPFolder_wpSetIconPos(f, o, j->x[i], j->y[i]);
        }
        if (f && wpo_is_a(f, WPFolderClass)) on_layout_changed(j->folder);
    } else if (!j->c->dead) {
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
                if (ok && j->lock && j->lock[i] >= 0) ok = WPFolder_wpSetIconLock(f, o, j->lock[i] == 1);
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
    free(j->lock);
    if (j->c) client_unref(j->c);
    free(j);
}

/* A job dropped items at a point: place them once they have their names. */
static void job_placed(const char *dest, char **paths, int64_t *x, int64_t *y, size_t n)
{
    struct place_job *j = calloc(1, sizeof *j);
    snprintf(j->folder, sizeof j->folder, "%s", dest);
    j->targets = calloc(n ? n : 1, sizeof *j->targets);
    j->x = calloc(n ? n : 1, sizeof *j->x);
    j->y = calloc(n ? n : 1, sizeof *j->y);
    j->items = calloc(n ? n : 1, sizeof *j->items);
    for (size_t i = 0; i < n; i++) {
        j->targets[i] = strdup(paths[i]);
        j->x[i] = x[i];
        j->y[i] = y[i];
    }
    j->n = n;
    wpo_defer(loop, "place dropped items", place_work, place_done, j);
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
    j->lock = malloc(items->n ? items->n : 1);
    for (size_t i = 0; i < items->n; i++) {
        const char *t = cv_text(&items->items[i], "target");
        if (!t) continue;
        j->targets[j->n] = strdup(t);
        cv_int(&items->items[i], "x", &j->x[j->n]);
        cv_int(&items->items[i], "y", &j->y[j->n]);
        const cv *lk = cv_get(&items->items[i], "lock");
        j->lock[j->n] = lk && lk->k == CV_BOOL ? lk->b : -1;
        j->n++;
    }
    wpo_defer(loop, "place icons", place_work, place_done, j);
}


/* ---- vmove: move or copy a central object to another folder */

struct vmove_job {
    struct client *c;
    int64_t id;
    char target[64], dest[PATH_MAX], from_folder[PATH_MAX];
    bool copy, del;         /* del: vdelete, the object is deleted */
    bool trash;             /* vtrash: into the Trash Can, its folder noted (restore) */
    char new_id[WPS_ID_LEN + 1];
    int err;
};

static void vmove_work(void *arg)
{
    struct vmove_job *j = arg;
    wps_loaded ld, dst, from;
    wps_load(j->target, false, &ld);
    if (j->del) {
        if (ld.err || !ld.central) j->err = ENOENT;
        else {
            char fid[WPS_ID_LEN + 8];
            snprintf(fid, sizeof fid, "id:%s", ld.folder_id);
            wps_load(fid, false, &from);
            if (!from.err) snprintf(j->from_folder, sizeof j->from_folder, "%s", from.path);
            wps_loaded_free(&from);
            j->err = wps_central_delete(ld.id);
        }
        wps_loaded_free(&ld);
        return;
    }
    wps_load(j->dest, true, &dst);
    if (ld.err || !ld.central) j->err = ENOENT;
    else if (dst.err || !S_ISDIR(dst.st.st_mode) || !dst.id[0]) j->err = ENOTDIR;
    else {
        char fid[WPS_ID_LEN + 8];
        snprintf(fid, sizeof fid, "id:%s", ld.folder_id);
        wps_load(fid, false, &from);
        if (!from.err) snprintf(j->from_folder, sizeof j->from_folder, "%s", from.path);
        wps_loaded_free(&from);
        j->err = wps_central_move(ld.id, dst.vol, dst.id, j->copy, j->new_id);
        if (!j->err && j->trash) {   /* where it was, for Restore */
            char loc[WPS_ID_LEN + 8], key[PATH_MAX + 128];
            snprintf(loc, sizeof loc, "vobj:%s", j->new_id);
            wps_db_key(key, sizeof key, loc, "WPS", "origin");
            wps_db_put(db, dst.vol, key, 's', ld.folder_id, WPS_ID_LEN);
        }
    }
    wps_loaded_free(&ld);
    wps_loaded_free(&dst);
}

static void vmove_done(void *arg)
{
    struct vmove_job *j = arg;
    if (!j->err && j->del) {
        wps_central_forget(j->target + 5);
        if (j->from_folder[0]) on_central_change(j->from_folder, j->target + 5, 'd');
    } else if (!j->err) {
        if (!j->copy && j->from_folder[0]) on_central_change(j->from_folder, j->target + 5, 'd');
        on_central_change(j->dest, j->new_id, 'c');
    }
    if (!j->c->dead) {
        if (j->err) reply_error(j->c, j->id, "failed", strerror(j->err));
        else {
            char a[WPS_ID_LEN + 8];
            snprintf(a, sizeof a, "vobj:%s", j->del ? j->target + 5 : j->new_id);
            struct simple s = { .c = j->c, .id = j->id };
            j->c->refs++;
            struct simple *sp = malloc(sizeof *sp);
            *sp = s;
            reply_simple_ok(sp, a);
        }
    }
    client_unref(j->c);
    free(j);
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
    /* A title with line breaks (OS/2's): the file gets spaces in their
     * place, the title keeps them. */
    char fsname[NAME_MAX + 1];
    snprintf(fsname, sizeof fsname, "%s", j->name);
    for (char *p = fsname; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    snprintf(j->newpath, sizeof j->newpath, "%s/%s", dir[0] ? dir : "", fsname);
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
            if (j->as_title || strchr(j->name, '\n')) WPObject_wpSetTitle(o, j->name);
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

static void computer_dirs(char *comp, size_t cn, char *drives, size_t dn);
static void system_dir(char *out, size_t n);
static void setup_dir(char *out, size_t n);
static void data_dir(const char *name, char *out, size_t n);
static void autostart_dir(char *out, size_t n);

static char known_desktop[PATH_MAX];   /* the desktop's folder (its first_start says) */

/* WPS's own folders: System, My Storages, Drives, Programs, System Setup,
 * the Trash Can, Startup; and the desktop and Templates (XDG user folders:
 * renamed, user-dirs.dirs would not follow). */
static bool wps_folder(const char *path);

static bool is_desktop(const char *path) { return known_desktop[0] && path && !strcmp(path, known_desktop); }

static bool own_folder(const char *path)
{
    if (!path || !*path) return false;
    if (known_desktop[0] && !strcmp(path, known_desktop)) return true;
    if (wps_templates_dir()[0] && !strcmp(path, wps_templates_dir())) return true;
    return wps_folder(path);
}

/* The folders wpsd finds by their paths (the desktop and Templates are
 * the user's: user-dirs.dirs says where they are). */
static bool wps_folder(const char *path)
{
    if (!path || !*path) return false;
    char d[7][PATH_MAX + 8];
    system_dir(d[0], sizeof d[0]);
    computer_dirs(d[1], sizeof d[1], d[2], sizeof d[2]);
    data_dir("Programs", d[3], sizeof d[3]);
    setup_dir(d[4], sizeof d[4]);
    wps_trash_home(d[5], sizeof d[5]);
    strncat(d[5], "/files", sizeof d[5] - strlen(d[5]) - 1);
    autostart_dir(d[6], sizeof d[6]);
    for (int i = 0; i < 7; i++) if (!strcmp(path, d[i])) return true;
    return false;
}

static void do_rename(struct req *r, wpo_object *o)
{
    const char *name = cv_text(&r->msg, "name");
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) > NAME_MAX) {
        reply_error(r->c, r->id, "bad request", "a name must be non-empty, without '/', and not . or ..");
        return;
    }
    if (!wpo_is_a(o, WPFileSystemClass) || own_folder(r->tgt.path)) {
        /* programs and abstract objects, and WPS's own folders (found by
         * their paths: renamed, they would be lost): the title */
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

static void start_create(struct client *c, int64_t id, const char *folder, const char *cls, const char *title,
                         const char *command);

static void do_from_template(struct req *r, wpo_object *o)
{
    const char *folder = cv_text(&r->msg, "folder"), *title = cv_text(&r->msg, "title");
    /* A template: marked as one, or anything in the templates folder. */
    char *makes = NULL;
    wps_state_get_str(o, "WPS", "makes", &makes);
    bool tpl = WPObject_data(o)->is_template || (makes && *makes) || (r->tgt.path[0] && wps_in_templates(r->tgt.path));
    if (!tpl) { free(makes); reply_error(r->c, r->id, "bad request", "not a template"); return; }
    if (!folder) { free(makes); reply_error(r->c, r->id, "bad request", "from_template needs a folder"); return; }
    char *t = WPObject_wpQueryTitle(o);
    if (makes && *makes) {
        /* A built-in template (Folder, Data File, Program): a new, empty
         * object of the class it makes, titled like the template. */
        wpo_class *mc = wpo_find_class(makes);
        if (!mc || mc->is_meta || !wpo_subclass_of(mc, WPObjectClass) || wpo_subclass_of(mc, WPShadowClass))
            reply_error(r->c, r->id, "failed", "the template makes an unknown kind of object");
        else
            start_create(r->c, r->id, folder, makes, title && *title ? title : t ? t : "New", NULL);
        free(makes);
        free(t);
        return;
    }
    free(makes);
    if (!S_ISREG(r->tgt.st.st_mode) && !S_ISDIR(r->tgt.st.st_mode)) {
        free(t);
        reply_error(r->c, r->id, "bad request", "only file and folder templates");
        return;
    }
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
    bool central;
    wps_loaded ld;
};

static void create_work(void *arg)
{
    struct create_req *r = arg;
    wpo_class *c = wpo_find_class(r->cls);   /* the registry is read-only after start-up */
    if (wpo_subclass_of(c, WPAbstractClass) && !wpo_subclass_of(c, WPProgramClass)) {
        wps_loaded fld;
        wps_load(r->folder, true, &fld);
        bool central = !fld.err && fld.id[0] && !wps_folder_keeps_files(&fld);
        if (central) {
            char id[WPS_ID_LEN + 1];
            r->err = wps_central_create(fld.vol, fld.id, r->cls, r->title, NULL, id);
            if (!r->err) {
                snprintf(r->path, sizeof r->path, "vobj:%s", id);
                wps_load(r->path, false, &r->ld);
                r->central = true;
            }
        }
        wps_loaded_free(&fld);
        if (central) return;
    }
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
            /* A file or folder is titled by its name (renaming its icon renames
             * it): a title of its own would hide a numbered name, "Folder (2)". */
            if (r->title[0] && !wpo_is_a(o, WPFileSystemClass)) WPObject_wpSetTitle(o, r->title);
            if (r->command[0] && wpo_is_a(o, WPProgramClass)) WPProgram_wpSetCommand(o, r->command);
            if (r->central && wps_on_central_change) wps_on_central_change(r->folder, r->path + 5, 'c');
            struct req fake = { .c = r->c, .id = r->id };
            reply_ok_object(&fake, o, NULL);
        }
    }
    wps_loaded_free(&r->ld);
    client_unref(r->c);
    free(r);
}

static void start_create(struct client *c, int64_t id, const char *folder, const char *cls, const char *title,
                         const char *command)
{
    struct create_req *r = calloc(1, sizeof *r);
    r->c = c; c->refs++;
    r->id = id;
    snprintf(r->folder, sizeof r->folder, "%s", folder);
    snprintf(r->cls, sizeof r->cls, "%s", cls);
    snprintf(r->title, sizeof r->title, "%s", title ? title : "");
    snprintf(r->command, sizeof r->command, "%s", command ? command : "");
    wpo_defer(loop, "create object", create_work, create_done, r);
}

/* ---- the built-in templates (Folder, Data File, Program), made once in
 * the templates folder as central objects: other programs (GNOME's "New
 * Document") do not see them. Not made again once the user deletes them. */
static void builtin_templates_work(void *arg)
{
    (void)arg;
    const char *td = wps_templates_dir();
    if (!td[0]) return;
    if (mkdir(td, 0755) != 0 && errno != EEXIST) return;
    wps_loaded fl;
    wps_load(td, true, &fl);
    if (fl.err || !fl.id[0]) { wps_loaded_free(&fl); return; }
    char loc[WPS_ID_LEN + 16], key[PATH_MAX + 128];
    snprintf(loc, sizeof loc, "templates:%s", fl.id);
    wps_props done;
    wps_props_init(&done);
    int64_t v = 0;
    bool made = wps_db_get_class(db, fl.vol, loc, "WPS", &done) > 0 && wps_props_int(&done, "builtin", &v) && v;
    wps_props_free(&done);
    if (!made) {
        static const char *const builtins[][2] = { { "WPFolder", "Folder" }, { "WPDataFile", "Data File" }, { "WPProgram", "Program" } };
        for (size_t i = 0; i < 3; i++) {
            char id[WPS_ID_LEN + 1], vl[64];
            if (wps_central_create(fl.vol, fl.id, "WPAbstract", builtins[i][1], NULL, id)) continue;
            snprintf(vl, sizeof vl, "vobj:%s", id);
            wps_db_key(key, sizeof key, vl, "WPS", "makes");
            wps_db_put(db, fl.vol, key, 's', builtins[i][0], (uint32_t)strlen(builtins[i][0]));
            unsigned char one[8] = { 1 };
            wps_db_key(key, sizeof key, vl, "WPObject", "template");
            wps_db_put(db, fl.vol, key, 'i', one, 8);
        }
        unsigned char one[8] = { 1 };
        wps_db_key(key, sizeof key, loc, "WPS", "builtin");
        wps_db_put(db, fl.vol, key, 'i', one, 8);
    }
    wps_loaded_free(&fl);
}

/* ---- Drives (v0.2-design 5) */

static void put_drive(cbuf *b, const wps_drive_info *d)
{
    char title[200];
    wps_drive_title(d, title, sizeof title);
    cb_map(b, 13);
    cb_text(b, "uuid"); cb_text(b, d->uuid);
    cb_text(b, "title"); cb_text(b, title);
    cb_text(b, "device"); cb_text(b, d->device);
    cb_text(b, "label"); cb_text(b, d->label);
    cb_text(b, "fstype"); cb_text(b, d->fstype);
    cb_text(b, "size"); cb_uint(b, d->size);
    cb_text(b, "mount"); cb_text(b, d->mount);
    cb_text(b, "kind"); cb_text(b, d->kind);
    cb_text(b, "removable"); cb_bool(b, d->removable);
    cb_text(b, "ejectable"); cb_bool(b, d->ejectable);
    cb_text(b, "can_poweroff"); cb_bool(b, d->can_poweroff);
    cb_text(b, "encrypted"); cb_bool(b, d->encrypted);
    cb_text(b, "unlocked"); cb_bool(b, d->unlocked);
}

static void drives_changed(void);

/* A drive's action through udisks2; for one that unmounts, the volume is
 * let go of first (wpsd's database on it closed), and taken up again if
 * the unmount fails. */
struct drive_call { char uuid[80], action[16], mount[PATH_MAX], *pass; wps_udisks_fn done; void *arg; };

static void drive_call_done(void *arg, int err, const char *msg, const char *mount)
{
    struct drive_call *k = arg;
    if (err) { volume_unrelease_all(); start_volume_job(NULL); }
    k->done(k->arg, err, msg, mount);
    if (k->pass) { explicit_bzero(k->pass, strlen(k->pass)); free(k->pass); }
    free(k);
}

static void drive_release_work(void *arg) { volume_release(((struct drive_call *)arg)->mount); }

static void drive_release_done(void *arg)
{
    struct drive_call *k = arg;
    wps_udisks_call(k->uuid, k->action, k->pass, drive_call_done, k);
}

static void drive_call(const char *uuid, const char *action, const char *pass, wps_udisks_fn done, void *arg)
{
    wps_drive_info d;
    bool unmounts = strcmp(action, "mount") && strcmp(action, "unlock");
    if (!unmounts || !wps_udisks_find(uuid, &d) || !d.mount[0]) { wps_udisks_call(uuid, action, pass, done, arg); return; }
    struct drive_call *k = calloc(1, sizeof *k);
    snprintf(k->uuid, sizeof k->uuid, "%s", uuid);
    snprintf(k->action, sizeof k->action, "%s", action);
    snprintf(k->mount, sizeof k->mount, "%s", d.mount);
    k->pass = pass ? strdup(pass) : NULL;
    k->done = done;
    k->arg = arg;
    wpo_defer(loop, "release volume", drive_release_work, drive_release_done, k);
}

/* A drive's action, answered when udisks2 is done: {mount} or an error
 * whose code says why ("not authorized", "busy", ...). */

static void drive_done(void *arg, int err, const char *msg, const char *mount)
{
    struct drive_req *q = arg;
    if (!q->c->dead) {
        if (err) {
            const char *code = err == EACCES ? "not authorized" : err == EBUSY ? "busy" : err == ENOTSUP ? "no udisks" :
                               err == ENODEV ? "not found" : err == EPERM ? "locked" : "failed";
            reply_error(q->c, q->id, code, msg && *msg ? msg : strerror(err));
        } else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_int(&b, q->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result");
            if (q->open) {   /* opened: its top folder, as a folder opens */
                cb_map(&b, 1);
                cb_text(&b, "folder"); cb_text(&b, mount ? mount : "/");
            } else cb_text(&b, mount ? mount : "");
            send_frame(q->c, &b);
            cb_free(&b);
        }
    }
    client_unref(q->c);
    if (q->pass) { explicit_bzero(q->pass, strlen(q->pass)); free(q->pass); }
    free(q);
}

/* {op:"drive", target} (a drive object): its UUID, read off the loop. */
static void drive_target_work(void *arg)
{
    struct drive_req *q = arg;
    wps_loaded ld;
    wps_load(q->uuid, false, &ld);   /* (uuid holds the target until here) */
    q->uuid[0] = 0;
    if (!ld.err) {
        const char *loc = ld.locator[0] ? ld.locator : ld.path;
        wps_props ps;
        wps_props_init(&ps);
        char *u = NULL;
        if (!strcmp(ld.obj_class, "WPDrive") && wps_db_get_class(db, ld.vol, loc, "WPDrive", &ps) > 0 && wps_props_str(&ps, "uuid", &u))
            snprintf(q->uuid, sizeof q->uuid, "%s", u);
        else if (ld.shadow_of[0]) {   /* a shadow of a drive */
            char tl[WPS_ID_LEN + 8];
            snprintf(tl, sizeof tl, "id:%s", ld.shadow_of);
            wps_loaded tg;
            wps_load(tl, false, &tg);
            wps_props_free(&ps);
            wps_props_init(&ps);
            if (!tg.err && !strcmp(tg.obj_class, "WPDrive") &&
                wps_db_get_class(db, tg.vol, tg.locator[0] ? tg.locator : tg.path, "WPDrive", &ps) > 0 && wps_props_str(&ps, "uuid", &u))
                snprintf(q->uuid, sizeof q->uuid, "%s", u);
            wps_loaded_free(&tg);
        }
        free(u);
        wps_props_free(&ps);
    }
    wps_loaded_free(&ld);
}

static void drive_target_done(void *arg)
{
    struct drive_req *q = arg;
    if (!q->uuid[0]) { drive_done(q, ENODEV, "not a drive", NULL); return; }
    drive_call(q->uuid, q->action, q->pass, drive_done, q);
}

/* For WPDrive's menu and notebook (classes/wps_drive_hook.h). */
static void human_size(uint64_t v, char *out, size_t n)
{
    const char *u[] = { "bytes", "kB", "MB", "GB", "TB", "PB" };
    double s = (double)v;
    int i = 0;
    while (s >= 1000 && i < 5) { s /= 1000; i++; }
    if (i && s < 10) snprintf(out, n, "%.1f %s", s, u[i]);
    else snprintf(out, n, "%.0f %s", s, u[i]);
}

static bool drive_query(const char *uuid, wps_drive_state *st, bool details)
{
    wps_drive_info d;
    if (!wps_udisks_find(uuid, &d)) return false;
    st->present = true;
    st->mounted = d.mount[0] != 0;
    st->ejectable = d.ejectable;
    st->can_poweroff = d.can_poweroff;
    st->encrypted = d.encrypted;
    st->unlocked = d.unlocked;
    st->root = !strcmp(d.kind, "root");
    snprintf(st->kind, sizeof st->kind, "%s", d.kind);
    snprintf(st->device, sizeof st->device, "%s", d.device);
    snprintf(st->fstype, sizeof st->fstype, "%s", d.fstype);
    snprintf(st->mount, sizeof st->mount, "%s", d.mount);
    human_size(d.size, st->size, sizeof st->size);
    struct statvfs sv;
    if (details && d.mount[0] && statvfs(d.mount, &sv) == 0) human_size((uint64_t)sv.f_bavail * sv.f_frsize, st->free, sizeof st->free);
    return true;
}

/* ---- the Trash Can: putting an item back (v0.2-design 4) */
struct restore_job { struct client *c; int64_t id; char path[PATH_MAX], out[PATH_MAX], from[PATH_MAX]; char new_id[WPS_ID_LEN + 1]; int err; };

static void restore_work(void *arg)
{
    struct restore_job *j = arg;
    if (!strncmp(j->path, "vobj:", 5)) {
        /* a central object (a shadow): back into the folder it came from */
        wps_loaded ld;
        wps_load(j->path, false, &ld);
        wps_props ps;
        wps_props_init(&ps);
        char *origin = NULL;
        if (ld.err || !ld.central) j->err = ENOENT;
        else if (wps_db_get_class(db, ld.vol, ld.locator, "WPS", &ps) <= 0 || !wps_props_str(&ps, "origin", &origin))
            j->err = ENOENT;
        else {
            char fid[WPS_ID_LEN + 8];
            snprintf(fid, sizeof fid, "id:%s", origin);
            wps_loaded of, tf;
            wps_load(fid, false, &of);
            char tfid[WPS_ID_LEN + 8];
            snprintf(tfid, sizeof tfid, "id:%s", ld.folder_id);
            wps_load(tfid, false, &tf);
            if (of.err || !S_ISDIR(of.st.st_mode)) j->err = ENOENT;   /* its folder is gone */
            else {
                j->err = wps_central_move(ld.id, of.vol, of.id, false, j->new_id);
                if (!j->err) {
                    char key[PATH_MAX + 128];
                    wps_db_key(key, sizeof key, ld.locator, "WPS", "origin");
                    wps_db_delete(db, of.vol, key);
                    snprintf(j->out, sizeof j->out, "%s", of.path);
                    if (!tf.err) snprintf(j->from, sizeof j->from, "%s", tf.path);
                }
            }
            wps_loaded_free(&of);
            wps_loaded_free(&tf);
        }
        free(origin);
        wps_props_free(&ps);
        wps_loaded_free(&ld);
        return;
    }
    j->err = j->path[0] == '/' ? -wps_trash_restore(j->path, j->out, sizeof j->out) : EINVAL;
}

static void restore_done(void *arg)
{
    struct restore_job *j = arg;
    if (!j->err && !strncmp(j->path, "vobj:", 5)) {
        wps_central_forget(j->path + 5);
        if (j->from[0]) on_central_change(j->from, j->path + 5, 'd');
        on_central_change(j->out, j->new_id, 'c');
    }
    if (!j->c->dead) {
        if (j->err) reply_error(j->c, j->id, "failed", strerror(j->err));
        else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_int(&b, j->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result"); cb_text(&b, j->out);
            send_frame(j->c, &b);
            cb_free(&b);
        }
    }
    client_unref(j->c);
    free(j);
}

/* ---- Empty trash: its central objects (trashed shadows) deleted, then a
 * delete job for its files */
struct empty_job { struct client *c; int64_t id; char files[PATH_MAX]; char (*gone)[WPS_ID_LEN + 1]; size_t ngone; };

static void empty_collect(const char *vol, const char *vid, void *user)
{
    (void)vol;
    struct empty_job *j = user;
    j->gone = realloc(j->gone, (j->ngone + 1) * sizeof *j->gone);
    snprintf(j->gone[j->ngone++], sizeof j->gone[0], "%s", vid);
}

static void empty_work(void *arg)
{
    struct empty_job *j = arg;
    wps_loaded fl;
    wps_load(j->files, false, &fl);
    if (!fl.err && fl.id[0]) wps_central_in(fl.vol, fl.id, empty_collect, j);
    wps_loaded_free(&fl);
    for (size_t k = 0; k < j->ngone; k++) wps_central_delete(j->gone[k]);
}

static void empty_done(void *arg)
{
    struct empty_job *j = arg;
    for (size_t k = 0; k < j->ngone; k++) {
        wps_central_forget(j->gone[k]);
        on_central_change(j->files, j->gone[k], 'd');
    }
    char errm[256] = "";
    bool any_file = false;
    DIR *d = opendir(j->files);
    for (struct dirent *e; d && (e = readdir(d)) && !any_file;) any_file = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    if (d) closedir(d);
    int64_t job = 0;
    if (any_file) {
        int64_t sel = wps_select(j->files, true, NULL, 0);
        job = sel < 0 ? -1 : wps_job_start("delete", sel, NULL, errm, sizeof errm);
    }
    if (!j->c->dead) {
        if (job < 0) reply_error(j->c, j->id, "failed", errm[0] ? errm : "the trash cannot be read");
        else reply_int(j->c, j->id, job);   /* (0: no files, nothing more to do) */
    }
    client_unref(j->c);
    free(j->gone);
    free(j);
}

/* ---- the desktop's first start (v0.2-design 2): what a new desktop gets,
 * in versioned steps, each done once for the user (<config>/desktop-made:
 * "version N"; 0.1 wrote it without a version: 1). The desktop asks each
 * time it opens ({op:"first_start", desktop}); a step the user undid (a
 * deleted shadow) is not redone. Steps:
 *   1 (0.1's): a shadow of the Templates folder, one of System Setup;
 *   2 (0.2's): (the System folder, Programs, Trash Can: v0.2 steps 2-5);
 *   7: Templates in System, as on OS/2 (the desktop's shadows of it move
 *      there); the desktop puts the Trash Can in its lower right corner;
 *   8: Restore Desktop in System;
 *   9 (0.3): the home folder in Computer (a shadow);
 *  10 (0.3): titles (the owner's): System "My System - <host name>",
 *      Computer "My Storages" (unless titled already).
 * After it, the default objects in place are noted (restore_defaults). */
#define FIRST_START_VERSION 10
static void setup_dir(char *out, size_t n);
static void system_dir(char *out, size_t n);
static void computer_dirs(char *comp, size_t cn, char *drives, size_t dn);
static void data_dir(const char *name, char *out, size_t n);

struct first_start {
    struct client *c;
    int64_t id;
    char desk[PATH_MAX];
    int from;
    char gone[8][WPS_ID_LEN + 1];   /* the desktop's shadows of System Setup, removed */
    int ngone;
    char viewer[WPS_ID_LEN + 1];    /* the Minimized Window Viewer, made */
    char restorer[WPS_ID_LEN + 1];  /* Restore Desktop, made (step 8) */
    char sys[PATH_MAX], autostart[PATH_MAX];
    wps_loaded asld;                /* the autostart folder (titled Startup) */
    wps_loaded sysld, compld;       /* step 10: System and Computer, titled */
    char trash[PATH_MAX];           /* the Trash Can: the home trash's files folder */
    char programs[PATH_MAX];        /* Programs (step 6) */
    char tpl_moved[8][WPS_ID_LEN + 1];   /* step 7: the desktop's Templates shadows */
    char tpl_new[8][WPS_ID_LEN + 1];     /* each one's ID in System ("": deleted, one was there) */
    int ntpl;
    bool tpl_in_system;             /* System has one already */
    bool scan_sys;
    wps_loaded tld;
};

static int first_start_done_version(char *marker, size_t n)
{
    snprintf(marker, n, "%s/desktop-made", env.config_dir);
    FILE *f = fopen(marker, "r");
    if (!f) return 0;
    char line[128];
    int v = 1;   /* 0.1's marker: no version */
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "version ", 8)) v = atoi(line + 8);
    fclose(f);
    return v;
}

static void first_start_shadow_done(void *arg, const char *link, const char *error)
{
    (void)arg;
    if (!link && error) log_msg("first start: a shadow was not made: %s", error);
}

/* the desktop's shadows of System Setup (0.1 put one there) */
static void find_setup_shadow(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct first_start *j = user;
    char loc[WPS_ID_LEN + 8];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_loaded ld;
    wps_load(loc, false, &ld);
    char sd[PATH_MAX];
    setup_dir(sd, sizeof sd);
    wps_loaded sl;
    wps_load(sd, false, &sl);
    if (!ld.err && !sl.err && sl.id[0] && !strcmp(ld.shadow_of, sl.id) && j->ngone < 8)
        snprintf(j->gone[j->ngone++], sizeof j->gone[0], "%s", id);
    wps_loaded_free(&sl);
    wps_loaded_free(&ld);
}

/* shadows of the Templates folder (on the desktop: to move; in System:
 * there already) */
static void find_tpl_shadow(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct first_start *j = user;
    char loc[WPS_ID_LEN + 8];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_loaded ld, tl;
    wps_load(loc, false, &ld);
    wps_load(wps_templates_dir(), false, &tl);
    if (!ld.err && !tl.err && tl.id[0] && !strcmp(ld.shadow_of, tl.id)) {
        if (j->scan_sys) j->tpl_in_system = true;
        else if (j->ntpl < 8) snprintf(j->tpl_moved[j->ntpl++], sizeof j->tpl_moved[0], "%s", id);
    }
    wps_loaded_free(&tl);
    wps_loaded_free(&ld);
}

static void first_start_work(void *arg)
{
    struct first_start *j = arg;
    char marker[PATH_MAX + 32];
    j->from = first_start_done_version(marker, sizeof marker);
    if (j->from >= FIRST_START_VERSION) return;
    wps_mkdirs(env.config_dir, 0700);
    char sd[PATH_MAX];
    setup_dir(sd, sizeof sd);
    wps_mkdirs(sd, 0700);
    const char *td = wps_templates_dir();
    if (td[0]) mkdir(td, 0755);
    system_dir(j->sys, sizeof j->sys);
    wps_mkdirs(j->sys, 0700);
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xc && *xc) snprintf(j->autostart, sizeof j->autostart, "%s/autostart", xc);
    else snprintf(j->autostart, sizeof j->autostart, "%s/.config/autostart", home ? home : "");
    if (j->from < 3) {
        /* step 3 (0.2): System on the desktop in System Setup's place;
         * the Minimized Window Viewer and Startup in System */
        wps_mkdirs(j->autostart, 0755);
        wps_loaded fl;
        wps_load(j->desk, true, &fl);
        if (!fl.err && fl.id[0]) wps_central_in(fl.vol, fl.id, find_setup_shadow, j);
        wps_loaded_free(&fl);
        for (int k = 0; k < j->ngone; k++) wps_central_delete(j->gone[k]);
        wps_loaded sy;
        wps_load(j->sys, true, &sy);
        if (!sy.err && sy.id[0] && wps_central_create(sy.vol, sy.id, "WPMinWinViewer", "Minimized Window Viewer", NULL, j->viewer) != 0)
            j->viewer[0] = 0;
        wps_loaded_free(&sy);
        wps_load(j->autostart, true, &j->asld);
    }
    if (j->from < 4) {
        /* step 4 (0.2): the Trash Can - the home trash's files folder, of
         * class WPTrashCan - on the desktop */
        char home[PATH_MAX], info[PATH_MAX + 8];
        wps_trash_home(home, sizeof home);
        snprintf(j->trash, sizeof j->trash, "%s/files", home);
        snprintf(info, sizeof info, "%s/info", home);
        wps_mkdirs(j->trash, 0700);
        wps_mkdirs(info, 0700);
        if (wps_set_folder_class(j->trash, "WPTrashCan") == 0) {
            /* its title, on it at once (a shadow made next shows it: the
             * object's own saving comes a moment later) */
            wps_props cur, set;
            wps_props_init(&cur);
            wps_props_init(&set);
            if (wps_attr_read(j->trash, "WPObject", &cur) != WPS_A_OK || !wps_props_get(&cur, "title")) {
                wps_props_set_str(&set, "title", "Trash Can");
                wps_attr_update(j->trash, "WPObject", &set, NULL, 2000, NULL, NULL);
            }
            wps_props_free(&cur);
            wps_props_free(&set);
            wps_load(j->trash, true, &j->tld);
        } else j->trash[0] = 0;
    }
    if (j->from < 5) {
        /* step 5 (0.2): Computer, with Drives, in System */
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        wps_mkdirs(drv, 0700);
        wps_set_folder_class(comp, "WPComputer");
        wps_set_folder_class(drv, "WPDrives");
    }
    if (j->from < 6) {
        /* step 6 (0.2): Programs, on the desktop */
        data_dir("Programs", j->programs, sizeof j->programs);
        wps_mkdirs(j->programs, 0700);
        if (wps_set_folder_class(j->programs, "WPProgramsFolder") != 0) j->programs[0] = 0;
    }
    if (j->from < 10) {   /* step 10 (0.3): their titles, set in first_start_done */
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        wps_mkdirs(drv, 0700);   /* (as step 5: there on every desktop since) */
        wps_set_folder_class(comp, "WPComputer");
        wps_load(j->sys, true, &j->sysld);
        wps_load(comp, true, &j->compld);
    }
    if (j->from < 8) {
        /* step 8 (0.2): Restore Desktop in System */
        wps_loaded sy;
        wps_load(j->sys, true, &sy);
        if (sy.err || !sy.id[0] || wps_central_create(sy.vol, sy.id, "WPRestoreDesktop", "Restore Desktop", NULL, j->restorer) != 0)
            j->restorer[0] = 0;
        wps_loaded_free(&sy);
    }
    if (j->from < 7 && j->from >= 1 && wps_templates_dir()[0]) {
        /* step 7 (0.2): the desktop's Templates shadows into System */
        wps_loaded sy, fl;
        wps_load(j->sys, true, &sy);
        wps_load(j->desk, true, &fl);
        if (!sy.err && sy.id[0]) { j->scan_sys = true; wps_central_in(sy.vol, sy.id, find_tpl_shadow, j); j->scan_sys = false; }
        if (!fl.err && fl.id[0]) wps_central_in(fl.vol, fl.id, find_tpl_shadow, j);
        int k = 0;
        for (int i = 0; i < j->ntpl; i++) {
            char nid[WPS_ID_LEN + 1] = "";
            bool moved = !j->tpl_in_system && !sy.err && sy.id[0] && wps_central_move(j->tpl_moved[i], sy.vol, sy.id, false, nid) == 0;
            if (moved) j->tpl_in_system = true;
            else if (wps_central_delete(j->tpl_moved[i]) != 0) continue;   /* (one in System is enough) */
            else nid[0] = 0;
            if (k != i) memcpy(j->tpl_moved[k], j->tpl_moved[i], sizeof j->tpl_moved[0]);
            snprintf(j->tpl_new[k++], sizeof j->tpl_new[0], "%s", nid);
        }
        j->ntpl = k;
        wps_loaded_free(&sy);
        wps_loaded_free(&fl);
    }
}

static void defaults_start(struct client *c, int64_t id, const char *desk, bool restore);

/* My System - <host name> and My Storages, unless they have titles (the
 * user's, or these): at step 10, and again for folders made anew (gone
 * from outside, put back by Restore Desktop or wpsd's start). */
static void default_titles(wps_loaded *sysld, wps_loaded *compld)
{
    char host[256] = "", t[300];
    if (gethostname(host, sizeof host - 1) != 0 || !host[0]) snprintf(host, sizeof host, "this computer");
    snprintf(t, sizeof t, "My System - %s", host);
    wpo_object *so = sysld->err ? NULL : wps_object_from_loaded(sysld);
    if (so && wpo_is_a(so, WPObjectClass) && !WPObject_data(so)->title) WPObject_wpSetTitle(so, t);
    wpo_object *co = compld->err ? NULL : wps_object_from_loaded(compld);
    if (co && wpo_is_a(co, WPObjectClass) && !WPObject_data(co)->title) WPObject_wpSetTitle(co, "My Storages");
}

static void first_start_done(void *arg)
{
    struct first_start *j = arg;
    if (j->from < FIRST_START_VERSION && j->desk[0]) {
        char marker[PATH_MAX + 32], legacy[PATH_MAX + 32];
        snprintf(marker, sizeof marker, "%s/desktop-made", env.config_dir);
        snprintf(legacy, sizeof legacy, "%s/desktop-setup-made", env.config_dir);
        if (j->from < 1) {   /* step 1: a new desktop: Templates, in System since step 7 */
            const char *td = wps_templates_dir();
            if (td[0]) wps_shadow_create(td, j->sys, first_start_shadow_done, NULL);
        }
        /* step 2 (0.2's first): nothing */
        if (j->from < 3) {
            for (int k = 0; k < j->ngone; k++) {
                wps_central_forget(j->gone[k]);
                on_central_change(j->desk, j->gone[k], 'd');
            }
            wps_shadow_create(j->sys, j->desk, first_start_shadow_done, NULL);
            if (j->viewer[0]) on_central_change(j->sys, j->viewer, 'c');
            /* Startup: the autostart folder, titled so, shown in System */
            wpo_object *as = j->asld.err ? NULL : wps_object_from_loaded(&j->asld);
            if (as && wpo_is_a(as, WPObjectClass) && !WPObject_data(as)->title) WPObject_wpSetTitle(as, "Startup");
            wps_shadow_create(j->autostart, j->sys, first_start_shadow_done, NULL);
        }
        if (j->from < 4 && j->trash[0]) {
            sobj *old = wps_sobj_by_path(j->trash);   /* (loaded as a plain folder before) */
            if (old) wps_sobj_forget(old);
            wpo_object *tc = j->tld.err ? NULL : wps_object_from_loaded(&j->tld);
            if (tc && wpo_is_a(tc, WPObjectClass) && !WPObject_data(tc)->title) WPObject_wpSetTitle(tc, "Trash Can");
            wps_shadow_create(j->trash, j->desk, first_start_shadow_done, NULL);
        }
        if (j->from < 5) {
            char comp[PATH_MAX], drv[PATH_MAX + 8];
            computer_dirs(comp, sizeof comp, drv, sizeof drv);
            for (int k = 0; k < 2; k++) {   /* (seen as plain folders before) */
                sobj *old = wps_sobj_by_path(k ? drv : comp);
                if (old) wps_sobj_forget(old);
            }
            drives_changed();
        }
        if (j->from < 6 && j->programs[0]) {
            sobj *old = wps_sobj_by_path(j->programs);
            if (old) wps_sobj_forget(old);
            wps_shadow_create(j->programs, j->desk, first_start_shadow_done, NULL);
            wps_programs_sync();   /* filled now */
        }
        if (j->from < 7 && j->from >= 1) {
            for (int i = 0; i < j->ntpl; i++) {
                wps_central_forget(j->tpl_moved[i]);
                on_central_change(j->desk, j->tpl_moved[i], 'd');
                if (j->tpl_new[i][0]) on_central_change(j->sys, j->tpl_new[i], 'c');
            }
            const char *td = wps_templates_dir();
            if (!j->tpl_in_system && !j->ntpl && td[0]) wps_shadow_create(td, j->sys, first_start_shadow_done, NULL);
        }
        if (j->restorer[0]) on_central_change(j->sys, j->restorer, 'c');
        if (j->from < 10) default_titles(&j->sysld, &j->compld);   /* step 10 */
        const char *home = getenv("HOME");
        if (j->from < 9 && home && *home) {   /* step 9: the home folder in Computer */
            char comp[PATH_MAX], drv[PATH_MAX + 8];
            computer_dirs(comp, sizeof comp, drv, sizeof drv);
            wps_mkdirs(comp, 0700);
            wps_shadow_create(home, comp, first_start_shadow_done, NULL);
        }
        FILE *f = fopen(marker, "w");
        if (f) {
            fprintf(f, "version %d\n# The desktop's first-start steps done (wpsd); 0.1 wrote no version.\n", FIRST_START_VERSION);
            fclose(f);
        }
        unlink(legacy);
        log_msg("first start: the desktop %s brought from step %d to %d", j->desk, j->from, FIRST_START_VERSION);
    }
    if (j->desk[0]) defaults_start(NULL, 0, j->desk, false);   /* the defaults in place, noted */
    wps_loaded_free(&j->asld);
    wps_loaded_free(&j->sysld);
    wps_loaded_free(&j->compld);
    wps_loaded_free(&j->tld);
    if (!j->c->dead) reply_int(j->c, j->id, j->from);
    client_unref(j->c);
    free(j);
}

/* ---- settings objects (System, Sound, Mouse, Scheme Palette): one of each
 * per user, central objects in the System Setup folder
 * ($XDG_DATA_HOME/wps/System Setup; WPS_SETUP_DIR for tests), made once and
 * remembered by class (locator "setup:<folder ID>", class WPS, prop
 * <class> = object ID). */
static const char *const setup_classes[][2] = {
    { "WPSystem", "System" }, { "WPSound", "Sound" }, { "WPMouse", "Mouse" }, { "WPSchemePalette", "Scheme Palette" },
    /* (0.2: in the System folder itself, v0.2-design 9 and 10) */
    { "WPLaunchPad", "LaunchPad" }, { "WPWarpCenter", "WarpCenter" }, { "WPMessages", "Messages" },
};
#define NSETUP 7
#define SETUP_IN_SYSTEM 4   /* setup_classes from this one on */

/* WPS's own folders in the user's data ($XDG_DATA_HOME/wps/<name>). */
static void data_dir(const char *name, char *out, size_t n)
{
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (dh && *dh) snprintf(out, n, "%s/wps%s%s", dh, name[0] ? "/" : "", name);
    else snprintf(out, n, "%s/.local/share/wps%s%s", home ? home : "", name[0] ? "/" : "", name);
}

/* The System folder (OS/2's "OS/2 System", v0.2-design 7). */
static void system_dir(char *out, size_t n)
{
    const char *e = getenv("WPS_SYSTEM_DIR");
    if (e && *e) snprintf(out, n, "%s", e);
    else data_dir("System", out, n);
}

/* System Setup: in the System folder since 0.2 (0.1 had it beside it). */
static void setup_dir(char *out, size_t n)
{
    const char *e = getenv("WPS_SETUP_DIR");
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    char sys[PATH_MAX], old[PATH_MAX];
    system_dir(sys, sizeof sys);
    snprintf(out, n, "%s/System Setup", sys);
    /* (if 0.1's could not be moved: it is still where it was) */
    data_dir("System Setup", old, sizeof old);
    if (access(out, F_OK) != 0 && access(old, F_OK) == 0) snprintf(out, n, "%s", old);
}

/* At start, before tracking: the System folder made, and 0.1's System
 * Setup moved into it (one rename: its ID, settings objects and the
 * shadows of it go with it). */
static void system_folder_init(void)
{
    char sys[PATH_MAX], old[PATH_MAX], now[PATH_MAX];
    system_dir(sys, sizeof sys);
    if (wps_mkdirs(sys, 0700) != 0) { log_msg("cannot make the System folder %s", sys); return; }
    /* 0.3: My Storages' folder is Storages (0.2's Computer), the owner's */
    char was[PATH_MAX + 16], comp[PATH_MAX], drv[PATH_MAX + 8];
    snprintf(was, sizeof was, "%s/Computer", sys);
    computer_dirs(comp, sizeof comp, drv, sizeof drv);
    struct stat cs;
    if (stat(was, &cs) == 0 && S_ISDIR(cs.st_mode) && access(comp, F_OK) != 0) {
        if (rename(was, comp) == 0) {
            wps_shadows_prefix_moved(was, comp);
            log_msg("My Storages: its folder is %s now (was Computer)", comp);
        } else {
            log_msg("cannot rename Computer to Storages: %s", strerror(errno));
        }
    }
    if (getenv("WPS_SETUP_DIR")) return;
    data_dir("System Setup", old, sizeof old);
    snprintf(now, sizeof now, "%s/System Setup", sys);   /* (not setup_dir: it falls back to the old one) */
    struct stat st;
    if (stat(old, &st) == 0 && S_ISDIR(st.st_mode) && access(now, F_OK) != 0) {
        if (rename(old, now) == 0) {
            wps_shadows_prefix_moved(old, now);
            log_msg("System Setup moved into the System folder (%s)", now);
        } else {
            log_msg("cannot move System Setup into the System folder: %s", strerror(errno));
        }
    }
}

static void setup_objects_work(void *arg)
{
    (void)arg;
    char sd[PATH_MAX];
    setup_dir(sd, sizeof sd);
    if (wps_mkdirs(sd, 0700) != 0) return;
    wps_loaded fl;
    wps_load(sd, true, &fl);
    if (fl.err || !fl.id[0]) { wps_loaded_free(&fl); return; }
    wps_props known;
    wps_props_init(&known);
    char sloc[WPS_ID_LEN + 8];
    snprintf(sloc, sizeof sloc, "setup:%s", fl.id);   /* by the folder: one set per System Setup folder */
    wps_db_get_class(db, fl.vol, sloc, "WPS", &known);
    for (size_t i = 0; i < NSETUP; i++) {
        char *id = NULL, vol[WPS_VOLID_MAX];
        if (wps_props_str(&known, setup_classes[i][0], &id) && id && wps_db_vobj_vol(db, id, vol, sizeof vol)) { free(id); continue; }
        free(id);
        char nid[WPS_ID_LEN + 1], key[256];
        if (i >= SETUP_IN_SYSTEM) {
            char sys[PATH_MAX];
            system_dir(sys, sizeof sys);
            wps_loaded sy;
            wps_load(sys, true, &sy);
            int rc = sy.err || !sy.id[0] ? -1 : wps_central_create(sy.vol, sy.id, setup_classes[i][0], setup_classes[i][1], NULL, nid);
            wps_loaded_free(&sy);
            if (rc) continue;
        } else if (wps_central_create(fl.vol, fl.id, setup_classes[i][0], setup_classes[i][1], NULL, nid)) continue;
        wps_db_key(key, sizeof key, sloc, "WPS", setup_classes[i][0]);
        wps_db_put(db, fl.vol, key, 's', nid, WPS_ID_LEN);
    }
    wps_props_free(&known);
    wps_loaded_free(&fl);
}

static void builtin_templates_done(void *arg)
{
    (void)arg;
    wps_listing_invalidate(wps_templates_dir());
    wpo_defer(loop, "settings objects", setup_objects_work, NULL, NULL);
}

/* ---- the default objects (0.2): System, Programs and the Trash Can on the
 * desktop; Templates, Startup, the Minimized Window Viewer and Restore
 * Desktop in System; the settings objects where they belong. Each is
 * noted by ID once it is in place (locator "wps-defaults", class WPS, one
 * prop per object), so {op:"restore_defaults", desktop} can put back the
 * very object the user moved or deleted (a central object moved gets a
 * new ID: noted again), or make one if it is gone for good; missing
 * default folders are made again. Each desktop open notes the ones in
 * place ({op:"first_start"}). */
#define DEFAULTS_LOC "wps-defaults"
#define NDEFAULTS 8

struct defobj {
    const char *name, *title;
    const char *cls;            /* a central object of this class; NULL: a shadow of target */
    char target[PATH_MAX], folder[PATH_MAX];
};

struct defaults_job {
    struct client *c;
    int64_t id;
    bool restore;
    wps_loaded sysld, compld;   /* their titles, if made anew */
    char desk[PATH_MAX];
    struct { char folder[PATH_MAX], id[WPS_ID_LEN + 1], old_folder[PATH_MAX], old_id[WPS_ID_LEN + 1]; } ev[24];
    int nev;
    const char *put_back[32];   /* titles, for the reply */
    int nput;
    bool made_programs, made_trash, made_computer;
    bool no_desktop;            /* none named, none known */
};

static void autostart_dir(char *out, size_t n)
{
    const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xc && *xc) snprintf(out, n, "%s/autostart", xc);
    else snprintf(out, n, "%s/.config/autostart", home ? home : "");
}

static int defaults_list(const char *desk, struct defobj *d)
{
    char sys[PATH_MAX], home[PATH_MAX];
    system_dir(sys, sizeof sys);
    wps_trash_home(home, sizeof home);
    int n = 0;
    d[n] = (struct defobj){ "desktop.system", "My System", NULL, "", "" };
    snprintf(d[n].target, sizeof d[n].target, "%s", sys);
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", desk);
    d[n] = (struct defobj){ "desktop.programs", "Programs", NULL, "", "" };
    data_dir("Programs", d[n].target, sizeof d[n].target);
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", desk);
    d[n] = (struct defobj){ "desktop.trash", "Trash Can", NULL, "", "" };
    snprintf(d[n].target, sizeof d[n].target, "%s/files", home);
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", desk);
    if (wps_templates_dir()[0]) {
        d[n] = (struct defobj){ "system.templates", "Templates", NULL, "", "" };
        snprintf(d[n].target, sizeof d[n].target, "%s", wps_templates_dir());
        snprintf(d[n++].folder, sizeof d[0].folder, "%s", sys);
    }
    d[n] = (struct defobj){ "system.startup", "Startup", NULL, "", "" };
    autostart_dir(d[n].target, sizeof d[n].target);
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", sys);
    d[n] = (struct defobj){ "system.viewer", "Minimized Window Viewer", "WPMinWinViewer", "", "" };
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", sys);
    d[n] = (struct defobj){ "system.restore", "Restore Desktop", "WPRestoreDesktop", "", "" };
    snprintf(d[n++].folder, sizeof d[0].folder, "%s", sys);
    const char *uh = getenv("HOME");
    if (uh && *uh) {
        char drv[PATH_MAX + 8];
        d[n] = (struct defobj){ "computer.home", "Home", NULL, "", "" };
        snprintf(d[n].target, sizeof d[n].target, "%s", uh);
        computer_dirs(d[n].folder, sizeof d[n].folder, drv, sizeof drv);
        n++;
    }
    return n;
}

struct def_match { const char *tid, *cls; char found[WPS_ID_LEN + 1]; };

static void def_match_one(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct def_match *m = user;
    if (m->found[0]) return;
    char loc[WPS_ID_LEN + 8];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_loaded ld;
    wps_load(loc, false, &ld);
    if (!ld.err && (m->cls ? !strcmp(ld.obj_class, m->cls) : m->tid && !strcmp(ld.shadow_of, m->tid)))
        snprintf(m->found, sizeof m->found, "%s", id);
    wps_loaded_free(&ld);
}

/* One like it (a shadow of tid, or of class cls) among folder's central objects. */
static bool def_find_in(const wps_loaded *folder, const char *tid, const char *cls, char out[WPS_ID_LEN + 1])
{
    if (folder->err || !folder->id[0]) return false;
    struct def_match m = { tid, cls, "" };
    wps_central_in(folder->vol, folder->id, def_match_one, &m);
    if (!m.found[0]) return false;
    memcpy(out, m.found, WPS_ID_LEN + 1);
    return true;
}

static void def_note(const char *vol, const char *name, const char *id)
{
    char key[256];
    wps_db_key(key, sizeof key, DEFAULTS_LOC, "WPS", name);
    wps_db_put(db, vol, key, 's', id, WPS_ID_LEN);
}

static void def_event(struct defaults_job *j, const char *folder, const char *id, const char *old_folder, const char *old_id)
{
    if (j->nev >= 24) return;
    snprintf(j->ev[j->nev].folder, sizeof j->ev[0].folder, "%s", folder);
    snprintf(j->ev[j->nev].id, sizeof j->ev[0].id, "%s", id);
    snprintf(j->ev[j->nev].old_folder, sizeof j->ev[0].old_folder, "%s", old_folder ? old_folder : "");
    snprintf(j->ev[j->nev].old_id, sizeof j->ev[0].old_id, "%s", old_id ? old_id : "");
    j->nev++;
}

/* Move central object `id` into folder `to` (loaded); its old folder's
 * path for the event. */
static bool def_move(struct defaults_job *j, const char *id, const wps_loaded *to, const char *to_path, char nid[WPS_ID_LEN + 1])
{
    char loc[WPS_ID_LEN + 8], from[PATH_MAX] = "";
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_loaded ld, fl;
    wps_load(loc, false, &ld);
    if (ld.err) { wps_loaded_free(&ld); return false; }
    snprintf(loc, sizeof loc, "id:%s", ld.folder_id);
    wps_load(loc, false, &fl);
    if (!fl.err) snprintf(from, sizeof from, "%s", fl.path);
    wps_loaded_free(&fl);
    wps_loaded_free(&ld);
    if (wps_central_move(id, to->vol, to->id, false, nid) != 0) return false;
    def_event(j, to_path, nid, from, id);
    return true;
}

static bool def_made_dir(const char *path)
{
    if (access(path, F_OK) == 0) return false;
    return wps_mkdirs(path, 0700) == 0;
}

static void defaults_work(void *arg)
{
    struct defaults_job *j = arg;
    char sys[PATH_MAX], sd[PATH_MAX], home[PATH_MAX], tmp[PATH_MAX + 16];
    system_dir(sys, sizeof sys);
    setup_dir(sd, sizeof sd);
    wps_trash_home(home, sizeof home);
    if (j->restore) {
        /* the default folders, if gone (they come back empty, of their class) */
        if (def_made_dir(sys)) j->put_back[j->nput++] = "the System folder";
        if (def_made_dir(sd)) j->put_back[j->nput++] = "System Setup";
        autostart_dir(tmp, sizeof tmp);
        if (def_made_dir(tmp)) j->put_back[j->nput++] = "the Startup folder";
        if (wps_templates_dir()[0] && def_made_dir(wps_templates_dir())) j->put_back[j->nput++] = "the Templates folder";
        data_dir("Programs", tmp, sizeof tmp);
        if (def_made_dir(tmp)) { j->made_programs = true; j->put_back[j->nput++] = "the Programs folder"; }
        wps_set_folder_class(tmp, "WPProgramsFolder");
        snprintf(tmp, sizeof tmp, "%s/files", home);
        if (def_made_dir(tmp)) { j->made_trash = true; j->put_back[j->nput++] = "the Trash Can"; }
        wps_set_folder_class(tmp, "WPTrashCan");
        snprintf(tmp, sizeof tmp, "%s/info", home);
        wps_mkdirs(tmp, 0700);
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        if (def_made_dir(drv)) { j->made_computer = true; j->put_back[j->nput++] = "My Storages"; }
        wps_set_folder_class(comp, "WPComputer");
        wps_set_folder_class(drv, "WPDrives");
    }
    {   /* (titled in defaults_done, if made anew) */
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        wps_load(sys, true, &j->sysld);
        wps_load(comp, true, &j->compld);
    }
    wps_loaded sy;
    wps_load(sys, true, &sy);
    if (sy.err || !sy.id[0]) { wps_loaded_free(&sy); return; }
    wps_props known;
    wps_props_init(&known);
    wps_db_get_class(db, sy.vol, DEFAULTS_LOC, "WPS", &known);
    char key[256], *dk = NULL;
    wps_db_key(key, sizeof key, DEFAULTS_LOC, "WPS", "desktop");
    if (!j->desk[0] && wps_props_str(&known, "desktop", &dk) && dk) snprintf(j->desk, sizeof j->desk, "%s", dk);
    else if (j->desk[0] && (!wps_props_str(&known, "desktop", &dk) || !dk || strcmp(dk, j->desk)))
        wps_db_put(db, sy.vol, key, 's', j->desk, (uint32_t)strlen(j->desk));   /* (wpsctl restore-defaults) */
    free(dk);
    if (!j->desk[0]) { j->no_desktop = true; wps_props_free(&known); wps_loaded_free(&sy); return; }
    char trash_files[PATH_MAX + 8];
    snprintf(trash_files, sizeof trash_files, "%s/files", home);
    wps_loaded tr;
    wps_load(trash_files, false, &tr);
    struct defobj d[NDEFAULTS];
    int n = defaults_list(j->desk, d);
    for (int i = 0; i < n; i++) {
        wps_loaded fl, tl;
        wps_load(d[i].folder, true, &fl);
        if (fl.err || !fl.id[0] || !S_ISDIR(fl.st.st_mode)) { wps_loaded_free(&fl); continue; }
        char tid[WPS_ID_LEN + 1] = "";
        if (!d[i].cls) {
            wps_load(d[i].target, true, &tl);
            if (!tl.err) snprintf(tid, sizeof tid, "%s", tl.shadow_of[0] ? tl.shadow_of : tl.id);
            wps_loaded_free(&tl);
            if (!tid[0]) { wps_loaded_free(&fl); continue; }
        }
        char *rec = NULL, nid[WPS_ID_LEN + 1] = "";
        wps_props_str(&known, d[i].name, &rec);
        bool done = false;
        if (rec && strlen(rec) == WPS_ID_LEN) {
            /* the one noted: in place, or put back */
            char loc[WPS_ID_LEN + 8];
            snprintf(loc, sizeof loc, "vobj:%s", rec);
            wps_loaded ld;
            wps_load(loc, false, &ld);
            if (!ld.err && ld.central) {
                done = true;
                if (strcmp(ld.folder_id, fl.id) && j->restore && def_move(j, rec, &fl, d[i].folder, nid)) {
                    def_note(sy.vol, d[i].name, nid);
                    j->put_back[j->nput++] = d[i].title;
                }
            }
            wps_loaded_free(&ld);
        }
        if (!done && def_find_in(&fl, tid, d[i].cls, nid)) {   /* one is there: noted from now on */
            def_note(sy.vol, d[i].name, nid);
            done = true;
        }
        if (!done && j->restore) {
            char was[WPS_ID_LEN + 1];
            if (def_find_in(&tr, tid, d[i].cls, was) && def_move(j, was, &fl, d[i].folder, nid)) {   /* from the Trash Can */
                def_note(sy.vol, d[i].name, nid);
            } else if (wps_central_create(fl.vol, fl.id, d[i].cls ? d[i].cls : "WPShadow", d[i].cls ? d[i].title : NULL,
                                          d[i].cls ? NULL : tid, nid) == 0) {
                def_note(sy.vol, d[i].name, nid);
                def_event(j, d[i].folder, nid, NULL, NULL);
            } else nid[0] = 0;
            if (nid[0]) j->put_back[j->nput++] = d[i].title;
        }
        free(rec);
        wps_loaded_free(&fl);
    }
    wps_props_free(&known);
    if (j->restore) {
        /* the settings objects: each back in its folder; any gone, made again */
        wps_loaded sf;
        wps_load(sd, true, &sf);
        if (!sf.err && sf.id[0]) {
            char sloc[WPS_ID_LEN + 8];
            snprintf(sloc, sizeof sloc, "setup:%s", sf.id);
            wps_props_init(&known);
            wps_db_get_class(db, sf.vol, sloc, "WPS", &known);
            char before[NSETUP][WPS_ID_LEN + 1];
            for (size_t i = 0; i < NSETUP; i++) {
                char *id = NULL, nid[WPS_ID_LEN + 1];
                before[i][0] = 0;
                if (!wps_props_str(&known, setup_classes[i][0], &id) || !id || strlen(id) != WPS_ID_LEN) { free(id); continue; }
                snprintf(before[i], sizeof before[i], "%s", id);
                char loc[WPS_ID_LEN + 8];
                snprintf(loc, sizeof loc, "vobj:%s", id);
                wps_loaded ld;
                wps_load(loc, false, &ld);
                const wps_loaded *home_f = i >= SETUP_IN_SYSTEM ? &sy : &sf;
                if (!ld.err && strcmp(ld.folder_id, home_f->id) &&
                    def_move(j, id, home_f, i >= SETUP_IN_SYSTEM ? sys : sd, nid)) {
                    char key[256];
                    wps_db_key(key, sizeof key, sloc, "WPS", setup_classes[i][0]);
                    wps_db_put(db, sf.vol, key, 's', nid, WPS_ID_LEN);
                    snprintf(before[i], sizeof before[i], "%s", nid);
                    j->put_back[j->nput++] = setup_classes[i][1];
                }
                wps_loaded_free(&ld);
                free(id);
            }
            wps_props_free(&known);
            setup_objects_work(NULL);   /* makes the ones gone */
            wps_props_init(&known);
            wps_db_get_class(db, sf.vol, sloc, "WPS", &known);
            for (size_t i = 0; i < NSETUP; i++) {
                char *id = NULL;
                if (wps_props_str(&known, setup_classes[i][0], &id) && id && strlen(id) == WPS_ID_LEN && strcmp(id, before[i])) {
                    def_event(j, i >= SETUP_IN_SYSTEM ? sys : sd, id, NULL, NULL);
                    j->put_back[j->nput++] = setup_classes[i][1];
                }
                free(id);
            }
            wps_props_free(&known);
        }
        wps_loaded_free(&sf);
    }
    wps_loaded_free(&tr);
    wps_loaded_free(&sy);
}

static void defaults_done(void *arg)
{
    struct defaults_job *j = arg;
    for (int i = 0; i < j->nev; i++) {
        if (j->ev[i].old_id[0]) {
            wps_central_forget(j->ev[i].old_id);
            if (j->ev[i].old_folder[0]) on_central_change(j->ev[i].old_folder, j->ev[i].old_id, 'd');
        }
        on_central_change(j->ev[i].folder, j->ev[i].id, 'c');
    }
    char home[PATH_MAX], tmp[PATH_MAX + 16];
    if (j->made_programs) {
        data_dir("Programs", tmp, sizeof tmp);
        sobj *old = wps_sobj_by_path(tmp);
        if (old) wps_sobj_forget(old);
        wps_programs_sync();
    }
    if (j->made_trash) {
        wps_trash_home(home, sizeof home);
        snprintf(tmp, sizeof tmp, "%s/files", home);
        sobj *old = wps_sobj_by_path(tmp);
        if (old) wps_sobj_forget(old);
    }
    if (j->made_computer) drives_changed();
    if (j->restore && j->no_desktop) {
        if (j->c && !j->c->dead) reply_error(j->c, j->id, "no desktop", "the desktop has not opened yet: name its folder");
    } else if (j->restore) {
        if (j->nput) log_msg("restore defaults: %d put back", j->nput);
        if (j->c && !j->c->dead) {
            cbuf val;
            cb_init(&val);
            cb_map(&val, 1);
            cb_text(&val, "restored");
            cb_array(&val, (size_t)j->nput);
            for (int i = 0; i < j->nput; i++) cb_text(&val, j->put_back[i]);
            reply_value_cbor(j->c, j->id, &val);
            cb_free(&val);
        }
    }
    default_titles(&j->sysld, &j->compld);
    wps_loaded_free(&j->sysld);
    wps_loaded_free(&j->compld);
    if (j->c) client_unref(j->c);
    free(j);
}

static void defaults_start(struct client *c, int64_t id, const char *desk, bool restore)
{
    struct defaults_job *j = calloc(1, sizeof *j);
    if (c) { j->c = c; c->refs++; }
    j->id = id;
    j->restore = restore;
    snprintf(j->desk, sizeof j->desk, "%s", desk);
    wpo_defer(loop, restore ? "restore defaults" : "note defaults", defaults_work, defaults_done, j);
}

/* {op:"setup"} -> {folder, objects:{<class>:{address, settings:{key:value}}}} */
struct setup_req {
    struct client *c;
    int64_t id;
    char folder[PATH_MAX];
    wps_loaded ld[NSETUP];
};

static void setup_req_work(void *arg)
{
    struct setup_req *r = arg;
    setup_dir(r->folder, sizeof r->folder);
    wps_loaded fl;
    wps_load(r->folder, false, &fl);
    wps_props known;
    wps_props_init(&known);
    char sloc[WPS_ID_LEN + 8];
    snprintf(sloc, sizeof sloc, "setup:%s", fl.id);
    if (!fl.err && fl.id[0]) wps_db_get_class(db, fl.vol, sloc, "WPS", &known);
    for (size_t i = 0; i < NSETUP; i++) {
        char *id = NULL, loc[WPS_ID_LEN + 8];
        r->ld[i].err = ENOENT;
        if (wps_props_str(&known, setup_classes[i][0], &id) && id) {
            snprintf(loc, sizeof loc, "vobj:%s", id);
            wps_load(loc, false, &r->ld[i]);
        }
        free(id);
    }
    wps_props_free(&known);
    wps_loaded_free(&fl);
}

static void setup_req_done(void *arg)
{
    struct setup_req *r = arg;
    if (!r->c->dead) {
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, r->id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result");
        cb_map(&b, 2);
        cb_text(&b, "folder"); cb_text(&b, r->folder);
        size_t n = 0;
        wpo_object *objs[NSETUP] = { 0 };
        for (size_t i = 0; i < NSETUP; i++)
            if (!r->ld[i].err && (objs[i] = wps_object_from_loaded(&r->ld[i]))) n++;
        cb_text(&b, "objects");
        cb_map(&b, n);
        for (size_t i = 0; i < NSETUP; i++) {
            if (!objs[i]) continue;
            char *text = NULL;
            wpo_value rv;
            if (wpo_send(objs[i], "wpQuerySettings", NULL, 0, &rv) == WPO_OK && rv.type == 's') { text = rv.u.s; rv.u.s = NULL; }
            wpo_value_clear(&rv);
            if (!text) text = strdup("");
            size_t nk = 0;
            for (const char *p = text; *p; p++) nk += *p == '\n';
            cb_text(&b, setup_classes[i][0]);
            cb_map(&b, 2);
            char addr[WPS_ID_LEN + 8];
            snprintf(addr, sizeof addr, "vobj:%s", WPObject_data(objs[i])->id);
            cb_text(&b, "address"); cb_text(&b, addr);
            cb_text(&b, "settings");
            cb_map(&b, nk);
            for (char *line = text, *nl; (nl = strchr(line, '\n')); line = nl + 1) {
                *nl = 0;   /* key TAB value */
                char *tab = strchr(line, '\t');
                if (tab) *tab = 0;
                cb_text(&b, line);
                cb_text(&b, tab ? tab + 1 : "");
            }
            free(text);
        }
        send_frame(r->c, &b);
        cb_free(&b);
    }
    for (size_t i = 0; i < NSETUP; i++) wps_loaded_free(&r->ld[i]);
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

/* A batch of a search's results, as a "found" event to the client that
 * asked (the search's ID as its sub). */
static void found(void *arg, int64_t find, const wps_found *items, size_t n, bool done, uint64_t scanned, bool truncated,
                  int err)
{
    struct client *c = arg;
    if (c->dead) {
        if (!done) wps_find_cancel(find);   /* nobody reads it any more */
    } else {
        cbuf b;
        event_head(&b, "found", find, 4 + (err != 0));
        cb_text(&b, "items");
        cb_array(&b, n);
        for (size_t i = 0; i < n; i++) {
            cb_map(&b, 4);
            cb_text(&b, "path"); cb_text(&b, items[i].path);
            cb_text(&b, "type"); cb_text(&b, items[i].type);
            cb_text(&b, "size"); cb_int(&b, items[i].size);
            cb_text(&b, "mtime"); cb_int(&b, items[i].mtime);
        }
        cb_text(&b, "done"); cb_bool(&b, done);
        cb_text(&b, "scanned"); cb_uint(&b, scanned);
        cb_text(&b, "truncated"); cb_bool(&b, truncated);
        if (err) { cb_text(&b, "error"); cb_text(&b, strerror(err)); }
        send_frame(c, &b);
        cb_free(&b);
    }
    if (done) client_unref(c);
}

/* ---- Startup (startup.h) */

struct startup_job {
    struct client *c;
    int64_t id;
    char target[PATH_MAX], made[PATH_MAX], dir[PATH_MAX];
    int err;
};

static void startup_work(void *arg)
{
    struct startup_job *j = arg;
    wps_startup_dir(j->dir, sizeof j->dir);
    wps_mkdirs(j->dir, 0755);
    wps_loaded ld;
    wps_load(j->target, false, &ld);
    char path[PATH_MAX] = "";
    if (!ld.err && ld.shadow_of[0]) {   /* a shadow: what it stands for */
        char loc[WPS_ID_LEN + 8];
        snprintf(loc, sizeof loc, "id:%s", ld.shadow_of);
        wps_loaded tl;
        wps_load(loc, false, &tl);
        if (!tl.err && !tl.central) snprintf(path, sizeof path, "%s", tl.path);
        wps_loaded_free(&tl);
    } else if (!ld.err && !ld.central) snprintf(path, sizeof path, "%s", ld.path);
    j->err = ld.err ? ld.err : path[0] ? wps_startup_add(path, j->made, sizeof j->made) : ENOEXEC;
    wps_loaded_free(&ld);
}

static void startup_done(void *arg)
{
    struct startup_job *j = arg;
    if (j->err == ENOEXEC) {   /* not a program: a shadow, opened at login */
        struct simple *s = calloc(1, sizeof *s);
        s->c = j->c; s->id = j->id;   /* (takes the reference) */
        wps_shadow_create(j->target, j->dir, shadow_made, s);
        free(j);
        return;
    }
    if (!j->c->dead) {
        if (j->err) reply_error(j->c, j->id, "failed", strerror(j->err));
        else {
            struct simple *s = calloc(1, sizeof *s);
            s->c = j->c; s->id = j->id; j->c->refs++;
            reply_simple_ok(s, j->made);
        }
    }
    client_unref(j->c);
    free(j);
}

static bool startup_query(wpo_object *o, bool *on)
{
    const char *p = wps_object_path(o);
    if (!p || !wps_startup_is_entry(p)) return false;
    *on = wps_startup_enabled(p);
    return true;
}

static bool startup_set(wpo_object *o, bool on)
{
    const char *p = wps_object_path(o);
    return p && wps_startup_set_enabled(p, on) == 0;
}

static void handle(struct client *c, cv *msg)
{
    int64_t id = 0;
    cv_int(msg, "id", &id);
    const char *op = cv_text(msg, "op");
    if (!op) { reply_error(c, id, "bad request", "missing op"); cv_free(msg); return; }

    if (strcmp(op, "ping") == 0) {
        /* "pong", and what this service is: a GUI newer than its service
         * (during an upgrade, before logging out) can tell */
        cbuf b;
        cb_init(&b);
        cb_map(&b, 5);
        cb_text(&b, "id"); cb_int(&b, id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result"); cb_text(&b, "pong");
        cb_text(&b, "version"); cb_text(&b, WPS_VERSION);
        cb_text(&b, "protocol"); cb_int(&b, WPS_PROTOCOL);
        send_frame(c, &b);
        cb_free(&b);
    } else if (strcmp(op, "describe") == 0) {
        do_describe(c, id, cv_text(msg, "class"));
    } else if (strcmp(op, "apps") == 0) {
        /* Other program... (Open With): every installed application */
        wps_app *a = malloc(1024 * sizeof *a);
        int n = a ? wps_apps_all(a, 1024) : 0;
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result");
        cb_array(&b, (size_t)n);
        for (int i = 0; i < n; i++) {
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_text(&b, a[i].id);
            cb_text(&b, "name"); cb_text(&b, a[i].name);
            cb_text(&b, "icon"); cb_text(&b, a[i].icon);
        }
        send_frame(c, &b);
        cb_free(&b);
        free(a);
    } else if (strcmp(op, "flush") == 0 || strcmp(op, "shutdown") == 0 || strcmp(op, "sync") == 0) {
        struct simple *s = calloc(1, sizeof *s);
        s->c = c; s->id = id; c->refs++;
        if (op[0] == 'f') wps_objects_flush(flushed, s);
        else if (op[1] == 'h') wps_objects_flush(shutdown_flushed, s);
        else start_volume_job(s);
    } else if (handle_jobs(c, id, op, msg)) {
        /* selections and jobs */
    } else if (strcmp(op, "find") == 0) {
        const char *folder = cv_text(msg, "folder"), *name = cv_text(msg, "name"), *kind = cv_text(msg, "kind");
        const cv *sub = cv_get(msg, "subfolders");
        int64_t limit = 1000;
        cv_int(msg, "limit", &limit);
        if (!folder || folder[0] != '/' || limit < 1 || limit > 100000) {
            reply_error(c, id, "bad request", "find needs an absolute folder and a sane limit");
            cv_free(msg);
            return;
        }
        wps_find_kind k = !kind || !strcmp(kind, "any") ? WPS_FIND_ANY : !strcmp(kind, "folder") ? WPS_FIND_FOLDER :
                          !strcmp(kind, "file") ? WPS_FIND_FILE : !strcmp(kind, "program") ? WPS_FIND_PROGRAM : (wps_find_kind)-1;
        if ((int)k < 0) {
            reply_error(c, id, "bad request", "find: kind is any, folder, file or program");
            cv_free(msg);
            return;
        }
        c->refs++;   /* until the last batch */
        int64_t fid = wps_find_start(folder, name ? name : "", sub && sub->k == CV_BOOL && sub->b, k, (size_t)limit, found, c);
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result"); cb_map(&b, 1); cb_text(&b, "find"); cb_int(&b, fid);
        send_frame(c, &b);
        cb_free(&b);
    } else if (strcmp(op, "find_cancel") == 0) {
        int64_t fid = 0;
        cv_int(msg, "find", &fid);
        if (wps_find_cancel(fid)) {
            struct simple *s = calloc(1, sizeof *s);
            s->c = c; s->id = id; c->refs++;
            reply_simple_ok(s, "cancelled");
        } else {
            reply_error(c, id, "no such find", "it ended, or never was");
        }
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
        snprintf(r->folder, sizeof r->folder, "%s", folder);
        const cv *hid = cv_get(msg, "hidden");   /* (false: without dot names) */
        wps_listing_request_v(folder, cv_text(msg, "filter"), hid && hid->k == CV_BOOL && !hid->b, (size_t)off, (size_t)cnt,
                              det && det->k == CV_BOOL && det->b, list_reply, r);
    } else if (strcmp(op, "create") == 0) {
        const char *folder = cv_text(msg, "folder"), *clsname = cv_text(msg, "class");
        const char *title = cv_text(msg, "title"), *command = cv_text(msg, "command");
        wpo_class *cls = clsname ? wpo_find_class(clsname) : NULL;
        if (!folder || !cls || cls->is_meta || !wpo_subclass_of(cls, WPObjectClass) || wpo_subclass_of(cls, WPShadowClass)) {
            reply_error(c, id, "bad request", "create needs a folder and an object class (shadows: use shadow)");
            cv_free(msg);
            return;
        }
        start_create(c, id, folder, clsname, title, command);
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
    } else if (strcmp(op, "drive") == 0) {
        const char *uuid = cv_text(msg, "uuid"), *action = cv_text(msg, "action"), *pass = cv_text(msg, "pass");
        const char *target = cv_text(msg, "target");
        if ((!uuid && !target) || !action) { reply_error(c, id, "bad request", "drive needs uuid (or target) and action"); cv_free(msg); return; }
        struct drive_req *q = calloc(1, sizeof *q);
        q->c = c; c->refs++;
        q->id = id;
        if (uuid) drive_call(uuid, action, pass, drive_done, q);
        else {   /* a drive object (or its shadow): its UUID first */
            snprintf(q->uuid, sizeof q->uuid, "%.79s", target);
            snprintf(q->action, sizeof q->action, "%s", action);
            q->pass = pass ? strdup(pass) : NULL;
            if (strlen(target) >= sizeof q->uuid) { drive_done(q, EINVAL, "bad target", NULL); cv_free(msg); return; }
            wpo_defer(loop, "drive target", drive_target_work, drive_target_done, q);
        }
    } else if (strcmp(op, "drives") == 0) {
        wps_drive_info *ds = NULL;
        size_t n = wps_udisks_list(&ds);
        cbuf b;
        cb_init(&b);
        cb_map(&b, 4);
        cb_text(&b, "id"); cb_int(&b, id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "udisks"); cb_bool(&b, wps_udisks_available());
        cb_text(&b, "result"); cb_array(&b, n);
        for (size_t i = 0; i < n; i++) put_drive(&b, &ds[i]);
        send_frame(c, &b);
        cb_free(&b);
        free(ds);
    } else if (strcmp(op, "restore") == 0) {
        const char *t = cv_text(msg, "target");
        struct restore_job *j = calloc(1, sizeof *j);
        j->c = c; c->refs++;
        j->id = id;
        snprintf(j->path, sizeof j->path, "%s", t ? t : "");
        wpo_defer(loop, "restore", restore_work, restore_done, j);
    } else if (strcmp(op, "empty_trash") == 0) {
        struct empty_job *j = calloc(1, sizeof *j);
        j->c = c; c->refs++;
        j->id = id;
        wps_trash_home(j->files, sizeof j->files);
        strncat(j->files, "/files", sizeof j->files - strlen(j->files) - 1);
        wpo_defer(loop, "empty trash", empty_work, empty_done, j);
    } else if (strcmp(op, "startup_add") == 0) {
        const char *t = cv_text(msg, "target");
        if (!t) { reply_error(c, id, "bad request", "startup_add needs a target"); cv_free(msg); return; }
        struct startup_job *j = calloc(1, sizeof *j);
        j->c = c; c->refs++;
        j->id = id;
        sobj *known = strncmp(t, "id:", 3) == 0 ? wps_sobj_by_id(t + 3) : NULL;
        snprintf(j->target, sizeof j->target, "%s", known ? known->path : t);
        wpo_defer(loop, "startup", startup_work, startup_done, j);
    } else if (strcmp(op, "msg_add") == 0) {
        int64_t mid = wps_messages_add(msg);
        reply_int(c, id, mid);
        messages_broadcast(mid);
    } else if (strcmp(op, "msg_list") == 0) {
        cbuf b;
        cb_init(&b);
        cb_map(&b, 3);
        cb_text(&b, "id"); cb_int(&b, id);
        cb_text(&b, "ok"); cb_bool(&b, true);
        cb_text(&b, "result");
        cb_map(&b, 2);
        cb_text(&b, "unread"); cb_uint(&b, wps_messages_unread());
        cb_text(&b, "messages"); wps_messages_encode(&b);
        send_frame(c, &b);
        cb_free(&b);
    } else if (strcmp(op, "msg_set") == 0 || strcmp(op, "msg_delete") == 0 || strcmp(op, "msg_closed") == 0) {
        const cv *all = cv_get(msg, "all"), *rd = cv_get(msg, "read"), *cl = cv_get(msg, "closed");
        bool everything = all && all->k == CV_BOOL && all->b;
        size_t n;
        if (op[4] == 'd') n = wps_messages_delete(cv_get(msg, "ids"), everything);
        else if (op[4] == 's') n = wps_messages_set(cv_get(msg, "ids"), everything, rd && rd->k == CV_BOOL ? rd->b : -1,
                                                   cl && cl->k == CV_BOOL ? cl->b : -1);
        else {
            int64_t nid = 0;
            cv_int(msg, "nid", &nid);
            n = wps_messages_closed(nid, cv_text(msg, "sender"));
        }
        reply_int(c, id, (int64_t)n);
        if (n) messages_broadcast(0);
    } else if (strcmp(op, "first_start") == 0) {
        const char *d = cv_text(msg, "desktop");
        struct first_start *j = calloc(1, sizeof *j);
        j->c = c; c->refs++;
        j->id = id;
        if (d && d[0] == '/') snprintf(j->desk, sizeof j->desk, "%s", d);
        char real[PATH_MAX];   /* (renamed by its title only from now on) */
        if (d && d[0] == '/') snprintf(known_desktop, sizeof known_desktop, "%s", realpath(d, real) ? real : d);
        wpo_defer(loop, "first start", first_start_work, first_start_done, j);
    } else if (strcmp(op, "restore_defaults") == 0) {
        const char *d = cv_text(msg, "desktop");   /* (none: the one last opened) */
        char real[PATH_MAX];
        if (d && !realpath(d, real)) {
            reply_error(c, id, "bad request", "no such desktop folder");
            cv_free(msg);
            return;
        }
        defaults_start(c, id, d ? real : "", true);
    } else if (strcmp(op, "setup") == 0) {
        struct setup_req *r = calloc(1, sizeof *r);
        r->c = c; c->refs++;
        r->id = id;
        wpo_defer(loop, "settings objects", setup_req_work, setup_req_done, r);
    } else if (strcmp(op, "vtrash") == 0) {
        /* a central object (a shadow, ...) into the Trash Can, restorable */
        const char *t = cv_text(msg, "target");
        if (!t || strncmp(t, "vobj:", 5) || strlen(t) != 5 + WPS_ID_LEN) {
            reply_error(c, id, "bad request", "vtrash needs a central object (vobj:ID)");
            cv_free(msg);
            return;
        }
        struct vmove_job *j = calloc(1, sizeof *j);
        j->c = c; c->refs++;
        j->id = id;
        j->trash = true;
        snprintf(j->target, sizeof j->target, "%s", t);
        wps_trash_home(j->dest, sizeof j->dest);
        strncat(j->dest, "/files", sizeof j->dest - strlen(j->dest) - 1);
        wps_mkdirs(j->dest, 0700);
        wpo_defer(loop, "trash central object", vmove_work, vmove_done, j);
    } else if (strcmp(op, "vdelete") == 0) {
        const char *t = cv_text(msg, "target");
        if (!t || strncmp(t, "vobj:", 5) || strlen(t) != 5 + WPS_ID_LEN) {
            reply_error(c, id, "bad request", "vdelete needs a central object (vobj:ID)");
            cv_free(msg);
            return;
        }
        struct vmove_job *j = calloc(1, sizeof *j);
        j->c = c; c->refs++;
        j->id = id;
        j->del = true;
        snprintf(j->target, sizeof j->target, "%s", t);
        wpo_defer(loop, "delete central object", vmove_work, vmove_done, j);
    } else if (strcmp(op, "vmove") == 0) {
        const char *t = cv_text(msg, "target"), *dest = cv_text(msg, "folder");
        const cv *cp = cv_get(msg, "copy");
        if (!t || strncmp(t, "vobj:", 5) || strlen(t) != 5 + WPS_ID_LEN || !dest) {
            reply_error(c, id, "bad request", "vmove needs a central object (vobj:ID) and a folder");
            cv_free(msg);
            return;
        }
        struct vmove_job *j = calloc(1, sizeof *j);
        j->c = c; c->refs++;
        j->id = id;
        snprintf(j->target, sizeof j->target, "%s", t);
        snprintf(j->dest, sizeof j->dest, "%s", dest);
        j->copy = cp && cp->k == CV_BOOL && cp->b;
        wpo_defer(loop, "move central object", vmove_work, vmove_done, j);
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

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("wpsd %s\n", WPS_VERSION); return 0; }
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
    system_folder_init();
    wps_drive_query = drive_query;
    wps_startup_query = startup_query;
    wps_is_desktop = is_desktop;
    wps_startup_set = startup_set;
    {
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        wps_listing_folder_icon(comp, "computer.png");
        char sys[PATH_MAX];
        system_dir(sys, sizeof sys);
        wps_listing_folder_icon(sys, "computer.png");   /* (the owner's: as OS/2's System showed one) */
    }
    wps_udisks_init(loop, drives_changed);
    {
        char pf[PATH_MAX];
        data_dir("Programs", pf, sizeof pf);
        wps_listing_folder_icon(pf, "programs.png");
        wps_programs_init(loop, pf, env.state_dir);
    }
    {
        char mf[PATH_MAX + 16];
        snprintf(mf, sizeof mf, "%s/messages", env.state_dir);
        wps_messages_init(mf);
    }
    wps_listing_init(loop);
    wps_find_init(loop);
    wps_jobs_init(loop, db, env.identity);
    wps_launch_init(loop, on_in_use);
    wps_templates_init();
    wps_plugins_init(loop, &env);
    wps_on_central_change = on_central_change;
    wps_on_job_placed = job_placed;
    wps_on_changed = on_object_changed;
    wps_on_fs_event = on_fs_event;
    wps_on_job = on_job;
    if (!wps_trash_home_ready()) log_msg("the home trash cannot be made: %s", strerror(errno));
    wps_tracker_init(loop, &env, db);
    wpo_defer(loop, "built-in templates", builtin_templates_work, builtin_templates_done, NULL);
    wps_tracker_stats ts;
    wps_tracker_get_stats(&ts);
    log_msg("tracking changes with %s", ts.mode);
    log_msg("ready: socket %s, identity %s, host %s", sock_path, env.identity, env.host);
    wpo_loop_run(loop);

    wpo_loop_run_until_idle(loop, 10000);
    while (clients) client_close(clients);
    /* Running jobs pause and are journaled; they resume at the next start
     * (design: logout). */
    wps_find_shutdown();
    wps_jobs_shutdown(true);
    /* The jobs' last progress events (no subscribers remain to hear them). */
    wpo_loop_run_until_idle(loop, 1000);
    wps_plugins_shutdown();
    wps_launch_shutdown();
    wps_tracker_shutdown();
    wps_programs_shutdown();
    wps_messages_shutdown();
    wps_udisks_shutdown();
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

/* ---- Drives kept in step (v0.2-design 3, 5): a WPDrive central object
 * per volume in Drives (the root file system's in Computer), found by
 * its UUID (WPDrive "uuid"). A drive that goes away is parked (moved to
 * a folder ID no folder has) and brought back when it returns, keeping
 * its object ID and so its place. Titles follow the label unless the
 * user renamed it (WPDrive "autotitle": the title WPS gave). */

static void computer_dirs(char *comp, size_t cn, char *drives, size_t dn)
{
    char sys[PATH_MAX];
    system_dir(sys, sizeof sys);
    snprintf(comp, cn, "%s/Storages", sys);   /* (0.2's "Computer": moved at the start) */
    snprintf(drives, dn, "%s/Drives", comp);
}

#define DRIVES_PARKED "00000000-0000-4000-8000-0000000d15c0"

struct drive_obj { char id[WPS_ID_LEN + 1], uuid[80], title[256], autotitle[256]; int where; bool seen; };
struct drive_event { char folder[PATH_MAX]; char id[WPS_ID_LEN + 1]; char kind; };

struct drives_sync {
    wps_drive_info *ds;
    size_t n;
    char comp[PATH_MAX], drives[PATH_MAX + 8];
    char vol[WPS_VOLID_MAX], cid[WPS_ID_LEN + 1], did[WPS_ID_LEN + 1];
    struct drive_obj *objs;
    size_t nobjs;
    int collecting;   /* where the objects being listed are: 0 Drives, 1 Computer, 2 parked */
    struct drive_event *ev;
    size_t nev;
    bool changed;
};
static bool drives_syncing, drives_again;

static void drives_collect(const char *vol, const char *id, void *user)
{
    struct drives_sync *j = user;
    char loc[WPS_ID_LEN + 8], key[PATH_MAX + 128];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_props ps;
    wps_props_init(&ps);
    char *v = NULL;
    wps_db_key(key, sizeof key, loc, "WPS", "class");
    bool drive = wps_db_get_class(db, vol, loc, "WPS", &ps) > 0 && wps_props_str(&ps, "class", &v) && !strcmp(v, "WPDrive");
    free(v);
    wps_props_free(&ps);
    if (!drive) return;
    struct drive_obj o;
    memset(&o, 0, sizeof o);
    snprintf(o.id, sizeof o.id, "%s", id);
    o.where = j->collecting;
    wps_props_init(&ps);
    if (wps_db_get_class(db, vol, loc, "WPDrive", &ps) > 0) {
        v = NULL;
        if (wps_props_str(&ps, "uuid", &v)) snprintf(o.uuid, sizeof o.uuid, "%s", v);
        free(v);
        v = NULL;
        if (wps_props_str(&ps, "autotitle", &v)) snprintf(o.autotitle, sizeof o.autotitle, "%s", v);
        free(v);
    }
    wps_props_free(&ps);
    wps_props_init(&ps);
    v = NULL;
    if (wps_db_get_class(db, vol, loc, "WPObject", &ps) > 0 && wps_props_str(&ps, "title", &v)) snprintf(o.title, sizeof o.title, "%s", v);
    free(v);
    wps_props_free(&ps);
    j->objs = realloc(j->objs, (j->nobjs + 1) * sizeof *j->objs);
    j->objs[j->nobjs++] = o;
}

static void drive_event(struct drives_sync *j, int where, const char *id, char kind)
{
    if (where == 2) return;   /* parked: no window shows it */
    j->ev = realloc(j->ev, (j->nev + 1) * sizeof *j->ev);
    struct drive_event *e = &j->ev[j->nev++];
    snprintf(e->folder, sizeof e->folder, "%s", where ? j->comp : j->drives);
    snprintf(e->id, sizeof e->id, "%s", id);
    e->kind = kind;
}

static void put_str(const char *vol, const char *id, const char *cls, const char *prop, const char *v)
{
    char loc[WPS_ID_LEN + 8], key[PATH_MAX + 128];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_db_key(key, sizeof key, loc, cls, prop);
    wps_db_put(db, vol, key, 's', v, (uint32_t)strlen(v));
}

static void drives_sync_work(void *arg)
{
    struct drives_sync *j = arg;
    wps_loaded c, d;
    wps_load(j->comp, true, &c);
    wps_load(j->drives, true, &d);
    bool ok = !c.err && !d.err && c.id[0] && d.id[0] && S_ISDIR(c.st.st_mode) && S_ISDIR(d.st.st_mode) && !strcmp(c.vol, d.vol);
    if (ok) {
        snprintf(j->vol, sizeof j->vol, "%s", d.vol);
        snprintf(j->cid, sizeof j->cid, "%s", c.id);
        snprintf(j->did, sizeof j->did, "%s", d.id);
    }
    wps_loaded_free(&c);
    wps_loaded_free(&d);
    if (!ok) return;   /* no Computer (the user removed it, or first start not yet) */
    j->collecting = 0;
    wps_central_in(j->vol, j->did, drives_collect, j);
    j->collecting = 1;
    wps_central_in(j->vol, j->cid, drives_collect, j);
    j->collecting = 2;
    wps_central_in(j->vol, DRIVES_PARKED, drives_collect, j);
    for (size_t i = 0; i < j->n; i++) {
        const wps_drive_info *v = &j->ds[i];
        int where = !strcmp(v->kind, "root") ? 1 : 0;
        char title[256];
        wps_drive_title(v, title, sizeof title);
        struct drive_obj *o = NULL;
        for (size_t k = 0; k < j->nobjs && !o; k++)
            if (!j->objs[k].seen && !strcmp(j->objs[k].uuid, v->uuid)) o = &j->objs[k];
        char newid[WPS_ID_LEN + 1];
        if (!o) {
            if (wps_central_create(j->vol, where ? j->cid : j->did, "WPDrive", title, NULL, newid) != 0) continue;
            put_str(j->vol, newid, "WPDrive", "uuid", v->uuid);
            put_str(j->vol, newid, "WPDrive", "autotitle", title);
            drive_event(j, where, newid, 'c');
            j->changed = true;
            continue;
        }
        o->seen = true;
        if (o->where != where) {
            if (wps_central_move(o->id, j->vol, where ? j->cid : j->did, false, newid) == 0) {
                drive_event(j, o->where, o->id, 'd');
                drive_event(j, where, o->id, 'c');
                j->changed = true;
            }
        }
        /* its label changed (and the user did not rename it) */
        if (strcmp(o->autotitle, title) && (!o->title[0] || !strcmp(o->title, o->autotitle))) {
            put_str(j->vol, o->id, "WPObject", "title", title);
            put_str(j->vol, o->id, "WPDrive", "autotitle", title);
            drive_event(j, where, o->id, 't');
            j->changed = true;
        }
    }
    for (size_t k = 0; k < j->nobjs; k++) {
        struct drive_obj *o = &j->objs[k];
        if (o->seen || o->where == 2) continue;
        char newid[WPS_ID_LEN + 1];
        if (wps_central_move(o->id, j->vol, DRIVES_PARKED, false, newid) == 0) {   /* gone: parked */
            drive_event(j, o->where, o->id, 'd');
            j->changed = true;
        }
    }
}

static void drives_broadcast(void)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead || (!x->nfolders && !x->ntargets)) continue;
        cbuf b;
        event_head(&b, "drives_changed", x->id, 0);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

static void drives_sync_done(void *arg)
{
    struct drives_sync *j = arg;
    for (size_t i = 0; i < j->nev; i++) {
        wps_central_forget(j->ev[i].id);
        if (j->ev[i].kind != 't') on_central_change(j->ev[i].folder, j->ev[i].id, j->ev[i].kind);
    }
    drives_broadcast();   /* mounted, unmounted, titles: the icons and rows again */
    free(j->ev);
    free(j->objs);
    free(j->ds);
    free(j);
    drives_syncing = false;
    if (drives_again) { drives_again = false; drives_changed(); }
}

static void drives_changed(void)
{
    if (drives_syncing) { drives_again = true; return; }
    drives_syncing = true;
    struct drives_sync *j = calloc(1, sizeof *j);
    j->n = wps_udisks_list(&j->ds);
    computer_dirs(j->comp, sizeof j->comp, j->drives, sizeof j->drives);
    wpo_defer(loop, "drives sync", drives_sync_work, drives_sync_done, j);
}
