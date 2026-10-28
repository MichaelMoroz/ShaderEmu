/*
 * printf_check [N]: a double's %e, %f, %g and %a (formatted in doubles: vfprintf_double.py) against the same value
 * as a long double (%Le and the rest: musl's own long double path, exact), over N random doubles (all exponents,
 * short decimals, halves that round to even, denormals, zeros, infinities and NaNs) and 16 formats. Prints how many
 * differ and a sum of every string, which the host's C library must give too (glibc is exact as well; %a apart).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t seed = 88172645463325252ull;
static uint64_t next(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

static double value(long i)
{
	uint64_t r = next();
	double d;

	switch (i % 8) {
	case 0: case 1: memcpy(&d, &r, 8); return d;			/* any bits: every exponent, NaNs, infinities */
	case 2: return (double)(int64_t)(r % 2000001) / 1000.0 - 1000.0;	/* a short decimal */
	case 3: return (double)(r % 100000) + 0.5;			/* a half */
	case 4: return (double)(r % 1000) / 8.0;			/* exact in binary */
	case 5: r &= 0x000fffffffffffffull; memcpy(&d, &r, 8); return d;	/* a denormal */
	case 6: return (double)(int64_t)r;				/* a large integer */
	default: return (double)(r >> 11) / (double)(1ull << (r % 60));	/* 53 bits around 1 */
	}
}

static const char *const formats[][2] = {
	{"%.0f", "%.0Lf"}, {"%.3f", "%.3Lf"}, {"%f", "%Lf"}, {"%.17g", "%.17Lg"}, {"%g", "%Lg"}, {"%.3e", "%.3Le"},
	{"%e", "%Le"}, {"%.10e", "%.10Le"}, {"%20.5f", "%20.5Lf"}, {"%-+12.4g", "%-+12.4Lg"}, {"%#.0f", "%#.0Lf"},
	{"%#g", "%#Lg"}, {"%.30f", "%.30Lf"}, {"%.1g", "%.1Lg"}, {"%a", "%La"}, {"%.3a", "%.3La"},
};

static unsigned instructions(void)
{
	unsigned v = 0;
#ifdef __riscv
	__asm__ volatile ("rdinstret %0" : "=r"(v));
#endif
	return v;
}

int main(int argc, char **argv)
{
	unsigned spent[2] = {0, 0};

	if (argc > 2 && (argv[2][0] == 'D' || argv[2][0] == 'L')) {	/* a game's prints: %.2f, %g and %.3e of everyday values */
		uint32_t h = 0;
		char s[64];
		for (long i = 0, n = atol(argv[1]); i < n; i++) {
			double d = i * 0.37 - 500;
			if (argv[2][0] == 'D')
				snprintf(s, sizeof s, "%.2f %g %.3e", d, d, d);
			else
				snprintf(s, sizeof s, "%.2Lf %Lg %.3Le", (long double)d, (long double)d, (long double)d);
			for (char *c = s; *c; c++) h = h * 31 + (unsigned char)*c;
		}
		printf("printf_check: %s %ld, sum %08x\n", argv[2], atol(argv[1]), (unsigned)h);
		return 0;
	}
	long n = argc > 1 ? atol(argv[1]) : 20000, differ = 0, count = 0;
	int only = argc > 2 ? argv[2][0] : 0;	/* d: the doubles alone, l: the long doubles alone (for a count of each) */
	uint32_t sum = 0, sum_no_a = 0;
	char a[2048], b[2048];

	for (long i = 0; i < n; i++) {
		double d = value(i);
		for (unsigned k = 0; k < sizeof formats / sizeof *formats; k++) {
			unsigned t0 = instructions();
			if (only != 'l') snprintf(a, sizeof a, formats[k][0], d);
			unsigned t1 = instructions();
			if (only != 'd') snprintf(b, sizeof b, formats[k][1], (long double)d);
			if (only == 'l') strcpy(a, b);
			if (only == 'd') strcpy(b, a);
			spent[0] += t1 - t0, spent[1] += instructions() - t1;
			count++;
			if (strcmp(a, b)) {
				if (differ++ < 10) printf("printf_check: %s gives %s, %s gives %s\n", formats[k][0], a, formats[k][1], b);
			}
			for (char *s = a; *s; s++) {
				sum = sum * 31 + (unsigned char)*s;
				if (k < 14) sum_no_a = sum_no_a * 31 + (unsigned char)*s;
			}
		}
	}
	printf("printf_check: %ld of %ld differ, sum %08x, without %%a %08x\n", differ, count, (unsigned)sum, (unsigned)sum_no_a);
	return differ != 0;
}
