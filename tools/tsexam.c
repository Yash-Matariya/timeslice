/*
 * tsexam: CPU-scheduling exam questions, answered twice.
 *
 *   tsexam --policy rr --quantum 2 < processes.txt
 *   tsexam --practice --policy srtf --seed 7          a question to solve by hand
 *   tsexam --practice --policy srtf --seed 7 --answer  ...and its answer
 *
 * Input is one process per line: name arrival burst [priority]. '#' starts a
 * comment. The answer comes from the textbook calculation and from actually
 * running one thread per process under the real scheduler; tsexam prints both
 * Gantt charts and says whether they agree (they always should).
 */
#include "textbook.h"
#include "timeslice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX 32

static void usage(void) {
    fputs("usage: tsexam --policy fcfs|sjf|srtf|prio|prio-p|rr [--quantum N] [FILE]\n"
          "       tsexam --practice --policy P [--quantum N] [--seed S] [--count N] [--answer]\n",
          stderr);
    exit(2);
}

static int parse_policy(const char *s, ts_policy *out) {
    static const struct { const char *name; ts_policy p; } table[] = {
        {"fcfs", TS_FCFS}, {"sjf", TS_SJF}, {"srtf", TS_SRTF}, {"prio", TS_PRIORITY},
        {"prio-p", TS_PRIORITY_PREEMPTIVE}, {"rr", TS_RR},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(s, table[i].name)) { *out = table[i].p; return 1; }
    return 0;
}

static char names[MAX][16];

static size_t read_procs(FILE *f, tb_proc *p) {
    char line[256];
    size_t n = 0;
    while (n < MAX && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        unsigned arrival, burst;
        int prio = 0;
        int k = sscanf(line, "%15s %u %u %d", names[n], &arrival, &burst, &prio);
        if (k < 3) continue;
        if (burst == 0) { fprintf(stderr, "tsexam: %s has a burst of 0\n", names[n]); exit(2); }
        p[n] = (tb_proc){names[n], arrival, burst, k == 4 ? prio : 0};
        n++;
    }
    return n;
}

static void print_gantt(const char *label, const ts_slice *s, size_t n, const tb_proc *p) {
    printf("%s\n  ", label);
    for (size_t i = 0; i < n; i++) {
        const char *name = s[i].thread < 0 ? "idle" : p[s[i].thread].name;
        int width = (int)(s[i].end - s[i].start) * 2;
        if (width < (int)strlen(name) + 2) width = (int)strlen(name) + 2;
        printf("|%-*s", width, name);
    }
    printf("|\n  ");
    for (size_t i = 0; i < n; i++) {
        const char *name = s[i].thread < 0 ? "idle" : p[s[i].thread].name;
        int width = (int)(s[i].end - s[i].start) * 2;
        if (width < (int)strlen(name) + 2) width = (int)strlen(name) + 2;
        printf("%-*u", width + 1, s[i].start);
    }
    printf("%u\n", n ? s[n - 1].end : 0);
}

static void worker(void *arg) { ts_work(*(unsigned *)arg); }

int main(int argc, char **argv) {
    ts_policy policy = TS_FCFS;
    int have_policy = 0, practice = 0, answer = 0;
    unsigned quantum = 2, seed = 1, count = 5;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--policy") && i + 1 < argc) {
            if (!parse_policy(argv[++i], &policy)) usage();
            have_policy = 1;
        } else if (!strcmp(argv[i], "--quantum") && i + 1 < argc) quantum = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--practice")) practice = 1;
        else if (!strcmp(argv[i], "--answer")) answer = 1;
        else if (argv[i][0] == '-') usage();
        else file = argv[i];
    }
    if (!have_policy || quantum == 0 || count == 0 || count > MAX) usage();

    tb_proc p[MAX];
    size_t n;
    if (practice) {
        srand(seed);
        n = count;
        for (size_t i = 0; i < n; i++) {
            snprintf(names[i], sizeof names[i], "P%zu", i + 1);
            p[i] = (tb_proc){names[i], i == 0 ? 0u : (unsigned)(rand() % 8), 1u + (unsigned)(rand() % 8), 1 + rand() % 5};
        }
    } else {
        FILE *f = file ? fopen(file, "r") : stdin;
        if (!f) { perror(file); return 2; }
        n = read_procs(f, p);
        if (f != stdin) fclose(f);
        if (!n) { fputs("tsexam: no processes read\n", stderr); return 2; }
    }

    int uses_prio = policy == TS_PRIORITY || policy == TS_PRIORITY_PREEMPTIVE;
    printf("%s\n\n", ts_policy_name(policy));
    if (policy == TS_RR) printf("Quantum: %u\n\n", quantum);
    printf("Process  Arrival  Burst%s\n", uses_prio ? "  Priority" : "");
    for (size_t i = 0; i < n; i++) {
        printf("%-7s  %7u  %5u", p[i].name, p[i].arrival, p[i].burst);
        if (uses_prio) printf("  %8d", p[i].priority);
        printf("\n");
    }
    if (practice && !answer) {
        printf("\nDraw the Gantt chart; find each process's completion, turnaround and waiting time,\n"
               "and the average waiting time. Then run again with --answer.\n");
        return 0;
    }

    /* the textbook calculation */
    ts_slice tb[512];
    tb_result res[MAX];
    size_t ntb = tb_schedule(policy, quantum, p, n, tb, 512, res);

    /* the real scheduler: one thread per process */
    static unsigned bursts[MAX];
    ts_init(&(ts_config){.policy = policy, .quantum = quantum});
    for (size_t i = 0; i < n; i++) {
        bursts[i] = p[i].burst;
        ts_spawn(worker, &bursts[i], &(ts_attr){.name = p[i].name, .arrival = p[i].arrival, .burst = p[i].burst,
                                               .priority = p[i].priority});
    }
    ts_run();
    ts_slice rs[512];
    size_t nrs = ts_timeline(rs, 512);
    ts_stats st[MAX];
    ts_stats_get(st, MAX);

    printf("\n");
    print_gantt("Gantt chart (textbook calculation)", tb, ntb, p);
    print_gantt("Gantt chart (the scheduler, running real threads)", rs, nrs, p);

    printf("\nProcess  Completion  Turnaround  Waiting  Response\n");
    double tw = 0, tt = 0, tr = 0;
    for (size_t i = 0; i < n; i++) {
        printf("%-7s  %10u  %10u  %7u  %8u\n", p[i].name, st[i].finish, st[i].turnaround, st[i].waiting, st[i].response);
        tw += st[i].waiting;
        tt += st[i].turnaround;
        tr += st[i].response;
    }
    printf("Average               %10.2f  %7.2f  %8.2f\n", tt / (double)n, tw / (double)n, tr / (double)n);

    int same = ntb == nrs;
    for (size_t i = 0; same && i < ntb; i++)
        same = tb[i].start == rs[i].start && tb[i].end == rs[i].end && tb[i].thread == rs[i].thread;
    for (size_t i = 0; same && i < n; i++) same = res[i].finish == st[i].finish;
    printf("\nThe scheduler %s the textbook answer (%lu context switch%s, %lu preemption%s).\n",
           same ? "matches" : "DOES NOT match", ts_switches(), ts_switches() == 1 ? "" : "es", ts_preemptions(),
           ts_preemptions() == 1 ? "" : "s");
    ts_shutdown();
    return same ? 0 : 1;
}
