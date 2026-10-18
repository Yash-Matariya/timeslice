/*
 * The textbook calculation, written separately on purpose.
 *
 * This is the kind of program an OS course asks for: given a table of
 * processes, work out the Gantt chart tick by tick with arrays. It shares no
 * code with the scheduler. The tests run both on the same workloads and
 * require identical timelines, so each checks the other.
 */
#ifndef TIMESLICE_TEXTBOOK_H
#define TIMESLICE_TEXTBOOK_H

#include "timeslice.h"

typedef struct {
    const char *name;
    unsigned arrival, burst;
    int priority;
} tb_proc;

typedef struct {
    unsigned first_run, finish;
} tb_result;

/* MLFQ has no single textbook answer and is not supported here.
 * Returns the number of slices (possibly more than max), or 0 on bad input. */
size_t tb_schedule(ts_policy policy, unsigned quantum, const tb_proc *procs, size_t n, ts_slice *out, size_t max,
                   tb_result *results);

#endif
