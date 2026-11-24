/*
 * The service's objects: finding, loading and saving file-backed objects
 * (design: Object identity, Persistence, Concurrency).
 *
 * Loading and saving do disk I/O and run on workers (osd_load, the save
 * jobs); everything that touches objects runs on the loop thread.
 *
 * Where each property goes when saved:
 *   - the file's user.osd.<Class> attribute, merged under the file's lock,
 *     when the filesystem supports attributes and the user may write;
 *   - otherwise the user's database (an overlay that only this user sees),
 *     keyed by the object's locator. The same applies when the file's lock
 *     cannot be taken in time, and to icon positions in folders the user
 *     does not own (each user keeps their own arrangement there).
 * A locator is the object ID stored on the file, or "fh:<handle>" for a
 * file the user cannot tag, or "path:<relative path>" on filesystems
 * without attributes (portable between computers).
 */
#ifndef OSD_OBJECTS_H
#define OSD_OBJECTS_H

#include <limits.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "attrs.h"
#include "db.h"
#include "env.h"
#include "volumes.h"
#include "osdo.h"
#include "osdo_loop.h"

#define OSD_MAX_CLASSES 16

typedef struct {
    int err;                         /* 0, or an errno value */
    struct stat st;
    char path[PATH_MAX];
    char vol[OSD_VOLID_MAX];
    char rel[PATH_MAX];
    bool xattr_ok, writable, owned;
    char id[OSD_ID_LEN + 1];         /* "" if none */
    char locator[PATH_MAX + 16];
    struct { char cls[64]; osd_props props; } c[OSD_MAX_CLASSES];
    int nc;
    /* Symlinks: where it points, and the target's ID if the folder records
     * it as a managed shadow (design: Links). */
    char link_target[PATH_MAX];
    char shadow_of[OSD_ID_LEN + 1];
    /* Abstract objects stored as files (design: Abstract objects are files):
     * a .desktop file is a OsdProgram; a .osdobj file names its class. */
    char obj_class[64];
    char desk_name[256];
    char desk_exec[1024];
    /* Central objects (decided 2026-11-18): no file; kept in the database
     * as "vobj:<id>" rows. path is empty; folder is its folder's ID. */
    bool central;
    char folder_id[OSD_ID_LEN + 1];
} osd_loaded;

void osd_objects_init(const osd_env *env, osd_db *db, osdo_loop *loop, int save_delay_ms);
void osd_objects_shutdown(void);

/* Worker: stat and read everything about `target` (a path, "id:<ID>", or
 * "vobj:<ID>" for a central object).
 * With assign_id, an object without an ID gets one now. */
void osd_load(const char *target, bool assign_id, osd_loaded *out);
void osd_loaded_free(osd_loaded *ld);

/* Loop: the object for loaded data, created or refreshed. Borrowed. */
osdo_object *osd_object_from_loaded(osd_loaded *ld);
const char *osd_object_path(osdo_object *o);   /* NULL if not file-backed (central objects) */

/* Loop: write all pending changes now; `done` runs when nothing is pending. */
void osd_objects_flush(void (*done)(void *arg), void *arg);
size_t osd_objects_count(void);
size_t osd_objects_pending(void);

/* ---- Object records, for the change tracker (loop thread only). */
typedef struct sobj {
    osdo_object *o;
    dev_t dev;
    ino_t ino;
    nlink_t nlink;
    struct timespec ctime, mtime;
    char path[PATH_MAX];
    char vol[OSD_VOLID_MAX];
    char rel[PATH_MAX];
    char locator[PATH_MAX + 16];
    bool xattr_ok, writable, owned;
    bool queued, saving;
    bool retry;                  /* some keys went to the overlay because the file was busy */
    bool forgotten;              /* removed while a save was running; freed when it ends */
    char declared_class[64];     /* a .osdobj's class, if it is not installed here */
    bool central;                /* a central object: no file, rows only */
    char folder_id[OSD_ID_LEN + 1];
    int retry_delay_ms;
} sobj;

sobj *osd_sobj_by_path(const char *path);
sobj *osd_sobj_by_id(const char *id);
sobj *osd_sobj_of(osdo_object *o);
size_t osd_sobj_count(void);
sobj *osd_sobj_at(size_t i);
/* The object now lives at `path` (renamed or moved); updates its record,
 * its OsdFileSystem path and its location row. */
void osd_sobj_set_path(sobj *s, const char *path);
/* The object's file was replaced by another inode at the same place
 * (identity carried over). */
void osd_sobj_set_inode(sobj *s, const struct stat *st);
/* The object is gone (deleted, or replaced by another object). */
void osd_sobj_forget(sobj *s);
/* Snapshot of all stored properties, per class, for copying them elsewhere. */
void osd_sobj_snapshot(sobj *s, osd_loaded *out);
/* Called when an object's WPS settings were saved (the tracker treats a
 * pending duplicate as a copy from then on). */
extern void (*osd_on_saved)(sobj *s);

/* The file name for a new object of class `cls` titled `title` in `folder`
 * (".desktop" for programs, ".osdobj" for other abstract classes). */
void osd_object_filename(const char *folder, const char *cls, const char *title, int n, char *out, size_t len);
/* Worker: a new central object of class `cls` in the folder with object ID
 * `folder_id` on `vol`, with extra rows under class WPS (e.g. "target").
 * Writes its ID into `id`. 0, or an errno value. */
int osd_central_create(const char *vol, const char *folder_id, const char *cls, const char *title,
                       const char *target_id, char id[OSD_ID_LEN + 1]);
/* Worker: does this loaded folder keep its objects as files? */
bool osd_folder_keeps_files(const osd_loaded *folder);
/* Worker: the central objects in a folder (by the folder's ID). */
int osd_central_in(const char *vol, const char *folder_id, void (*fn)(const char *vol, const char *id, void *user), void *user);
/* Worker: a special folder's class (a OsdFolder subclass), stored for this
 * user as the folder's WPS "class". 0, or an errno value. */
int osd_set_folder_class(const char *path, const char *cls);
/* Worker: delete a central object (all its rows). 0, or an errno value. */
int osd_central_delete(const char *id);
/* Loop thread: drop a deleted central object from memory. */
void osd_central_forget(const char *id);
/* Worker: move a central object to another folder, or copy it (a new ID). */
int osd_central_move(const char *id, const char *dest_vol, const char *dest_folder_id, bool copy, char new_id[OSD_ID_LEN + 1]);

/* The templates folder: XDG_TEMPLATES_DIR from user-dirs.dirs, else
 * ~/Templates (OSD_TEMPLATES_DIR for tests). The same folder GNOME's
 * "New Document" uses, so files put there are templates in both. Resolved
 * once at start-up (osd_templates_init), read-only after. */
void osd_templates_init(void);
const char *osd_templates_dir(void);
/* Whether `path` is directly in the templates folder (so a template). */
bool osd_in_templates(const char *path);

/* Write a new abstract object's file (no-replace); 0 or an errno value. */
int osd_object_file_create(const char *path, const char *cls, const char *title);

/* Excluded paths (design: Change tracking): defaults plus <config>/excluded.
 * Files there are never given IDs or state, and are not watched. */
bool osd_path_excluded(const char *path);
/* Excluded, or (in a root session) inside another user's home: never
 * watched or scanned automatically (design: Root sessions). */
bool osd_path_untracked(const char *path);
void osd_set_root_session(bool on);

/* Volumes: a snapshot shared with workers. */
void osd_volumes_replace(osd_volumes *vs);    /* loop; takes ownership */
bool osd_volume_for_path(const char *path, osd_volume *out);
bool osd_volume_for_uuid(const char *uuid, osd_volume *out);

#endif
