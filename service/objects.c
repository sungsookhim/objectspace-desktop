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

/* Service-side record of a file-backed object. A linear table is enough
 * for M2; M4 replaces it with an index (design: Scale). */
struct sobj {
    wpo_object *o;
    dev_t dev;
    ino_t ino;
    struct timespec ctime;
    char path[PATH_MAX];
    char vol[WPS_VOLID_MAX];
    char rel[PATH_MAX];
    char locator[PATH_MAX + 16];
    bool xattr_ok, writable, owned;
    bool queued, saving;
};

static struct sobj **objs;
static size_t nobjs, capobjs;
static size_t inflight;          /* saves being written by workers */

struct flush_waiter { void (*done)(void *); void *arg; struct flush_waiter *next; };
static struct flush_waiter *waiters;

static int timer_fd = -1;
static bool timer_armed;

static struct sobj *by_object(wpo_object *o)
{
    for (size_t i = 0; i < nobjs; i++)
        if (objs[i]->o == o) return objs[i];
    return NULL;
}

static struct sobj *by_inode(dev_t dev, ino_t ino)
{
    for (size_t i = 0; i < nobjs; i++)
        if (objs[i]->dev == dev && objs[i]->ino == ino) return objs[i];
    return NULL;
}

size_t wps_objects_count(void) { return nobjs; }

size_t wps_objects_pending(void)
{
    size_t n = inflight;
    for (size_t i = 0; i < nobjs; i++) n += objs[i]->queued;
    return n;
}

const char *wps_object_path(wpo_object *o)
{
    struct sobj *s = by_object(o);
    return s ? s->path : NULL;
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

void wps_load(const char *target, bool assign_id, wps_loaded *ld)
{
    memset(ld, 0, sizeof *ld);
    if (strncmp(target, "id:", 3) == 0) {
        /* An object named by ID: find where it was last seen. */
        char vol[WPS_VOLID_MAX], rel[PATH_MAX];
        wps_volume v;
        if (wps_db_get_location(db, target + 3, vol, sizeof vol, rel, sizeof rel) < 0 ||
            !wps_volume_for_uuid(vol, &v)) {
            ld->err = ENOENT;
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

    /* Shared state: every user.wps.<Class> attribute on the file. */
    char names[8192];
    ssize_t nl = S_ISLNK(ld->st.st_mode) ? -1 : llistxattr(ld->path, names, sizeof names);
    ld->xattr_ok = nl >= 0 || (errno != ENOTSUP && !S_ISLNK(ld->st.st_mode));
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

    if (assign_id && !ld->id[0]) {
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

static wpo_class *class_for_mode(mode_t m)
{
    if (S_ISDIR(m)) return WPFolderClass;
    if (S_ISREG(m)) return WPDataFileClass;
    return WPFileSystemClass;
}

static void apply_loaded(struct sobj *s, wps_loaded *ld)
{
    WPObjectData *d = WPObject_data(s->o);
    if (ld->id[0]) memcpy(d->id, ld->id, sizeof d->id);
    for (int i = 0; i < ld->nc; i++) wps_state_load(s->o, ld->c[i].cls, &ld->c[i].props);
    WPObject_wpRestoreProperties(s->o);
    s->ctime = ld->st.st_ctim;
    snprintf(s->path, sizeof s->path, "%s", ld->path);
    snprintf(s->vol, sizeof s->vol, "%s", ld->vol);
    snprintf(s->rel, sizeof s->rel, "%s", ld->rel);
    snprintf(s->locator, sizeof s->locator, "%s", ld->locator);
    s->xattr_ok = ld->xattr_ok;
    s->writable = ld->writable;
    s->owned = ld->owned;
}

wpo_object *wps_object_from_loaded(wps_loaded *ld)
{
    if (ld->err) return NULL;
    struct sobj *s = by_inode(ld->st.st_dev, ld->st.st_ino);
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
        struct sobj **n = realloc(objs, ncap * sizeof *n);
        if (!n) return NULL;
        objs = n;
        capobjs = ncap;
    }
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->o = wpo_new(class_for_mode(ld->st.st_mode));   /* replacements apply */
    if (!s->o) { free(s); return NULL; }
    s->dev = ld->st.st_dev;
    s->ino = ld->st.st_ino;
    WPFileSystem_wpSetPath(s->o, ld->path);
    apply_loaded(s, ld);
    objs[nobjs++] = s;
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
    struct sobj *s;                   /* loop-thread pointer; workers only read the copies */
    char path[PATH_MAX], vol[WPS_VOLID_MAX], rel[PATH_MAX], locator[PATH_MAX + 16];
    bool xattr_ok, writable, owned;
    char id[WPS_ID_LEN + 1];          /* in: current ID; out: assigned ID */
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
        if (st != WPS_A_OK) {
            /* No permission, no attributes, too big, or lock not obtained:
             * keep it in this user's overlay. */
            db_write_props(j, ch->cls, &ch->set, ch->removed, ch->nremoved);
        }
    }
    if (j->id[0]) wps_db_set_location(db, j->id, j->vol, j->rel);
}

static void schedule_flush(void);

static void save_done(void *arg)
{
    struct save_job *j = arg;
    struct sobj *s = j->s;
    if (j->id[0] && !WPObject_data(s->o)->id[0]) memcpy(WPObject_data(s->o)->id, j->id, WPS_ID_LEN + 1);
    snprintf(s->locator, sizeof s->locator, "%s", j->locator);
    s->saving = false;
    for (size_t c = 0; c < j->nch; c++) {
        wps_props_free(&j->ch[c].set);
        for (size_t i = 0; i < j->ch[c].nremoved; i++) free(j->ch[c].removed[i]);
        free(j->ch[c].removed);
    }
    free(j->ch);
    free(j);
    inflight--;
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
static void start_save(struct sobj *s)
{
    s->queued = false;
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
    s->saving = true;
    inflight++;
    wpo_defer(loop, "save object", save_work, save_done, j);
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
    struct sobj *s = by_object(o);
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
}
