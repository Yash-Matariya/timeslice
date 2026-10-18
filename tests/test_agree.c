/* The real scheduler against the textbook calculation, on thousands of random
 * process tables per policy. Two implementations that share no code must
 * produce the same Gantt chart, tick for tick. */
#include "check.h"
#include "helpers.h"

#include <stdlib.h>

static unsigned rng_state;
static unsigned rnd(unsigned n) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) % n;
}

static int agree(ts_policy policy, unsigned quantum, int cases) {
    static const char *names[] = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
    rng_state = 12345u + (unsigned)policy * 1000u + quantum;
    for (int c = 0; c < cases; c++) {
        size_t n = 1 + rnd(8);
        tb_proc p[8];
        for (size_t i = 0; i < n; i++)
            p[i] = (tb_proc){names[i], rnd(12), 1 + rnd(9), (int)(1 + rnd(5))};
        run_result a, b;
        if (run_textbook(policy, quantum, p, n, &a) != 0 || run_scheduler(policy, quantum, p, n, &b) != 0) return 0;
        if (!same_run(&a, &b, n)) {
            printf("        %s q=%u, case %d:", ts_policy_name(policy), quantum, c);
            for (size_t i = 0; i < n; i++) printf(" %s(arr %u, burst %u, prio %d)", p[i].name, p[i].arrival, p[i].burst, p[i].priority);
            printf("\n");
            print_run("textbook", &a, p);
            print_run("scheduler", &b, p);
            return 0;
        }
    }
    return 1;
}

TEST("agree: FCFS, 3000 random workloads", agree_fcfs) { CHECK(agree(TS_FCFS, 0, 3000)); }
TEST("agree: SJF, 3000 random workloads", agree_sjf) { CHECK(agree(TS_SJF, 0, 3000)); }
TEST("agree: SRTF, 3000 random workloads", agree_srtf) { CHECK(agree(TS_SRTF, 0, 3000)); }
TEST("agree: priority, 3000 random workloads", agree_prio) { CHECK(agree(TS_PRIORITY, 0, 3000)); }
TEST("agree: preemptive priority, 3000 random workloads", agree_prio_p) { CHECK(agree(TS_PRIORITY_PREEMPTIVE, 0, 3000)); }
TEST("agree: round robin q=1,2,3,5, 3000 random workloads each", agree_rr) {
    CHECK(agree(TS_RR, 1, 3000));
    CHECK(agree(TS_RR, 2, 3000));
    CHECK(agree(TS_RR, 3, 3000));
    CHECK(agree(TS_RR, 5, 3000));
}
