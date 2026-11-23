#define _GNU_SOURCE
#include "wpsd.h"

static void put_object_depth(cbuf *b, wpo_object *o, int depth);
static void local_call_reply(struct req *r, wpo_object *o);

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

wps_env env;

wps_db *db;

wpo_loop *loop;

static char sock_path[PATH_MAX + 32];

bool quitting;

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wpsd: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

struct client *clients;

void client_unref(struct client *c)
{
    if (--c->refs) return;
    free(c->in);
    free(c->out);
    free(c);
}

void client_close(struct client *c)
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

void client_flush(struct client *c)
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

void send_frame(struct client *c, cbuf *b)
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

void reply_error(struct client *c, int64_t id, const char *error, const char *message)
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

void put_object(cbuf *b, wpo_object *o) { put_object_depth(b, o, 0); }

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
    /* a template that makes another class (Folder, Data File, Program): its
     * Class... is that class's */
    char *makes = NULL;
    wpo_class *mk = NULL;
    if (wpo_is_a(o, WPObjectClass) && wps_state_get_str(o, "WPS", "makes", &makes) && makes && *makes) mk = wpo_find_class(makes);
    cb_map(b, 8 + (shadow ? 2 : 0) + (missing ? 1 : 0) + (mk ? 2 : 0));
    if (mk) {
        char *mt = WPObject_wpclsQueryTitle(mk);
        cb_text(b, "makes"); cb_text(b, mk->name);
        cb_text(b, "makes_title"); cb_text(b, mt && *mt ? mt : mk->name);
        free(mt);
    }
    free(makes);
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
    char *ct = wpo_is_a(o, WPObjectClass) ? WPObject_wpclsQueryTitle(o->cls) : NULL;   /* ("Folder": its Class... menu) */
    cb_text(b, "class_title"); cb_text(b, ct && *ct ? ct : o->cls->name);
    free(ct);
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

void prepare_work(void *arg)    /* worker */
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

void reply_ok_object(struct req *r, wpo_object *o, const wpo_value *result) { reply_ok_object_sound(r, o, result, NULL); }

/* The same, with the sound of the event that came with it (fire_event's:
 * NULL none, so the Sound object's; "" silence). */
void reply_ok_object_sound(struct req *r, wpo_object *o, const wpo_value *result, const char *sound)
{
    cbuf b;
    cb_init(&b);
    cb_map(&b, sound ? 5 : 4);
    if (sound) { cb_text(&b, "sound"); cb_text(&b, sound); }
    cb_text(&b, "id"); cb_int(&b, r->id);
    cb_text(&b, "ok"); cb_bool(&b, true);
    cb_text(&b, "result");
    if (result) put_value(&b, result);
    else put_object(&b, o);
    cb_text(&b, "object"); put_object(&b, o);
    send_frame(r->c, &b);
    cb_free(&b);
}

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
        bool ok = r->remote_done ? r->remote_ok   /* (its plug-in's host handled it) */
                                 : wpo_send(o, "wpMenuItemSelected", &a, 1, &ret) == WPO_OK && ret.type == 'b' && ret.u.b;
        if (!r->remote_done) wpo_value_clear(&ret);
        if (!ok) { reply_error(r->c, r->id, "failed", "the item was not handled"); return; }
    } else if (!strcmp(r->op, "settings_set")) {
        if (cv_int(&r->msg, "expect", &v) && (uint64_t)v != WPObject_data(o)->changes) err = "conflict";
        else if (!r->remote_done) err = apply_settings(o, cv_get(&r->msg, "values"), r->remote_text);
        else if (!r->remote_ok) err = "the setter refused the value";
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
        char *t = r->remote_text ? settings_text_with(o, strdup(r->remote_text)) : settings_text(o);
        char shown[PATH_MAX];   /* what its Icon page shows without its own */
        const char *p = wpo_is_a(o, WPFileSystemClass) ? WPFileSystem_data(o)->path : NULL;
        wps_listing_shown_icon(p, wpo_is_a(o, WPFolderClass), wpo_is_a(o, WPProgramClass), shown, sizeof shown);
        put_settings(&b, o, t, shown);
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

/* ---- a plug-in's methods, run in its host (object-properties-design.md
 * 2.7): a remote call, then what follows (cont: true if it keeps r - it
 * started another step). Missed deadlines: NULL, the parent's behaviour. */
struct rstep { struct req *r; wpo_object *o; bool (*cont)(struct req *r, wpo_object *o, const cv *result); };

static void rstep_done(void *arg, const cv *result)
{
    struct rstep *x = arg;
    bool keep = !x->r->c->dead && x->cont(x->r, x->o, result);
    wpo_unref(x->o);
    if (!keep) req_free(x->r);
    free(x);
}

static void remote_step(struct req *r, wpo_object *o, const char *method, cv *args,
                        bool (*cont)(struct req *r, wpo_object *o, const cv *result))
{
    struct rstep *x = calloc(1, sizeof *x);
    x->r = r;
    x->o = wpo_ref(o);
    x->cont = cont;
    wps_plugin_call(o, wps_object_path(o), method, args, 2000, rstep_done, x);
}

static bool pages_got(struct req *r, wpo_object *o, const cv *result);

/* settings_set: the field's setter a plug-in's method? Its host sets it. */
static bool setter_got(struct req *r, wpo_object *o, const cv *result)
{
    r->remote_done = true;
    r->remote_ok = result && (result->k != CV_BOOL || result->b);
    do_menus(r, o);
    return false;
}

/* Started its plug-in's setter (true), or nothing done (false: set here). */
static bool settings_set_remote(struct req *r, wpo_object *o)
{
    const cv *values = cv_get(&r->msg, "values");
    char setter[128];
    if (values && values->k == CV_MAP && values->n == 1 && values->items[0].k == CV_TEXT &&
        settings_setter_of(o, r->remote_text, values->items[0].s, setter, sizeof setter) && setter[0] != '@' &&
        wps_plugin_routes(o, setter)) {
        cv args = { .k = CV_ARRAY, .n = 1, .nitems = 1, .items = (cv *)&values->items[1] };
        remote_step(r, o, setter, &args, setter_got);
        return true;
    }
    return false;
}

static bool pages_got(struct req *r, wpo_object *o, const cv *result)
{
    if (result && result->k == CV_TEXT) r->remote_text = strdup(result->s);
    if (!strcmp(r->op, "settings_set") && settings_set_remote(r, o)) return true;
    do_menus(r, o);
    return false;
}

static bool selected_got(struct req *r, wpo_object *o, const cv *result)
{
    r->remote_done = true;
    r->remote_ok = result && result->k == CV_BOOL && result->b;
    do_menus(r, o);
    return false;
}

void prepare_done(void *arg)    /* loop */
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
    else if ((!strcmp(r->op, "settings") || !strcmp(r->op, "settings_set")) && wps_plugin_routes(o, "wpAddSettingsPages")) {
        cv empty = { .k = CV_TEXT, .s = "", .len = 0 }, args = { .k = CV_ARRAY, .n = 1, .nitems = 1, .items = &empty };
        remote_step(r, o, "wpAddSettingsPages", &args, pages_got);   /* its plug-in's pages */
        return;
    }
    else if (!strcmp(r->op, "settings_set") && settings_set_remote(r, o)) return;
    else if (!strcmp(r->op, "menu_select") && wps_plugin_routes(o, "wpMenuItemSelected")) {
        int64_t item = -1;
        cv_int(&r->msg, "item", &item);
        cv it = { .k = CV_INT, .i = item }, args = { .k = CV_ARRAY, .n = 1, .nitems = 1, .items = &it };
        remote_step(r, o, "wpMenuItemSelected", &args, selected_got);   /* its plug-in handles the item */
        return;
    }
    else if (!strcmp(r->op, "menu") || !strcmp(r->op, "settings") || !strcmp(r->op, "settings_set") ||
             !strcmp(r->op, "menu_select")) do_menus(r, o);
    else if (!strcmp(r->op, "open")) do_open(r, o);
    else if (!strcmp(r->op, "from_template")) do_from_template(r, o);
    else if (!strcmp(r->op, "rename")) do_rename(r, o);
    else if (!strcmp(r->op, "layout")) do_layout(r, o);
    else if (!strcmp(r->op, "arrange")) do_arrange(r, o);
    else if (!strcmp(r->op, "get") || !strcmp(r->op, "set") || !strcmp(r->op, "props") || !strcmp(r->op, "reset")) do_props(r, o);
    else if (do_call(r, o)) return;   /* the reply comes from the plug-in */
    req_free(r);
}

/* ---- dispatch */

/* A batch of a search's results, as a "found" event to the client that
 * asked (the search's ID as its sub). */
void found(void *arg, int64_t find, const wps_found *items, size_t n, bool done, uint64_t scanned, bool truncated,
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
    props_init();   /* the chain's class and type settings (wpsd_props.c) */

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
