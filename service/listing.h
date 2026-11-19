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
} wps_entry;

typedef struct {
    int err;
    size_t total;            /* entries read so far */
    bool complete;           /* the whole folder has been read */
    size_t offset, n;
    wps_entry *items;
} wps_page;

typedef void (*wps_page_fn)(void *arg, const wps_page *page);

void wps_listing_init(wpo_loop *loop);
void wps_listing_shutdown(void);

/* Loop thread. `reply` runs on the loop thread, once. */
void wps_listing_request(const char *folder, size_t offset, size_t count, bool details,
                         wps_page_fn reply, void *arg);

/* Something changed in this folder (from the tracker). */
void wps_listing_invalidate(const char *folder);

size_t wps_listing_count(void);

#endif
