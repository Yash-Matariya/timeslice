/* A small test harness. TEST registers a function; CHECK records a failure
 * and returns from the test. Checks belong in the test body, which runs in the
 * main context, never inside a timeslice thread. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

typedef void (*test_fn)(void);
void check_register(const char *name, test_fn fn);
void check_failed(const char *file, int line, const char *what);
void check_skip(const char *why);

/* Run fn in a child process, so a crash, a hang or leftover signal state stays
 * there. Returns the child's wait status, or -1 if it had to be killed after
 * `seconds`. */
int run_isolated(test_fn fn, unsigned seconds);

#define TEST(name, fn)                                                                   \
    static void fn(void);                                                                \
    __attribute__((constructor)) static void register_##fn(void) { check_register(name, fn); } \
    static void fn(void)

#define CHECK(expr)                                         \
    do {                                                    \
        if (!(expr)) {                                      \
            check_failed(__FILE__, __LINE__, #expr);        \
            return;                                         \
        }                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        long long check_a = (long long)(a), check_b = (long long)(b);                      \
        if (check_a != check_b) {                                                          \
            char check_msg[256];                                                           \
            snprintf(check_msg, sizeof check_msg, "%s == %s (%lld != %lld)", #a, #b, check_a, check_b); \
            check_failed(__FILE__, __LINE__, check_msg);                                   \
            return;                                                                        \
        }                                                                                  \
    } while (0)

#define SKIP(why)            \
    do {                     \
        check_skip(why);     \
        return;              \
    } while (0)

#endif
