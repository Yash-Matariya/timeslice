/* The real clock: a timer signal preempts code that never yields. Each test
 * runs in a child process with a deadline, so a hang (preemption stopped
 * working) is reported as a failure instead of freezing the suite. */
#include "check.h"
#include "timeslice.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void spin(void *arg) { ts_work(*(unsigned *)arg); } /* a busy loop; it never calls ts_yield */

static void two_spinners(void) {
    static unsigned forty = 40;
    ts_init(&(ts_config){.policy = TS_RR, .clock = TS_REAL, .quantum = 2, .tick_usec = 1000});
    ts_spawn(spin, &forty, &(ts_attr){.name = "A"});
    ts_spawn(spin, &forty, &(ts_attr){.name = "B"});
    if (ts_run() != 0) _exit(3);
    ts_slice tl[256];
    size_t n = ts_timeline(tl, 256);
    ts_stats st[2];
    ts_stats_get(st, 2);
    printf("        %zu slices, %lu preemptions, both used %u and %u ticks\n", n, ts_preemptions(), st[0].used, st[1].used);
    int ok = n >= 10 && ts_preemptions() >= 10 && st[0].used == 40 && st[1].used == 40;
    _exit(ok ? 0 : 1);
}

TEST("real: two threads that never yield still take turns", real_spinners) {
    int status = run_isolated(two_spinners, 10);
    CHECK(status != -1);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void yielder(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        ts_yield(); /* comes back through the scheduler, which runs with the timer masked */
        ts_work(3); /* hangs for ever if the timer was not unmasked on the way back */
    }
}

static void yield_then_spin(void) {
    static unsigned sixty = 60;
    ts_init(&(ts_config){.policy = TS_RR, .clock = TS_REAL, .quantum = 2, .tick_usec = 1000});
    ts_spawn(yielder, NULL, &(ts_attr){.name = "Y"});
    ts_spawn(spin, &sixty, &(ts_attr){.name = "S"});
    int rc = ts_run();
    _exit(rc == 0 && ts_preemptions() >= 10 ? 0 : 1);
}

TEST("real: preemption still works after a thread has yielded", real_after_yield) {
    int status = run_isolated(yield_then_spin, 10);
    CHECK(status != -1); /* -1: killed at the deadline, i.e. hung */
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static unsigned woke_at;
static void sleeper(void *arg) {
    (void)arg;
    ts_sleep(20);
    woke_at = ts_now();
}

static void sleep_child(void) {
    ts_init(&(ts_config){.policy = TS_RR, .clock = TS_REAL, .tick_usec = 1000});
    ts_spawn(sleeper, NULL, NULL);
    ts_run();
    _exit(woke_at >= 20 ? 0 : 1);
}

TEST("real: a sleeping thread wakes after its ticks, with the CPU idle meanwhile", real_sleep) {
    int status = run_isolated(sleep_child, 10);
    CHECK(status != -1);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
