/*
 * The service's objects: finding, loading and saving file-backed objects
 * (design: Object identity, Persistence, Concurrency).
 *
 * Loading and saving do disk I/O and run on workers (wps_load, the save
 * jobs); everything that touches objects runs on the loop thread.
 *
 * Where each property goes when saved:
 *   - the file's user.wps.<Class> attribute, merged under the file's lock,
 *     when the filesystem supports attributes and the user may write;
 *   - otherwise the user's database (an overlay that only this user sees),
 *     keyed by the object's locator. The same applies when the file's lock
 *     cannot be taken in time, and to icon positions in folders the user
 *     does not own (each user keeps their own arrangement there).
 * A locator is the object ID stored on the file, or "fh:<handle>" for a
 * file the user cannot tag, or "path:<relative path>" on filesystems
 * without attributes (portable between computers).
 */
#ifndef WPS_OBJECTS_H
#define WPS_OBJECTS_H

#include <limits.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "attrs.h"
#include "db.h"
#include "env.h"
#include "volumes.h"
#include "wpo.h"
#include "wpo_loop.h"

#define WPS_MAX_CLASSES 16

typedef struct {
    int err;                         /* 0, or an errno value */
    struct stat st;
    char path[PATH_MAX];
    char vol[WPS_VOLID_MAX];
    char rel[PATH_MAX];
    bool xattr_ok, writable, owned;
    char id[WPS_ID_LEN + 1];         /* "" if none */
    char locator[PATH_MAX + 16];
    struct { char cls[64]; wps_props props; } c[WPS_MAX_CLASSES];
    int nc;
    /* Symlinks: where it points, and the target's ID if the folder records
     * it as a managed shadow (design: Links). */
    char link_target[PATH_MAX];
    char shadow_of[WPS_ID_LEN + 1];
    /* Abstract objects stored as files (design: Abstract objects are files):
     * a .desktop file is a WPProgram; a .wpobj file names its class. */
    char obj_class[64];
    char desk_name[256];
    char desk_exec[1024];
} wps_loaded;

void wps_objects_init(const wps_env *env, wps_db *db, wpo_loop *loop, int save_delay_ms);
void wps_objects_shutdown(void);

/* Worker: stat and read everything about `target` (a path, or "id:<ID>").
 * With assign_id, an object without an ID gets one now. */
void wps_load(const char *target, bool assign_id, wps_loaded *out);
void wps_loaded_free(wps_loaded *ld);

/* Loop: the object for loaded data, created or refreshed. Borrowed. */
wpo_object *wps_object_from_loaded(wps_loaded *ld);
const char *wps_object_path(wpo_object *o);   /* NULL if not file-backed */

/* Loop: write all pending changes now; `done` runs when nothing is pending. */
void wps_objects_flush(void (*done)(void *arg), void *arg);
size_t wps_objects_count(void);
size_t wps_objects_pending(void);

/* ---- Object records, for the change tracker (loop thread only). */
typedef struct sobj {
    wpo_object *o;
    dev_t dev;
    ino_t ino;
    nlink_t nlink;
    struct timespec ctime, mtime;
    char path[PATH_MAX];
    char vol[WPS_VOLID_MAX];
    char rel[PATH_MAX];
    char locator[PATH_MAX + 16];
    bool xattr_ok, writable, owned;
    bool queued, saving;
    bool retry;                  /* some keys went to the overlay because the file was busy */
    bool forgotten;              /* removed while a save was running; freed when it ends */
    char declared_class[64];     /* a .wpobj's class, if it is not installed here */
    int retry_delay_ms;
} sobj;

sobj *wps_sobj_by_path(const char *path);
sobj *wps_sobj_by_id(const char *id);
sobj *wps_sobj_of(wpo_object *o);
size_t wps_sobj_count(void);
sobj *wps_sobj_at(size_t i);
/* The object now lives at `path` (renamed or moved); updates its record,
 * its WPFileSystem path and its location row. */
void wps_sobj_set_path(sobj *s, const char *path);
/* The object's file was replaced by another inode at the same place
 * (identity carried over). */
void wps_sobj_set_inode(sobj *s, const struct stat *st);
/* The object is gone (deleted, or replaced by another object). */
void wps_sobj_forget(sobj *s);
/* Snapshot of all stored properties, per class, for copying them elsewhere. */
void wps_sobj_snapshot(sobj *s, wps_loaded *out);
/* Called when an object's WPS settings were saved (the tracker treats a
 * pending duplicate as a copy from then on). */
extern void (*wps_on_saved)(sobj *s);

/* The file name for a new object of class `cls` titled `title` in `folder`
 * (".desktop" for programs, ".wpobj" for other abstract classes). */
void wps_object_filename(const char *folder, const char *cls, const char *title, int n, char *out, size_t len);
/* Write a new abstract object's file (no-replace); 0 or an errno value. */
int wps_object_file_create(const char *path, const char *cls, const char *title);

/* Excluded paths (design: Change tracking): defaults plus <config>/excluded.
 * Files there are never given IDs or state, and are not watched. */
bool wps_path_excluded(const char *path);
/* Excluded, or (in a root session) inside another user's home: never
 * watched or scanned automatically (design: Root sessions). */
bool wps_path_untracked(const char *path);
void wps_set_root_session(bool on);

/* Volumes: a snapshot shared with workers. */
void wps_volumes_replace(wps_volumes *vs);    /* loop; takes ownership */
bool wps_volume_for_path(const char *path, wps_volume *out);
bool wps_volume_for_uuid(const char *uuid, wps_volume *out);

#endif
