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
#ifndef OSD_ATTRS_H
#define OSD_ATTRS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSD_ATTR_PREFIX "user.wps."
#define OSD_ATTR_ID     "user.wps.id"
#define OSD_ID_LEN      36

typedef struct {
    char *key;
    char type;
    uint32_t len;
    unsigned char *val;
} osd_prop;

typedef struct {
    uint64_t counter;
    osd_prop *p;
    size_t n, cap;
} osd_props;

void osd_props_init(osd_props *ps);
void osd_props_free(osd_props *ps);
int osd_props_decode(const void *buf, size_t len, osd_props *out);   /* 0 or -1 */
/* Returns a malloc'd buffer and its length in *len, or NULL. */
unsigned char *osd_props_encode(const osd_props *ps, size_t *len);

const osd_prop *osd_props_get(const osd_props *ps, const char *key);
int osd_props_set(osd_props *ps, const char *key, char type, const void *val, uint32_t len);
int osd_props_set_str(osd_props *ps, const char *key, const char *s);
int osd_props_set_int(osd_props *ps, const char *key, int64_t v);
int osd_props_set_bool(osd_props *ps, const char *key, bool v);
bool osd_props_remove(osd_props *ps, const char *key);
/* Convenience readers; return false if missing or of another type. */
bool osd_props_str(const osd_props *ps, const char *key, char **out);   /* malloc'd */
bool osd_props_int(const osd_props *ps, const char *key, int64_t *out);

/* Result of an attribute operation. */
typedef enum {
    OSD_A_OK = 0,
    OSD_A_NOPERM,      /* no write permission: use the per-user overlay */
    OSD_A_NOTSUP,      /* filesystem has no user attributes: use the database */
    OSD_A_BUSY,        /* lock not obtained in time: overlay, marked pending */
    OSD_A_TOOBIG,      /* does not fit: the caller stores it in the database */
    OSD_A_NOFILE,      /* the file is gone */
    OSD_A_CORRUPT,     /* existing value could not be decoded (left untouched) */
    OSD_A_ERROR
} osd_astatus;

const char *osd_astatus_str(osd_astatus st);

/* Read user.wps.<cls> of `path` (no symlink following). A missing attribute
 * gives empty properties and OSD_A_OK. */
osd_astatus osd_attr_read(const char *path, const char *cls, osd_props *out);

/*
 * Merge `set` into user.wps.<cls> and remove the keys in `removed` (NULL-
 * terminated, may be NULL), under the file's lock. The counter is
 * incremented. If `expect` is not NULL, the update happens only when the
 * stored counter equals *expect; otherwise OSD_A_ERROR is returned with
 * *expect set to the stored counter (a conflict). `result`, if not NULL,
 * receives the merged properties.
 */
osd_astatus osd_attr_update(const char *path, const char *cls, const osd_props *set,
                            const char *const *removed, int lock_timeout_ms,
                            uint64_t *expect, osd_props *result);

/* Assign an object ID if the file has none (XATTR_CREATE: exactly one
 * writer wins); `id` receives the file's ID either way (OSD_ID_LEN + 1). */
osd_astatus osd_attr_assign_id(const char *path, const char *candidate, char *id);
/* Read the ID; false if none. */
bool osd_attr_get_id(const char *path, char *id);

/* A new random object ID (UUID version 4). */
void osd_new_id(char out[OSD_ID_LEN + 1]);

#endif
