/*
 * The change tracker (design: Change tracking, Edge cases, Detecting saves).
 *
 * Events come from osd-watchd when it is running (fanotify, whole
 * filesystems, filtered per user) or else from inotify on the folders the
 * service tracks. Both are turned into one stream of path-based events,
 * which the rules below apply to the service's objects:
 *
 *   - move or rename of an object: it follows;
 *   - rename of a file without an ID onto an object (atomic save): the
 *     object keeps its identity on the new inode; with its own ID, the
 *     incoming file replaces the object;
 *   - displacement (vim): an object renamed within its folder, then a new
 *     file at its old name, then the renamed object deleted: identity
 *     passes to the new file. With the backup-names setting, a rename to
 *     name~, name.bak or name.orig counts as a save at once;
 *   - a file appearing with another file's ID: shared until either changes
 *     (then a copy, with a new ID) or the original is deleted (a move);
 *   - a save through one name of a hard-linked file: the new file becomes
 *     its own object with the settings copied, marked as split;
 *   - deletion: the object is forgotten; its location row is kept.
 *
 * A catch-up scan finds objects that moved while nothing was watching.
 */
#ifndef OSD_TRACKER_H
#define OSD_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

#include "db.h"
#include "env.h"
#include "osdo_loop.h"

void osd_tracker_init(osdo_loop *loop, const osd_env *env, osd_db *db);
void osd_tracker_shutdown(void);

/* Make sure events for this folder's entries arrive (loop thread). */
void osd_tracker_watch(const char *dir);

/* Run the catch-up scan now; `done` (may be NULL) runs when it finishes. */
void osd_tracker_scan(void (*done)(void *arg, const char *report), void *arg);

typedef struct {
    const char *mode;        /* "osd-watchd" or "inotify" */
    uint64_t events;         /* events applied */
    uint64_t overflows;
    uint64_t watched;        /* folders watched */
    uint64_t scans;
    uint64_t missing;        /* objects the last scan could not find */
} osd_tracker_stats;

void osd_tracker_get_stats(osd_tracker_stats *out);

/* An object record is going away: drop pending pairings that refer to it. */
struct sobj;
void osd_tracker_forget(struct sobj *s);
/* Folder content changes, for event subscribers (loop thread):
 * kind 'c' created, 'd' deleted, 'm' moved (from -> path; either may be ""
 * when outside the folders in view). */
extern void (*osd_on_fs_event)(char kind, const char *from, const char *path);

/* An object record was created for `path`. */
void osd_tracker_object_at(const char *path);

#endif
