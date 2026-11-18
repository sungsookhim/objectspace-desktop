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
#ifndef WPS_JOBS_H
#define WPS_JOBS_H

#include <stdbool.h>
#include <stdint.h>

#include "cbor.h"
#include "db.h"
#include "wpo_loop.h"

/* `owner` (the user's WPS identity) makes temporary names unique between
 * users writing into the same folder. */
void wps_jobs_init(wpo_loop *loop, wps_db *db, const char *owner);
/* Pause running jobs for logout and wait for their threads (bounded). */
void wps_jobs_shutdown(bool logout);

/* Selections. Returns the selection ID, or -1. `names` may be NULL. */
int64_t wps_select(const char *folder, bool all, const char *const *names, size_t n);

/* Start a move or copy of a selection into `dest`. Returns the job ID, or
 * -1 with a message. */
int64_t wps_job_start(const char *kind, int64_t selection, const char *dest, char *err, size_t errlen);

/* Job control. Return false if there is no such job (or the action does
 * not apply). decision: "skip" or "rename". */
bool wps_job_cancel(int64_t id);
bool wps_job_pause(int64_t id);
bool wps_job_resume(int64_t id);
bool wps_job_decide(int64_t id, const char *decision, bool all);

/* Encode a job's status (or all jobs, id < 0) as a CBOR value. */
bool wps_job_status(int64_t id, cbuf *out);

#endif
