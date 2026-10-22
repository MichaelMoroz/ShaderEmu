/* The tests' checks: CHECK(cond) prints a failure with its line and counts it; DONE() prints the count and is the
   program's exit code (0 when all held). */
#include <stdio.h>
#include <string.h>

static int check_failed, check_count;
#define CHECK(c) do { check_count++; if (!(c)) { check_failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)
#define CHECK_STR(a, b) do { const char *x_ = (a), *y_ = (b); check_count++; if (strcmp(x_, y_)) { check_failed++; \
	printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, x_, y_); fflush(stdout); } } while (0)
#define CHECK_INT(a, b) do { long long x_ = (a), y_ = (b); check_count++; if (x_ != y_) { check_failed++; \
	printf("FAIL %s:%d: %s = %lld, not %lld\n", __FILE__, __LINE__, #a, x_, y_); fflush(stdout); } } while (0)
#define DONE(name) (printf("%s: %d checks, %d failed\n", name, check_count, check_failed), fflush(stdout), \
	check_failed != 0)
