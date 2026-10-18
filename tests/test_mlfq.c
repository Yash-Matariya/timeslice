/* The multi-level feedback queue, following the rules in Arpaci-Dusseau,
 * "Operating Systems: Three Easy Pieces", chapter 8: new work starts at the
 * top; using up an allotment moves a thread down, however it was spent; and
 * every so often everything goes back to the top. */
#include "check.h"
#include "timeslice.h"

static void busy(void *arg) { ts_work(*(unsigned *)arg); }

TEST("mlfq: a CPU-bound thread sinks to the bottom queue", mlfq_sinks) {
    static unsigned twenty = 20;
    ts_init(&(ts_config){.policy = TS_MLFQ, .quantum = 2, .mlfq_levels = 3});
    ts_thread *hog = ts_spawn(busy, &twenty, &(ts_attr){.name = "hog"});
    ts_run();
    CHECK_EQ(ts_level(hog), 2);
    ts_shutdown();
}

TEST("mlfq: a new short job runs at once, ahead of a sunk CPU hog", mlfq_short_first) {
    static unsigned thirty = 30, one = 1;
    ts_stats st[2];

    ts_init(&(ts_config){.policy = TS_MLFQ, .quantum = 2, .mlfq_levels = 3});
    ts_spawn(busy, &thirty, &(ts_attr){.name = "hog", .arrival = 0});
    ts_spawn(busy, &one, &(ts_attr){.name = "short", .arrival = 10});
    ts_run();
    ts_stats_get(st, 2);
    ts_shutdown();
    CHECK_EQ(st[1].waiting, 0); /* it arrives at the top; the hog is at the bottom by then */

    ts_init(&(ts_config){.policy = TS_FCFS}); /* the same pair, first come first served */
    ts_spawn(busy, &thirty, &(ts_attr){.name = "hog", .arrival = 0});
    ts_spawn(busy, &one, &(ts_attr){.name = "short", .arrival = 10});
    ts_run();
    ts_stats_get(st, 2);
    ts_shutdown();
    CHECK_EQ(st[1].waiting, 20); /* stuck behind the hog until t=30 */
}

static void gamer(void *arg) {
    (void)arg;
    for (int i = 0; i < 12; i++) {
        ts_work(1); /* never a full quantum in one go... */
        ts_yield(); /* ...hoping to stay at the top */
    }
}

TEST("mlfq: yielding just before the quantum ends does not keep a thread at the top", mlfq_gaming) {
    ts_init(&(ts_config){.policy = TS_MLFQ, .quantum = 2, .mlfq_levels = 3});
    ts_thread *g = ts_spawn(gamer, NULL, &(ts_attr){.name = "gamer"});
    ts_run();
    CHECK_EQ(ts_level(g), 2); /* allotments are counted across slices: 2 ticks, then 4, and it is at the bottom */
    ts_shutdown();
}

static unsigned hog_finish(unsigned boost) {
    static unsigned ten = 10, one = 1;
    ts_init(&(ts_config){.policy = TS_MLFQ, .quantum = 2, .mlfq_levels = 3, .mlfq_boost = boost});
    ts_spawn(busy, &ten, &(ts_attr){.name = "hog", .arrival = 0});
    for (unsigned t = 6; t < 46; t++) ts_spawn(busy, &one, &(ts_attr){.arrival = t}); /* a new job every tick */
    ts_run();
    ts_stats st[1];
    ts_stats_get(st, 1);
    ts_shutdown();
    return st[0].finish;
}

TEST("mlfq: without a boost a stream of new jobs starves the hog; with one it does not", mlfq_boost) {
    unsigned starved = hog_finish(0), boosted = hog_finish(8);
    CHECK_EQ(starved, 50);  /* 6 ticks before the stream, none during it, 4 after it ends at 46 */
    CHECK(boosted < 46);    /* the boost lets it finish while the stream is still running */
}
