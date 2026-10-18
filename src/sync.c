/*
 * Mutexes (with optional priority inheritance), condition variables and
 * semaphores. A thread that must wait joins the object's queue and blocks;
 * the thread that releases hands the resource straight to a waiter, so a
 * woken thread never has to race anyone for it.
 */
#include "internal.h"

#include <string.h>

static int by_priority(void) { return S.cfg.policy == TS_PRIORITY || S.cfg.policy == TS_PRIORITY_PREEMPTIVE; }

/* Under a priority policy the most important waiter goes first (ties: the one
 * that waited longest). Otherwise strictly first come, first served. */
static ts_thread *pop_waiter(ts_queue *q) {
    if (!by_priority()) return q_pop(q);
    ts_thread *best = NULL;
    for (ts_thread *w = q->head; w; w = w->next)
        if (!best || w->prio < best->prio) best = w;
    if (best) q_remove(q, best);
    return best;
}

/* ------------------------------------------------------------ mutex */

void ts_mutex_init(ts_mutex *m, int inherit_priority) {
    memset(m, 0, sizeof *m);
    m->inherit = inherit_priority;
}

static void hold(ts_thread *t, ts_mutex *m) {
    m->owner = t;
    m->held_next = t->held;
    t->held = m;
}

static void unhold(ts_thread *t, ts_mutex *m) {
    for (ts_mutex **p = &t->held; *p; p = &(*p)->held_next) {
        if (*p == m) {
            *p = m->held_next;
            m->held_next = NULL;
            return;
        }
    }
}

/* Priority inheritance. A low-priority thread holding a lock that a
 * high-priority thread needs runs at the high priority until it lets go, so a
 * medium-priority thread cannot keep it (and therefore the high one) off the
 * CPU. The raise follows the chain: if the owner is itself waiting on another
 * inheriting mutex, that mutex's owner is raised too. */
static void lend_priority(ts_thread *owner, int prio) {
    while (owner && prio < owner->prio) {
        owner->prio = prio;
        ts_mutex *next = owner->waiting_on;
        owner = next && next->inherit ? next->owner : NULL;
    }
}

/* A thread's priority is its own, or the most important one among the
 * threads waiting on any inheriting mutex it still holds. */
static void recompute_priority(ts_thread *t) {
    int p = t->base_prio;
    for (ts_mutex *m = t->held; m; m = m->held_next) {
        if (!m->inherit) continue;
        for (ts_thread *w = m->waiters.head; w; w = w->next)
            if (w->prio < p) p = w->prio;
    }
    t->prio = p;
}

/* Give the mutex to the next waiter, or free it. Returns the new owner. */
static ts_thread *release(ts_mutex *m) {
    ts_thread *t = S.current;
    unhold(t, m);
    m->owner = NULL;
    ts_thread *next = pop_waiter(&m->waiters);
    if (next) {
        hold(next, m);
        if (m->inherit) recompute_priority(next); /* it now owns m, and inherits from m's remaining waiters */
        make_ready(next);
    }
    if (m->inherit) recompute_priority(t); /* give back whatever it borrowed through m */
    return next;
}

void ts_mutex_lock(ts_mutex *m) {
    enter_critical();
    ts_thread *t = S.current;
    if (!m->owner) {
        hold(t, m);
        leave_critical();
        return;
    }
    t->waiting_on = m;
    q_push(&m->waiters, t);
    if (m->inherit) lend_priority(m->owner, t->prio);
    block_current(ST_BLOCKED); /* returns once the unlocker has handed us the mutex */
    t->waiting_on = NULL;
    leave_critical();
}

int ts_mutex_trylock(ts_mutex *m) {
    enter_critical();
    int got = !m->owner;
    if (got) hold(S.current, m);
    leave_critical();
    return got;
}

void ts_mutex_unlock(ts_mutex *m) {
    enter_critical();
    ts_thread *next = release(m);
    if (next && should_preempt(S.current)) preempt_current(); /* the waiter may outrank us now */
    leave_critical();
}

/* ------------------------------------------------------------ condition variable */

void ts_cond_init(ts_cond *c) { memset(c, 0, sizeof *c); }

/* Release the mutex and sleep as one step: no signal can land in between and
 * be lost. On waking, take the mutex again. Like pthreads, a wake-up is a hint
 * ("Mesa" semantics): re-check the condition in a loop. */
void ts_cond_wait(ts_cond *c, ts_mutex *m) {
    enter_critical();
    ts_thread *t = S.current;
    q_push(&c->waiters, t);
    release(m);
    block_current(ST_BLOCKED);
    leave_critical();
    ts_mutex_lock(m);
}

void ts_cond_signal(ts_cond *c) {
    enter_critical();
    ts_thread *w = q_pop(&c->waiters);
    if (w) make_ready(w);
    leave_critical();
}

void ts_cond_broadcast(ts_cond *c) {
    enter_critical();
    for (ts_thread *w; (w = q_pop(&c->waiters));) make_ready(w);
    leave_critical();
}

/* ------------------------------------------------------------ semaphore */

void ts_sem_init(ts_sem *s, int count) {
    memset(s, 0, sizeof *s);
    s->count = count;
}

void ts_sem_wait(ts_sem *s) {
    enter_critical();
    if (s->count > 0) {
        s->count--;
    } else {
        q_push(&s->waiters, S.current);
        block_current(ST_BLOCKED); /* post hands us the unit directly */
    }
    leave_critical();
}

void ts_sem_post(ts_sem *s) {
    enter_critical();
    ts_thread *w = pop_waiter(&s->waiters);
    if (w) make_ready(w);
    else s->count++;
    leave_critical();
}
