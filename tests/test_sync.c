#include "check.h"
#include "timeslice.h"

static ts_mutex m, m2;
static int counter;

static void increment_locked(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        ts_mutex_lock(&m);
        int v = counter;
        ts_work(1); /* preempted here, holding the lock */
        counter = v + 1;
        ts_mutex_unlock(&m);
    }
}

static void increment_racy(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        int v = counter;
        ts_work(1); /* preempted between the read and the write */
        counter = v + 1;
    }
}

TEST("sync: without the mutex, updates are lost; with it, none are", sync_counter) {
    ts_init(&(ts_config){.policy = TS_RR, .quantum = 1});
    counter = 0;
    for (int i = 0; i < 5; i++) ts_spawn(increment_racy, NULL, NULL);
    ts_run();
    ts_shutdown();
    CHECK(counter < 250); /* the race is real even on one CPU: preemption is enough */

    ts_init(&(ts_config){.policy = TS_RR, .quantum = 1});
    ts_mutex_init(&m, 0);
    counter = 0;
    for (int i = 0; i < 5; i++) ts_spawn(increment_locked, NULL, NULL);
    CHECK_EQ(ts_run(), 0);
    ts_shutdown();
    CHECK_EQ(counter, 250);
}

/* ---- priority inversion: the Mars Pathfinder bug ----
 * L (low) takes the lock. H (high) arrives and needs it, so H waits for L.
 * M (medium) arrives and, outranking L, takes the CPU for its whole burst.
 * H, the most important thread, ends up waiting for M, which does not even
 * use the lock. Inheritance fixes it: while H waits, L runs at H's priority. */

static void low(void *arg) {
    (void)arg;
    ts_mutex_lock(&m);
    ts_work(4);
    ts_mutex_unlock(&m);
    ts_work(1);
}
static void high(void *arg) {
    (void)arg;
    ts_mutex_lock(&m);
    ts_work(2);
    ts_mutex_unlock(&m);
}
static void medium(void *arg) {
    (void)arg;
    ts_work(6);
}

static void inversion(int inherit, ts_stats st[3]) {
    ts_init(&(ts_config){.policy = TS_PRIORITY_PREEMPTIVE});
    ts_mutex_init(&m, inherit);
    ts_spawn(low, NULL, &(ts_attr){.name = "L", .arrival = 0, .burst = 5, .priority = 3});
    ts_spawn(high, NULL, &(ts_attr){.name = "H", .arrival = 1, .burst = 2, .priority = 1});
    ts_spawn(medium, NULL, &(ts_attr){.name = "M", .arrival = 2, .burst = 6, .priority = 2});
    ts_run();
    ts_stats_get(st, 3);
    ts_shutdown();
}

TEST("sync: priority inversion, and priority inheritance fixing it", sync_inversion) {
    ts_stats st[3];
    inversion(0, st);
    /* L 0-1, H 1 (blocks at once), L 1-2, M 2-8, L 8-10 (unlocks), H 10-12. */
    CHECK_EQ(st[2].finish, 8);  /* M finishes first... */
    CHECK_EQ(st[1].finish, 12); /* ...and H waited for all of it */

    inversion(1, st);
    /* L 0-1, H blocks at 1 and lends L priority 1, L 1-4 (M arrives at 2 and waits), H 4-6, M 6-12. */
    CHECK_EQ(st[1].finish, 6);
    CHECK_EQ(st[2].finish, 12);
    CHECK_EQ(st[0].priority, 3); /* the loan is over: L's own priority is untouched */
}

/* ---- condition variable: a bounded buffer ---- */

static ts_cond not_full, not_empty;
static int buf[2], buf_count, buf_head, got[20], ngot;

static void producer(void *arg) {
    (void)arg;
    for (int i = 1; i <= 20; i++) {
        ts_mutex_lock(&m);
        while (buf_count == 2) ts_cond_wait(&not_full, &m); /* a loop, not an if: Mesa semantics */
        buf[(buf_head + buf_count++) % 2] = i;
        ts_cond_signal(&not_empty);
        ts_mutex_unlock(&m);
        ts_work(1);
    }
}

static void consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        ts_mutex_lock(&m);
        while (buf_count == 0) ts_cond_wait(&not_empty, &m);
        got[ngot++] = buf[buf_head];
        buf_head = (buf_head + 1) % 2;
        buf_count--;
        ts_cond_signal(&not_full);
        ts_mutex_unlock(&m);
        ts_work(2);
    }
}

TEST("sync: a bounded buffer with two condition variables loses nothing", sync_buffer) {
    ts_init(&(ts_config){.policy = TS_RR, .quantum = 1});
    ts_mutex_init(&m, 0);
    ts_cond_init(&not_full);
    ts_cond_init(&not_empty);
    buf_count = buf_head = ngot = 0;
    ts_spawn(consumer, NULL, NULL);
    ts_spawn(producer, NULL, NULL);
    CHECK_EQ(ts_run(), 0);
    ts_shutdown();
    CHECK_EQ(ngot, 20);
    for (int i = 0; i < 20; i++) CHECK_EQ(got[i], i + 1);
}

/* ---- semaphore ---- */

static ts_sem sem;
static int inside, most_inside;

static void limited(void *arg) {
    (void)arg;
    ts_sem_wait(&sem);
    if (++inside > most_inside) most_inside = inside;
    ts_work(3);
    inside--;
    ts_sem_post(&sem);
}

TEST("sync: a semaphore of 3 lets exactly 3 in at once", sync_semaphore) {
    ts_init(&(ts_config){.policy = TS_RR, .quantum = 1});
    ts_sem_init(&sem, 3);
    inside = most_inside = 0;
    for (int i = 0; i < 6; i++) ts_spawn(limited, NULL, NULL);
    CHECK_EQ(ts_run(), 0);
    ts_shutdown();
    CHECK_EQ(most_inside, 3);
}

/* ---- deadlock ---- */

static void ab(void *arg) {
    (void)arg;
    ts_mutex_lock(&m);
    ts_yield();
    ts_mutex_lock(&m2);
}
static void ba(void *arg) {
    (void)arg;
    ts_mutex_lock(&m2);
    ts_yield();
    ts_mutex_lock(&m);
}

TEST("sync: two locks taken in opposite orders deadlock, and ts_run says so", sync_deadlock) {
    ts_init(&(ts_config){.policy = TS_RR});
    ts_mutex_init(&m, 0);
    ts_mutex_init(&m2, 0);
    ts_spawn(ab, NULL, &(ts_attr){.name = "A"});
    ts_spawn(ba, NULL, &(ts_attr){.name = "B"});
    CHECK_EQ(ts_run(), -1);
    ts_shutdown();
}

static int try_result;
static void holder(void *arg) {
    (void)arg;
    ts_mutex_lock(&m);
    ts_yield();
    ts_mutex_unlock(&m);
}
static void trier(void *arg) {
    (void)arg;
    try_result = ts_mutex_trylock(&m);
}

TEST("sync: trylock fails instead of waiting", sync_trylock) {
    ts_init(&(ts_config){.policy = TS_FCFS});
    ts_mutex_init(&m, 0);
    ts_spawn(holder, NULL, NULL);
    ts_spawn(trier, NULL, NULL);
    ts_run();
    ts_shutdown();
    CHECK_EQ(try_result, 0);
}
