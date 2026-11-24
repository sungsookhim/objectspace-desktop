/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/*
 * osdo_loop - the service's main loop and worker pool.
 *
 * The rule (design: Responsiveness): the loop thread does only bounded work.
 * Anything that touches the disk, the network or an unknown number of items
 * runs on a worker and reports back.
 *
 *   - Three queues, served in a fixed order every turn, each with a quota
 *     of tasks per turn: interactive requests first, then results of
 *     deferred work, then filesystem events. A flood in one queue cannot
 *     starve the others.
 *   - osdo_defer() runs `work` on a worker thread and then `done` on the
 *     loop thread (through the results queue). Only `done` may touch objects.
 *   - A watchdog times every task and file-descriptor callback on the loop
 *     thread and reports any that exceed the budget.
 *   - File descriptors can be watched (sockets, fanotify, inotify).
 *
 * POSIX implementation: osdo_loop_posix.c (poll, eventfd, pthreads).
 */
#ifndef OSDO_LOOP_H
#define OSDO_LOOP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct osdo_loop osdo_loop;

typedef enum {
    OSDO_Q_INTERACTIVE = 0,   /* client requests */
    OSDO_Q_RESULTS = 1,       /* completions of deferred work */
    OSDO_Q_EVENTS = 2,        /* filesystem events */
    OSDO_Q_COUNT
} osdo_queue;

typedef void (*osdo_task_fn)(void *arg);
typedef void (*osdo_fd_fn)(int fd, short revents, void *arg);
/* Called on the loop thread after a slice over budget. */
typedef void (*osdo_watchdog_fn)(const char *label, double ms, void *user);

typedef struct {
    uint64_t tasks[OSDO_Q_COUNT];   /* tasks run, per queue */
    uint64_t fd_callbacks;
    uint64_t deferred;             /* work items completed by workers */
    uint64_t watchdog_hits;
    double max_slice_ms;
    char max_slice_label[64];
} osdo_loop_stats;

/* `workers`: number of worker threads (at least 1). */
osdo_loop *osdo_loop_new(unsigned workers);
/* Stops the workers (after their current item) and frees the loop.
 * Tasks still queued are dropped without running. */
void osdo_loop_free(osdo_loop *loop);

/* Queue a task. Safe from any thread. `label` names the task in watchdog
 * reports; it is copied (up to 47 characters). */
void osdo_loop_post(osdo_loop *loop, osdo_queue q, const char *label, osdo_task_fn fn, void *arg);

/* Run `work(arg)` on a worker, then `done(arg)` on the loop thread.
 * Safe from any thread. Either function may be NULL. */
void osdo_defer(osdo_loop *loop, const char *label, osdo_task_fn work, osdo_task_fn done, void *arg);

/* Watch a file descriptor (loop thread only). One callback per fd. */
int osdo_loop_watch_fd(osdo_loop *loop, int fd, short events, osdo_fd_fn fn, void *arg);
void osdo_loop_unwatch_fd(osdo_loop *loop, int fd);

/* Tasks taken from a queue per turn (defaults: 64, 32, 32). */
void osdo_loop_set_quota(osdo_loop *loop, osdo_queue q, unsigned per_turn);
/* Budget for one task or callback on the loop thread (default 50 ms). */
void osdo_loop_set_watchdog(osdo_loop *loop, double budget_ms, osdo_watchdog_fn fn, void *user);

/* Run until osdo_loop_quit(). */
void osdo_loop_run(osdo_loop *loop);
/* One turn: wait up to `timeout_ms` (-1: forever) for work, then serve the
 * fds and the queues once. Returns true if anything ran. */
bool osdo_loop_turn(osdo_loop *loop, int timeout_ms);
/* Run turns until all queues are empty and no deferred work is in flight,
 * or `timeout_ms` passes. Returns true if idle. For tests. */
bool osdo_loop_run_until_idle(osdo_loop *loop, int timeout_ms);
void osdo_loop_quit(osdo_loop *loop);   /* safe from any thread */

bool osdo_loop_on_loop_thread(const osdo_loop *loop);
void osdo_loop_get_stats(const osdo_loop *loop, osdo_loop_stats *out);

#endif
