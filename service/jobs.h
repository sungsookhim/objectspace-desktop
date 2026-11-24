/*
 * Selections and bulk jobs (design: Scale, Selections and drag and drop,
 * Bulk jobs, Simultaneous operations).
 *
 * A selection is a server-side set: everything in a folder (optionally
 * except some names), or a list of items. Creating one is constant-time,
 * whatever its size; drag and drop passes only its ID.
 *
 * A job moves or copies a selection into a folder on its own thread (at
 * most two run at once), never on the loop:
 *   - moves within a filesystem: one renameat2(RENAME_NOREPLACE) per item;
 *   - copies, and moves across filesystems: reflink (FICLONE) where
 *     possible, else copy_file_range in 64 MB chunks; attributes, mode and
 *     times copied (a copy does not take the source's object ID); each item
 *     is written under a temporary name and renamed into place without
 *     replacing, so nothing is ever overwritten;
 *   - a name clash pauses the job for a decision (skip or rename, once or
 *     for all); items that vanished meanwhile are reported, the rest done;
 *   - progress, pause, resume and cancel (a half-copied item is removed);
 *   - a journal row per job, so after a crash the job is reported as
 *     interrupted and can be resumed (already copied items are skipped);
 *     at logout running jobs pause and resume at the next start.
 */
#ifndef OSD_JOBS_H
#define OSD_JOBS_H

#include <stdbool.h>
#include <stdint.h>

#include "cbor.h"
#include "db.h"
#include "osdo_loop.h"

/* `owner` (the user's WPS identity) makes temporary names unique between
 * users writing into the same folder. */
void osd_jobs_init(osdo_loop *loop, osd_db *db, const char *owner);
/* Pause running jobs for logout and wait for their threads (bounded). */
void osd_jobs_shutdown(bool logout);

/* Selections. Returns the selection ID, or -1. `names` may be NULL. */
int64_t osd_select(const char *folder, bool all, const char *const *names, size_t n);

/* Each path of a selection (its folder's entries but the excepted ones, for
 * "all"), until `fn` returns false; false if the selection is not known or
 * `fn` stopped it. */
bool osd_selection_each(int64_t selection, bool (*fn)(const char *path, void *user), void *user);

/* Start a move or copy of a selection into `dest`. Returns the job ID, or
 * -1 with a message. */
int64_t osd_job_start(const char *kind, int64_t selection, const char *dest, char *err, size_t errlen);
/* Where the items should sit in `dest` (a drop point): by item name. Set
 * right after osd_job_start. When the job ends, osd_on_job_placed gets the
 * final paths (after any renaming) with their positions (loop thread). */
void osd_job_set_positions(int64_t id, const char *const *names, const int64_t *x, const int64_t *y, size_t n);
extern void (*osd_on_job_placed)(const char *dest, char **paths, int64_t *x, int64_t *y, size_t n);

/* Job control. Return false if there is no such job (or the action does
 * not apply). decision: "skip" or "rename". */
bool osd_job_cancel(int64_t id);
bool osd_job_pause(int64_t id);
bool osd_job_resume(int64_t id);
bool osd_job_decide(int64_t id, const char *decision, bool all);

/* Called on the loop thread when a job changes state or makes progress
 * (at most about four times a second per job). */
extern void (*osd_on_job)(int64_t id);

/* Encode a job's status (or all jobs, id < 0) as a CBOR value. */
bool osd_job_status(int64_t id, cbuf *out);

#endif
