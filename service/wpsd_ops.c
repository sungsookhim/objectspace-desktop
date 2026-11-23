/* wpsd: The protocol's ops: a handler each, and the table handle() looks them up in. */
#define _GNU_SOURCE
#include "wpsd.h"

/* ---- the protocol's ops (a table: handle() below) */

/* ping */
/* "pong", and what this service is: a GUI newer than its service
 * (during an upgrade, before logging out) can tell */
static bool op_ping(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op; (void)msg;
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
    return false;
}

/* describe */
static bool op_describe(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    do_describe(c, id, cv_text(msg, "class"));
    return false;
}

/* apps: Other program... (Open With): every installed application
 */
static bool op_apps(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op; (void)msg;
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
    return false;
}

/* flush, shutdown, sync */
static bool op_flush(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)msg;
    struct simple *s = calloc(1, sizeof *s);
    s->c = c; s->id = id; c->refs++;
    if (op[0] == 'f') wps_objects_flush(flushed, s);
    else if (op[1] == 'h') wps_objects_flush(shutdown_flushed, s);
    else start_volume_job(s);
    return false;
}

/* find */
static bool op_find(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *folder = cv_text(msg, "folder"), *name = cv_text(msg, "name"), *kind = cv_text(msg, "kind");
    const cv *sub = cv_get(msg, "subfolders");
    int64_t limit = 1000;
    cv_int(msg, "limit", &limit);
    if (!folder || folder[0] != '/' || limit < 1 || limit > 100000) {
        reply_error(c, id, "bad request", "find needs an absolute folder and a sane limit");
        return false;
    }
    wps_find_kind k = !kind || !strcmp(kind, "any") ? WPS_FIND_ANY : !strcmp(kind, "folder") ? WPS_FIND_FOLDER :
                      !strcmp(kind, "file") ? WPS_FIND_FILE : !strcmp(kind, "program") ? WPS_FIND_PROGRAM : (wps_find_kind)-1;
    if ((int)k < 0) {
        reply_error(c, id, "bad request", "find: kind is any, folder, file or program");
        return false;
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
    return false;
}

/* find_cancel */
static bool op_find_cancel(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    int64_t fid = 0;
    cv_int(msg, "find", &fid);
    if (wps_find_cancel(fid)) {
        struct simple *s = calloc(1, sizeof *s);
        s->c = c; s->id = id; c->refs++;
        reply_simple_ok(s, "cancelled");
    } else {
        reply_error(c, id, "no such find", "it ended, or never was");
    }
    return false;
}

/* list */
static bool op_list(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *folder = cv_text(msg, "target");
    int64_t off = 0, cnt = 100;
    cv_int(msg, "offset", &off);
    cv_int(msg, "count", &cnt);
    const cv *det = cv_get(msg, "details");
    if (!folder || off < 0 || cnt < 0 || cnt > 100000) {
        reply_error(c, id, "bad request", "list needs a target folder and a sane range");
        return false;
    }
    struct list_req *r = calloc(1, sizeof *r);
    r->c = c; c->refs++;
    r->id = id;
    snprintf(r->folder, sizeof r->folder, "%s", folder);
    const cv *hid = cv_get(msg, "hidden");   /* (false: without dot names) */
    wps_listing_request_v(folder, cv_text(msg, "filter"), hid && hid->k == CV_BOOL && !hid->b, (size_t)off, (size_t)cnt,
                          det && det->k == CV_BOOL && det->b, list_reply, r);
    return false;
}

/* create */
static bool op_create(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *folder = cv_text(msg, "folder"), *clsname = cv_text(msg, "class");
    const char *title = cv_text(msg, "title"), *command = cv_text(msg, "command");
    wpo_class *cls = clsname ? wpo_find_class(clsname) : NULL;
    if (!folder || !cls || cls->is_meta || !wpo_subclass_of(cls, WPObjectClass) || wpo_subclass_of(cls, WPShadowClass)) {
        reply_error(c, id, "bad request", "create needs a folder and an object class (shadows: use shadow)");
        return false;
    }
    start_create(c, id, folder, clsname, title, command);
    return false;
}

/* shadow */
static bool op_shadow(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *t = cv_text(msg, "target"), *f = cv_text(msg, "folder");
    if (!t || !f) { reply_error(c, id, "bad request", "shadow needs target and folder"); return false; }
    struct simple *s = calloc(1, sizeof *s);
    s->c = c; s->id = id; c->refs++;
    /* An ID of an object we hold: use where it is now. */
    sobj *known = strncmp(t, "id:", 3) == 0 ? wps_sobj_by_id(t + 3) : NULL;
    wps_shadow_create(known ? known->path : t, f, shadow_made, s);
    return false;
}

/* scan */
static bool op_scan(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op; (void)msg;
    struct simple *s = calloc(1, sizeof *s);
    s->c = c; s->id = id; c->refs++;
    wps_tracker_scan(scanned, s);
    return false;
}

/* stats */
static bool op_stats(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op; (void)msg;
    struct stats_job *j = calloc(1, sizeof *j);
    j->c = c; j->id = id; c->refs++;
    wpo_defer(loop, "stats", stats_work, stats_done, j);
    return false;
}

/* drive */
static bool op_drive(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *uuid = cv_text(msg, "uuid"), *action = cv_text(msg, "action"), *pass = cv_text(msg, "pass");
    const char *target = cv_text(msg, "target");
    if ((!uuid && !target) || !action) { reply_error(c, id, "bad request", "drive needs uuid (or target) and action"); return false; }
    struct drive_req *q = calloc(1, sizeof *q);
    q->c = c; c->refs++;
    q->id = id;
    if (uuid) drive_call(uuid, action, pass, drive_done, q);
    else {   /* a drive object (or its shadow): its UUID first */
        snprintf(q->uuid, sizeof q->uuid, "%.79s", target);
        snprintf(q->action, sizeof q->action, "%s", action);
        q->pass = pass ? strdup(pass) : NULL;
        if (strlen(target) >= sizeof q->uuid) { drive_done(q, EINVAL, "bad target", NULL); return false; }
        wpo_defer(loop, "drive target", drive_target_work, drive_target_done, q);
    }
    return false;
}

/* drives */
static bool op_drives(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op; (void)msg;
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
    return false;
}

/* restore */
static bool op_restore(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *t = cv_text(msg, "target");
    struct restore_job *j = calloc(1, sizeof *j);
    j->c = c; c->refs++;
    j->id = id;
    snprintf(j->path, sizeof j->path, "%s", t ? t : "");
    wpo_defer(loop, "restore", restore_work, restore_done, j);
    return false;
}

/* empty_trash */
static bool op_empty_trash(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op; (void)msg;
    struct empty_job *j = calloc(1, sizeof *j);
    j->c = c; c->refs++;
    j->id = id;
    wps_trash_home(j->files, sizeof j->files);
    strncat(j->files, "/files", sizeof j->files - strlen(j->files) - 1);
    wpo_defer(loop, "empty trash", empty_work, empty_done, j);
    return false;
}

/* startup_add */
static bool op_startup_add(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *t = cv_text(msg, "target");
    if (!t) { reply_error(c, id, "bad request", "startup_add needs a target"); return false; }
    struct startup_job *j = calloc(1, sizeof *j);
    j->c = c; c->refs++;
    j->id = id;
    sobj *known = strncmp(t, "id:", 3) == 0 ? wps_sobj_by_id(t + 3) : NULL;
    snprintf(j->target, sizeof j->target, "%s", known ? known->path : t);
    wpo_defer(loop, "startup", startup_work, startup_done, j);
    return false;
}

/* msg_add */
static bool op_msg_add(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    int64_t mid = wps_messages_add(msg);
    reply_int(c, id, mid);
    messages_broadcast(mid);
    return false;
}

/* msg_list */
static bool op_msg_list(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op; (void)msg;
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
    return false;
}

/* msg_set, msg_delete, msg_closed */
static bool op_msg_set(struct client *c, int64_t id, const char *op, cv *msg)
{
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
    return false;
}

/* first_start */
static bool op_first_start(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *d = cv_text(msg, "desktop");
    struct first_start *j = calloc(1, sizeof *j);
    j->c = c; c->refs++;
    j->id = id;
    if (d && d[0] == '/') snprintf(j->desk, sizeof j->desk, "%s", d);
    char real[PATH_MAX];   /* (renamed by its title only from now on) */
    if (d && d[0] == '/') snprintf(known_desktop, sizeof known_desktop, "%s", realpath(d, real) ? real : d);
    wpo_defer(loop, "first start", first_start_work, first_start_done, j);
    return false;
}

/* restore_defaults */
static bool op_restore_defaults(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *d = cv_text(msg, "desktop");   /* (none: the one last opened) */
    char real[PATH_MAX];
    if (d && !realpath(d, real)) {
        reply_error(c, id, "bad request", "no such desktop folder");
        return false;
    }
    defaults_start(c, id, d ? real : "", true);
    return false;
}

/* setup */
static bool op_setup(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op; (void)msg;
    struct setup_req *r = calloc(1, sizeof *r);
    r->c = c; c->refs++;
    r->id = id;
    wpo_defer(loop, "settings objects", setup_req_work, setup_req_done, r);
    return false;
}

/* vtrash: a central object (a shadow, ...) into the Trash Can, restorable
 */
static bool op_vtrash(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *t = cv_text(msg, "target");
    if (!t || strncmp(t, "vobj:", 5) || strlen(t) != 5 + WPS_ID_LEN) {
        reply_error(c, id, "bad request", "vtrash needs a central object (vobj:ID)");
        return false;
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
    return false;
}

/* vdelete */
static bool op_vdelete(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *t = cv_text(msg, "target");
    if (!t || strncmp(t, "vobj:", 5) || strlen(t) != 5 + WPS_ID_LEN) {
        reply_error(c, id, "bad request", "vdelete needs a central object (vobj:ID)");
        return false;
    }
    struct vmove_job *j = calloc(1, sizeof *j);
    j->c = c; c->refs++;
    j->id = id;
    j->del = true;
    snprintf(j->target, sizeof j->target, "%s", t);
    wpo_defer(loop, "delete central object", vmove_work, vmove_done, j);
    return false;
}

/* vmove */
static bool op_vmove(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    const char *t = cv_text(msg, "target"), *dest = cv_text(msg, "folder");
    const cv *cp = cv_get(msg, "copy");
    if (!t || strncmp(t, "vobj:", 5) || strlen(t) != 5 + WPS_ID_LEN || !dest) {
        reply_error(c, id, "bad request", "vmove needs a central object (vobj:ID) and a folder");
        return false;
    }
    struct vmove_job *j = calloc(1, sizeof *j);
    j->c = c; c->refs++;
    j->id = id;
    snprintf(j->target, sizeof j->target, "%s", t);
    snprintf(j->dest, sizeof j->dest, "%s", dest);
    j->copy = cp && cp->k == CV_BOOL && cp->b;
    wpo_defer(loop, "move central object", vmove_work, vmove_done, j);
    return false;
}

/* place */
static bool op_place(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
    start_place(c, id, msg);
    return false;
}

/* subscribe */
static bool op_subscribe(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
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
    return false;
}

/* unsubscribe */
static bool op_unsubscribe(struct client *c, int64_t id, const char *op, cv *msg)
{
    (void)op;
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
    return false;
}

/* resolve, call, menu, menu_select, settings, settings_set, open, from_template, rename, layout, arrange */
static bool op_resolve(struct client *c, int64_t id, const char *op, cv *msg)
{
    const char *target = cv_text(msg, "target");
    if (!target) { reply_error(c, id, "bad request", "missing target"); return false; }
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
    return true;   /* the request keeps msg */
}

static const struct op_def ops[] = {
    { "ping", op_ping },
    { "describe", op_describe },
    { "apps", op_apps },
    { "flush", op_flush },
    { "shutdown", op_flush },
    { "sync", op_flush },
    { "find", op_find },
    { "find_cancel", op_find_cancel },
    { "list", op_list },
    { "create", op_create },
    { "shadow", op_shadow },
    { "scan", op_scan },
    { "stats", op_stats },
    { "drive", op_drive },
    { "drives", op_drives },
    { "restore", op_restore },
    { "empty_trash", op_empty_trash },
    { "startup_add", op_startup_add },
    { "msg_add", op_msg_add },
    { "msg_list", op_msg_list },
    { "msg_set", op_msg_set },
    { "msg_delete", op_msg_set },
    { "msg_closed", op_msg_set },
    { "first_start", op_first_start },
    { "restore_defaults", op_restore_defaults },
    { "setup", op_setup },
    { "vtrash", op_vtrash },
    { "vdelete", op_vdelete },
    { "vmove", op_vmove },
    { "place", op_place },
    { "subscribe", op_subscribe },
    { "unsubscribe", op_unsubscribe },
    { "resolve", op_resolve },
    { "call", op_resolve },
    { "menu", op_resolve },
    { "menu_select", op_resolve },
    { "settings", op_resolve },
    { "settings_set", op_resolve },
    { "get", op_resolve },
    { "set", op_resolve },
    { "props", op_resolve },
    { "open", op_resolve },
    { "from_template", op_resolve },
    { "rename", op_resolve },
    { "layout", op_resolve },
    { "arrange", op_resolve },
};

static void handle(struct client *c, cv *msg)
{
    int64_t id = 0;
    cv_int(msg, "id", &id);
    const char *op = cv_text(msg, "op");
    if (!op) { reply_error(c, id, "bad request", "missing op"); cv_free(msg); return; }

    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++)
        if (!strcmp(op, ops[i].name)) {
            if (!ops[i].fn(c, id, op, msg)) cv_free(msg);
            return;
        }
    if (!(handle_jobs(c, id, op, msg))) reply_error(c, id, "bad request", "unknown op");
    cv_free(msg);
}

void client_io(int fd, short rev, void *arg)
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

void on_accept(int fd, short rev, void *arg)
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

void on_signal(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    struct signalfd_siginfo si;
    ssize_t r = read(fd, &si, sizeof si);
    (void)r;
    log_msg("signal %u: writing pending changes and stopping", si.ssi_signo);
    wps_objects_flush(quit_after_flush, NULL);
}

void on_watchdog(const char *label, double ms, void *user)
{
    (void)user;
    log_msg("watchdog: \"%s\" held the loop for %.1f ms", label, ms);
}
