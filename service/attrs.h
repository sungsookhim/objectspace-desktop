/*
 * Object state in extended attributes (design: Persistence, Concurrency).
 *
 * Each class stores its settings in its own attribute, user.wps.<Class>, as
 * a small tagged list of properties with a format version and a change
 * counter. Unknown keys and types are kept on rewrite, so older code never
 * deletes newer settings.
 *
 * Encoding (little-endian):
 *   u8  format version (1)
 *   u64 change counter
 *   repeated: u8 key length, key bytes, u8 type, u32 value length, value bytes
 * Types: 'b' bool (1 byte), 'i' int64 (8), 'd' double (8), 's' string (no NUL).
 *
 * Updates are read-modify-write under an advisory flock() on the file, taken
 * with a non-blocking retry for a bounded time, merging only the keys a
 * change touches. These functions do disk I/O: call them from workers.
 */
#ifndef WPS_ATTRS_H
#define WPS_ATTRS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WPS_ATTR_PREFIX "user.wps."
#define WPS_ATTR_ID     "user.wps.id"
#define WPS_ID_LEN      36

typedef struct {
    char *key;
    char type;
    uint32_t len;
    unsigned char *val;
} wps_prop;

typedef struct {
    uint64_t counter;
    wps_prop *p;
    size_t n, cap;
} wps_props;

void wps_props_init(wps_props *ps);
void wps_props_free(wps_props *ps);
int wps_props_decode(const void *buf, size_t len, wps_props *out);   /* 0 or -1 */
/* Returns a malloc'd buffer and its length in *len, or NULL. */
unsigned char *wps_props_encode(const wps_props *ps, size_t *len);

const wps_prop *wps_props_get(const wps_props *ps, const char *key);
int wps_props_set(wps_props *ps, const char *key, char type, const void *val, uint32_t len);
int wps_props_set_str(wps_props *ps, const char *key, const char *s);
int wps_props_set_int(wps_props *ps, const char *key, int64_t v);
int wps_props_set_bool(wps_props *ps, const char *key, bool v);
bool wps_props_remove(wps_props *ps, const char *key);
/* Convenience readers; return false if missing or of another type. */
bool wps_props_str(const wps_props *ps, const char *key, char **out);   /* malloc'd */
bool wps_props_int(const wps_props *ps, const char *key, int64_t *out);

/* Result of an attribute operation. */
typedef enum {
    WPS_A_OK = 0,
    WPS_A_NOPERM,      /* no write permission: use the per-user overlay */
    WPS_A_NOTSUP,      /* filesystem has no user attributes: use the database */
    WPS_A_BUSY,        /* lock not obtained in time: overlay, marked pending */
    WPS_A_TOOBIG,      /* does not fit: the caller stores it in the database */
    WPS_A_NOFILE,      /* the file is gone */
    WPS_A_CORRUPT,     /* existing value could not be decoded (left untouched) */
    WPS_A_ERROR
} wps_astatus;

const char *wps_astatus_str(wps_astatus st);

/* Read user.wps.<cls> of `path` (no symlink following). A missing attribute
 * gives empty properties and WPS_A_OK. */
wps_astatus wps_attr_read(const char *path, const char *cls, wps_props *out);

/*
 * Merge `set` into user.wps.<cls> and remove the keys in `removed` (NULL-
 * terminated, may be NULL), under the file's lock. The counter is
 * incremented. If `expect` is not NULL, the update happens only when the
 * stored counter equals *expect; otherwise WPS_A_ERROR is returned with
 * *expect set to the stored counter (a conflict). `result`, if not NULL,
 * receives the merged properties.
 */
wps_astatus wps_attr_update(const char *path, const char *cls, const wps_props *set,
                            const char *const *removed, int lock_timeout_ms,
                            uint64_t *expect, wps_props *result);

/* Assign an object ID if the file has none (XATTR_CREATE: exactly one
 * writer wins); `id` receives the file's ID either way (WPS_ID_LEN + 1). */
wps_astatus wps_attr_assign_id(const char *path, const char *candidate, char *id);
/* Read the ID; false if none. */
bool wps_attr_get_id(const char *path, char *id);

/* A new random object ID (UUID version 4). */
void wps_new_id(char out[WPS_ID_LEN + 1]);

#endif
