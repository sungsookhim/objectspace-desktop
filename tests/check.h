/* A minimal test harness: CHECK records failures and keeps going. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures, check_count;

#define CHECK(cond) do { \
    check_count++; \
    if (!(cond)) { check_failures++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(got, want) do { \
    const char *g_ = (got), *w_ = (want); \
    check_count++; \
    if (!g_ || strcmp(g_, w_) != 0) { \
        check_failures++; \
        fprintf(stderr, "  FAIL %s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #got, g_ ? g_ : "(null)", w_); \
    } \
} while (0)

#define TEST(name) static void name(void)
#define RUN(name) do { int f0_ = check_failures; name(); \
    printf("%s %s\n", check_failures == f0_ ? "ok  " : "FAIL", #name); } while (0)

#define CHECK_SUMMARY() (printf("\n%d checks, %d failed\n", check_count, check_failures), check_failures ? 1 : 0)

#endif
