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
 * Portable volumes have their own database, <mount>/.wps/<identity>.db,
 * with the same table. Synchronisation is git-like: pull rows that are
 * newer on the volume, push rows that are newer here; when both sides
 * changed a row, the higher (revision, writer) wins and the loser is kept
 * in the `conflicts` table.
 *
 * All functions do disk I/O: call them from workers. A wps_db may be used
 * from several threads; calls are serialised.
 */
#ifndef WPS_DB_H
#define WPS_DB_H

#include <stdbool.h>
#include <stdint.h>

#include "attrs.h"

#define WPS_KEY_SEP '\x1f'

typedef struct wps_db wps_db;

typedef struct {
    int pulled;      /* rows taken from the volume */
    int pushed;      /* rows written to the volume */
    int conflicts;   /* rows where both sides had changed */
} wps_sync_stats;

wps_db *wps_db_open(const char *path, const char *writer, char *err, size_t errlen);
void wps_db_close(wps_db *db);

/* Build a key "<locator>\x1f<cls>\x1f<prop>" (prop may be NULL for a prefix
 * ending in the separator). */
void wps_db_key(char *out, size_t n, const char *locator, const char *cls, const char *prop);

/* Write or delete one property (new revision, not yet pushed). If the
 * volume is attached and writable, it is pushed at once. */
int wps_db_put(wps_db *db, const char *vol, const char *key, char type, const void *val, uint32_t len);
int wps_db_delete(wps_db *db, const char *vol, const char *key);

/* All live properties of <locator>/<cls> on vol, into `out` (keys are the
 * property names). Returns the number found, or -1. */
int wps_db_get_class(wps_db *db, const char *vol, const char *locator, const char *cls, wps_props *out);

/* All live properties of <locator> on vol, for every class: calls fn once
 * per property. Returns the number found, or -1. */
typedef void (*wps_db_prop_fn)(const char *cls, const char *prop, char type,
                               const void *val, uint32_t len, void *user);
int wps_db_get_locator(wps_db *db, const char *vol, const char *locator, wps_db_prop_fn fn, void *user);

/* Portable volumes. attach opens (creating if writable) the volume's
 * database and synchronises; detach forgets it. A read-only volume is
 * pulled from but not pushed to; its changes wait until it is writable. */
int wps_db_attach_volume(wps_db *db, const char *vol, const char *voldb_path, bool writable,
                         wps_sync_stats *st, char *err, size_t errlen);
void wps_db_detach_volume(wps_db *db, const char *vol);
bool wps_db_volume_attached(wps_db *db, const char *vol);

/* Counters for tests and diagnostics. */
int64_t wps_db_count(wps_db *db, const char *table, const char *vol);   /* rows (live, for state) */
int64_t wps_db_unpushed(wps_db *db, const char *vol);
uint64_t wps_db_clock(wps_db *db);

/* Locations: object ID -> volume and path (refreshed whenever seen). */
int wps_db_set_location(wps_db *db, const char *id, const char *vol, const char *relpath);
int wps_db_get_location(wps_db *db, const char *id, char *vol, size_t vn, char *relpath, size_t rn);

#endif
