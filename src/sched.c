/*
 * The scheduler: threads, the clock, dispatching, and the policies.
 *
 * ts_run() is a loop in the caller's own context. Each pass admits threads
 * that have arrived or woken, asks the policy for the next one, and switches
 * to it. The thread runs until it blocks, yields, finishes, or is preempted,
 * and each of those switches back to the loop. Threads never switch straight
 * to each other; going through the loop keeps every decision in one place.
 */
#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

ts_sched S;

/* ------------------------------------------------------------ queues */

void q_push(ts_queue *q, ts_thread *t) {
    t->next = NULL;
    if (q->tail) q->tail->next = t;
    else q->head = t;
    q->tail = t;
}

ts_thread *q_pop(ts_queue *q) {
    ts_thread *t = q->head;
    if (t) {
        q->head = t->next;
        if (!q->head) q->tail = NULL;
        t->next = NULL;
    }
    return t;
}

void q_remove(ts_queue *q, ts_thread *t) {
    ts_thread *prev = NULL;
    for (ts_thread *c = q->head; c; prev = c, c = c->next) {
        if (c != t) continue;
        if (prev) prev->next = c->next;
        else q->head = c->next;
        if (q->tail == c) q->tail = prev;
        c->next = NULL;
        return;
    }
}

/* ------------------------------------------------------------ critical sections */

void enter_critical(void) {
    if (S.cfg.clock == TS_REAL) sigprocmask(SIG_BLOCK, &S.timer_mask, NULL);
}

void leave_critical(void) {
    if (S.cfg.clock == TS_REAL) sigprocmask(SIG_UNBLOCK, &S.timer_mask, NULL);
}

/* ------------------------------------------------------------ policies */

const char *ts_policy_name(ts_policy p) {
    switch (p) {
        case TS_FCFS: return "FCFS";
        case TS_SJF: return "SJF";
        case TS_SRTF: return "SRTF";
        case TS_PRIORITY: return "Priority";
        case TS_PRIORITY_PREEMPTIVE: return "Priority (preemptive)";
        case TS_RR: return "Round Robin";
        case TS_MLFQ: return "MLFQ";
    }
    return "?";
}

static unsigned remaining(const ts_thread *t) { return t->used >= t->burst ? 0 : t->burst - t->used; }

static unsigned allotment(const ts_thread *t) { return S.cfg.quantum << t->level; }

static int picks_by_key(void) {
    ts_policy p = S.cfg.policy;
    return p == TS_SJF || p == TS_SRTF || p == TS_PRIORITY || p == TS_PRIORITY_PREEMPTIVE;
}

/* The key the choosing policies sort by. Ties go to the earlier arrival, then
 * to spawn order, the convention textbooks use. */
static int better(const ts_thread *a, const ts_thread *b) {
    long ka = 0, kb = 0;
    switch (S.cfg.policy) {
        case TS_SJF: ka = a->burst; kb = b->burst; break;
        case TS_SRTF: ka = remaining(a); kb = remaining(b); break;
        default: ka = a->prio; kb = b->prio; break;
    }
    if (ka != kb) return ka < kb;
    if (a->arrival != b->arrival) return a->arrival < b->arrival;
    return a->index < b->index;
}

static ts_queue *queue_of(const ts_thread *t) { return S.cfg.policy == TS_MLFQ ? &S.mlfq[t->level] : &S.ready; }

/* Who would run next. FCFS and RR take the queue in order; the key policies
 * scan it (O(n); a heap would make it O(log n), and n here is small). */
static ts_thread *peek_best(void) {
    if (S.cfg.policy == TS_MLFQ) {
        for (unsigned l = 0; l < S.cfg.mlfq_levels; l++)
            if (S.mlfq[l].head) return S.mlfq[l].head;
        return NULL;
    }
    if (!picks_by_key()) return S.ready.head;
    ts_thread *best = NULL;
    for (ts_thread *t = S.ready.head; t; t = t->next)
        if (!best || better(t, best)) best = t;
    return best;
}

static ts_thread *take_best(void) {
    ts_thread *t = peek_best();
    if (t) q_remove(queue_of(t), t);
    return t;
}

void make_ready(ts_thread *t) {
    t->state = ST_READY;
    q_push(queue_of(t), t);
}

int should_preempt(ts_thread *t) {
    ts_thread *b;
    switch (S.cfg.policy) {
        case TS_SRTF:
            b = peek_best();
            return b && remaining(b) < remaining(t); /* strictly shorter: a tie does not preempt */
        case TS_PRIORITY_PREEMPTIVE:
            b = peek_best();
            return b && b->prio < t->prio;
        case TS_RR:
            return t->slice_used >= S.cfg.quantum && S.ready.head;
        case TS_MLFQ:
            for (unsigned l = 0; l < t->level; l++)
                if (S.mlfq[l].head) return 1;              /* someone more important is waiting */
            return t->exhausted && S.mlfq[t->level].head;  /* allotment used up: go behind the others */
        default:
            return 0; /* FCFS, SJF, Priority: run until done or blocked */
    }
}

/* ------------------------------------------------------------ the clock */

/* Make ready every thread whose arrival or wake-up is due, in spawn order. */
static void admit(void) {
    for (size_t i = 0; i < S.count; i++) {
        ts_thread *t = S.threads[i];
        if ((t->state == ST_NEW && t->arrival <= S.now) || (t->state == ST_SLEEPING && t->wake_at <= S.now))
            make_ready(t);
    }
}

/* MLFQ's anti-starvation rule: every mlfq_boost ticks, everyone goes back to
 * the top queue, so a CPU-heavy thread sunk to the bottom still gets to run. */
static void boost_check(void) {
    if (S.cfg.policy != TS_MLFQ || !S.cfg.mlfq_boost || S.now < S.next_boost) return;
    while (S.next_boost <= S.now) S.next_boost += S.cfg.mlfq_boost;
    for (unsigned l = 1; l < S.cfg.mlfq_levels; l++)
        while (S.mlfq[l].head) {
            ts_thread *t = q_pop(&S.mlfq[l]);
            t->level = 0;
            q_push(&S.mlfq[0], t);
        }
    for (size_t i = 0; i < S.count; i++) {
        S.threads[i]->level = 0;
        S.threads[i]->level_used = 0;
    }
}

/* The running thread used one tick: account for it, and admit whoever
 * arrived or woke meanwhile. No decision is made here. */
static void tick(ts_thread *t) {
    S.now++;
    t->used++;
    t->slice_used++;
    t->level_used++;
    if (S.tl_count) S.timeline[S.tl_count - 1].end = S.now;

    admit(); /* before any decision, so on a tie a new arrival queues ahead of a preempted thread */
    boost_check();

    if (S.cfg.policy == TS_MLFQ && t->level_used >= allotment(t)) {
        if (t->level + 1 < S.cfg.mlfq_levels) t->level++; /* used its whole allotment: move down */
        t->level_used = 0;
        t->exhausted = 1;
    }
}

/* A scheduling point: should the running thread keep the CPU?
 *
 * On the virtual clock this runs when a thread asks for its next tick, not
 * straight after its last one. The difference matters on the tick a thread's
 * work ends: deciding right after it would preempt a thread with nothing left
 * to do, which then "finishes" only when it next gets the CPU. The Gantt chart
 * looks identical, because that second turn is zero ticks long; the completion
 * time is wrong. The comparison with the textbook calculation caught exactly
 * that, on the tick where a round-robin quantum and a burst end together. */
static void scheduling_point(ts_thread *t) {
    if (S.cfg.policy == TS_RR && t->slice_used >= S.cfg.quantum && !S.ready.head)
        t->slice_used = 0; /* quantum over, nobody waiting: carry straight on */
    if (should_preempt(t)) preempt_current();
    t->exhausted = 0;
}

/* ------------------------------------------------------------ switching */

static void switch_to_scheduler(ts_thread *t) { ts_context_switch(&t->ctx, &S.sched_ctx); }

void block_current(ts_state state) {
    ts_thread *t = S.current;
    t->state = state;
    switch_to_scheduler(t);
}

void preempt_current(void) {
    ts_thread *t = S.current;
    t->preempted++;
    S.preemptions++;
    make_ready(t);
    switch_to_scheduler(t);
}

/* ------------------------------------------------------------ the timeline */

static void push_slice(unsigned start, unsigned end, int thread) {
    if (S.tl_count && S.timeline[S.tl_count - 1].thread == thread && S.timeline[S.tl_count - 1].end == start) {
        S.timeline[S.tl_count - 1].end = end; /* the same thread again, straight after: one slice */
        return;
    }
    if (S.tl_count == S.tl_cap) {
        S.tl_cap = S.tl_cap ? S.tl_cap * 2 : 64;
        S.timeline = realloc(S.timeline, S.tl_cap * sizeof *S.timeline);
        if (!S.timeline) abort();
    }
    S.timeline[S.tl_count++] = (ts_slice){start, end, thread};
}

static void drop_empty_slice(void) {
    if (S.tl_count && S.timeline[S.tl_count - 1].start == S.timeline[S.tl_count - 1].end) S.tl_count--;
}

/* ------------------------------------------------------------ the real clock */

static void on_tick(int sig) {
    (void)sig;
    int saved_errno = errno;
    ts_thread *t = S.current;
    if (t) {
        tick(t);
        scheduling_point(t); /* may switch away right here, inside the handler; resumes here later */
    } else {
        S.now++; /* the scheduler is idle, waiting for a sleeper */
    }
    errno = saved_errno;
}

static struct sigaction old_action;

static void start_timer(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_tick; /* no SA_NODEFER: the kernel masks the timer while the handler runs */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, &old_action);
    struct itimerval it = {{0, (int)S.cfg.tick_usec}, {0, (int)S.cfg.tick_usec}};
    setitimer(ITIMER_REAL, &it, NULL);
}

static void stop_timer(void) {
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    sigaction(SIGALRM, &old_action, NULL);
}

/* ------------------------------------------------------------ threads */

static void thread_entry(void) {
    leave_critical(); /* the scheduler dispatched us with the timer masked */
    ts_thread *t = S.current;
    t->fn(t->arg);
    ts_exit();
}

/* A stack is mapped with one inaccessible page below it. Stacks grow down, so
 * an overflow hits that page and faults at once, instead of silently writing
 * over whatever memory happened to sit below. */
static int make_stack(ts_thread *t, size_t size) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size = (size + page - 1) / page * page;
    size_t total = size + page;
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) return -1;
    if (mprotect(mem, page, PROT_NONE) != 0) {
        munmap(mem, total);
        return -1;
    }
    t->stack = mem;
    t->stack_bytes = total;
    ts_context_init(&t->ctx, (char *)mem + total, thread_entry);
    return 0;
}

int ts_init(const ts_config *cfg) {
    if (S.initialised) ts_shutdown();
    memset(&S, 0, sizeof S);
    S.cfg = cfg ? *cfg : (ts_config){0};
    if (!S.cfg.quantum) S.cfg.quantum = 2;
    if (!S.cfg.mlfq_levels) S.cfg.mlfq_levels = 3;
    if (!S.cfg.tick_usec) S.cfg.tick_usec = 1000;
    if (S.cfg.mlfq_levels > TS_MAX_LEVELS || S.cfg.policy > TS_MLFQ || S.cfg.clock > TS_REAL) return -1;
    S.next_boost = S.cfg.mlfq_boost;
    sigemptyset(&S.timer_mask);
    sigaddset(&S.timer_mask, SIGALRM);
    S.initialised = 1;
    return 0;
}

ts_thread *ts_spawn(ts_fn fn, void *arg, const ts_attr *attr) {
    ts_attr a = attr ? *attr : (ts_attr){0};
    ts_thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    if (make_stack(t, a.stack_size ? a.stack_size : 64 * 1024) != 0) {
        free(t);
        return NULL;
    }
    enter_critical(); /* a running thread may spawn; the handler must not see a half-grown array */
    t->fn = fn;
    t->arg = arg;
    t->index = (int)S.count;
    if (a.name) snprintf(t->name, sizeof t->name, "%s", a.name);
    else snprintf(t->name, sizeof t->name, "T%d", t->index);
    t->state = ST_NEW;
    t->arrival = S.current && a.arrival < S.now ? S.now : a.arrival;
    t->burst = a.burst;
    t->base_prio = t->prio = a.priority;
    if (S.count == S.cap) {
        S.cap = S.cap ? S.cap * 2 : 16;
        S.threads = realloc(S.threads, S.cap * sizeof *S.threads);
        if (!S.threads) abort();
    }
    S.threads[S.count++] = t;
    leave_critical();
    return t;
}

static void dispatch(ts_thread *t) {
    if (!t->started) {
        t->started = 1;
        t->first_run = S.now;
    }
    t->state = ST_RUNNING;
    t->slice_used = 0;
    t->exhausted = 0;
    push_slice(S.now, S.now, t->index);
    S.current = t;
    S.switches++;
    ts_context_switch(&S.sched_ctx, &t->ctx);
    S.current = NULL;
    drop_empty_slice();
    if (t->state == ST_DONE && t->stack) { /* its stack is no longer in use: we are off it */
        munmap(t->stack, t->stack_bytes);
        t->stack = NULL;
    }
}

int ts_run(void) {
    sigset_t outer;
    if (S.cfg.clock == TS_REAL) {
        sigprocmask(SIG_BLOCK, &S.timer_mask, &outer); /* the scheduler runs with the timer masked */
        start_timer();
    }
    S.deadlocked = 0;
    for (;;) {
        admit();
        ts_thread *t = take_best();
        if (t) {
            dispatch(t);
            continue;
        }
        /* Nothing ready. Wait for the next arrival or wake-up, or stop. */
        size_t alive = 0;
        unsigned next = UINT_MAX;
        for (size_t i = 0; i < S.count; i++) {
            ts_thread *u = S.threads[i];
            if (u->state == ST_DONE) continue;
            alive++;
            if (u->state == ST_NEW && u->arrival < next) next = u->arrival;
            if (u->state == ST_SLEEPING && u->wake_at < next) next = u->wake_at;
        }
        if (!alive) break;
        if (next == UINT_MAX) { /* only blocked threads left, and nothing can ever wake them */
            S.deadlocked = 1;
            fprintf(stderr, "timeslice: deadlock at tick %u:", S.now);
            for (size_t i = 0; i < S.count; i++)
                if (S.threads[i]->state == ST_BLOCKED) fprintf(stderr, " %s", S.threads[i]->name);
            fprintf(stderr, " are blocked for ever\n");
            break;
        }
        unsigned from = S.now;
        if (S.cfg.clock == TS_VIRTUAL) {
            S.now = next; /* idle time costs nothing to skip */
        } else {
            sigset_t wait = outer;
            sigdelset(&wait, SIGALRM);
            while (S.now < next) sigsuspend(&wait); /* sleep until the timer ticks */
        }
        push_slice(from, S.now, -1);
        boost_check();
    }
    if (S.cfg.clock == TS_REAL) {
        stop_timer();
        sigprocmask(SIG_SETMASK, &outer, NULL);
    }
    return S.deadlocked ? -1 : 0;
}

void ts_shutdown(void) {
    for (size_t i = 0; i < S.count; i++) {
        if (S.threads[i]->stack) munmap(S.threads[i]->stack, S.threads[i]->stack_bytes);
        free(S.threads[i]);
    }
    free(S.threads);
    free(S.timeline);
    memset(&S, 0, sizeof S);
}

/* ------------------------------------------------------------ inside a thread */

void ts_work(unsigned ticks) {
    ts_thread *t = S.current;
    if (S.cfg.clock == TS_REAL) {
        /* Burn CPU until the timer has counted `ticks` of it. The loop never
         * yields: any preemption comes from the signal, not from this code. */
        unsigned target = t->used + ticks;
        while (t->used < target) {
        }
        return;
    }
    while (ticks--) {
        scheduling_point(t);
        tick(t);
    }
}

void ts_yield(void) {
    enter_critical();
    make_ready(S.current);
    switch_to_scheduler(S.current);
    leave_critical(); /* resumed by the scheduler, which runs masked: unmask again */
}

void ts_sleep(unsigned ticks) {
    if (!ticks) {
        ts_yield();
        return;
    }
    enter_critical();
    S.current->wake_at = S.now + ticks;
    block_current(ST_SLEEPING);
    leave_critical();
}

void ts_exit(void) {
    enter_critical();
    ts_thread *t = S.current;
    t->state = ST_DONE;
    t->finish = S.now;
    switch_to_scheduler(t);
    abort(); /* a finished thread is never switched back to */
}

ts_thread *ts_self(void) { return S.current; }
unsigned ts_now(void) { return S.now; }
const char *ts_name(const ts_thread *t) { return t->name; }
int ts_priority(const ts_thread *t) { return t->prio; }
unsigned ts_level(const ts_thread *t) { return t->level; }

/* ------------------------------------------------------------ results */

size_t ts_stats_get(ts_stats *out, size_t max) {
    size_t n = S.count < max ? S.count : max;
    for (size_t i = 0; i < n; i++) {
        const ts_thread *t = S.threads[i];
        ts_stats s = {0};
        s.name = t->name;
        s.priority = t->base_prio;
        s.arrival = t->arrival;
        s.burst = t->burst;
        s.first_run = t->first_run;
        s.finish = t->finish;
        s.used = t->used;
        s.turnaround = t->finish - t->arrival;
        s.waiting = s.turnaround - t->used;
        s.response = t->first_run - t->arrival;
        s.preempted = t->preempted;
        out[i] = s;
    }
    return n;
}

size_t ts_timeline(ts_slice *out, size_t max) {
    size_t n = S.tl_count < max ? S.tl_count : max;
    memcpy(out, S.timeline, n * sizeof *out);
    return S.tl_count;
}

unsigned long ts_switches(void) { return S.switches; }
unsigned long ts_preemptions(void) { return S.preemptions; }
