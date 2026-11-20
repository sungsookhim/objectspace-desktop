/*
 * Find (G4 search; OS/2's Find): objects by name under a folder.
 *
 * Each search walks the folder (and its subfolders, if asked) on a thread
 * of its own, so a search of a large tree never holds up the loop or the
 * workers. Matches go to the loop in batches as they are found, so a
 * client shows them while the walk goes on; a search can be cancelled at
 * any time (a search field starts a new one at each key). Names match
 * without regard to case: a pattern with * ? or [ as a wildcard pattern,
 * any other as a part of the name. Hidden entries (dot names) are left
 * out unless the pattern starts with a dot; symbolic links are not
 * followed into.
 */
#ifndef WPS_FIND_H
#define WPS_FIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wpo_loop.h"

typedef enum { WPS_FIND_ANY, WPS_FIND_FOLDER, WPS_FIND_FILE, WPS_FIND_PROGRAM } wps_find_kind;

typedef struct {
    const char *path;
    const char *type;        /* "folder", "file", "link", "other" */
    int64_t size, mtime;
} wps_found;

/* A batch, on the loop thread. `done`: the last call (the search ended,
 * was cancelled, or reached its limit: `truncated`). `scanned`: entries
 * looked at so far. `err`: the folder could not be read (done). */
typedef void (*wps_find_fn)(void *arg, int64_t find, const wps_found *items, size_t n, bool done, uint64_t scanned,
                            bool truncated, int err);

void wps_find_init(wpo_loop *loop);
/* Cancels the searches and waits for their threads. */
void wps_find_shutdown(void);

/* Loop thread. Returns the search's ID (> 0); `fn` runs on the loop
 * thread for every batch, the last with done set, after which `arg` is
 * not used again. */
int64_t wps_find_start(const char *folder, const char *pattern, bool subfolders, wps_find_kind kind, size_t limit,
                       wps_find_fn fn, void *arg);

/* Loop thread: stop a search; its last batch still comes (done). False if
 * there is no such search running. */
bool wps_find_cancel(int64_t find);

/* The matching rule alone (tests). */
bool wps_find_match(const char *name, const char *pattern);

#endif
