/*
 * Folder listings for folders of any size (design: Scale, Opening a folder
 * with many files).
 *
 * A worker reads the directory in chunks and posts each chunk to the loop,
 * so a request for the first page is answered as soon as that page has
 * arrived, while the rest of a million-entry folder is still being read.
 * Records are names and types only; details (size, time, object ID,
 * title) are read on a worker for the requested page alone. Entries are in
 * directory order; sorted views come with the GUI (M5). A change in the
 * folder marks its listing stale; the next request for its first page
 * reads it again.
 */
#ifndef WPS_LISTING_H
#define WPS_LISTING_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "attrs.h"
#include "wpo_loop.h"

typedef struct {
    char name[NAME_MAX + 1];
    unsigned char type;      /* DT_REG, DT_DIR, DT_LNK, ... */
    bool detailed;
    int64_t size;
    int64_t mtime;
    char id[WPS_ID_LEN + 1];
    char title[256];
    bool shadow;             /* a shadow: title and type are its target's */
    unsigned char target_type;
    char target_path[PATH_MAX];
    bool locked;             /* a lock file next to it (or its target) says it is open */
    bool is_template;        /* marked as one, or in the templates folder */
    char makes[64];          /* a built-in template: the class of what it makes */
    char icon[PATH_MAX];     /* its own icon (a PNG), or its shadow's target's; "": its kind's */
    char icon_open[PATH_MAX];   /* its own open icon, or its shadow's target's; "": none */
    bool open_hatch;         /* hatched while open, over its open icon too */
    unsigned char exec;      /* (0.4.1) a program file (+x, ELF or #!): 1 a window one, 2 text mode */
} wps_entry;

typedef struct {
    int err;
    size_t total;            /* entries read so far */
    bool complete;           /* the whole folder has been read */
    size_t offset, n;
    wps_entry *items;
    uint64_t version;        /* the listing's version (0: being read again) */
} wps_page;

typedef void (*wps_page_fn)(void *arg, const wps_page *page);

void wps_listing_init(wpo_loop *loop);
void wps_listing_shutdown(void);

/* Loop thread. `reply` runs on the loop thread, once. */
void wps_listing_request(const char *folder, size_t offset, size_t count, bool details,
                         wps_page_fn reply, void *arg);

/* Something changed in this folder that cannot be followed entry by
 * entry: read it again at the next request for the first page. */
/* The same, for a window's own listing of the folder (v0.3-design 6):
 * only names (central objects: titles) containing `filter` (any case;
 * "" all), and without hidden (dot) names when `hide`. Lock files are
 * never listed. */
void wps_listing_request_v(const char *folder, const char *filter, bool hide, size_t offset, size_t count, bool details,
                           wps_page_fn reply, void *arg);
void wps_listing_invalidate(const char *folder);

/* Loop thread: an entry of `folder` appeared ('c', name, its type),
 * went ('d', name) or was renamed in place ('r', name to newname). The
 * cached listing follows it: new entries at the end, gone ones taken out,
 * renamed ones in their place; each change adds one to its version, so a
 * client that applies the same edits stays in step without reading the
 * folder again (design: Scale). Temporary names are never listed. */
void wps_listing_apply(const char *folder, char kind, const char *name, const char *newname, unsigned char type);

/* The cached listing's version: 0 when there is none, or it is being
 * read or must be read again (clients then read it again too). */
uint64_t wps_listing_version(const char *folder);

/* Where the last change to the folder's listing happened (the index of
 * the entry removed or renamed, before the change), or -1: events send
 * it, so a client that has not read every name still follows. */
long wps_listing_last_index(const char *folder);

size_t wps_listing_count(void);
/* Whether a folder's listing (as read) has this entry (e.g. "\x01vobj:ID"). */
bool wps_listing_has(const char *folder, const char *name);

/* The Trash Can (a trash's files folder): whether anything is in it; the
 * icon it shows now - its own for that state, or WPS's default. */
bool wps_trash_full(const char *files);
void wps_trash_icon(const char *files, char *out, size_t n);

/* One of WPS's own icons (data/icons/NAME; installed, or the tree's). */
void wps_default_icon(const char *name, char *out, size_t n);
/* A special folder's icon (NAME as above), shown for it and its shadows. */
void wps_listing_folder_icon(const char *path, const char *png);
/* The icon an object without its own shows (for its Icon page): a PNG's
 * path (the Trash Can's, a special folder's), else "kind:folder",
 * "kind:program" or "kind:file" (drawn by the GUI). */
/* A central object's icon as its window shows it (its own, its shadow's
 * original's, a template's class's, its class's); false: none known. */
bool wps_listing_central_shown(const char *id, char *out, size_t n);
void wps_listing_shown_icon(const char *path, const char *cls, const char *mime, bool dir, bool program, char *out, size_t n);

#endif
