/*
 * wpo_loop - the service's main loop and worker pool.
 *
 * The rule (design: Responsiveness): the loop thread does only bounded work.
 * Anything that touches the disk, the network or an unknown number of items
 * runs on a worker and reports back.
 *
 *   - Three queues, served in a fixed order every turn, each with a quota
 *     of tasks per turn: interactive requests first, then results of
 *     deferred work, then filesystem events. A flood in one queue cannot
 *     starve the others.
 *   - wpo_defer() runs `work` on a worker thread and then `done` on the
 *     loop thread (through the results queue). Only `done` may touch objects.
 *   - A watchdog times every task and file-descriptor callback on the loop
 *     thread and reports any that exceed the budget.
 *   - File descriptors can be watched (sockets, fanotify, inotify).
 *
 * POSIX implementation: wpo_loop_posix.c (poll, eventfd, pthreads).
 */
#ifndef WPO_LOOP_H
#define WPO_LOOP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct wpo_loop wpo_loop;

typedef enum {
    WPO_Q_INTERACTIVE = 0,   /* client requests */
    WPO_Q_RESULTS = 1,       /* completions of deferred work */
    WPO_Q_EVENTS = 2,        /* filesystem events */
    WPO_Q_COUNT
} wpo_queue;

typedef void (*wpo_task_fn)(void *arg);
typedef void (*wpo_fd_fn)(int fd, short revents, void *arg);
/* Called on the loop thread after a slice over budget. */
typedef void (*wpo_watchdog_fn)(const char *label, double ms, void *user);

typedef struct {
    uint64_t tasks[WPO_Q_COUNT];   /* tasks run, per queue */
    uint64_t fd_callbacks;
    uint64_t deferred;             /* work items completed by workers */
    uint64_t watchdog_hits;
    double max_slice_ms;
    char max_slice_label[64];
} wpo_loop_stats;

/* `workers`: number of worker threads (at least 1). */
wpo_loop *wpo_loop_new(unsigned workers);
/* Stops the workers (after their current item) and frees the loop.
 * Tasks still queued are dropped without running. */
void wpo_loop_free(wpo_loop *loop);

/* Queue a task. Safe from any thread. `label` must stay valid until the
 * task has run; it names the task in watchdog reports. */
void wpo_loop_post(wpo_loop *loop, wpo_queue q, const char *label, wpo_task_fn fn, void *arg);

/* Run `work(arg)` on a worker, then `done(arg)` on the loop thread.
 * Safe from any thread. Either function may be NULL. */
void wpo_defer(wpo_loop *loop, const char *label, wpo_task_fn work, wpo_task_fn done, void *arg);

/* Watch a file descriptor (loop thread only). One callback per fd. */
int wpo_loop_watch_fd(wpo_loop *loop, int fd, short events, wpo_fd_fn fn, void *arg);
void wpo_loop_unwatch_fd(wpo_loop *loop, int fd);

/* Tasks taken from a queue per turn (defaults: 64, 32, 32). */
void wpo_loop_set_quota(wpo_loop *loop, wpo_queue q, unsigned per_turn);
/* Budget for one task or callback on the loop thread (default 50 ms). */
void wpo_loop_set_watchdog(wpo_loop *loop, double budget_ms, wpo_watchdog_fn fn, void *user);

/* Run until wpo_loop_quit(). */
void wpo_loop_run(wpo_loop *loop);
/* One turn: wait up to `timeout_ms` (-1: forever) for work, then serve the
 * fds and the queues once. Returns true if anything ran. */
bool wpo_loop_turn(wpo_loop *loop, int timeout_ms);
/* Run turns until all queues are empty and no deferred work is in flight,
 * or `timeout_ms` passes. Returns true if idle. For tests. */
bool wpo_loop_run_until_idle(wpo_loop *loop, int timeout_ms);
void wpo_loop_quit(wpo_loop *loop);   /* safe from any thread */

bool wpo_loop_on_loop_thread(const wpo_loop *loop);
void wpo_loop_get_stats(const wpo_loop *loop, wpo_loop_stats *out);

#endif
