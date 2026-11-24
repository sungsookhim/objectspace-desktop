/* osdd: Drives, the Trash Can (putting back, emptying), Drives kept in step. */
#define _GNU_SOURCE
#include "osdd.h"

/* ---- Drives (v0.2-design 5) */

void put_drive(cbuf *b, const osd_drive_info *d)
{
    char title[200];
    osd_drive_title(d, title, sizeof title);
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
    osd_udisks_call(k->uuid, k->action, k->pass, drive_call_done, k);
}

void drive_call(const char *uuid, const char *action, const char *pass, osd_udisks_fn done, void *arg)
{
    osd_drive_info d;
    bool unmounts = strcmp(action, "mount") && strcmp(action, "unlock");
    if (!unmounts || !osd_udisks_find(uuid, &d) || !d.mount[0]) { osd_udisks_call(uuid, action, pass, done, arg); return; }
    struct drive_call *k = calloc(1, sizeof *k);
    snprintf(k->uuid, sizeof k->uuid, "%s", uuid);
    snprintf(k->action, sizeof k->action, "%s", action);
    snprintf(k->mount, sizeof k->mount, "%s", d.mount);
    k->pass = pass ? strdup(pass) : NULL;
    k->done = done;
    k->arg = arg;
    osdo_defer(loop, "release volume", drive_release_work, drive_release_done, k);
}

/* A drive's action, answered when udisks2 is done: {mount} or an error
 * whose code says why ("not authorized", "busy", ...). */

void drive_done(void *arg, int err, const char *msg, const char *mount)
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
void drive_target_work(void *arg)
{
    struct drive_req *q = arg;
    osd_loaded ld;
    osd_load(q->uuid, false, &ld);   /* (uuid holds the target until here) */
    q->uuid[0] = 0;
    if (!ld.err) {
        const char *loc = ld.locator[0] ? ld.locator : ld.path;
        osd_props ps;
        osd_props_init(&ps);
        char *u = NULL;
        if (!strcmp(ld.obj_class, "OsdDrive") && osd_db_get_class(db, ld.vol, loc, "OsdDrive", &ps) > 0 && osd_props_str(&ps, "uuid", &u))
            snprintf(q->uuid, sizeof q->uuid, "%s", u);
        else if (ld.shadow_of[0]) {   /* a shadow of a drive */
            char tl[OSD_ID_LEN + 8];
            snprintf(tl, sizeof tl, "id:%s", ld.shadow_of);
            osd_loaded tg;
            osd_load(tl, false, &tg);
            osd_props_free(&ps);
            osd_props_init(&ps);
            if (!tg.err && !strcmp(tg.obj_class, "OsdDrive") &&
                osd_db_get_class(db, tg.vol, tg.locator[0] ? tg.locator : tg.path, "OsdDrive", &ps) > 0 && osd_props_str(&ps, "uuid", &u))
                snprintf(q->uuid, sizeof q->uuid, "%s", u);
            osd_loaded_free(&tg);
        }
        free(u);
        osd_props_free(&ps);
    }
    osd_loaded_free(&ld);
}

void drive_target_done(void *arg)
{
    struct drive_req *q = arg;
    if (!q->uuid[0]) { drive_done(q, ENODEV, "not a drive", NULL); return; }
    drive_call(q->uuid, q->action, q->pass, drive_done, q);
}

/* For OsdDrive's menu and notebook (classes/osd_drive_hook.h). */
static void human_size(uint64_t v, char *out, size_t n)
{
    const char *u[] = { "bytes", "kB", "MB", "GB", "TB", "PB" };
    double s = (double)v;
    int i = 0;
    while (s >= 1000 && i < 5) { s /= 1000; i++; }
    if (i && s < 10) snprintf(out, n, "%.1f %s", s, u[i]);
    else snprintf(out, n, "%.0f %s", s, u[i]);
}

bool drive_query(const char *uuid, osd_drive_state *st, bool details)
{
    osd_drive_info d;
    if (!osd_udisks_find(uuid, &d)) return false;
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

void restore_work(void *arg)
{
    struct restore_job *j = arg;
    if (!strncmp(j->path, "vobj:", 5)) {
        /* a central object (a shadow): back into the folder it came from */
        osd_loaded ld;
        osd_load(j->path, false, &ld);
        osd_props ps;
        osd_props_init(&ps);
        char *origin = NULL;
        if (ld.err || !ld.central) j->err = ENOENT;
        else if (osd_db_get_class(db, ld.vol, ld.locator, "Objectspace", &ps) <= 0 || !osd_props_str(&ps, "origin", &origin))
            j->err = ENOENT;
        else {
            char fid[OSD_ID_LEN + 8];
            snprintf(fid, sizeof fid, "id:%s", origin);
            osd_loaded of, tf;
            osd_load(fid, false, &of);
            char tfid[OSD_ID_LEN + 8];
            snprintf(tfid, sizeof tfid, "id:%s", ld.folder_id);
            osd_load(tfid, false, &tf);
            if (of.err || !S_ISDIR(of.st.st_mode)) j->err = ENOENT;   /* its folder is gone */
            else {
                j->err = osd_central_move(ld.id, of.vol, of.id, false, j->new_id);
                if (!j->err) {
                    char key[PATH_MAX + 128];
                    osd_db_key(key, sizeof key, ld.locator, "Objectspace", "origin");
                    osd_db_delete(db, of.vol, key);
                    snprintf(j->out, sizeof j->out, "%s", of.path);
                    if (!tf.err) snprintf(j->from, sizeof j->from, "%s", tf.path);
                }
            }
            osd_loaded_free(&of);
            osd_loaded_free(&tf);
        }
        free(origin);
        osd_props_free(&ps);
        osd_loaded_free(&ld);
        return;
    }
    j->err = j->path[0] == '/' ? -osd_trash_restore(j->path, j->out, sizeof j->out) : EINVAL;
}

void restore_done(void *arg)
{
    struct restore_job *j = arg;
    if (!j->err && !strncmp(j->path, "vobj:", 5)) {
        osd_central_forget(j->path + 5);
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

static void empty_collect(const char *vol, const char *vid, void *user)
{
    (void)vol;
    struct empty_job *j = user;
    j->gone = realloc(j->gone, (j->ngone + 1) * sizeof *j->gone);
    snprintf(j->gone[j->ngone++], sizeof j->gone[0], "%s", vid);
}

void empty_work(void *arg)
{
    struct empty_job *j = arg;
    osd_loaded fl;
    osd_load(j->files, false, &fl);
    if (!fl.err && fl.id[0]) osd_central_in(fl.vol, fl.id, empty_collect, j);
    osd_loaded_free(&fl);
    for (size_t k = 0; k < j->ngone; k++) osd_central_delete(j->gone[k]);
}

void empty_done(void *arg)
{
    struct empty_job *j = arg;
    for (size_t k = 0; k < j->ngone; k++) {
        osd_central_forget(j->gone[k]);
        on_central_change(j->files, j->gone[k], 'd');
    }
    char errm[256] = "";
    bool any_file = false;
    DIR *d = opendir(j->files);
    for (struct dirent *e; d && (e = readdir(d)) && !any_file;) any_file = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    if (d) closedir(d);
    int64_t job = 0;
    if (any_file) {
        int64_t sel = osd_select(j->files, true, NULL, 0);
        job = sel < 0 ? -1 : osd_job_start("delete", sel, NULL, errm, sizeof errm);
    }
    if (!j->c->dead) {
        if (job < 0) reply_error(j->c, j->id, "failed", errm[0] ? errm : "the trash cannot be read");
        else reply_int(j->c, j->id, job);   /* (0: no files, nothing more to do) */
    }
    client_unref(j->c);
    free(j->gone);
    free(j);
}

/* ---- Drives kept in step (v0.2-design 3, 5): a OsdDrive central object
 * per volume in Drives (the root file system's in Computer), found by
 * its UUID (OsdDrive "uuid"). A drive that goes away is parked (moved to
 * a folder ID no folder has) and brought back when it returns, keeping
 * its object ID and so its place. Titles follow the label unless the
 * user renamed it (OsdDrive "autotitle": the title Objectspace gave). */

void computer_dirs(char *comp, size_t cn, char *drives, size_t dn)
{
    char sys[PATH_MAX];
    system_dir(sys, sizeof sys);
    snprintf(comp, cn, "%s/Storages", sys);   /* (0.2's "Computer": moved at the start) */
    snprintf(drives, dn, "%s/Drives", comp);
}

static bool drives_syncing, drives_again;

static void drives_collect(const char *vol, const char *id, void *user)
{
    struct drives_sync *j = user;
    char loc[OSD_ID_LEN + 8], key[PATH_MAX + 128];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    osd_props ps;
    osd_props_init(&ps);
    char *v = NULL;
    osd_db_key(key, sizeof key, loc, "Objectspace", "class");
    bool drive = osd_db_get_class(db, vol, loc, "Objectspace", &ps) > 0 && osd_props_str(&ps, "class", &v) && !strcmp(v, "OsdDrive");
    free(v);
    osd_props_free(&ps);
    if (!drive) return;
    struct drive_obj o;
    memset(&o, 0, sizeof o);
    snprintf(o.id, sizeof o.id, "%s", id);
    o.where = j->collecting;
    osd_props_init(&ps);
    if (osd_db_get_class(db, vol, loc, "OsdDrive", &ps) > 0) {
        v = NULL;
        if (osd_props_str(&ps, "uuid", &v)) snprintf(o.uuid, sizeof o.uuid, "%s", v);
        free(v);
        v = NULL;
        if (osd_props_str(&ps, "autotitle", &v)) snprintf(o.autotitle, sizeof o.autotitle, "%s", v);
        free(v);
    }
    osd_props_free(&ps);
    osd_props_init(&ps);
    v = NULL;
    if (osd_db_get_class(db, vol, loc, "OsdObject", &ps) > 0 && osd_props_str(&ps, "title", &v)) snprintf(o.title, sizeof o.title, "%s", v);
    free(v);
    osd_props_free(&ps);
    j->objs = realloc(j->objs, (j->nobjs + 1) * sizeof *j->objs);
    j->objs[j->nobjs++] = o;
}

void drive_event(struct drives_sync *j, int where, const char *id, char kind)
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
    char loc[OSD_ID_LEN + 8], key[PATH_MAX + 128];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    osd_db_key(key, sizeof key, loc, cls, prop);
    osd_db_put(db, vol, key, 's', v, (uint32_t)strlen(v));
}

static void drives_sync_work(void *arg)
{
    struct drives_sync *j = arg;
    osd_loaded c, d;
    osd_load(j->comp, true, &c);
    osd_load(j->drives, true, &d);
    bool ok = !c.err && !d.err && c.id[0] && d.id[0] && S_ISDIR(c.st.st_mode) && S_ISDIR(d.st.st_mode) && !strcmp(c.vol, d.vol);
    if (ok) {
        snprintf(j->vol, sizeof j->vol, "%s", d.vol);
        snprintf(j->cid, sizeof j->cid, "%s", c.id);
        snprintf(j->did, sizeof j->did, "%s", d.id);
    }
    osd_loaded_free(&c);
    osd_loaded_free(&d);
    if (!ok) return;   /* no Computer (the user removed it, or first start not yet) */
    j->collecting = 0;
    osd_central_in(j->vol, j->did, drives_collect, j);
    j->collecting = 1;
    osd_central_in(j->vol, j->cid, drives_collect, j);
    j->collecting = 2;
    osd_central_in(j->vol, DRIVES_PARKED, drives_collect, j);
    for (size_t i = 0; i < j->n; i++) {
        const osd_drive_info *v = &j->ds[i];
        int where = !strcmp(v->kind, "root") ? 1 : 0;
        char title[256];
        osd_drive_title(v, title, sizeof title);
        struct drive_obj *o = NULL;
        for (size_t k = 0; k < j->nobjs && !o; k++)
            if (!j->objs[k].seen && !strcmp(j->objs[k].uuid, v->uuid)) o = &j->objs[k];
        char newid[OSD_ID_LEN + 1];
        if (!o) {
            if (osd_central_create(j->vol, where ? j->cid : j->did, "OsdDrive", title, NULL, newid) != 0) continue;
            put_str(j->vol, newid, "OsdDrive", "uuid", v->uuid);
            put_str(j->vol, newid, "OsdDrive", "autotitle", title);
            drive_event(j, where, newid, 'c');
            j->changed = true;
            continue;
        }
        o->seen = true;
        if (o->where != where) {
            if (osd_central_move(o->id, j->vol, where ? j->cid : j->did, false, newid) == 0) {
                drive_event(j, o->where, o->id, 'd');
                drive_event(j, where, o->id, 'c');
                j->changed = true;
            }
        }
        /* its label changed (and the user did not rename it) */
        if (strcmp(o->autotitle, title) && (!o->title[0] || !strcmp(o->title, o->autotitle))) {
            put_str(j->vol, o->id, "OsdObject", "title", title);
            put_str(j->vol, o->id, "OsdDrive", "autotitle", title);
            drive_event(j, where, o->id, 't');
            j->changed = true;
        }
    }
    for (size_t k = 0; k < j->nobjs; k++) {
        struct drive_obj *o = &j->objs[k];
        if (o->seen || o->where == 2) continue;
        char newid[OSD_ID_LEN + 1];
        if (osd_central_move(o->id, j->vol, DRIVES_PARKED, false, newid) == 0) {   /* gone: parked */
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
        osd_central_forget(j->ev[i].id);
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

void drives_changed(void)
{
    if (drives_syncing) { drives_again = true; return; }
    drives_syncing = true;
    struct drives_sync *j = calloc(1, sizeof *j);
    j->n = osd_udisks_list(&j->ds);
    computer_dirs(j->comp, sizeof j->comp, j->drives, sizeof j->drives);
    osdo_defer(loop, "drives sync", drives_sync_work, drives_sync_done, j);
}


