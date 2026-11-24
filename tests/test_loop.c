/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Sungsoo Khim */
/* Unit tests for the main loop, queues, workers and watchdog. */
#define _GNU_SOURCE
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "osdo_loop.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- order and fairness */

static char order[4096];
static size_t norder;
static void mark_i(void *a) { (void)a; order[norder++] = 'I'; }
static void mark_r(void *a) { (void)a; order[norder++] = 'R'; }
static void mark_e(void *a) { (void)a; order[norder++] = 'E'; }

TEST(priority_order)
{
    osdo_loop *l = osdo_loop_new(1);
    norder = 0;
    osdo_loop_post(l, OSDO_Q_EVENTS, "e", mark_e, NULL);
    osdo_loop_post(l, OSDO_Q_RESULTS, "r", mark_r, NULL);
    osdo_loop_post(l, OSDO_Q_INTERACTIVE, "i", mark_i, NULL);
    osdo_loop_turn(l, 0);
    order[norder] = 0;
    CHECK_STR(order, "IRE");   /* posted last, served first */
    osdo_loop_free(l);
}

TEST(no_starvation)
{
    /* A flood of interactive requests must not starve events, and a flood
     * of events must not delay interactive requests beyond one turn. */
    osdo_loop *l = osdo_loop_new(1);
    norder = 0;
    for (int i = 0; i < 1000; i++) osdo_loop_post(l, OSDO_Q_INTERACTIVE, "i", mark_i, NULL);
    for (int i = 0; i < 10; i++) osdo_loop_post(l, OSDO_Q_EVENTS, "e", mark_e, NULL);
    osdo_loop_turn(l, 0);
    order[norder] = 0;
    size_t ni = 0, ne = 0;
    for (size_t i = 0; i < norder; i++) { ni += order[i] == 'I'; ne += order[i] == 'E'; }
    CHECK(ni == 64 && ne == 10);   /* default quotas: 64 interactive, events all served */
    osdo_loop_run_until_idle(l, 1000);

    norder = 0;
    for (int i = 0; i < 1000; i++) osdo_loop_post(l, OSDO_Q_EVENTS, "e", mark_e, NULL);
    osdo_loop_post(l, OSDO_Q_INTERACTIVE, "i", mark_i, NULL);
    osdo_loop_turn(l, 0);
    CHECK(norder > 0 && order[0] == 'I');   /* the click goes first despite 1,000 events */
    osdo_loop_free(l);
}

/* ---- deferred work */

struct djob {
    pthread_t worker;
    bool work_on_loop, done_on_loop;
    osdo_loop *loop;
    atomic_int *count;
};

static void djob_work(void *a)
{
    struct djob *j = a;
    j->worker = pthread_self();
    j->work_on_loop = osdo_loop_on_loop_thread(j->loop);
}

static void djob_done(void *a)
{
    struct djob *j = a;
    j->done_on_loop = osdo_loop_on_loop_thread(j->loop);
    atomic_fetch_add(j->count, 1);
}

TEST(deferred_work)
{
    osdo_loop *l = osdo_loop_new(4);
    osdo_loop_turn(l, 0);   /* fix the loop thread */
    atomic_int count = 0;
    static struct djob jobs[1000];
    for (int i = 0; i < 1000; i++) {
        jobs[i] = (struct djob){ .loop = l, .count = &count };
        osdo_defer(l, "job", djob_work, djob_done, &jobs[i]);
    }
    CHECK(osdo_loop_run_until_idle(l, 5000));
    CHECK(atomic_load(&count) == 1000);
    int bad = 0;
    for (int i = 0; i < 1000; i++) bad += jobs[i].work_on_loop || !jobs[i].done_on_loop;
    CHECK(bad == 0);   /* work on workers, completion on the loop */
    osdo_loop_stats st;
    osdo_loop_get_stats(l, &st);
    CHECK(st.deferred == 1000 && st.tasks[OSDO_Q_RESULTS] == 1000);
    osdo_loop_free(l);
}

/* ---- the loop stays responsive while workers do slow work */

struct ping { double posted; double served; };
static void ping_serve(void *a) { ((struct ping *)a)->served = now_ms(); }
static void slow_work(void *a) { (void)a; sleep_ms(300); }

struct pinger { osdo_loop *l; struct ping pings[20]; };
static void *pinger_main(void *a)
{
    struct pinger *p = a;
    for (int i = 0; i < 20; i++) {
        sleep_ms(10);
        p->pings[i].posted = now_ms();
        osdo_loop_post(p->l, OSDO_Q_INTERACTIVE, "ping", ping_serve, &p->pings[i]);
    }
    return NULL;
}

TEST(responsive_during_slow_work)
{
    osdo_loop *l = osdo_loop_new(2);
    for (int i = 0; i < 4; i++) osdo_defer(l, "slow", slow_work, NULL, NULL);
    static struct pinger p;
    memset(&p, 0, sizeof p);
    p.l = l;
    pthread_t th;
    pthread_create(&th, NULL, pinger_main, &p);
    double end = now_ms() + 2000;
    while (now_ms() < end) {
        osdo_loop_turn(l, 20);
        int served = 0;
        for (int i = 0; i < 20; i++) served += p.pings[i].served > 0;
        if (served == 20) break;
    }
    pthread_join(th, NULL);
    double worst = 0;
    for (int i = 0; i < 20; i++) {
        double lat = p.pings[i].served - p.pings[i].posted;
        if (p.pings[i].served <= 0) lat = 1e9;
        if (lat > worst) worst = lat;
    }
    printf("      worst interactive latency while workers were busy: %.2f ms\n", worst);
    CHECK(worst < 20.0);
    osdo_loop_run_until_idle(l, 3000);
    osdo_loop_free(l);
}

/* ---- watchdog */

static char wd_label[64];
static double wd_ms;
static void on_watchdog(const char *label, double ms, void *user)
{
    (void)user;
    snprintf(wd_label, sizeof wd_label, "%s", label);
    wd_ms = ms;
}
static void blocking_task(void *a) { (void)a; sleep_ms(80); }
static void quick_task(void *a) { (void)a; }

TEST(watchdog)
{
    osdo_loop *l = osdo_loop_new(1);
    osdo_loop_set_watchdog(l, 50.0, on_watchdog, NULL);
    osdo_loop_post(l, OSDO_Q_INTERACTIVE, "quick", quick_task, NULL);
    osdo_loop_turn(l, 0);
    CHECK(wd_label[0] == 0);
    osdo_loop_post(l, OSDO_Q_INTERACTIVE, "OsdFolder.osdPopulate (blocking!)", blocking_task, NULL);
    osdo_loop_turn(l, 0);
    CHECK_STR(wd_label, "OsdFolder.osdPopulate (blocking!)");
    CHECK(wd_ms >= 50.0);
    osdo_loop_stats st;
    osdo_loop_get_stats(l, &st);
    CHECK(st.watchdog_hits == 1);
    CHECK_STR(st.max_slice_label, "OsdFolder.osdPopulate (blocking!)");
    osdo_loop_free(l);
}

/* ---- file descriptors, wake-ups, quit */

static int fd_hits;
static void on_fd(int fd, short rev, void *a)
{
    (void)rev; (void)a;
    char buf[64];
    ssize_t r = read(fd, buf, sizeof buf);
    if (r > 0) fd_hits++;
}

TEST(watch_fd)
{
    osdo_loop *l = osdo_loop_new(1);
    int p[2];
    CHECK(pipe(p) == 0);
    osdo_loop_watch_fd(l, p[0], POLLIN, on_fd, NULL);
    CHECK(write(p[1], "x", 1) == 1);
    osdo_loop_turn(l, 1000);
    CHECK(fd_hits == 1);
    osdo_loop_unwatch_fd(l, p[0]);
    CHECK(write(p[1], "y", 1) == 1);
    osdo_loop_turn(l, 0);
    CHECK(fd_hits == 1);
    close(p[0]);
    close(p[1]);
    osdo_loop_free(l);
}

static void *quitter(void *a)
{
    sleep_ms(50);
    osdo_loop_quit(a);
    return NULL;
}

TEST(wake_and_quit_from_other_threads)
{
    osdo_loop *l = osdo_loop_new(1);
    pthread_t th;
    double t0 = now_ms();
    pthread_create(&th, NULL, quitter, l);
    osdo_loop_run(l);   /* blocks in poll until the other thread quits it */
    pthread_join(th, NULL);
    double took = now_ms() - t0;
    CHECK(took >= 40 && took < 500);
    osdo_loop_free(l);
}

int main(void)
{
    RUN(priority_order);
    RUN(no_starvation);
    RUN(deferred_work);
    RUN(responsive_during_slow_work);
    RUN(watchdog);
    RUN(watch_fd);
    RUN(wake_and_quit_from_other_threads);
    return CHECK_SUMMARY();
}
