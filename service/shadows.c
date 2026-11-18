#define _GNU_SOURCE
#include "shadows.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "WPFolder.h"
#include "WPObject.h"
#include "wps_state.h"

static wpo_loop *loop;
static wps_db *db;

void wps_shadows_init(wpo_loop *l, wps_db *d)
{
    loop = l;
    db = d;
}

bool wps_in_trash(const char *path)
{
    return strstr(path, "/.local/share/Trash/files/") || strstr(path, "/.Trash-") || strstr(path, "/.Trash/");
}

static void split_path(const char *path, char *dir, size_t dn, const char **name)
{
    snprintf(dir, dn, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = 0;
    else if (slash) dir[1] = 0;
    const char *n = strrchr(path, '/');
    *name = n ? n + 1 : path;
}

/* Replace (or create) the symlink at `link` so it points to `target`,
 * atomically: a new symlink under a temporary name, renamed over. */
static int write_symlink(const char *link, const char *target, bool replace)
{
    char dir[PATH_MAX], tmp[PATH_MAX + 64];
    const char *name;
    split_path(link, dir, sizeof dir, &name);
    char id[WPS_ID_LEN + 1];
    wps_new_id(id);
    snprintf(tmp, sizeof tmp, "%s/" WPS_SHADOW_TMP "%.8s", dir, id);
    if (symlink(target, tmp) < 0) return -1;
    int rc = replace ? rename(tmp, link) : renameat2(AT_FDCWD, tmp, AT_FDCWD, link, RENAME_NOREPLACE);
    int e = errno;
    if (rc < 0) unlink(tmp);
    errno = e;
    return rc;
}

static bool link_path(const char *vol, const char *rel, char *out, size_t n)
{
    wps_volume v;
    if (!wps_volume_for_uuid(vol, &v)) return false;
    snprintf(out, n, "%s%s%s", v.mnt, strcmp(v.mnt, "/") ? "/" : "", rel);
    return true;
}

/* ------------------------------------------------------------ creating */

struct create_job {
    char target[PATH_MAX], folder[PATH_MAX];
    wps_loaded tld, fld;            /* the target and the folder, loaded */
    char link[PATH_MAX];
    char error[128];
    void (*done)(void *, const char *, const char *);
    void *arg;
};

static void create_work(void *arg)
{
    struct create_job *j = arg;
    wps_load(j->target, true, &j->tld);   /* a shadow stores something about its target: it needs an ID */
    wps_load(j->folder, false, &j->fld);
    if (j->tld.err || j->fld.err) { snprintf(j->error, sizeof j->error, "%s", strerror(j->tld.err ? j->tld.err : j->fld.err)); return; }
    if (!S_ISDIR(j->fld.st.st_mode)) { snprintf(j->error, sizeof j->error, "not a folder"); return; }
    if (!j->tld.id[0]) { snprintf(j->error, sizeof j->error, "target has no ID (excluded path?)"); return; }
    const char *slash = strrchr(j->tld.path, '/');
    const char *base = slash ? slash + 1 : j->tld.path;
    /* Never overwrite: "name", then "name (2)", "name (3)", ... */
    for (int k = 1; k < 100; k++) {
        if (k == 1) snprintf(j->link, sizeof j->link, "%s/%s", j->fld.path, base);
        else snprintf(j->link, sizeof j->link, "%s/%s (%d)", j->fld.path, base, k);
        if (write_symlink(j->link, j->tld.path, false) == 0) {
            wps_volume v;
            if (wps_volume_for_path(j->link, &v))
                wps_db_shadow_put(db, v.uuid, wps_volume_relpath(&v, j->link), j->tld.id, j->tld.vol, j->tld.rel);
            return;
        }
        if (errno != EEXIST) { snprintf(j->error, sizeof j->error, "%s", strerror(errno)); j->link[0] = 0; return; }
    }
    snprintf(j->error, sizeof j->error, "no free name");
    j->link[0] = 0;
}

static void create_done(void *arg)
{
    struct create_job *j = arg;
    if (!j->error[0] && j->link[0]) {
        /* The record on the folder, so other users' services can maintain it. */
        wpo_object *folder = wps_object_from_loaded(&j->fld);
        if (folder) {
            const char *name = strrchr(j->link, '/') + 1;
            char key[NAME_MAX + 16];
            snprintf(key, sizeof key, "shadow:%s", name);
            wps_state_put_str(folder, "WPFolder", key, j->tld.id);
            WPObject_wpSaveDeferred(folder);
        }
        j->done(j->arg, j->link, NULL);
    } else {
        j->done(j->arg, NULL, j->error[0] ? j->error : "failed");
    }
    wps_loaded_free(&j->tld);
    wps_loaded_free(&j->fld);
    free(j);
}

void wps_shadow_create(const char *target, const char *folder,
                       void (*done)(void *, const char *, const char *), void *arg)
{
    struct create_job *j = calloc(1, sizeof *j);
    snprintf(j->target, sizeof j->target, "%s", target);
    snprintf(j->folder, sizeof j->folder, "%s", folder);
    j->done = done;
    j->arg = arg;
    wpo_defer(loop, "create shadow", create_work, create_done, j);
}

/* -------------------------------------------------- following targets */

enum skind { S_MOVED, S_PREFIX, S_DELETED, S_LEARN };

struct sjob {
    enum skind kind;
    char id[WPS_ID_LEN + 1];
    char a[PATH_MAX], b[PATH_MAX];      /* new path; or old and new prefix; or the folder */
    char (*names)[NAME_MAX + 1];        /* S_LEARN: link names */
    char (*ids)[WPS_ID_LEN + 1];        /* S_LEARN: their targets */
    size_t n;
};

struct rewrite_ctx { struct sjob *j; int done; };

static void rewrite_one(const char *lv, const char *lr, const char *tid, const char *tv, const char *tr, void *user)
{
    struct rewrite_ctx *c = user;
    struct sjob *j = c->j;
    char link[PATH_MAX], newtarget[PATH_MAX];
    if (!link_path(lv, lr, link, sizeof link)) return;   /* its volume is not mounted */
    if (j->kind == S_PREFIX) {
        char oldtarget[PATH_MAX];
        if (!link_path(tv, tr, oldtarget, sizeof oldtarget)) return;
        size_t l = strlen(j->a);
        if (strncmp(oldtarget, j->a, l) != 0) return;
        snprintf(newtarget, sizeof newtarget, "%s%s", j->b, oldtarget + l);
    } else {
        snprintf(newtarget, sizeof newtarget, "%s", j->a);
    }
    struct stat st;
    if (lstat(link, &st) < 0 || !S_ISLNK(st.st_mode)) { wps_db_shadow_del(db, lv, lr); return; }
    char cur[PATH_MAX];
    ssize_t n = readlink(link, cur, sizeof cur - 1);
    cur[n < 0 ? 0 : n] = 0;
    if (j->kind == S_DELETED) {
        /* Only a managed shadow is removed, and only if it still points
         * where the target was (the user may have repointed it). */
        if (n > 0 && access(cur, F_OK) != 0) unlink(link);
        wps_db_shadow_del(db, lv, lr);
        c->done++;
        return;
    }
    if (strcmp(cur, newtarget) == 0) return;
    char dir[PATH_MAX];
    const char *name;
    split_path(link, dir, sizeof dir, &name);
    if (access(dir, W_OK) != 0) return;   /* another user who can write the folder will */
    if (write_symlink(link, newtarget, true) == 0) {
        wps_volume v;
        if (wps_volume_for_path(newtarget, &v))
            wps_db_shadow_put(db, lv, lr, tid, v.uuid, wps_volume_relpath(&v, newtarget));
        c->done++;
    }
}

static void sjob_work(void *arg)
{
    struct sjob *j = arg;
    struct rewrite_ctx c = { j, 0 };
    if (j->kind == S_LEARN) {
        wps_volume fv;
        if (!wps_volume_for_path(j->a, &fv)) return;
        for (size_t i = 0; i < j->n; i++) {
            char link[PATH_MAX + NAME_MAX + 2], tv[WPS_VOLID_MAX] = "", tr[PATH_MAX] = "";
            snprintf(link, sizeof link, "%s/%s", j->a, j->names[i]);
            struct stat st;
            if (lstat(link, &st) < 0 || !S_ISLNK(st.st_mode)) continue;   /* stale record */
            wps_db_get_location(db, j->ids[i], tv, sizeof tv, tr, sizeof tr);
            wps_db_shadow_put(db, fv.uuid, wps_volume_relpath(&fv, link), j->ids[i], tv, tr);
        }
        return;
    }
    if (j->kind == S_PREFIX) {
        wps_volume v;
        if (!wps_volume_for_path(j->a, &v)) return;
        wps_db_shadows_under(db, v.uuid, wps_volume_relpath(&v, j->a), rewrite_one, &c);
        return;
    }
    wps_db_shadows_for(db, j->id, rewrite_one, &c);
}

static void sjob_done(void *arg)
{
    struct sjob *j = arg;
    free(j->names);
    free(j->ids);
    free(j);
}

static void start(struct sjob *j) { wpo_defer(loop, "maintain shadows", sjob_work, sjob_done, j); }

void wps_shadows_target_moved(const char *id, const char *newpath)
{
    if (!id || !id[0] || strstr(newpath, "/" WPS_SHADOW_TMP)) return;
    struct sjob *j = calloc(1, sizeof *j);
    j->kind = S_MOVED;
    snprintf(j->id, sizeof j->id, "%s", id);
    snprintf(j->a, sizeof j->a, "%s", newpath);
    start(j);
}

void wps_shadows_prefix_moved(const char *oldpath, const char *newpath)
{
    struct sjob *j = calloc(1, sizeof *j);
    j->kind = S_PREFIX;
    snprintf(j->a, sizeof j->a, "%s", oldpath);
    snprintf(j->b, sizeof j->b, "%s", newpath);
    start(j);
}

void wps_shadows_target_deleted(const char *id)
{
    if (!id || !id[0]) return;
    struct sjob *j = calloc(1, sizeof *j);
    j->kind = S_DELETED;
    snprintf(j->id, sizeof j->id, "%s", id);
    start(j);
}

void wps_shadows_folder_loaded(const char *folder, const wps_loaded *ld)
{
    for (int c = 0; c < ld->nc; c++) {
        if (strcmp(ld->c[c].cls, "WPFolder") != 0) continue;
        const wps_props *p = &ld->c[c].props;
        struct sjob *j = NULL;
        for (size_t i = 0; i < p->n; i++) {
            if (strncmp(p->p[i].key, "shadow:", 7) != 0 || p->p[i].type != 's' || p->p[i].len != WPS_ID_LEN) continue;
            if (!j) {
                j = calloc(1, sizeof *j);
                j->kind = S_LEARN;
                snprintf(j->a, sizeof j->a, "%s", folder);
                j->names = calloc(p->n, sizeof *j->names);
                j->ids = calloc(p->n, sizeof *j->ids);
            }
            snprintf(j->names[j->n], NAME_MAX + 1, "%s", p->p[i].key + 7);
            memcpy(j->ids[j->n], p->p[i].val, WPS_ID_LEN);
            j->ids[j->n][WPS_ID_LEN] = 0;
            j->n++;
        }
        if (j) start(j);
    }
}
