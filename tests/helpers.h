/* Run a process table through the real scheduler (virtual clock) and through
 * the textbook calculation, so tests can compare the two. */
#ifndef HELPERS_H
#define HELPERS_H

#include "textbook.h"
#include "timeslice.h"

#include <stddef.h>

#define MAX_PROCS 16
#define MAX_SLICES 1024

typedef struct {
    size_t nslices;
    ts_slice slices[MAX_SLICES];
    unsigned finish[MAX_PROCS], first_run[MAX_PROCS];
} run_result;

/* Each thread's whole body is ts_work(burst): a CPU-bound process, as in an exam question. */
int run_scheduler(ts_policy policy, unsigned quantum, const tb_proc *p, size_t n, run_result *out);
int run_textbook(ts_policy policy, unsigned quantum, const tb_proc *p, size_t n, run_result *out);
int same_run(const run_result *a, const run_result *b, size_t n);
void print_run(const char *label, const run_result *r, const tb_proc *p);
double avg_waiting(const run_result *r, const tb_proc *p, size_t n);

#endif
