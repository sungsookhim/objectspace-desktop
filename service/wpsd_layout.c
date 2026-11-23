/* wpsd: Icon positions (layout, place, arrange), vmove, rename, create, the built-in templates. */
#define _GNU_SOURCE
#include "wpsd.h"

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

void do_layout(struct req *r, wpo_object *o)
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
void do_arrange(struct req *r, wpo_object *o)
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
void job_placed(const char *dest, char **paths, int64_t *x, int64_t *y, size_t n)
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

void start_place(struct client *c, int64_t id, const cv *msg)
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

void vmove_work(void *arg)
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

void vmove_done(void *arg)
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

char known_desktop[PATH_MAX];   /* the desktop's folder (its first_start says) */

/* WPS's own folders: System, My Storages, Drives, Programs, System Setup,
 * the Trash Can, Startup; and the desktop and Templates (XDG user folders:
 * renamed, user-dirs.dirs would not follow). */

bool is_desktop(const char *path) { return known_desktop[0] && path && !strcmp(path, known_desktop); }

static bool own_folder(const char *path)
{
    if (!path || !*path) return false;
    if (known_desktop[0] && !strcmp(path, known_desktop)) return true;
    if (wps_templates_dir()[0] && !strcmp(path, wps_templates_dir())) return true;
    return wps_folder(path);
}

/* The folders wpsd finds by their paths (the desktop and Templates are
 * the user's: user-dirs.dirs says where they are). */
bool wps_folder(const char *path)
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

void do_rename(struct req *r, wpo_object *o)
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

void do_from_template(struct req *r, wpo_object *o)
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
            char *sound = fire_event(o, wps_object_path(o), "object.create", NULL, NULL, NULL, 0);   /* its actions (When made) */
            struct req fake = { .c = r->c, .id = r->id };
            reply_ok_object_sound(&fake, o, NULL, sound);
            free(sound);
        }
    }
    wps_loaded_free(&r->ld);
    client_unref(r->c);
    free(r);
}

void start_create(struct client *c, int64_t id, const char *folder, const char *cls, const char *title,
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
void builtin_templates_work(void *arg)
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
