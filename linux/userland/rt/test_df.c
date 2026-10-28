/* rt/dfast.c against the host's doubles (x86-64, SSE): gcc -O2 -ffp-contract=off test_df.c && ./a.out [millions].
   The slow routines are the host's own sum and product; operands are shaped to reach every branch: exponents a few
   apart and far apart, near cancellations, halfway cases, carries out of the top, and both ends of the range. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __adddf3 t_adddf3
#define __subdf3 t_subdf3
#define __muldf3 t_muldf3
#define __adddf3_slow t_adddf3_slow
#define __muldf3_slow t_muldf3_slow
#define __floatsidf t_floatsidf
#define __floatunsidf t_floatunsidf
double t_adddf3_slow(double a, double b) { return a + b; }
double t_muldf3_slow(double a, double b) { return a * b; }
#include "dfast.c"

static u64 st = 0x9E3779B97F4A7C15ULL;
static u64 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static double bits(u64 b) { double d; memcpy(&d, &b, 8); return d; }
static u64 ubits(double d) { u64 b; memcpy(&b, &d, 8); return b; }
static u64 mant(void)
{
	u64 m = rnd() & 0xfffffffffffffULL;
	switch (rnd() % 5) {
	case 0: return m & ~((1ULL << (rnd() % 52)) - 1);           /* trailing zeros: exact and halfway cases */
	case 1: return m | ((1ULL << (rnd() % 52)) - 1);            /* trailing ones: carries */
	case 2: return rnd() % 2 ? 0xfffffffffffffULL : 0;
	default: return m;
	}
}
static u64 expo(void)
{
	switch (rnd() % 6) {
	case 0: return rnd() % 8;                                  /* the bottom: subnormal results */
	case 1: return 2047 - rnd() % 8;                           /* the top: overflow, infinities */
	case 2: return 1023 + rnd() % 64 - 32;
	default: return rnd() % 2048;
	}
}
static int same(double r, double t) { return (r != r && t != t) || ubits(r) == ubits(t); }

int main(int argc, char **argv)
{
	long n = (argc > 1 ? atol(argv[1]) : 100) * 1000000L, bad = 0;
	for (long i = 0; i < n; i++) {
		u64 ea = expo(), a = (rnd() & 1ULL) << 63 | ea << 52 | mant(), b;
		switch (rnd() % 4) {
		case 0: b = (rnd() & 1ULL) << 63 | ((ea + rnd() % 8 - 4) & 2047) << 52 | mant(); break;   /* a few apart */
		case 1: b = (a ^ (rnd() & 1ULL) << 63) + (rnd() % 16) - 8; break;                         /* near a or -a */
		case 2: b = (rnd() & 1ULL) << 63 | ((ea + rnd() % 128 - 64) & 2047) << 52 | mant(); break;
		default: b = (rnd() & 1ULL) << 63 | expo() << 52 | mant(); break;
		}
		double x = bits(a), y = bits(b), r;
		if (!same(r = t_adddf3(x, y), x + y) && bad++ < 10) printf("add %016llx %016llx: %016llx want %016llx\n", a, b, ubits(r), ubits(x + y));
		if (!same(r = t_subdf3(x, y), x - y) && bad++ < 10) printf("sub %016llx %016llx: %016llx want %016llx\n", a, b, ubits(r), ubits(x - y));
		if (!same(r = t_muldf3(x, y), x * y) && bad++ < 10) printf("mul %016llx %016llx: %016llx want %016llx\n", a, b, ubits(r), ubits(x * y));
		/* a product's exponents: both near 1023 often, else the sum overflows or underflows */
		u64 c = (rnd() & 1ULL) << 63 | ((1023 + rnd() % 1024 - 512) & 2047) << 52 | mant(), d = (rnd() & 1ULL) << 63 | ((1023 + rnd() % 1024 - 512) & 2047) << 52 | mant();
		x = bits(c), y = bits(d);
		if (!same(r = t_muldf3(x, y), x * y) && bad++ < 10) printf("mul %016llx %016llx: %016llx want %016llx\n", c, d, ubits(r), ubits(x * y));
	}
	for (long i = 0; i < n / 4; i++) {
		u32 u = (u32)rnd() >> (rnd() % 32);
		if (ubits(t_floatunsidf(u)) != ubits((double)u) && bad++ < 10) printf("u2d %x\n", u);
		if (ubits(t_floatsidf((int)u)) != ubits((double)(int)u) && bad++ < 10) printf("i2d %x\n", u);
		if (ubits(t_floatsidf(-(int)u)) != ubits((double)-(int)u) && bad++ < 10) printf("i2d -%x\n", u);
	}
	printf("%ld operand pairs, %ld wrong: %s\n", n, bad, bad ? "FAIL" : "PASS");
	return bad != 0;
}
