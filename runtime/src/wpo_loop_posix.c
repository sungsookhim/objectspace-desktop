/*
 * wpo_loop for Linux: poll() on an eventfd plus watched descriptors,
 * three task queues, and a pthread worker pool.
 */
#define _GNU_SOURCE
#include "wpo_loop.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

struct task {
    struct task *next;
    const char *label;
    wpo_task_fn fn;          /* loop task, or `done` for deferred work */
    wpo_task_fn work;        /* deferred work, run on a worker */
    void *arg;
    bool deferred;           /* counts toward `inflight` until `fn` has run */
};

struct fifo {
    struct task *head, *tail;
    size_t len;
};

struct watch {
    int fd;
    short events;
    wpo_fd_fn fn;
    void *arg;
};

struct wpo_loop {
    pthread_mutex_t mu;            /* guards queues, jobs, flags */
    pthread_cond_t job_cv;
    struct fifo q[WPO_Q_COUNT];
    unsigned quota[WPO_Q_COUNT];
    struct fifo jobs;              /* deferred work waiting for a worker */
    size_t inflight;               /* deferred items not yet completed on the loop */
    bool stopping, quit;
    int wake_fd;
    pthread_t loop_thread;
    bool loop_thread_set;

    pthread_t *workers;
    unsigned nworkers;

    struct watch *watches;
    size_t nwatches, capwatches;

    double budget_ms;
    wpo_watchdog_fn watchdog;
    void *watchdog_user;
    wpo_loop_stats stats;
};

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void fifo_push(struct fifo *f, struct task *t)
{
    t->next = NULL;
    if (f->tail) f->tail->next = t;
    else f->head = t;
    f->tail = t;
    f->len++;
}

static struct task *fifo_pop(struct fifo *f)
{
    struct task *t = f->head;
    if (!t) return NULL;
    f->head = t->next;
    if (!f->head) f->tail = NULL;
    f->len--;
    return t;
}

static void fifo_drop(struct fifo *f)
{
    struct task *t;
    while ((t = fifo_pop(f))) free(t);
}

static void wake(wpo_loop *l)
{
    uint64_t one = 1;
    ssize_t r = write(l->wake_fd, &one, sizeof one);
    (void)r;
}

static void *worker_main(void *arg)
{
    wpo_loop *l = arg;
    pthread_mutex_lock(&l->mu);
    for (;;) {
        while (!l->stopping && !l->jobs.head) pthread_cond_wait(&l->job_cv, &l->mu);
        if (l->stopping) break;
        struct task *t = fifo_pop(&l->jobs);
        pthread_mutex_unlock(&l->mu);
        if (t->work) t->work(t->arg);
        pthread_mutex_lock(&l->mu);
        l->stats.deferred++;
        /* Hand the completion to the loop thread. */
        fifo_push(&l->q[WPO_Q_RESULTS], t);
        pthread_mutex_unlock(&l->mu);
        wake(l);
        pthread_mutex_lock(&l->mu);
    }
    pthread_mutex_unlock(&l->mu);
    return NULL;
}

wpo_loop *wpo_loop_new(unsigned workers)
{
    wpo_loop *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->job_cv, NULL);
    l->quota[WPO_Q_INTERACTIVE] = 64;
    l->quota[WPO_Q_RESULTS] = 32;
    l->quota[WPO_Q_EVENTS] = 32;
    l->budget_ms = 50.0;
    l->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (l->wake_fd < 0) { free(l); return NULL; }
    if (workers < 1) workers = 1;
    l->workers = calloc(workers, sizeof *l->workers);
    for (unsigned i = 0; i < workers; i++)
        if (pthread_create(&l->workers[i], NULL, worker_main, l) == 0) l->nworkers++;
    return l;
}

void wpo_loop_free(wpo_loop *l)
{
    if (!l) return;
    pthread_mutex_lock(&l->mu);
    l->stopping = true;
    pthread_cond_broadcast(&l->job_cv);
    pthread_mutex_unlock(&l->mu);
    for (unsigned i = 0; i < l->nworkers; i++) pthread_join(l->workers[i], NULL);
    for (int q = 0; q < WPO_Q_COUNT; q++) fifo_drop(&l->q[q]);
    fifo_drop(&l->jobs);
    close(l->wake_fd);
    free(l->workers);
    free(l->watches);
    pthread_cond_destroy(&l->job_cv);
    pthread_mutex_destroy(&l->mu);
    free(l);
}

void wpo_loop_post(wpo_loop *l, wpo_queue q, const char *label, wpo_task_fn fn, void *arg)
{
    if (q >= WPO_Q_COUNT || !fn) return;
    struct task *t = calloc(1, sizeof *t);
    if (!t) return;
    t->label = label ? label : "task";
    t->fn = fn;
    t->arg = arg;
    pthread_mutex_lock(&l->mu);
    fifo_push(&l->q[q], t);
    pthread_mutex_unlock(&l->mu);
    wake(l);
}

static void noop(void *arg) { (void)arg; }

void wpo_defer(wpo_loop *l, const char *label, wpo_task_fn work, wpo_task_fn done, void *arg)
{
    struct task *t = calloc(1, sizeof *t);
    if (!t) return;
    t->label = label ? label : "deferred";
    t->work = work;
    t->fn = done ? done : noop;
    t->arg = arg;
    t->deferred = true;
    pthread_mutex_lock(&l->mu);
    l->inflight++;
    fifo_push(&l->jobs, t);
    pthread_cond_signal(&l->job_cv);
    pthread_mutex_unlock(&l->mu);
}

int wpo_loop_watch_fd(wpo_loop *l, int fd, short events, wpo_fd_fn fn, void *arg)
{
    for (size_t i = 0; i < l->nwatches; i++)
        if (l->watches[i].fd == fd) {
            l->watches[i] = (struct watch){ fd, events, fn, arg };
            return 0;
        }
    if (l->nwatches == l->capwatches) {
        size_t ncap = l->capwatches ? l->capwatches * 2 : 8;
        struct watch *n = realloc(l->watches, ncap * sizeof *n);
        if (!n) return -1;
        l->watches = n;
        l->capwatches = ncap;
    }
    l->watches[l->nwatches++] = (struct watch){ fd, events, fn, arg };
    return 0;
}

void wpo_loop_unwatch_fd(wpo_loop *l, int fd)
{
    for (size_t i = 0; i < l->nwatches; i++)
        if (l->watches[i].fd == fd) {
            l->watches[i] = l->watches[--l->nwatches];
            return;
        }
}

void wpo_loop_set_quota(wpo_loop *l, wpo_queue q, unsigned per_turn)
{
    if (q < WPO_Q_COUNT) l->quota[q] = per_turn ? per_turn : 1;
}

void wpo_loop_set_watchdog(wpo_loop *l, double budget_ms, wpo_watchdog_fn fn, void *user)
{
    l->budget_ms = budget_ms;
    l->watchdog = fn;
    l->watchdog_user = user;
}

/* Time one slice of loop-thread work and report it if over budget. */
static void timed(wpo_loop *l, const char *label, double start)
{
    double ms = now_ms() - start;
    if (ms > l->stats.max_slice_ms) {
        l->stats.max_slice_ms = ms;
        snprintf(l->stats.max_slice_label, sizeof l->stats.max_slice_label, "%s", label);
    }
    if (ms > l->budget_ms) {
        l->stats.watchdog_hits++;
        if (l->watchdog) l->watchdog(label, ms, l->watchdog_user);
    }
}

bool wpo_loop_turn(wpo_loop *l, int timeout_ms)
{
    if (!l->loop_thread_set) {
        l->loop_thread = pthread_self();
        l->loop_thread_set = true;
    }
    pthread_mutex_lock(&l->mu);
    bool pending = false;
    for (int q = 0; q < WPO_Q_COUNT; q++) pending |= l->q[q].head != NULL;
    pthread_mutex_unlock(&l->mu);

    size_t n = l->nwatches + 1;
    struct pollfd *pf = calloc(n, sizeof *pf);
    struct watch *snap = calloc(n, sizeof *snap);
    if (!pf || !snap) { free(pf); free(snap); return false; }
    pf[0].fd = l->wake_fd;
    pf[0].events = POLLIN;
    for (size_t i = 0; i < l->nwatches; i++) {
        snap[i + 1] = l->watches[i];
        pf[i + 1].fd = l->watches[i].fd;
        pf[i + 1].events = l->watches[i].events;
    }
    int r = poll(pf, (nfds_t)n, pending ? 0 : timeout_ms);
    bool ran = false;
    if (r > 0) {
        if (pf[0].revents & POLLIN) {
            uint64_t v;
            ssize_t rr = read(l->wake_fd, &v, sizeof v);
            (void)rr;
        }
        for (size_t i = 1; i < n; i++)
            if (pf[i].revents) {
                double t0 = now_ms();
                snap[i].fn(snap[i].fd, pf[i].revents, snap[i].arg);
                timed(l, "fd callback", t0);
                l->stats.fd_callbacks++;
                ran = true;
            }
    }
    free(pf);
    free(snap);

    /* Serve each queue up to its quota, interactive first. */
    for (int q = 0; q < WPO_Q_COUNT; q++) {
        for (unsigned k = 0; k < l->quota[q]; k++) {
            pthread_mutex_lock(&l->mu);
            struct task *t = fifo_pop(&l->q[q]);
            pthread_mutex_unlock(&l->mu);
            if (!t) break;
            double t0 = now_ms();
            t->fn(t->arg);
            timed(l, t->label, t0);
            l->stats.tasks[q]++;
            if (t->deferred) {
                pthread_mutex_lock(&l->mu);
                l->inflight--;
                pthread_mutex_unlock(&l->mu);
            }
            free(t);
            ran = true;
        }
    }
    return ran;
}

void wpo_loop_run(wpo_loop *l)
{
    l->quit = false;
    for (;;) {
        pthread_mutex_lock(&l->mu);
        bool q = l->quit;
        pthread_mutex_unlock(&l->mu);
        if (q) break;
        wpo_loop_turn(l, 1000);
    }
}

bool wpo_loop_run_until_idle(wpo_loop *l, int timeout_ms)
{
    double end = now_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&l->mu);
        bool idle = l->inflight == 0;
        for (int q = 0; q < WPO_Q_COUNT; q++) idle &= l->q[q].head == NULL;
        pthread_mutex_unlock(&l->mu);
        if (idle) return true;
        if (now_ms() > end) return false;
        wpo_loop_turn(l, 10);
    }
}

void wpo_loop_quit(wpo_loop *l)
{
    pthread_mutex_lock(&l->mu);
    l->quit = true;
    pthread_mutex_unlock(&l->mu);
    wake(l);
}

bool wpo_loop_on_loop_thread(const wpo_loop *l)
{
    return l->loop_thread_set && pthread_equal(l->loop_thread, pthread_self());
}

void wpo_loop_get_stats(const wpo_loop *l, wpo_loop_stats *out)
{
    pthread_mutex_lock((pthread_mutex_t *)&l->mu);
    *out = l->stats;
    pthread_mutex_unlock((pthread_mutex_t *)&l->mu);
}
