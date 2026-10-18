#include "check.h"
#include "context.h"
#include "timeslice.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static ts_context main_ctx, co_ctx;
static volatile long seen;

/* Keeps its own locals live across every switch. At -O2 those live in
 * callee-saved registers, the same ones the test body is using, so if the
 * switch failed to save or restore one, the two would corrupt each other. */
static void coroutine(void) {
    long local = 0;
    double d = 1.0;
    for (;;) {
        local++;
        d += 0.25;
        seen = local + (long)(d * 0); /* d stays live */
        ts_context_switch(&co_ctx, &main_ctx);
    }
}

TEST("context: a million switches, and both sides' registers survive", context_million) {
    static char stack[64 * 1024] __attribute__((aligned(16)));
    ts_context_init(&co_ctx, stack + sizeof stack, coroutine);
    long mine = 7;
    double md = 3.0;
    for (long i = 1; i <= 1000000; i++) {
        ts_context_switch(&main_ctx, &co_ctx);
        if (seen != i) CHECK_EQ(seen, i);
        mine += 2;
        md += 0.5;
    }
    CHECK_EQ(mine, 7 + 2000000);
    CHECK(md == 3.0 + 500000.0);
}

TEST("context: how long one switch takes", context_speed) {
    static char stack[64 * 1024] __attribute__((aligned(16)));
    ts_context_init(&co_ctx, stack + sizeof stack, coroutine);
    enum { N = 2000000 };
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (long i = 0; i < N; i++) ts_context_switch(&main_ctx, &co_ctx);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / (2.0 * N);
    printf("        %.1f ns per switch (%d round trips)\n", ns, N);
    CHECK(ns > 0);
}

/* ---- a stack overflow must hit the guard page ---- */

static void on_fault(int sig) { _exit(sig == SIGSEGV || sig == SIGBUS ? 77 : 1); }

static int dive(int n) {
    volatile char pad[512];
    pad[0] = (char)n;
    return n == 0 ? 0 : dive(n - 1) + pad[0]; /* not a tail call: every frame stays */
}

static void overflow_body(void *arg) {
    (void)arg;
    dive(1 << 20);
}

static void overflow_child(void) {
    /* The fault happens with the thread's stack exhausted, so the handler must
     * run on a stack of its own: sigaltstack. */
    static char alt[64 * 1024];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_handler = on_fault;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    ts_init(&(ts_config){.policy = TS_FCFS});
    ts_spawn(overflow_body, NULL, &(ts_attr){.stack_size = 32 * 1024});
    ts_run();
    _exit(2); /* should never get here */
}

TEST("context: running off the end of a thread's stack hits the guard page", context_guard) {
    int status = run_isolated(overflow_child, 10);
    CHECK(status != -1);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 77);
}
