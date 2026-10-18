#include "textbook.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ts_slice *out;
    size_t max, count;
} slices;

static void add(slices *s, unsigned start, unsigned end, int proc) {
    if (s->count && s->count <= s->max && s->out[s->count - 1].thread == proc && s->out[s->count - 1].end == start) {
        s->out[s->count - 1].end = end;
        return;
    }
    if (s->count < s->max) s->out[s->count] = (ts_slice){start, end, proc};
    s->count++;
}

size_t tb_schedule(ts_policy policy, unsigned quantum, const tb_proc *p, size_t n, ts_slice *out, size_t max,
                   tb_result *res) {
    if (policy == TS_MLFQ || n == 0) return 0;
    if (quantum == 0) quantum = 2;
    unsigned *left = calloc(n, sizeof *left);
    int *arrived = calloc(n, sizeof *arrived);
    int *fifo = calloc(n + 1, sizeof *fifo); /* arrival order, for FCFS and RR */
    size_t head = 0, tail = 0;              /* a ring of n + 1 slots */
    for (size_t i = 0; i < n; i++) {
        left[i] = p[i].burst;
        res[i].first_run = UINT_MAX;
        res[i].finish = 0;
    }
    slices s = {out, max, 0};
    unsigned time = 0, slice = 0;
    size_t done = 0;
    int cur = -1;

#define ADMIT()                                                   \
    for (size_t i = 0; i < n; i++)                                \
        if (!arrived[i] && p[i].arrival <= time) {                \
            arrived[i] = 1;                                       \
            fifo[tail] = (int)i;                                  \
            tail = (tail + 1) % (n + 1);                          \
        }

    /* The key a choosing policy sorts by; ties to the earlier arrival, then the lower index. */
#define KEY(i) (policy == TS_SJF ? (long)p[i].burst : policy == TS_SRTF ? (long)left[i] : (long)p[i].priority)
#define BEATS(a, b) (KEY(a) != KEY(b) ? KEY(a) < KEY(b) : p[a].arrival != p[b].arrival ? p[a].arrival < p[b].arrival : (a) < (b))

    ADMIT();
    while (done < n) {
        int by_key = policy == TS_SJF || policy == TS_SRTF || policy == TS_PRIORITY || policy == TS_PRIORITY_PREEMPTIVE;
        int best = -1;
        if (by_key) {
            for (size_t i = 0; i < n; i++)
                if (arrived[i] && left[i] && (int)i != cur && (best < 0 || BEATS(i, (size_t)best))) best = (int)i;
        }
        if (cur < 0) {
            if (by_key) {
                cur = best;
            } else if (head != tail) {
                cur = fifo[head];
                head = (head + 1) % (n + 1);
            }
            slice = 0;
        } else if ((policy == TS_SRTF || policy == TS_PRIORITY_PREEMPTIVE) && best >= 0 && KEY((size_t)best) < KEY((size_t)cur)) {
            cur = best; /* strictly better: preempt. A tie keeps the running one. */
            slice = 0;
        }
        if (cur < 0) { /* idle until the next arrival */
            unsigned next = UINT_MAX;
            for (size_t i = 0; i < n; i++)
                if (!arrived[i] && p[i].arrival < next) next = p[i].arrival;
            add(&s, time, next, -1);
            time = next;
            ADMIT();
            continue;
        }
        if (res[cur].first_run == UINT_MAX) res[cur].first_run = time;
        add(&s, time, time + 1, cur);
        left[cur]--;
        time++;
        slice++;
        ADMIT(); /* before re-queueing: a new arrival goes ahead of a preempted process */
        if (left[cur] == 0) {
            res[cur].finish = time;
            done++;
            cur = -1;
        } else if (policy == TS_RR && slice == quantum) {
            if (head != tail) {
                fifo[tail] = cur;
                tail = (tail + 1) % (n + 1);
                cur = -1;
            }
            slice = 0;
        }
    }
    free(left);
    free(arrived);
    free(fifo);
    return s.count;
}
