/* Time (clock_gettime, nanosleep, gettimeofday, time, localtime and strftime, times) and the environment and argv. */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "check.h"

extern char **environ;

static long long ns(struct timespec t) { return t.tv_sec * 1000000000LL + t.tv_nsec; }

int main(int argc, char **argv)
{
	/* argv and the environment the test's init gave */
	CHECK_INT(argc, 4);
	CHECK_STR(argv[0], "/bin/timeenv");
	CHECK_STR(argv[1], "one");
	CHECK_STR(argv[2], "two words");
	CHECK_STR(argv[3], "");
	CHECK(argv[4] == 0);
	CHECK(getenv("GVM_TEST") && !strcmp(getenv("GVM_TEST"), "yes"));
	CHECK(getenv("PATH") != 0);
	CHECK(getenv("NOT_SET") == 0);
	CHECK_INT(setenv("NEW_VAR", "value", 0), 0);
	CHECK_STR(getenv("NEW_VAR"), "value");
	CHECK_INT(setenv("NEW_VAR", "other", 0), 0);
	CHECK_STR(getenv("NEW_VAR"), "value");
	CHECK_INT(setenv("NEW_VAR", "other", 1), 0);
	CHECK_STR(getenv("NEW_VAR"), "other");
	CHECK_INT(unsetenv("NEW_VAR"), 0);
	CHECK(getenv("NEW_VAR") == 0);
	int n = 0;
	for (char **e = environ; *e; e++) n++;
	CHECK(n >= 2);

	/* clocks */
	struct timespec a, b, res;
	CHECK_INT(clock_gettime(CLOCK_MONOTONIC, &a), 0);
	CHECK_INT(clock_getres(CLOCK_MONOTONIC, &res), 0);
	CHECK(res.tv_sec == 0 && res.tv_nsec > 0);
	struct timespec req = { 0, 50 * 1000000 };
	CHECK_INT(nanosleep(&req, 0), 0);
	CHECK_INT(clock_gettime(CLOCK_MONOTONIC, &b), 0);
	long long d = ns(b) - ns(a);
	printf("nanosleep(50 ms) took %lld us\n", d / 1000);
	CHECK(d >= 50000000LL && d < 500000000LL);
	CHECK_INT(usleep(10000), 0);
	struct timespec c;
	clock_gettime(CLOCK_MONOTONIC, &c);
	CHECK(ns(c) - ns(b) >= 10000000LL);
	CHECK_INT(clock_gettime(CLOCK_REALTIME, &a), 0);
	struct timeval tv;
	CHECK_INT(gettimeofday(&tv, 0), 0);
	CHECK(tv.tv_sec >= a.tv_sec && tv.tv_sec <= a.tv_sec + 1);
	time_t t = time(0);
	CHECK(t >= a.tv_sec && t <= a.tv_sec + 1);
	CHECK(sizeof(time_t) == 8);
	CHECK_INT(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &a), 0);
	struct tms tm_;
	CHECK(times(&tm_) != (clock_t)-1);
	CHECK(clock() >= 0);

	/* setting the clock, and calendar time */
	struct timespec set = { 1700000000, 0 };   /* 14 November 2023 22:13:20 UTC */
	CHECK_INT(clock_settime(CLOCK_REALTIME, &set), 0);
	t = time(0);
	CHECK(t >= 1700000000 && t < 1700000005);
	time_t fixed = 1700000000;
	struct tm tm;
	CHECK(gmtime_r(&fixed, &tm) != 0);
	CHECK(tm.tm_year == 123 && tm.tm_mon == 10 && tm.tm_mday == 14 && tm.tm_hour == 22 && tm.tm_min == 13);
	char buf[64];
	strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %a %b", &tm);
	CHECK_STR(buf, "2023-11-14 22:13:20 Tue Nov");
	CHECK_INT(mktime(&tm), 1700000000);   /* no TZ: UTC */
	time_t big = 4102444800LL;           /* 2100: past 2038 */
	gmtime_r(&big, &tm);
	CHECK(tm.tm_year == 200 && tm.tm_mon == 0 && tm.tm_mday == 1);

	struct utsname u;
	CHECK_INT(uname(&u), 0);
	CHECK_STR(u.sysname, "Linux");
	printf("uname: %s %s %s\n", u.sysname, u.release, u.machine);
	char host[64];
	CHECK_INT(gethostname(host, sizeof host), 0);
	CHECK(sysconf(_SC_PAGESIZE) == 4096);
	CHECK(sysconf(_SC_CLK_TCK) == 100);

	return DONE("timeenv");
}
