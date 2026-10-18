#include "helpers.h"

#include <stdio.h>
#include <string.h>

static void cpu_bound(void *arg) { ts_work(*(unsigned *)arg); }

int run_scheduler(ts_policy policy, unsigned quantum, const tb_proc *p, size_t n, run_result *out) {
    static unsigned bursts[MAX_PROCS];
    memset(out, 0, sizeof *out);
    ts_config cfg = {.policy = policy, .clock = TS_VIRTUAL, .quantum = quantum};
    if (ts_init(&cfg) != 0) return -1;
    for (size_t i = 0; i < n; i++) {
        bursts[i] = p[i].burst;
        ts_attr a = {.name = p[i].name, .arrival = p[i].arrival, .burst = p[i].burst, .priority = p[i].priority};
        if (!ts_spawn(cpu_bound, &bursts[i], &a)) return -1;
    }
    int rc = ts_run();
    out->nslices = ts_timeline(out->slices, MAX_SLICES);
    ts_stats st[MAX_PROCS];
    size_t k = ts_stats_get(st, MAX_PROCS);
    for (size_t i = 0; i < k; i++) {
        out->finish[i] = st[i].finish;
        out->first_run[i] = st[i].first_run;
    }
    ts_shutdown();
    return rc;
}

int run_textbook(ts_policy policy, unsigned quantum, const tb_proc *p, size_t n, run_result *out) {
    memset(out, 0, sizeof *out);
    tb_result res[MAX_PROCS];
    out->nslices = tb_schedule(policy, quantum, p, n, out->slices, MAX_SLICES, res);
    for (size_t i = 0; i < n; i++) {
        out->finish[i] = res[i].finish;
        out->first_run[i] = res[i].first_run;
    }
    return out->nslices ? 0 : -1;
}

int same_run(const run_result *a, const run_result *b, size_t n) {
    if (a->nslices != b->nslices) return 0;
    for (size_t i = 0; i < a->nslices; i++)
        if (a->slices[i].start != b->slices[i].start || a->slices[i].end != b->slices[i].end ||
            a->slices[i].thread != b->slices[i].thread)
            return 0;
    for (size_t i = 0; i < n; i++)
        if (a->finish[i] != b->finish[i] || a->first_run[i] != b->first_run[i]) return 0;
    return 1;
}

void print_run(const char *label, const run_result *r, const tb_proc *p) {
    printf("        %-10s", label);
    for (size_t i = 0; i < r->nslices && i < MAX_SLICES; i++)
        printf(" %s[%u-%u)", r->slices[i].thread < 0 ? "idle" : p[r->slices[i].thread].name, r->slices[i].start,
               r->slices[i].end);
    printf("\n");
}

double avg_waiting(const run_result *r, const tb_proc *p, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)(r->finish[i] - p[i].arrival - p[i].burst);
    return sum / (double)n;
}
