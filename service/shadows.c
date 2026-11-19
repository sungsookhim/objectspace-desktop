/* Shadows as WPS object files (decided 2026-11-18; see shadows.h). */
#define _GNU_SOURCE
#include "shadows.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "WPObject.h"
#include "attrs.h"
#include "volumes.h"
#include "wps_state.h"

static wpo_loop *loop;
static wps_db *db;
void (*wps_on_central_change)(const char *folder, const char *id, char kind);

void wps_shadows_init(wpo_loop *l, wps_db *d)
{
    loop = l;
    db = d;
}

bool wps_in_trash(const char *path)
{
    if (strstr(path, "/.local/share/Trash/files/") || strstr(path, "/.Trash-") || strstr(path, "/.Trash/")) return true;
    const char *dh = getenv("XDG_DATA_HOME");   /* the home trash, wherever it is */
    if (!dh || !*dh) return false;
    size_t l = strlen(dh);
    return !strncmp(path, dh, l) && !strncmp(path + l, "/Trash/files/", 13);
}

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static bool file_path(const char *vol, const char *rel, char *out, size_t n)
{
    wps_volume v;
    if (!wps_volume_for_uuid(vol, &v)) return false;
    snprintf(out, n, "%s%s%s", v.mnt, strcmp(v.mnt, "/") ? "/" : "", rel);
    return true;
}

/* Read a shadow file: its target's ID and last known path. */
bool wps_shadow_read(const char *path, char id[WPS_ID_LEN + 1], char *target_path, size_t tn)
{
    char buf[4096];
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    if (!strstr(buf, "[WPS Object]")) return false;
    const char *cls = strstr(buf, "\nClass=WPShadow"), *t = strstr(buf, "\nTarget="), *tp = strstr(buf, "\nTargetPath=");
    if (!cls || !t) return false;
    t += 8;
    size_t l = strcspn(t, "\r\n");
    if (l != WPS_ID_LEN) return false;
    memcpy(id, t, WPS_ID_LEN);
    id[WPS_ID_LEN] = 0;
    if (target_path) {
        target_path[0] = 0;
        if (tp) {
            tp += 12;
            snprintf(target_path, tn, "%.*s", (int)strcspn(tp, "\r\n"), tp);
        }
    }
    return true;
}

static int write_shadow(int fd, const char *id, const char *target_path)
{
    char buf[PATH_MAX + 256];
    int n = snprintf(buf, sizeof buf, "[WPS Object]\nVersion=1\nClass=WPShadow\nTarget=%s\nTargetPath=%s\n", id, target_path);
    if (lseek(fd, 0, SEEK_SET) != 0 || write(fd, buf, (size_t)n) != n) return -1;
    return ftruncate(fd, n);
}

/* Whoever can write a folder may maintain the shadows in it (design:
 * Shadows maintained by any user): a shadow file we cannot write is
 * replaced by a new one, renamed over it, with its WPS settings copied. */
static int replace_shadow(const char *file, const char *id, const char *target_path)
{
    char dir[PATH_MAX], tmp[PATH_MAX + 64], nid[WPS_ID_LEN + 1];
    snprintf(dir, sizeof dir, "%s", file);
    char *sl = strrchr(dir, '/');
    if (!sl) return -1;
    *sl = 0;
    if (access(dir, W_OK) != 0) return -1;   /* another user who can write the folder will */
    wps_new_id(nid);
    snprintf(tmp, sizeof tmp, "%s/" WPS_SHADOW_TMP "%.8s", dir, nid);
    int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
    if (out < 0) return -1;
    int rc = write_shadow(out, id, target_path);
    int in = open(file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    char names[8192];
    ssize_t nl = in >= 0 ? flistxattr(in, names, sizeof names) : -1;
    for (ssize_t o = 0; nl > 0 && o < nl; o += (ssize_t)strlen(names + o) + 1) {
        if (strncmp(names + o, "user.wps.", 9)) continue;
        char v[65536];
        ssize_t vl = fgetxattr(in, names + o, v, sizeof v);
        if (vl >= 0) fsetxattr(out, names + o, v, (size_t)vl, 0);
    }
    if (in >= 0) close(in);
    close(out);
    if (rc == 0) rc = rename(tmp, file);
    if (rc != 0) unlink(tmp);
    return rc;
}

/* Remember a shadow file in this service's index (from any thread). */
static void index_put(const char *shadow, const char *id)
{
    wps_volume sv;
    char tv[WPS_VOLID_MAX] = "", tr[PATH_MAX] = "";
    if (!wps_volume_for_path(shadow, &sv)) return;
    wps_db_get_location(db, id, tv, sizeof tv, tr, sizeof tr);
    wps_db_shadow_put(db, sv.uuid, wps_volume_relpath(&sv, shadow), id, tv, tr);
}

/* ------------------------------------------------------------ creating */

struct create_job {
    char target[PATH_MAX], folder[PATH_MAX];
    wps_loaded tld, fld;
    char file[PATH_MAX];      /* the shadow file, or "vobj:<id>" for a central shadow */
    char error[128];
    void (*done)(void *, const char *, const char *);
    void *arg;
};

static void create_work(void *arg)
{
    struct create_job *j = arg;
    wps_load(j->target, true, &j->tld);   /* a shadow stores its target's ID: it needs one */
    if (j->tld.err) { snprintf(j->error, sizeof j->error, "%s", strerror(j->tld.err)); return; }
    if (!j->tld.id[0]) { snprintf(j->error, sizeof j->error, "target has no ID (excluded path?)"); return; }
    /* A shadow of a shadow is a shadow of its original (as in OS/2). */
    const char *tid = j->tld.shadow_of[0] ? j->tld.shadow_of : j->tld.id;
    wps_load(j->folder, true, &j->fld);   /* central objects are kept by their folder's ID */
    if (j->fld.err || !S_ISDIR(j->fld.st.st_mode)) { snprintf(j->error, sizeof j->error, "not a folder"); return; }
    if (!wps_folder_keeps_files(&j->fld) && j->fld.id[0]) {
        char id[WPS_ID_LEN + 1];
        int e = wps_central_create(j->fld.vol, j->fld.id, "WPShadow", NULL, tid, id);
        if (e) { snprintf(j->error, sizeof j->error, "%s", strerror(e)); return; }
        snprintf(j->file, sizeof j->file, "vobj:%s", id);
        return;
    }
    if (j->tld.central) { snprintf(j->error, sizeof j->error, "a shadow file of a central object is not supported yet"); return; }
    const char *slash = strrchr(j->tld.path, '/');
    const char *base = slash ? slash + 1 : j->tld.path;
    char stem[NAME_MAX - 24];
    snprintf(stem, sizeof stem, "%s", base);
    if (ends_with(stem, ".wpobj")) stem[strlen(stem) - 6] = 0;   /* a shadow of an abstract object */
    /* Never overwrite: "name.wpobj", then "name (2).wpobj", ... */
    for (int k = 1; k < 1000; k++) {
        if (k == 1) snprintf(j->file, sizeof j->file, "%s/%s.wpobj", j->folder, stem);
        else snprintf(j->file, sizeof j->file, "%s/%s (%d).wpobj", j->folder, stem, k);
        int fd = open(j->file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0 && errno == EEXIST) continue;
        if (fd < 0) { snprintf(j->error, sizeof j->error, "%s", strerror(errno)); j->file[0] = 0; return; }
        int rc = write_shadow(fd, tid, j->tld.path);
        close(fd);
        if (rc < 0) { unlink(j->file); snprintf(j->error, sizeof j->error, "cannot write the shadow"); j->file[0] = 0; return; }
        index_put(j->file, tid);
        return;
    }
    snprintf(j->error, sizeof j->error, "no free name");
    j->file[0] = 0;
}

static void create_done(void *arg)
{
    struct create_job *j = arg;
    if (!j->error[0] && j->file[0]) {
        if (!strncmp(j->file, "vobj:", 5) && wps_on_central_change) wps_on_central_change(j->folder, j->file + 5, 'c');
        j->done(j->arg, j->file, NULL);
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

enum skind { S_MOVED, S_PREFIX, S_DELETED, S_LEARN, S_SCAN };

struct sjob {
    enum skind kind;
    char id[WPS_ID_LEN + 1];
    char a[PATH_MAX], b[PATH_MAX];      /* new path; old and new prefix; or a shadow file (S_LEARN) */
    char (*gone)[WPS_ID_LEN + 1];       /* S_DELETED: central shadows removed, and their folders */
    char (*gone_in)[PATH_MAX];
    size_t ngone;
};

static void central_shadow_gone(const char *vol, const char *id, void *user)
{
    struct sjob *j = user;
    wps_loaded ld, fl;
    char loc[WPS_ID_LEN + 8];
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_load(loc, false, &ld);
    char folder[PATH_MAX] = "";
    if (!ld.err && ld.folder_id[0]) {
        char fid[WPS_ID_LEN + 8];
        snprintf(fid, sizeof fid, "id:%s", ld.folder_id);
        wps_load(fid, false, &fl);
        if (!fl.err) snprintf(folder, sizeof folder, "%s", fl.path);
        wps_loaded_free(&fl);
    }
    wps_loaded_free(&ld);
    wps_db_vobj_delete(db, vol, id);
    j->gone = realloc(j->gone, (j->ngone + 1) * sizeof *j->gone);
    j->gone_in = realloc(j->gone_in, (j->ngone + 1) * sizeof *j->gone_in);
    snprintf(j->gone[j->ngone], sizeof j->gone[0], "%s", id);
    snprintf(j->gone_in[j->ngone], sizeof j->gone_in[0], "%s", folder);
    j->ngone++;
}

struct rewrite_ctx { struct sjob *j; int done; };

/* One indexed shadow file: bring its target hint up to date, or remove it
 * if its target is gone. The ID inside the file is what makes it a shadow;
 * the hint only saves a lookup and helps other computers. */
static void rewrite_one(const char *lv, const char *lr, const char *tid, const char *tv, const char *tr, void *user)
{
    struct rewrite_ctx *c = user;
    struct sjob *j = c->j;
    char file[PATH_MAX], newtarget[PATH_MAX];
    if (!file_path(lv, lr, file, sizeof file)) return;   /* its volume is not mounted */
    char fid[WPS_ID_LEN + 1], hint[PATH_MAX];
    if (!wps_shadow_read(file, fid, hint, sizeof hint) || strcmp(fid, tid) != 0) {
        wps_db_shadow_del(db, lv, lr);   /* moved away or no longer a shadow of this: learnt again when seen */
        return;
    }
    if (j->kind == S_DELETED) {
        unlink(file);   /* the original is really gone (not in the trash): its shadows go too */
        wps_db_shadow_del(db, lv, lr);
        c->done++;
        return;
    }
    if (j->kind == S_PREFIX) {
        char oldtarget[PATH_MAX];
        if (!file_path(tv, tr, oldtarget, sizeof oldtarget)) return;
        size_t l = strlen(j->a);
        if (strncmp(oldtarget, j->a, l) != 0) return;
        snprintf(newtarget, sizeof newtarget, "%s%s", j->b, oldtarget + l);
    } else {
        snprintf(newtarget, sizeof newtarget, "%s", j->a);
    }
    if (strcmp(hint, newtarget) == 0) return;
    int rc = -1;
    int fd = open(file, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);   /* in place: the same inode keeps any settings */
    if (fd >= 0) {
        rc = write_shadow(fd, tid, newtarget);
        close(fd);
    } else {
        rc = replace_shadow(file, tid, newtarget);   /* someone else's file in a folder we may write */
    }
    if (rc == 0) {
        wps_volume v;
        if (wps_volume_for_path(newtarget, &v)) wps_db_shadow_put(db, lv, lr, tid, v.uuid, wps_volume_relpath(&v, newtarget));
        c->done++;
    }
}

static void sjob_work(void *arg)
{
    struct sjob *j = arg;
    struct rewrite_ctx c = { j, 0 };
    if (j->kind == S_LEARN) {
        char id[WPS_ID_LEN + 1];
        if (wps_shadow_read(j->a, id, NULL, 0)) index_put(j->a, id);
        return;
    }
    if (j->kind == S_SCAN) {   /* a folder in view: the shadows in it, whoever made them */
        DIR *d = opendir(j->a);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_type != DT_REG && de->d_type != DT_UNKNOWN) continue;
            if (!ends_with(de->d_name, ".wpobj")) continue;
            char p[PATH_MAX + NAME_MAX + 2], id[WPS_ID_LEN + 1];
            snprintf(p, sizeof p, "%s/%s", j->a, de->d_name);
            if (wps_shadow_read(p, id, NULL, 0)) index_put(p, id);
        }
        if (d) closedir(d);
        return;
    }
    if (j->kind == S_PREFIX) {
        wps_volume v;
        if (!wps_volume_for_path(j->a, &v)) return;
        wps_db_shadows_under(db, v.uuid, wps_volume_relpath(&v, j->a), rewrite_one, &c);
        return;
    }
    wps_db_shadows_for(db, j->id, rewrite_one, &c);
    if (j->kind == S_DELETED) wps_db_vobjs_shadowing(db, j->id, central_shadow_gone, j);
}

static void sjob_done(void *arg)
{
    struct sjob *j = arg;
    for (size_t i = 0; i < j->ngone; i++)
        if (j->gone_in[i][0] && wps_on_central_change) wps_on_central_change(j->gone_in[i], j->gone[i], 'd');
    free(j->gone);
    free(j->gone_in);
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

void wps_shadows_folder_seen(const char *folder)
{
    struct sjob *j = calloc(1, sizeof *j);
    j->kind = S_SCAN;
    snprintf(j->a, sizeof j->a, "%s", folder);
    start(j);
}

void wps_shadows_file_seen(const char *path)
{
    if (!ends_with(path, ".wpobj") || strstr(path, "/" WPS_SHADOW_TMP)) return;
    struct sjob *j = calloc(1, sizeof *j);
    j->kind = S_LEARN;
    snprintf(j->a, sizeof j->a, "%s", path);
    start(j);
}
