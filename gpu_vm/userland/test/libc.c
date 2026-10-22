/* stdio, malloc, string, qsort, strtod and printf of doubles */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;
	return (x > y) - (x < y);
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(void)
{
	char buf[256];

	/* printf */
	snprintf(buf, sizeof buf, "%d %u %x %o %c %s %5.2s|%-4d|%04d", -42, 4000000000u, 0xbeef, 8, 'z', "str", "abc", 7, 7);
	CHECK_STR(buf, "-42 4000000000 beef 10 z str    ab|7   |0007");
	snprintf(buf, sizeof buf, "%lld %llu %" PRId64 " %p", -1234567890123LL, 18446744073709551615ULL, (int64_t)1 << 40, (void *)0x1234);
	CHECK_STR(buf, "-1234567890123 18446744073709551615 1099511627776 0x1234");
	snprintf(buf, sizeof buf, "%f %.3f %e %g %g %.10g", 3.14159265358979, -2.5, 123456.789, 0.0001, 1e20, 1.0 / 3);
	CHECK_STR(buf, "3.141593 -2.500 1.234568e+05 0.0001 1e+20 0.3333333333");
	snprintf(buf, sizeof buf, "%.17g %a %f %f", 0.1, 1.0, 1.0 / 0.0, -0.0);
	CHECK_STR(buf, "0.10000000000000001 0x1p+0 inf -0.000000");
	snprintf(buf, sizeof buf, "%.0f %.1f %.2f", 2.5, 0.05, 1e15 + 0.25);
	CHECK_STR(buf, "2 0.1 1000000000000000.25");
	CHECK_INT(snprintf(buf, 4, "abcdefg"), 7);
	CHECK_STR(buf, "abc");

	/* scanf, strtod, strtol */
	int i1, i2; double d1; char w[16];
	CHECK_INT(sscanf("12 -7 2.5e3 word", "%d %d %lf %15s", &i1, &i2, &d1, w), 4);
	CHECK(i1 == 12 && i2 == -7 && d1 == 2500.0);
	CHECK_STR(w, "word");
	char *end;
	CHECK(strtod("3.25xyz", &end) == 3.25 && *end == 'x');
	CHECK(strtod("1e-310", 0) > 0 && strtod("1e-310", 0) < 1e-300);   /* a denormal double: software */
	CHECK(strtod("0x1.8p1", 0) == 3.0);
	CHECK(strtod("-inf", 0) == -1.0 / 0.0);
	CHECK(strtod("0.1", 0) == 0.1);
	CHECK(strtof("0.1", 0) == 0.1f);
	CHECK(strtol("-0x7f", 0, 0) == -127);
	CHECK(strtoul("777", 0, 8) == 511);
	errno = 0;
	CHECK(strtol("99999999999", 0, 10) == LONG_MAX && errno == ERANGE);
	CHECK(strtoll("-9223372036854775807", 0, 10) == -9223372036854775807LL);
	CHECK(atoi("  42abc") == 42);

	/* malloc */
	char *p = malloc(10);
	CHECK(p != 0);
	strcpy(p, "123456789");
	p = realloc(p, 100000);
	CHECK(p && !strcmp(p, "123456789"));
	free(p);
	void *blocks[200];
	for (int i = 0; i < 200; i++) {
		blocks[i] = malloc(16 + i * 37);
		memset(blocks[i], i, 16 + i * 37);
	}
	int good = 1;
	for (int i = 0; i < 200; i++)
		for (int j = 0; j < 16 + i * 37; j++)
			if (((unsigned char *)blocks[i])[j] != (unsigned char)i) good = 0;
	CHECK(good);
	for (int i = 0; i < 200; i += 2) free(blocks[i]);
	for (int i = 1; i < 200; i += 2) free(blocks[i]);
	char *big = malloc(1 << 20);   /* a megabyte: mmap */
	CHECK(big != 0);
	memset(big, 0x5a, 1 << 20);
	CHECK(big[0] == 0x5a && big[(1 << 20) - 1] == 0x5a);
	free(big);
	int *z = calloc(1000, sizeof *z);
	good = 1;
	for (int i = 0; i < 1000; i++) if (z[i]) good = 0;
	CHECK(z && good);
	free(z);

	/* string */
	CHECK(strlen("hello") == 5);
	CHECK(strcmp("abc", "abd") < 0 && strncmp("abcx", "abcy", 3) == 0);
	CHECK(strcasecmp("HeLLo", "hello") == 0);
	CHECK_STR(strchr("a/b/c", '/'), "/b/c");
	CHECK_STR(strrchr("a/b/c", '/'), "/c");
	CHECK_STR(strstr("needle in haystack", "hay"), "haystack");
	strcpy(buf, "one,two,,three");
	char *save, *tok = strtok_r(buf, ",", &save);
	CHECK_STR(tok, "one");
	CHECK_STR(strtok_r(0, ",", &save), "two");
	CHECK_STR(strtok_r(0, ",", &save), "three");
	CHECK(strtok_r(0, ",", &save) == 0);
	char m[64];
	for (int i = 0; i < 64; i++) m[i] = i;
	memmove(m + 1, m, 40);
	CHECK(m[1] == 0 && m[40] == 39 && m[41] == 41);
	memmove(m, m + 1, 40);
	CHECK(m[0] == 0 && m[39] == 39);
	CHECK(memcmp("abc", "abd", 3) < 0);
	char *dup = strdup("duplicate");
	CHECK_STR(dup, "duplicate");
	free(dup);
	CHECK(isdigit('7') && !isdigit('x') && isspace('\t') && toupper('q') == 'Q');
	CHECK_STR(strerror(ENOENT), "No such file or directory");

	/* qsort, bsearch */
	int a[1000];
	unsigned s = 12345;
	for (int i = 0; i < 1000; i++) { s = s * 1103515245 + 12345; a[i] = (int)(s >> 8) % 100000 - 50000; }
	qsort(a, 1000, sizeof *a, cmp_int);
	good = 1;
	for (int i = 1; i < 1000; i++) if (a[i - 1] > a[i]) good = 0;
	CHECK(good);
	int key = a[500];
	CHECK(bsearch(&key, a, 1000, sizeof *a, cmp_int) != 0);
	const char *words[] = { "pear", "apple", "fig", "banana", "cherry" };
	qsort(words, 5, sizeof *words, cmp_str);
	CHECK_STR(words[0], "apple");
	CHECK_STR(words[4], "pear");

	/* 64-bit arithmetic (compiler-rt) */
	volatile long long x = 1234567890123456789LL, y = 987654321;
	CHECK(x / y == 1249999988LL && x % y == 725308641LL);
	volatile unsigned long long ux = 18446744073709551615ULL;
	CHECK(ux / 3 == 6148914691236517205ULL);
	volatile double dd = 1.0;
	for (int i = 0; i < 10; i++) dd = dd * 3.0 + 0.5;
	CHECK(dd == 73811.0);   /* 1.25 * 3^10 - 0.25 */

	return DONE("libc");
}
