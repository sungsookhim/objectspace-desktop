#define _GNU_SOURCE
#include "objects.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "WPDataFile.h"
#include "WPFileSystem.h"
#include "WPFolder.h"
#include "WPObject.h"
#include "shadows.h"
#include "tracker.h"
#include "WPShadow.h"
#include "WPAbstract.h"
#include "WPProgram.h"
#include "wps_state.h"

#define LOCK_WAIT_MS 200

static const wps_env *env;
static wps_db *db;
static wpo_loop *loop;
static int save_delay_ms;

/* ------------------------------------------------------------- volumes */

static pthread_mutex_t vol_mu = PTHREAD_MUTEX_INITIALIZER;
static wps_volumes vols;

void wps_volumes_replace(wps_volumes *vs)
{
    pthread_mutex_lock(&vol_mu);
    wps_volumes_free(&vols);
    vols = *vs;
    vs->v = NULL;
    vs->n = 0;
    pthread_mutex_unlock(&vol_mu);
}

bool wps_volume_for_path(const char *path, wps_volume *out)
{
    pthread_mutex_lock(&vol_mu);
    const wps_volume *v = wps_volumes_of_path(&vols, path);
    if (v) *out = *v;
    pthread_mutex_unlock(&vol_mu);
    return v != NULL;
}

bool wps_volume_for_uuid(const char *uuid, wps_volume *out)
{
    pthread_mutex_lock(&vol_mu);
    const wps_volume *v = wps_volumes_by_uuid(&vols, uuid);
    if (v) *out = *v;
    pthread_mutex_unlock(&vol_mu);
    return v != NULL;
}

/* ------------------------------------------------------------- objects */

/* Records of file-backed objects. A linear table is enough for M2/M3;
 * M4 replaces it with an index (design: Scale). */
static sobj **objs;
static size_t nobjs, capobjs;
static size_t inflight;          /* saves being written by workers */

struct flush_waiter { void (*done)(void *); void *arg; struct flush_waiter *next; };
static struct flush_waiter *waiters;

static int timer_fd = -1;
static bool timer_armed;

static sobj *by_object(wpo_object *o)
{
    for (size_t i = 0; i < nobjs; i++)
        if (objs[i]->o == o) return objs[i];
    return NULL;
}

static sobj *by_inode(dev_t dev, ino_t ino)
{
    for (size_t i = 0; i < nobjs; i++)
        if (objs[i]->dev == dev && objs[i]->ino == ino) return objs[i];
    return NULL;
}

size_t wps_objects_count(void) { return nobjs; }
size_t wps_sobj_count(void) { return nobjs; }
sobj *wps_sobj_at(size_t i) { return i < nobjs ? objs[i] : NULL; }
sobj *wps_sobj_of(wpo_object *o) { return by_object(o); }
void (*wps_on_saved)(sobj *s);

sobj *wps_sobj_by_path(const char *path)
{
    if (!path || !path[0]) return NULL;   /* central objects have no path */
    for (size_t i = 0; i < nobjs; i++)
        if (strcmp(objs[i]->path, path) == 0) return objs[i];
    return NULL;
}

static sobj *by_central(const char *id)
{
    for (size_t i = 0; i < nobjs; i++)
        if (objs[i]->central && !strcmp(WPObject_data(objs[i]->o)->id, id)) return objs[i];
    return NULL;
}

sobj *wps_sobj_by_id(const char *id)
{
    for (size_t i = 0; i < nobjs; i++)
        if (id[0] && strcmp(WPObject_data(objs[i]->o)->id, id) == 0) return objs[i];
    return NULL;
}

/* ---- excluded paths */

static char **excluded;
static size_t nexcluded;

static void excluded_load(void)
{
    static const char *defaults[] = { ".git", "node_modules", "/nix/store", "~/.cache", ".wps", NULL };
    const char *home = getenv("HOME");
    char line[PATH_MAX], path[PATH_MAX + 32];
    for (int i = 0; defaults[i]; i++) {
        excluded = realloc(excluded, (nexcluded + 1) * sizeof *excluded);
        if (defaults[i][0] == '~' && home) {
            snprintf(line, sizeof line, "%s%s", home, defaults[i] + 1);
            excluded[nexcluded++] = strdup(line);
        } else {
            excluded[nexcluded++] = strdup(defaults[i]);
        }
    }
    snprintf(path, sizeof path, "%s/excluded", env->config_dir);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        excluded = realloc(excluded, (nexcluded + 1) * sizeof *excluded);
        excluded[nexcluded++] = strdup(line);
    }
    if (f) fclose(f);
}

static bool root_session;
void wps_set_root_session(bool on) { root_session = on; }

bool wps_path_untracked(const char *path)
{
    if (wps_path_excluded(path)) return true;
    if (!root_session) return false;
    /* Root never follows other users' homes on its own; it may still open
     * something there explicitly. */
    return !strncmp(path, "/home/", 6) || !strcmp(path, "/home") || !strncmp(path, "/var/home/", 10);
}

/* A pattern with a slash is a path prefix; otherwise it matches any one
 * path component ("node_modules" excludes every node_modules folder). */
bool wps_path_excluded(const char *path)
{
    for (size_t i = 0; i < nexcluded; i++) {
        const char *x = excluded[i];
        size_t l = strlen(x);
        if (strchr(x, '/')) {
            if (strncmp(path, x, l) == 0 && (path[l] == '/' || path[l] == 0)) return true;
            continue;
        }
        for (const char *p = path; (p = strstr(p, x)); p += l)
            if ((p == path || p[-1] == '/') && (p[l] == '/' || p[l] == 0)) return true;
    }
    return false;
}

size_t wps_objects_pending(void)
{
    size_t n = inflight;
    for (size_t i = 0; i < nobjs; i++) n += objs[i]->queued;
    return n;
}

const char *wps_object_path(wpo_object *o)
{
    sobj *s = by_object(o);
    return s && !s->central ? s->path : NULL;
}

/* ------------------------------------------------------- loading (worker) */

static void handle_hex(const char *path, char *out, size_t n)
{
    _Alignas(struct file_handle) unsigned char buf[sizeof(struct file_handle) + MAX_HANDLE_SZ];
    struct file_handle *fh = (struct file_handle *)buf;
    const unsigned char *bytes = buf + offsetof(struct file_handle, f_handle);
    int mid;
    fh->handle_bytes = MAX_HANDLE_SZ;
    out[0] = 0;
    if (name_to_handle_at(AT_FDCWD, path, fh, &mid, 0) < 0) return;
    size_t o = (size_t)snprintf(out, n, "fh:%x:", fh->handle_type);
    for (unsigned i = 0; i < fh->handle_bytes && o + 3 < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%02x", bytes[i]);
}

static wps_props *class_props(wps_loaded *ld, const char *cls)
{
    for (int i = 0; i < ld->nc; i++)
        if (strcmp(ld->c[i].cls, cls) == 0) return &ld->c[i].props;
    if (ld->nc == WPS_MAX_CLASSES) return NULL;
    snprintf(ld->c[ld->nc].cls, sizeof ld->c[ld->nc].cls, "%s", cls);
    wps_props_init(&ld->c[ld->nc].props);
    return &ld->c[ld->nc++].props;
}

static void overlay_prop(const char *cls, const char *prop, char type, const void *val, uint32_t len, void *user)
{
    wps_props *p = class_props(user, cls);
    if (p) wps_props_set(p, prop, type, val, len);
}

void wps_loaded_free(wps_loaded *ld)
{
    for (int i = 0; i < ld->nc; i++) wps_props_free(&ld->c[i].props);
    ld->nc = 0;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* The first matching "Key=value" line within the given [group]. */
static bool ini_value(const char *text, const char *group, const char *key, char *out, size_t n)
{
    bool in = false;
    size_t kl = strlen(key);
    for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (line[0] == '[') { in = len == strlen(group) && strncmp(line, group, len) == 0; continue; }
        if (in && len > kl && strncmp(line, key, kl) == 0 && line[kl] == '=') {
            snprintf(out, n, "%.*s", (int)(len - kl - 1), line + kl + 1);
            return true;
        }
    }
    return false;
}

static void read_object_file(wps_loaded *ld)
{
    bool desk = ends_with(ld->path, ".desktop"), wpobj = ends_with(ld->path, ".wpobj");
    if (!desk && !wpobj) return;
    char buf[8192];
    int fd = open(ld->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0) return;
    buf[n] = 0;
    if (desk) {
        char type[64] = "";
        ini_value(buf, "[Desktop Entry]", "Type", type, sizeof type);
        if (strcmp(type, "Application") != 0) return;   /* links and directories stay data files */
        snprintf(ld->obj_class, sizeof ld->obj_class, "WPProgram");
        ini_value(buf, "[Desktop Entry]", "Name", ld->desk_name, sizeof ld->desk_name);
        ini_value(buf, "[Desktop Entry]", "Exec", ld->desk_exec, sizeof ld->desk_exec);
    } else if (!ini_value(buf, "[WPS Object]", "Class", ld->obj_class, sizeof ld->obj_class)) {
        snprintf(ld->obj_class, sizeof ld->obj_class, "WPAbstract");
    } else if (!strcmp(ld->obj_class, "WPShadow")) {
        char id[WPS_ID_LEN + 8] = "";
        ini_value(buf, "[WPS Object]", "Target", id, sizeof id);
        if (strlen(id) == WPS_ID_LEN) memcpy(ld->shadow_of, id, WPS_ID_LEN + 1);
        ini_value(buf, "[WPS Object]", "TargetPath", ld->link_target, sizeof ld->link_target);
    }
}

void wps_object_filename(const char *folder, const char *cls, const char *title, int n, char *out, size_t len)
{
    char safe[NAME_MAX - 16];
    snprintf(safe, sizeof safe, "%s", title && *title ? title : cls);
    for (char *p = safe; *p; p++)
        if (*p == '/' || (unsigned char)*p < 0x20) *p = '-';
    if (safe[0] == '.') safe[0] = '_';
    wpo_class *c = wpo_find_class(cls);
    const char *ext = !c ? "" : wpo_subclass_of(c, WPProgramClass) ? ".desktop" :
                      wpo_subclass_of(c, WPAbstractClass) ? ".wpobj" : "";
    if (n <= 1) snprintf(out, len, "%s/%s%s", folder, safe, ext);
    else snprintf(out, len, "%s/%s (%d)%s", folder, safe, n, ext);
}

int wps_object_file_create(const char *path, const char *cls, const char *title)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);   /* never overwrite */
    if (fd < 0) return errno;
    wpo_class *c = wpo_find_class(cls);
    if (c && wpo_subclass_of(c, WPProgramClass))
        dprintf(fd, "[Desktop Entry]\nType=Application\nName=%s\nExec=\n", title ? title : "Program");
    else if (c && wpo_subclass_of(c, WPAbstractClass))
        dprintf(fd, "[WPS Object]\nVersion=1\nClass=%s\n", cls);
    close(fd);
    return 0;
}

/* Keep a program's .desktop file in step with its title and command, in
 * place (same inode, so its attributes stay). */
static void update_desktop_file(const char *path, const char *name, const char *exec)
{
    char buf[16384];
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n < 0) { close(fd); return; }
    buf[n] = 0;
    char out[sizeof buf + 2048];
    size_t o = 0;
    bool in = false, did_name = false, did_exec = false;
    for (char *line = buf; line && *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (line[0] == '[') in = strncmp(line, "[Desktop Entry]", len) == 0 && len == 15;
        if (in && name && !did_name && strncmp(line, "Name=", 5) == 0) {
            o += (size_t)snprintf(out + o, sizeof out - o, "Name=%s\n", name);
            did_name = true;
        } else if (in && exec && !did_exec && strncmp(line, "Exec=", 5) == 0) {
            o += (size_t)snprintf(out + o, sizeof out - o, "Exec=%s\n", exec);
            did_exec = true;
        } else {
            o += (size_t)snprintf(out + o, sizeof out - o, "%.*s\n", (int)len, line);
        }
        if (o >= sizeof out) break;
        line = end ? end + 1 : NULL;
    }
    if (o < sizeof out && lseek(fd, 0, SEEK_SET) == 0 && write(fd, out, o) == (ssize_t)o) {
        if (ftruncate(fd, (off_t)o) < 0) { /* leave as written */ }
    }
    close(fd);
}

/* A central object: its rows are all there is. */
static void load_central(const char *id, wps_loaded *ld)
{
    if (strlen(id) != WPS_ID_LEN || !wps_db_vobj_vol(db, id, ld->vol, sizeof ld->vol)) { ld->err = ENOENT; return; }
    ld->central = true;
    memcpy(ld->id, id, WPS_ID_LEN + 1);
    snprintf(ld->locator, sizeof ld->locator, "vobj:%s", id);
    ld->owned = ld->writable = true;   /* this user's own rows */
    ld->xattr_ok = false;
    wps_db_get_locator(db, ld->vol, ld->locator, overlay_prop, ld);
    wps_props *w = class_props(ld, "WPS");
    char *v = NULL;
    if (w && wps_props_str(w, "class", &v)) snprintf(ld->obj_class, sizeof ld->obj_class, "%s", v);
    free(v);
    v = NULL;
    if (w && wps_props_str(w, "folder", &v) && strlen(v) == WPS_ID_LEN) memcpy(ld->folder_id, v, WPS_ID_LEN + 1);
    free(v);
    v = NULL;
    if (w && wps_props_str(w, "target", &v) && strlen(v) == WPS_ID_LEN) memcpy(ld->shadow_of, v, WPS_ID_LEN + 1);
    free(v);
    if (!ld->obj_class[0]) ld->err = ENOENT;
}

void wps_load(const char *target, bool assign_id, wps_loaded *ld)
{
    memset(ld, 0, sizeof *ld);
    if (strncmp(target, "vobj:", 5) == 0) { load_central(target + 5, ld); return; }
    if (strncmp(target, "id:", 3) == 0) {
        /* An object named by ID: find where it was last seen. */
        char vol[WPS_VOLID_MAX], rel[PATH_MAX];
        wps_volume v;
        if (wps_db_get_location(db, target + 3, vol, sizeof vol, rel, sizeof rel) < 0 ||
            !wps_volume_for_uuid(vol, &v)) {
            load_central(target + 3, ld);   /* or a central object */
            return;
        }
        snprintf(ld->path, sizeof ld->path, "%s%s%s", v.mnt, strcmp(v.mnt, "/") ? "/" : "", rel);
    } else {
        snprintf(ld->path, sizeof ld->path, "%s", target);
    }
    if (lstat(ld->path, &ld->st) < 0) { ld->err = errno; return; }

    wps_volume v;
    if (!wps_volume_for_path(ld->path, &v)) { ld->err = ENODEV; return; }
    snprintf(ld->vol, sizeof ld->vol, "%s", v.uuid);
    snprintf(ld->rel, sizeof ld->rel, "%s", wps_volume_relpath(&v, ld->path));
    ld->owned = ld->st.st_uid == geteuid();
    ld->writable = access(ld->path, W_OK) == 0 && !v.ro;
    if (S_ISREG(ld->st.st_mode)) read_object_file(ld);
    if (S_ISLNK(ld->st.st_mode)) {   /* a plain file-system link (shadows are .wpobj files) */
        ssize_t n = readlink(ld->path, ld->link_target, sizeof ld->link_target - 1);
        ld->link_target[n < 0 ? 0 : n] = 0;
    }

    /* Does the filesystem take user attributes? Probe the user namespace
     * itself: FAT, for one, lists security labels but refuses user.*. */
    char probe[1];
    ld->xattr_ok = !S_ISLNK(ld->st.st_mode) &&
                   (lgetxattr(ld->path, "user.wps.probe", probe, 0) >= 0 || errno != ENOTSUP);
    /* Shared state: every user.wps.<Class> attribute on the file. */
    char names[8192];
    ssize_t nl = ld->xattr_ok ? llistxattr(ld->path, names, sizeof names) : -1;
    if (ld->xattr_ok && wps_attr_get_id(ld->path, ld->id)) { /* has an ID */ }
    for (ssize_t o = 0; nl > 0 && o < nl; o += (ssize_t)strlen(names + o) + 1) {
        const char *n = names + o;
        if (strncmp(n, WPS_ATTR_PREFIX, strlen(WPS_ATTR_PREFIX)) != 0 || strcmp(n, WPS_ATTR_ID) == 0) continue;
        wps_props *p = class_props(ld, n + strlen(WPS_ATTR_PREFIX));
        if (p) wps_attr_read(ld->path, n + strlen(WPS_ATTR_PREFIX), p);
    }

    /* The locator for this user's own rows. */
    if (ld->id[0]) snprintf(ld->locator, sizeof ld->locator, "%s", ld->id);
    else if (!ld->xattr_ok) snprintf(ld->locator, sizeof ld->locator, "path:%s", ld->rel);
    else handle_hex(ld->path, ld->locator, sizeof ld->locator);
    if (!ld->locator[0]) snprintf(ld->locator, sizeof ld->locator, "path:%s", ld->rel);

    /* Per-user state on top: overlays, own layouts, state for files without
     * attributes. Its "@id" is the ID this user gave a file it cannot tag. */
    wps_db_get_locator(db, ld->vol, ld->locator, overlay_prop, ld);
    wps_props *op = class_props(ld, "WPObject");
    char *dbid = NULL;
    if (op && wps_props_str(op, "@id", &dbid)) {
        if (!ld->id[0] && strlen(dbid) == WPS_ID_LEN) memcpy(ld->id, dbid, WPS_ID_LEN + 1);
        wps_props_remove(op, "@id");
    }
    free(dbid);

    if (assign_id && !ld->id[0] && !wps_path_excluded(ld->path)) {
        char cand[WPS_ID_LEN + 1];
        wps_new_id(cand);
        if (ld->xattr_ok && ld->writable && wps_attr_assign_id(ld->path, cand, ld->id) == WPS_A_OK) {
            /* stored on the file (another writer's ID wins if it was first) */
        } else {
            char key[PATH_MAX + 128];
            wps_db_key(key, sizeof key, ld->locator, "WPObject", "@id");
            wps_db_put(db, ld->vol, key, 's', cand, WPS_ID_LEN);
            memcpy(ld->id, cand, WPS_ID_LEN + 1);
        }
    }
    if (ld->id[0]) wps_db_set_location(db, ld->id, ld->vol, ld->rel);
}

/* ------------------------------------------------- objects (loop thread) */

static wpo_class *class_for(const wps_loaded *ld)
{
    mode_t m = ld->st.st_mode;
    if (S_ISDIR(m)) return WPFolderClass;
    if (S_ISREG(m) && ld->obj_class[0]) {
        wpo_class *c = wpo_find_class(ld->obj_class);
        return c && !c->is_meta && wpo_subclass_of(c, WPAbstractClass) ? c : WPAbstractClass;
    }
    if (S_ISREG(m)) return WPDataFileClass;
    return WPFileSystemClass;   /* user-made symlinks and special files */
}

static void apply_loaded(sobj *s, wps_loaded *ld)
{
    s->nlink = ld->st.st_nlink;
    s->mtime = ld->st.st_mtim;
    WPObjectData *d = WPObject_data(s->o);
    if (ld->id[0]) memcpy(d->id, ld->id, sizeof d->id);
    for (int i = 0; i < ld->nc; i++) wps_state_load(s->o, ld->c[i].cls, &ld->c[i].props);
    WPObject_wpRestoreProperties(s->o);
    /* A program's .desktop file is the source of its command and default
     * title, so other desktops and WPS agree. */
    if (wpo_is_a(s->o, WPProgramClass)) {
        if (ld->desk_exec[0] && !WPProgram_data(s->o)->command) WPProgram_data(s->o)->command = strdup(ld->desk_exec);
        if (ld->desk_name[0] && !d->title) d->title = strdup(ld->desk_name);
    }
    s->ctime = ld->st.st_ctim;
    snprintf(s->path, sizeof s->path, "%s", ld->path);
    snprintf(s->vol, sizeof s->vol, "%s", ld->vol);
    snprintf(s->rel, sizeof s->rel, "%s", ld->rel);
    snprintf(s->locator, sizeof s->locator, "%s", ld->locator);
    s->xattr_ok = ld->xattr_ok;
    s->writable = ld->writable;
    s->owned = ld->owned;
}

static wpo_object *central_from_loaded(wps_loaded *ld)
{
    sobj *s = by_central(ld->id);
    if (s) {
        if (!s->queued && !s->saving && !WPObject_data(s->o)->dirty) {
            for (int i = 0; i < ld->nc; i++) wps_state_load(s->o, ld->c[i].cls, &ld->c[i].props);
            WPObject_wpRestoreProperties(s->o);
        }
        memcpy(s->folder_id, ld->folder_id, sizeof s->folder_id);
        return s->o;
    }
    if (nobjs == capobjs) {
        size_t ncap = capobjs ? capobjs * 2 : 64;
        sobj **n = realloc(objs, ncap * sizeof *n);
        if (!n) return NULL;
        objs = n;
        capobjs = ncap;
    }
    wpo_class *c = wpo_find_class(ld->obj_class);
    if (!c || c->is_meta || !wpo_subclass_of(c, WPAbstractClass)) c = WPAbstractClass;
    s = calloc(1, sizeof *s);
    s->o = wpo_new(c);
    if (!s->o) { free(s); return NULL; }
    s->central = true;
    memcpy(s->folder_id, ld->folder_id, sizeof s->folder_id);
    if (strcmp(c->name, ld->obj_class)) snprintf(s->declared_class, sizeof s->declared_class, "%s", ld->obj_class);
    WPObjectData *d = WPObject_data(s->o);
    memcpy(d->id, ld->id, sizeof d->id);
    for (int i = 0; i < ld->nc; i++) wps_state_load(s->o, ld->c[i].cls, &ld->c[i].props);
    WPObject_wpRestoreProperties(s->o);
    snprintf(s->vol, sizeof s->vol, "%s", ld->vol);
    snprintf(s->locator, sizeof s->locator, "%s", ld->locator);
    s->writable = s->owned = true;
    objs[nobjs++] = s;
    return s->o;
}

wpo_object *wps_object_from_loaded(wps_loaded *ld)
{
    if (ld->err) return NULL;
    if (ld->central) return central_from_loaded(ld);
    sobj *s = by_inode(ld->st.st_dev, ld->st.st_ino);
    if (!s && S_ISLNK(ld->st.st_mode)) {
        /* A shadow's symlink is replaced when it is rewritten: same object. */
        s = wps_sobj_by_path(ld->path);
        if (s) { s->dev = ld->st.st_dev; s->ino = ld->st.st_ino; }
    }
    if (s) {
        /* Changed on disk since we read it (another process or user)? Reload,
         * unless we have unsaved changes of our own; our save merges then. */
        bool changed = s->ctime.tv_sec != ld->st.st_ctim.tv_sec || s->ctime.tv_nsec != ld->st.st_ctim.tv_nsec ||
                       strcmp(s->path, ld->path) != 0;
        if (changed && !s->queued && !s->saving && !WPObject_data(s->o)->dirty) apply_loaded(s, ld);
        else if (ld->id[0] && !WPObject_data(s->o)->id[0]) memcpy(WPObject_data(s->o)->id, ld->id, WPS_ID_LEN + 1);
        return s->o;
    }
    if (nobjs == capobjs) {
        size_t ncap = capobjs ? capobjs * 2 : 64;
        sobj **n = realloc(objs, ncap * sizeof *n);
        if (!n) return NULL;
        objs = n;
        capobjs = ncap;
    }
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->o = wpo_new(class_for(ld));   /* replacements apply */
    if (!s->o) { free(s); return NULL; }
    s->dev = ld->st.st_dev;
    s->ino = ld->st.st_ino;
    if (wpo_is_a(s->o, WPFileSystemClass)) WPFileSystem_wpSetPath(s->o, ld->path);
    if (ld->obj_class[0] && strcmp(s->o->cls->name, ld->obj_class) != 0 && !wpo_find_class(ld->obj_class))
        snprintf(s->declared_class, sizeof s->declared_class, "%s", ld->obj_class);   /* not installed here */
    apply_loaded(s, ld);
    objs[nobjs++] = s;
    /* Follow it from now on: its folder's entries, and a folder's own. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", ld->path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = 0; wps_tracker_watch(dir); }
    if (S_ISDIR(ld->st.st_mode)) {
        wps_tracker_watch(ld->path);
        wps_shadows_folder_seen(ld->path);   /* the shadows in it, so they follow their targets */
    }
    if (ld->shadow_of[0]) wps_shadows_file_seen(ld->path);   /* index it */
    wps_tracker_object_at(ld->path);
    return s->o;
}

/* ------------------------------------------------------------- saving */

struct cls_change {
    char cls[64];
    wps_props set;
    char **removed;
    size_t nremoved;
};

struct save_job {
    sobj *s;                   /* loop-thread pointer; workers only read the copies */
    bool busy;                 /* a class went to the overlay because the file was locked */
    bool retry_only;           /* move overlay values for busy classes back to the file */
    char path[PATH_MAX], vol[WPS_VOLID_MAX], rel[PATH_MAX], locator[PATH_MAX + 16];
    bool xattr_ok, writable, owned;
    char id[WPS_ID_LEN + 1];          /* in: current ID; out: assigned ID */
    char *desk_name, *desk_exec;      /* a program's .desktop file to update, if set */
    struct cls_change *ch;
    size_t nch;
    int failures;
};

static void db_write_props(struct save_job *j, const char *cls, const wps_props *set,
                           char *const *removed, size_t nremoved)
{
    char key[PATH_MAX + 128];
    for (size_t i = 0; set && i < set->n; i++) {
        wps_db_key(key, sizeof key, j->locator, cls, set->p[i].key);
        if (wps_db_put(db, j->vol, key, set->p[i].type, set->p[i].val, set->p[i].len) < 0) j->failures++;
    }
    for (size_t i = 0; i < nremoved; i++) {
        wps_db_key(key, sizeof key, j->locator, cls, removed[i]);
        wps_db_delete(db, j->vol, key);
    }
}

static void save_work(void *arg)
{
    struct save_job *j = arg;
    /* Lazy identity: storing something is what gives an object its ID. */
    if (!j->id[0]) {
        char cand[WPS_ID_LEN + 1];
        wps_new_id(cand);
        if (j->xattr_ok && j->writable && wps_attr_assign_id(j->path, cand, j->id) == WPS_A_OK) {
            snprintf(j->locator, sizeof j->locator, "%s", j->id);
        } else {
            char key[PATH_MAX + 128];
            wps_db_key(key, sizeof key, j->locator, "WPObject", "@id");
            wps_db_put(db, j->vol, key, 's', cand, WPS_ID_LEN);
            memcpy(j->id, cand, WPS_ID_LEN + 1);
        }
    }
    for (size_t c = 0; c < j->nch; c++) {
        struct cls_change *ch = &j->ch[c];
        /* Icon positions in a folder the user does not own are per user. */
        wps_props personal;
        wps_props_init(&personal);
        if (strcmp(ch->cls, "WPFolder") == 0 && !j->owned) {
            for (size_t i = 0; i < ch->set.n;) {
                if (strncmp(ch->set.p[i].key, "pos:", 4) == 0) {
                    wps_props_set(&personal, ch->set.p[i].key, ch->set.p[i].type, ch->set.p[i].val, ch->set.p[i].len);
                    wps_props_remove(&ch->set, ch->set.p[i].key);
                } else {
                    i++;
                }
            }
            db_write_props(j, ch->cls, &personal, NULL, 0);
        }
        wps_props_free(&personal);
        if (ch->set.n == 0 && ch->nremoved == 0) continue;
        wps_astatus st = WPS_A_NOTSUP;
        if (j->xattr_ok && j->writable)
            st = wps_attr_update(j->path, ch->cls, &ch->set, (const char *const *)ch->removed, LOCK_WAIT_MS, NULL, NULL);
        if (st == WPS_A_OK && j->retry_only) {
            /* Back in the shared attribute: drop the overlay copies. */
            char key[PATH_MAX + 128];
            for (size_t i = 0; i < ch->set.n; i++) {
                wps_db_key(key, sizeof key, j->locator, ch->cls, ch->set.p[i].key);
                wps_db_delete(db, j->vol, key);
            }
        } else if (st != WPS_A_OK && !j->retry_only) {
            /* No permission, no attributes, too big, or lock not obtained:
             * keep it in this user's overlay. A busy file is retried. */
            db_write_props(j, ch->cls, &ch->set, ch->removed, ch->nremoved);
            if (st == WPS_A_BUSY) j->busy = true;
        } else if (st == WPS_A_BUSY) {
            j->busy = true;
        }
    }
    if (j->id[0] && j->path[0]) wps_db_set_location(db, j->id, j->vol, j->rel);   /* not for central objects */
    if ((j->desk_name || j->desk_exec) && j->writable) update_desktop_file(j->path, j->desk_name, j->desk_exec);
}

static void schedule_flush(void);
static void schedule_retry(int ms);

static void save_done(void *arg)
{
    struct save_job *j = arg;
    sobj *s = j->s;
    if (s->forgotten) {
        s->saving = false;
        for (size_t c = 0; c < j->nch; c++) {
            wps_props_free(&j->ch[c].set);
            for (size_t i = 0; i < j->ch[c].nremoved; i++) free(j->ch[c].removed[i]);
            free(j->ch[c].removed);
        }
        free(j->ch);
        free(j);
        inflight--;
        wpo_unref(s->o);
        free(s);
        return;
    }
    if (j->id[0] && !WPObject_data(s->o)->id[0]) memcpy(WPObject_data(s->o)->id, j->id, WPS_ID_LEN + 1);
    snprintf(s->locator, sizeof s->locator, "%s", j->locator);
    s->saving = false;
    free(j->desk_name);
    free(j->desk_exec);
    bool was_retry = j->retry_only, busy = j->busy;
    for (size_t c = 0; c < j->nch; c++) {
        wps_props_free(&j->ch[c].set);
        for (size_t i = 0; i < j->ch[c].nremoved; i++) free(j->ch[c].removed[i]);
        free(j->ch[c].removed);
    }
    free(j->ch);
    free(j);
    inflight--;
    /* A busy file: try again later, backing off up to a minute. */
    if (busy) {
        s->retry = true;
        s->retry_delay_ms = s->retry_delay_ms ? (s->retry_delay_ms * 2 > 60000 ? 60000 : s->retry_delay_ms * 2) : 1000;
        schedule_retry(s->retry_delay_ms);
    } else if (was_retry) {
        s->retry = false;
        s->retry_delay_ms = 0;
    }
    if (!was_retry && wps_on_saved) wps_on_saved(s);
    if (s->queued) schedule_flush();
    if (wps_objects_pending() == 0) {
        struct flush_waiter *w = waiters;
        waiters = NULL;
        while (w) {
            struct flush_waiter *next = w->next;
            w->done(w->arg);
            free(w);
            w = next;
        }
    }
}

/* Snapshot the touched keys of each class, then hand them to a worker. */
static void start_save(sobj *s)
{
    s->queued = false;
    if (s->path[0] && wps_path_excluded(s->path)) {
        /* Excluded paths keep no WPS state (design: Excluded paths). */
        WPObject_wpSaveState(s->o);
        WPObject_data(s->o)->dirty = 0;
        wps_state *st = WPObject_data(s->o)->state;
        for (size_t c = 0; st && c < st->n; c++) wps_state_clear_touched(&st->c[c]);
        return;
    }
    WPObject_wpSaveState(s->o);
    WPObject_data(s->o)->dirty = 0;
    wps_state *st = WPObject_data(s->o)->state;
    struct save_job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->s = s;
    snprintf(j->path, sizeof j->path, "%s", s->path);
    snprintf(j->vol, sizeof j->vol, "%s", s->vol);
    snprintf(j->rel, sizeof j->rel, "%s", s->rel);
    snprintf(j->locator, sizeof j->locator, "%s", s->locator);
    memcpy(j->id, WPObject_data(s->o)->id, sizeof j->id);
    j->xattr_ok = s->xattr_ok;
    j->writable = s->writable;
    j->owned = s->owned;
    j->ch = calloc(st ? st->n + 1 : 1, sizeof *j->ch);
    for (size_t c = 0; st && c < st->n; c++) {
        wps_cstate *cs = &st->c[c];
        if (!cs->ntouched) continue;
        struct cls_change *ch = &j->ch[j->nch++];
        snprintf(ch->cls, sizeof ch->cls, "%s", cs->cls);
        wps_props_init(&ch->set);
        ch->removed = calloc(cs->ntouched + 1, sizeof *ch->removed);
        for (size_t k = 0; k < cs->ntouched; k++) {
            const wps_prop *p = wps_props_get(&cs->props, cs->touched[k]);
            if (p) wps_props_set(&ch->set, p->key, p->type, p->val, p->len);
            else ch->removed[ch->nremoved++] = strdup(cs->touched[k]);
        }
        wps_state_clear_touched(cs);
    }
    if (wpo_is_a(s->o, WPProgramClass) && strlen(s->path) > 8 && !strcmp(s->path + strlen(s->path) - 8, ".desktop")) {
        const char *t = WPObject_data(s->o)->title, *c = WPProgram_data(s->o)->command;
        j->desk_name = t ? strdup(t) : NULL;
        j->desk_exec = c ? strdup(c) : NULL;
    }
    s->saving = true;
    inflight++;
    wpo_defer(loop, "save object", save_work, save_done, j);
}

static int retry_fd = -1;

static void start_retry(sobj *s)
{
    wps_state *st = WPObject_data(s->o)->state;
    struct save_job *j = calloc(1, sizeof *j);
    if (!j || !st) { free(j); s->retry = false; return; }
    j->s = s;
    j->retry_only = true;
    snprintf(j->path, sizeof j->path, "%s", s->path);
    snprintf(j->vol, sizeof j->vol, "%s", s->vol);
    snprintf(j->rel, sizeof j->rel, "%s", s->rel);
    snprintf(j->locator, sizeof j->locator, "%s", s->locator);
    memcpy(j->id, WPObject_data(s->o)->id, sizeof j->id);
    j->xattr_ok = s->xattr_ok;
    j->writable = s->writable;
    j->owned = s->owned;
    j->ch = calloc(st->n + 1, sizeof *j->ch);
    for (size_t c = 0; c < st->n; c++) {
        struct cls_change *ch = &j->ch[j->nch++];
        snprintf(ch->cls, sizeof ch->cls, "%s", st->c[c].cls);
        wps_props_init(&ch->set);
        for (size_t k = 0; k < st->c[c].props.n; k++) {
            const wps_prop *p = &st->c[c].props.p[k];
            if (strncmp(p->key, "pos:", 4) == 0 && !s->owned) continue;   /* stays personal */
            wps_props_set(&ch->set, p->key, p->type, p->val, p->len);
        }
    }
    s->saving = true;
    inflight++;
    wpo_defer(loop, "retry busy save", save_work, save_done, j);
}

static void on_retry(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    uint64_t n;
    ssize_t r = read(fd, &n, sizeof n);
    (void)r;
    for (size_t i = 0; i < nobjs; i++)
        if (objs[i]->retry && !objs[i]->saving && !objs[i]->queued) start_retry(objs[i]);
}

static void schedule_retry(int ms)
{
    struct itimerspec its = { .it_value = { ms / 1000, (long)(ms % 1000) * 1000000L } };
    timerfd_settime(retry_fd, 0, &its, NULL);
}

static void flush_now(void)
{
    for (size_t i = 0; i < nobjs; i++)
        if (objs[i]->queued && !objs[i]->saving) start_save(objs[i]);
}

static void on_timer(int fd, short rev, void *arg)
{
    (void)rev; (void)arg;
    uint64_t n;
    ssize_t r = read(fd, &n, sizeof n);
    (void)r;
    timer_armed = false;
    flush_now();
}

static void schedule_flush(void)
{
    if (timer_armed) return;
    struct itimerspec its = { .it_value = { save_delay_ms / 1000, (long)(save_delay_ms % 1000) * 1000000L } };
    if (!its.it_value.tv_sec && !its.it_value.tv_nsec) its.it_value.tv_nsec = 1;
    timerfd_settime(timer_fd, 0, &its, NULL);
    timer_armed = true;
}

static void on_dirty(wpo_object *o)
{
    sobj *s = by_object(o);
    if (!s) return;
    s->queued = true;
    schedule_flush();   /* written after `save_delay_ms` of quiet (design: Write policy) */
}

void wps_objects_flush(void (*done)(void *), void *arg)
{
    flush_now();
    if (wps_objects_pending() == 0) { done(arg); return; }
    struct flush_waiter *w = calloc(1, sizeof *w);
    w->done = done;
    w->arg = arg;
    w->next = waiters;
    waiters = w;
}

void wps_objects_init(const wps_env *e, wps_db *d, wpo_loop *l, int delay_ms)
{
    env = e;
    db = d;
    loop = l;
    save_delay_ms = delay_ms;
    wps_on_dirty = on_dirty;
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    wpo_loop_watch_fd(loop, timer_fd, POLLIN, on_timer, NULL);
    retry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    wpo_loop_watch_fd(loop, retry_fd, POLLIN, on_retry, NULL);
    excluded_load();
}

/* ---- updates from the change tracker */

struct loc_job { char id[WPS_ID_LEN + 1]; char vol[WPS_VOLID_MAX]; char rel[PATH_MAX]; };

static void loc_work(void *arg)
{
    struct loc_job *j = arg;
    wps_db_set_location(db, j->id, j->vol, j->rel);
}

static void free_arg(void *arg) { free(arg); }

void wps_sobj_set_path(sobj *s, const char *path)
{
    snprintf(s->path, sizeof s->path, "%s", path);
    wps_volume v;
    if (wps_volume_for_path(path, &v)) {
        snprintf(s->vol, sizeof s->vol, "%s", v.uuid);
        snprintf(s->rel, sizeof s->rel, "%s", wps_volume_relpath(&v, path));
    }
    if (wpo_is_a(s->o, WPFileSystemClass)) WPFileSystem_wpSetPath(s->o, path);
    WPObject_wpObjectChanged(s->o);
    const char *id = WPObject_data(s->o)->id;
    wps_shadows_target_moved(id, path);   /* shadows follow, into the trash too */
    if (id[0]) {
        struct loc_job *j = calloc(1, sizeof *j);
        memcpy(j->id, id, sizeof j->id);
        snprintf(j->vol, sizeof j->vol, "%s", s->vol);
        snprintf(j->rel, sizeof j->rel, "%s", s->rel);
        wpo_defer(loop, "location", loc_work, free_arg, j);
    }
}

void wps_sobj_set_inode(sobj *s, const struct stat *st)
{
    s->dev = st->st_dev;
    s->ino = st->st_ino;
    s->nlink = st->st_nlink;
    s->ctime = st->st_ctim;
    s->mtime = st->st_mtim;
    WPObject_wpObjectChanged(s->o);
}

void wps_sobj_forget(sobj *s)
{
    for (size_t i = 0; i < nobjs; i++)
        if (objs[i] == s) {
            objs[i] = objs[--nobjs];
            break;
        }
    if (s->saving) { s->forgotten = true; return; }   /* freed when the save ends */
    wpo_unref(s->o);
    free(s);
}

void wps_sobj_snapshot(sobj *s, wps_loaded *out)
{
    memset(out, 0, sizeof *out);
    WPObject_wpSaveState(s->o);
    wps_state *st = WPObject_data(s->o)->state;
    for (size_t c = 0; st && c < st->n && out->nc < WPS_MAX_CLASSES; c++) {
        snprintf(out->c[out->nc].cls, sizeof out->c[out->nc].cls, "%s", st->c[c].cls);
        wps_props_init(&out->c[out->nc].props);
        for (size_t k = 0; k < st->c[c].props.n; k++) {
            const wps_prop *p = &st->c[c].props.p[k];
            wps_props_set(&out->c[out->nc].props, p->key, p->type, p->val, p->len);
        }
        out->nc++;
    }
    memcpy(out->id, WPObject_data(s->o)->id, sizeof out->id);
}

void wps_objects_shutdown(void)
{
    for (size_t i = 0; i < nobjs; i++) {
        wpo_unref(objs[i]->o);
        free(objs[i]);
    }
    free(objs);
    objs = NULL;
    nobjs = capobjs = 0;
    wps_volumes_free(&vols);
    if (timer_fd >= 0) close(timer_fd);
    if (retry_fd >= 0) close(retry_fd);
    for (size_t i = 0; i < nexcluded; i++) free(excluded[i]);
    free(excluded);
    excluded = NULL;
    nexcluded = 0;
}

/* ------------------------------------------------------ central objects */

int wps_central_create(const char *vol, const char *folder_id, const char *cls, const char *title,
                       const char *target_id, char id[WPS_ID_LEN + 1])
{
    char key[PATH_MAX + 128], loc[64];
    wps_new_id(id);
    snprintf(loc, sizeof loc, "vobj:%s", id);
    wps_db_key(key, sizeof key, loc, "WPS", "folder");
    if (wps_db_put(db, vol, key, 's', folder_id, WPS_ID_LEN) < 0) return EIO;
    wps_db_key(key, sizeof key, loc, "WPS", "class");
    wps_db_put(db, vol, key, 's', cls, (uint32_t)strlen(cls));
    if (target_id && *target_id) {
        wps_db_key(key, sizeof key, loc, "WPS", "target");
        wps_db_put(db, vol, key, 's', target_id, WPS_ID_LEN);
    }
    if (title && *title) {
        wps_db_key(key, sizeof key, loc, "WPObject", "title");
        wps_db_put(db, vol, key, 's', title, (uint32_t)strlen(title));
    }
    return 0;
}

int wps_central_in(const char *vol, const char *folder_id, void (*fn)(const char *vol, const char *id, void *user), void *user)
{
    return wps_db_vobjs_in(db, vol, folder_id, fn, user);
}

int wps_central_move(const char *id, const char *dest_vol, const char *dest_folder_id, bool copy, char new_id[WPS_ID_LEN + 1])
{
    char vol[WPS_VOLID_MAX], key[PATH_MAX + 128], loc[64];
    if (!wps_db_vobj_vol(db, id, vol, sizeof vol)) return ENOENT;
    if (copy) wps_new_id(new_id);
    else memcpy(new_id, id, WPS_ID_LEN + 1);
    if (copy || strcmp(vol, dest_vol)) {
        if (wps_db_vobj_copy(db, vol, id, dest_vol, new_id, !copy) < 0) return EIO;
    }
    snprintf(loc, sizeof loc, "vobj:%s", new_id);
    wps_db_key(key, sizeof key, loc, "WPS", "folder");
    return wps_db_put(db, dest_vol, key, 's', dest_folder_id, WPS_ID_LEN) < 0 ? EIO : 0;
}

bool wps_folder_keeps_files(const wps_loaded *folder)
{
    for (int i = 0; i < folder->nc; i++) {
        if (strcmp(folder->c[i].cls, "WPFolder")) continue;
        int64_t v = 0;
        if (wps_props_int(&folder->c[i].props, "local", &v)) return v != 0;
    }
    return false;
}

int wps_central_delete(const char *id)
{
    char vol[WPS_VOLID_MAX];
    if (!wps_db_vobj_vol(db, id, vol, sizeof vol)) return ENOENT;
    return wps_db_vobj_delete(db, vol, id) < 0 ? EIO : 0;
}

void wps_central_forget(const char *id)
{
    sobj *s = by_central(id);
    if (s) wps_sobj_forget(s);
}

/* ------------------------------------------------------- templates folder */

static char templates[PATH_MAX];

void wps_templates_init(void)
{
    const char *t = getenv("WPS_TEMPLATES_DIR"), *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    if (t && *t) { snprintf(templates, sizeof templates, "%s", t); return; }
    snprintf(templates, sizeof templates, "%s/Templates", home ? home : "");
    char cfg[PATH_MAX], line[PATH_MAX + 64];
    if (xdg && *xdg) snprintf(cfg, sizeof cfg, "%s/user-dirs.dirs", xdg);
    else snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home ? home : "");
    FILE *f = fopen(cfg, "re");
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "XDG_TEMPLATES_DIR=\"", 19)) continue;
        char *v = line + 19, *e = strrchr(v, '"');
        if (!e) break;
        *e = 0;
        if (!strncmp(v, "$HOME", 5)) snprintf(templates, sizeof templates, "%s%s", home ? home : "", v + 5);
        else if (v[0] == '/') snprintf(templates, sizeof templates, "%s", v);
        break;
    }
    if (f) fclose(f);
    char real[PATH_MAX];
    if (realpath(templates, real)) snprintf(templates, sizeof templates, "%s", real);
}

const char *wps_templates_dir(void) { return templates; }

bool wps_in_templates(const char *path)
{
    size_t l = strlen(templates);
    if (!l || !path || strncmp(path, templates, l) || path[l] != '/') return false;
    const char *name = path + l + 1;
    return *name && *name != '.' && !strchr(name, '/');   /* a direct entry, not a hidden one */
}
