/*
 * The user's database (design: Persistence, State that travels with a
 * volume).
 *
 * Per-user state lives in one table of rows keyed by (volume UUID, key):
 *     key = <locator> US <class> US <property>        (US = 0x1f)
 * where <locator> names the object on its volume: its object ID, or
 * "fh:<handle>" for a file the user cannot tag, or "path:<relative path>"
 * on filesystems without extended attributes. Each row carries a revision
 * (a logical clock), its writer ("<identity>/<host>"), a deletion flag
 * (tombstones) and whether it has been pushed to the volume's own database.
 *
 * Portable volumes have their own database, <mount>/.osd/<identity>.db,
 * with the same table. Synchronisation is git-like: pull rows that are
 * newer on the volume, push rows that are newer here; when both sides
 * changed a row, the higher (revision, writer) wins and the loser is kept
 * in the `conflicts` table.
 *
 * All functions do disk I/O: call them from workers. A osd_db may be used
 * from several threads; calls are serialised.
 */
#ifndef OSD_DB_H
#define OSD_DB_H

#include <stdbool.h>
#include <stdint.h>

#include "attrs.h"

#define OSD_KEY_SEP '\x1f'

typedef struct osd_db osd_db;

typedef struct {
    int pulled;      /* rows taken from the volume */
    int pushed;      /* rows written to the volume */
    int conflicts;   /* rows where both sides had changed */
} osd_sync_stats;

/* The databases' format version (SQLite's user_version); 0.1's had none. */
#define OSD_DB_VERSION 2
osd_db *osd_db_open(const char *path, const char *writer, char *err, size_t errlen);
int64_t osd_db_version(osd_db *db);
void osd_db_close(osd_db *db);

/* Build a key "<locator>\x1f<cls>\x1f<prop>" (prop may be NULL for a prefix
 * ending in the separator). */
void osd_db_key(char *out, size_t n, const char *locator, const char *cls, const char *prop);

/* Write or delete one property (new revision, not yet pushed). If the
 * volume is attached and writable, it is pushed at once. */
int osd_db_put(osd_db *db, const char *vol, const char *key, char type, const void *val, uint32_t len);
int osd_db_delete(osd_db *db, const char *vol, const char *key);

/* All live properties of <locator>/<cls> on vol, into `out` (keys are the
 * property names). Returns the number found, or -1. */
int osd_db_get_class(osd_db *db, const char *vol, const char *locator, const char *cls, osd_props *out);

/* All live properties of <locator> on vol, for every class: calls fn once
 * per property. Returns the number found, or -1. */
typedef void (*osd_db_prop_fn)(const char *cls, const char *prop, char type,
                               const void *val, uint32_t len, void *user);
int osd_db_get_locator(osd_db *db, const char *vol, const char *locator, osd_db_prop_fn fn, void *user);

/* Portable volumes. attach opens (creating if writable) the volume's
 * database and synchronises; detach forgets it. A read-only volume is
 * pulled from but not pushed to; its changes wait until it is writable. */
int osd_db_attach_volume(osd_db *db, const char *vol, const char *voldb_path, bool writable,
                         osd_sync_stats *st, char *err, size_t errlen);
void osd_db_detach_volume(osd_db *db, const char *vol);
bool osd_db_volume_attached(osd_db *db, const char *vol);

/* Counters for tests and diagnostics. */
int64_t osd_db_count(osd_db *db, const char *table, const char *vol);   /* rows (live, for state) */
int64_t osd_db_unpushed(osd_db *db, const char *vol);
uint64_t osd_db_clock(osd_db *db);

/* Locations: object ID -> volume and path (refreshed whenever seen). */
int osd_db_set_location(osd_db *db, const char *id, const char *vol, const char *relpath);
int osd_db_get_location(osd_db *db, const char *id, char *vol, size_t vn, char *relpath, size_t rn);

/* Every known location (live rows), for the catch-up scan. */
typedef void (*osd_db_loc_fn)(const char *id, const char *vol, const char *relpath, void *user);
int osd_db_locations(osd_db *db, osd_db_loc_fn fn, void *user);
/* The object was deleted: keep the row (an undo or a restore can reclaim
 * the ID) but mark it, so the scan stops looking for it. */
int osd_db_location_forget(osd_db *db, const char *id);

/* Shadows: managed symlinks (link) pointing at objects (target). */
typedef void (*osd_db_shadow_fn)(const char *link_vol, const char *link_rel, const char *target_id,
                                 const char *target_vol, const char *target_rel, void *user);
int osd_db_shadow_put(osd_db *db, const char *link_vol, const char *link_rel, const char *target_id,
                      const char *target_vol, const char *target_rel);
int osd_db_shadow_del(osd_db *db, const char *link_vol, const char *link_rel);
int osd_db_shadows_for(osd_db *db, const char *target_id, osd_db_shadow_fn fn, void *user);
/* Shadows whose target lies under <rel_prefix>/ on target_vol (a folder moved). */
int osd_db_shadows_under(osd_db *db, const char *target_vol, const char *rel_prefix, osd_db_shadow_fn fn, void *user);

/* The job journal (design: Bulk jobs). `sel` is the selection, encoded by
 * the job manager. */
int osd_db_job_put(osd_db *db, int64_t id, const char *kind, const char *src, const char *sel, const char *dest,
                   const char *state, int64_t items_done, int64_t bytes_done);
typedef void (*osd_db_job_fn)(int64_t id, const char *kind, const char *src, const char *sel, const char *dest,
                              const char *state, int64_t items_done, int64_t bytes_done, void *user);
int osd_db_jobs_list(osd_db *db, osd_db_job_fn fn, void *user);

/* Central objects (decided 2026-11-18): Objectspace-only objects kept here rather
 * than as .osdobj files. Locator "vobj:<id>"; their own rows under class
 * "Objectspace": folder (the folder's object ID), class, and target (shadows). Their
 * settings use the ordinary rows. Rows on a portable volume travel in its
 * database like any other. */
int osd_db_vobjs_in(osd_db *db, const char *vol, const char *folder_id,
                    void (*fn)(const char *vol, const char *id, void *user), void *user);
int osd_db_vobjs_shadowing(osd_db *db, const char *target_id,
                           void (*fn)(const char *vol, const char *id, void *user), void *user);
bool osd_db_vobj_vol(osd_db *db, const char *id, char *vol, size_t vn);
int osd_db_vobj_copy(osd_db *db, const char *vol, const char *id, const char *new_vol, const char *new_id, bool remove);
int osd_db_vobj_delete(osd_db *db, const char *vol, const char *id);

#endif
