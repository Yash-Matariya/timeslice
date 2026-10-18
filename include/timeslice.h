/*
 * timeslice: user-space threads with real schedulers.
 *
 * Threads here are coroutines with their own stacks. The library switches
 * between them itself, saving and restoring registers in a few lines of
 * assembly, and a scheduler decides who runs next: FCFS, SJF, SRTF, priority
 * (with or without preemption), round robin, or a multi-level feedback queue.
 *
 * Two clocks:
 *   TS_VIRTUAL  time advances only when a thread calls ts_work(), one tick at
 *               a time, and the scheduler may preempt at every tick. Fully
 *               deterministic: the same program always gives the same timeline,
 *               which is what lets it be checked against a textbook answer.
 *   TS_REAL     a timer signal interrupts whatever is running, so a thread in a
 *               busy loop that never yields still loses the CPU. This is
 *               preemption the way a kernel (or an RTOS) does it.
 *
 * Everything runs on the one OS thread that calls ts_run(). Not for use from
 * several OS threads at once.
 */
#ifndef TIMESLICE_H
#define TIMESLICE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TS_FCFS,                /* first come, first served; never preempts */
    TS_SJF,                 /* shortest declared burst first; never preempts */
    TS_SRTF,                /* shortest remaining time first; a shorter job preempts */
    TS_PRIORITY,            /* smallest priority number first; never preempts */
    TS_PRIORITY_PREEMPTIVE, /* the same, and a more important thread preempts */
    TS_RR,                  /* round robin, fixed quantum */
    TS_MLFQ,                /* multi-level feedback queue */
} ts_policy;

typedef enum { TS_VIRTUAL, TS_REAL } ts_clock;

typedef struct {
    ts_policy policy;
    ts_clock clock;
    unsigned quantum;     /* RR, and MLFQ's top level, in ticks. 0 = 2 */
    unsigned mlfq_levels; /* 0 = 3. Level k gets quantum << k ticks */
    unsigned mlfq_boost;  /* every this many ticks, move every thread back to the top. 0 = never */
    unsigned tick_usec;   /* TS_REAL: length of one tick. 0 = 1000 */
} ts_config;

typedef struct {
    const char *name;
    unsigned arrival;  /* the tick at which the thread becomes ready */
    unsigned burst;    /* the CPU time it declares; SJF and SRTF schedule by this */
    int priority;      /* smaller = more important, as in textbooks */
    size_t stack_size; /* 0 = 64 KiB */
} ts_attr;

typedef struct ts_thread ts_thread;
typedef void (*ts_fn)(void *arg);

int ts_init(const ts_config *config); /* 0, or -1 for a bad config */
ts_thread *ts_spawn(ts_fn fn, void *arg, const ts_attr *attr);
int ts_run(void);                     /* run every thread to completion. 0, or -1 on deadlock */
void ts_shutdown(void);               /* free everything; ts_init may be called again */

/* Called from inside a thread. */
void ts_work(unsigned ticks); /* use the CPU for this many ticks */
void ts_yield(void);
void ts_sleep(unsigned ticks);
void ts_exit(void);
ts_thread *ts_self(void);
unsigned ts_now(void);
const char *ts_name(const ts_thread *thread);
int ts_priority(const ts_thread *thread); /* effective priority, after any inheritance */
unsigned ts_level(const ts_thread *thread); /* MLFQ: the queue it is in, 0 = top */

/* Synchronisation. A thread waits in one queue at a time, linked through the
 * thread itself, so none of these allocate. */
typedef struct ts_queue {
    ts_thread *head, *tail;
} ts_queue;

typedef struct ts_mutex {
    ts_thread *owner;
    ts_queue waiters;
    int inherit;                /* priority inheritance on or off */
    struct ts_mutex *held_next; /* the owner's list of mutexes it holds */
} ts_mutex;

typedef struct {
    ts_queue waiters;
} ts_cond;

typedef struct {
    int count;
    ts_queue waiters;
} ts_sem;

void ts_mutex_init(ts_mutex *m, int inherit_priority);
void ts_mutex_lock(ts_mutex *m);
int ts_mutex_trylock(ts_mutex *m); /* 1 if taken */
void ts_mutex_unlock(ts_mutex *m);
void ts_cond_init(ts_cond *c);
void ts_cond_wait(ts_cond *c, ts_mutex *m);
void ts_cond_signal(ts_cond *c);
void ts_cond_broadcast(ts_cond *c);
void ts_sem_init(ts_sem *s, int count);
void ts_sem_wait(ts_sem *s);
void ts_sem_post(ts_sem *s);

/* What happened, available after ts_run() returns. */
typedef struct {
    const char *name;
    int priority;
    unsigned arrival, burst;
    unsigned first_run, finish;
    unsigned turnaround; /* finish - arrival */
    unsigned waiting;    /* turnaround - CPU ticks used */
    unsigned response;   /* first_run - arrival */
    unsigned used;       /* CPU ticks actually used */
    unsigned preempted;  /* times taken off the CPU while still able to run */
} ts_stats;

typedef struct {
    unsigned start, end; /* ticks, end exclusive */
    int thread;          /* index in spawn order, or -1 for idle */
} ts_slice;

size_t ts_stats_get(ts_stats *out, size_t max);
size_t ts_timeline(ts_slice *out, size_t max); /* adjacent slices of one thread are merged */
unsigned long ts_switches(void);               /* times a thread was given the CPU */
unsigned long ts_preemptions(void);            /* ...taken off it while still runnable */
const char *ts_policy_name(ts_policy policy);

#ifdef __cplusplus
}
#endif

#endif
