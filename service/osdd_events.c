/* osdd: Subscriptions and events (design: Service API, Events). */
#define _GNU_SOURCE
#include "osdd.h"

struct sub *subs;

int64_t next_sub = 1;

void subs_drop_client(struct client *c)
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
void event_head(cbuf *b, const char *name, int64_t sub, size_t extra)
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
    osd_trash_home(files, sizeof files);
    strncat(files, "/files", sizeof files - strlen(files) - 1);
    size_t l = strlen(files);
    if (!path || strncmp(path, files, l) || path[l] != '/' || strchr(path + l + 1, '/')) return;
    bool full = osd_trash_full(files);
    if (trash_was_full == (int)full) return;
    trash_was_full = full;
    trash_broadcast(full);
}

/* The messages changed: every client with a subscription (each once). */
void messages_broadcast(int64_t added)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        bool sent = false;
        for (struct sub *y = subs; y != x && !sent; y = y->next) sent = y->c == x->c;
        if (sent) continue;
        cbuf b;
        event_head(&b, "messages_changed", x->id, 3);
        cb_text(&b, "unread"); cb_uint(&b, osd_messages_unread());
        cb_text(&b, "total"); cb_uint(&b, osd_messages_count());
        cb_text(&b, "added"); cb_int(&b, added);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

/* A message for the desktop's status line (osdctl say, a script's or a
 * timer's): every client once; the desktop shows it. */
void say_broadcast(const char *text, bool stay)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        bool sent = false;
        for (struct sub *y = subs; y != x && !sent; y = y->next) sent = y->c == x->c;
        if (sent) continue;
        cbuf b;
        event_head(&b, "say", x->id, 2);
        cb_text(&b, "text"); cb_text(&b, text);
        cb_text(&b, "stay"); cb_bool(&b, stay);
        send_frame(x->c, &b);
        cb_free(&b);
    }
}

static void collect_shadow(const char *vol, const char *id, void *user)
{
    (void)vol;
    struct shadow_ids *sh = user;
    if (sh->n < 32) snprintf(sh->ids[sh->n++], sizeof sh->ids[0], "%s", id);
}

/* The osd command's questions for the desktop (0.4.1 step 14): "ask" (a
 * dialog: text, buttons), "selection" (the active folder window's), "show"
 * (a folder's window, or a view); once per client. How many were sent. */
size_t desktop_broadcast(const char *kind, int64_t ask, const char *text, const cv *buttons)
{
    size_t n = 0;
    for (struct sub *x = subs; x; x = x->next) {
        if (x->c->dead) continue;
        bool sent = false;
        for (struct sub *y = subs; y != x && !sent; y = y->next) sent = y->c == x->c;
        if (sent) continue;
        cbuf b;
        size_t nb = buttons && buttons->k == CV_ARRAY ? buttons->n : 0;
        event_head(&b, kind, x->id, 3);
        cb_text(&b, "ask"); cb_int(&b, ask);
        cb_text(&b, "text"); cb_text(&b, text ? text : "");
        cb_text(&b, "buttons");
        cb_array(&b, nb);
        for (size_t i = 0; i < nb; i++) cb_text(&b, buttons->items[i].k == CV_TEXT ? buttons->items[i].s : "");
        send_frame(x->c, &b);
        cb_free(&b);
        n++;
    }
    return n;
}

/* The System object's Terminal (v0.3-design 8), for text-mode programs. */
void terminal_from_system(osdo_object *o)
{
    char *t = OsdSetup_osdQuerySettings(o), choice[64] = "", cmd[1024] = "";
    for (char *q = t, *l; q && (l = strsep(&q, "\n"));) {
        char *tab = strchr(l, '\t');
        if (!tab) continue;
        *tab = 0;
        if (!strcmp(l, "terminal")) snprintf(choice, sizeof choice, "%s", tab + 1);
        else if (!strcmp(l, "terminal_command")) snprintf(cmd, sizeof cmd, "%s", tab + 1);
    }
    free(t);
    osd_terminal_choose(choice, cmd);
}

void on_object_changed(osdo_object *o)
{
    if (osdo_is_a(o, OsdSystemClass)) terminal_from_system(o);
    if (osdo_is_a(o, OsdTrashCanClass)) trash_broadcast(trash_was_full == 1);   /* (its icons) */
    if (osdo_is_a(o, OsdSchemeClass)) props_broadcast("schemes");   /* its colours: every window */
    sobj *rec = osd_sobj_of(o);
    const char *path = rec ? rec->path : osd_object_path(o);
    const char *id = osdo_is_a(o, OsdObjectClass) ? OsdObject_data(o)->id : "";
    for (struct sub *x = subs; x; x = x->next) {
        bool hit = false;
        for (size_t i = 0; i < x->ntargets && !hit; i++)
            hit = (id[0] && !strcmp(x->targets[i], id)) || (path && !strcmp(x->targets[i], path));
        for (size_t i = 0; i < x->nfolders && !hit && path; i++) hit = in_folder(path, x->folders[i]);
        /* a central object (no file): the folders listing it, e.g. System
         * Setup's objects renamed - their windows showed the old title */
        if (!hit && rec && rec->central && id[0]) {
            char nm[OSD_ID_LEN + 16];
            snprintf(nm, sizeof nm, "\x01vobj:%s", id);
            for (size_t i = 0; i < x->nfolders && !hit; i++) hit = osd_listing_has(x->folders[i], nm);
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
    osd_db_vobjs_shadowing(db, id, collect_shadow, &sh);
    for (int k = 0; k < sh.n; k++) {
        char nm[OSD_ID_LEN + 16], addr[OSD_ID_LEN + 8];
        snprintf(nm, sizeof nm, "\x01vobj:%s", sh.ids[k]);
        snprintf(addr, sizeof addr, "vobj:%s", sh.ids[k]);
        for (struct sub *x = subs; x; x = x->next) {
            bool hit = false;
            for (size_t i = 0; i < x->nfolders && !hit; i++) hit = osd_listing_has(x->folders[i], nm);
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

/* Icon positions were stored in a folder without a client asking (a job's
 * dropped items, placed after the job reported it was done): windows on
 * it read them again, else they keep the spot they gave the new icon. */
void on_layout_changed(const char *folder)
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
void on_central_change(const char *folder, const char *id, char kind)
{
    if (is_schemes_path(folder)) props_broadcast("schemes");   /* a scheme made or gone: the palette's list */
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/\x01vobj:%s", folder, id);
    osd_listing_apply(folder, kind == 'c' ? 'c' : 'd', p + strlen(folder) + 1, NULL, DT_UNKNOWN);
    on_fs_event(kind, "", p);
}

void on_fs_event(char kind, const char *from, const char *path)
{
    /* One of Objectspace's own folders renamed or moved outside Objectspace (a shell, a
     * file manager): put back, as osdd finds them by their paths (else the
     * desktop's shadows would follow the folder and osdd would make a new,
     * empty one). The move back comes as its own event. */
    if (kind == 'm' && from[0] && path[0] && osd_folder(from) && access(from, F_OK) != 0) {
        if (rename(path, from) == 0) { log_msg("%s is Objectspace's own folder: moved back from %s", from, path); return; }
        log_msg("%s is Objectspace's own folder, moved to %s: cannot move it back (%s)", from, path, strerror(errno));
    }
    if (kind == 'm') osd_launch_moved(from, path);
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
            if (in_new && osd_is_lock_file(strrchr(path, '/') + 1)) in_new = false;
            if (in_old && osd_is_lock_file(strrchr(from, '/') + 1)) in_old = false;
            if (!in_new && !in_old) continue;
            cbuf b;
            /* version: the folder's listing after this change (0: read it again) */
            uint64_t ver = osd_listing_version(f);
            if (kind == 'm' && in_new && in_old) {
                event_head(&b, "object_moved", x->id, 5);
                cb_text(&b, "folder"); cb_text(&b, f);
                cb_text(&b, "from"); cb_text(&b, strrchr(from, '/') + 1);
                cb_text(&b, "to"); cb_text(&b, strrchr(path, '/') + 1);
                cb_text(&b, "version"); cb_uint(&b, ver);
                cb_text(&b, "index"); cb_int(&b, osd_listing_last_index(f));   /* where it is, by position */
            } else {
                bool added = kind == 'c' || (kind == 'm' && in_new);
                const char *p = added ? path : (kind == 'm' ? from : path);
                const char *nm = strrchr(p, '/') + 1;
                event_head(&b, added ? "content_added" : "content_removed", x->id, 5);
                cb_text(&b, "folder"); cb_text(&b, f);
                cb_text(&b, "name"); cb_text(&b, nm);
                cb_text(&b, "version"); cb_uint(&b, ver);
                cb_text(&b, "index"); cb_int(&b, osd_listing_last_index(f));   /* where, by position (added: at the end) */
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

/* Programs Objectspace started opened or closed `path`: every window may show it
 * (directly, or as a shadow), so every subscription hears it. */
void on_in_use(const char *path, bool in_use)
{
    work_area_in_use(path, in_use);   /* (a work area's group: when it ended) */
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

void on_job(int64_t jid)
{
    for (struct sub *x = subs; x; x = x->next) {
        if (!x->jobs || x->c->dead) continue;
        cbuf st;
        cb_init(&st);
        if (!osd_job_status(jid, &st)) { cb_free(&st); return; }
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

void on_volumes_changed(const char *report)
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

void strlist(const cv *a, char ***out, size_t *n)
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
