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

/* Volumes: a snapshot shared with workers. */
void wps_volumes_replace(wps_volumes *vs);    /* loop; takes ownership */
bool wps_volume_for_path(const char *path, wps_volume *out);
bool wps_volume_for_uuid(const char *uuid, wps_volume *out);

#endif
