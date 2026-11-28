/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
#define _GNU_SOURCE
#include "osdd.h"

static void put_object_depth(cbuf *b, osdo_object *o, int depth);
static void local_call_reply(struct req *r, osdo_object *o);

/*
 * osdd - the per-user Objectspace object service (M2).
 *
 * Serves clients on $OSD_RUNTIME_DIR/objectspace.sock (or $OSD_SOCKET): each message
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
 *   {op:"ping"}                         "pong", with version (OSD_VERSION) and protocol (a number)
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
 *   {op:"work_area", target:FOLDER, act:"open"|"close"|"minimize"|"restore"}
 *                                  a work area's group: {pids, scopes, reopen}
 *   {op:"rename", target, name}   rename a file or folder in its folder (never
 *                      replacing); its title then follows the name. Without
 *                      permission to rename, the name becomes its title
 *                      (shared or private, as titles are). Others: title.
 *   {op:"create", folder, class, title, command}   a new object (file, folder,
 *                      .desktop program or .osdobj abstract object), never overwriting
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

osd_env env;

osd_db *db;

osdo_loop *loop;

static char sock_path[PATH_MAX + 32];

bool quitting;

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osdd: ");
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
    osdo_loop_unwatch_fd(loop, c->fd);
    close(c->fd);
    client_unref(c);
}

/* A client that cannot be written to: closed now from the loop
 * (client_io), else marked and closed by the loop's next turn for it - a
 * send made while walking the subscriptions must not free them under the
 * walk (an event to a client that had gone). */
static void drop(struct client *c, bool now)
{
    if (now) { client_close(c); return; }
    c->broken = true;
    osdo_loop_watch_fd(loop, c->fd, POLLIN | POLLOUT, client_io, c);
}

static void flush(struct client *c, bool now)
{
    while (c->outoff < c->outlen) {
        ssize_t w = send(c->fd, c->out + c->outoff, c->outlen - c->outoff, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN) break;
            drop(c, now);
            return;
        }
        c->outoff += (size_t)w;
    }
    if (c->outoff == c->outlen) c->outoff = c->outlen = 0;
    /* A client that stops reading gets a bounded buffer, then is dropped:
     * nobody else waits for it (design: Responsiveness). */
    if (c->outlen > 16u << 20) { drop(c, now); return; }
    osdo_loop_watch_fd(loop, c->fd, POLLIN | (c->outlen ? POLLOUT : 0), client_io, c);
}

void client_flush(struct client *c) { flush(c, true); }

void send_frame(struct client *c, cbuf *b)
{
    if (c->dead || c->broken || b->oom) return;
    size_t need = c->outlen + 4 + b->n;
    unsigned char *n = realloc(c->out, need);
    if (!n) { drop(c, false); return; }
    c->out = n;
    unsigned char *w = c->out + c->outlen;
    w[0] = (unsigned char)(b->n >> 24); w[1] = (unsigned char)(b->n >> 16);
    w[2] = (unsigned char)(b->n >> 8); w[3] = (unsigned char)b->n;
    memcpy(w + 4, b->d, b->n);
    c->outlen = need;
    flush(c, false);
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

void put_object(cbuf *b, osdo_object *o) { put_object_depth(b, o, 0); }

static void put_object_depth(cbuf *b, osdo_object *o, int depth)
{
    sobj *crec = osd_sobj_of(o);
    bool central = crec && crec->central;
    if (!o) { cb_null(b); return; }
    const char *path = osd_object_path(o);
    char *title = osdo_is_a(o, OsdObjectClass) ? OsdObject_osdQueryTitle(o) : NULL;
    bool shadow = osdo_is_a(o, OsdShadowClass);
    sobj *rec = osd_sobj_of(o);
    bool missing = rec && rec->declared_class[0];
    /* a template that makes another class (Folder, Data File, Program): its
     * Class... is that class's */
    char *makes = NULL;
    osdo_class *mk = NULL;
    if (osdo_is_a(o, OsdObjectClass) && osd_state_get_str(o, "Objectspace", "makes", &makes) && makes && *makes) mk = osdo_find_class(makes);
    cb_map(b, 8 + (shadow ? 2 : 0) + (missing ? 1 : 0) + (mk ? 2 : 0));
    if (mk) {
        char *mt = OsdObject_osdclsQueryTitle(mk);
        cb_text(b, "makes"); cb_text(b, mk->name);
        cb_text(b, "makes_title"); cb_text(b, mt && *mt ? mt : mk->name);
        free(mt);
    }
    free(makes);
    cb_text(b, "path"); path ? cb_text(b, path) : cb_null(b);
    cb_text(b, "address");
    if (central) {
        char a[OSD_ID_LEN + 8];
        snprintf(a, sizeof a, "vobj:%s", OsdObject_data(o)->id);
        cb_text(b, a);
    } else {
        path ? cb_text(b, path) : cb_null(b);
    }
    cb_text(b, "id"); cb_text(b, osdo_is_a(o, OsdObjectClass) ? OsdObject_data(o)->id : "");
    cb_text(b, "class"); cb_text(b, o->cls->name);
    char *ct = osdo_is_a(o, OsdObjectClass) ? OsdObject_osdclsQueryTitle(o->cls) : NULL;   /* ("Folder": its Class... menu) */
    cb_text(b, "class_title"); cb_text(b, ct && *ct ? ct : o->cls->name);
    free(ct);
    cb_text(b, "title"); title ? cb_text(b, title) : cb_null(b);
    cb_text(b, "changes"); cb_uint(b, osdo_is_a(o, OsdObjectClass) ? OsdObject_data(o)->changes : 0);
    cb_text(b, "trashed"); cb_bool(b, path && osd_in_trash(path));
    if (missing) { cb_text(b, "class_not_installed"); cb_text(b, rec->declared_class); }
    if (shadow) {
        osdo_object *t = OsdShadow_data(o)->target;
        cb_text(b, "target");
        if (t && depth == 0) put_object_depth(b, t, 1);
        else cb_null(b);
        cb_text(b, "available"); cb_bool(b, t != NULL);
    }
    free(title);
}

static void put_value(cbuf *b, const osdo_value *v)
{
    switch (v->type) {
    case 'b': cb_bool(b, v->u.b); break;
    case 'i': cb_int(b, v->u.i); break;
    case 'd': cb_double(b, v->u.d); break;
    case 's': v->u.s ? cb_text(b, v->u.s) : cb_null(b); break;
    case 'o': put_object(b, v->u.o); break;
    case 'p': {
        const osdo_value *pv = osdo_pending_value(v->u.p);
        if (pv) put_value(b, pv);
        else cb_text(b, "<pending>");
        break;
    }
    default: cb_null(b); break;
    }
}

static void req_free(struct req *r)
{
    osd_loaded_free(&r->tgt);
    osd_loaded_free(&r->shadow_target);
    free(r->remote_text);
    free(r->open_with);
    for (int i = 0; r->objs && i < r->nobj; i++) osd_loaded_free(&r->objs[i]);
    free(r->objs);
    cv_free(&r->msg);
    client_unref(r->c);
    free(r);
}

static char *open_with_list(const char *p);

void prepare_work(void *arg)    /* worker */
{
    struct req *r = arg;
    osd_load(r->target, false, &r->tgt);
    if (!r->tgt.err && (!strcmp(r->op, "menu") || !strcmp(r->op, "menu_select")))
        r->open_with = open_with_list(r->tgt.path);   /* (here, off the loop) */
    if (!r->tgt.err && r->tgt.shadow_of[0]) {
        char t[OSD_ID_LEN + 4];
        snprintf(t, sizeof t, "id:%s", r->tgt.shadow_of);
        osd_load(t, false, &r->shadow_target);   /* fails if its volume is not mounted */
        /* Not where it was last seen (moved while this user was away, say):
         * the shadow file's hint, if the object there is the right one;
         * and remember where it is now. */
        char tid[OSD_ID_LEN + 1], hint[PATH_MAX];
        if (r->shadow_target.err && r->tgt.path[0] && osd_shadow_read(r->tgt.path, tid, hint, sizeof hint) && hint[0] &&
            !strcmp(tid, r->tgt.shadow_of)) {
            osd_loaded alt;
            osd_load(hint, false, &alt);
            if (!alt.err && !strcmp(alt.id, r->tgt.shadow_of)) {
                osd_loaded_free(&r->shadow_target);
                r->shadow_target = alt;
                osd_db_set_location(db, alt.id, alt.vol, alt.rel);
            } else {
                osd_loaded_free(&alt);
            }
        }
    }
    /* a shadow's menu is its original's: its Open with list too */
    if (!r->tgt.err && r->tgt.shadow_of[0] && !r->shadow_target.err && r->shadow_target.path[0] &&
        (!strcmp(r->op, "menu") || !strcmp(r->op, "menu_select"))) {
        free(r->open_with);
        r->open_with = open_with_list(r->shadow_target.path);
    }
    for (int i = 0; i < r->nobj; i++) osd_load(r->objpath[i], true, &r->objs[i]);
}

void reply_ok_object(struct req *r, osdo_object *o, const osdo_value *result) { reply_ok_object_sound(r, o, result, NULL); }

/* The same, with the sound of the event that came with it (fire_event's:
 * NULL none, so the Sound object's; "" silence). */
void reply_ok_object_sound(struct req *r, osdo_object *o, const osdo_value *result, const char *sound)
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
    osdo_unref(x->o);
    free(x);
    req_free(r);
}

/* Returns true if the reply will come later (the request is kept). */
static bool do_call(struct req *r, osdo_object *o)
{
    const char *method = cv_text(&r->msg, "method");
    int64_t ex;
    if (method && osd_plugin_routes(o, method) &&
        !(cv_int(&r->msg, "expect", &ex) && (uint64_t)ex != OsdObject_data(o)->changes)) {
        struct remote_ctx *x = calloc(1, sizeof *x);
        x->r = r;
        x->o = osdo_ref(o);
        osd_plugin_call(o, osd_object_path(o), method, cv_get(&r->msg, "args"), 2000, remote_done, x);
        return true;
    }
    local_call_reply(r, o);
    return false;
}

static void local_call_reply(struct req *r, osdo_object *o)
{
    const char *method = cv_text(&r->msg, "method");
    const cv *args = cv_get(&r->msg, "args");
    int64_t expect;
    if (!method) { reply_error(r->c, r->id, "bad request", "call needs a method"); return; }
    /* Optimistic concurrency (design: Concurrency): a write based on an
     * old view of the object is refused with the current values. */
    if (cv_int(&r->msg, "expect", &expect) && (uint64_t)expect != OsdObject_data(o)->changes) {
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
    osdo_value av[MAX_ARGS] = { 0 };
    size_t na = 0;
    int oi = 0;
    for (size_t i = 0; args && args->k == CV_ARRAY && i < args->n && na < MAX_ARGS; i++) {
        const cv *a = &args->items[i];
        switch (a->k) {
        case CV_BOOL: av[na++] = osdo_v_bool(a->b); break;
        case CV_INT: av[na++] = osdo_v_int(a->i); break;
        case CV_FLOAT: av[na++] = osdo_v_double(a->d); break;
        case CV_TEXT: av[na++] = osdo_v_string(a->s); break;
        case CV_MAP: {
            osdo_object *ao = oi < r->nobj ? osd_object_from_loaded(&r->objs[oi]) : NULL;
            if (!ao) {
                reply_error(r->c, r->id, "not found", oi < r->nobj ? strerror(r->objs[oi].err) : "object");
                for (size_t k = 0; k < na; k++) osdo_value_clear(&av[k]);
                return;
            }
            oi++;
            av[na++] = osdo_v_object(ao);
            break;
        }
        default: av[na++] = osdo_v_void(); break;
        }
    }
    osdo_value ret;
    osdo_status st = osdo_send(o, method, av, na, &ret);
    for (size_t k = 0; k < na; k++) osdo_value_clear(&av[k]);
    if (st != OSDO_OK) {
        reply_error(r->c, r->id, st == OSDO_ENOMETHOD ? "no method" : st == OSDO_EARGS ? "bad arguments" : "failed",
                    osdo_strerror(st));
        return;
    }
    reply_ok_object(r, o, &ret);
    osdo_value_clear(&ret);
}

/* Open With (v0.3-design 7): a data file's Open is a cascade of the
 * programs for its type, the default checked, then Other program...;
 * the GUI opens item OSD_MI_OPEN_APP + k with "app_index" k. */
/* (a worker: reading every program's desktop entry takes a while) The
 * items for a regular file that is not a program, or NULL. */
static char *open_with_list(const char *p)
{
    struct stat st;
    if (!p || !*p || stat(p, &st) != 0 || !S_ISREG(st.st_mode) || osd_is_program_file(p)) return NULL;
    osd_app *a = malloc(OSD_MI_OPEN_APPS * sizeof *a);
    int n = a ? osd_apps_for_file(p, NULL, 0, a, OSD_MI_OPEN_APPS) : 0;
    size_t len = 0, cap = (size_t)n * 200 + 64;
    char *r = malloc(cap);
    if (!r) { free(a); return NULL; }
    r[0] = 0;
    for (int i = 0; i < n; i++) {
        char name[128];
        snprintf(name, sizeof name, "%s", a[i].name);
        for (char *q = name; *q; q++) if (*q == '\t' || *q == '\n') *q = ' ';
        len += (size_t)snprintf(r + len, cap - len, "%d\t%s\t1\t%s\n", OSD_MI_OPEN_APP + i, name, a[i].is_default ? "checked" : "");
    }
    snprintf(r + len, cap - len, "%d\tOther program...\t1\n", OSD_MI_OPEN_OTHER);
    free(a);
    return r;
}

/* the menu with them, for a data file (made on the worker: r->open_with) */
static char *open_with_items(osdo_object *o, struct req *r, char *menu)
{
    if (!osdo_is_a(o, OsdDataFileClass) || !r->open_with) return menu;
    char *m = osd_concat(menu, r->open_with);
    free(menu);
    return m;
}

/* A shadow's menu (OS/2's): its original's, then the shadow's own items
 * the original's has not (by ID); the GUI's own items act on the shadow
 * row as before (Delete: the shadow; Properties, Open: the original). */
static char *shadow_menu(osdo_object *shadow, osdo_object *orig, struct req *r)
{
    char *t = open_with_items(orig, r, call_text(orig, "osdModifyPopupMenu", ""));
    char *own = call_text(shadow, "osdModifyPopupMenu", "");
    for (const char *l = own; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l), idl = strcspn(l, "\t\n");
        bool have = false;
        for (const char *m = t; m && *m && !have; m = strchr(m, '\n') ? strchr(m, '\n') + 1 : NULL)
            have = strcspn(m, "\t\n") == idl && !strncmp(m, l, idl);
        if (have) continue;
        char line[600];
        snprintf(line, sizeof line, "%s%.*s\n", t && *t && t[strlen(t) - 1] != '\n' ? "\n" : "", (int)len, l);
        char *n = osd_concat(t, line);
        free(t);
        t = n;
    }
    free(own);
    return t;
}

static osdo_object *shadow_orig(osdo_object *o)
{
    return o && osdo_is_a(o, OsdShadowClass) ? OsdShadow_data(o)->target : NULL;   /* (borrowed; NULL: unavailable) */
}

static void do_menus(struct req *r, osdo_object *o)
{
    const char *err = NULL;
    int64_t v;
    osdo_object *orig = shadow_orig(o);
    if (!strcmp(r->op, "menu_select")) {
        osdo_value a, ret = osdo_v_void();
        a = osdo_v_int(cv_int(&r->msg, "item", &v) ? v : -1);
        /* (a shadow: its original's item, if the original takes it) */
        bool ok = !r->remote_done && orig && osdo_send(orig, "osdMenuItemSelected", &a, 1, &ret) == OSDO_OK && ret.type == 'b' && ret.u.b;
        if (orig && !r->remote_done && !ok) { osdo_value_clear(&ret); ret = osdo_v_void(); }
        if (!ok)
            ok = r->remote_done ? r->remote_ok   /* (its plug-in's host handled it) */
                                : osdo_send(o, "osdMenuItemSelected", &a, 1, &ret) == OSDO_OK && ret.type == 'b' && ret.u.b;
        if (!r->remote_done) osdo_value_clear(&ret);
        if (!ok) { reply_error(r->c, r->id, "failed", "the item was not handled"); return; }
    } else if (!strcmp(r->op, "settings_set")) {
        if (cv_int(&r->msg, "expect", &v) && (uint64_t)v != OsdObject_data(o)->changes) err = "conflict";
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
        char *t = r->remote_text ? strdup(r->remote_text) : orig ? shadow_menu(o, orig, r) : call_text(o, "osdModifyPopupMenu", "");
        if (!r->remote_text && !orig) t = open_with_items(o, r, t);
        put_menu(&b, t, "");
        free(t);
    } else {
        char *t = r->remote_text ? settings_text_with(o, strdup(r->remote_text)) : settings_text(o);
        char shown[PATH_MAX];   /* what its Icon page shows without its own */
        const char *p = osdo_is_a(o, OsdFileSystemClass) ? OsdFileSystem_data(o)->path : NULL;
        char *mt = NULL;
        osdo_value mr = { 0 };
        if (osdo_is_a(o, OsdDataFileClass) && osdo_send(o, "osdQueryMimeType", NULL, 0, &mr) == OSDO_OK && mr.type == 's' && mr.u.s) mt = strdup(mr.u.s);
        osdo_value_clear(&mr);
        if (p || !osdo_is_a(o, OsdObjectClass) || !osd_listing_central_shown(OsdObject_data(o)->id, shown, sizeof shown))
            osd_listing_shown_icon(p, o->cls->name, mt && *mt ? mt : NULL, osdo_is_a(o, OsdFolderClass), osdo_is_a(o, OsdProgramClass), shown, sizeof shown);
        free(mt);
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
    osdo_unref(x->o);
    free(x);
    req_free(r);
}

/* ---- a plug-in's methods, run in its host (object-properties-design.md
 * 2.7): a remote call, then what follows (cont: true if it keeps r - it
 * started another step). Missed deadlines: NULL, the parent's behaviour. */
struct rstep { struct req *r; osdo_object *o; bool (*cont)(struct req *r, osdo_object *o, const cv *result); };

static void rstep_done(void *arg, const cv *result)
{
    struct rstep *x = arg;
    bool keep = !x->r->c->dead && x->cont(x->r, x->o, result);
    osdo_unref(x->o);
    if (!keep) req_free(x->r);
    free(x);
}

static void remote_step(struct req *r, osdo_object *o, const char *method, cv *args,
                        bool (*cont)(struct req *r, osdo_object *o, const cv *result))
{
    struct rstep *x = calloc(1, sizeof *x);
    x->r = r;
    x->o = osdo_ref(o);
    x->cont = cont;
    osd_plugin_call(o, osd_object_path(o), method, args, 2000, rstep_done, x);
}

static bool pages_got(struct req *r, osdo_object *o, const cv *result);

/* settings_set: the field's setter a plug-in's method? Its host sets it. */
static bool setter_got(struct req *r, osdo_object *o, const cv *result)
{
    r->remote_done = true;
    r->remote_ok = result && (result->k != CV_BOOL || result->b);
    do_menus(r, o);
    return false;
}

/* Started its plug-in's setter (true), or nothing done (false: set here). */
static bool settings_set_remote(struct req *r, osdo_object *o)
{
    const cv *values = cv_get(&r->msg, "values");
    char setter[128];
    if (values && values->k == CV_MAP && values->n == 1 && values->items[0].k == CV_TEXT &&
        settings_setter_of(o, r->remote_text, values->items[0].s, setter, sizeof setter) && setter[0] != '@' &&
        osd_plugin_routes(o, setter)) {
        cv args = { .k = CV_ARRAY, .n = 1, .nitems = 1, .items = (cv *)&values->items[1] };
        remote_step(r, o, setter, &args, setter_got);
        return true;
    }
    return false;
}

static bool pages_got(struct req *r, osdo_object *o, const cv *result)
{
    if (result && result->k == CV_TEXT) r->remote_text = strdup(result->s);
    if (!strcmp(r->op, "settings_set") && settings_set_remote(r, o)) return true;
    do_menus(r, o);
    return false;
}

static bool selected_got(struct req *r, osdo_object *o, const cv *result)
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
    osdo_object *o = osd_object_from_loaded(&r->tgt);
    if (o && osdo_is_a(o, OsdShadowClass)) {
        /* (a central original, such as a settings object, has no path) */
        osdo_object *t = r->tgt.shadow_of[0] && !r->shadow_target.err && (r->shadow_target.path[0] || r->shadow_target.central)
                            ? osd_object_from_loaded(&r->shadow_target) : NULL;
        OsdShadow_osdSetTarget(o, t);   /* NULL: unavailable (volume not mounted, or gone) */
    }
    if (!o) reply_error(r->c, r->id, "failed", "cannot create the object");
    else if (strcmp(r->op, "resolve") == 0) reply_ok_object(r, o, NULL);
    else if (!strcmp(r->op, "menu") && osd_plugin_routes(o, "osdModifyPopupMenu")) {
        /* A plug-in builds this menu: ask its host, with a deadline. */
        struct remote_ctx *x = calloc(1, sizeof *x);
        x->r = r;
        x->o = osdo_ref(o);
        cv args = { .k = CV_ARRAY, .n = 1, .nitems = 1 };
        cv empty = { .k = CV_TEXT, .s = "", .len = 0 };
        args.items = &empty;
        osd_plugin_call(o, osd_object_path(o), "osdModifyPopupMenu", &args, 2000, menu_remote_done, x);
        return;
    }
    else if ((!strcmp(r->op, "settings") || !strcmp(r->op, "settings_set")) && osd_plugin_routes(o, "osdAddSettingsPages")) {
        cv empty = { .k = CV_TEXT, .s = "", .len = 0 }, args = { .k = CV_ARRAY, .n = 1, .nitems = 1, .items = &empty };
        remote_step(r, o, "osdAddSettingsPages", &args, pages_got);   /* its plug-in's pages */
        return;
    }
    else if (!strcmp(r->op, "settings_set") && settings_set_remote(r, o)) return;
    else if (!strcmp(r->op, "menu_select") && osd_plugin_routes(o, "osdMenuItemSelected")) {
        int64_t item = -1;
        cv_int(&r->msg, "item", &item);
        cv it = { .k = CV_INT, .i = item }, args = { .k = CV_ARRAY, .n = 1, .nitems = 1, .items = &it };
        remote_step(r, o, "osdMenuItemSelected", &args, selected_got);   /* its plug-in handles the item */
        return;
    }
    else if (!strcmp(r->op, "menu") || !strcmp(r->op, "settings") || !strcmp(r->op, "settings_set") ||
             !strcmp(r->op, "menu_select")) do_menus(r, o);
    else if (!strcmp(r->op, "open")) do_open(r, o);
    else if (!strcmp(r->op, "from_template")) do_from_template(r, o);
    else if (!strcmp(r->op, "rename")) do_rename(r, o);
    else if (!strcmp(r->op, "layout")) do_layout(r, o);
    else if (!strcmp(r->op, "arrange")) do_arrange(r, o);
    else if (!strcmp(r->op, "work_area")) do_work_area(r, o);
    else if (!strcmp(r->op, "get") || !strcmp(r->op, "set") || !strcmp(r->op, "props") || !strcmp(r->op, "reset")) do_props(r, o);
    else if (do_call(r, o)) return;   /* the reply comes from the plug-in */
    req_free(r);
}

/* ---- dispatch */

/* A batch of a search's results, as a "found" event to the client that
 * asked (the search's ID as its sub). */
void found(void *arg, int64_t find, const osd_found *items, size_t n, bool done, uint64_t scanned, bool truncated,
                  int err)
{
    struct client *c = arg;
    if (c->dead) {
        if (!done) osd_find_cancel(find);   /* nobody reads it any more */
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
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("osdd %s\n", OSD_VERSION); return 0; }
    char err[512];
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (osd_env_load(&env, err, sizeof err) < 0) { log_msg("%s", err); return 1; }

    /* One service per user (design: Concurrency). */
    char lockpath[PATH_MAX + 16];
    snprintf(lockpath, sizeof lockpath, "%s/osdd.lock", env.state_dir);
    int lockfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
        log_msg("another osdd is already running for this user (%s)", lockpath);
        return 1;
    }

    char dbpath[PATH_MAX + 16];
    snprintf(dbpath, sizeof dbpath, "%s/objects.db", env.state_dir);
    db = osd_db_open(dbpath, env.writer, err, sizeof err);
    if (!db) { log_msg("%s", err); return 1; }
    if (osdo_init() != OSDO_OK || osd_classes_register() != OSDO_OK) { log_msg("class registration failed"); return 1; }
    osd_mime_init(getenv("OSD_MIME_GLOBS"));
    if (geteuid() == 0) {
        /* Supported, not recommended: root's service could see every
         * user's files. It never follows other users' homes on its own. */
        log_msg("warning: running as root; other users' home folders are not tracked");
        osd_set_root_session(true);
    }

    loop = osdo_loop_new(8);   /* workers wait on the disk; more of them keep it busy */
    osdo_loop_set_watchdog(loop, 50.0, on_watchdog, NULL);
    const char *delay = getenv("OSD_SAVE_DELAY_MS");
    osd_objects_init(&env, db, loop, delay ? atoi(delay) : 2000);
    props_init();   /* the chain's class and type settings (osdd_props.c) */

    const char *sp = getenv("OSD_SOCKET");
    if (sp && *sp) snprintf(sock_path, sizeof sock_path, "%s", sp);
    else snprintf(sock_path, sizeof sock_path, "%s/objectspace.sock", env.runtime_dir);
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    /* A socket address holds 107 bytes; a longer path must not be cut
     * short (the stale-socket cleanup below would miss the real name). */
    if (strlen(sock_path) >= sizeof sa.sun_path) {
        log_msg("%s: socket path longer than %zu bytes; set OSD_RUNTIME_DIR or OSD_SOCKET to a shorter one",
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
    osdo_loop_watch_fd(loop, ls, POLLIN, on_accept, NULL);

    int mfd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mfd >= 0) osdo_loop_watch_fd(loop, mfd, POLLPRI, on_mounts, NULL);

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    int sfd = signalfd(-1, &sigs, SFD_CLOEXEC | SFD_NONBLOCK);
    osdo_loop_watch_fd(loop, sfd, POLLIN, on_signal, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Know the volumes before the first request; attach portable ones in
     * the background. */
    osd_volumes vs;
    if (osd_volumes_scan(&vs, &env) == 0) osd_volumes_replace(&vs);
    start_volume_job(NULL);
    osd_shadows_init(loop, db);
    system_folder_init();
    osd_drive_query = drive_query;
    osd_startup_query = startup_query;
    osd_is_desktop = is_desktop;
    osd_is_program = osd_is_program_file;
    osd_startup_set = startup_set;
    {
        char comp[PATH_MAX], drv[PATH_MAX + 8];
        computer_dirs(comp, sizeof comp, drv, sizeof drv);
        osd_listing_folder_icon(comp, "computer.png");
        char sys[PATH_MAX];
        system_dir(sys, sizeof sys);
        osd_listing_folder_icon(sys, "computer.png");   /* (the owner's: as OS/2's System showed one) */
        /* (0.4 step 2: the theme has these too) */
        osd_listing_folder_icon(drv, "drives.png");
        const char *home = getenv("HOME");
        if (home && *home) osd_listing_folder_icon(home, "home.png");
        if (osd_templates_dir()[0]) osd_listing_folder_icon(osd_templates_dir(), "templates.png");
        char st[PATH_MAX];
        osd_startup_dir(st, sizeof st);
        if (st[0]) osd_listing_folder_icon(st, "startup.png");
    }
    osd_udisks_init(loop, drives_changed);
    network_init();   /* (0.8: GNOME's shares and windows on shares, every five seconds) */
    {
        char pf[PATH_MAX];
        data_dir("Programs", pf, sizeof pf);
        osd_listing_folder_icon(pf, "programs.png");
        osd_programs_init(loop, pf, env.state_dir);
    }
    {
        char mf[PATH_MAX + 16];
        snprintf(mf, sizeof mf, "%s/messages", env.state_dir);
        osd_messages_init(mf);
    }
    osd_listing_init(loop);
    osd_find_init(loop);
    osd_jobs_init(loop, db, env.identity);
    osd_launch_init(loop, on_in_use);
    osd_templates_init();
    osd_plugins_init(loop, &env);
    osd_on_central_change = on_central_change;
    osd_on_job_placed = job_placed;
    osd_on_changed = on_object_changed;
    osd_on_fs_event = on_fs_event;
    osd_on_job = on_job;
    if (!osd_trash_home_ready()) log_msg("the home trash cannot be made: %s", strerror(errno));
    osd_tracker_init(loop, &env, db);
    osdo_defer(loop, "built-in templates", builtin_templates_work, builtin_templates_done, NULL);
    osd_tracker_stats ts;
    osd_tracker_get_stats(&ts);
    log_msg("tracking changes with %s", ts.mode);
    log_msg("ready: socket %s, identity %s, host %s", sock_path, env.identity, env.host);
    osdo_loop_run(loop);

    osdo_loop_run_until_idle(loop, 10000);
    while (clients) client_close(clients);
    /* Running jobs pause and are journaled; they resume at the next start
     * (design: logout). */
    osd_find_shutdown();
    osd_jobs_shutdown(true);
    /* The jobs' last progress events (no subscribers remain to hear them). */
    osdo_loop_run_until_idle(loop, 1000);
    osd_plugins_shutdown();
    osd_launch_shutdown();
    osd_tracker_shutdown();
    osd_programs_shutdown();
    osd_messages_shutdown();
    osd_udisks_shutdown();
    osd_listing_shutdown();
    unlink(sock_path);
    osd_objects_shutdown();
    osdo_loop_free(loop);
    osd_db_close(db);
    osdo_shutdown();
    osd_plugins_free_defs();
    log_msg("stopped");
    return quitting ? 0 : 1;
}
