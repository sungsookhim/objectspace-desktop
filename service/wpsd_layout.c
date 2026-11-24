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
        char p[PATH_MAX + NAME_MAX + 2], id[OSD_ID_LEN + 1];
        snprintf(p, sizeof p, "%s/%s", j->folder, de->d_name);
        if (!osd_attr_get_id(p, id)) continue;
        for (size_t i = 0; i < j->n; i++)
            if (!j->names[i] && !strcmp(j->ids[i], id)) { j->names[i] = strdup(de->d_name); found++; break; }
    }
    if (d) closedir(d);
    /* Central objects of this folder are named "\x01vobj:<id>". */
    char fid[OSD_ID_LEN + 1];
    if (found < j->n && osd_attr_get_id(j->folder, fid)) {
        for (size_t i = 0; i < j->n; i++) {
            if (j->names[i]) continue;
            char loc[OSD_ID_LEN + 8];
            snprintf(loc, sizeof loc, "vobj:%s", j->ids[i]);
            osd_loaded ld;
            osd_load(loc, false, &ld);
            if (!ld.err && !strcmp(ld.folder_id, fid)) {
                char nm[OSD_ID_LEN + 8];
                snprintf(nm, sizeof nm, "\x01vobj:%s", j->ids[i]);
                j->names[i] = strdup(nm);
            }
            osd_loaded_free(&ld);
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

void do_layout(struct req *r, osdo_object *o)
{
    if (!osdo_is_a(o, OsdFolderClass)) { reply_error(r->c, r->id, "bad request", "not a folder"); return; }
    osd_state *st = OsdObject_data(o)->state;
    osd_cstate *cs = st ? osd_state_class(&st, "OsdFolder", false) : NULL;
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
        const osd_prop *p = &cs->props.p[i];
        if (strncmp(p->key, "pos:", 4)) continue;
        snprintf(j->ids[j->n], sizeof j->ids[0], "%s", p->key + 4);
        j->vals[j->n++] = strndup((const char *)p->val, p->len);
    }
    if (!j->n) { layout_done(j); return; }   /* nothing placed: no need to read the folder */
    osdo_defer(loop, "layout", layout_work, layout_done, j);
}

/* {op:"arrange", target:FOLDER}: forget the stored positions; the GUI
 * arranges the icons (and may place them again). */
void do_arrange(struct req *r, osdo_object *o)
{
    if (!osdo_is_a(o, OsdFolderClass)) { reply_error(r->c, r->id, "bad request", "not a folder"); return; }
    osd_state *st = OsdObject_data(o)->state;
    osd_cstate *cs = st ? osd_state_class(&st, "OsdFolder", false) : NULL;
    char **keys = NULL;
    size_t n = 0;
    for (size_t i = 0; cs && i < cs->props.n; i++)
        if (!strncmp(cs->props.p[i].key, "pos:", 4)
            && !(cs->props.p[i].len && ((const char *)cs->props.p[i].val)[cs->props.p[i].len - 1] == 'L')) {   /* (locked: kept) */
            keys = realloc(keys, (n + 1) * sizeof *keys);
            keys[n++] = strdup(cs->props.p[i].key);
        }
    for (size_t i = 0; i < n; i++) { osd_state_remove(o, "OsdFolder", keys[i]); free(keys[i]); }
    free(keys);
    if (n) { OsdObject_osdSaveDeferred(o); OsdObject_osdObjectChanged(o); }
    osdo_value v = osdo_v_int((int64_t)n);
    reply_ok_object(r, o, &v);
}

static void place_work(void *arg)
{
    struct place_job *j = arg;
    osd_load(j->folder, false, &j->fld);
    for (size_t i = 0; i < j->n; i++) osd_load(j->targets[i], true, &j->items[i]);
}

static void place_done(void *arg)
{
    struct place_job *j = arg;
    osdo_object *f = j->fld.err ? NULL : osd_object_from_loaded(&j->fld);
    if (!j->c) {   /* placing items a job dropped: nobody waits for a reply */
        for (size_t i = 0; f && osdo_is_a(f, OsdFolderClass) && i < j->n; i++) {
            osdo_object *o = j->items[i].err ? NULL : osd_object_from_loaded(&j->items[i]);
            if (o) OsdFolder_osdSetIconPos(f, o, j->x[i], j->y[i]);
        }
        if (f && osdo_is_a(f, OsdFolderClass)) on_layout_changed(j->folder);
    } else if (!j->c->dead) {
        if (!f || !osdo_is_a(f, OsdFolderClass)) reply_error(j->c, j->id, "failed", "the folder cannot be loaded");
        else {
            cbuf b;
            cb_init(&b);
            cb_map(&b, 3);
            cb_text(&b, "id"); cb_int(&b, j->id);
            cb_text(&b, "ok"); cb_bool(&b, true);
            cb_text(&b, "result");
            cb_array(&b, j->n);
            for (size_t i = 0; i < j->n; i++) {
                osdo_object *o = j->items[i].err ? NULL : osd_object_from_loaded(&j->items[i]);
                bool ok = o && OsdFolder_osdSetIconPos(f, o, j->x[i], j->y[i]);
                if (ok && j->lock && j->lock[i] >= 0) ok = OsdFolder_osdSetIconLock(f, o, j->lock[i] == 1);
                if (ok) cb_text(&b, OsdObject_data(o)->id);
                else cb_null(&b);
            }
            send_frame(j->c, &b);
            cb_free(&b);
        }
    }
    osd_loaded_free(&j->fld);
    for (size_t i = 0; i < j->n; i++) { osd_loaded_free(&j->items[i]); free(j->targets[i]); }
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
    osdo_defer(loop, "place dropped items", place_work, place_done, j);
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
    osdo_defer(loop, "place icons", place_work, place_done, j);
}

void vmove_work(void *arg)
{
    struct vmove_job *j = arg;
    osd_loaded ld, dst, from;
    osd_load(j->target, false, &ld);
    if (j->del) {
        if (ld.err || !ld.central) j->err = ENOENT;
        else {
            char fid[OSD_ID_LEN + 8];
            snprintf(fid, sizeof fid, "id:%s", ld.folder_id);
            osd_load(fid, false, &from);
            if (!from.err) snprintf(j->from_folder, sizeof j->from_folder, "%s", from.path);
            osd_loaded_free(&from);
            j->err = osd_central_delete(ld.id);
        }
        osd_loaded_free(&ld);
        return;
    }
    osd_load(j->dest, true, &dst);
    if (ld.err || !ld.central) j->err = ENOENT;
    else if (dst.err || !S_ISDIR(dst.st.st_mode) || !dst.id[0]) j->err = ENOTDIR;
    else {
        char fid[OSD_ID_LEN + 8];
        snprintf(fid, sizeof fid, "id:%s", ld.folder_id);
        osd_load(fid, false, &from);
        if (!from.err) snprintf(j->from_folder, sizeof j->from_folder, "%s", from.path);
        osd_loaded_free(&from);
        j->err = osd_central_move(ld.id, dst.vol, dst.id, j->copy, j->new_id);
        if (!j->err && j->trash) {   /* where it was, for Restore */
            char loc[OSD_ID_LEN + 8], key[PATH_MAX + 128];
            snprintf(loc, sizeof loc, "vobj:%s", j->new_id);
            osd_db_key(key, sizeof key, loc, "WPS", "origin");
            osd_db_put(db, dst.vol, key, 's', ld.folder_id, OSD_ID_LEN);
        }
    }
    osd_loaded_free(&ld);
    osd_loaded_free(&dst);
}

void vmove_done(void *arg)
{
    struct vmove_job *j = arg;
    if (!j->err && j->del) {
        osd_central_forget(j->target + 5);
        if (j->from_folder[0]) on_central_change(j->from_folder, j->target + 5, 'd');
    } else if (!j->err) {
        if (!j->copy && j->from_folder[0]) on_central_change(j->from_folder, j->target + 5, 'd');
        on_central_change(j->dest, j->new_id, 'c');
    }
    if (!j->c->dead) {
        if (j->err) reply_error(j->c, j->id, "failed", strerror(j->err));
        else {
            char a[OSD_ID_LEN + 8];
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
    osd_load(j->as_title ? j->path : j->newpath, false, &j->ld);
}

static void rename_done(void *arg)
{
    struct rename_job *j = arg;
    if (!j->c->dead) {
        osdo_object *o = j->err || j->ld.err ? NULL : osd_object_from_loaded(&j->ld);
        if (!o && j->err == EEXIST) reply_error(j->c, j->id, "exists", "an object with that name is already there");
        else if (!o) reply_error(j->c, j->id, "failed", strerror(j->err ? j->err : j->ld.err));
        else {
            if (j->as_title || strchr(j->name, '\n')) OsdObject_osdSetTitle(o, j->name);
            else if (OsdObject_data(o)->title) OsdObject_osdSetTitle(o, NULL);   /* the title follows the name */
            struct req fake = { .c = j->c, .id = j->id };
            osdo_value v = osdo_v_bool(!j->as_title);   /* result: was the file renamed? */
            reply_ok_object(&fake, o, &v);
        }
    }
    osd_loaded_free(&j->ld);
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
    if (osd_templates_dir()[0] && !strcmp(path, osd_templates_dir())) return true;
    return osd_folder(path);
}

/* The folders wpsd finds by their paths (the desktop and Templates are
 * the user's: user-dirs.dirs says where they are). */
bool osd_folder(const char *path)
{
    if (!path || !*path) return false;
    char d[7][PATH_MAX + 8];
    system_dir(d[0], sizeof d[0]);
    computer_dirs(d[1], sizeof d[1], d[2], sizeof d[2]);
    data_dir("Programs", d[3], sizeof d[3]);
    setup_dir(d[4], sizeof d[4]);
    osd_trash_home(d[5], sizeof d[5]);
    strncat(d[5], "/files", sizeof d[5] - strlen(d[5]) - 1);
    autostart_dir(d[6], sizeof d[6]);
    for (int i = 0; i < 7; i++) if (!strcmp(path, d[i])) return true;
    return false;
}

void do_rename(struct req *r, osdo_object *o)
{
    const char *name = cv_text(&r->msg, "name");
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) > NAME_MAX) {
        reply_error(r->c, r->id, "bad request", "a name must be non-empty, without '/', and not . or ..");
        return;
    }
    if (!osdo_is_a(o, OsdFileSystemClass) || own_folder(r->tgt.path)) {
        /* programs and abstract objects, and WPS's own folders (found by
         * their paths: renamed, they would be lost): the title */
        OsdObject_osdSetTitle(o, name);
        reply_ok_object(r, o, NULL);
        return;
    }
    struct rename_job *j = calloc(1, sizeof *j);
    j->c = r->c; r->c->refs++;
    j->id = r->id;
    snprintf(j->path, sizeof j->path, "%s", r->tgt.path);
    snprintf(j->name, sizeof j->name, "%s", name);
    osdo_defer(loop, "rename", rename_work, rename_done, j);
}

static void tmpl_flushed(void *arg) { osdo_defer(loop, "from template", tmpl_work, tmpl_done, arg); }

void do_from_template(struct req *r, osdo_object *o)
{
    const char *folder = cv_text(&r->msg, "folder"), *title = cv_text(&r->msg, "title");
    /* A template: marked as one, or anything in the templates folder. */
    char *makes = NULL;
    osd_state_get_str(o, "WPS", "makes", &makes);
    bool tpl = OsdObject_data(o)->is_template || (makes && *makes) || (r->tgt.path[0] && osd_in_templates(r->tgt.path));
    if (!tpl) { free(makes); reply_error(r->c, r->id, "bad request", "not a template"); return; }
    if (!folder) { free(makes); reply_error(r->c, r->id, "bad request", "from_template needs a folder"); return; }
    char *t = OsdObject_osdQueryTitle(o);
    if (makes && *makes) {
        /* A built-in template (Folder, Data File, Program): a new, empty
         * object of the class it makes, titled like the template. */
        osdo_class *mc = osdo_find_class(makes);
        if (!mc || mc->is_meta || !osdo_subclass_of(mc, OsdObjectClass) || osdo_subclass_of(mc, OsdShadowClass))
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
    osd_objects_flush(tmpl_flushed, j);   /* the template's latest settings saved first: the copy has them */
}

static void create_work(void *arg)
{
    struct create_req *r = arg;
    osdo_class *c = osdo_find_class(r->cls);   /* the registry is read-only after start-up */
    if (osdo_subclass_of(c, OsdAbstractClass) && !osdo_subclass_of(c, OsdProgramClass)) {
        osd_loaded fld;
        osd_load(r->folder, true, &fld);
        bool central = !fld.err && fld.id[0] && !osd_folder_keeps_files(&fld);
        if (central) {
            char id[OSD_ID_LEN + 1];
            r->err = osd_central_create(fld.vol, fld.id, r->cls, r->title, NULL, id);
            if (!r->err) {
                snprintf(r->path, sizeof r->path, "vobj:%s", id);
                osd_load(r->path, false, &r->ld);
                r->central = true;
            }
        }
        osd_loaded_free(&fld);
        if (central) return;
    }
    for (int n = 1; n < 100; n++) {
        osd_object_filename(r->folder, r->cls, r->title, n, r->path, sizeof r->path);
        int e;
        if (osdo_subclass_of(c, OsdFolderClass)) e = mkdir(r->path, 0777) == 0 ? 0 : errno;
        else if (osdo_subclass_of(c, OsdDataFileClass)) {
            int fd = open(r->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
            e = fd >= 0 ? 0 : errno;
            if (fd >= 0) close(fd);
        } else {
            e = osd_object_file_create(r->path, r->cls, r->title);
        }
        if (e == 0) { osd_load(r->path, false, &r->ld); return; }
        if (e != EEXIST) { r->err = e; return; }
    }
    r->err = EEXIST;
}

static void create_done(void *arg)
{
    struct create_req *r = arg;
    if (!r->c->dead) {
        osdo_object *o = r->err || r->ld.err ? NULL : osd_object_from_loaded(&r->ld);
        if (!o) {
            reply_error(r->c, r->id, "failed", strerror(r->err ? r->err : r->ld.err));
        } else {
            /* A file or folder is titled by its name (renaming its icon renames
             * it): a title of its own would hide a numbered name, "Folder (2)". */
            if (r->title[0] && !osdo_is_a(o, OsdFileSystemClass)) OsdObject_osdSetTitle(o, r->title);
            if (r->command[0] && osdo_is_a(o, OsdProgramClass)) OsdProgram_osdSetCommand(o, r->command);
            if (r->central && osd_on_central_change) osd_on_central_change(r->folder, r->path + 5, 'c');
            char *sound = fire_event_for(r->c, o, osd_object_path(o), "object.create");   /* its actions (When made) */
            struct req fake = { .c = r->c, .id = r->id };
            reply_ok_object_sound(&fake, o, NULL, sound);
            free(sound);
        }
    }
    osd_loaded_free(&r->ld);
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
    osdo_defer(loop, "create object", create_work, create_done, r);
}

/* ---- the built-in templates (Folder, Data File, Program), made once in
 * the templates folder as central objects: other programs (GNOME's "New
 * Document") do not see them. Not made again once the user deletes them. */
void builtin_templates_work(void *arg)
{
    (void)arg;
    const char *td = osd_templates_dir();
    if (!td[0]) return;
    if (mkdir(td, 0755) != 0 && errno != EEXIST) return;
    osd_loaded fl;
    osd_load(td, true, &fl);
    if (fl.err || !fl.id[0]) { osd_loaded_free(&fl); return; }
    char loc[OSD_ID_LEN + 16], key[PATH_MAX + 128];
    snprintf(loc, sizeof loc, "templates:%s", fl.id);
    osd_props done;
    osd_props_init(&done);
    int64_t v = 0;
    bool made = osd_db_get_class(db, fl.vol, loc, "WPS", &done) > 0 && osd_props_int(&done, "builtin", &v) && v;
    osd_props_free(&done);
    if (!made) {
        static const char *const builtins[][2] = { { "OsdFolder", "Folder" }, { "OsdDataFile", "Data File" }, { "OsdProgram", "Program" } };
        for (size_t i = 0; i < 3; i++) {
            char id[OSD_ID_LEN + 1], vl[64];
            if (osd_central_create(fl.vol, fl.id, "OsdAbstract", builtins[i][1], NULL, id)) continue;
            snprintf(vl, sizeof vl, "vobj:%s", id);
            osd_db_key(key, sizeof key, vl, "WPS", "makes");
            osd_db_put(db, fl.vol, key, 's', builtins[i][0], (uint32_t)strlen(builtins[i][0]));
            unsigned char one[8] = { 1 };
            osd_db_key(key, sizeof key, vl, "OsdObject", "template");
            osd_db_put(db, fl.vol, key, 'i', one, 8);
        }
        unsigned char one[8] = { 1 };
        osd_db_key(key, sizeof key, loc, "WPS", "builtin");
        osd_db_put(db, fl.vol, key, 'i', one, 8);
    }
    osd_loaded_free(&fl);
}
