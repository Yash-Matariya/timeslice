/* Shared between the scheduler (sched.c) and the locks (sync.c). */
#ifndef TS_INTERNAL_H
#define TS_INTERNAL_H

#include "context.h"
#include "timeslice.h"

#include <signal.h>

typedef enum { ST_NEW, ST_READY, ST_RUNNING, ST_BLOCKED, ST_SLEEPING, ST_DONE } ts_state;

struct ts_thread {
    ts_context ctx; /* first, so its alignment is the struct's */
    ts_fn fn;
    void *arg;
    char name[32];
    int index; /* spawn order */
    ts_state state;
    unsigned arrival, burst;
    volatile unsigned used;  /* CPU ticks consumed; in TS_REAL the timer handler writes it */
    int base_prio, prio;     /* prio is base_prio, or higher if a waiter lent it (inheritance) */
    unsigned level;          /* MLFQ: its queue */
    unsigned level_used;     /* MLFQ: ticks used at that level, across every slice */
    int exhausted;           /* MLFQ: used up its allotment on this tick */
    unsigned slice_used;     /* ticks since it was last given the CPU */
    unsigned wake_at;
    int started;
    unsigned first_run, finish;
    unsigned preempted;
    ts_mutex *waiting_on;    /* the mutex it is blocked on */
    ts_mutex *held;          /* the mutexes it owns, linked through ts_mutex.held_next */
    void *stack;
    size_t stack_bytes;
    ts_thread *next;         /* link for whichever queue it is in; a thread waits in one at a time */
};

#define TS_MAX_LEVELS 8

typedef struct {
    ts_config cfg;
    int initialised;
    ts_context sched_ctx; /* the scheduler loop's own registers, inside ts_run */
    ts_thread *volatile current;
    ts_thread **threads;
    size_t count, cap;
    ts_queue ready;                /* every policy but MLFQ */
    ts_queue mlfq[TS_MAX_LEVELS];
    volatile unsigned now;         /* ticks */
    unsigned next_boost;
    unsigned long switches, preemptions;
    ts_slice *timeline;
    size_t tl_count, tl_cap;
    sigset_t timer_mask;
    int deadlocked;
} ts_sched;

extern ts_sched S;

void q_push(ts_queue *q, ts_thread *t);
ts_thread *q_pop(ts_queue *q);
void q_remove(ts_queue *q, ts_thread *t);

/* The timer signal is this library's interrupt. Scheduler code runs with it
 * masked, as a kernel runs with interrupts off; every way back into a thread
 * unmasks it again. */
void enter_critical(void);
void leave_critical(void);

void make_ready(ts_thread *t);
void block_current(ts_state state); /* current thread stops until something makes it ready */
int should_preempt(ts_thread *running);
void preempt_current(void);         /* back to the ready queue, still runnable */

#endif
