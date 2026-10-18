#include "check.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_TESTS 256

static struct {
    const char *name;
    test_fn fn;
} tests[MAX_TESTS];
static int ntests;
static int failed_now, skipped_now;

void check_register(const char *name, test_fn fn) {
    if (ntests < MAX_TESTS) tests[ntests++] = (typeof(tests[0])){name, fn};
}

void check_failed(const char *file, int line, const char *what) {
    printf("        %s:%d: CHECK(%s) failed\n", file, line, what);
    failed_now = 1;
}

void check_skip(const char *why) {
    printf("        skipped: %s\n", why);
    skipped_now = 1;
}

int run_isolated(test_fn fn, unsigned seconds) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        failed_now = 0;
        fn();
        fflush(stdout);
        _exit(failed_now ? 1 : 0);
    }
    for (unsigned waited = 0; waited < seconds * 100; waited++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) return status;
        struct timespec ten_ms = {0, 10 * 1000 * 1000};
        nanosleep(&ten_ms, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* ./timeslice_tests           everything
 * ./timeslice_tests mutex     tests whose name contains "mutex" */
int main(int argc, char **argv) {
    int passed = 0, failed = 0, skipped = 0;
    for (int i = 0; i < ntests; i++) {
        if (argc > 1 && !strstr(tests[i].name, argv[1])) continue;
        failed_now = skipped_now = 0;
        tests[i].fn();
        if (failed_now) {
            printf("  FAIL  %s\n", tests[i].name);
            failed++;
        } else if (skipped_now) {
            printf("  skip  %s\n", tests[i].name);
            skipped++;
        } else {
            printf("  ok    %s\n", tests[i].name);
            passed++;
        }
    }
    printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
