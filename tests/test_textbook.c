/* The worked examples from Silberschatz, Galvin and Gagne, "Operating System
 * Concepts", chapter 5: the standard answers every OS course checks against.
 * Each one runs through both the textbook calculation and the real scheduler. */
#include "check.h"
#include "helpers.h"

#include <math.h>

static int both_equal(ts_policy policy, unsigned q, const tb_proc *p, size_t n, double want_avg_wait) {
    run_result a, b;
    if (run_textbook(policy, q, p, n, &a) != 0 || run_scheduler(policy, q, p, n, &b) != 0) return 0;
    if (!same_run(&a, &b, n)) {
        print_run("textbook", &a, p);
        print_run("scheduler", &b, p);
        return 0;
    }
    return fabs(avg_waiting(&a, p, n) - want_avg_wait) < 1e-9;
}

TEST("textbook: FCFS, P1=24 P2=3 P3=3, average wait 17", fcfs_book) {
    tb_proc p[] = {{"P1", 0, 24, 0}, {"P2", 0, 3, 0}, {"P3", 0, 3, 0}};
    CHECK(both_equal(TS_FCFS, 0, p, 3, 17.0));
    tb_proc q[] = {{"P2", 0, 3, 0}, {"P3", 0, 3, 0}, {"P1", 0, 24, 0}}; /* the convoy effect, reversed */
    CHECK(both_equal(TS_FCFS, 0, q, 3, 3.0));
}

TEST("textbook: SJF, 6 8 7 3, average wait 7", sjf_book) {
    tb_proc p[] = {{"P1", 0, 6, 0}, {"P2", 0, 8, 0}, {"P3", 0, 7, 0}, {"P4", 0, 3, 0}};
    CHECK(both_equal(TS_SJF, 0, p, 4, 7.0));
}

TEST("textbook: SRTF with arrivals 0 1 2 3, average wait 6.5", srtf_book) {
    tb_proc p[] = {{"P1", 0, 8, 0}, {"P2", 1, 4, 0}, {"P3", 2, 9, 0}, {"P4", 3, 5, 0}};
    CHECK(both_equal(TS_SRTF, 0, p, 4, 6.5));
    run_result r;
    CHECK(run_scheduler(TS_SRTF, 0, p, 4, &r) == 0);
    /* P1 0-1, P2 1-5, P4 5-10, P1 10-17, P3 17-26 */
    CHECK_EQ(r.nslices, 5);
    CHECK_EQ(r.slices[0].thread, 0);
    CHECK_EQ(r.slices[1].thread, 1);
    CHECK_EQ(r.slices[2].thread, 3);
    CHECK_EQ(r.slices[4].end, 26);
}

TEST("textbook: Round Robin q=4, 24 3 3, average wait 17/3", rr_book) {
    tb_proc p[] = {{"P1", 0, 24, 0}, {"P2", 0, 3, 0}, {"P3", 0, 3, 0}};
    CHECK(both_equal(TS_RR, 4, p, 3, 17.0 / 3.0));
}

TEST("textbook: non-preemptive priority, average wait 8.2", prio_book) {
    tb_proc p[] = {{"P1", 0, 10, 3}, {"P2", 0, 1, 1}, {"P3", 0, 2, 4}, {"P4", 0, 1, 5}, {"P5", 0, 5, 2}};
    CHECK(both_equal(TS_PRIORITY, 0, p, 5, 8.2));
}

TEST("textbook: RR, a process arriving as a quantum expires queues ahead of the preempted one", rr_tie) {
    /* At t=2, A's quantum ends exactly as B arrives. B goes first, then A. */
    tb_proc p[] = {{"A", 0, 4, 0}, {"B", 2, 2, 0}};
    run_result a, b;
    CHECK(run_textbook(TS_RR, 2, p, 2, &a) == 0);
    CHECK(run_scheduler(TS_RR, 2, p, 2, &b) == 0);
    CHECK(same_run(&a, &b, 2));
    CHECK_EQ(b.slices[1].thread, 1); /* B at 2-4 */
    CHECK_EQ(b.slices[2].thread, 0); /* A at 4-6 */
}

TEST("textbook: SRTF, an arrival with equal remaining time does not preempt", srtf_tie) {
    tb_proc p[] = {{"A", 0, 4, 0}, {"B", 1, 3, 0}}; /* at t=1 A has 3 left, B needs 3 */
    run_result r;
    CHECK(run_scheduler(TS_SRTF, 0, p, 2, &r) == 0);
    CHECK_EQ(r.nslices, 2);
    CHECK_EQ(r.slices[0].end, 4); /* A runs 0-4 uninterrupted */
}

TEST("textbook: idle gaps appear in the timeline", idle_gap) {
    tb_proc p[] = {{"A", 0, 2, 0}, {"B", 5, 1, 0}};
    run_result a, b;
    CHECK(run_textbook(TS_FCFS, 0, p, 2, &a) == 0);
    CHECK(run_scheduler(TS_FCFS, 0, p, 2, &b) == 0);
    CHECK(same_run(&a, &b, 2));
    CHECK_EQ(b.slices[1].thread, -1);
    CHECK_EQ(b.slices[1].start, 2);
    CHECK_EQ(b.slices[1].end, 5);
}
